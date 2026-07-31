/*
 * NeuralAxe timed pool sessions — ESP-IDF runtime adapter (Gate B6).
 *
 * See pool_session_runtime.h for the full contract. This translation unit
 * owns the ONLY production runtime instance, the ONLY runtime owner task and
 * the ONLY reset-reason read. It contains NO pool-configuration change, NO
 * Stratum call, NO OTA, NO restart and NO HTTP surface: every such call site
 * belongs to Gate B7 or later.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "pool_session_runtime.h"
#include "pool_session_reset.h"

/* ------------------------------------------------------------------ */
/* Bounded task configuration                                          */
/* ------------------------------------------------------------------ */

/*
 * Statically allocated: no heap is used for the runtime task at any point.
 * Every large working struct lives inside PoolSessionRuntime, so the task
 * stack only carries small locals, the FreeRTOS frame and ESP_LOG calls.
 * The module-wide single-task guard means exactly ONE stack buffer is ever
 * needed (production has one instance; tests serialize their own).
 */
#define POOL_RUNTIME_TASK_STACK_BYTES 4096
#define POOL_RUNTIME_TASK_PRIORITY    4
#define POOL_RUNTIME_TASK_NAME        "nx_pool_rt"

/* Bounded tick of the runtime task: no busy loop, and a missed notification
 * can never strand the bounded trusted-time wait. */
#define POOL_RUNTIME_TICK_MS   1000u
#define POOL_RUNTIME_TICK_S    1u
/* Bounded stop handshake (never an unbounded join). */
#define POOL_RUNTIME_STOP_WAIT_MS 2000u

/*
 * Hard per-boot ceiling on B4 proposal commits: EXACTLY ONE.
 *
 * The committed B4 counter model derives its proposal from the record's
 * persisted values (reboot_count = increment(record.reboot_count)), so
 * re-evaluating the engine on a freshly persisted record would propose a
 * SECOND increment for the same physical boot. The committed B4 §4.1 order
 * is therefore plan -> persist the proposal -> act, never plan -> persist ->
 * re-plan -> persist. Gate B6 obeys that literally: one durable proposal per
 * boot, and re-evaluation NEVER writes. No flash-wear loop is possible.
 */
#define POOL_RUNTIME_MAX_PROPOSAL_COMMITS 1u

static const char *TAG = "nx_pool_rt";

/* ------------------------------------------------------------------ */
/* Module-wide guards (single task, coordinator-call depth)            */
/* ------------------------------------------------------------------ */

static portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_task_count   = 0u;
static uint32_t     s_coord_depth  = 0u;

static StaticTask_t s_task_tcb;
static StackType_t  s_task_stack[POOL_RUNTIME_TASK_STACK_BYTES];

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

static bool runtime_time_source_configured(const PoolSessionRuntime *rt)
{
    return rt->deps.sntp_ops != NULL && rt->deps.ntp_server != NULL &&
           rt->deps.ntp_server[0] != '\0';
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
                                      rt->control.proposal_commits,
                                      rt->task_running, &rt->snapshot);

    ESP_LOGI(TAG, "state=%s protocol=%s status=%s decision=%s store=%s phase=%s",
             pool_runtime_state_str(rt->decision.state),
             pool_runtime_protocol_str(rt->snapshot.protocol),
             pool_runtime_status_str(rt->decision.status),
             pool_boot_decision_str(rt->plan.decision),
             pool_store_result_str(rt->store_result),
             pool_operation_phase_str(rt->lease.phase));
}

/* ------------------------------------------------------------------ */
/* Persistence before action (commit + independent read-back proof)    */
/* ------------------------------------------------------------------ */

/*
 * Commit the mandatory B4 proposal through B3 and PROVE it landed by an
 * independent reload. Only after that proof does the runtime state advance.
 * Never clears a record, never writes a tombstone, never discharges the
 * restore obligation, never lowers the trusted-epoch floor.
 */
static PoolRuntimeStatus runtime_persist_proposal(PoolSessionRuntime *rt)
{
    PoolStoreResult  commit_res;
    PoolStoreResult  reload_res;
    PoolRuntimeStatus proof;
    PoolOperationPersistenceProof ev;
    PoolOperationStatus os;

    if (!rt->record_present) {
        /* A proposal without a committed record is internally inconsistent:
         * hold rather than invent a record. */
        rt->persist_attempted = true;
        rt->persist_verified  = false;
        rt->persist_result    = STORE_STATE_CONFLICT;
        return RUNTIME_ERR_INTERNAL_CONSISTENCY;
    }
    if (rt->control.proposal_commits >= POOL_RUNTIME_MAX_PROPOSAL_COMMITS) {
        rt->persist_attempted = true;
        rt->persist_verified  = false;
        rt->persist_result    = STORE_STATE_CONFLICT;
        return RUNTIME_ERR_PERSIST_DUPLICATE;
    }
    if (!pool_runtime_proposal_should_commit(&rt->control, rt->plan.plan_fingerprint)) {
        /* The identical proposal is already durable: not an error, and
         * crucially NOT a second write. */
        rt->persist_verified = true;
        return RUNTIME_OK;
    }

    /* Stage: copy the committed record and apply ONLY the abstract proposal. */
    rt->work_record = rt->record;
    if (rt->plan.record_proposal.update_needed) {
        rt->work_record.state             = rt->plan.record_proposal.proposed_state;
        rt->work_record.last_failure_code = rt->plan.record_proposal.proposed_failure_code;
    }
    rt->work_record.reboot_count                  = rt->plan.counters.reboot_count;
    rt->work_record.recovery_attempt_count        = rt->plan.counters.recovery_attempt_count;
    rt->work_record.consecutive_recovery_failures = rt->plan.counters.consecutive_recovery_failures;
    rt->work_record.last_reset_class              = rt->plan.counters.reset_class_for_record;

    rt->persist_attempted = true;
    commit_res = pool_session_store_commit_record(&rt->store, &rt->work_record);
    rt->persist_result = commit_res;
    if (commit_res != STORE_OK) {
        rt->persist_verified = false;
        if (commit_res == STORE_COMMIT_UNCERTAIN) {
            /* Contract 13: create the B5 recovery guard and keep the hold. */
            coord_enter();
            memset(&ev, 0, sizeof(ev));
            ev.kind         = OP_PROOF_RECOVERY_UPDATE_COMMITTED;
            ev.store_result = STORE_COMMIT_UNCERTAIN;
            ev.session_id   = rt->record.session_id;
            (void)pool_operation_coordinator_apply_persistence_proof(&rt->coord, &rt->token,
                                                                     &ev, &rt->token);
            coord_exit();
            runtime_refresh_lease(rt);
            return RUNTIME_ERR_STORE_UNCERTAIN;
        }
        return RUNTIME_ERR_PERSIST_FAILED;
    }

    /* Independent read-back: reload the committed state and prove it. */
    reload_res = pool_session_store_load(&rt->store, &rt->work_record, &rt->load_info);
    proof = pool_runtime_verify_proposal_readback(&rt->record, &rt->work_record,
                                                  &rt->plan, reload_res);
    if (proof != RUNTIME_OK) {
        rt->persist_verified = false;
        rt->persist_result   = reload_res;
        return proof;
    }

    /* Proven durable: adopt the reloaded record as the committed truth. */
    rt->record = rt->work_record;
    (void)pool_session_store_committed_generation(&rt->store, &rt->committed_generation);
    rt->persist_verified = true;
    (void)pool_runtime_proposal_record_commit(&rt->control, rt->plan.plan_fingerprint);

    /* Hand the durable evidence to B5 (outside any store operation). */
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
        return RUNTIME_ERR_OWNERSHIP_MISMATCH;
    }
    return RUNTIME_OK;
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
 * This NEVER writes: the single per-boot proposal was already committed and
 * read-back verified during boot, and re-running the counter model would
 * propose a second increment for the same boot. It only tightens the posture
 * (owned phase + runtime state) and republishes. It never releases a protocol
 * hold, never frees ownership and never executes a pool or Stratum action.
 */
static void runtime_reevaluate(PoolSessionRuntime *rt)
{
    runtime_refresh_time_snapshot(rt);
    runtime_build_boot_context(rt);
    (void)pool_session_recovery_plan(&rt->boot_ctx, &rt->plan);
    runtime_reconcile_lease(rt);
    runtime_refresh_lease(rt);
    runtime_publish(rt);
}

/* ------------------------------------------------------------------ */
/* Trusted-time provider (started only when recovery requires it)      */
/* ------------------------------------------------------------------ */

/*
 * Start the B2 SNTP provider. Called ONLY when all three hold:
 *   1. the recovery plan requires trusted time;
 *   2. network-ready has been signalled;
 *   3. a valid product-configured time source exists.
 * With no configured source the runtime reports
 * RUNTIME_ERR_TIME_SOURCE_UNCONFIGURED and keeps the protocol HELD — it
 * never contacts a public NTP server and never fabricates trust.
 */
static PoolRuntimeStatus runtime_start_time_provider(PoolSessionRuntime *rt)
{
    PoolTimeSntpConfig cfg;
    PoolTimeError      err;

    if (!rt->plan.trusted_time_required) {
        return RUNTIME_OK; /* nothing to do; no networking */
    }
    if (!rt->control.network_ready) {
        return RUNTIME_ERR_TIME_WAIT_PENDING;
    }
    if (!runtime_time_source_configured(rt)) {
        return RUNTIME_ERR_TIME_SOURCE_UNCONFIGURED;
    }
    if (rt->time_provider_started) {
        return RUNTIME_OK; /* idempotent */
    }

    if (!rt->time_provider_initialized) {
        pool_time_sntp_config_defaults(&cfg);
        cfg.server_count = 1u;
        strncpy(cfg.servers[0], rt->deps.ntp_server, sizeof(cfg.servers[0]) - 1u);
        cfg.servers[0][sizeof(cfg.servers[0]) - 1u] = '\0';
        cfg.sync_wait_s = rt->control.wait_limit_s;

        err = pool_time_sntp_init(&rt->time_provider, rt->deps.sntp_ops, &cfg, &rt->time_policy);
        if (err != TIME_OK) {
            ESP_LOGW(TAG, "time provider init rejected (%s)", pool_time_error_str(err));
            return RUNTIME_ERR_TIME_SOURCE_UNCONFIGURED;
        }
        rt->time_provider_initialized = true;
    }

    err = pool_time_sntp_start(&rt->time_provider);
    if (err != TIME_OK) {
        ESP_LOGW(TAG, "time provider start rejected (%s)", pool_time_error_str(err));
        return RUNTIME_ERR_TIME_SOURCE_UNCONFIGURED;
    }
    rt->time_provider_started = true;
    ESP_LOGI(TAG, "trusted-time provider started (bounded wait %us)", rt->control.wait_limit_s);
    return RUNTIME_OK;
}

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
    rt->time_source_configured = runtime_time_source_configured(rt);

    pool_runtime_control_init(&rt->control, runtime_sync_wait_limit(rt));
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
        (void)pool_time_sntp_deinit(&rt->time_provider);
        rt->time_provider_initialized = false;
        rt->time_provider_started     = false;
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

    /* ---- Step 9: persist + read-back verify the mandatory proposal. ---- */
    if (os == OP_OK && pool_runtime_plan_requires_persistence(&rt->plan)) {
        persist_status = runtime_persist_proposal(rt);
        (void)persist_status; /* the posture is decided by classification */
    }

    /*
     * ---- Step 10: re-evaluation is NOT required here, and doing it would be
     * WRONG. The committed B4 counter model derives reboot_count from the
     * record's persisted value, so re-running the engine on the record we
     * just wrote would propose a second increment for this one boot. The
     * committed B4 §4.1 order is plan -> persist -> act; the plan the B5
     * lease was bootstrapped from stays authoritative for this boot, and
     * re-evaluation happens only in the owner task when the CONTEXT changes.
     */

    /* ---- Steps 11/12: classify and publish the sanitized snapshot. ---- */
    runtime_refresh_lease(rt);
    runtime_publish(rt);
    rt->booted = true;

    return rt->decision.status;
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
        if (outcome.start_time_provider) {
            (void)runtime_start_time_provider(rt);
        }
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
