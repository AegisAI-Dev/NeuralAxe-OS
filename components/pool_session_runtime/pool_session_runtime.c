/*
 * NeuralAxe timed pool sessions — ESP-IDF runtime adapter (Gate B6).
 *
 * See pool_session_runtime.h for the full contract. This translation unit
 * owns the ONLY production runtime instance, the ONLY runtime owner task and
 * the ONLY reset-reason read. It contains NO pool-configuration change, NO
 * Stratum call, NO OTA, NO restart and NO HTTP surface: every such call site
 * belongs to Gate B7 or later.
 */

#include <assert.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "pool_session_runtime.h"
#include "pool_session_reset.h"

#if defined(CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS) && \
    !defined(CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE)
#error "Gate B10.1 pilot diagnostics require CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE"
#endif

#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS
/*
 * Gate B10.1 — bounded observation-pilot diagnostics. Compiled ONLY under the
 * pilot flag; with it off not one symbol, byte of RAM or log line exists and
 * this adapter is byte-for-byte the committed Gate B10 adapter.
 */
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "pool_session_runtime_pilot.h"
#endif

/* ------------------------------------------------------------------ */
/* Bounded task configuration                                          */
/* ------------------------------------------------------------------ */

/*
 * Statically allocated: no heap is used for the runtime task at any point.
 * Every large working struct lives inside PoolSessionRuntime, so the task
 * stack only carries small locals, the FreeRTOS frame and ESP_LOG calls.
 * The module-wide single-task guard means exactly ONE stack buffer is ever
 * needed (production has one instance; tests serialize their own).
 *
 * Under the Gate B7 execution flag the same task additionally drives the
 * executor (adapter calls: nvs_config staging, an independent NVS readback
 * handle, controlled stratum task creation), so the static stack grows.
 */
/*
 * Gate B10.1 raises the same bound for a PILOT build: the bounded diagnostics
 * add an snprintf and an extra ESP_LOG frame to this loop, and an unattended
 * hardware pilot is the worst possible place to discover a stack overflow.
 * The cost is paid only by a pilot artifact; the shipped default is unchanged.
 */
#if defined(CONFIG_NX_TIMED_SESSIONS_EXECUTION) || \
    defined(CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS)
#define POOL_RUNTIME_TASK_STACK_BYTES 8192
#else
#define POOL_RUNTIME_TASK_STACK_BYTES 4096
#endif
#define POOL_RUNTIME_TASK_PRIORITY    4
#define POOL_RUNTIME_TASK_NAME        "nx_pool_rt"

/* Bounded tick of the runtime task: no busy loop, and a missed notification
 * can never strand the bounded trusted-time wait. */
#define POOL_RUNTIME_TICK_MS   1000u
#define POOL_RUNTIME_TICK_S    1u
/* Bounded stop handshake (never an unbounded join). */
#define POOL_RUNTIME_STOP_WAIT_MS 2000u

/*
 * Persistence-proposal contract (generation-aware; replaces the earlier
 * blanket one-proposal-per-boot ceiling):
 *
 * The committed B4 counter model derives its proposal from the record's
 * persisted values (reboot_count = increment(record.reboot_count)), so
 * re-evaluating the engine on a freshly persisted record re-proposes the
 * per-PHYSICAL-BOOT increments (reboot; reset-class-driven consecutive) a
 * second time. The corrected rule is NOT "never write again": it is
 *  - the same semantic proposal from the same committed source generation
 *    commits at most once (tracker dedupe);
 *  - the per-boot increments are durably accounted at most once per
 *    physical boot and NORMALIZED out of later raw plans, WITHOUT
 *    discarding the other field changes those plans carry;
 *  - a different later persist-before-action proposal (new state, failure
 *    code, recovery-attempt increment, raised trusted-epoch floor) still
 *    commits, is independently read back and is proven to B5 in the same
 *    physical boot — and no related state advancement happens before that.
 * Writes remain event-driven (boot, TIME_SYNC/MONOTONIC/STORE_RELOAD and
 * the bounded wait expiry) and deduped, so no flash-wear loop is possible.
 */

static const char *TAG = "nx_pool_rt";

/* ------------------------------------------------------------------ */
/* Module-wide guards (single task, coordinator-call depth)            */
/* ------------------------------------------------------------------ */

static portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_task_count   = 0u;
static uint32_t     s_coord_depth  = 0u;

static StaticTask_t s_task_tcb;
static StackType_t  s_task_stack[POOL_RUNTIME_TASK_STACK_BYTES];

#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
/*
 * Gate B7 executor hook (see the header contract). Written once at boot
 * before the owner task starts stepping; read only by the owner task.
 * s_executor_owns_flow caches the hook's latest answer so the re-evaluation
 * machinery on the SAME task can consult it without re-entering the hook.
 */
static PoolRuntimeExecutorHook s_executor_hook = NULL;
static void                   *s_executor_hook_ctx = NULL;
static bool                    s_executor_owns_flow = false;

void pool_session_runtime_register_executor(PoolRuntimeExecutorHook hook, void *ctx)
{
    s_executor_hook     = hook;
    s_executor_hook_ctx = ctx;
}

static bool runtime_executor_owns_flow(void)
{
    return s_executor_owns_flow;
}

static void runtime_step_executor(void)
{
    if (s_executor_hook != NULL) {
        s_executor_owns_flow = s_executor_hook(s_executor_hook_ctx);
    }
}
#else
static bool runtime_executor_owns_flow(void) { return false; }
static void runtime_step_executor(void) {}
#endif /* CONFIG_NX_TIMED_SESSIONS_EXECUTION */

#ifdef CONFIG_NX_TIMED_SESSIONS_API
/*
 * Gate B8 API command hook (see the header contract). Registered once at
 * boot before the owner task starts stepping; consumed ONLY by that task,
 * so the API command processor is not a second mutating owner. The cached
 * answer tells the re-evaluation machinery on the SAME task that the API
 * owns the flow of a freshly created pre-mutation session.
 */
static PoolRuntimeApiCommandHook s_api_hook = NULL;
static void                     *s_api_hook_ctx = NULL;
static bool                      s_api_owns_flow = false;

void pool_session_runtime_register_api_commands(PoolRuntimeApiCommandHook hook, void *ctx)
{
    s_api_hook     = hook;
    s_api_hook_ctx = ctx;
}

static bool runtime_api_owns_flow(void)
{
    return s_api_owns_flow;
}

static void runtime_step_api_commands(void)
{
    if (s_api_hook != NULL) {
        s_api_owns_flow = s_api_hook(s_api_hook_ctx);
    }
}
#else
static bool runtime_api_owns_flow(void) { return false; }
static void runtime_step_api_commands(void) {}
#endif /* CONFIG_NX_TIMED_SESSIONS_API */

uint32_t pool_session_runtime_task_count(void)
{
    uint32_t n;
    portENTER_CRITICAL(&s_runtime_lock);
    n = s_task_count;
    portEXIT_CRITICAL(&s_runtime_lock);
    return n;
}

uint32_t pool_session_runtime_coordinator_depth(void)
{
    uint32_t n;
    portENTER_CRITICAL(&s_runtime_lock);
    n = s_coord_depth;
    portEXIT_CRITICAL(&s_runtime_lock);
    return n;
}

/*
 * Contract 11: no NVS, SNTP or external callback may run while a B5
 * coordinator call is in progress. The B5 API takes no callbacks at all, so
 * the only way to violate this would be for THIS adapter to interleave a
 * store/SNTP call with a coordinator call. These markers make that testable:
 * the fake store backend and fake SNTP ops assert the depth is 0.
 */
static void coord_enter(void)
{
    portENTER_CRITICAL(&s_runtime_lock);
    s_coord_depth++;
    portEXIT_CRITICAL(&s_runtime_lock);
}

static void coord_exit(void)
{
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_coord_depth > 0u) {
        s_coord_depth--;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
}

/* task_running is written by the runtime task and read by the stopper, so
 * both go through the module lock — never a cached or torn read. */
static void runtime_set_task_running(PoolSessionRuntime *rt, bool running)
{
    portENTER_CRITICAL(&s_runtime_lock);
    rt->task_running = running;
    portEXIT_CRITICAL(&s_runtime_lock);
}

static bool runtime_task_is_running(const PoolSessionRuntime *rt)
{
    bool running;
    portENTER_CRITICAL(&s_runtime_lock);
    running = rt->task_running;
    portEXIT_CRITICAL(&s_runtime_lock);
    return running;
}

/* ------------------------------------------------------------------ */
/* Production platform ops                                             */
/* ------------------------------------------------------------------ */

static int32_t prod_read_reset_reason_raw(void)
{
    /* The ONLY esp_reset_reason() call in the timed-session runtime. The raw
     * value is classified immediately by the caller and never stored,
     * published or logged. */
    return (int32_t)esp_reset_reason();
}

static uint64_t prod_monotonic_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS
/*
 * Gate B10.1 — the bounded pilot link fact. It answers exactly one question:
 * "is the station currently associated?" The AP record is read into a local,
 * used ONLY for the return code and zeroed immediately, so no SSID, BSSID,
 * RSSI, channel or address is retained, published or logged. Non-blocking,
 * never called from an interrupt, never called while a lock is held, and it
 * feeds no decision anywhere in the runtime.
 */
static bool prod_link_up(void)
{
    wifi_ap_record_t ap;
    esp_err_t        err;

    memset(&ap, 0, sizeof(ap));
    err = esp_wifi_sta_get_ap_info(&ap);
    memset(&ap, 0, sizeof(ap));
    return err == ESP_OK;
}
#endif /* CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS */

void pool_session_runtime_production_deps(PoolSessionRuntimeDeps *out,
                                          PoolStoreNvsBackend *backend)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->store_ops             = pool_store_nvs_ops();
    out->store_ctx             = backend;
    out->sntp_ops              = pool_time_sntp_real_ops();
    out->read_reset_reason_raw = prod_read_reset_reason_raw;
    out->monotonic_us          = prod_monotonic_us;
    /*
     * NO trusted-time source is hardcoded here. CONFIG_NX_TIMED_SESSIONS_NTP_SERVER
     * defaults to the empty string: until the product explicitly configures a
     * source, the runtime reports an unconfigured time source and HOLDS
     * protocol startup rather than inventing trust from a public server.
     */
#ifdef CONFIG_NX_TIMED_SESSIONS_NTP_SERVER
    out->ntp_server = CONFIG_NX_TIMED_SESSIONS_NTP_SERVER;
#else
    out->ntp_server = "";
#endif
    out->sync_wait_limit_s = POOL_TIME_SYNC_WAIT_DEFAULT_S;
    /*
     * Gate B10 observation-only mode. A FOURTH independent safety flag,
     * default n, that authorizes nothing: see the header contract.
     */
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    out->observe_enabled = true;
#else
    out->observe_enabled = false;
#endif
    /*
     * Gate B10.1 pilot link fact — bound ONLY under the pilot-diagnostics
     * flag. The shipped default leaves it NULL, so no Wi-Fi call exists at
     * all and the pilot link events are simply never produced.
     */
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS
    out->link_up = prod_link_up;
#else
    out->link_up = NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static uint64_t runtime_monotonic_us(const PoolSessionRuntime *rt)
{
    if (rt == NULL || rt->deps.monotonic_us == NULL) {
        return 0u;
    }
    return rt->deps.monotonic_us();
}

/* Boot clock: monotonic always comes from the injected platform clock; the
 * anchor is only ever the provider-owned accepted anchor (never fabricated,
 * and absent entirely until the provider is initialized). */
static uint64_t runtime_clock_monotonic_us(void *ctx)
{
    return runtime_monotonic_us((const PoolSessionRuntime *)ctx);
}

static bool runtime_clock_read_anchor(void *ctx, PoolTimeAnchor *out)
{
    PoolSessionRuntime *rt = (PoolSessionRuntime *)ctx;
    PoolTimeClock       provider_clock;

    if (rt == NULL || out == NULL || !rt->time_provider_initialized) {
        return false; /* no anchor state exists: untrusted, never guessed */
    }
    pool_time_sntp_get_clock(&rt->time_provider, &provider_clock);
    if (provider_clock.ops == NULL || provider_clock.ops->read_anchor == NULL) {
        return false;
    }
    return provider_clock.ops->read_anchor(provider_clock.ctx, out);
}

static const PoolTimeClockOps s_runtime_clock_ops = {
    .monotonic_us = runtime_clock_monotonic_us,
    .read_anchor  = runtime_clock_read_anchor,
};

static uint32_t runtime_sync_wait_limit(const PoolSessionRuntime *rt)
{
    uint32_t limit = rt->deps.sync_wait_limit_s;
    if (limit == 0u) {
        limit = POOL_TIME_SYNC_WAIT_DEFAULT_S;
    }
    if (limit > POOL_TIME_SYNC_WAIT_MAX_S) {
        limit = POOL_TIME_SYNC_WAIT_MAX_S;
    }
    return limit;
}

/* A source STRING was configured (before any validation). */
static bool runtime_time_source_present(const PoolSessionRuntime *rt)
{
    return rt->deps.sntp_ops != NULL && rt->deps.ntp_server != NULL &&
           rt->deps.ntp_server[0] != '\0';
}

/*
 * Gate B10: a source string exists AND passes the bounded pure validator.
 * An invalid source is treated exactly like an absent one — no DNS lookup
 * and no SNTP start can be reached from here.
 */
static bool runtime_time_source_configured(const PoolSessionRuntime *rt)
{
    return runtime_time_source_present(rt) && rt->time_source_validation.usable;
}

/* Refresh the B2 snapshot from the (possibly uninitialized) provider under
 * the SAME floor the B4 engine enforces. Never fabricates trust. */
static void runtime_refresh_time_snapshot(PoolSessionRuntime *rt)
{
    (void)pool_time_snapshot(&rt->clock, &rt->time_policy, &rt->time_snapshot);
}

/* Build the immutable B4 boot context from this boot's facts. */
static void runtime_build_boot_context(PoolSessionRuntime *rt)
{
    memset(&rt->boot_ctx, 0, sizeof(rt->boot_ctx));
    rt->boot_ctx.store_result   = rt->store_result;
    rt->boot_ctx.record_present = rt->record_present;
    rt->boot_ctx.record         = rt->record_present ? &rt->record : NULL;
    rt->boot_ctx.reset_class    = rt->reset_class;
    rt->boot_ctx.sync_wait_elapsed_s = rt->control.wait_elapsed_s;
    rt->boot_ctx.sync_wait_limit_s   = rt->control.wait_limit_s;
    rt->boot_ctx.time_provider_initialized = rt->time_provider_initialized;
    rt->boot_ctx.time_snapshot             = rt->time_snapshot;
    /*
     * Gate B6 CAN guarantee target-mining inhibition while waiting for
     * trusted time, and does so in the strongest possible way: the protocol
     * barrier withholds protocol_coordinator startup entirely, so no Stratum
     * task exists and no pool connection of any kind can be made.
     */
    rt->boot_ctx.mining_inhibition_available = true;
}

static void runtime_build_classify_input(const PoolSessionRuntime *rt,
                                         PoolRuntimeClassifyInput *in)
{
    memset(in, 0, sizeof(*in));
    in->store_opened   = rt->store_opened;
    in->store_loaded   = rt->store_loaded;
    in->store_result   = rt->store_result;
    in->record_present = rt->record_present;
    in->record         = rt->record_present ? &rt->record : NULL;
    in->plan           = &rt->plan;

    in->bootstrap_status           = rt->bootstrap_status;
    in->lease_owner                = rt->lease.owner;
    in->lease_phase                = rt->lease.phase;
    in->lease_generation           = rt->lease.lease_generation;
    in->terminal_pending           = rt->lease.terminal_pending;
    in->lease_restore_required     = rt->lease.restore_required;
    in->lease_persistence_required = rt->lease.persistence_required_before_action;

    in->persist_required  = (rt->proposal.kind != RUNTIME_PROPOSAL_NONE);
    in->persist_attempted = rt->persist_attempted;
    in->persist_verified  = rt->persist_verified;
    in->persist_result    = rt->persist_result;
}

/* Take a consistent copy of the B5 state (never a torn read). */
static void runtime_refresh_lease(PoolSessionRuntime *rt)
{
    coord_enter();
    (void)pool_operation_coordinator_snapshot(&rt->coord, &rt->lease);
    coord_exit();
}

/* Defined with the trusted-time provider below; used by runtime_publish. */
static void runtime_refresh_time_diagnostics(PoolSessionRuntime *rt);

/* Classify + publish. Never releases a hold: the barrier answer is derived
 * from the state through the single total rule. */
static void runtime_publish(PoolSessionRuntime *rt)
{
    PoolRuntimeClassifyInput in;

    runtime_build_classify_input(rt, &in);
    (void)pool_runtime_classify(&in, &rt->decision);
    (void)pool_runtime_control_adopt(&rt->control, &rt->decision);
    (void)pool_runtime_snapshot_build(&in, &rt->decision, rt->reset_class,
                                      rt->committed_generation,
                                      rt->control.wait_elapsed_s,
                                      rt->control.wait_limit_s,
                                      rt->tracker.commit_count,
                                      rt->task_running, &rt->snapshot);

    /* Gate B10: refresh the sanitized trusted-time diagnostics alongside the
     * runtime snapshot so both views describe the same evaluation. */
    runtime_refresh_time_diagnostics(rt);

    ESP_LOGI(TAG, "state=%s protocol=%s status=%s decision=%s store=%s phase=%s time=%s",
             pool_runtime_state_str(rt->decision.state),
             pool_runtime_protocol_str(rt->snapshot.protocol),
             pool_runtime_status_str(rt->decision.status),
             pool_boot_decision_str(rt->plan.decision),
             pool_store_result_str(rt->store_result),
             pool_operation_phase_str(rt->lease.phase),
             pool_time_source_state_str(rt->time_diag.state));
}

/* ------------------------------------------------------------------ */
/* Persistence before action (commit + independent read-back proof)    */
/* ------------------------------------------------------------------ */

/*
 * Commit the CURRENT evaluation's NORMALIZED semantic proposal
 * (rt->proposal) through B3 and PROVE it landed by an independent reload,
 * then hand the durable evidence to B5 whenever an owned session-class
 * lease exists (B5 proofs are token-gated by design: an unowned posture —
 * e.g. a retained safe terminal on the FREE phase — has no lease to update,
 * so commit + independent read-back verification is the complete proof
 * there and no B5 call is made). Only after every applicable step succeeds
 * is the tracker marked complete and may the runtime state advance.
 * Never clears a record, never writes a tombstone, never discharges the
 * restore obligation, never lowers the trusted-epoch floor.
 */
static PoolRuntimeStatus runtime_persist_proposal(PoolSessionRuntime *rt)
{
    PoolStoreResult   commit_res;
    PoolStoreResult   reload_res;
    PoolRuntimeStatus proof;
    PoolOperationPersistenceProof ev;
    PoolOperationStatus os;
    PoolSessionRecord   pre_commit;

    if (!rt->record_present) {
        /* A proposal without a committed record is internally inconsistent:
         * hold rather than invent a record. */
        rt->persist_attempted = true;
        rt->persist_verified  = false;
        rt->persist_result    = STORE_STATE_CONFLICT;
        return RUNTIME_ERR_INTERNAL_CONSISTENCY;
    }

    /* Stage: copy the committed record and apply ONLY the normalized
     * semantic proposal. Session identity, obligation and epochs are
     * preserved by construction; the epoch part goes through the B3
     * acceptance helper so the band/floor rules stay authoritative. */
    pre_commit      = rt->record;
    rt->work_record = rt->record;
    if (rt->proposal.state_update) {
        rt->work_record.state             = (PoolSessionState)rt->proposal.proposed_state;
        rt->work_record.last_failure_code = rt->proposal.proposed_failure_code;
    }
    rt->work_record.reboot_count                  = rt->proposal.reboot_count;
    rt->work_record.recovery_attempt_count        = rt->proposal.recovery_attempt_count;
    rt->work_record.consecutive_recovery_failures = rt->proposal.consecutive_recovery_failures;
    rt->work_record.last_reset_class              = rt->proposal.reset_class_for_record;
    if (rt->proposal.raise_epoch_floor &&
        pool_session_record_propose_trusted_epoch(&rt->work_record,
                                                  rt->proposal.proposed_epoch_floor_s)
            != RECORD_OK) {
        rt->persist_attempted = true;
        rt->persist_verified  = false;
        rt->persist_result    = STORE_STATE_CONFLICT;
        return RUNTIME_ERR_INTERNAL_CONSISTENCY;
    }

    rt->persist_attempted = true;
    commit_res = pool_session_store_commit_record(&rt->store, &rt->work_record);
    rt->persist_result = commit_res;
    if (commit_res != STORE_OK) {
        rt->persist_verified = false;
        if (commit_res == STORE_COMMIT_UNCERTAIN) {
            /* Contract 13: create the B5 recovery guard and keep the hold.
             * The guard escalation is unconditional — with no owned lease
             * the coordinator still enters the guard via the uncertain
             * proof path only when a token exists; without one the
             * classifier's STORE_COMMIT_UNCERTAIN rule guards regardless. */
            if (rt->token.valid) {
                coord_enter();
                memset(&ev, 0, sizeof(ev));
                ev.kind         = OP_PROOF_RECOVERY_UPDATE_COMMITTED;
                ev.store_result = STORE_COMMIT_UNCERTAIN;
                ev.session_id   = rt->record.session_id;
                (void)pool_operation_coordinator_apply_persistence_proof(&rt->coord, &rt->token,
                                                                         &ev, &rt->token);
                coord_exit();
                runtime_refresh_lease(rt);
            }
            return RUNTIME_ERR_STORE_UNCERTAIN;
        }
        return RUNTIME_ERR_PERSIST_FAILED;
    }

    /* Independent read-back: reload the committed state and prove it
     * against the NORMALIZED proposal. */
    reload_res = pool_session_store_load(&rt->store, &rt->work_record, &rt->load_info);
    proof = pool_runtime_verify_proposal_readback(&pre_commit, &rt->work_record,
                                                  &rt->proposal, reload_res);
    if (proof != RUNTIME_OK) {
        rt->persist_verified = false;
        rt->persist_result   = reload_res;
        return proof;
    }

    /* Durable: adopt the reloaded record as the committed truth and account
     * the per-physical-boot increments NOW — they are facts about flash and
     * must survive a later proof-step failure. */
    rt->record = rt->work_record;
    (void)pool_session_store_committed_generation(&rt->store, &rt->committed_generation);
    (void)pool_runtime_tracker_record_commit(&rt->tracker, &rt->proposal, &pre_commit,
                                             rt->committed_generation);

    /* Hand the durable evidence to B5 (outside any store operation) when an
     * owned lease exists. A rejected proof keeps the proposal UNPROVEN and
     * the posture pending — never an advancement. */
    if (rt->token.valid) {
        memset(&ev, 0, sizeof(ev));
        ev.kind                        = OP_PROOF_RECOVERY_UPDATE_COMMITTED;
        ev.store_result                = STORE_OK;
        ev.committed_record_generation = rt->record.generation;
        ev.session_id                  = rt->record.session_id;
        ev.persisted_state             = rt->record.state;
        ev.restore_required            = rt->record.restore_required;

        coord_enter();
        os = pool_operation_coordinator_apply_persistence_proof(&rt->coord, &rt->token,
                                                                &ev, &rt->token);
        coord_exit();
        runtime_refresh_lease(rt);
        if (os != OP_OK) {
            rt->persist_verified = false;
            return RUNTIME_ERR_OWNERSHIP_MISMATCH;
        }
        if (rt->tracker.proof_count < UINT32_MAX) {
            rt->tracker.proof_count++;
        }
    }

    rt->persist_verified = true;
    (void)pool_runtime_tracker_record_proven(&rt->tracker, &rt->proposal);
    return RUNTIME_OK;
}

/*
 * Evaluate the persistence demand of the CURRENT plan: build the normalized
 * semantic proposal (per-boot increments already accounted are normalized
 * away; a raised trusted-epoch floor is added when the B2 snapshot proves
 * one), dedupe it against the tracker, and commit + read back + prove any
 * NEW proposal. Deterministic; called from the boot path and from every
 * owner-task re-evaluation.
 */
static PoolRuntimeStatus runtime_evaluate_persistence(PoolSessionRuntime *rt)
{
    bool     epoch_valid;
    uint64_t epoch_s;

    /* The floor candidate exists only when the B2 predicate fully passed. */
    epoch_valid = rt->time_snapshot.trusted && rt->time_snapshot.status == TIME_OK;
    epoch_s     = epoch_valid ? rt->time_snapshot.trusted_epoch_s : 0u;

    (void)pool_runtime_proposal_build(&rt->plan,
                                      rt->record_present ? &rt->record : NULL,
                                      &rt->tracker, rt->committed_generation,
                                      epoch_valid, epoch_s, &rt->proposal);

    if (rt->proposal.kind == RUNTIME_PROPOSAL_NONE) {
        /* Nothing (after normalization) would change the committed record:
         * the plan's demand is already durably satisfied. Not a write. */
        rt->persist_verified = true;
        return RUNTIME_OK;
    }
    if (pool_runtime_proposal_already_proven(&rt->tracker, &rt->proposal)) {
        /* The identical proposal from the same committed source generation
         * is already durable and proven: not an error, NOT a second write. */
        rt->persist_verified = true;
        return RUNTIME_OK;
    }

    /* A NEW distinct proposal: reset the per-evaluation evidence and run
     * the full commit + independent-readback + proof sequence. */
    rt->persist_attempted = false;
    rt->persist_verified  = false;
    return runtime_persist_proposal(rt);
}

/* ------------------------------------------------------------------ */
/* B4 re-evaluation (owner-task duty; idempotent by B4 contract)       */
/* ------------------------------------------------------------------ */

/*
 * Move the B5 lease onto the phase the RE-EVALUATED plan now requires, over
 * the committed B5 transition graph only. Every target below tightens the
 * posture (target verification -> restore -> guard); nothing here frees
 * ownership, releases a protocol hold or authorizes an external action, and
 * a refused transition simply leaves the guard-producing mismatch in place.
 */
static void runtime_reconcile_lease(PoolSessionRuntime *rt)
{
    PoolOperationLeasePhase target;

    if (rt->bootstrap_status != OP_OK) {
        return;
    }
    switch (rt->plan.decision) {
    case POOL_BOOT_DECISION_RESTORE_SOURCE_NOW:
    case POOL_BOOT_DECISION_VERIFY_RESTORE:
        target = OP_PHASE_RESTORING_SOURCE;
        break;
    case POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET:
        target = OP_PHASE_VERIFYING_TARGET;
        break;
    case POOL_BOOT_DECISION_RECOVERY_REQUIRED:
        target = OP_PHASE_RECOVERY_GUARD;
        break;
    default:
        return; /* nothing to reconcile */
    }
    if (rt->lease.phase == target) {
        return; /* idempotent */
    }
    coord_enter();
    (void)pool_operation_coordinator_transition_phase(&rt->coord, &rt->token, target,
                                                      &rt->token);
    coord_exit();
    runtime_refresh_lease(rt);
}

/*
 * Rebuild the boot context from the CURRENT facts and re-run the pure B4
 * engine. Required only when the CONTEXT changes (a trusted-time sync, the
 * bounded wait reaching its bound, or a store reload) — never merely because
 * a proposal was persisted.
 *
 * PERSISTENCE ORDER (the corrected contract): the re-derived plan's
 * NORMALIZED semantic proposal — the per-physical-boot increments already
 * durably accounted are normalized away; every OTHER change (new state,
 * failure code, recovery-attempt increment, raised trusted-epoch floor) is
 * preserved — is committed through B3, independently reloaded, readback-
 * verified and proven to B5 BEFORE any lease reconciliation or state
 * advancement. A failed/uncertain/unproven proposal leaves the posture
 * pending and reconciles NOTHING. This never releases a protocol hold,
 * never frees ownership and never executes a pool or Stratum action.
 */
static void runtime_reevaluate(PoolSessionRuntime *rt)
{
    PoolRuntimeStatus ps;

    runtime_refresh_time_snapshot(rt);
    runtime_build_boot_context(rt);
    (void)pool_session_recovery_plan(&rt->boot_ctx, &rt->plan);

    /*
     * Gate B7: while the executor OWNS the session flow it is the ONE
     * mutating owner — the freshly rebuilt plan above is its INPUT, and the
     * B6 plan-persistence + lease-reconcile machinery must not compete with
     * its action-boundary commits. Without the execution flag this branch
     * compiles to the committed B6 behavior unchanged.
     */
    /*
     * Gate B8 extends the SAME rule to the API command processor: it reports
     * the flow as owned while a command transaction is in progress AND while
     * the executor it handed the session to still owns it, so there is never
     * a window in which the BOOT-time B4 table could re-plan over an in-boot
     * creation. The plan itself is still rebuilt above, because the executor
     * consumes it as INPUT. Without the API flag this is the committed B7
     * condition unchanged.
     */
    if (!runtime_executor_owns_flow() && !runtime_api_owns_flow()) {
        ps = runtime_evaluate_persistence(rt);
        if (ps == RUNTIME_OK && rt->persist_verified) {
            /* Only a durably satisfied evaluation may tighten the owned phase. */
            runtime_reconcile_lease(rt);
        }
    }
    runtime_refresh_lease(rt);
    runtime_publish(rt);
}

/* ------------------------------------------------------------------ */
/* Trusted-time provider (started only when recovery requires it)      */
/* ------------------------------------------------------------------ */

/*
 * Gate B10 — the bounded post-synchronization observer.
 *
 * Runs in the lwIP tcpip thread from the ESP-IDF SNTP sync callback, with NO
 * provider or coordinator lock held. It does exactly two things: record the
 * bounded verdict for diagnostics, and post ONE task notification to the
 * single runtime owner task so the B4/B6 re-evaluation happens THERE.
 *
 * It performs no NVS access, no B5 transition, no pool or protocol
 * operation, no restart, no HTTP operation, no allocation and no blocking,
 * and it is not given (so cannot log) the server name, the epoch or the sync
 * generation. Duplicate callbacks are naturally idempotent: the notification
 * is an OR of event bits, and B2 has already made an unchanged anchor a
 * no-op before the observer is reached.
 */
static void runtime_sync_observer(void *ctx, PoolTimeError verdict)
{
    PoolSessionRuntime *rt = (PoolSessionRuntime *)ctx;

    if (rt == NULL) {
        return;
    }
    /* Contract 10: a callback must never run while a B5 coordinator call is
     * in progress. Asserted here so a violation is caught deterministically
     * rather than reasoned about. */
    assert(pool_session_runtime_coordinator_depth() == 0u);

    portENTER_CRITICAL(&s_runtime_lock);
    rt->time_last_sync_result = verdict;
    if (rt->time_sync_callbacks < UINT32_MAX) {
        rt->time_sync_callbacks++;
    }
    portEXIT_CRITICAL(&s_runtime_lock);

    (void)pool_session_runtime_notify_from_callback(rt, RUNTIME_EVENT_TIME_SYNC_CHANGED);
}

/* Assemble the pure B10 start input from this boot's facts. */
static void runtime_build_time_start_input(const PoolSessionRuntime *rt,
                                           PoolTimeSourceStartInput *in)
{
    memset(in, 0, sizeof(*in));
    /* This adapter exists only inside the timed-session runtime, so the
     * runtime feature is enabled by construction wherever it runs. */
    in->runtime_enabled       = true;
    in->observe_enabled       = rt->deps.observe_enabled;
    in->network_ready         = rt->control.network_ready;
    in->trusted_time_required = rt->plan.trusted_time_required;
    in->source_present        = runtime_time_source_present(rt);
    in->source_usable         = runtime_time_source_configured(rt);
    in->provider_started      = rt->time_provider_started;
    /* Observation is only for a device with no session owner at all: any
     * committed record, or any plan that needs trusted time, disqualifies it. */
    in->session_owner_present = rt->record_present || rt->plan.trusted_time_required ||
                                rt->plan.restore_required;
}

/*
 * Start the B2 SNTP provider under the pure Gate B10 rules.
 *
 * The provider starts ONLY when a VALIDATED product-configured source
 * exists, the network is ready, no provider is running yet, and either the
 * B4 recovery plan requires trusted time or observation-only mode is enabled
 * on a device with no session owner.
 *
 * With no configured (or an invalid) source the runtime reports
 * RUNTIME_ERR_TIME_SOURCE_UNCONFIGURED and keeps the protocol HELD whenever
 * recovery needed the trust — it never contacts a public NTP server, never
 * performs DNS and never fabricates trust. In observation mode an
 * unconfigured source is simply reported and changes nothing.
 *
 * Start attempts are HARD BOUNDED (POOL_TIME_SOURCE_ATTEMPTS_MAX) with a
 * monotonic backoff, so no unlimited retry or tight poll can exist. This is
 * never called from an HTTP handler, a timer callback or the sync callback:
 * only the single runtime owner task reaches it.
 *
 * HONEST LIMIT: a platform START failure drives the B2 provider into its
 * ERROR lifecycle, which by the committed B2 contract only a deinit clears.
 * Later attempts therefore cannot revive it; they simply consume the bounded
 * budget and the diagnostics report TIME_SOURCE_ERROR. Deliberately no
 * automatic deinit/reinit cycle is performed here — tearing the service down
 * and rebuilding it from a bounded retry path is exactly the kind of
 * unattended recovery this gate is not authorized to introduce.
 */
static PoolRuntimeStatus runtime_start_time_provider(PoolSessionRuntime *rt)
{
    PoolTimeSourceStartInput    start_in;
    PoolTimeSourceStartDecision decision;
    PoolTimeSourceRetryDecision retry;
    PoolTimeSntpConfig          cfg;
    PoolTimeError               err;
    uint64_t                    now;

    runtime_build_time_start_input(rt, &start_in);
    decision = pool_time_source_decide_start(&start_in);

    if (rt->time_provider_started) {
        /* A session that appears later (Gate B8 create) ends observation
         * without restarting anything: the same single provider continues. */
        rt->time_observation_active = decision.observation_only;
    }

    if (!decision.start_provider) {
        switch (decision.state) {
        case TIME_SOURCE_UNCONFIGURED:
        case TIME_SOURCE_INVALID:
            /* Only a posture that actually NEEDS trusted time reports this as
             * a failure; an observation-only device is simply left alone. */
            return start_in.trusted_time_required ? RUNTIME_ERR_TIME_SOURCE_UNCONFIGURED
                                                  : RUNTIME_OK;
        case TIME_SOURCE_START_PENDING:
            return RUNTIME_ERR_TIME_WAIT_PENDING; /* network not ready yet */
        default:
            return RUNTIME_OK; /* validated but idle, or already running */
        }
    }

    /* Bounded start budget with monotonic backoff. */
    now   = runtime_monotonic_us(rt);
    retry = pool_time_source_retry_decide(&rt->time_retry_policy, rt->time_start_attempts,
                                          rt->time_attempt_made, rt->time_last_attempt_us,
                                          now);
    if (retry.exhausted) {
        if (!rt->time_attempts_exhausted) {
            rt->time_attempts_exhausted = true;
            ESP_LOGW(TAG, "trusted-time start budget exhausted after %u attempts",
                     (unsigned)rt->time_start_attempts);
        }
        return RUNTIME_ERR_TIME_WAIT_EXPIRED;
    }
    if (!retry.may_attempt) {
        return RUNTIME_ERR_TIME_WAIT_PENDING; /* backoff still running */
    }

    rt->time_start_attempts  = retry.attempt_index;
    rt->time_attempt_made    = true;
    rt->time_last_attempt_us = now;

    if (!rt->time_provider_initialized) {
        pool_time_sntp_config_defaults(&cfg);
        cfg.server_count = 1u; /* EXACTLY one server; never a fallback pool */
        strncpy(cfg.servers[0], rt->deps.ntp_server, sizeof(cfg.servers[0]) - 1u);
        cfg.servers[0][sizeof(cfg.servers[0]) - 1u] = '\0';
        cfg.sync_wait_s = rt->control.wait_limit_s;

        err = pool_time_sntp_init(&rt->time_provider, rt->deps.sntp_ops, &cfg, &rt->time_policy);
        if (err != TIME_OK) {
            /* The machine token never carries the hostname. */
            ESP_LOGW(TAG, "time provider init rejected (%s)", pool_time_error_str(err));
            return RUNTIME_ERR_TIME_SOURCE_UNCONFIGURED;
        }
        rt->time_provider_initialized = true;

        /* Register the callback BEFORE starting: initialization performs no
         * networking, so no sync can fire in this window. */
        (void)pool_time_sntp_set_observer(&rt->time_provider, runtime_sync_observer, rt);
    }

    err = pool_time_sntp_start(&rt->time_provider);
    if (err != TIME_OK) {
        ESP_LOGW(TAG, "time provider start rejected (%s)", pool_time_error_str(err));
        return RUNTIME_ERR_TIME_SOURCE_UNCONFIGURED;
    }
    rt->time_provider_started   = true;
    rt->time_observation_active = decision.observation_only;
    ESP_LOGI(TAG, "trusted-time provider started (%s, attempt %u, bounded wait %us)",
             decision.observation_only ? "observation-only" : "session recovery",
             (unsigned)rt->time_start_attempts, (unsigned)rt->control.wait_limit_s);
    return RUNTIME_OK;
}

/*
 * Rebuild and publish the sanitized trusted-time diagnostics. Reads only
 * already-collected bounded facts; performs no platform call, no networking
 * and no store access, and copies no string.
 */
static void runtime_refresh_time_diagnostics(PoolSessionRuntime *rt)
{
    PoolTimeSourceDiagnosticsInput in;
    PoolTimeSourceDiagnostics      built;

    memset(&in, 0, sizeof(in));
    in.runtime_enabled      = true;
    in.observe_enabled      = rt->deps.observe_enabled;
    in.source_present       = runtime_time_source_present(rt);
    in.source_usable        = runtime_time_source_configured(rt);
    in.provider_initialized = rt->time_provider_initialized;
    in.provider_started     = rt->time_provider_started;
    in.lifecycle            = (uint8_t)pool_time_sntp_lifecycle(
        rt->time_provider_initialized ? &rt->time_provider : NULL);
    in.snapshot_trusted   = rt->time_snapshot.trusted;
    in.anchor_age_us      = rt->time_snapshot.anchor_age_us;
    in.anchor_age_valid   = rt->time_snapshot.trusted;
    in.attempts           = rt->time_start_attempts;
    in.attempts_exhausted = rt->time_attempts_exhausted;
    in.wait_limit_s       = rt->control.wait_limit_s;
    /*
     * The B4 bounded recovery wait only advances in WAITING_FOR_TRUSTED_TIME.
     * Observation runs in a FREE posture, so it carries its OWN bounded
     * window — a purely diagnostic one that never drives a plan, a restore or
     * a protocol change.
     */
    in.wait_elapsed_s = rt->time_observation_active ? rt->time_observe_elapsed_s
                                                    : rt->control.wait_elapsed_s;
    in.wait_expired   = pool_runtime_wait_expired(in.wait_elapsed_s, in.wait_limit_s);

    portENTER_CRITICAL(&s_runtime_lock);
    in.last_sync_result = rt->time_last_sync_result;
    portEXIT_CRITICAL(&s_runtime_lock);

    pool_time_source_diagnostics_build(&in, &built);

    portENTER_CRITICAL(&s_runtime_lock);
    rt->time_diag = built; /* published atomically for readers */
    portEXIT_CRITICAL(&s_runtime_lock);
}

/*
 * Gate B10 — the bounded observation-only tick.
 *
 * Advances the observation window, re-reads the B2 anchor and republishes
 * the diagnostics. That is ALL it does. It never re-plans, never evaluates
 * persistence, never writes the session store, never acquires or reconciles
 * a lease, never touches the pool configuration, the protocol, the ASIC gate
 * or the mining grant, and never restarts the device. Reaching the bound
 * changes only what is REPORTED — normal source mining continues untouched.
 */
static void runtime_step_observation(PoolSessionRuntime *rt)
{
    /*
     * Observation ends the moment a session owner appears — including one
     * created later in the SAME boot through the Gate B8 API. From then on
     * the B4 plan, not observation, decides what the time provider is for.
     */
    if (!rt->time_observation_active || rt->plan.trusted_time_required ||
        rt->record_present) {
        return;
    }
    if (rt->time_observe_elapsed_s < rt->control.wait_limit_s) {
        rt->time_observe_elapsed_s += POOL_RUNTIME_TICK_S;
        if (rt->time_observe_elapsed_s > rt->control.wait_limit_s) {
            rt->time_observe_elapsed_s = rt->control.wait_limit_s;
        }
    }
    runtime_refresh_time_snapshot(rt);
    runtime_refresh_time_diagnostics(rt);
}

/* ------------------------------------------------------------------ */
/* Gate B10.1 — bounded observation-pilot diagnostics                  */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS

/*
 * RAM-only, owned exclusively by the single B6 owner task. The line buffer
 * lives here rather than on the task stack, matching the module rule that no
 * large working struct is ever a task local.
 */
static PoolPilotState s_pilot;
static char           s_pilot_line[POOL_PILOT_SUMMARY_MAX];
static uint32_t       s_pilot_heartbeat_writes;

/*
 * The bounded pilot tick. STRICTLY read-only: it copies the already-published
 * sanitized B6 snapshot and B10 diagnostics, reads bounded platform counters,
 * asks the pure step which bounded lines are due, and logs them.
 *
 * It never writes the store, never touches a lease, never re-plans, never
 * changes the pool configuration, the protocol permission, the ASIC gate, the
 * mining grant, the frequency, the voltage or the fan configuration, never
 * writes NVS and never restarts the device. An invariant violation is
 * REPORTED and nothing more.
 *
 * LOCKING: every accessor below takes and releases the runtime lock inside
 * itself, so no lock is held across a platform call or an ESP_LOG call.
 */
static void runtime_step_pilot(PoolSessionRuntime *rt)
{
    PoolRuntimeSnapshot       snap;
    PoolTimeSourceDiagnostics diag;
    PoolPilotInvariantInput   inv_in;
    PoolPilotInvariantReport  inv;
    PoolPilotObservation      obs;
    PoolPilotStepResult       step;
    PoolPilotSummary          sum;
    uint64_t                  now_us;
    uint32_t                  uptime_s;
    uint32_t                  i;
    bool                      exec_hook;
    bool                      api_hook;

    if (rt == NULL || !rt->initialized || !rt->booted) {
        return;
    }

    (void)pool_session_runtime_snapshot(rt, &snap);
    (void)pool_session_runtime_time_diagnostics(rt, &diag);

    now_us   = runtime_monotonic_us(rt);
    uptime_s = (uint32_t)(now_us / 1000000ull);

#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
    exec_hook = (s_executor_hook != NULL);
#else
    exec_hook = false;
#endif
#ifdef CONFIG_NX_TIMED_SESSIONS_API
    api_hook = (s_api_hook != NULL);
#else
    api_hook = false;
#endif

    memset(&inv_in, 0, sizeof(inv_in));
    inv_in.snapshot_model_version      = snap.model_version;
    inv_in.snapshot_structurally_valid = pool_runtime_snapshot_valid(&snap);
    inv_in.store_result                = snap.store_result;
    inv_in.session_present             = snap.session_present;
    inv_in.lease_owner                 = snap.lease_owner;
    inv_in.restore_required            = snap.restore_required;
    inv_in.session_write_count         = snap.proposal_commits;
    inv_in.heartbeat_write_count       = s_pilot_heartbeat_writes;
#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
    inv_in.execution_compiled = true;
#endif
#ifdef CONFIG_NX_TIMED_SESSIONS_API
    inv_in.api_compiled = true;
#endif
    inv_in.execution_hook_registered = exec_hook;
    inv_in.api_hook_registered       = api_hook;
    inv_in.target_mining_authorized  = snap.target_mining_authorized;
    inv_in.pool_mutation_permitted   = snap.pool_mutation_permitted;
    inv_in.protocol                  = snap.protocol;
    inv_in.runtime_state             = snap.state;
    inv_in.time_state                = diag.state;

    pool_pilot_invariants_check(&inv_in, &inv);

    memset(&obs, 0, sizeof(obs));
    obs.network_ready  = rt->control.network_ready;
    obs.link_known     = (rt->deps.link_up != NULL);
    obs.link_up        = obs.link_known ? rt->deps.link_up() : false;
    obs.source_state   = diag.state;
    obs.attempt_count  = diag.sync_attempt_count;
    obs.monotonic_us   = now_us;
    obs.invariant_mask = inv.mask;

    pool_pilot_step(&s_pilot, &obs, &step);

    for (i = 0u; i < step.count; i++) {
        if (step.events[i] == POOL_PILOT_EVENT_INVARIANT_VIOLATION) {
            if (pool_pilot_violation_format(s_pilot.sequence, uptime_s, &inv,
                                            s_pilot_line, sizeof(s_pilot_line)) > 0u) {
                ESP_LOGW(TAG, "%s", s_pilot_line);
            }
            continue;
        }
        if (pool_pilot_event_format(step.events[i], uptime_s, s_pilot_line,
                                    sizeof(s_pilot_line)) > 0u) {
            ESP_LOGI(TAG, "%s", s_pilot_line);
        }
    }

    if (!step.summary_due) {
        return;
    }

    memset(&sum, 0, sizeof(sum));
    sum.sequence                 = step.sequence;
    sum.uptime_s                 = uptime_s;
    sum.source_state             = diag.state;
    sum.trusted_available        = diag.trusted_time_available;
    sum.trusted_operational      = diag.trusted_time_operational;
    sum.attempt_count            = diag.sync_attempt_count;
    sum.sync_age_valid           = diag.sync_age_valid;
    sum.sync_age_s               = diag.sync_age_s;
    sum.protocol                 = snap.protocol;
    sum.runtime_state            = snap.state;
    sum.lease_owner              = snap.lease_owner;
    sum.restore_required         = snap.restore_required;
    sum.session_write_count      = snap.proposal_commits;
    sum.heartbeat_write_count    = s_pilot_heartbeat_writes;
    sum.execution_reachable      = inv_in.execution_compiled || exec_hook;
    sum.api_reachable            = inv_in.api_compiled || api_hook;
    sum.free_internal_heap_b     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    sum.min_free_internal_heap_b =
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    /* The owner task measures ITSELF, so no foreign handle is dereferenced. */
    sum.owner_task_stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    sum.invariants           = inv;

    if (pool_pilot_summary_format(&sum, s_pilot_line, sizeof(s_pilot_line)) > 0u) {
        ESP_LOGI(TAG, "%s", s_pilot_line);
    }
}

#else /* !CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS */

/* No pilot state, no pilot symbol, no pilot line: the committed B10 build. */
static void runtime_step_pilot(PoolSessionRuntime *rt) { (void)rt; }

#endif /* CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS */
#endif /* CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE */

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

PoolRuntimeStatus pool_session_runtime_init(PoolSessionRuntime *rt,
                                            const PoolSessionRuntimeDeps *deps)
{
    if (rt == NULL || deps == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (rt->initialized) {
        return RUNTIME_ERR_ALREADY_INITIALIZED;
    }
    if (deps->store_ops == NULL || deps->read_reset_reason_raw == NULL ||
        deps->monotonic_us == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }

    memset(rt, 0, sizeof(*rt));
    rt->deps        = *deps;
    rt->clock.ops   = &s_runtime_clock_ops;
    rt->clock.ctx   = rt;
    rt->reset_class = POOL_RESET_CLASS_UNKNOWN;
    rt->store_result = STORE_NOT_INITIALIZED;
    rt->bootstrap_status = OP_ERR_BOOTSTRAP_REQUIRED;

    /*
     * Gate B10: validate the product-configured trusted-time source ONCE, at
     * bind time, with the pure bounded validator. This performs no DNS, no
     * networking and no allocation, and it keeps no copy of the candidate —
     * only its shape. An invalid source is treated exactly like an absent
     * one from here on.
     */
    (void)pool_time_source_validate(rt->deps.ntp_server, &rt->time_source_validation);
    rt->time_source_configured = runtime_time_source_configured(rt);
    rt->time_last_sync_result  = TIME_ERR_NOT_INITIALIZED;
    pool_time_source_retry_defaults(&rt->time_retry_policy);
    pool_time_source_diagnostics_init(&rt->time_diag);

    pool_runtime_control_init(&rt->control, runtime_sync_wait_limit(rt));
    pool_runtime_tracker_init(&rt->tracker);
    pool_runtime_snapshot_init(&rt->snapshot);
    pool_time_trust_policy_defaults(&rt->time_policy);
    pool_operation_state_init(&rt->lease);

    rt->initialized = true;
    return RUNTIME_OK;
}

PoolRuntimeStatus pool_session_runtime_deinit(PoolSessionRuntime *rt)
{
    if (rt == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!rt->initialized) {
        return RUNTIME_OK; /* idempotent */
    }
    (void)pool_session_runtime_stop_task(rt);
    if (rt->time_provider_initialized) {
        /* Clear the observer BEFORE tearing the service down so no in-flight
         * callback can reach a runtime that is being zeroed. Deinit also
         * stops the service and drops the accepted anchor. */
        (void)pool_time_sntp_set_observer(&rt->time_provider, NULL, NULL);
        (void)pool_time_sntp_deinit(&rt->time_provider);
        rt->time_provider_initialized = false;
        rt->time_provider_started     = false;
        rt->time_observation_active   = false;
    }
    if (rt->store_opened) {
        (void)pool_session_store_deinit(&rt->store);
        rt->store_opened = false;
    }
    coord_enter();
    (void)pool_operation_coordinator_deinit(&rt->coord);
    coord_exit();
    memset(rt, 0, sizeof(*rt));
    return RUNTIME_OK;
}

PoolRuntimeStatus pool_session_runtime_boot(PoolSessionRuntime *rt)
{
    PoolStoreResult      sr;
    PoolOperationStatus  os;
    PoolOperationBootstrapInput bi;
    PoolRuntimeStatus    persist_status = RUNTIME_OK;

    if (rt == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!rt->initialized) {
        return RUNTIME_ERR_NOT_INITIALIZED;
    }
    if (rt->booted) {
        return RUNTIME_ERR_ALREADY_INITIALIZED; /* never re-reads anything */
    }

    rt->control.state = RUNTIME_BOOTSTRAPPING;

    /* ---- Step 2: read esp_reset_reason() EXACTLY ONCE, then classify. ---- */
    if (!rt->reset_reason_read) {
        int32_t raw = rt->deps.read_reset_reason_raw();
        rt->reset_reason_read = true;
        rt->reset_reason_read_count++;
        rt->reset_class = pool_session_reset_classify_raw(raw);
        /* The raw value is now out of scope: never stored, published or logged. */
    }

    /* ---- Step 3: open the real B3 store (NVS is already initialized). ---- */
    sr = pool_session_store_init(&rt->store, rt->deps.store_ops, rt->deps.store_ctx);
    rt->store_opened = (sr == STORE_OK);

    /* ---- Step 4: load the committed state. A failed open or load is NEVER
     *              interpreted as STORE_EMPTY (contract 7). ---- */
    if (rt->store_opened) {
        rt->store_result = pool_session_store_load(&rt->store, &rt->record, &rt->load_info);
        rt->store_loaded = true;
        rt->record_present = (rt->store_result == STORE_OK);
        if (!rt->record_present) {
            memset(&rt->record, 0, sizeof(rt->record));
        }
        (void)pool_session_store_committed_generation(&rt->store, &rt->committed_generation);
    } else {
        rt->store_result   = STORE_NOT_INITIALIZED;
        rt->store_loaded   = false;
        rt->record_present = false;
        memset(&rt->record, 0, sizeof(rt->record));
    }

    /* ---- Step 5: the B4 floor policy, then the initial UNTRUSTED snapshot. ---- */
    pool_session_recovery_build_time_policy(rt->record_present ? &rt->record : NULL,
                                            &rt->time_policy);
    runtime_refresh_time_snapshot(rt);

    /* ---- Steps 6/7: boot context, then the pure B4 recovery plan. ---- */
    runtime_build_boot_context(rt);
    (void)pool_session_recovery_plan(&rt->boot_ctx, &rt->plan);

    /* ---- Step 8: bootstrap EXACTLY ONE B5 coordinator. ---- */
    coord_enter();
    (void)pool_operation_coordinator_init(&rt->coord);
    coord_exit();

    memset(&bi, 0, sizeof(bi));
    bi.store_result         = rt->store_result;
    bi.record_present       = rt->record_present;
    bi.record               = rt->record_present ? &rt->record : NULL;
    bi.plan                 = &rt->plan;
    bi.committed_generation = rt->committed_generation;

    coord_enter();
    os = pool_operation_coordinator_bootstrap(&rt->coord, &bi, &rt->token);
    coord_exit();
    rt->bootstrap_status = os;
    runtime_refresh_lease(rt);

    /* ---- Step 9: normalize, commit, independently read back and prove the
     *      mandatory proposal (generation-aware persistence tracking). ---- */
    if (os == OP_OK) {
        persist_status = runtime_evaluate_persistence(rt);
        (void)persist_status; /* the posture is decided by classification */
    }

    /*
     * ---- Step 10: boot itself never re-plans. The plan the B5 lease was
     * bootstrapped from stays authoritative for this boot's decision; the
     * B4 §4.1 order is plan -> persist -> act. Re-evaluation happens only
     * in the owner task when the CONTEXT changes (time sync, monotonic
     * boundary, store reload, bounded-wait expiry) — and, under the
     * corrected contract, each such re-evaluation commits, reads back and
     * proves its own NORMALIZED proposal before any state advancement,
     * with the per-physical-boot increments accounted at most once.
     */

    /* ---- Steps 11/12: classify and publish the sanitized snapshot. ---- */
    runtime_refresh_lease(rt);
    runtime_publish(rt);
    rt->booted = true;

    return rt->decision.status;
}

PoolRuntimeStatus pool_session_runtime_commit_epoch_heartbeat(PoolSessionRuntime *rt,
                                                              uint64_t epoch_s)
{
    PoolSessionRecoveryPlan neutral;

    if (rt == NULL || !rt->initialized || !rt->booted) {
        return RUNTIME_ERR_NOT_INITIALIZED;
    }
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS
    /* Gate B10.1 audit counter ONLY: it is incremented on ENTRY so even a
     * refused heartbeat is counted, and it changes no behaviour whatsoever.
     * An observation pilot must be able to prove this call had no caller. */
    if (s_pilot_heartbeat_writes < UINT32_MAX) {
        s_pilot_heartbeat_writes++;
    }
#endif
    if (!rt->record_present || rt->store_result != STORE_OK ||
        rt->record.kind != (uint8_t)POOL_RECORD_KIND_SESSION) {
        return RUNTIME_ERR_INTERNAL_CONSISTENCY;
    }

    /*
     * A NEUTRAL plan: every counter equals the CURRENTLY COMMITTED value and
     * no record proposal is requested, so pool_runtime_proposal_build()
     * derives an EPOCH-FLOOR-ONLY proposal. The heartbeat therefore touches
     * exactly one persisted field — the monotonically-advancing trusted
     * epoch floor — and structurally cannot change the state, the failure
     * code, any counter, the duration or ANY deadline field.
     */
    memset(&neutral, 0, sizeof(neutral));
    neutral.counters.reboot_count                  = rt->record.reboot_count;
    neutral.counters.recovery_attempt_count        = rt->record.recovery_attempt_count;
    neutral.counters.consecutive_recovery_failures = rt->record.consecutive_recovery_failures;
    neutral.counters.reset_class_for_record        = rt->record.last_reset_class;

    (void)pool_runtime_proposal_build(&neutral, &rt->record, &rt->tracker,
                                      rt->committed_generation, true, epoch_s,
                                      &rt->proposal);
    if (rt->proposal.kind == RUNTIME_PROPOSAL_NONE) {
        /* Nothing would change in flash: not an error and NOT a write. */
        return RUNTIME_ERR_PERSIST_DUPLICATE;
    }
    if (rt->proposal.kind != RUNTIME_PROPOSAL_EPOCH_FLOOR) {
        /* Defensive: a heartbeat may never carry a plan part. */
        return RUNTIME_ERR_INTERNAL_CONSISTENCY;
    }
    if (pool_runtime_proposal_already_proven(&rt->tracker, &rt->proposal)) {
        return RUNTIME_ERR_PERSIST_DUPLICATE;
    }

    /* The committed generation-aware engine: commit -> independent reload ->
     * exact readback verification -> B5 proof. */
    rt->persist_attempted = false;
    rt->persist_verified  = false;
    return runtime_persist_proposal(rt);
}

PoolRuntimeProtocolPermission pool_session_runtime_protocol_permission(
    const PoolSessionRuntime *rt)
{
    if (rt == NULL || !rt->initialized || !rt->booted) {
        return POOL_RUNTIME_PROTOCOL_HOLD; /* fail closed */
    }
    /* Derived from the state through the single total rule — never cached
     * and never widened. */
    return pool_runtime_protocol_for_state(rt->decision.state);
}

PoolRuntimeStatus pool_session_runtime_snapshot(const PoolSessionRuntime *rt,
                                                PoolRuntimeSnapshot *out)
{
    if (rt == NULL || out == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_runtime_lock);
    *out = rt->snapshot; /* consistent copy, never torn */
    portEXIT_CRITICAL(&s_runtime_lock);
    return RUNTIME_OK;
}

uint32_t pool_session_runtime_reset_reason_reads(const PoolSessionRuntime *rt)
{
    return (rt == NULL) ? 0u : rt->reset_reason_read_count;
}

/* ------------------------------------------------------------------ */
/* Gate B10 — sanitized trusted-time diagnostics                       */
/* ------------------------------------------------------------------ */

PoolRuntimeStatus pool_session_runtime_time_diagnostics(const PoolSessionRuntime *rt,
                                                        PoolTimeSourceDiagnostics *out)
{
    if (out == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    pool_time_source_diagnostics_init(out); /* fail-closed on every path */
    if (rt == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_runtime_lock);
    *out = rt->time_diag; /* consistent copy, never torn */
    portEXIT_CRITICAL(&s_runtime_lock);
    return RUNTIME_OK;
}

uint32_t pool_session_runtime_time_start_attempts(const PoolSessionRuntime *rt)
{
    return (rt == NULL) ? 0u : rt->time_start_attempts;
}

uint32_t pool_session_runtime_time_sync_callbacks(const PoolSessionRuntime *rt)
{
    uint32_t n;
    if (rt == NULL) {
        return 0u;
    }
    portENTER_CRITICAL(&s_runtime_lock);
    n = rt->time_sync_callbacks;
    portEXIT_CRITICAL(&s_runtime_lock);
    return n;
}

bool pool_session_runtime_time_observation_active(const PoolSessionRuntime *rt)
{
    return (rt == NULL) ? false : rt->time_observation_active;
}

/* ------------------------------------------------------------------ */
/* The single bounded runtime owner task                               */
/* ------------------------------------------------------------------ */

static void runtime_task(void *arg)
{
    PoolSessionRuntime *rt = (PoolSessionRuntime *)arg;
    uint32_t            bits;
    PoolRuntimeEventOutcome outcome;

    ESP_LOGI(TAG, "runtime owner task started");

    for (;;) {
        bits = 0u;
        /* Bounded wait: no busy loop, and a lost notification can never
         * strand the bounded trusted-time wait (the tick still advances it). */
        (void)xTaskNotifyWait(0u, UINT32_MAX, &bits, pdMS_TO_TICKS(POOL_RUNTIME_TICK_MS));

        (void)pool_runtime_control_apply(&rt->control, bits, &outcome);

        if (outcome.stop_task) {
            break;
        }

        /*
         * Gate B8: drain at most one bounded API command FIRST, so the flow
         * ownership answer below is fresh on every path (including the
         * store-reload branch that continues the loop). A no-op without the
         * API flag or a registered hook. This is the ONLY consumer of the
         * command mailbox: no second mutating task exists.
         */
        runtime_step_api_commands();

        if (outcome.start_time_provider) {
            (void)runtime_start_time_provider(rt);
        }
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
        else if ((outcome.applied_events & RUNTIME_EVENT_NETWORK_READY) != 0u) {
            /*
             * Gate B10 observation-only path. The pure B6 control block asks
             * for a provider start only while WAITING_FOR_TRUSTED_TIME; an
             * observation device is FREE, so the adapter offers the same
             * network-ready fact to the pure B10 start rule, which permits it
             * ONLY when no session owner exists. This branch is compiled out
             * entirely when the observation flag is off, so the committed
             * build is byte-for-byte unchanged.
             */
            (void)runtime_start_time_provider(rt);
            runtime_refresh_time_diagnostics(rt);
        }
#endif
        if (outcome.reload_store) {
            /* B6 reloads only — it never mutates on a reload request. */
            PoolStoreResult r = pool_session_store_load(&rt->store, &rt->work_record,
                                                        &rt->load_info);
            if (r == STORE_OK) {
                rt->record         = rt->work_record;
                rt->record_present = true;
                rt->store_result   = r;
                (void)pool_session_store_committed_generation(&rt->store,
                                                              &rt->committed_generation);
            } else if (r != STORE_EMPTY && r != STORE_CLEARED) {
                /* Any non-OK reload keeps the existing evidence and holds. */
                rt->store_result = r;
            } else {
                rt->store_result   = r;
                rt->record_present = false;
            }
            runtime_reevaluate(rt);
            continue;
        }

        if (rt->control.state == RUNTIME_WAITING_FOR_TRUSTED_TIME) {
            /* Advance the BOUNDED wait, then re-evaluate. Reaching the bound
             * makes B4 fail safe toward restore — it never releases the
             * protocol hold and never frees ownership. */
            bool expired = pool_runtime_control_advance_wait(&rt->control, POOL_RUNTIME_TICK_S);
            if (outcome.reevaluate_plan || expired) {
                runtime_reevaluate(rt);
            }
            if (!rt->time_provider_started) {
                (void)runtime_start_time_provider(rt);
            }
        } else if (outcome.reevaluate_plan) {
            runtime_reevaluate(rt);
        }

#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
        /* Bounded observation tick — measurement only, never authorization. */
        runtime_step_observation(rt);
        /* Gate B10.1 — bounded, read-only pilot diagnostics on the SAME owner
         * task. A no-op unless the pilot-diagnostics flag is enabled, and even
         * then it only reads published facts and writes bounded log lines. */
        runtime_step_pilot(rt);
#endif

        /* Gate B7: step the executor LAST so it always consumes a fresh
         * plan. A no-op without the execution flag / a registered hook. */
        runtime_step_executor();
    }

    ESP_LOGI(TAG, "runtime owner task stopping (state=%s)",
             pool_runtime_state_str(rt->control.state));

    rt->task_handle = NULL;
    portENTER_CRITICAL(&s_runtime_lock);
    if (s_task_count > 0u) {
        s_task_count--;
    }
    rt->task_running = false;
    portEXIT_CRITICAL(&s_runtime_lock);

    vTaskDelete(NULL);
}

PoolRuntimeStatus pool_session_runtime_start_task(PoolSessionRuntime *rt)
{
    TaskHandle_t h;
    uint32_t     latched;

    if (rt == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!rt->initialized || !rt->booted) {
        return RUNTIME_ERR_NOT_INITIALIZED;
    }

    portENTER_CRITICAL(&s_runtime_lock);
    if (s_task_count != 0u) {
        portEXIT_CRITICAL(&s_runtime_lock);
        return RUNTIME_ERR_TASK_ALREADY_RUNNING; /* exactly one, module-wide */
    }
    s_task_count = 1u;
    portEXIT_CRITICAL(&s_runtime_lock);

    rt->control.shutdown_requested = false;
    latched = rt->control.pending_events;
    rt->control.pending_events = 0u;

    h = xTaskCreateStatic(runtime_task, POOL_RUNTIME_TASK_NAME,
                          POOL_RUNTIME_TASK_STACK_BYTES, (void *)rt,
                          POOL_RUNTIME_TASK_PRIORITY, s_task_stack, &s_task_tcb);
    if (h == NULL) {
        portENTER_CRITICAL(&s_runtime_lock);
        s_task_count = 0u;
        portEXIT_CRITICAL(&s_runtime_lock);
        rt->control.pending_events = latched;
        ESP_LOGE(TAG, "runtime owner task creation failed");
        /* A task failure NEVER releases ownership or a protocol hold. */
        return RUNTIME_ERR_TASK_CREATE_FAILED;
    }

    rt->task_handle  = (void *)h;
    runtime_set_task_running(rt, true);
    rt->snapshot.task_running = true;
    /* Deliver the bootstrap event plus anything latched before the task
     * existed; duplicate bits simply OR together and stay idempotent. */
    (void)pool_session_runtime_notify(rt, latched | RUNTIME_EVENT_BOOTSTRAP_COMPLETE);
    return RUNTIME_OK;
}

PoolRuntimeStatus pool_session_runtime_stop_task(PoolSessionRuntime *rt)
{
    uint32_t waited_ms = 0u;

    if (rt == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!runtime_task_is_running(rt) || rt->task_handle == NULL) {
        return RUNTIME_OK; /* idempotent */
    }
    (void)pool_session_runtime_notify(rt, RUNTIME_EVENT_SHUTDOWN_FOR_TEST);
    while (runtime_task_is_running(rt) && waited_ms < POOL_RUNTIME_STOP_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10u;
    }
    rt->snapshot.task_running = runtime_task_is_running(rt);
    return rt->snapshot.task_running ? RUNTIME_ERR_TASK_ALREADY_RUNNING : RUNTIME_OK;
}

PoolRuntimeStatus pool_session_runtime_notify(PoolSessionRuntime *rt, uint32_t event_bits)
{
    uint32_t sanitized;

    if (rt == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    sanitized = pool_runtime_event_sanitize(event_bits);
    if (sanitized == 0u) {
        return RUNTIME_ERR_UNSUPPORTED_EVENT; /* unknown bits: dropped */
    }
    if (rt->task_handle == NULL) {
        /* Latch until the task exists; duplicate bits simply OR together. */
        rt->control.pending_events |= sanitized;
        return RUNTIME_OK;
    }
    xTaskNotify((TaskHandle_t)rt->task_handle, sanitized, eSetBits);
    return RUNTIME_OK;
}

PoolRuntimeStatus pool_session_runtime_notify_from_callback(PoolSessionRuntime *rt,
                                                            uint32_t event_bits)
{
    uint32_t sanitized;
    void    *handle;

    if (rt == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    sanitized = pool_runtime_event_sanitize(event_bits);
    if (sanitized == 0u) {
        return RUNTIME_ERR_UNSUPPORTED_EVENT; /* unknown bits: dropped */
    }
    portENTER_CRITICAL(&s_runtime_lock);
    handle = rt->task_handle;
    portEXIT_CRITICAL(&s_runtime_lock);
    if (handle == NULL) {
        /* Deliberately NOT latched: writing control.pending_events from a
         * foreign thread would be an unsynchronized read-modify-write. The
         * bounded tick re-evaluates instead, so nothing is stranded. */
        return RUNTIME_ERR_NOT_INITIALIZED;
    }
    xTaskNotify((TaskHandle_t)handle, sanitized, eSetBits);
    return RUNTIME_OK;
}
