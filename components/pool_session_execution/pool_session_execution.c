/*
 * NeuralAxe timed pool sessions — controlled-execution engine
 * (Phase 2M.1B, Gate B7). See pool_session_execution.h for the contract.
 *
 * The engine is driven exclusively by the single B6 runtime owner task; it
 * creates no task, no queue and no callback of its own. Every external
 * effect flows through the injected bounded adapters, every durable state
 * change flows through the bound runtime's B3 store + B5 coordinator under
 * the CURRENT lease token, and every log line carries machine tokens only —
 * never a hostname, account, worker, wallet, password, session identifier,
 * record byte or unrestricted protocol error.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "pool_session_execution.h"

static const char *TAG = "nx_pool_exec";

/* ------------------------------------------------------------------ */
/* ASIC job-delivery gate (module-wide; spinlock-guarded)              */
/* ------------------------------------------------------------------ */

/*
 * Single writer: the executor engine on the B6 owner task. Concurrent
 * reader: the job pipeline. The zero/reset posture is DEFAULT_OPEN — safe
 * because target work cannot exist before the executor starts a protocol
 * connection, and the executor sets INHIBITED before its first protocol
 * action (see pool_exec_state_gate() in the pure core).
 */
static portMUX_TYPE s_gate_lock = portMUX_INITIALIZER_UNLOCKED;

/*
 * LOCK-DEPTH INSTRUMENTATION (Gate B7 blocking-IO contract). Every take
 * and release of the gate spinlock goes through these wrappers, so the
 * current nesting depth is machine-checkable: test fakes standing in for
 * every blocking boundary (socket writes, protocol start/stop, NVS)
 * assert pool_session_execution_lock_depth() == 0 on entry. The engine
 * NEVER holds this lock across an adapter call, a serial write, a socket
 * operation or an NVS operation — the lock guards bounded CPU-only
 * sections, and callers that need data across a blocking operation take
 * a caller-owned COPY under the lock, release it, then block.
 */
static volatile uint32_t s_gate_lock_depth = 0u;

static inline void gate_lock(void)
{
    portENTER_CRITICAL(&s_gate_lock);
    s_gate_lock_depth++;
}

static inline void gate_unlock(void)
{
    s_gate_lock_depth--;
    portEXIT_CRITICAL(&s_gate_lock);
}

uint32_t pool_session_execution_lock_depth(void)
{
    return s_gate_lock_depth;
}

static PoolExecGatePosture s_gate_posture = EXEC_GATE_DEFAULT_OPEN;
static bool     s_gate_grant_valid = false;
/* Per-WORK-GENERATION counters: zeroed by begin_work_generation() so no
 * fact from an earlier generation can ever be observed by a later window. */
static uint32_t s_work_generation     = 0u;
static uint64_t s_jobs_forwarded      = 0u;
static uint64_t s_asic_job_results    = 0u;
static uint64_t s_asic_register_reads = 0u;

/* Delivered-work registry and identity slot pool (defined below; declared
 * here so the shared reset can clear every one of them under one lock).
 * The record retains the item's EXACT canonical header facts so header
 * uniqueness across generations is decided by exact comparison — never by
 * a short hash. Bounded: POOL_EXEC_JOB_SLOTS * sizeof(NxDeliveredWork)
 * static bytes, compiled only under CONFIG_NX_TIMED_SESSIONS_EXECUTION. */
typedef struct {
    bool     occupied;
    bool     consumed;
    bool     facts_valid;         /* facts were supplied at delivery      */
    bool     header_unique;       /* proven distinct from every relevant
                                   * prior-generation header (exact cmp)  */
    uint8_t  disc_kind;           /* PoolExecHeaderDiscriminator at embed */
    uint32_t disc_tag;            /* generation tag issued for this item  */
    uint32_t prior_match_slot;    /* matching prior slot; UINT32_MAX none */
    uint32_t protocol_generation;
    uint32_t config_generation;
    uint32_t work_generation;
    PoolExecWorkFacts facts;
} NxDeliveredWork;

/*
 * Extranonce2 generation-domain state. `s_domain_active` is written only
 * by the executor (single writer) around the controlled protocol
 * lifetime. The issued-tag handshake records the discriminator decision
 * of the MOST RECENT tag query so the delivery that follows on the same
 * job-pipeline task can be stamped with it; the pipeline is one task, so
 * query -> build -> deliver is strictly sequential. A delivery with no
 * preceding query (SV2 standard, normal mining, self-test) consumes
 * nothing and is stamped NONE — fail closed.
 */
static bool     s_domain_active        = false;
static bool     s_disc_issued_valid    = false;
static uint8_t  s_disc_issued_kind     = 0u; /* POOL_EXEC_DISCRIMINATOR_NONE */
static uint32_t s_disc_issued_tag      = 0u;
static uint32_t s_disc_issued_generation = 0u;

/*
 * PER-BOOT NON-WRAPPING DISCRIMINATOR ALLOCATOR. `s_domain_tag_index`
 * only ever increases within one runtime boot; each controlled protocol
 * start consumes exactly one index and the derived tag (0x81..0xFF) is
 * NEVER issued twice. When the budget is spent the allocator fails
 * closed: no tag exists, the embed query declines, and the EXECUTOR
 * refuses to start any further controlled verification at all
 * (EXEC_REASON_GENERATION_EXHAUSTED). Only the boot/test gate_reset —
 * never deinit, never a rebind — returns the budget.
 */
static uint32_t s_domain_tag_index = 0u; /* allocations consumed this boot */
static uint32_t s_domain_tag       = 0u; /* current generation's tag; 0 = none */
static bool     s_domain_exhausted = false;
static uint64_t s_rolling_declined = 0u; /* counters refused (> 24-bit space) */

/*
 * `refcount` counts LIVE READER BORROWS ONLY. Publication is tracked
 * separately by `published`, because a quiescent reclaim legitimately ends
 * the epoch's publication but must NEVER pull storage out from under a
 * reader that still holds a reference.
 */
typedef struct {
    PoolExecIdentityStrings strings;
    bool     used;      /* handed out at least once this epoch */
    bool     published; /* has been the live identity          */
    uint32_t refcount;  /* live reader borrows ONLY            */
} NxIdentitySlot;

static NxDeliveredWork s_delivered[POOL_EXEC_JOB_SLOTS];
static uint32_t        s_config_generation   = 0u;
static uint32_t        s_protocol_generation = 0u;
static uint64_t        s_rejected[POOL_EXEC_RESULT_VERDICT__COUNT];
static NxIdentitySlot  s_identity[POOL_EXEC_IDENTITY_SLOTS];
static uint32_t        s_identity_published = UINT32_MAX;

static void gate_write(PoolExecGatePosture posture, bool grant_valid)
{
    gate_lock();
    s_gate_posture     = posture;
    s_gate_grant_valid = grant_valid;
    gate_unlock();
}

/* ------------------------------------------------------------------ */
/* Delivered-work registry (per-item generation binding)               */
/* ------------------------------------------------------------------ */

/*
 * One record per ASIC job slot (declared above): `occupied` marks a real
 * delivery, the three generations are the EXACT epoch that produced the
 * item, and `consumed` makes acceptance one-time. Static and bounded.
 */
const char *pool_session_execution_result_verdict_str(PoolExecResultVerdict v)
{
    switch (v) {
    case EXEC_RESULT_ACCEPTED:         return "ASIC_RESULT_ACCEPTED";
    case EXEC_RESULT_UNKNOWN_JOB:      return "ASIC_RESULT_UNKNOWN_JOB";
    case EXEC_RESULT_STALE_GENERATION: return "ASIC_RESULT_STALE_GENERATION";
    case EXEC_RESULT_ALREADY_CONSUMED: return "ASIC_RESULT_ALREADY_CONSUMED";
    case EXEC_RESULT_WORK_MISMATCH:    return "ASIC_RESULT_WORK_MISMATCH";
    case EXEC_RESULT_INVALID:          return "ASIC_RESULT_INVALID";
    case EXEC_RESULT_NO_DISCRIMINATOR: return "ASIC_RESULT_NO_DISCRIMINATOR";
    case EXEC_RESULT_HEADER_ALIASED:   return "ASIC_RESULT_HEADER_ALIASED";
    default:                           return "ASIC_RESULT_UNKNOWN";
    }
}

double pool_session_execution_work_proof_threshold(uint32_t asic_difficulty)
{
    double d = (double)asic_difficulty;

    /* Clamp into the compiled band. A device config reporting 0, 1 or an
     * absurd value can neither weaken nor stall the local proof, and the
     * POOL difficulty is never consulted anywhere on this path. */
    if (!(d >= POOL_EXEC_WORK_PROOF_MIN_DIFF)) { /* also catches NaN */
        d = POOL_EXEC_WORK_PROOF_MIN_DIFF;
    }
    if (d > POOL_EXEC_WORK_PROOF_MAX_DIFF) {
        d = POOL_EXEC_WORK_PROOF_MAX_DIFF;
    }
    return d;
}

void pool_session_execution_note_work_delivered(uint8_t job_id,
                                                const PoolExecWorkFacts *facts)
{
    NxDeliveredWork *rec;
    bool     unique     = false;
    uint32_t match_slot = UINT32_MAX;
    uint8_t  kind       = (uint8_t)POOL_EXEC_DISCRIMINATOR_NONE;
    uint32_t tag        = 0u;
    uint32_t i;

    if ((uint32_t)job_id >= POOL_EXEC_JOB_SLOTS) {
        return;
    }
    gate_lock();
    rec = &s_delivered[job_id];

    /*
     * HEADER UNIQUENESS — established HERE, before the job frame reaches
     * the chip. Exact field-by-field comparison against every retained
     * record from an OLDER (protocol, config, work) epoch, including the
     * record this delivery is about to overwrite. A record of the SAME
     * epoch is not a temporal alias and is never compared. Absent facts
     * fail closed: the record can never be unique, hence never evidence.
     */
    if (facts != NULL) {
        unique = true;
        for (i = 0u; i < POOL_EXEC_JOB_SLOTS; i++) {
            const NxDeliveredWork *old = &s_delivered[i];
            if (!old->occupied || !old->facts_valid) {
                continue;
            }
            if (old->protocol_generation == s_protocol_generation &&
                old->config_generation == s_config_generation &&
                old->work_generation == s_work_generation) {
                continue; /* same epoch: not a temporal alias */
            }
            if (pool_exec_work_facts_equal(&old->facts, facts)) {
                unique     = false;
                match_slot = i;
                break;
            }
        }
    }

    /* Consume the issued-tag handshake (same-task sequencing): the tag
     * query that produced THIS work item ran immediately before it. A
     * stale issue from an older generation is never consumed. */
    if (s_disc_issued_valid && s_disc_issued_generation == s_work_generation) {
        kind = s_disc_issued_kind;
        tag  = s_disc_issued_tag;
    }
    s_disc_issued_valid = false;

    /* Stamp the item with the epoch that produced it. Overwriting a
     * previous record is correct: the chip reused the slot, so the older
     * item can no longer produce an attributable result — the comparison
     * above already ran against it. */
    rec->occupied            = true;
    rec->consumed            = false;
    rec->facts_valid         = (facts != NULL);
    rec->header_unique       = (facts != NULL) && unique;
    rec->disc_kind           = kind;
    rec->disc_tag            = tag;
    rec->prior_match_slot    = match_slot;
    rec->protocol_generation = s_protocol_generation;
    rec->config_generation   = s_config_generation;
    rec->work_generation     = s_work_generation;
    if (facts != NULL) {
        rec->facts = *facts;
    } else {
        memset(&rec->facts, 0, sizeof(rec->facts));
    }
    if (s_jobs_forwarded < UINT64_MAX) {
        s_jobs_forwarded++;
    }
    gate_unlock();
}

bool pool_session_execution_allocate_generation_tag(void)
{
    bool ok = false;

    gate_lock();
    if (s_domain_tag_index >= POOL_EXEC_GENERATION_TAG_LIMIT) {
        /* Budget spent for this boot: no tag, permanently (until reboot).
         * Never wrap, never reuse. */
        s_domain_exhausted = true;
        s_domain_tag       = 0u;
    } else {
        s_domain_tag_index++;
        s_domain_tag = pool_exec_generation_tag(s_domain_tag_index);
        ok = (s_domain_tag != 0u);
    }
    gate_unlock();
    return ok;
}

uint32_t pool_session_execution_generation_tag_current(void)
{
    uint32_t tag;

    gate_lock();
    tag = s_domain_tag;
    gate_unlock();
    return tag;
}

uint32_t pool_session_execution_generation_tags_remaining(void)
{
    uint32_t left;

    gate_lock();
    left = POOL_EXEC_GENERATION_TAG_LIMIT - s_domain_tag_index;
    gate_unlock();
    return left;
}

bool pool_session_execution_generation_exhausted(void)
{
    bool e;

    gate_lock();
    e = s_domain_exhausted;
    gate_unlock();
    return e;
}

uint64_t pool_session_execution_rolling_declined(void)
{
    uint64_t n;

    gate_lock();
    n = s_rolling_declined;
    gate_unlock();
    return n;
}

bool pool_session_execution_extranonce2_tag(bool protocol_v2,
                                            bool sv2_extended_channel,
                                            uint32_t extranonce2_len,
                                            uint64_t counter,
                                            uint64_t *tagged)
{
    PoolExecHeaderDiscriminator kind;
    bool applied = false;

    if (tagged == NULL) {
        return false;
    }
    *tagged = counter;
    kind = pool_exec_discriminator_capability(protocol_v2, sv2_extended_channel,
                                              extranonce2_len);
    gate_lock();
    if (counter > POOL_EXEC_GENERATION_COUNTER_MASK) {
        /* ROLLING-DOMAIN GUARD: this counter no longer fits the low 24
         * bits, so embedding it would silently overflow INTO the tag
         * byte — a previously used domain. Refuse: the item goes out
         * untagged (stock bytes) and can never be verification
         * evidence. Fail closed, never silent reuse. */
        if (s_rolling_declined < UINT64_MAX) {
            s_rolling_declined++;
        }
        s_disc_issued_valid = false;
    } else if (s_domain_active && s_work_generation != 0u &&
               s_domain_tag != 0u &&
               kind == POOL_EXEC_DISCRIMINATOR_EXTRANONCE2) {
        *tagged = pool_exec_apply_generation_tag(counter, s_domain_tag);
        s_disc_issued_valid      = true;
        s_disc_issued_kind       = (uint8_t)kind;
        s_disc_issued_tag        = s_domain_tag;
        s_disc_issued_generation = s_work_generation;
        applied = true;
    } else {
        /* No domain for this delivery (inactive, incapable, or the
         * per-boot tag budget is exhausted): make sure a stale issue can
         * never leak onto it. */
        s_disc_issued_valid = false;
    }
    gate_unlock();
    return applied;
}

void pool_session_execution_set_work_domain_active(bool active)
{
    gate_lock();
    s_domain_active = active;
    if (!active) {
        s_disc_issued_valid = false;
    }
    gate_unlock();
}

bool pool_session_execution_work_domain_active(void)
{
    bool active;

    gate_lock();
    active = s_domain_active;
    gate_unlock();
    return active;
}

bool pool_session_execution_delivered_view(uint8_t job_id,
                                           PoolExecDeliveredView *out)
{
    const NxDeliveredWork *rec;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if ((uint32_t)job_id >= POOL_EXEC_JOB_SLOTS) {
        return false;
    }
    gate_lock();
    rec = &s_delivered[job_id];
    out->occupied            = rec->occupied;
    out->consumed            = rec->consumed;
    out->facts_valid         = rec->facts_valid;
    out->header_unique       = rec->header_unique;
    out->discriminator_kind  = rec->disc_kind;
    out->discriminator_tag   = rec->disc_tag;
    out->prior_match_slot    = rec->prior_match_slot;
    out->protocol_generation = rec->protocol_generation;
    out->config_generation   = rec->config_generation;
    out->work_generation     = rec->work_generation;
    gate_unlock();
    return out->occupied;
}

PoolExecResultVerdict pool_session_execution_resolve_asic_result(uint8_t job_id,
                                                                 bool work_bound)
{
    PoolExecResultVerdict verdict;
    NxDeliveredWork      *rec;

    if ((uint32_t)job_id >= POOL_EXEC_JOB_SLOTS) {
        gate_lock();
        s_rejected[EXEC_RESULT_INVALID]++;
        gate_unlock();
        return EXEC_RESULT_INVALID;
    }

    gate_lock();
    rec = &s_delivered[job_id];
    if (!rec->occupied) {
        verdict = EXEC_RESULT_UNKNOWN_JOB;
    } else if (rec->protocol_generation != s_protocol_generation ||
               rec->config_generation != s_config_generation ||
               rec->work_generation != s_work_generation ||
               s_work_generation == 0u) {
        /* Delivered under an older protocol/configuration/work epoch: the
         * chip is answering for work this generation never sent. */
        verdict = EXEC_RESULT_STALE_GENERATION;
    } else if (!work_bound) {
        /* The nonce does not prove work for the item CURRENTLY in this
         * slot (probabilistic binding at the fixed local difficulty). */
        verdict = EXEC_RESULT_WORK_MISMATCH;
    } else if (rec->disc_kind != (uint8_t)POOL_EXEC_DISCRIMINATOR_EXTRANONCE2) {
        /* GENERATION-UNIQUE WORK CONTRACT, part 1: work that carries no
         * generation discriminator (SV2 standard channel, a too-narrow
         * extranonce2, an inactive domain) can never be freshness
         * evidence — the capability failure closes the path BEFORE any
         * mining verification, regardless of how valid the nonce is. */
        verdict = EXEC_RESULT_NO_DISCRIMINATOR;
    } else if (!rec->header_unique) {
        /* Part 2: this header is byte-identical to a still-relevant
         * prior-generation header, so a delayed old result would be
         * GENUINELY valid for it — proof of work cannot establish
         * freshness here and the result is excluded outright. */
        verdict = EXEC_RESULT_HEADER_ALIASED;
    } else if (rec->consumed) {
        verdict = EXEC_RESULT_ALREADY_CONSUMED;
    } else {
        /* The ONLY evidence increment: generations current, nonce bound
         * to this exact item, discriminator present, header proven
         * unique, first consumption. */
        rec->consumed = true;
        if (s_asic_job_results < UINT64_MAX) {
            s_asic_job_results++;
        }
        verdict = EXEC_RESULT_ACCEPTED;
    }
    if (verdict != EXEC_RESULT_ACCEPTED) {
        s_rejected[verdict]++;
    }
    gate_unlock();
    return verdict;
}

uint64_t pool_session_execution_asic_rejected(PoolExecResultVerdict v)
{
    uint64_t n = 0u;

    if ((unsigned)v >= (unsigned)POOL_EXEC_RESULT_VERDICT__COUNT) {
        return 0u;
    }
    gate_lock();
    n = s_rejected[v];
    gate_unlock();
    return n;
}

uint32_t pool_session_execution_begin_work_generation(void)
{
    uint32_t gen;

    gate_lock();
    s_work_generation++;
    if (s_work_generation == 0u) {
        s_work_generation = 1u; /* 0 stays the reserved "no generation" */
    }
    s_jobs_forwarded      = 0u;
    s_asic_job_results    = 0u;
    s_asic_register_reads = 0u;
    /*
     * Delivered-work records are RETAINED, not wiped. Their generation
     * stamps are now stale, so none of them can ever be credited — but
     * their canonical header facts are exactly the comparison set that
     * lets the next generation detect a byte-identical rebuilt header
     * while the chip still holds the old jobs. A retained record dies
     * only when its chip slot is reused (note_work_delivered overwrite)
     * or at the boot/test gate_reset.
     */
    s_disc_issued_valid = false; /* an issue never crosses generations */
    gen = s_work_generation;
    gate_unlock();
    return gen;
}

uint32_t pool_session_execution_begin_config_generation(void)
{
    uint32_t gen;

    gate_lock();
    s_config_generation++;
    if (s_config_generation == 0u) {
        s_config_generation = 1u;
    }
    gen = s_config_generation;
    gate_unlock();
    return gen;
}

void pool_session_execution_set_protocol_generation(uint32_t generation)
{
    gate_lock();
    s_protocol_generation = generation;
    gate_unlock();
}

uint32_t pool_session_execution_config_generation(void)
{
    uint32_t gen;

    gate_lock();
    gen = s_config_generation;
    gate_unlock();
    return gen;
}

uint32_t pool_session_execution_protocol_generation(void)
{
    uint32_t gen;

    gate_lock();
    gen = s_protocol_generation;
    gate_unlock();
    return gen;
}

/* ------------------------------------------------------------------ */
/* Live-identity slot pool (proven reader lifetime)                    */
/* ------------------------------------------------------------------ */

/*
 * See the header contract (types declared above). Static, never freed,
 * written once before publication, never reused within an epoch, reclaimed
 * only at a quiescent point, fail-closed on exhaustion.
 */
PoolExecIdentityStrings *pool_session_execution_identity_acquire(uint32_t *out_slot)
{
    PoolExecIdentityStrings *s = NULL;
    uint32_t i;

    gate_lock();
    for (i = 0u; i < POOL_EXEC_IDENTITY_SLOTS; i++) {
        if (!s_identity[i].used) {
            /* Never reused within the epoch: a reader that still holds a
             * pointer into any earlier slot cannot be affected. */
            s_identity[i].used      = true;
            s_identity[i].published = false;
            s_identity[i].refcount  = 0u;
            memset(&s_identity[i].strings, 0, sizeof(s_identity[i].strings));
            s = &s_identity[i].strings;
            if (out_slot != NULL) {
                *out_slot = i;
            }
            break;
        }
    }
    gate_unlock();
    return s; /* NULL == exhausted: the caller MUST fail closed */
}

bool pool_session_execution_identity_publish(uint32_t slot)
{
    if (slot >= POOL_EXEC_IDENTITY_SLOTS) {
        return false;
    }
    gate_lock();
    if (!s_identity[slot].used || s_identity[slot].published) {
        gate_unlock();
        return false; /* never publish an unwritten or re-published slot */
    }
    /* Publication only moves the "live" marker. Reader references are the
     * only thing that pins storage, and the previously published slot keeps
     * its own borrows (a protocol instance still reading it stays safe). */
    s_identity[slot].published = true;
    s_identity_published       = slot;
    gate_unlock();
    return true;
}

const PoolExecIdentityStrings *pool_session_execution_identity_borrow(uint32_t *out_slot)
{
    const PoolExecIdentityStrings *s = NULL;

    gate_lock();
    if (s_identity_published < POOL_EXEC_IDENTITY_SLOTS) {
        s_identity[s_identity_published].refcount++;
        s = &s_identity[s_identity_published].strings;
        if (out_slot != NULL) {
            *out_slot = s_identity_published;
        }
    }
    gate_unlock();
    return s;
}

void pool_session_execution_identity_release(uint32_t slot)
{
    if (slot >= POOL_EXEC_IDENTITY_SLOTS) {
        return;
    }
    gate_lock();
    if (s_identity[slot].refcount > 0u) {
        s_identity[slot].refcount--;
    }
    gate_unlock();
}

bool pool_session_execution_identity_copy(PoolExecIdentityCopy *out)
{
    bool ok = false;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    gate_lock();
    if (s_identity_published < POOL_EXEC_IDENTITY_SLOTS) {
        const PoolExecIdentityStrings *s = &s_identity[s_identity_published].strings;
        /* One consistent copy taken under the same lock that publishes a
         * slot: the reader can never observe a half-updated identity and
         * never retains a pointer into the pool. */
        memcpy(out->primary_host, s->primary_host, sizeof(out->primary_host));
        memcpy(out->primary_user, s->primary_user, sizeof(out->primary_user));
        memcpy(out->fallback_host, s->fallback_host, sizeof(out->fallback_host));
        memcpy(out->fallback_user, s->fallback_user, sizeof(out->fallback_user));
        ok = true;
    }
    gate_unlock();
    out->valid = ok;
    return ok;
}

uint32_t pool_session_execution_identity_reclaim_all(uint32_t *out_pinned)
{
    uint32_t reclaimed = 0u, pinned = 0u, i;

    gate_lock();
    for (i = 0u; i < POOL_EXEC_IDENTITY_SLOTS; i++) {
        if (!s_identity[i].used) {
            continue;
        }
        if (s_identity[i].refcount > 0u) {
            /* A live protocol instance or an in-progress borrow still owns
             * this slot: never reclaim it out from under a reader. */
            pinned++;
            continue;
        }
        memset(&s_identity[i], 0, sizeof(s_identity[i]));
        reclaimed++;
    }
    if (pinned == 0u) {
        s_identity_published = UINT32_MAX;
    }
    gate_unlock();
    if (out_pinned != NULL) {
        *out_pinned = pinned;
    }
    return reclaimed;
}

uint32_t pool_session_execution_identity_slots_used(void)
{
    uint32_t n = 0u, i;

    gate_lock();
    for (i = 0u; i < POOL_EXEC_IDENTITY_SLOTS; i++) {
        if (s_identity[i].used) {
            n++;
        }
    }
    gate_unlock();
    return n;
}

uint32_t pool_session_execution_identity_refcount(uint32_t slot)
{
    uint32_t n = 0u;

    if (slot >= POOL_EXEC_IDENTITY_SLOTS) {
        return 0u;
    }
    gate_lock();
    n = s_identity[slot].refcount;
    gate_unlock();
    return n;
}

uint32_t pool_session_execution_identity_published_slot(void)
{
    uint32_t s;

    gate_lock();
    s = s_identity_published;
    gate_unlock();
    return s;
}

bool pool_session_execution_asic_work_allowed(void)
{
    PoolExecGatePosture posture;
    bool grant_valid;

    gate_lock();
    posture     = s_gate_posture;
    grant_valid = s_gate_grant_valid;
    gate_unlock();
    return pool_exec_gate_allows(posture, grant_valid);
}

void pool_session_execution_note_job_forwarded(void)
{
    /* Retained for the job-pipeline delivery signal. The authoritative
     * per-item accounting is pool_session_execution_note_work_delivered(),
     * which stamps the item with its generations at the ASIC send point;
     * this entry point only counts gate releases. */
    gate_lock();
    if (s_jobs_forwarded < UINT64_MAX) {
        s_jobs_forwarded++;
    }
    gate_unlock();
}

void pool_session_execution_note_asic_register_read(void)
{
    gate_lock();
    if (s_asic_register_reads < UINT64_MAX) {
        s_asic_register_reads++;
    }
    gate_unlock();
}

PoolExecGatePosture pool_session_execution_gate_posture(void)
{
    PoolExecGatePosture posture;

    gate_lock();
    posture = s_gate_posture;
    gate_unlock();
    return posture;
}

uint32_t pool_session_execution_work_generation(void)
{
    uint32_t gen;

    gate_lock();
    gen = s_work_generation;
    gate_unlock();
    return gen;
}

uint64_t pool_session_execution_jobs_forwarded(void)
{
    uint64_t n;

    gate_lock();
    n = s_jobs_forwarded;
    gate_unlock();
    return n;
}

uint64_t pool_session_execution_asic_job_results(void)
{
    uint64_t n;

    gate_lock();
    n = s_asic_job_results;
    gate_unlock();
    return n;
}

uint64_t pool_session_execution_asic_register_reads(void)
{
    uint64_t n;

    gate_lock();
    n = s_asic_register_reads;
    gate_unlock();
    return n;
}

void pool_session_execution_gate_reset(void)
{
    gate_lock();
    s_gate_posture        = EXEC_GATE_DEFAULT_OPEN;
    s_gate_grant_valid    = false;
    s_work_generation     = 0u;
    s_config_generation   = 0u;
    s_protocol_generation = 0u;
    s_jobs_forwarded      = 0u;
    s_asic_job_results    = 0u;
    s_asic_register_reads = 0u;
    s_domain_active          = false;
    s_disc_issued_valid      = false;
    s_disc_issued_kind       = (uint8_t)POOL_EXEC_DISCRIMINATOR_NONE;
    s_disc_issued_tag        = 0u;
    s_disc_issued_generation = 0u;
    /* The per-boot discriminator budget returns ONLY here (boot/test).
     * A deinit or rebind never refunds it. */
    s_domain_tag_index = 0u;
    s_domain_tag       = 0u;
    s_domain_exhausted = false;
    s_rolling_declined = 0u;
    memset(s_delivered, 0, sizeof(s_delivered));
    memset(s_rejected, 0, sizeof(s_rejected));
    /*
     * A quiescent point by contract: no session protocol instance exists.
     * Slots still carrying a reference (a live borrow) are NOT reclaimed —
     * their storage stays valid for the reader that holds it. Unreferenced
     * slots return to the pool.
     */
    {
        uint32_t i;
        bool     any_pinned = false;
        for (i = 0u; i < POOL_EXEC_IDENTITY_SLOTS; i++) {
            if (s_identity[i].used && s_identity[i].refcount > 0u) {
                any_pinned = true;
                continue;
            }
            memset(&s_identity[i], 0, sizeof(s_identity[i]));
        }
        if (!any_pinned) {
            s_identity_published = UINT32_MAX;
        }
    }
    gate_unlock();
}

/* ------------------------------------------------------------------ */
/* Small engine helpers                                                */
/* ------------------------------------------------------------------ */

static uint64_t exec_now_us(const PoolSessionExecutor *ex)
{
    if (ex == NULL || ex->rt == NULL || ex->rt->deps.monotonic_us == NULL) {
        return 0u;
    }
    return ex->rt->deps.monotonic_us();
}

static void exec_sample_counters(PoolSessionExecutor *ex,
                                 PoolExecProtocolCounters *out)
{
    memset(out, 0, sizeof(*out));
    ex->proto_ops->counters(ex->proto_ctx, out);
    /* Delivery and HARDWARE facts come from the gate module, which zeroes
     * them per work generation — so nothing from an earlier generation can
     * reach this window. */
    out->jobs_forwarded      = pool_session_execution_jobs_forwarded();
    out->asic_job_results    = pool_session_execution_asic_job_results();
    out->asic_register_reads = pool_session_execution_asic_register_reads();
}

/* True when the grant is valid against the CURRENT bindings. */
static bool exec_grant_currently_valid(const PoolSessionExecutor *ex)
{
    return pool_exec_grant_valid(&ex->grant, ex->session.session_id,
                                 ex->rt->token.lease_generation,
                                 ex->rt->committed_generation,
                                 ex->protocol_generation);
}

/* Publish the sanitized snapshot (string-free by construction). */
static void exec_publish(PoolSessionExecutor *ex)
{
    PoolExecutionSnapshot *s = &ex->snapshot;

    pool_exec_snapshot_init(s);
    s->state             = ex->state;
    s->reason            = ex->reason;
    s->last_apply_result = ex->last_apply_result;
    s->owner             = ex->rt != NULL ? ex->rt->lease.owner : OP_OWNER_NONE;
    s->phase             = ex->rt != NULL ? ex->rt->lease.phase : OP_PHASE_UNBOOTSTRAPPED;
    s->gate              = pool_exec_state_gate(ex->state);

    s->executor_bound  = ex->bound;
    s->system_ready    = ex->system_ready;
    s->restore_required = ex->session_loaded ? ex->session.restore_required : false;
    s->config_verified  = (ex->last_apply_result == EXEC_CONFIG_APPLY_EXACT ||
                           ex->last_apply_result == EXEC_CONFIG_APPLY_NO_MUTATION);
    s->protocol_verified = ex->connection_observed_ram && ex->identity_observed_ram;
    s->job_verified      = ex->session_loaded &&
                           (ex->session.target_verify.mining_observed ||
                            ex->session.restore_verify.mining_observed);
    s->mining_grant_active = (ex->state == EXEC_STATE_TARGET_MINING) &&
                             exec_grant_currently_valid(ex);
    s->protocol_generation_present = ex->protocol_generation != 0u;

    s->target_role         = ex->apply_role;
    s->stop_attempts       = ex->stop_attempts;
    s->protocol_generation = ex->protocol_generation;
    s->work_generation     = ex->work_generation;
    s->asic_evidence_seen  = ex->asic_evidence_seen;
    s->commit_count        = ex->commit_count;
    s->event_sequence      = ex->event_sequence;
}

/*
 * Enter a substate: update the gate to the substate's contract posture,
 * arm the substate's bounded deadline and record the machine reason.
 * The OPEN_TARGET posture additionally publishes the grant validity the
 * engine just evaluated (the gate never trusts a stale evaluation).
 */
static void exec_set_state(PoolSessionExecutor *ex, PoolExecState next,
                           PoolExecReason reason)
{
    uint64_t now = exec_now_us(ex);
    PoolExecGatePosture gate = pool_exec_state_gate(next);

    if (ex->state != next) {
        ESP_LOGI(TAG, "state %s -> %s (%s)", pool_exec_state_str(ex->state),
                 pool_exec_state_str(next), pool_exec_reason_str(reason));
    }
    ex->state  = next;
    ex->reason = reason;
    if (ex->event_sequence < UINT32_MAX) {
        ex->event_sequence++;
    }

    gate_write(gate, gate == EXEC_GATE_OPEN_TARGET && exec_grant_currently_valid(ex));

    ex->phase_deadline_valid = false;
    switch (next) {
    case EXEC_STATE_TARGET_READBACK:
    case EXEC_STATE_TARGET_APPLYING:
    case EXEC_STATE_SOURCE_APPLYING:
    case EXEC_STATE_TARGET_PROTOCOL_VERIFIED:
    case EXEC_STATE_TARGET_CONFIG_VERIFIED:
    case EXEC_STATE_SOURCE_CONFIG_VERIFIED:
        ex->phase_deadline_valid = true;
        ex->phase_deadline_us = pool_exec_deadline_us(now, ex->policy.config_timeout_s);
        break;
    case EXEC_STATE_TARGET_CONNECTING:
    case EXEC_STATE_SOURCE_CONNECTING:
        ex->phase_deadline_valid = true;
        ex->phase_deadline_us  = pool_exec_deadline_us(now, ex->policy.connect_timeout_s);
        ex->job_deadline_valid = true;
        ex->job_deadline_us    = pool_exec_deadline_us(now, ex->policy.job_timeout_s);
        break;
    case EXEC_STATE_TARGET_MINING:
        ex->phase_deadline_valid = true;
        ex->phase_deadline_us = pool_exec_deadline_us(now, ex->policy.target_health_s);
        break;
    case EXEC_STATE_SOURCE_MINING_VERIFYING:
        ex->phase_deadline_valid = true;
        ex->phase_deadline_us = pool_exec_deadline_us(now, ex->policy.source_health_s);
        break;
    case EXEC_STATE_RESTORE_PENDING:
    case EXEC_STATE_COMPLETE_HANDOFF:
        ex->phase_deadline_valid = true;
        ex->phase_deadline_us = pool_exec_deadline_us(now, ex->policy.stop_timeout_s);
        break;
    default:
        break;
    }
    /* Per-transaction scratch resets on every boundary. */
    ex->cfg_pre_valid  = false;
    ex->stage_rejected = false;
    ex->stage_started  = false;
}

/* Enter the fail-closed recovery guard: revoke, inhibit, escalate B5. */
static void exec_enter_guard(PoolSessionExecutor *ex, PoolExecReason reason)
{
    pool_exec_grant_revoke(&ex->grant);
    gate_write(EXEC_GATE_INHIBITED, false);
    if (ex->rt != NULL && ex->rt->token.valid) {
        (void)pool_operation_coordinator_transition_phase(&ex->rt->coord,
                                                          &ex->rt->token,
                                                          OP_PHASE_RECOVERY_GUARD,
                                                          &ex->rt->token);
        (void)pool_operation_coordinator_snapshot(&ex->rt->coord, &ex->rt->lease);
    }
    exec_set_state(ex, EXEC_STATE_RECOVERY_GUARD, reason);
}

/* ------------------------------------------------------------------ */
/* Durable B1 transitions (commit + independent readback + B5 proof)   */
/* ------------------------------------------------------------------ */

/*
 * Drive ONE B1 event on the live session and make its outcome durable when
 * (and only when) the resulting state is a B1-persistent state:
 *   stage from the transitioned session + the committed record's time
 *   facts/counters -> commit through the SAME B3 store -> independent
 *   reload -> EXACT field-by-field readback proof -> B5 persistence proof
 *   under the CURRENT token -> adopt as the runtime's committed truth.
 * An identical staged record (ignoring generation) is NOT recommitted: the
 * committed truth already carries the boundary and rewriting it would be
 * pure flash wear (the resume path re-verifies onto an unchanged record).
 *
 * Returns EXEC_REASON_NONE on success. On a definite commit failure the
 * live session is ROLLED BACK to the committed truth so RAM never runs
 * ahead of flash; on STORE_COMMIT_UNCERTAIN the executor enters the B5
 * recovery guard (never an advancement, never a release).
 */
static PoolExecReason exec_apply_event(PoolSessionExecutor *ex,
                                       PoolSessionEventType type)
{
    PoolSessionRuntime *rt = ex->rt;
    PoolSessionEvent    ev;
    PoolSessionOutcome  out;
    PoolSession         before;
    PoolStoreResult     sr;
    PoolExecReason      reason;
    PoolOperationPersistenceProof proof;
    PoolOperationStatus os;
    uint32_t            pre_generation;
    bool                pre_obligation;

    memset(&ev, 0, sizeof(ev));
    ev.type       = type;
    ev.session_id = ex->session.session_id;

    before = ex->session;
    out    = pool_session_transition(&ex->session, &ev, &ex->session);
    if (!out.changed) {
        if (out.error != ERR_NONE) {
            ex->session = before;
            return EXEC_REASON_TRANSITION_REJECTED;
        }
        return EXEC_REASON_NONE; /* idempotent no-op (e.g. duplicate observe) */
    }

    if (!pool_state_is_persistent(ex->session.state)) {
        return EXEC_REASON_NONE; /* RAM-only span between durable boundaries */
    }

    /* Stage the durable form of the transitioned session. */
    if (pool_exec_stage_record(&ex->session, &rt->record, &ex->staged) != RECORD_OK) {
        ex->session = before;
        return EXEC_REASON_INTERNAL;
    }
    if (pool_exec_record_equal_exact(&ex->staged, &rt->record,
                                     /*ignore_generation=*/true)) {
        return EXEC_REASON_NONE; /* boundary already durable — never rewrite */
    }

    pre_generation = rt->committed_generation;
    pre_obligation = rt->record.restore_required;

    sr = pool_session_store_commit_record(&rt->store, &ex->staged);
    if (sr == STORE_COMMIT_UNCERTAIN) {
        /* The committed truth is unknowable until a reload: guard. */
        if (rt->token.valid) {
            memset(&proof, 0, sizeof(proof));
            proof.kind         = OP_PROOF_RECOVERY_UPDATE_COMMITTED;
            proof.store_result = STORE_COMMIT_UNCERTAIN;
            proof.session_id   = ex->session.session_id;
            (void)pool_operation_coordinator_apply_persistence_proof(&rt->coord,
                                                                     &rt->token,
                                                                     &proof,
                                                                     &rt->token);
            (void)pool_operation_coordinator_snapshot(&rt->coord, &rt->lease);
        }
        return EXEC_REASON_PERSIST_UNCERTAIN;
    }
    if (sr != STORE_OK) {
        ESP_LOGW(TAG, "boundary commit refused (%s)", pool_store_result_str(sr));
        ex->session = before; /* RAM never runs ahead of flash */
        return EXEC_REASON_PERSIST_FAILED;
    }

    /* Independent reload + EXACT readback proof. */
    sr = pool_session_store_load(&rt->store, &ex->reloaded, &rt->load_info);
    reason = pool_exec_verify_transition_readback(&ex->staged, &ex->reloaded,
                                                  pre_generation, pre_obligation, sr);
    if (reason != EXEC_REASON_NONE) {
        /* The committed truth is unproven: roll the live session back so a
         * bounded retry re-drives the SAME boundary (RAM never runs ahead
         * of verified flash). */
        ex->session = before;
        return reason;
    }

    /* Adopt as THE committed truth (single-truth model). */
    rt->record         = ex->reloaded;
    rt->record_present = true;
    rt->store_result   = STORE_OK;
    (void)pool_session_store_committed_generation(&rt->store,
                                                  &rt->committed_generation);
    if (ex->commit_count < UINT32_MAX) {
        ex->commit_count++;
    }

    /* Prove the durable boundary to B5 under the CURRENT token. */
    if (!rt->token.valid) {
        return EXEC_REASON_STALE_TOKEN;
    }
    memset(&proof, 0, sizeof(proof));
    proof.kind = (ex->staged.state == POOL_STATE_COMPLETE)
                     ? OP_PROOF_TERMINAL_COMMITTED
                     : OP_PROOF_RECOVERY_UPDATE_COMMITTED;
    proof.store_result                = STORE_OK;
    proof.committed_record_generation = rt->record.generation;
    proof.session_id                  = ex->session.session_id;
    proof.persisted_state             = ex->staged.state;
    proof.restore_required            = ex->staged.restore_required;

    os = pool_operation_coordinator_apply_persistence_proof(&rt->coord, &rt->token,
                                                            &proof, &rt->token);
    (void)pool_operation_coordinator_snapshot(&rt->coord, &rt->lease);
    if (os != OP_OK) {
        return EXEC_REASON_OWNERSHIP_MISMATCH;
    }
    return EXEC_REASON_NONE;
}

/*
 * Apply one B1 event with the uniform failure policy:
 *  - STORE_COMMIT_UNCERTAIN, a refused B5 proof or a stale token -> the
 *    recovery guard (never an advancement, never a release);
 *  - any other failure -> published machine reason, bounded retry of the
 *    SAME boundary on the next step (the substate deadline bounds it);
 *  - success (including idempotent no-ops) -> true.
 */
static bool exec_event_or_guard(PoolSessionExecutor *ex, PoolSessionEventType type,
                                PoolExecReason *out_reason)
{
    PoolExecReason pr = exec_apply_event(ex, type);

    if (out_reason != NULL) {
        *out_reason = pr;
    }
    if (pr == EXEC_REASON_NONE) {
        return true;
    }
    if (pr == EXEC_REASON_PERSIST_UNCERTAIN ||
        pr == EXEC_REASON_OWNERSHIP_MISMATCH || pr == EXEC_REASON_STALE_TOKEN) {
        exec_enter_guard(ex, pr);
        return false;
    }
    ESP_LOGW(TAG, "boundary retry (%s)", pool_exec_reason_str(pr));
    ex->reason = pr;
    return false;
}

/* Route the lease onto a required phase under the CURRENT token. */
static PoolExecReason exec_require_phase(PoolSessionExecutor *ex,
                                         PoolOperationLeasePhase phase)
{
    PoolSessionRuntime *rt = ex->rt;
    PoolOperationStatus os;

    if (rt->lease.phase == phase) {
        return EXEC_REASON_NONE;
    }
    if (!rt->token.valid) {
        return EXEC_REASON_STALE_TOKEN;
    }
    os = pool_operation_coordinator_transition_phase(&rt->coord, &rt->token,
                                                     phase, &rt->token);
    (void)pool_operation_coordinator_snapshot(&rt->coord, &rt->lease);
    return (os == OP_OK) ? EXEC_REASON_NONE : EXEC_REASON_PHASE_MISMATCH;
}

/* ------------------------------------------------------------------ */
/* Restoration entry (revoke first, quiesce, then drive B1)            */
/* ------------------------------------------------------------------ */

static bool exec_event_or_guard(PoolSessionExecutor *ex, PoolSessionEventType type,
                                PoolExecReason *out_reason);

static void exec_begin_restore(PoolSessionExecutor *ex, PoolExecReason reason,
                               PoolSessionEventType b1_cause)
{
    /* Order matters: close the gate BEFORE revoking and stopping so no job
     * can slip through between the decisions. */
    gate_write(EXEC_GATE_INHIBITED, false);
    pool_exec_grant_revoke(&ex->grant);
    ex->session_deadline_valid = false;

    /* Persist the restore cause when the session can take it here (the
     * TARGET_ACTIVE / VERIFYING_* paths move to RESTORE_DUE). A retryable
     * commit failure still enters RESTORE_PENDING, whose drive logic
     * re-runs the boundary under its bounded window; guard-class failures
     * stop here in the recovery guard. */
    if (b1_cause != POOL_EVT__COUNT) {
        PoolExecReason pr;
        (void)exec_event_or_guard(ex, b1_cause, &pr);
        if (ex->state == EXEC_STATE_RECOVERY_GUARD) {
            return;
        }
    }
    ex->stop_attempts = 0u;
    exec_set_state(ex, EXEC_STATE_RESTORE_PENDING, reason);
}

/* ------------------------------------------------------------------ */
/* Entry validation (fail closed on every uncertainty)                 */
/* ------------------------------------------------------------------ */

static bool exec_device_supported(PoolSessionExecutor *ex, PoolExecReason *why)
{
    char board[POOL_SESSION_BOARD_MAX];
    char asic[POOL_SESSION_ASIC_MAX];

    if (!ex->config_ops->device_identity(ex->config_ctx, board, sizeof(board),
                                         asic, sizeof(asic))) {
        *why = EXEC_REASON_BOARD_UNSUPPORTED; /* unknown hardware fails closed */
        return false;
    }
    if (!pool_exec_board_supported(board)) {
        *why = EXEC_REASON_BOARD_UNSUPPORTED;
        return false;
    }
    if (!pool_exec_asic_supported(asic)) {
        *why = EXEC_REASON_ASIC_UNSUPPORTED;
        return false;
    }
    return true;
}

static void exec_handle_entry(PoolSessionExecutor *ex)
{
    PoolSessionRuntime *rt = ex->rt;
    PoolExecReason      why = EXEC_REASON_NONE;
    PoolExecTlsVerdict  tls;

    if (!ex->system_ready) {
        ex->reason = EXEC_REASON_SYSTEM_NOT_READY;
        return; /* wait; no deadline — the posture is externally gated */
    }
    if (!exec_device_supported(ex, &why)) {
        exec_set_state(ex, EXEC_STATE_ERROR, why);
        return;
    }

    /*
     * TLS representability gate (Blocker 2) — evaluated BEFORE any B1
     * mutation intent and before any pool write, so a rejected session
     * leaves NVS, the RAM configuration, the lease and the record exactly
     * as they were and the device keeps mining its current pool. The
     * committed B1 identity carries only a BOOLEAN tls flag, so a custom
     * certificate mode could not be restored exactly — and a boolean-only
     * "match" would be a FALSE exact-restoration claim.
     */
    ex->config_ops->read_effective(ex->config_ctx, &ex->cfg_now);
    tls = pool_exec_tls_representable(&ex->cfg_now);
    if (tls != EXEC_TLS_OK) {
        exec_set_state(ex, EXEC_STATE_ERROR,
                       (tls == EXEC_TLS_UNREADABLE) ? EXEC_REASON_CONFIG_UNCERTAIN
                                                    : EXEC_REASON_TLS_MODE_UNSUPPORTED);
        return;
    }
    if (!rt->record_present || rt->record.kind != POOL_RECORD_KIND_SESSION ||
        rt->record.session_id == 0u ||
        rt->record.password_policy != POOL_SESSION_PW_KEEP_CURRENT) {
        exec_set_state(ex, EXEC_STATE_ERROR, EXEC_REASON_RECORD_INCOMPATIBLE);
        return;
    }
    if (!rt->token.valid) {
        exec_enter_guard(ex, EXEC_REASON_STALE_TOKEN);
        return;
    }

    /* Load the live session from THE committed truth. */
    if (pool_session_record_to_session(&rt->record, &ex->session) != RECORD_OK) {
        exec_set_state(ex, EXEC_STATE_ERROR, EXEC_REASON_RECORD_INCOMPATIBLE);
        return;
    }
    ex->session_loaded = true;

    /* Capture the plan's bounded remaining-time verdict for this entry. */
    ex->entry_remaining_valid = rt->plan.remaining_valid;
    ex->entry_remaining_s     = rt->plan.remaining_target_s;

    /* Normalize the post-reboot posture through the committed B1 rule.
     * (TARGET_ACTIVE -> VERIFYING_TARGET with reset evidence; mid-apply
     * states -> INTERRUPTED; restore-side spans -> VERIFYING_RESTORE.) */
    if (!exec_event_or_guard(ex, POOL_EVT_DEVICE_RESTART_OBSERVED, &why)) {
        return; /* guard, or bounded retry of the boundary on the next step */
    }

    /* Dispatch on the runtime posture + the normalized session state. */
    if (rt->decision.state == RUNTIME_VERIFY_TARGET_PENDING &&
        ex->session.state == POOL_STATE_VERIFYING_TARGET) {
        why = exec_require_phase(ex, OP_PHASE_VERIFYING_TARGET);
        if (why != EXEC_REASON_NONE) {
            exec_enter_guard(ex, why);
            return;
        }
        ex->apply_role = 1u;
        exec_set_state(ex, EXEC_STATE_TARGET_READBACK, EXEC_REASON_NONE);
        return;
    }
    if (rt->decision.state == RUNTIME_RESTORE_SOURCE_PENDING) {
        switch (ex->session.state) {
        case POOL_STATE_RESTORE_FAILED:
            /* B1: only an operator Restore Now re-attempt may retry. */
            exec_set_state(ex, EXEC_STATE_RESTORE_FAILED_HELD,
                           EXEC_REASON_RESTORE_FAILED);
            return;
        case POOL_STATE_RECOVERY_REQUIRED:
            exec_enter_guard(ex, EXEC_REASON_RECOVERY_GUARD);
            return;
        case POOL_STATE_TARGET_ACTIVE:
        case POOL_STATE_RESTORE_DUE:
        case POOL_STATE_TARGET_FAILED:
        case POOL_STATE_INTERRUPTED:
        case POOL_STATE_VERIFYING_RESTORE:
            ex->apply_role = 2u;
            exec_begin_restore(ex, EXEC_REASON_RESTORE_STARTED,
                               (ex->session.state == POOL_STATE_TARGET_ACTIVE)
                                   ? POOL_EVT_DEADLINE_REACHED
                                   : POOL_EVT__COUNT);
            return;
        default:
            exec_set_state(ex, EXEC_STATE_ERROR, EXEC_REASON_RECORD_INCOMPATIBLE);
            return;
        }
    }

    /* The action posture disappeared while validating: nothing to execute. */
    exec_set_state(ex, EXEC_STATE_IDLE, EXEC_REASON_NONE);
}

/* ------------------------------------------------------------------ */
/* Configuration transaction (bounded staged apply + readback)         */
/* ------------------------------------------------------------------ */

static const PoolConfigIdentity *exec_role_identity(const PoolSessionExecutor *ex)
{
    return (ex->apply_role == 2u) ? &ex->session.source : &ex->session.target;
}

/* One bounded poll of the running transaction. Returns the classification
 * (PENDING while the window is open). */
static PoolExecConfigApplyResult exec_poll_apply(PoolSessionExecutor *ex)
{
    const PoolConfigIdentity *identity = exec_role_identity(ex);
    bool timed_out;

    if (!ex->cfg_pre_valid) {
        ex->config_ops->read_effective(ex->config_ctx, &ex->cfg_pre);
        if (!ex->cfg_pre.valid) {
            /* No trustworthy pre-state yet: bounded wait, then UNCERTAIN. */
            if (ex->phase_deadline_valid &&
                pool_exec_deadline_reached(exec_now_us(ex), ex->phase_deadline_us)) {
                return EXEC_CONFIG_APPLY_UNCERTAIN;
            }
            return EXEC_CONFIG_PENDING;
        }
        ex->cfg_pre_valid = true;
    }
    if (!ex->stage_started) {
        ex->stage_started  = true;
        ex->stage_rejected = !ex->config_ops->stage_apply(ex->config_ctx, identity);
    }
    ex->config_ops->read_effective(ex->config_ctx, &ex->cfg_now);
    timed_out = ex->phase_deadline_valid &&
                pool_exec_deadline_reached(exec_now_us(ex), ex->phase_deadline_us);
    return pool_exec_classify_apply(&ex->cfg_pre, identity, &ex->cfg_now,
                                    ex->stage_rejected, timed_out);
}

/* Readback-only poll (no staging): used by the resume verification path. */
static PoolExecConfigApplyResult exec_poll_readback(PoolSessionExecutor *ex)
{
    const PoolConfigIdentity *identity = exec_role_identity(ex);
    uint32_t mask;

    ex->config_ops->read_effective(ex->config_ctx, &ex->cfg_now);
    if (pool_exec_effective_matches_identity(&ex->cfg_now, identity, &mask)) {
        return EXEC_CONFIG_APPLY_NO_MUTATION; /* already exactly effective */
    }
    if (ex->cfg_now.valid) {
        return EXEC_CONFIG_APPLY_READBACK_MISMATCH; /* definite difference */
    }
    if (ex->phase_deadline_valid &&
        pool_exec_deadline_reached(exec_now_us(ex), ex->phase_deadline_us)) {
        return EXEC_CONFIG_APPLY_UNCERTAIN;
    }
    return EXEC_CONFIG_PENDING;
}

/* ------------------------------------------------------------------ */
/* Controlled protocol lifecycle                                       */
/* ------------------------------------------------------------------ */

static bool exec_protocol_start(PoolSessionExecutor *ex)
{
    const PoolConfigIdentity *identity = exec_role_identity(ex);

    if (ex->proto_ops->running(ex->proto_ctx)) {
        return false; /* a stale task would poison generation binding */
    }
    /*
     * DISCRIMINATOR BUDGET FIRST — before any generation bump and before
     * any protocol action. When the per-boot budget is spent, NO further
     * controlled verification start is issued at all: no connection, no
     * delivered work, no reused domain. The caller reports the honest
     * EXEC_REASON_GENERATION_EXHAUSTED and the ordinary bounded failure
     * paths keep target mining ungranted, source COMPLETE unreachable
     * and restore_required held.
     */
    if (!pool_session_execution_allocate_generation_tag()) {
        ex->generation_exhausted = true;
        return false;
    }
    /*
     * Start a NEW work generation FIRST: this zeroes the per-generation
     * delivery and ASIC counters, so any hardware result still in flight
     * from the previous connection is discarded rather than credited to the
     * new one. Then start the engine and baseline the counters.
     */
    ex->work_generation = pool_session_execution_begin_work_generation();
    ex->protocol_generation++;
    if (ex->protocol_generation == 0u) {
        ex->protocol_generation = 1u; /* generation 0 stays reserved-invalid */
    }
    /* Publish the protocol generation BEFORE the engine can deliver work, so
     * every delivered item is stamped with the epoch it truly belongs to. */
    pool_session_execution_set_protocol_generation(ex->protocol_generation);
    if (!ex->proto_ops->start(ex->proto_ctx, identity->primary.protocol)) {
        return false;
    }
    /* The controlled work domain opens with the connection: from here on
     * the job pipeline embeds the generation discriminator whenever the
     * active protocol's capability supports one. */
    pool_session_execution_set_work_domain_active(true);
    exec_sample_counters(ex, &ex->baseline);
    ex->events_seen             = 0u;
    ex->connection_observed_ram = false;
    ex->identity_observed_ram   = false;
    ex->asic_evidence_seen      = false; /* hardware facts are per generation */
    return true;
}

/* Bounded controlled stop. True once no controlled task remains. */
static bool exec_protocol_stop(PoolSessionExecutor *ex)
{
    if (!ex->proto_ops->running(ex->proto_ctx)) {
        pool_session_execution_set_work_domain_active(false);
        return true;
    }
    if (ex->proto_ops->stop(ex->proto_ctx)) {
        pool_session_execution_set_work_domain_active(false);
        return true;
    }
    if (ex->stop_attempts < UINT8_MAX) {
        ex->stop_attempts++;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Per-substate handlers                                               */
/* ------------------------------------------------------------------ */

/*
 * Bounded failure of the target verification span. The B1 event is chosen
 * from the session's activation-span state; every path is budgeted:
 * verify-timeout retries re-apply the target from scratch, restart retries
 * re-run the activation boundary, apply failures re-run the transaction,
 * and exhaustion is TARGET_FAILED — which owes the restoration.
 */
static void exec_fail_target_verification(PoolSessionExecutor *ex,
                                          PoolExecReason reason)
{
    PoolSessionEventType evt;

    switch (ex->session.state) {
    case POOL_STATE_VERIFYING_TARGET:      evt = POOL_EVT_TARGET_VERIFY_TIMEOUT; break;
    case POOL_STATE_RESTARTING_FOR_TARGET: evt = POOL_EVT_TARGET_RETRY_REQUESTED; break;
    case POOL_STATE_APPLYING_TARGET:       evt = POOL_EVT_TARGET_APPLY_FAILED;    break;
    default:
        exec_enter_guard(ex, EXEC_REASON_INTERNAL);
        return;
    }
    if (!exec_event_or_guard(ex, evt, NULL)) {
        return;
    }
    (void)exec_protocol_stop(ex);
    switch (ex->session.state) {
    case POOL_STATE_APPLYING_TARGET:
        exec_set_state(ex, EXEC_STATE_TARGET_APPLYING, reason);
        break;
    case POOL_STATE_RESTARTING_FOR_TARGET:
        exec_set_state(ex, EXEC_STATE_TARGET_CONFIG_VERIFIED, reason);
        break;
    default: /* TARGET_FAILED: restoration is now owed */
        ex->apply_role = 2u;
        exec_begin_restore(ex, reason, POOL_EVT__COUNT);
        break;
    }
}

/* Bounded failure of the restore span (same state-aware rule); exhaustion
 * is RESTORE_FAILED — terminal, obligation retained, protocol held. */
static void exec_fail_restore_verification(PoolSessionExecutor *ex,
                                           PoolExecReason reason)
{
    PoolSessionEventType evt;

    switch (ex->session.state) {
    case POOL_STATE_VERIFYING_RESTORE:      evt = POOL_EVT_RESTORE_VERIFY_TIMEOUT; break;
    case POOL_STATE_RESTARTING_FOR_RESTORE: evt = POOL_EVT_RESTORE_RETRY_REQUESTED; break;
    case POOL_STATE_APPLYING_RESTORE:       evt = POOL_EVT_RESTORE_APPLY_FAILED;    break;
    default:
        exec_enter_guard(ex, EXEC_REASON_INTERNAL);
        return;
    }
    if (!exec_event_or_guard(ex, evt, NULL)) {
        return;
    }
    (void)exec_protocol_stop(ex);
    gate_write(EXEC_GATE_INHIBITED, false);
    switch (ex->session.state) {
    case POOL_STATE_APPLYING_RESTORE:
        exec_set_state(ex, EXEC_STATE_SOURCE_APPLYING, reason);
        break;
    case POOL_STATE_RESTARTING_FOR_RESTORE:
        exec_set_state(ex, EXEC_STATE_SOURCE_CONFIG_VERIFIED, reason);
        break;
    default: /* RESTORE_FAILED: terminal, obligation retained, protocol held */
        exec_set_state(ex, EXEC_STATE_RESTORE_FAILED_HELD, EXEC_REASON_RESTORE_FAILED);
        break;
    }
}

static void exec_step_target_readback(PoolSessionExecutor *ex)
{
    switch (exec_poll_readback(ex)) {
    case EXEC_CONFIG_APPLY_NO_MUTATION:
        ex->last_apply_result = EXEC_CONFIG_APPLY_NO_MUTATION;
        exec_set_state(ex, EXEC_STATE_TARGET_CONFIG_VERIFIED, EXEC_REASON_CONFIG_VERIFIED);
        return;
    case EXEC_CONFIG_APPLY_READBACK_MISMATCH:
        ex->last_apply_result = EXEC_CONFIG_APPLY_READBACK_MISMATCH;
        exec_fail_target_verification(ex, EXEC_REASON_CONFIG_MISMATCH);
        return;
    case EXEC_CONFIG_APPLY_UNCERTAIN:
        ex->last_apply_result = EXEC_CONFIG_APPLY_UNCERTAIN;
        exec_fail_target_verification(ex, EXEC_REASON_CONFIG_TIMEOUT);
        return;
    default:
        return; /* pending inside the bounded window */
    }
}

static void exec_step_target_applying(PoolSessionExecutor *ex)
{
    PoolExecConfigApplyResult r = exec_poll_apply(ex);

    if (!pool_exec_apply_result_final(r)) {
        return;
    }
    ex->last_apply_result = r;
    switch (r) {
    case EXEC_CONFIG_APPLY_EXACT:
        exec_set_state(ex, EXEC_STATE_TARGET_CONFIG_VERIFIED, EXEC_REASON_CONFIG_VERIFIED);
        return;
    case EXEC_CONFIG_APPLY_NO_MUTATION:
        if (pool_exec_writes_needed(&ex->cfg_pre, exec_role_identity(ex)) == 0u) {
            /* Already exactly the target: nothing to write. */
            exec_set_state(ex, EXEC_STATE_TARGET_CONFIG_VERIFIED,
                           EXEC_REASON_CONFIG_VERIFIED);
            return;
        }
        /* Definite clean failure (nothing staged): bounded B1 retry. */
        if (!exec_event_or_guard(ex, POOL_EVT_TARGET_APPLY_FAILED, NULL)) {
            return;
        }
        if (ex->session.state == POOL_STATE_APPLYING_TARGET) {
            exec_set_state(ex, EXEC_STATE_TARGET_APPLYING,
                           EXEC_REASON_CONFIG_STAGE_REJECTED);
        } else { /* TARGET_FAILED after budget exhaustion */
            ex->apply_role = 2u;
            exec_begin_restore(ex, EXEC_REASON_CONFIG_STAGE_REJECTED,
                               POOL_EVT__COUNT);
        }
        return;
    case EXEC_CONFIG_APPLY_PARTIAL:
    case EXEC_CONFIG_APPLY_UNCERTAIN:
    case EXEC_CONFIG_APPLY_READBACK_MISMATCH: {
        /* Uncertain target mutation NEVER continues toward the target:
         * restoration is the only permitted direction (contract 14). */
        PoolExecReason cause = (r == EXEC_CONFIG_APPLY_PARTIAL)
                                   ? EXEC_REASON_CONFIG_PARTIAL
                                   : (r == EXEC_CONFIG_APPLY_UNCERTAIN)
                                         ? EXEC_REASON_CONFIG_UNCERTAIN
                                         : EXEC_REASON_CONFIG_MISMATCH;
        if (!exec_event_or_guard(ex, POOL_EVT_RESTORE_NOW_REQUESTED, NULL)) {
            return;
        }
        ex->apply_role = 2u;
        exec_begin_restore(ex, cause, POOL_EVT__COUNT);
        return;
    }
    default:
        return;
    }
}

/* Shared activation boundary: live-copy refresh, B1 restart-span events,
 * then the controlled protocol start. */
static void exec_step_config_verified(PoolSessionExecutor *ex, bool target_side)
{
    PoolExecReason pr;

    /* March the B1 session across its activation span. On the readback-only
     * resume paths the session is already in the VERIFYING state and these
     * events are simply not applicable. */
    if (target_side && ex->session.state == POOL_STATE_APPLYING_TARGET) {
        if (!exec_event_or_guard(ex, POOL_EVT_TARGET_RESTART_STARTED, &pr)) {
            return;
        }
    }
    if (!target_side && ex->session.state == POOL_STATE_APPLYING_RESTORE) {
        if (!exec_event_or_guard(ex, POOL_EVT_RESTORE_RESTART_STARTED, &pr)) {
            return;
        }
    }

    /* Refresh the live protocol-facing copies while nothing is running. */
    if (ex->proto_ops->running(ex->proto_ctx)) {
        if (!exec_protocol_stop(ex)) {
            if (ex->stop_attempts > ex->policy.stop_retry_max) {
                exec_enter_guard(ex, EXEC_REASON_PROTOCOL_STOP_FAILED);
            }
            return;
        }
    }
    if (!ex->config_ops->refresh_live(ex->config_ctx, exec_role_identity(ex))) {
        /* Includes identity-slot exhaustion: the adapter fails closed and
         * publishes nothing, so no protocol instance is started against an
         * unowned or truncated identity. */
        if (target_side) {
            exec_fail_target_verification(ex, EXEC_REASON_CONFIG_STAGE_REJECTED);
        } else {
            exec_fail_restore_verification(ex, EXEC_REASON_CONFIG_STAGE_REJECTED);
        }
        return;
    }
    /* A NEW live configuration is now published: work delivered under the
     * previous configuration can never be credited to this one. */
    ex->config_generation = pool_session_execution_begin_config_generation();

    if (target_side && ex->session.state == POOL_STATE_RESTARTING_FOR_TARGET) {
        if (!exec_event_or_guard(ex, POOL_EVT_TARGET_RESTART_COMPLETE, &pr)) {
            return;
        }
    }
    if (!target_side && ex->session.state == POOL_STATE_RESTARTING_FOR_RESTORE) {
        if (!exec_event_or_guard(ex, POOL_EVT_RESTORE_RESTART_COMPLETE, &pr)) {
            return;
        }
    }
    (void)pr;

    /* Verification-only protocol start: the delivery gate stays INHIBITED
     * (the substate contract pins it) until a grant or a verified source. */
    if (!exec_protocol_start(ex)) {
        PoolExecReason why = ex->generation_exhausted
                                 ? EXEC_REASON_GENERATION_EXHAUSTED
                                 : EXEC_REASON_PROTOCOL_START_FAILED;
        if (target_side) {
            exec_fail_target_verification(ex, why);
        } else {
            exec_fail_restore_verification(ex, why);
        }
        return;
    }
    exec_set_state(ex, target_side ? EXEC_STATE_TARGET_CONNECTING
                                   : EXEC_STATE_SOURCE_CONNECTING,
                   EXEC_REASON_NONE);
}

static void exec_step_connecting(PoolSessionExecutor *ex, bool target_side)
{
    PoolExecProtocolCounters now_counters;
    PoolExecEvidence         evidence;
    PoolExecReason           pr;
    uint64_t                 now = exec_now_us(ex);
    uint32_t                 mask;

    ex->events_seen |= pool_exec_protocol_events_sanitize(
        ex->proto_ops->poll_events(ex->proto_ctx));
    exec_sample_counters(ex, &now_counters);
    pool_exec_evaluate_evidence(&ex->baseline, &now_counters, ex->events_seen,
                                &evidence);

    if (evidence.anomaly) {
        if (target_side) {
            exec_fail_target_verification(ex, EXEC_REASON_EVIDENCE_STALE);
        } else {
            exec_fail_restore_verification(ex, EXEC_REASON_EVIDENCE_STALE);
        }
        return;
    }
    if ((ex->events_seen & (EXEC_PEVT_CONNECTION_FAILED | EXEC_PEVT_TASK_EXITED |
                            EXEC_PEVT_SHUTDOWN_FAILED)) != 0u) {
        if (target_side) {
            exec_fail_target_verification(ex, EXEC_REASON_PROTOCOL_FAILED_EVENT);
        } else {
            exec_fail_restore_verification(ex, EXEC_REASON_PROTOCOL_FAILED_EVENT);
        }
        return;
    }

    /* Connection evidence -> the B1 connection observation (RAM span). */
    if (evidence.connection_evidence && !ex->connection_observed_ram) {
        if (!exec_event_or_guard(ex, target_side ? POOL_EVT_TARGET_CONNECTION_OBSERVED
                                                 : POOL_EVT_RESTORE_CONNECTION_OBSERVED,
                                 &pr)) {
            return;
        }
        ex->connection_observed_ram = true;
    }

    /* Endpoint identity: the exact configuration is still effective AND the
     * connection generation is current. Honest claim only — configuration +
     * resolver + transport binding (TLS common-name when enabled), never a
     * cryptographic pool-identity proof. */
    if (ex->connection_observed_ram && !ex->identity_observed_ram) {
        ex->config_ops->read_effective(ex->config_ctx, &ex->cfg_now);
        if (ex->cfg_now.valid &&
            pool_exec_effective_matches_identity(&ex->cfg_now,
                                                 exec_role_identity(ex), &mask)) {
            if (!exec_event_or_guard(ex, target_side ? POOL_EVT_TARGET_HOST_VERIFIED
                                                     : POOL_EVT_RESTORE_IDENTITY_VERIFIED,
                                     &pr)) {
                return;
            }
            ex->identity_observed_ram = true;
        } else if (ex->cfg_now.valid) {
            /* The live configuration no longer matches the expected
             * identity: inhibit immediately and fail the verification. */
            if (target_side) {
                exec_fail_target_verification(ex, EXEC_REASON_IDENTITY_MISMATCH);
            } else {
                exec_fail_restore_verification(ex, EXEC_REASON_IDENTITY_MISMATCH);
            }
            return;
        }
    }

    if (target_side) {
        /* Valid target-sourced work completes the pre-grant evidence. */
        if (ex->connection_observed_ram && ex->identity_observed_ram &&
            evidence.job_evidence) {
            if (!exec_event_or_guard(ex, POOL_EVT_TARGET_MINING_OBSERVED, &pr)) {
                return;
            }
            if (ex->session.state == POOL_STATE_TARGET_ACTIVE) {
                /* TARGET_ACTIVE is durable; route the lease to ACTIVE. */
                pr = exec_require_phase(ex, OP_PHASE_ACTIVE);
                if (pr != EXEC_REASON_NONE) {
                    exec_enter_guard(ex, pr);
                    return;
                }
                exec_set_state(ex, EXEC_STATE_TARGET_PROTOCOL_VERIFIED,
                               EXEC_REASON_NONE);
            }
            return;
        }
    } else {
        /* Source side: connection + identity open the delivery gate; the
         * mining-resumed proof runs in the dedicated verification window. */
        if (ex->connection_observed_ram && ex->identity_observed_ram) {
            exec_set_state(ex, EXEC_STATE_SOURCE_PROTOCOL_VERIFIED, EXEC_REASON_NONE);
            return;
        }
    }

    /* Bounded windows. */
    if (!ex->connection_observed_ram && ex->phase_deadline_valid &&
        pool_exec_deadline_reached(now, ex->phase_deadline_us)) {
        if (target_side) {
            exec_fail_target_verification(ex, EXEC_REASON_CONNECT_TIMEOUT);
        } else {
            exec_fail_restore_verification(ex, EXEC_REASON_CONNECT_TIMEOUT);
        }
        return;
    }
    if (ex->job_deadline_valid &&
        pool_exec_deadline_reached(now, ex->job_deadline_us)) {
        if (target_side) {
            exec_fail_target_verification(ex, EXEC_REASON_JOB_TIMEOUT);
        } else {
            exec_fail_restore_verification(ex, EXEC_REASON_JOB_TIMEOUT);
        }
        return;
    }
}

static void exec_step_target_protocol_verified(PoolSessionExecutor *ex)
{
    /* Grant issuance: recheck the CURRENT token, bind every generation.
     * The committed TARGET_ACTIVE record is the persistence proof — the
     * grant may only bind to it. */
    if (!ex->rt->token.valid) {
        exec_enter_guard(ex, EXEC_REASON_STALE_TOKEN);
        return;
    }
    if (!ex->entry_remaining_valid) {
        /* No trustworthy remaining-session bound: never mine blind. */
        ex->apply_role = 2u;
        exec_begin_restore(ex, EXEC_REASON_DEADLINE_REACHED, POOL_EVT_DEADLINE_REACHED);
        return;
    }
    if (ex->issue_sequence < UINT32_MAX) {
        ex->issue_sequence++;
    }
    if (!pool_exec_grant_issue(&ex->grant, ex->session.session_id,
                               ex->rt->token.lease_generation,
                               ex->rt->committed_generation,
                               ex->protocol_generation, ex->issue_sequence)) {
        exec_enter_guard(ex, EXEC_REASON_INTERNAL);
        return;
    }
    /* Arm the bounded session deadline from the plan's remaining verdict. */
    {
        uint64_t remaining = ex->entry_remaining_s;
        if (remaining > (uint64_t)POOL_SESSION_MAX_DURATION_S) {
            remaining = POOL_SESSION_MAX_DURATION_S;
        }
        ex->session_deadline_valid = true;
        ex->session_deadline_us =
            pool_exec_deadline_us(exec_now_us(ex), (uint32_t)remaining);
    }
    ex->target_health_ok = false;
    exec_sample_counters(ex, &ex->baseline); /* health window baseline */
    ex->events_seen = 0u;
    exec_set_state(ex, EXEC_STATE_TARGET_MINING, EXEC_REASON_GRANT_ISSUED);
}

static void exec_step_target_mining(PoolSessionExecutor *ex)
{
    PoolExecProtocolCounters now_counters;
    PoolExecEvidence         evidence;
    uint64_t                 now = exec_now_us(ex);

    /* The grant must remain valid against EVERY current binding. */
    if (!exec_grant_currently_valid(ex)) {
        exec_begin_restore(ex, EXEC_REASON_GRANT_REVOKED, POOL_EVT_RESTORE_NOW_REQUESTED);
        return;
    }
    /* Refresh the published grant validity for the gate. */
    gate_write(EXEC_GATE_OPEN_TARGET, true);

    ex->events_seen |= pool_exec_protocol_events_sanitize(
        ex->proto_ops->poll_events(ex->proto_ctx));
    if ((ex->events_seen & (EXEC_PEVT_CONNECTION_FAILED | EXEC_PEVT_TASK_EXITED |
                            EXEC_PEVT_SHUTDOWN_FAILED)) != 0u) {
        exec_begin_restore(ex, EXEC_REASON_PROTOCOL_FAILED_EVENT,
                           POOL_EVT_RESTORE_NOW_REQUESTED);
        return;
    }

    /* Session deadline: the monotonic bound armed at grant, or the B4 plan
     * turning to restoration under trusted time. */
    if ((ex->session_deadline_valid &&
         pool_exec_deadline_reached(now, ex->session_deadline_us)) ||
        ex->rt->plan.decision == POOL_BOOT_DECISION_RESTORE_SOURCE_NOW ||
        ex->rt->plan.decision == POOL_BOOT_DECISION_VERIFY_RESTORE) {
        exec_begin_restore(ex, EXEC_REASON_DEADLINE_REACHED, POOL_EVT_DEADLINE_REACHED);
        return;
    }

    /*
     * Initial bounded health window. The full mining-verified predicate is
     * required: the pool keeps serving work for THIS generation, work
     * reached the ASIC through the delivery gate, and the BM1370 returned a
     * processing result for it. Software-side delivery alone (a dequeue, an
     * ASIC_send_work call, a gate counter tick) is NOT accepted. No invented
     * hashrate threshold and no share requirement; the window is a bounded
     * TIME limit whose expiry fails safe into restoration.
     */
    if (!ex->target_health_ok) {
        exec_sample_counters(ex, &now_counters);
        pool_exec_evaluate_evidence(&ex->baseline, &now_counters, ex->events_seen,
                                    &evidence);
        if (evidence.anomaly) {
            exec_begin_restore(ex, EXEC_REASON_EVIDENCE_STALE,
                               POOL_EVT_RESTORE_NOW_REQUESTED);
            return;
        }
        ex->asic_evidence_seen = ex->asic_evidence_seen ||
                                 evidence.asic_processing_evidence;
        if (pool_exec_evidence_mining_verified(&evidence)) {
            ex->target_health_ok = true;
        } else if (ex->phase_deadline_valid &&
                   pool_exec_deadline_reached(now, ex->phase_deadline_us)) {
            exec_begin_restore(ex,
                               (evidence.job_evidence && evidence.forward_evidence)
                                   ? EXEC_REASON_ASIC_EVIDENCE_MISSING
                                   : EXEC_REASON_HEALTH_FAILED,
                               POOL_EVT_RESTORE_NOW_REQUESTED);
            return;
        }
    }
}

static void exec_step_restore_pending(PoolSessionExecutor *ex)
{
    PoolExecReason pr;

    /* Quiesce first: no source mutation while a session task may run. */
    if (!exec_protocol_stop(ex)) {
        if (ex->stop_attempts > ex->policy.stop_retry_max) {
            exec_enter_guard(ex, EXEC_REASON_PROTOCOL_STOP_FAILED);
        } else if (ex->phase_deadline_valid &&
                   pool_exec_deadline_reached(exec_now_us(ex), ex->phase_deadline_us)) {
            exec_enter_guard(ex, EXEC_REASON_PROTOCOL_STOP_FAILED);
        }
        return;
    }

    /* Route the lease onto the restoration phase (token rotates). */
    pr = exec_require_phase(ex, OP_PHASE_RESTORING_SOURCE);
    if (pr != EXEC_REASON_NONE) {
        exec_enter_guard(ex, pr);
        return;
    }

    /* Drive the B1 session onto the restore-apply boundary. */
    switch (ex->session.state) {
    case POOL_STATE_TARGET_ACTIVE:
        if (!exec_event_or_guard(ex, POOL_EVT_DEADLINE_REACHED, &pr)) {
            return;
        }
        break;
    case POOL_STATE_VERIFYING_TARGET:
    case POOL_STATE_RESTARTING_FOR_TARGET:
    case POOL_STATE_APPLYING_TARGET:
        if (!exec_event_or_guard(ex, POOL_EVT_RESTORE_NOW_REQUESTED, &pr)) {
            return;
        }
        break;
    case POOL_STATE_RESTORE_DUE:
    case POOL_STATE_TARGET_FAILED:
    case POOL_STATE_INTERRUPTED:
        if (!exec_event_or_guard(ex, POOL_EVT_RESTORE_APPLY_REQUESTED, &pr)) {
            return;
        }
        break;
    case POOL_STATE_VERIFYING_RESTORE:
        /* Boot re-entry mid-restore: verify the effective configuration
         * before any protocol action (SOURCE_APPLYING handles both the
         * readback-only and the re-apply cases). */
        ex->apply_role = 2u;
        exec_set_state(ex, EXEC_STATE_SOURCE_APPLYING, EXEC_REASON_RESTORE_STARTED);
        return;
    case POOL_STATE_RESTORE_FAILED:
        exec_set_state(ex, EXEC_STATE_RESTORE_FAILED_HELD, EXEC_REASON_RESTORE_FAILED);
        return;
    default:
        exec_enter_guard(ex, EXEC_REASON_INTERNAL);
        return;
    }

    if (ex->session.state == POOL_STATE_APPLYING_RESTORE) {
        ex->apply_role = 2u;
        exec_set_state(ex, EXEC_STATE_SOURCE_APPLYING, EXEC_REASON_RESTORE_STARTED);
    }
    /* RESTORE_DUE waits one more step for the apply boundary. */
}

static void exec_step_source_applying(PoolSessionExecutor *ex)
{
    PoolExecConfigApplyResult r;
    PoolExecReason            pr;

    /* Boot re-entry over VERIFYING_RESTORE is readback-only first. */
    if (ex->session.state == POOL_STATE_VERIFYING_RESTORE) {
        r = exec_poll_readback(ex);
        if (r == EXEC_CONFIG_APPLY_NO_MUTATION) {
            ex->last_apply_result = r;
            exec_set_state(ex, EXEC_STATE_SOURCE_CONFIG_VERIFIED,
                           EXEC_REASON_CONFIG_VERIFIED);
            return;
        }
        if (r == EXEC_CONFIG_APPLY_READBACK_MISMATCH ||
            r == EXEC_CONFIG_APPLY_UNCERTAIN) {
            ex->last_apply_result = r;
            /* Bounded: consumes a restore-verify retry and re-applies. */
            exec_fail_restore_verification(ex,
                                           r == EXEC_CONFIG_APPLY_UNCERTAIN
                                               ? EXEC_REASON_CONFIG_TIMEOUT
                                               : EXEC_REASON_CONFIG_MISMATCH);
            return;
        }
        return; /* pending */
    }

    r = exec_poll_apply(ex);
    if (!pool_exec_apply_result_final(r)) {
        return;
    }
    ex->last_apply_result = r;
    if (r == EXEC_CONFIG_APPLY_EXACT ||
        (r == EXEC_CONFIG_APPLY_NO_MUTATION &&
         pool_exec_writes_needed(&ex->cfg_pre, exec_role_identity(ex)) == 0u)) {
        exec_set_state(ex, EXEC_STATE_SOURCE_CONFIG_VERIFIED,
                       EXEC_REASON_CONFIG_VERIFIED);
        return;
    }

    /* Every other final verdict is a bounded restore-apply failure: B1
     * retries the source transaction, then RESTORE_FAILED retains the
     * obligation with the protocol held. */
    if (!exec_event_or_guard(ex, POOL_EVT_RESTORE_APPLY_FAILED, &pr)) {
        return;
    }
    if (ex->session.state == POOL_STATE_APPLYING_RESTORE) {
        exec_set_state(ex, EXEC_STATE_SOURCE_APPLYING,
                       (r == EXEC_CONFIG_APPLY_PARTIAL)   ? EXEC_REASON_CONFIG_PARTIAL
                       : (r == EXEC_CONFIG_APPLY_UNCERTAIN) ? EXEC_REASON_CONFIG_UNCERTAIN
                       : (r == EXEC_CONFIG_APPLY_READBACK_MISMATCH)
                           ? EXEC_REASON_CONFIG_MISMATCH
                           : EXEC_REASON_CONFIG_STAGE_REJECTED);
    } else {
        exec_set_state(ex, EXEC_STATE_RESTORE_FAILED_HELD, EXEC_REASON_RESTORE_FAILED);
    }
}

static void exec_step_source_protocol_verified(PoolSessionExecutor *ex)
{
    /* Connection + exact identity are proven: open the source side of the
     * delivery gate so mining can actually resume, then prove it within
     * the bounded window. */
    exec_sample_counters(ex, &ex->baseline);
    ex->events_seen = 0u;
    exec_set_state(ex, EXEC_STATE_SOURCE_MINING_VERIFYING, EXEC_REASON_NONE);
}

static void exec_step_source_mining_verifying(PoolSessionExecutor *ex)
{
    PoolExecProtocolCounters now_counters;
    PoolExecEvidence         evidence;
    PoolExecReason           pr;
    uint64_t                 now = exec_now_us(ex);

    ex->events_seen |= pool_exec_protocol_events_sanitize(
        ex->proto_ops->poll_events(ex->proto_ctx));
    if ((ex->events_seen & (EXEC_PEVT_CONNECTION_FAILED | EXEC_PEVT_TASK_EXITED |
                            EXEC_PEVT_SHUTDOWN_FAILED)) != 0u) {
        exec_fail_restore_verification(ex, EXEC_REASON_PROTOCOL_FAILED_EVENT);
        return;
    }
    exec_sample_counters(ex, &now_counters);
    pool_exec_evaluate_evidence(&ex->baseline, &now_counters, ex->events_seen,
                                &evidence);
    if (evidence.anomaly) {
        exec_fail_restore_verification(ex, EXEC_REASON_EVIDENCE_STALE);
        return;
    }

    ex->asic_evidence_seen = ex->asic_evidence_seen ||
                             evidence.asic_processing_evidence;
    if (pool_exec_evidence_mining_verified(&evidence)) {
        /*
         * The audited bounded criterion for "source mining resumed": pool
         * work for THIS generation, delivered through the gate, and an ASIC
         * processing result for it. The COMPLETE transition below is the
         * ONLY discharge of the restore obligation, and it is persisted
         * with the full restore evidence.
         *
         * OWNERSHIP: the session lease is deliberately NOT released here.
         * It is released only after the protocol handoff is proven, so no
         * window exists in which a session-owned protocol engine runs with
         * no owner (see exec_step_complete_handoff).
         */
        if (!exec_event_or_guard(ex, POOL_EVT_RESTORE_MINING_OBSERVED, &pr)) {
            return;
        }
        if (ex->session.state == POOL_STATE_COMPLETE) {
            ex->handoff_attempted = false;
            ex->stop_attempts     = 0u;
            exec_set_state(ex, EXEC_STATE_COMPLETE_HANDOFF,
                           EXEC_REASON_COMPLETE_VERIFIED);
        }
        return;
    }

    if (ex->phase_deadline_valid && pool_exec_deadline_reached(now, ex->phase_deadline_us)) {
        exec_fail_restore_verification(ex,
                                       (evidence.job_evidence && evidence.forward_evidence)
                                           ? EXEC_REASON_ASIC_EVIDENCE_MISSING
                                           : EXEC_REASON_HEALTH_FAILED);
        return;
    }
}

/*
 * THE post-COMPLETE handoff contract (stop-then-restart-under-coordinator).
 *
 * COMPLETE is already durable and the restore obligation is discharged, but
 * the session lease is still HELD — deliberately. The exact sequence is:
 *
 *   1. stop the controlled protocol engine (bounded retries);
 *   2. start the production coordinator, which then owns its own engine;
 *   3. ONLY THEN release the session lease (FREE, terminal retained).
 *
 * Invariants this ordering guarantees:
 *   - never two protocol engines: the coordinator is started only after the
 *     controlled engine provably exited;
 *   - never an ownerless running engine: the lease is released only after
 *     the production coordinator has taken over;
 *   - never FREE while a session-owned engine exists;
 *   - a failure has ONE truthful posture (EXEC_STATE_HANDOFF_FAILED): the
 *     session lease is retained, the delivery gate stays INHIBITED and no
 *     claim is made that mining continues;
 *   - bounded: stop retries are capped and the coordinator start is a
 *     single attempt — there is no unbounded retry anywhere.
 */
static void exec_step_complete_handoff(PoolSessionExecutor *ex)
{
    PoolSessionRuntime *rt = ex->rt;
    PoolOperationStatus os;

    /* 1. Stop the controlled engine. Ownership is retained throughout. */
    if (!exec_protocol_stop(ex)) {
        if (ex->stop_attempts > ex->policy.stop_retry_max ||
            (ex->phase_deadline_valid &&
             pool_exec_deadline_reached(exec_now_us(ex), ex->phase_deadline_us))) {
            /* The controlled engine would not exit. Never start a second
             * engine beside it and never release the lease: an operator
             * owns this from here. No mining is claimed. */
            exec_set_state(ex, EXEC_STATE_HANDOFF_FAILED,
                           EXEC_REASON_HANDOFF_STOP_FAILED);
        }
        return;
    }

    /* 2. Start the production coordinator (single bounded attempt). */
    if (!ex->handoff_attempted) {
        ex->handoff_attempted = true;
        if (!ex->proto_ops->handoff_source(ex->proto_ctx)) {
            /* No engine is running and none could be started: hold the
             * lease and report the failure truthfully. */
            exec_set_state(ex, EXEC_STATE_HANDOFF_FAILED,
                           EXEC_REASON_HANDOFF_START_FAILED);
            return;
        }
    }

    /* 3. The production coordinator owns mining now: release the session
     *    lease. The terminal result stays retained for the (out-of-B7)
     *    acknowledgement. A refused release is an ownership inconsistency
     *    and enters the guard rather than silently dropping the lease. */
    if (rt->token.valid && rt->lease.phase == OP_PHASE_TERMINAL_ACK_PENDING) {
        os = pool_operation_coordinator_release_session(&rt->coord, &rt->token);
        (void)pool_operation_coordinator_snapshot(&rt->coord, &rt->lease);
        if (os != OP_OK) {
            exec_enter_guard(ex, EXEC_REASON_OWNERSHIP_MISMATCH);
            return;
        }
        rt->token.valid = false; /* released: no live lease remains */
    }
    exec_set_state(ex, EXEC_STATE_DONE, EXEC_REASON_COMPLETE_VERIFIED);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void pool_session_executor_init(PoolSessionExecutor *ex)
{
    if (ex == NULL) {
        return;
    }
    memset(ex, 0, sizeof(*ex));
    ex->initialized = true;
    ex->state       = EXEC_STATE_DISABLED;
    pool_exec_grant_init(&ex->grant);
    pool_exec_snapshot_init(&ex->snapshot);
}

PoolExecReason pool_session_executor_bind(PoolSessionExecutor *ex,
                                          PoolSessionRuntime *rt,
                                          const PoolExecConfigOps *config_ops,
                                          void *config_ctx,
                                          const PoolExecProtocolOps *proto_ops,
                                          void *proto_ctx,
                                          const PoolExecPolicy *policy)
{
    if (ex == NULL || !ex->initialized) {
        return EXEC_REASON_NOT_BOUND;
    }
    if (rt == NULL || !rt->initialized || !rt->booted ||
        config_ops == NULL || proto_ops == NULL ||
        config_ops->device_identity == NULL || config_ops->stage_apply == NULL ||
        config_ops->read_effective == NULL || config_ops->refresh_live == NULL ||
        proto_ops->start == NULL || proto_ops->stop == NULL ||
        proto_ops->running == NULL || proto_ops->poll_events == NULL ||
        proto_ops->counters == NULL || proto_ops->handoff_source == NULL) {
        return EXEC_REASON_NOT_BOUND; /* fail closed: nothing binds partially */
    }
    ex->rt         = rt;
    ex->config_ops = config_ops;
    ex->config_ctx = config_ctx;
    ex->proto_ops  = proto_ops;
    ex->proto_ctx  = proto_ctx;
    if (policy != NULL) {
        ex->policy = *policy;
    } else {
        pool_exec_policy_defaults(&ex->policy);
    }
    pool_exec_policy_clamp(&ex->policy);
    ex->bound = true;
    exec_set_state(ex, EXEC_STATE_IDLE, EXEC_REASON_NONE);
    exec_publish(ex);
    return EXEC_REASON_NONE;
}

void pool_session_executor_deinit(PoolSessionExecutor *ex)
{
    if (ex == NULL) {
        return;
    }
    /* Never silently reopen a held gate: a mid-execution deinit keeps the
     * INHIBITED posture (only a fresh boot-time gate_reset clears it). The
     * work domain, however, closes with its executor — an unowned epoch
     * must never keep tagging (or crediting) new work. */
    pool_session_execution_set_work_domain_active(false);
    memset(ex, 0, sizeof(*ex));
}

void pool_session_executor_set_system_ready(PoolSessionExecutor *ex, bool ready)
{
    if (ex != NULL && ex->initialized) {
        ex->system_ready = ready;
    }
}

bool pool_session_executor_owns_flow(const PoolSessionExecutor *ex)
{
    if (ex == NULL || !ex->initialized || !ex->bound) {
        return false;
    }
    return pool_exec_state_owns_flow(ex->state);
}

PoolExecReason pool_session_executor_snapshot(const PoolSessionExecutor *ex,
                                              PoolExecutionSnapshot *out)
{
    if (out == NULL) {
        return EXEC_REASON_INTERNAL;
    }
    if (ex == NULL || !ex->initialized) {
        pool_exec_snapshot_init(out);
        return EXEC_REASON_NOT_BOUND;
    }
    *out = ex->snapshot;
    return EXEC_REASON_NONE;
}

PoolExecState pool_session_executor_step(PoolSessionExecutor *ex)
{
    PoolSessionRuntime *rt;

    if (ex == NULL || !ex->initialized || !ex->bound || ex->rt == NULL) {
        return EXEC_STATE_DISABLED;
    }
    rt = ex->rt;

    /* Consistent lease view for this step. */
    (void)pool_operation_coordinator_snapshot(&rt->coord, &rt->lease);

    /* Ownership sanity for every OWNING substate: a lease that no longer
     * matches the substate contract revokes and guards (fail closed). */
    if (pool_exec_state_owns_flow(ex->state) &&
        !pool_exec_state_ownership_compatible(ex->state, rt->lease.owner,
                                              rt->lease.phase)) {
        exec_enter_guard(ex, EXEC_REASON_OWNERSHIP_MISMATCH);
        exec_publish(ex);
        return ex->state;
    }

    switch (ex->state) {
    case EXEC_STATE_IDLE:
        if (rt->booted && rt->record_present &&
            (rt->decision.state == RUNTIME_VERIFY_TARGET_PENDING ||
             rt->decision.state == RUNTIME_RESTORE_SOURCE_PENDING)) {
            exec_set_state(ex, EXEC_STATE_ENTRY_PENDING, EXEC_REASON_NONE);
        }
        break;
    case EXEC_STATE_ENTRY_PENDING:
        exec_handle_entry(ex);
        break;
    case EXEC_STATE_TARGET_READBACK:
        exec_step_target_readback(ex);
        break;
    case EXEC_STATE_TARGET_APPLYING:
        exec_step_target_applying(ex);
        break;
    case EXEC_STATE_TARGET_CONFIG_VERIFIED:
        exec_step_config_verified(ex, /*target_side=*/true);
        break;
    case EXEC_STATE_TARGET_CONNECTING:
        exec_step_connecting(ex, /*target_side=*/true);
        break;
    case EXEC_STATE_TARGET_PROTOCOL_VERIFIED:
        exec_step_target_protocol_verified(ex);
        break;
    case EXEC_STATE_TARGET_MINING:
        exec_step_target_mining(ex);
        break;
    case EXEC_STATE_RESTORE_PENDING:
        exec_step_restore_pending(ex);
        break;
    case EXEC_STATE_SOURCE_APPLYING:
        exec_step_source_applying(ex);
        break;
    case EXEC_STATE_SOURCE_CONFIG_VERIFIED:
        exec_step_config_verified(ex, /*target_side=*/false);
        break;
    case EXEC_STATE_SOURCE_CONNECTING:
        exec_step_connecting(ex, /*target_side=*/false);
        break;
    case EXEC_STATE_SOURCE_PROTOCOL_VERIFIED:
        exec_step_source_protocol_verified(ex);
        break;
    case EXEC_STATE_SOURCE_MINING_VERIFYING:
        exec_step_source_mining_verifying(ex);
        break;
    case EXEC_STATE_COMPLETE_HANDOFF:
        exec_step_complete_handoff(ex);
        break;
    case EXEC_STATE_DONE:
    case EXEC_STATE_HANDOFF_FAILED:
    case EXEC_STATE_RESTORE_FAILED_HELD:
    case EXEC_STATE_RECOVERY_GUARD:
    case EXEC_STATE_ERROR:
    case EXEC_STATE_DISABLED:
        break; /* terminal/held postures perform no action */
    default:
        exec_enter_guard(ex, EXEC_REASON_INTERNAL); /* unknown state: guard */
        break;
    }

    exec_publish(ex);
    return ex->state;
}
