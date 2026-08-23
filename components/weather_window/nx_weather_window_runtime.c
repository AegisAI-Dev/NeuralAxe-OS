/*
 * NeuralAxe — schedule-window dedup persistence owner (Gate W6.4), routed to
 * the internal-RAM NVS owner task by Gate W6.4.1.
 *
 * See the header for the full contract and for WHY this is asynchronous.
 * Every byte goes through the committed Gate W2 dual-slot store in the
 * existing "nx_wtp" namespace; "nx_tps" is never referenced here.
 *
 * TASK OWNERSHIP, WHICH IS THE WHOLE POINT OF THIS FILE:
 *
 *   EXECUTOR-PRIVATE (touched ONLY by the persistence owner task, and only
 *   from nx_weather_window_runtime_execute()):
 *       s_store, s_rec, s_have_rec, s_staged
 *   These are never read by the observation task, which is why the ~1.5 KB of
 *   store and record state needs no lock at all.
 *
 *   SHARED (both tasks; every touch inside the spinlock):
 *       s_fact, s_pub, s_job_*, s_done_*, the counters and the token latches
 *   Every one is a small scalar or an 8-byte struct, so each critical section
 *   is a bounded copy — the same shape the committed Gate W6.3 worker and the
 *   telemetry safety store already use, under the same primitive.
 *
 *   REQUEST-SIDE-PRIVATE (observation task only):
 *       s_credit_*, s_latch_*, s_expect_gen, s_load_requested
 *
 * `volatile` appears nowhere: mutual exclusion plus the barriers that
 * portENTER/EXIT_CRITICAL already imply IS the synchronisation, and volatile
 * would neither add ordering nor make a multi-field update atomic.
 *
 * NO LOCK IS EVER HELD ACROSS A FLASH OPERATION. The executor snapshots the
 * job under the lock, releases it, performs the transaction holding nothing,
 * then re-takes the lock only to publish the bounded result.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"

#include "nx_weather_window_runtime.h"

/* ------------------------------------------------------------------ */
/* Shared state and its lock                                           */
/* ------------------------------------------------------------------ */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Published durable authority — what the observation task is allowed to see. */
static NxWeatherWindowStoreFact  s_fact;
static TuningScheduleWindowState s_pub;

/*
 * The single pending job slot. One slot IS the "at most one pending" rule.
 *
 * IT CARRIES THE DECISION *INPUTS*, NEVER THE DECISION OUTPUT. Shipping a
 * precomputed TuningScheduleWindowState across a task boundary would be a
 * lost-update waiting to happen: the commit is an ABSOLUTE overwrite of the
 * window fields, so a state computed one tick ago could erase a mask the
 * durable record has gained since — re-opening every window of the day and
 * making that the durable truth. Carrying the plan instead lets the executor
 * re-run the pure decision against ITS OWN current record microseconds before
 * staging, which restores the read/write adjacency the guarantee depends on.
 */
static NxWeatherWindowJobKind    s_job_kind;
static uint32_t                  s_job_gen;
static WeatherSchedulePlan       s_job_plan;
static uint8_t                   s_job_slot_count;
static NxWeatherWindowId         s_job_id;

/* The single completion slot, consumed exactly once. */
static NxWeatherWindowOutcome    s_done_outcome;
static uint32_t                  s_done_gen;
static NxWeatherWindowId         s_done_id;
static uint8_t                   s_done_mask;

static uint8_t  s_last_result;
static uint8_t  s_last_decision;
static uint8_t  s_last_outcome;
static uint32_t s_claims;
static uint32_t s_suppressed;
static uint32_t s_persist_fail;
static uint32_t s_enqueue_busy;
static uint32_t s_executed;

/* ------------------------------------------------------------------ */
/* Executor-private state                                              */
/* ------------------------------------------------------------------ */

/*
 * Long-lived by W2's own contract: TuningStore carries two 512-byte scratch
 * buffers and must never sit on a small task stack. The loaded record is kept
 * because a claim is a LOAD-MODIFY-COMMIT of the whole policy record: the
 * window fields are the only ones this gate changes, and every other field
 * must be written back exactly as it was read.
 *
 * s_staged is a module static rather than a local because a TuningPolicyRecord
 * is ~440 bytes; as a local it made this the deepest frame in the whole gate
 * (measured at 480 bytes with -fstack-usage). The owner task is the only
 * writer, so a static costs nothing in safety and keeps the executor's frame
 * small on a stack shared with the NVS and SPI-flash drivers.
 */
static TuningStore        s_store;
static TuningPolicyRecord s_rec;
static TuningPolicyRecord s_staged;
static bool               s_have_rec;
static const TuningStoreBackendOps *s_backend_ops;
static void                        *s_backend_ctx;

/* ------------------------------------------------------------------ */
/* Request-side-private state                                          */
/* ------------------------------------------------------------------ */

/*
 * THE SUBMIT CREDIT. Set only when a claim was observed DURABLE, cleared the
 * moment it is spent. It is RAM-only and deliberately so: if power is lost
 * while a credit is outstanding, the reboot finds the claim durable on flash,
 * decides ALREADY_CLAIMED and submits nothing. One optional recommendation is
 * lost; a window is never served twice. That is the at-most-once trade.
 */
static bool                 s_credit_valid;
static NxWeatherWindowId    s_credit_id;
static NxWeatherWindowGrant s_credit_grant;

/*
 * THE FAIL-PASSIVE LATCH. A transaction that failed or came back UNCERTAIN
 * must not be retried at 1 Hz against flash. The window is latched for the
 * remainder of THIS BOOT and nothing more: it is RAM-only, so a reboot
 * re-derives eligibility from W2 recovery rather than from a remembered
 * verdict, which is the only authority entitled to resolve an uncertain
 * commit.
 */
static bool              s_latch_valid;
static NxWeatherWindowId s_latch_id;

static uint32_t s_expect_gen;
static bool     s_load_requested;

/*
 * BOUNDED LOAD RETRIES. A store that cannot be opened or read must not be
 * re-attempted once per second forever: that would queue a flash transaction
 * every tick, ahead of real configuration writes, for the entire uptime of a
 * device whose dedup authority is simply broken. After this many attempts the
 * fact latches INHIBITED — which is not serviceable, so the pilot stays silent
 * — and nothing further is posted for the rest of the boot. It is RAM-only, so
 * a reboot gets a fresh budget and W2 recovery remains the authority.
 */
#define NX_WX_LOAD_MAX_ATTEMPTS 3u
static uint32_t s_load_attempts;

static const NxWeatherWindowExecutorOps *s_exec_ops;
static void                             *s_exec_ctx;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void bump(uint32_t *c)
{
    if (*c < NX_WX_WINDOW_COUNTER_MAX) {
        (*c)++;
    }
}

static bool id_equal(const NxWeatherWindowId *a, const NxWeatherWindowId *b)
{
    return a->year == b->year && a->month == b->month &&
           a->day == b->day && a->slot == b->slot;
}

/* The canonical identity of the window a plan refers to. */
static bool plan_id(const WeatherSchedulePlan *plan, NxWeatherWindowId *out)
{
    memset(out, 0, sizeof(*out));
    out->slot = -1;
    if (plan == NULL || plan->slot_index < 0 ||
        !plan->proposed_progress.date.year) {
        return false;
    }
    out->year  = plan->proposed_progress.date.year;
    out->month = plan->proposed_progress.date.month;
    out->day   = plan->proposed_progress.date.day;
    out->slot  = plan->slot_index;
    return true;
}

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */

const char *nx_weather_window_store_fact_str(NxWeatherWindowStoreFact f)
{
    switch (f) {
    case NX_WX_WSTORE_NOT_LOADED:      return "WST_NOT_LOADED";
    case NX_WX_WSTORE_READY_EMPTY:     return "WST_READY_EMPTY";
    case NX_WX_WSTORE_READY_RECOVERED: return "WST_READY_RECOVERED";
    case NX_WX_WSTORE_UNAVAILABLE:     return "WST_UNAVAILABLE";
    case NX_WX_WSTORE_INHIBITED:       return "WST_INHIBITED";
    default:                           return "WST_NOT_LOADED";
    }
}

const char *nx_weather_window_outcome_str(NxWeatherWindowOutcome o)
{
    switch (o) {
    case NX_WX_WDONE_NONE:          return "WDONE_NONE";
    case NX_WX_WDONE_LOADED:        return "WDONE_LOADED";
    case NX_WX_WDONE_LOAD_FAILED:   return "WDONE_LOAD_FAILED";
    case NX_WX_WDONE_CLAIM_DURABLE: return "WDONE_CLAIM_DURABLE";
    case NX_WX_WDONE_CLAIM_FAILED:  return "WDONE_CLAIM_FAILED";
    default:                        return "WDONE_NONE";
    }
}

const char *nx_weather_window_gate_str(NxWeatherWindowGate g)
{
    switch (g) {
    case NX_WX_GATE_REFUSE: return "GATE_REFUSE";
    case NX_WX_GATE_WAIT:   return "GATE_WAIT";
    case NX_WX_GATE_SUBMIT: return "GATE_SUBMIT";
    default:                return "GATE_REFUSE";
    }
}

bool nx_weather_window_store_serviceable(NxWeatherWindowStoreFact f)
{
    return f == NX_WX_WSTORE_READY_EMPTY || f == NX_WX_WSTORE_READY_RECOVERED;
}

/* ------------------------------------------------------------------ */
/* The executor seam                                                   */
/* ------------------------------------------------------------------ */

void nx_weather_window_runtime_bind_executor(const NxWeatherWindowExecutorOps *ops,
                                             void *ctx)
{
    s_exec_ops = ops;
    s_exec_ctx = ctx;
}

bool nx_weather_window_runtime_executor_bound(void)
{
    return s_exec_ops != NULL && s_exec_ops->wake != NULL;
}

/*
 * Claim the single job slot and ring the doorbell.
 *
 * The slot is claimed INSIDE the critical section, so two callers can never
 * both believe they posted. The doorbell is rung OUTSIDE it, because the
 * transport may take a queue lock and nothing that can block belongs in a
 * spinlock.
 *
 * If the doorbell is refused the slot is released again: no flash operation
 * has occurred, so this is an ADMISSION failure and a later tick may retry
 * while the window is still due. That is categorically different from a
 * persistence failure, and only the latter latches.
 */
static bool post_job(NxWeatherWindowJobKind kind,
                     const WeatherSchedulePlan *plan,
                     uint8_t slot_count,
                     const NxWeatherWindowId *id)
{
    uint32_t gen;
    bool     claimed = false;

    if (!nx_weather_window_runtime_executor_bound()) {
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_job_kind == NX_WX_WJOB_NONE && s_done_outcome == NX_WX_WDONE_NONE) {
        s_job_gen++;
        gen        = s_job_gen;
        s_job_kind = kind;
        if (plan != NULL) {
            s_job_plan = *plan;
        } else {
            memset(&s_job_plan, 0, sizeof(s_job_plan));
        }
        s_job_slot_count = slot_count;
        if (id != NULL) {
            s_job_id = *id;
        } else {
            memset(&s_job_id, 0, sizeof(s_job_id));
            s_job_id.slot = -1;
        }
        claimed = true;
    } else {
        gen = s_job_gen;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!claimed) {
        return false;
    }

    s_expect_gen = gen;

    if (s_exec_ops->wake(s_exec_ctx)) {
        return true;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_job_kind == kind && s_job_gen == gen) {
        s_job_kind = NX_WX_WJOB_NONE;   /* nothing was ever handed over */
    }
    bump(&s_enqueue_busy);
    portEXIT_CRITICAL(&s_lock);
    return false;
}

/* ------------------------------------------------------------------ */
/* EXECUTOR SIDE — persistence owner task only                         */
/* ------------------------------------------------------------------ */

/* Classify a load result exactly as Gate W6.4 always did. Owner task only. */
static NxWeatherWindowStoreFact classify_load(TuningStoreResult r)
{
    switch (r) {
    case TUNING_STORE_OK:
        /*
         * A committed record exists. It may be a pre-W6.4 (v1) record, in
         * which case the decoder already gave us window.present == false —
         * "never claimed", which is the correct starting point and must NOT
         * be mistaken for "already served".
         */
        s_have_rec = true;
        return NX_WX_WSTORE_READY_RECOVERED;

    case TUNING_STORE_EMPTY:
        /*
         * VIRGIN STORE. Not corruption. The first due window may proceed, but
         * only by first successfully creating its durable claim — so we start
         * from a fresh default STATE record rather than refusing.
         */
        tuning_record_init_state(&s_rec);
        s_have_rec = true;
        return NX_WX_WSTORE_READY_EMPTY;

    case TUNING_STORE_CLEARED:
        /*
         * An administrative tombstone. There is no policy state and therefore
         * no window claim. Treated exactly like a virgin store for this gate's
         * purposes: a fresh state record is staged, and the first claim will
         * commit it. Nothing is erased and the tombstone is not "repaired".
         */
        tuning_record_init_state(&s_rec);
        s_have_rec = true;
        return NX_WX_WSTORE_READY_EMPTY;

    default:
        /*
         * FAIL CLOSED. Corrupt slot, invalid/missing pointer, unsupported
         * schema, recovery required, I/O error — every one of these means the
         * dedup authority is not trustworthy, so outbound service must not
         * happen. Deliberately: nothing is erased, nothing is recreated, and
         * the evidence is preserved for recovery.
         */
        s_have_rec = false;
        memset(&s_rec, 0, sizeof(s_rec));
        return NX_WX_WSTORE_UNAVAILABLE;
    }
}

void nx_weather_window_runtime_execute(void)
{
    NxWeatherWindowJobKind    kind;
    uint32_t                  gen;
    WeatherSchedulePlan       plan;
    uint8_t                   slot_count;
    NxWeatherWindowId         id;
    TuningStoreResult         r;
    NxWeatherWindowOutcome    outcome = NX_WX_WDONE_NONE;
    NxWeatherWindowStoreFact  fact;
    TuningScheduleWindowState pub;
    uint8_t                   mask = 0u;
    bool                      durable = false;

    /* --- 1. Take the job under the lock, then let go of it entirely. --- */
    portENTER_CRITICAL(&s_lock);
    kind       = s_job_kind;
    gen        = s_job_gen;
    plan       = s_job_plan;
    slot_count = s_job_slot_count;
    id         = s_job_id;
    fact       = s_fact;
    portEXIT_CRITICAL(&s_lock);

    if (kind == NX_WX_WJOB_NONE) {
        return;                       /* spurious doorbell: nothing to do */
    }

    /* --- 2. THE FLASH TRANSACTION. No lock of ours is held here. --- */
    if (kind == NX_WX_WJOB_LOAD) {
        if (s_backend_ops == NULL) {
            fact    = NX_WX_WSTORE_UNAVAILABLE;
            outcome = NX_WX_WDONE_LOAD_FAILED;
        } else {
            r = tuning_store_init(&s_store, s_backend_ops, s_backend_ctx);
            if (r != TUNING_STORE_OK) {
                s_have_rec = false;
                fact    = NX_WX_WSTORE_UNAVAILABLE;
                outcome = NX_WX_WDONE_LOAD_FAILED;
            } else {
                memset(&s_rec, 0, sizeof(s_rec));
                r    = tuning_store_load(&s_store, &s_rec, NULL);
                fact = classify_load(r);
                outcome = nx_weather_window_store_serviceable(fact)
                              ? NX_WX_WDONE_LOADED
                              : NX_WX_WDONE_LOAD_FAILED;
            }
            s_last_result = (uint8_t)r;
        }
    } else {
        /*
         * LOAD-MODIFY-COMMIT of the whole record. Only the window fields move;
         * every other field is written back exactly as it was read, so a
         * schedule claim can never clobber settings, climate stance, a profile
         * transaction, the override, the cooldown or the trusted-epoch floor.
         */
        if (!s_have_rec) {
            fact    = NX_WX_WSTORE_UNAVAILABLE;
            outcome = NX_WX_WDONE_CLAIM_FAILED;
        } else {
            TuningScheduleWindowState next;
            NxWeatherWindowDecision   d;

            /*
             * RE-DECIDE HERE, against THIS task's own record, microseconds
             * before staging it. The request side's decision was a pre-filter
             * that authorized nothing; this one is the authority.
             *
             * That is what makes the merge safe: the mask can only ever be
             * derived from the record that is about to be overwritten, so a
             * job that sat in the slot while the record moved on cannot lose a
             * bit, and a window claimed in the meantime is refused here rather
             * than silently re-claimed.
             */
            d = nx_weather_window_decide(&plan, &s_rec.window, slot_count,
                                         &next);
            if (d != NX_WX_WINDOW_CLAIM_REQUIRED) {
                s_last_decision = (uint8_t)d;
                fact    = nx_weather_window_store_serviceable(fact)
                              ? fact
                              : NX_WX_WSTORE_UNAVAILABLE;
                outcome = NX_WX_WDONE_CLAIM_FAILED;
                goto publish;
            }

            s_staged        = s_rec;
            s_staged.window = next;

            r = tuning_store_commit_record(&s_store, &s_staged);
            s_last_result = (uint8_t)r;

            if (r == TUNING_STORE_OK) {
                /* Durable. Adopt exactly what was committed — including the
                 * generation the store assigned — so RAM and flash cannot
                 * diverge. */
                s_rec   = s_staged;
                fact    = NX_WX_WSTORE_READY_RECOVERED;
                outcome = NX_WX_WDONE_CLAIM_DURABLE;
                durable = true;
                mask    = s_rec.window.served_mask;
            } else {
                /*
                 * The commit did NOT verify — including
                 * TUNING_STORE_COMMIT_UNCERTAIN, where the pointer write
                 * failed and the outcome genuinely is not knowable from here.
                 * The old committed record remains authoritative on flash by
                 * the W2 algorithm, and the in-RAM authority is deliberately
                 * left untouched so RAM cannot claim more than flash. The
                 * caller must not submit, and only W2 recovery on the next
                 * boot may resolve an uncertain commit.
                 */
                fact    = nx_weather_window_store_serviceable(s_fact)
                              ? s_fact
                              : NX_WX_WSTORE_UNAVAILABLE;
                outcome = NX_WX_WDONE_CLAIM_FAILED;
            }
        }
    }

publish:
    pub = (s_have_rec && nx_weather_window_store_serviceable(fact))
              ? s_rec.window
              : (TuningScheduleWindowState){ 0 };

    /* --- 3. Publish the bounded result. --- */
    portENTER_CRITICAL(&s_lock);
    s_fact         = fact;
    s_pub          = pub;
    s_done_outcome = outcome;
    s_done_gen     = gen;
    s_done_id      = id;
    s_done_mask    = mask;
    s_last_outcome = (uint8_t)outcome;
    s_job_kind     = NX_WX_WJOB_NONE;
    if (durable) {
        bump(&s_claims);
    } else if (kind == NX_WX_WJOB_CLAIM) {
        bump(&s_persist_fail);
    }
    bump(&s_executed);
    portEXIT_CRITICAL(&s_lock);
}

/* ------------------------------------------------------------------ */
/* REQUEST SIDE — observation task only                                */
/* ------------------------------------------------------------------ */

static void consume_completion(void);

bool nx_weather_window_runtime_begin(const TuningStoreBackendOps *ops, void *ctx)
{
    NxWeatherWindowStoreFact fact;

    /*
     * Take delivery of anything the owner finished since the last call. The
     * load requester must observe its OWN completion: without this a failed
     * load would never be re-requested and, more importantly, the bounded
     * retry budget would never advance to the INHIBITED latch.
     */
    consume_completion();

    portENTER_CRITICAL(&s_lock);
    fact = s_fact;
    portEXIT_CRITICAL(&s_lock);

    if (nx_weather_window_store_serviceable(fact)) {
        return true;                   /* idempotent: already loaded */
    }
    if (ops == NULL) {
        portENTER_CRITICAL(&s_lock);
        s_fact = NX_WX_WSTORE_UNAVAILABLE;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }

    s_backend_ops = ops;
    s_backend_ctx = ctx;

    /*
     * Request the load ONCE. A refused doorbell is not fatal: s_load_requested
     * stays false so a later tick tries again, and until the owner has
     * actually classified the store the fact remains NOT_LOADED — which is
     * never serviceable, so nothing can be submitted meanwhile.
     */
    if (!s_load_requested && fact != NX_WX_WSTORE_INHIBITED) {
        if (post_job(NX_WX_WJOB_LOAD, NULL, 0u, NULL)) {
            s_load_requested = true;
            s_load_attempts++;
        }
    }
    return false;
}

void nx_weather_window_runtime_end(void)
{
    NxWeatherWindowJobKind pending;

    /*
     * REFUSE WHILE BUSY. s_store is executor-private; tearing it down from
     * another task while the owner is inside a transaction would free the very
     * instance it is writing through. There is no production caller, so a
     * refusal is the whole safety requirement here.
     */
    portENTER_CRITICAL(&s_lock);
    pending = s_job_kind;
    portEXIT_CRITICAL(&s_lock);
    if (pending != NX_WX_WJOB_NONE) {
        return;
    }

    (void)tuning_store_deinit(&s_store);
    s_have_rec    = false;
    s_backend_ops = NULL;
    s_backend_ctx = NULL;
    memset(&s_rec, 0, sizeof(s_rec));
    portENTER_CRITICAL(&s_lock);
    s_fact = NX_WX_WSTORE_NOT_LOADED;
    memset(&s_pub, 0, sizeof(s_pub));
    portEXIT_CRITICAL(&s_lock);
}

NxWeatherWindowStoreFact nx_weather_window_runtime_fact(void)
{
    NxWeatherWindowStoreFact f;

    portENTER_CRITICAL(&s_lock);
    f = s_fact;
    portEXIT_CRITICAL(&s_lock);
    return f;
}

bool nx_weather_window_runtime_ready(void)
{
    return nx_weather_window_store_serviceable(nx_weather_window_runtime_fact());
}

bool nx_weather_window_runtime_state(TuningScheduleWindowState *out)
{
    bool ok;

    if (out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    ok = nx_weather_window_store_serviceable(s_fact);
    *out = ok ? s_pub : (TuningScheduleWindowState){ 0 };
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

void nx_weather_window_runtime_note_suppressed(NxWeatherWindowDecision d)
{
    portENTER_CRITICAL(&s_lock);
    s_last_decision = (uint8_t)d;
    if (d == NX_WX_WINDOW_ALREADY_CLAIMED || d == NX_WX_WINDOW_DAY_REGRESSION) {
        bump(&s_suppressed);
    }
    portEXIT_CRITICAL(&s_lock);
}

/*
 * Consume the single completion slot, exactly once.
 *
 * A completion whose generation is not the one we are waiting for is
 * DISCARDED: it belongs to a request this boot no longer owns, and acting on
 * it could grant a submit credit for a window nobody asked about.
 */
static void consume_completion(void)
{
    NxWeatherWindowOutcome o;
    NxWeatherWindowId      id;
    uint32_t               gen;
    uint8_t                mask;

    portENTER_CRITICAL(&s_lock);
    o    = s_done_outcome;
    id   = s_done_id;
    gen  = s_done_gen;
    mask = s_done_mask;
    s_done_outcome = NX_WX_WDONE_NONE;      /* consumed exactly once */
    portEXIT_CRITICAL(&s_lock);

    if (o == NX_WX_WDONE_NONE || gen != s_expect_gen) {
        return;                             /* stale generation: discard */
    }

    switch (o) {
    case NX_WX_WDONE_CLAIM_DURABLE:
        /*
         * The ONLY path that may ever authorize an outbound submission, and
         * the grant records EXACTLY what reached flash. The caller must build
         * its request from these fields rather than from a fresh clock: a
         * midnight rollover between the claim and the submission would
         * otherwise send a request for a window that was never claimed.
         */
        s_credit_valid            = true;
        s_credit_id               = id;
        s_credit_grant.generation = gen;
        s_credit_grant.date.year  = id.year;
        s_credit_grant.date.month = id.month;
        s_credit_grant.date.day   = id.day;
        s_credit_grant.slot_index = id.slot;
        s_credit_grant.served_mask = mask;
        break;

    case NX_WX_WDONE_CLAIM_FAILED:
        /* Fail passive for this window for the rest of this boot. */
        s_latch_valid = true;
        s_latch_id    = id;
        break;

    case NX_WX_WDONE_LOAD_FAILED:
        /*
         * Allow a bounded number of further attempts. The fact already says
         * UNAVAILABLE meanwhile, so nothing can be submitted; once the budget
         * is spent the fact latches INHIBITED and nothing more is posted for
         * this boot, which is what stops a broken store from queueing a flash
         * transaction every second ahead of real configuration writes.
         */
        if (s_load_attempts >= NX_WX_LOAD_MAX_ATTEMPTS) {
            portENTER_CRITICAL(&s_lock);
            s_fact = NX_WX_WSTORE_INHIBITED;
            portEXIT_CRITICAL(&s_lock);
        } else {
            s_load_requested = false;
        }
        break;

    default:
        break;
    }
}

NxWeatherWindowGate nx_weather_window_authorize(const WeatherSchedulePlan *plan,
                                                uint8_t slot_count,
                                                NxWeatherWindowGrant *out_grant)
{
    TuningScheduleWindowState persisted;
    TuningScheduleWindowState next;
    NxWeatherWindowDecision   d;
    NxWeatherWindowId         id;
    NxWeatherWindowJobKind    pending;

    /* Fail closed FIRST: an ignored verdict must never leave a usable grant. */
    if (out_grant != NULL) {
        memset(out_grant, 0, sizeof(*out_grant));
        out_grant->slot_index = -1;
    }

    /*
     * THE ORDERING THAT MAKES THE GUARANTEE. The window becomes durably
     * claimed BEFORE the caller may submit anything for it. Claiming
     * afterwards would permit a duplicate: power lost between the request and
     * the write leaves nothing on flash, and the next boot would find the same
     * window still open and serve it again.
     *
     * Gate W6.4.1 keeps that ordering and makes it span ticks: the claim is
     * REQUESTED here, executed on the internal-RAM persistence owner, and only
     * a later tick — having observed a verified durable commit — returns
     * SUBMIT. Nothing on this path blocks, and nothing on this path touches
     * flash.
     */

    /* 1 — take delivery of anything the owner finished since the last tick. */
    consume_completion();

    if (!plan_id(plan, &id)) {
        return NX_WX_GATE_REFUSE;
    }

    /* 2 — persistence must be trustworthy before anything else is considered. */
    if (!nx_weather_window_runtime_ready()) {
        return NX_WX_GATE_REFUSE;
    }

    /*
     * 3 — a durable claim we made this boot and have not yet spent. Checked
     * BEFORE the decision, because by now the persisted mask already contains
     * this slot and the decision would (correctly) say ALREADY_CLAIMED.
     * The credit is cleared here whatever the caller then does with it, so it
     * can authorize at most one submission.
     */
    if (s_credit_valid && id_equal(&s_credit_id, &id)) {
        s_credit_valid = false;
        if (out_grant == NULL) {
            /* No grant to hand over means the caller cannot name the window it
             * would submit for. Refuse rather than let it re-derive one. */
            return NX_WX_GATE_REFUSE;
        }
        *out_grant = s_credit_grant;
        return NX_WX_GATE_SUBMIT;
    }

    /* 4 — a window whose transaction already failed this boot is not retried. */
    if (s_latch_valid && id_equal(&s_latch_id, &id)) {
        return NX_WX_GATE_REFUSE;
    }

    /* 5 — the pure decision, against the DURABLE authority. */
    (void)nx_weather_window_runtime_state(&persisted);
    d = nx_weather_window_decide(plan, &persisted, slot_count, &next);
    (void)next;   /* PRE-FILTER ONLY: the executor re-decides authoritatively */
    if (!nx_weather_window_requires_claim(d)) {
        nx_weather_window_runtime_note_suppressed(d);
        return NX_WX_GATE_REFUSE;
    }

    /* 6 — a transaction already in flight: wait, never submit, never re-post. */
    portENTER_CRITICAL(&s_lock);
    pending = s_job_kind;
    portEXIT_CRITICAL(&s_lock);
    if (pending != NX_WX_WJOB_NONE) {
        return NX_WX_GATE_WAIT;
    }

    /*
     * 7 — request the one bounded flash transaction of this gate: at most one
     * per newly claimed slot, never one per tick, and never on this task.
     * A refused doorbell means no flash happened at all, so the window stays
     * due and a later tick may try again.
     */
    if (!post_job(NX_WX_WJOB_CLAIM, plan, slot_count, &id)) {
        return NX_WX_GATE_REFUSE;
    }
    return NX_WX_GATE_WAIT;
}

/* ------------------------------------------------------------------ */
/* Observation                                                         */
/* ------------------------------------------------------------------ */

void nx_weather_window_runtime_observe(NxWeatherWindowDiag *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    portENTER_CRITICAL(&s_lock);
    out->fact               = (uint8_t)s_fact;
    out->ready              = nx_weather_window_store_serviceable(s_fact);
    out->recovered          = (s_fact == NX_WX_WSTORE_READY_RECOVERED);
    out->last_result        = s_last_result;
    out->last_decision      = s_last_decision;
    out->claim_count        = s_claims;
    out->suppressed_count   = s_suppressed;
    out->persist_fail_count = s_persist_fail;
    out->job_pending        = (uint8_t)s_job_kind;
    out->last_outcome       = s_last_outcome;
    out->enqueue_busy_count = s_enqueue_busy;
    out->executed_count     = s_executed;
    if (out->ready) {
        out->day_present   = s_pub.present;
        out->service_year  = s_pub.year;
        out->service_month = s_pub.month;
        out->service_day   = s_pub.day;
        out->served_mask   = s_pub.served_mask;
    }
    portEXIT_CRITICAL(&s_lock);

    out->submit_credit = s_credit_valid;
}

void nx_weather_window_runtime_reset_for_test(void)
{
    (void)tuning_store_deinit(&s_store);
    memset(&s_store, 0, sizeof(s_store));
    memset(&s_rec, 0, sizeof(s_rec));
    memset(&s_staged, 0, sizeof(s_staged));
    s_have_rec    = false;
    s_backend_ops = NULL;
    s_backend_ctx = NULL;

    portENTER_CRITICAL(&s_lock);
    s_fact = NX_WX_WSTORE_NOT_LOADED;
    memset(&s_pub, 0, sizeof(s_pub));
    s_job_kind = NX_WX_WJOB_NONE;
    s_job_gen  = 0u;
    memset(&s_job_plan, 0, sizeof(s_job_plan));
    s_job_slot_count = 0u;
    memset(&s_job_id, 0, sizeof(s_job_id));
    s_done_outcome = NX_WX_WDONE_NONE;
    s_done_gen     = 0u;
    memset(&s_done_id, 0, sizeof(s_done_id));
    s_done_mask = 0u;
    s_last_result   = 0u;
    s_last_decision = 0u;
    s_last_outcome  = 0u;
    s_claims        = 0u;
    s_suppressed    = 0u;
    s_persist_fail  = 0u;
    s_enqueue_busy  = 0u;
    s_executed      = 0u;
    portEXIT_CRITICAL(&s_lock);

    s_credit_valid   = false;
    s_latch_valid    = false;
    memset(&s_credit_id, 0, sizeof(s_credit_id));
    memset(&s_latch_id, 0, sizeof(s_latch_id));
    memset(&s_credit_grant, 0, sizeof(s_credit_grant));
    s_expect_gen     = 0u;
    s_load_requested = false;
    s_load_attempts  = 0u;
    /* The executor binding deliberately SURVIVES a test reset, exactly as it
     * survives a reboot in production: it is wiring, not state. */
}
