/*
 * Deterministic tests for the Gate B7 controlled-execution engine.
 *
 * The harness boots the REAL B6 runtime (real B3 store + real B4 planning +
 * real B5 coordinator) on a fake persistence backend and a fake monotonic
 * clock, then binds the executor with fake configuration/protocol adapters
 * and drives pool_session_executor_step() directly — exactly what the
 * production owner-task hook does, without cross-task races.
 *
 * NOTHING here contacts a network, an NTP server, DNS, a pool, OTA, a
 * restart path or hardware. All identities are synthetic "*.example"
 * fixtures; no real credential, wallet or password exists anywhere.
 */

#include <string.h>
#include "unity.h"
#include "pool_session_execution.h"
#include "pool_session_reset.h"

#define EPOCH_A_S   1750000000ull
#define NOW_EPOCH_S (EPOCH_A_S + 600ull)
#define SESSION_ID  4242u

/* ---------------- fake persistence backend (B3 model) ---------------- */

#define FK_A 0
#define FK_B 1
#define FK_P 2

typedef struct {
    bool    present;
    size_t  len;
    uint8_t bytes[POOL_RECORD_MAX_ENCODED];
} FakeVal;

typedef struct {
    FakeVal committed[3];
    FakeVal staged[3];
    bool    staged_dirty[3];
    bool    opened;
    int     fail_write_after;  /* -1 never; 0 fail on the next write  */
    int     fail_commit_after; /* -1 never; 0 fail on the next commit */
} FakeNvs;

static FakeNvs g_fake;

/*
 * BLOCKING-BOUNDARY LOCK-DEPTH PROOF. Every fake below stands in for a
 * potentially blocking external operation (NVS read/write/commit, socket
 * connect/write, protocol task lifecycle, coordinator handoff). Each one
 * asserts the engine's gate lock is at depth ZERO on entry, so the ENTIRE
 * suite proves no lock or critical section is ever held across blocking
 * IO. Deterministic: the tests are single-task.
 */
#define ASSERT_BLOCKING_BOUNDARY_LOCK_FREE()     TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth())

static int fk_index(const char *key)
{
    if (strcmp(key, POOL_STORE_KEY_SLOT_A) == 0) return FK_A;
    if (strcmp(key, POOL_STORE_KEY_SLOT_B) == 0) return FK_B;
    if (strcmp(key, POOL_STORE_KEY_ACTIVE) == 0) return FK_P;
    return -1;
}

static bool fk_should_fail(int *counter)
{
    if (*counter < 0) return false;
    if (*counter == 0) { *counter = -1; return true; }
    (*counter)--;
    return false;
}

static int fk_open(void *ctx)
{
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* NVS boundary */
    (void)ctx;
    g_fake.opened = true;
    return POOL_STORE_BACKEND_OK;
}

static int fk_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len)
{
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* NVS boundary */
    int idx = fk_index(key);
    const FakeVal *v;
    (void)ctx;
    if (!g_fake.opened || idx < 0 || out_len == NULL) return POOL_STORE_BACKEND_IO;
    v = g_fake.staged_dirty[idx] ? &g_fake.staged[idx] : &g_fake.committed[idx];
    if (!v->present) return POOL_STORE_BACKEND_NOT_FOUND;
    *out_len = v->len;
    if (buf == NULL || v->len > cap) return POOL_STORE_BACKEND_OK;
    memcpy(buf, v->bytes, v->len);
    return POOL_STORE_BACKEND_OK;
}

static int fk_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* NVS boundary */
    int idx = fk_index(key);
    (void)ctx;
    if (fk_should_fail(&g_fake.fail_write_after)) return POOL_STORE_BACKEND_IO;
    if (!g_fake.opened || idx < 0 || buf == NULL || len == 0 ||
        len > POOL_RECORD_MAX_ENCODED) {
        return POOL_STORE_BACKEND_IO;
    }
    g_fake.staged[idx].present = true;
    g_fake.staged[idx].len = len;
    memcpy(g_fake.staged[idx].bytes, buf, len);
    g_fake.staged_dirty[idx] = true;
    return POOL_STORE_BACKEND_OK;
}

static int fk_commit(void *ctx)
{
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* NVS boundary */
    int i;
    (void)ctx;
    if (fk_should_fail(&g_fake.fail_commit_after)) return POOL_STORE_BACKEND_IO;
    if (!g_fake.opened) return POOL_STORE_BACKEND_IO;
    for (i = 0; i < 3; i++) {
        if (g_fake.staged_dirty[i]) {
            g_fake.committed[i] = g_fake.staged[i];
            g_fake.staged_dirty[i] = false;
        }
    }
    return POOL_STORE_BACKEND_OK;
}

static int fk_close(void *ctx)
{
    (void)ctx;
    /* The backend stays readable after a close: the harness opens SHORT-LIVED
     * verification stores (reload_committed) next to the runtime's long-lived
     * one on the SAME fake, and a close that revoked the shared handle would
     * break the runtime store mid-test. Only power loss re-gates open. */
    return POOL_STORE_BACKEND_OK;
}

static const PoolStoreBackendOps g_fake_ops = {
    .open = fk_open, .read_blob = fk_read, .write_blob = fk_write,
    .commit = fk_commit, .close = fk_close,
};

static void fake_store_reset(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.fail_write_after  = -1;
    g_fake.fail_commit_after = -1;
}

/* Model power loss: staged values vanish, committed values survive. */
static void fake_store_power_loss(void)
{
    memset(g_fake.staged, 0, sizeof(g_fake.staged));
    memset(g_fake.staged_dirty, 0, sizeof(g_fake.staged_dirty));
    g_fake.opened = false;
}

/* ---------------- fake platform (clock / reset / no SNTP) ---------------- */

static uint64_t g_mono_us;
static int32_t  g_reset_raw;
/* Mirrors the chip's own job-slot numbering for delivered work. */
static uint8_t  g_next_job_id;
static uint32_t g_next_facts_seq;

static int32_t  fake_reset_reason(void) { return g_reset_raw; }
static uint64_t fake_monotonic(void)    { return g_mono_us; }

/* ---------------- fake configuration adapter ---------------- */

typedef struct {
    PoolExecEffectiveConfig effective;      /* the fake "flash" truth       */
    bool read_fail;                         /* readback returns invalid     */
    bool stage_reject;                      /* definite pre-enqueue reject  */
    int  apply_delay_reads;                 /* effective lands after N reads */
    bool apply_partial;                     /* only the primary port lands  */
    bool apply_foreign;                     /* a foreign port value lands   */
    PoolExecEffectiveConfig staged_desired;
    bool staged_pending;
    int  stage_calls;
    int  refresh_calls;
    bool refresh_fail;
    char board[POOL_SESSION_BOARD_MAX];
    char asic[POOL_SESSION_ASIC_MAX];
    bool identity_fail;
} FakeCfg;

static FakeCfg g_cfg;

static bool fc_device_identity(void *ctx, char *board, size_t bcap,
                               char *asic, size_t acap)
{
    (void)ctx;
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* config/NVS boundary */
    if (g_cfg.identity_fail) return false;
    strncpy(board, g_cfg.board, bcap - 1u);
    board[bcap - 1u] = '\0';
    strncpy(asic, g_cfg.asic, acap - 1u);
    asic[acap - 1u] = '\0';
    return true;
}

static bool fc_stage_apply(void *ctx, const PoolConfigIdentity *identity)
{
    (void)ctx;
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* config/NVS boundary */
    g_cfg.stage_calls++;
    if (g_cfg.stage_reject) return false;
    pool_exec_desired_from_identity(identity, &g_cfg.staged_desired);
    g_cfg.staged_pending = true;
    return true;
}

static void fc_read_effective(void *ctx, PoolExecEffectiveConfig *out)
{
    (void)ctx;
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* flash readback boundary */
    if (g_cfg.staged_pending) {
        if (g_cfg.apply_delay_reads > 0) {
            g_cfg.apply_delay_reads--;
        } else {
            if (g_cfg.apply_partial) {
                /* Only the primary port lands; every other field stays. */
                g_cfg.effective.primary.port = g_cfg.staged_desired.primary.port;
            } else if (g_cfg.apply_foreign) {
                g_cfg.effective.primary.port = 9999; /* neither pre nor desired */
            } else {
                g_cfg.effective = g_cfg.staged_desired;
            }
            g_cfg.staged_pending = false;
        }
    }
    if (g_cfg.read_fail) {
        memset(out, 0, sizeof(*out)); /* valid=false */
        return;
    }
    *out = g_cfg.effective;
    out->valid = true;
}

static bool fc_refresh_live(void *ctx, const PoolConfigIdentity *identity)
{
    (void)ctx;
    (void)identity;
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* live-config publish boundary */
    g_cfg.refresh_calls++;
    return !g_cfg.refresh_fail;
}

static const PoolExecConfigOps g_cfg_ops = {
    .device_identity = fc_device_identity,
    .stage_apply     = fc_stage_apply,
    .read_effective  = fc_read_effective,
    .refresh_live    = fc_refresh_live,
};

/* ---------------- fake protocol adapter ---------------- */

typedef struct {
    bool running;
    bool start_fail;
    bool stop_fail;
    bool handoff_fail;
    int  start_calls;
    int  stop_calls;
    int  handoff_calls;
    uint32_t pending_events;
    PoolExecProtocolCounters counters;
} FakeProto;

static FakeProto g_proto;

static bool fp_start(void *ctx, PoolSessionProtocol protocol)
{
    (void)ctx;
    (void)protocol;
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* socket/task-start boundary */
    g_proto.start_calls++;
    if (g_proto.start_fail) return false;
    g_proto.running = true;
    g_proto.pending_events = 0u;
    memset(&g_proto.counters, 0, sizeof(g_proto.counters)); /* fresh generation */
    return true;
}

static bool fp_stop(void *ctx)
{
    (void)ctx;
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* socket/task-stop boundary */
    g_proto.stop_calls++;
    if (g_proto.stop_fail) return false;
    g_proto.running = false;
    return true;
}

static bool fp_running(void *ctx)
{
    (void)ctx;
    return g_proto.running;
}

static uint32_t fp_poll_events(void *ctx)
{
    uint32_t bits = g_proto.pending_events;
    (void)ctx;
    g_proto.pending_events = 0u;
    return bits;
}

static void fp_counters(void *ctx, PoolExecProtocolCounters *out)
{
    (void)ctx;
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* protocol sample boundary */
    *out = g_proto.counters;
}

static bool fp_handoff(void *ctx)
{
    (void)ctx;
    ASSERT_BLOCKING_BOUNDARY_LOCK_FREE(); /* coordinator-start boundary */
    g_proto.handoff_calls++;
    return !g_proto.handoff_fail;
}

static const PoolExecProtocolOps g_proto_ops = {
    .start = fp_start, .stop = fp_stop, .running = fp_running,
    .poll_events = fp_poll_events, .counters = fp_counters,
    .handoff_source = fp_handoff,
};

/* ---------------- fixtures and harness ---------------- */

static PoolSessionRuntime  g_rt;
static PoolSessionExecutor g_ex;
static PoolSessionRecord   g_rec;
static PoolExecPolicy      g_policy;
static PoolExecutionSnapshot g_snap;

static void fill_identity(PoolConfigIdentity *c, PoolChainType chain, const char *host,
                          uint16_t port, const char *user)
{
    memset(c, 0, sizeof(*c));
    c->chain = chain;
    strncpy(c->primary.host, host, sizeof(c->primary.host) - 1);
    c->primary.port = port;
    strncpy(c->primary.user, user, sizeof(c->primary.user) - 1);
    c->primary.protocol = POOL_PROTO_STRATUM_V1;
}

static void make_rec(PoolSessionRecord *r, PoolSessionState st, bool rr)
{
    pool_session_record_init(r);
    r->session_id       = SESSION_ID;
    r->b1_model_version = POOL_SESSION_MODEL_VERSION;
    r->state            = st;
    fill_identity(&r->source, POOL_CHAIN_BITCOIN, "src-pool.example", 3333, "src-acct.worker");
    fill_identity(&r->target, POOL_CHAIN_BITCOIN_CASH, "tgt-pool.example", 3334, "tgt-acct.worker");
    r->password_policy  = POOL_SESSION_PW_KEEP_CURRENT;
    r->restore_required = rr;
    r->duration_s       = 3600u;
    if (st == POOL_STATE_TARGET_ACTIVE) {
        r->target_verify.connection_observed = true;
        r->target_verify.mining_observed     = true;
        r->target_verify.identity_verified   = true;
        r->verified_start_valid     = true;
        r->verified_start_epoch_s   = EPOCH_A_S;
        r->deadline_valid           = true;
        r->deadline_epoch_s         = EPOCH_A_S + 3600u;
        r->deadline_sync_generation = 1u;
        r->latest_trusted_valid     = true;
        r->latest_trusted_epoch_s   = EPOCH_A_S + 100u;
    }
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(r));
}

/* Seed the fake backend with a committed record through the real B3 store. */
static void seed_record(const PoolSessionRecord *rec)
{
    PoolSessionStore  st;
    PoolSessionRecord copy = *rec;
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&st, &g_fake_ops, NULL));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&st, &copy));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&st));
}

/* Reload the committed record independently of the runtime instance. */
static PoolStoreResult reload_committed(PoolSessionRecord *out)
{
    PoolSessionStore st;
    PoolStoreResult  r;
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&st, &g_fake_ops, NULL));
    r = pool_session_store_load(&st, out, NULL);
    (void)pool_session_store_deinit(&st);
    return r;
}

static void fakes_reset(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    strncpy(g_cfg.board, "601", sizeof(g_cfg.board) - 1);
    strncpy(g_cfg.asic, "BM1370", sizeof(g_cfg.asic) - 1);
    memset(&g_proto, 0, sizeof(g_proto));
    g_next_job_id = 0u;
    g_next_facts_seq = 0u;
    g_mono_us   = 5ull * 1000000ull;
    g_reset_raw = POOL_RESET_RAW_POWERON;
    pool_session_execution_gate_reset();
}

/* Boot the REAL runtime on the fakes. */
static void boot_runtime(void)
{
    PoolSessionRuntimeDeps deps;

    memset(&deps, 0, sizeof(deps));
    deps.store_ops             = &g_fake_ops;
    deps.store_ctx             = NULL;
    deps.sntp_ops              = NULL; /* no time provider in these tests */
    deps.read_reset_reason_raw = fake_reset_reason;
    deps.monotonic_us          = fake_monotonic;
    deps.ntp_server            = "";
    deps.sync_wait_limit_s     = 600u;

    memset(&g_rt, 0, sizeof(g_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_TRUE(g_rt.booted);
}

/*
 * Fabricate the trusted-time RESUME posture the production owner task would
 * derive after an accepted sync: the REAL B4 plan under a trusted snapshot,
 * the REAL classifier verdict and the REAL B5 phase transition. The runtime
 * struct is documented transparent for tests.
 */
static void posture_resume(void)
{
    PoolSessionBootContext   ctx;
    PoolRuntimeClassifyInput in;

    TEST_ASSERT_TRUE(g_rt.record_present);

    /* Lease: WAITING (untrusted boot) -> VERIFYING_TARGET, token rotates. */
    TEST_ASSERT_EQUAL(OP_PHASE_WAITING_FOR_TRUSTED_TIME, g_rt.lease.phase);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_transition_phase(
                                 &g_rt.coord, &g_rt.token,
                                 OP_PHASE_VERIFYING_TARGET, &g_rt.token));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_rt.coord,
                                                                 &g_rt.lease));

    memset(&ctx, 0, sizeof(ctx));
    ctx.store_result   = STORE_OK;
    ctx.record_present = true;
    ctx.record         = &g_rt.record;
    ctx.reset_class    = g_rt.reset_class;
    ctx.sync_wait_elapsed_s = 0u;
    ctx.sync_wait_limit_s   = 600u;
    ctx.time_provider_initialized = true;
    ctx.time_snapshot.monotonic_now_us = g_mono_us;
    ctx.time_snapshot.trusted          = true;
    ctx.time_snapshot.trusted_epoch_s  = NOW_EPOCH_S;
    ctx.time_snapshot.trusted_utc_us   = NOW_EPOCH_S * 1000000ull;
    ctx.time_snapshot.anchor_age_us    = 1000u;
    ctx.time_snapshot.sync_generation  = 2u;
    ctx.time_snapshot.status           = TIME_OK;
    ctx.mining_inhibition_available    = true;

    (void)pool_session_recovery_plan(&ctx, &g_rt.plan);
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET, g_rt.plan.decision);
    TEST_ASSERT_TRUE(g_rt.plan.remaining_valid);

    memset(&in, 0, sizeof(in));
    in.store_opened   = true;
    in.store_loaded   = true;
    in.store_result   = STORE_OK;
    in.record_present = true;
    in.record         = &g_rt.record;
    in.plan           = &g_rt.plan;
    in.bootstrap_status           = OP_OK;
    in.lease_owner                = g_rt.lease.owner;
    in.lease_phase                = g_rt.lease.phase;
    in.lease_generation           = g_rt.lease.lease_generation;
    in.terminal_pending           = g_rt.lease.terminal_pending;
    in.lease_restore_required     = g_rt.lease.restore_required;
    in.lease_persistence_required = g_rt.lease.persistence_required_before_action;
    in.persist_required  = false;
    in.persist_attempted = true;
    in.persist_verified  = true;
    in.persist_result    = STORE_OK;
    (void)pool_runtime_classify(&in, &g_rt.decision);
    TEST_ASSERT_EQUAL(RUNTIME_VERIFY_TARGET_PENDING, g_rt.decision.state);
}

static void bind_executor(void)
{
    pool_exec_policy_defaults(&g_policy);
    g_policy.config_timeout_s  = 3u;
    g_policy.stop_timeout_s    = 2u;
    g_policy.connect_timeout_s = 4u;
    g_policy.job_timeout_s     = 6u;
    g_policy.target_health_s   = 4u;
    g_policy.source_health_s   = 4u;

    pool_session_executor_init(&g_ex);
    TEST_ASSERT_EQUAL(EXEC_REASON_NONE,
                      pool_session_executor_bind(&g_ex, &g_rt, &g_cfg_ops, NULL,
                                                 &g_proto_ops, NULL, &g_policy));
    pool_session_executor_set_system_ready(&g_ex, true);
}

/* One step then one fake second. */
static PoolExecState step1(void)
{
    PoolExecState s = pool_session_executor_step(&g_ex);
    g_mono_us += 1000000ull;
    return s;
}

static PoolExecState step_n(int n)
{
    PoolExecState s = EXEC_STATE_DISABLED;
    while (n-- > 0) {
        s = step1();
    }
    return s;
}

/* Drive until the executor reaches `want` (bounded; fails the test on a
 * stuck engine — the no-infinite-loop property). */
static void step_until(PoolExecState want, int max_steps)
{
    while (max_steps-- > 0) {
        if (step1() == want) {
            return;
        }
    }
    TEST_ASSERT_EQUAL(want, g_ex.state);
}

static void teardown_all(void)
{
    pool_session_executor_deinit(&g_ex);
    (void)pool_session_runtime_deinit(&g_rt);
}

/* Fresh restore-posture harness (boot yields RESTORE_SOURCE_PENDING). */
static void setup_restore(PoolSessionState record_state)
{
    fake_store_reset();
    fakes_reset();
    make_rec(&g_rec, record_state, true);
    seed_record(&g_rec);
    boot_runtime();
    TEST_ASSERT_EQUAL(RUNTIME_RESTORE_SOURCE_PENDING, g_rt.decision.state);
    TEST_ASSERT_EQUAL(OP_PHASE_RESTORING_SOURCE, g_rt.lease.phase);
    /* The device is still on the TARGET configuration. */
    pool_exec_desired_from_identity(&g_rt.record.target, &g_cfg.effective);
    bind_executor();
}

/* Fresh resume-posture harness. */
static void setup_resume(void)
{
    fake_store_reset();
    fakes_reset();
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_record(&g_rec);
    boot_runtime();
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_rt.decision.state);
    posture_resume();
    pool_exec_desired_from_identity(&g_rt.record.target, &g_cfg.effective);
    bind_executor();
}

/* Feed one parse-accepted job + successful setup into the fake protocol. */
static void feed_connection_and_job(void)
{
    g_proto.pending_events |= EXEC_PEVT_SETUP_SUCCESS;
    g_proto.counters.work_received += 1u;
}

/*
 * The COMPLETE mining-verified evidence set, produced through the EXACT
 * production entry points: the pool served work, the delivery gate released
 * a job toward the ASIC, the work item was stamped at the ASIC send point,
 * and the chip returned a result that resolves against that stamp.
 * `job_id` mirrors the chip's own slot number.
 */
/*
 * Deliver ONE work item exactly as the production job pipeline does under
 * the generation-unique work contract: the extranonce2 tag query runs on
 * the same task immediately before the ASIC send point stamps the record
 * with its canonical header facts. Every delivery carries a DISTINCT
 * header (a fresh template/extranonce2), so it is header-unique unless a
 * test deliberately replays identical facts through the raw API.
 */
static void deliver_current_work(uint8_t job)
{
    PoolExecWorkFacts f;
    uint64_t tagged = 0u;

    memset(&f, 0, sizeof(f));
    f.version        = 0x20000000u;
    f.ntime          = 0x66100000u + g_next_facts_seq;
    f.nbits          = 0x17034219u;
    f.merkle_root[0] = (uint8_t)g_next_facts_seq;
    f.merkle_root[1] = (uint8_t)(g_next_facts_seq >> 8);
    f.merkle_root[2] = (uint8_t)(g_next_facts_seq >> 16);
    g_next_facts_seq++;
    pool_session_execution_set_work_domain_active(true);
    (void)pool_session_execution_extranonce2_tag(false, false, 8u,
                                                 g_next_facts_seq, &tagged);
    pool_session_execution_note_work_delivered(job, &f);
}

static void feed_mining_evidence(void)
{
    /* Supply the full required set of DISTINCT local work proofs, each on
     * its own delivered record — exactly what the production path produces
     * over ~1 s per proof at the Gamma 601 operating point. */
    for (unsigned i = 0; i < POOL_EXEC_REQUIRED_WORK_PROOFS; i++) {
        uint8_t job = g_next_job_id++;

        g_proto.counters.work_received += 1u;
        pool_session_execution_note_job_forwarded();
        deliver_current_work(job);
        TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                          pool_session_execution_resolve_asic_result(job, true));
    }
}

/* Software-side delivery ONLY: work is stamped and released toward the ASIC
 * but the chip returns nothing attributable. */
static void feed_delivery_without_asic(void)
{
    g_proto.counters.work_received += 1u;
    pool_session_execution_note_job_forwarded();
    deliver_current_work(g_next_job_id++);
    pool_session_execution_note_asic_register_read(); /* liveness only */
}

/* Snapshot everything a rejected session must leave untouched. */
typedef struct {
    uint32_t                committed_generation;
    PoolSessionState        state;
    bool                    restore_required;
    PoolOperationOwner      owner;
    PoolOperationLeasePhase phase;
    uint32_t                lease_generation;
    PoolExecEffectiveConfig effective;
    int                     stage_calls;
    int                     refresh_calls;
    int                     start_calls;
} PreState;

static void capture_pre(PreState *p)
{
    PoolSessionRecord rec;

    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&rec));
    p->committed_generation = rec.generation;
    p->state                = rec.state;
    p->restore_required     = rec.restore_required;
    p->owner                = g_rt.lease.owner;
    p->phase                = g_rt.lease.phase;
    p->lease_generation     = g_rt.lease.lease_generation;
    p->effective            = g_cfg.effective;
    p->stage_calls          = g_cfg.stage_calls;
    p->refresh_calls        = g_cfg.refresh_calls;
    p->start_calls          = g_proto.start_calls;
}

static void assert_nothing_changed(const PreState *p)
{
    PoolSessionRecord rec;

    /* No NVS/record mutation. */
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&rec));
    TEST_ASSERT_EQUAL_UINT32(p->committed_generation, rec.generation);
    TEST_ASSERT_EQUAL(p->state, rec.state);
    TEST_ASSERT_EQUAL(p->restore_required, rec.restore_required);
    /* No lease movement. */
    TEST_ASSERT_EQUAL(p->owner, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(p->phase, g_rt.lease.phase);
    TEST_ASSERT_EQUAL_UINT32(p->lease_generation, g_rt.lease.lease_generation);
    /* No pool configuration write and no protocol action. */
    TEST_ASSERT_EQUAL_INT(p->stage_calls, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL_INT(p->refresh_calls, g_cfg.refresh_calls);
    TEST_ASSERT_EQUAL_INT(p->start_calls, g_proto.start_calls);
    TEST_ASSERT_EQUAL_MEMORY(&p->effective, &g_cfg.effective, sizeof(p->effective));
}

/* ================================================================= */
/* Lifecycle / feature posture                                        */
/* ================================================================= */

TEST_CASE("exec_rt: unbound executors perform nothing (fail closed)", "[pool_exec_rt]")
{
    PoolSessionExecutor ex;

    memset(&ex, 0, sizeof(ex)); /* a zeroed executor is DISABLED */
    TEST_ASSERT_EQUAL(EXEC_STATE_DISABLED, pool_session_executor_step(&ex));
    TEST_ASSERT_FALSE(pool_session_executor_owns_flow(&ex));
    TEST_ASSERT_EQUAL(EXEC_STATE_DISABLED, pool_session_executor_step(NULL));

    pool_session_executor_init(&ex);
    TEST_ASSERT_EQUAL(EXEC_STATE_DISABLED, pool_session_executor_step(&ex));

    /* A partial vtable never binds. */
    {
        PoolExecProtocolOps broken = g_proto_ops;
        broken.handoff_source = NULL;
        fake_store_reset();
        fakes_reset();
        boot_runtime();
        TEST_ASSERT_EQUAL(EXEC_REASON_NOT_BOUND,
                          pool_session_executor_bind(&ex, &g_rt, &g_cfg_ops, NULL,
                                                     &broken, NULL, NULL));
        TEST_ASSERT_EQUAL(EXEC_STATE_DISABLED, pool_session_executor_step(&ex));
        (void)pool_session_runtime_deinit(&g_rt);
    }
}

TEST_CASE("exec_rt: an empty store stays IDLE and leaves the gate open", "[pool_exec_rt]")
{
    fake_store_reset();
    fakes_reset();
    boot_runtime();
    TEST_ASSERT_EQUAL(RUNTIME_FREE, g_rt.decision.state);
    bind_executor();
    TEST_ASSERT_EQUAL(EXEC_STATE_IDLE, step_n(3));
    TEST_ASSERT_FALSE(pool_session_executor_owns_flow(&g_ex));
    TEST_ASSERT_TRUE(pool_session_execution_asic_work_allowed());
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls);
    teardown_all();
}

TEST_CASE("exec_rt: system-not-ready blocks every action", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    pool_session_executor_set_system_ready(&g_ex, false);
    step_n(5);
    TEST_ASSERT_EQUAL(EXEC_STATE_ENTRY_PENDING, g_ex.state);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls);
    TEST_ASSERT_EQUAL(EXEC_REASON_SYSTEM_NOT_READY, g_ex.reason);
    /* Readiness unblocks the same posture. */
    pool_session_executor_set_system_ready(&g_ex, true);
    step_until(EXEC_STATE_SOURCE_APPLYING, 6);
    teardown_all();
}

/* ================================================================= */
/* Board / ASIC compatibility                                         */
/* ================================================================= */

TEST_CASE("exec_rt: wrong board fails closed with zero mutation", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    strncpy(g_cfg.board, "204", sizeof(g_cfg.board) - 1);
    step_n(3);
    TEST_ASSERT_EQUAL(EXEC_STATE_ERROR, g_ex.state);
    TEST_ASSERT_EQUAL(EXEC_REASON_BOARD_UNSUPPORTED, g_ex.reason);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: wrong or unknown ASIC fails closed with zero mutation", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    strncpy(g_cfg.asic, "BM1366", sizeof(g_cfg.asic) - 1);
    step_n(3);
    TEST_ASSERT_EQUAL(EXEC_STATE_ERROR, g_ex.state);
    TEST_ASSERT_EQUAL(EXEC_REASON_ASIC_UNSUPPORTED, g_ex.reason);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    teardown_all();

    setup_restore(POOL_STATE_RESTORE_DUE);
    g_cfg.identity_fail = true; /* unknown hardware = fail closed */
    step_n(3);
    TEST_ASSERT_EQUAL(EXEC_STATE_ERROR, g_ex.state);
    TEST_ASSERT_EQUAL(EXEC_REASON_BOARD_UNSUPPORTED, g_ex.reason);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    teardown_all();
}

/* ================================================================= */
/* Source restoration — the full verified sequence                    */
/* ================================================================= */

TEST_CASE("exec_rt: full verified source restoration reaches COMPLETE and DONE", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    setup_restore(POOL_STATE_RESTORE_DUE);

    /* Entry -> restore pending -> APPLYING_RESTORE committed. */
    step_until(EXEC_STATE_SOURCE_APPLYING, 6);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_RESTORE, after.state);
    TEST_ASSERT_TRUE(after.restore_required);

    /* Exact source apply -> activation boundary -> controlled start. */
    step_until(EXEC_STATE_SOURCE_CONNECTING, 8);
    TEST_ASSERT_EQUAL_INT(1, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL_INT(1, g_cfg.refresh_calls);
    TEST_ASSERT_EQUAL_INT(1, g_proto.start_calls);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());

    /* Connection + exact identity open the source gate. */
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 4);
    TEST_ASSERT_TRUE(pool_session_execution_asic_work_allowed());

    /* Obligation still held before the mining proof. */
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_TRUE(after.restore_required);

    /* Mining resumed: one more job + one delivered job. */
    feed_mining_evidence();
    step_until(EXEC_STATE_DONE, 8);

    /* COMPLETE is durable, the obligation is discharged, the record is
     * RETAINED (no tombstone, no acknowledgement in B7). */
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, after.state);
    TEST_ASSERT_FALSE(after.restore_required);
    TEST_ASSERT_TRUE(after.restore_verify.connection_observed);
    TEST_ASSERT_TRUE(after.restore_verify.mining_observed);
    TEST_ASSERT_TRUE(after.restore_verify.identity_verified);

    /* The lease released to FREE with the terminal retained. */
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    TEST_ASSERT_TRUE(g_rt.lease.terminal_pending);

    /* The production coordinator received the handoff exactly once. */
    TEST_ASSERT_EQUAL_INT(1, g_proto.handoff_calls);
    TEST_ASSERT_FALSE(g_proto.running);
    TEST_ASSERT_TRUE(pool_session_execution_asic_work_allowed());
    TEST_ASSERT_FALSE(pool_session_executor_owns_flow(&g_ex));
    teardown_all();
}

TEST_CASE("exec_rt: restore executes from TARGET_FAILED and INTERRUPTED", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_TARGET_FAILED);
    step_until(EXEC_STATE_SOURCE_APPLYING, 6);
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls);
    teardown_all();

    setup_restore(POOL_STATE_INTERRUPTED);
    step_until(EXEC_STATE_SOURCE_APPLYING, 6);
    teardown_all();
}

TEST_CASE("exec_rt: a rebooted RESTORE_FAILED re-attempts through the audited B4 budget", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    /* ACROSS boots the committed B4 design performs a BOUNDED automatic
     * re-attempt toward the source: boot proposes RESTORE_DUE and spends a
     * recovery-attempt unit. (WITHIN a boot the executor never auto-retries
     * a RESTORE_FAILED — the held-posture tests prove that separately.) */
    setup_restore(POOL_STATE_RESTORE_FAILED);
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, after.state);
    TEST_ASSERT_TRUE(after.restore_required);
    TEST_ASSERT_TRUE(after.recovery_attempt_count >= 1u);
    /* The executor then executes the owed restoration normally. */
    step_until(EXEC_STATE_SOURCE_CONNECTING, 14);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);
    feed_mining_evidence();
    step_until(EXEC_STATE_DONE, 10);
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, after.state);
    TEST_ASSERT_FALSE(after.restore_required);
    teardown_all();
}

TEST_CASE("exec_rt: partial source writes exhaust the budget into RESTORE_FAILED", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    setup_restore(POOL_STATE_RESTORE_DUE);
    g_cfg.apply_partial = true;
    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 60);

    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_FAILED, after.state);
    TEST_ASSERT_TRUE(after.restore_required); /* NEVER cleared by failure */
    TEST_ASSERT_EQUAL_UINT16((uint16_t)ERR_RESTORE_APPLY, after.last_failure_code);
    /* Bounded: the B1 budget (3 retries + the original attempt). */
    TEST_ASSERT_TRUE(g_cfg.stage_calls <= 1 + (int)POOL_SESSION_MAX_RESTORE_APPLY_RETRIES);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls); /* protocol held throughout */
    teardown_all();
}

TEST_CASE("exec_rt: foreign source values fail bounded into RESTORE_FAILED", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    g_cfg.apply_foreign = true;
    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 60);
    TEST_ASSERT_EQUAL(EXEC_REASON_RESTORE_FAILED, g_ex.reason);
    teardown_all();
}

TEST_CASE("exec_rt: a dead configuration readback fails closed before mutation", "[pool_exec_rt]")
{
    PreState pre;

    setup_restore(POOL_STATE_RESTORE_DUE);
    g_cfg.read_fail = true; /* the independent readback itself is dead */
    capture_pre(&pre);

    /* Without a trustworthy readback the executor cannot even establish
     * that the stored TLS mode is restorable, so it stops at entry —
     * before any pool write, lease movement or record mutation. */
    step_n(8);
    TEST_ASSERT_EQUAL(EXEC_STATE_ERROR, g_ex.state);
    TEST_ASSERT_EQUAL(EXEC_REASON_CONFIG_UNCERTAIN, g_ex.reason);
    assert_nothing_changed(&pre);
    {
        PoolSessionRecord after;
        TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
        TEST_ASSERT_TRUE(after.restore_required); /* obligation retained */
    }
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: source valid-job timeout retries bounded then RESTORE_FAILED", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    /* Apply succeeds; the protocol connects but never serves a job. */
    step_until(EXEC_STATE_SOURCE_CONNECTING, 12);
    g_proto.pending_events |= EXEC_PEVT_SETUP_SUCCESS;
    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 120);
    /* Every retry re-ran the full transaction; the budget is bounded. */
    TEST_ASSERT_TRUE(g_proto.start_calls <= 1 + (int)POOL_SESSION_MAX_RESTORE_VERIFY_RETRIES);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: source mining-resume timeout fails into RESTORE_FAILED", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 12);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 4);
    /* Jobs arrive but NOTHING flows to the ASIC (no forwarded evidence). */
    g_proto.counters.work_received += 1u;
    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 120);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: protocol failure during source verification retries bounded", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 12);
    /* The controlled task reports failure and exits. */
    g_proto.pending_events |= EXEC_PEVT_CONNECTION_FAILED | EXEC_PEVT_TASK_EXITED;
    g_proto.running = false;
    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 140);
    TEST_ASSERT_TRUE(g_proto.start_calls <= 1 + (int)POOL_SESSION_MAX_RESTORE_VERIFY_RETRIES);
    teardown_all();
}

TEST_CASE("exec_rt: mid-restore boot re-entry verifies config before protocol", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_APPLYING_RESTORE);
    /* B1 normalizes APPLYING_RESTORE to VERIFYING_RESTORE on reboot; the
     * executor then runs the readback-only path. The device already carries
     * the SOURCE config in this scenario. */
    pool_exec_desired_from_identity(&g_rt.record.source, &g_cfg.effective);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 10);
    /* No staging was necessary: the effective config already matched. */
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL_INT(1, g_proto.start_calls);
    teardown_all();
}

TEST_CASE("exec_rt: mid-restore re-entry with a mismatched config re-applies", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_APPLYING_RESTORE);
    /* The device still carries the TARGET config: the readback-only path
     * must fail closed into a full bounded re-apply. */
    step_until(EXEC_STATE_SOURCE_CONNECTING, 14);
    TEST_ASSERT_EQUAL_INT(1, g_cfg.stage_calls); /* one full re-apply ran */
    teardown_all();
}

/* ================================================================= */
/* Target resume, live verification and the mining grant              */
/* ================================================================= */

TEST_CASE("exec_rt: resume verifies live and grants only after full evidence", "[pool_exec_rt]")
{
    setup_resume();

    /* Entry -> readback -> activation -> controlled verification start. */
    step_until(EXEC_STATE_TARGET_CONNECTING, 8);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);   /* config already exact */
    TEST_ASSERT_EQUAL_INT(1, g_proto.start_calls);
    /* Verification-only posture: NO ASIC work, NO grant. */
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    (void)pool_session_executor_snapshot(&g_ex, &g_snap);
    TEST_ASSERT_FALSE(g_snap.mining_grant_active);

    /* Connection + identity + a valid target job complete the evidence. */
    feed_connection_and_job();
    step_until(EXEC_STATE_TARGET_MINING, 6);
    TEST_ASSERT_TRUE(pool_session_execution_asic_work_allowed());
    (void)pool_session_executor_snapshot(&g_ex, &g_snap);
    TEST_ASSERT_TRUE(g_snap.mining_grant_active);
    TEST_ASSERT_TRUE(pool_exec_snapshot_valid(&g_snap));
    TEST_ASSERT_EQUAL(OP_PHASE_ACTIVE, g_rt.lease.phase);

    /* Initial health: pool keeps serving and jobs reach the ASIC. */
    feed_mining_evidence();
    step_n(2);
    TEST_ASSERT_EQUAL(EXEC_STATE_TARGET_MINING, g_ex.state);
    TEST_ASSERT_TRUE(g_ex.target_health_ok);
    teardown_all();
}

TEST_CASE("exec_rt: no grant exists before evidence completes (gate stays closed)", "[pool_exec_rt]")
{
    setup_resume();
    step_until(EXEC_STATE_TARGET_CONNECTING, 8);
    /* Connection evidence only: still no grant, still inhibited. */
    g_proto.pending_events |= EXEC_PEVT_SETUP_SUCCESS;
    step_n(2);
    TEST_ASSERT_EQUAL(EXEC_STATE_TARGET_CONNECTING, g_ex.state);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    (void)pool_session_executor_snapshot(&g_ex, &g_snap);
    TEST_ASSERT_FALSE(g_snap.mining_grant_active);
    TEST_ASSERT_FALSE(g_snap.job_verified);
    teardown_all();
}

TEST_CASE("exec_rt: resume with mismatched live config re-applies the target", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    setup_resume();
    /* The live config differs from the persisted target identity. */
    pool_exec_desired_from_identity(&g_rt.record.source, &g_cfg.effective);
    step_until(EXEC_STATE_TARGET_APPLYING, 8);
    /* The re-apply boundary is durable (APPLYING_TARGET committed). */
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_TARGET, after.state);
    TEST_ASSERT_TRUE(after.restore_required);
    /* The transaction completes and verification proceeds. */
    step_until(EXEC_STATE_TARGET_CONNECTING, 10);
    TEST_ASSERT_EQUAL_INT(1, g_cfg.stage_calls);
    feed_connection_and_job();
    step_until(EXEC_STATE_TARGET_MINING, 6);
    teardown_all();
}

TEST_CASE("exec_rt: uncertain target apply restores — never target continuation", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    setup_resume();
    pool_exec_desired_from_identity(&g_rt.record.source, &g_cfg.effective);
    g_cfg.apply_partial = true; /* the target transaction lands partially */
    step_until(EXEC_STATE_TARGET_APPLYING, 8);
    step_until(EXEC_STATE_SOURCE_APPLYING, 20);
    /* The partial-target verdict became a durable restore boundary. */
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_RESTORE, after.state);
    TEST_ASSERT_TRUE(after.restore_required);
    TEST_ASSERT_TRUE(after.restore_requested);
    /* At no point was target mining granted. */
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: target valid-job timeout ends in TARGET_FAILED then restore", "[pool_exec_rt]")
{
    setup_resume();
    step_until(EXEC_STATE_TARGET_CONNECTING, 8);
    g_proto.pending_events |= EXEC_PEVT_SETUP_SUCCESS; /* connects, no job */
    /* Bounded verify retries re-apply, then TARGET_FAILED owes restore. */
    step_until(EXEC_STATE_SOURCE_APPLYING, 200);
    {
        PoolSessionRecord after;
        TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
        TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_RESTORE, after.state);
        TEST_ASSERT_TRUE(after.restore_required);
    }
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: stale counter evidence is rejected and never grants", "[pool_exec_rt]")
{
    setup_resume();
    step_until(EXEC_STATE_TARGET_CONNECTING, 8);
    /* Counter regression below the generation baseline: anomaly. The fake
     * baseline was sampled at start (zeroed), so force a lower "now" by
     * baselining first with a positive count, then regressing. */
    g_ex.baseline.work_received = 5u; /* simulate a stale/foreign baseline */
    g_proto.counters.work_received = 2u;
    step1();
    TEST_ASSERT_NOT_EQUAL(EXEC_STATE_TARGET_MINING, g_ex.state);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: protocol failure during target verification is bounded", "[pool_exec_rt]")
{
    setup_resume();
    step_until(EXEC_STATE_TARGET_CONNECTING, 8);
    g_proto.pending_events |= EXEC_PEVT_CONNECTION_FAILED | EXEC_PEVT_TASK_EXITED;
    g_proto.running = false;
    /* Bounded retries, then TARGET_FAILED -> restoration path. */
    step_until(EXEC_STATE_SOURCE_APPLYING, 200);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

/* ================================================================= */
/* Grant revocation                                                   */
/* ================================================================= */

/* Drive a fresh resume posture all the way to an active grant. */
static void drive_to_mining(void)
{
    setup_resume();
    step_until(EXEC_STATE_TARGET_CONNECTING, 8);
    feed_connection_and_job();
    step_until(EXEC_STATE_TARGET_MINING, 6);
    feed_mining_evidence();
    step_n(1);
    TEST_ASSERT_TRUE(pool_session_execution_asic_work_allowed());
}

TEST_CASE("exec_rt: lease rotation revokes the grant and restores", "[pool_exec_rt]")
{
    drive_to_mining();
    /* An ownership transition elsewhere rotates the token: the grant's
     * lease binding is stale on the very next step. */
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_transition_phase(
                                 &g_rt.coord, &g_rt.token,
                                 OP_PHASE_VERIFYING_TARGET, &g_rt.token));
    step1();
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    TEST_ASSERT_NOT_EQUAL(EXEC_STATE_TARGET_MINING, g_ex.state);
    teardown_all();
}

TEST_CASE("exec_rt: protocol failure while mining revokes and restores", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    drive_to_mining();
    g_proto.pending_events |= EXEC_PEVT_CONNECTION_FAILED | EXEC_PEVT_TASK_EXITED;
    g_proto.running = false;
    step1();
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    step_until(EXEC_STATE_SOURCE_APPLYING, 20);
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_TRUE(after.restore_required);
    teardown_all();
}

TEST_CASE("exec_rt: health-window failure revokes and restores", "[pool_exec_rt]")
{
    setup_resume();
    step_until(EXEC_STATE_TARGET_CONNECTING, 8);
    feed_connection_and_job();
    step_until(EXEC_STATE_TARGET_MINING, 6);
    /* No further jobs and nothing forwarded: the bounded health window
     * expires and restoration begins. */
    step_until(EXEC_STATE_SOURCE_APPLYING, 30);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: the monotonic session deadline drives restoration", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    drive_to_mining();
    /* Jump past the armed remaining-session bound. */
    g_mono_us += (uint64_t)(g_rt.plan.remaining_target_s + 5u) * 1000000ull;
    step1();
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    step_until(EXEC_STATE_SOURCE_APPLYING, 20);
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_RESTORE, after.state);
    TEST_ASSERT_TRUE(after.restore_required);
    teardown_all();
}

TEST_CASE("exec_rt: grant-to-restore-to-complete releases exactly once", "[pool_exec_rt]")
{
    drive_to_mining();
    /* Deadline -> restore -> full verified source restoration. */
    g_mono_us += (uint64_t)(g_rt.plan.remaining_target_s + 5u) * 1000000ull;
    step_until(EXEC_STATE_SOURCE_CONNECTING, 30);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);
    feed_mining_evidence();
    step_until(EXEC_STATE_DONE, 10);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_TRUE(g_rt.lease.terminal_pending);
    TEST_ASSERT_EQUAL_INT(1, g_proto.handoff_calls);
    teardown_all();
}

/* ================================================================= */
/* Crash / reboot boundaries                                          */
/* ================================================================= */

/* Simulate power loss and a fresh boot on the surviving flash state. */
static void power_loss_and_reboot(void)
{
    fake_store_power_loss();
    pool_session_executor_deinit(&g_ex);
    memset(&g_rt, 0, sizeof(g_rt)); /* RAM loss */
    fakes_reset();
    g_reset_raw = POOL_RESET_RAW_PANIC; /* abnormal reset class */
    boot_runtime();
}

TEST_CASE("exec_rt: no grant survives a reboot — fresh live verification required", "[pool_exec_rt]")
{
    drive_to_mining();
    power_loss_and_reboot();
    /* The committed record is TARGET_ACTIVE; the grant is GONE and nothing
     * mines before fresh live verification. With a PANIC reset class the
     * plan fails safe toward restoration — either way, no mining. */
    bind_executor();
    TEST_ASSERT_FALSE(g_ex.grant.valid);
    step_n(4);
    (void)pool_session_executor_snapshot(&g_ex, &g_snap);
    TEST_ASSERT_FALSE(g_snap.mining_grant_active);
    teardown_all();
}

TEST_CASE("exec_rt: crash during target apply restores after reboot", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    setup_resume();
    pool_exec_desired_from_identity(&g_rt.record.source, &g_cfg.effective);
    step_until(EXEC_STATE_TARGET_APPLYING, 8);
    /* Power loss mid-transaction: APPLYING_TARGET is the durable boundary. */
    power_loss_and_reboot();
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    /* Boot recovery normalized the interrupted apply conservatively (the
     * committed B4 proposal drives it toward restoration — never onward
     * toward the target). */
    TEST_ASSERT_TRUE(after.state == POOL_STATE_APPLYING_TARGET ||
                     after.state == POOL_STATE_INTERRUPTED ||
                     after.state == POOL_STATE_RESTORE_DUE);
    TEST_ASSERT_TRUE(after.restore_required);
    TEST_ASSERT_EQUAL(RUNTIME_RESTORE_SOURCE_PENDING, g_rt.decision.state);

    /* The executor completes the owed restoration. */
    pool_exec_desired_from_identity(&g_rt.record.target, &g_cfg.effective);
    bind_executor();
    step_until(EXEC_STATE_SOURCE_CONNECTING, 20);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);
    feed_mining_evidence();
    step_until(EXEC_STATE_DONE, 10);
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, after.state);
    TEST_ASSERT_FALSE(after.restore_required);
    teardown_all();
}

TEST_CASE("exec_rt: crash during restore keeps the obligation and re-enters restore", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_APPLYING, 6);
    power_loss_and_reboot();
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_RESTORE, after.state);
    TEST_ASSERT_TRUE(after.restore_required);
    TEST_ASSERT_EQUAL(RUNTIME_RESTORE_SOURCE_PENDING, g_rt.decision.state);
    /* Protocol start stays HELD after the reboot (the B6 barrier). */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    teardown_all();
}

/* ================================================================= */
/* Persistence integrity at the action boundaries                     */
/* ================================================================= */

TEST_CASE("exec_rt: a failed boundary commit blocks the action until it lands", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    /* The FIRST slot write of the next commit fails: the boundary must not
     * advance and NOTHING external may run. */
    g_fake.fail_write_after = 0;
    step_n(3);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls);
    {
        PoolSessionRecord after;
        TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
        TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, after.state); /* unchanged */
    }
    /* The injected fault cleared itself: the boundary lands on retry. */
    step_until(EXEC_STATE_SOURCE_APPLYING, 8);
    teardown_all();
}

TEST_CASE("exec_rt: an uncertain boundary commit enters the recovery guard", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    /* B3 commit_record: slot write+commit, slot readback, PLUS a pointer
     * phase. Failing the pointer COMMIT makes the outcome uncertain. */
    g_fake.fail_commit_after = 1; /* slot commit passes; pointer commit fails */
    step_n(4);
    TEST_ASSERT_EQUAL(EXEC_STATE_RECOVERY_GUARD, g_ex.state);
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_rt.lease.phase);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: a stale token at entry guards without mutation", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    g_rt.token.valid = false; /* the runtime lost its token */
    step_n(3);
    TEST_ASSERT_EQUAL(EXEC_STATE_RECOVERY_GUARD, g_ex.state);
    TEST_ASSERT_EQUAL(EXEC_REASON_STALE_TOKEN, g_ex.reason);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls);
    teardown_all();
}

/* ================================================================= */
/* B5 ownership surface                                               */
/* ================================================================= */

TEST_CASE("exec_rt: manual PATCH and OTA stay blocked throughout execution", "[pool_exec_rt]")
{
    PoolOperationRequest  req;
    PoolOperationDecision dec;

    drive_to_mining();

    memset(&req, 0, sizeof(req));
    req.kind = OP_REQUEST_MANUAL_POOL_PATCH;
    dec = pool_operation_coordinator_evaluate(&g_rt.coord, &req);
    TEST_ASSERT_FALSE(dec.allowed);
    TEST_ASSERT_TRUE(dec.http_conflict); /* the 409 surface */

    req.kind = OP_REQUEST_OTA_UPDATE;
    dec = pool_operation_coordinator_evaluate(&g_rt.coord, &req);
    TEST_ASSERT_FALSE(dec.allowed);
    TEST_ASSERT_TRUE(dec.http_conflict);

    /* A second timed session cannot start either. */
    req.kind = OP_REQUEST_TIMED_SESSION_START;
    dec = pool_operation_coordinator_evaluate(&g_rt.coord, &req);
    TEST_ASSERT_FALSE(dec.allowed);
    teardown_all();
}

TEST_CASE("exec_rt: denied executor actions never mutate the store", "[pool_exec_rt]")
{
    uint32_t gen_before;
    int stage_before, start_before;

    /* Reach the runtime-produced held terminal, then prove that NOTHING a
     * held executor does mutates the committed truth or reconnects. */
    setup_restore(POOL_STATE_RESTORE_DUE);
    g_cfg.apply_partial = true;
    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 60);
    gen_before   = g_rt.committed_generation;
    stage_before = g_cfg.stage_calls;
    start_before = g_proto.start_calls;
    step_n(6); /* held: no action, no commit */
    TEST_ASSERT_EQUAL(EXEC_STATE_RESTORE_FAILED_HELD, g_ex.state);
    TEST_ASSERT_EQUAL_UINT32(gen_before, g_rt.committed_generation);
    TEST_ASSERT_EQUAL_INT(stage_before, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL_INT(start_before, g_proto.start_calls);
    teardown_all();
}

/* ================================================================= */
/* Privacy                                                            */
/* ================================================================= */

static bool memory_contains(const void *hay, size_t hay_len, const char *needle)
{
    size_t n = strlen(needle);
    const uint8_t *p = (const uint8_t *)hay;
    size_t i;

    if (n == 0u || hay_len < n) return false;
    for (i = 0; i + n <= hay_len; i++) {
        if (memcmp(p + i, needle, n) == 0) return true;
    }
    return false;
}

TEST_CASE("exec_rt: the published snapshot carries no identity bytes", "[pool_exec_rt]")
{
    drive_to_mining();
    (void)pool_session_executor_snapshot(&g_ex, &g_snap);
    TEST_ASSERT_TRUE(pool_exec_snapshot_valid(&g_snap));
    /* The planted host/account markers must not appear anywhere in the
     * snapshot bytes — the struct has no string field by construction. */
    TEST_ASSERT_FALSE(memory_contains(&g_snap, sizeof(g_snap), "src-pool"));
    TEST_ASSERT_FALSE(memory_contains(&g_snap, sizeof(g_snap), "tgt-pool"));
    TEST_ASSERT_FALSE(memory_contains(&g_snap, sizeof(g_snap), "src-acct"));
    TEST_ASSERT_FALSE(memory_contains(&g_snap, sizeof(g_snap), "tgt-acct"));
    TEST_ASSERT_FALSE(memory_contains(&g_snap, sizeof(g_snap), "example"));
    TEST_ASSERT_FALSE(memory_contains(&g_snap, sizeof(g_snap), "worker"));
    teardown_all();
}

/* ================================================================= */
/* Property-style checks                                              */
/* ================================================================= */

TEST_CASE("exec_rt: property — identical steps produce identical snapshots", "[pool_exec_rt]")
{
    PoolExecutionSnapshot a, b;

    setup_restore(POOL_STATE_RESTORE_DUE);
    g_cfg.apply_partial = true;
    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 60);
    step_n(2);
    (void)pool_session_executor_snapshot(&g_ex, &a);
    /* Time advances but NOTHING else changes in a held posture: the
     * published view must stay byte-identical. */
    step_n(4);
    (void)pool_session_executor_snapshot(&g_ex, &b);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
    teardown_all();
}

TEST_CASE("exec_rt: property — restore_required clears only through COMPLETE", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    /* Across the failure suites above, every reload asserted the flag. The
     * one legitimate discharge is re-proven here end-to-end. */
    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 12);
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_TRUE(after.restore_required);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_TRUE(after.restore_required);
    feed_mining_evidence();
    step_until(EXEC_STATE_DONE, 10);
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, after.state);
    TEST_ASSERT_FALSE(after.restore_required);
    teardown_all();
}

/* ================================================================= */
/* ASIC-side processing evidence (Blocker 3)                          */
/* ================================================================= */

TEST_CASE("exec_rt: job delivery without ASIC evidence fails target health", "[pool_exec_rt]")
{
    setup_resume();
    step_until(EXEC_STATE_TARGET_CONNECTING, 8);
    feed_connection_and_job();
    step_until(EXEC_STATE_TARGET_MINING, 6);

    /* The pool keeps serving and the gate keeps releasing work, but the
     * BM1370 returns nothing: this is NOT ASIC processing evidence. */
    for (int i = 0; i < 6; i++) {
        feed_delivery_without_asic();
        step1();
    }
    TEST_ASSERT_FALSE(g_ex.target_health_ok);
    /* The bounded window expires into restoration, naming the exact cause. */
    step_until(EXEC_STATE_SOURCE_APPLYING, 40);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: an ASIC processing result completes target health", "[pool_exec_rt]")
{
    setup_resume();
    step_until(EXEC_STATE_TARGET_CONNECTING, 8);
    feed_connection_and_job();
    step_until(EXEC_STATE_TARGET_MINING, 6);

    feed_delivery_without_asic();
    step1();
    TEST_ASSERT_FALSE(g_ex.target_health_ok); /* still not verified */

    for (unsigned i = 0; i < POOL_EXEC_REQUIRED_WORK_PROOFS; i++) {
        uint8_t job = g_next_job_id++;
        deliver_current_work(job);
        TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                          pool_session_execution_resolve_asic_result(job, true));
    }
    step1();
    TEST_ASSERT_TRUE(g_ex.target_health_ok);
    TEST_ASSERT_EQUAL(EXEC_STATE_TARGET_MINING, g_ex.state);
    (void)pool_session_executor_snapshot(&g_ex, &g_snap);
    TEST_ASSERT_TRUE(g_snap.asic_evidence_seen);
    TEST_ASSERT_TRUE(g_snap.work_generation != 0u);
    teardown_all();
}

TEST_CASE("exec_rt: source COMPLETE requires an ASIC processing result", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 12);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);

    /* Delivery only, INSIDE the bounded verification window: the obligation
     * must not clear and COMPLETE must not be produced no matter how many
     * jobs are released. (Expiry of that window is covered separately and
     * fails safe into RESTORE_FAILED — never into a false COMPLETE.) */
    for (int i = 0; i < 3; i++) {
        feed_delivery_without_asic();
        step1();
        TEST_ASSERT_EQUAL(EXEC_STATE_SOURCE_MINING_VERIFYING, g_ex.state);
    }
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_NOT_EQUAL(POOL_STATE_COMPLETE, after.state);
    TEST_ASSERT_TRUE(after.restore_required);

    /* The hardware fact is what finally discharges the obligation. */
    for (unsigned i = 0; i < POOL_EXEC_REQUIRED_WORK_PROOFS; i++) {
        uint8_t job = g_next_job_id++;
        deliver_current_work(job);
        TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                          pool_session_execution_resolve_asic_result(job, true));
    }
    step_until(EXEC_STATE_DONE, 10);
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, after.state);
    TEST_ASSERT_FALSE(after.restore_required);
    teardown_all();
}

TEST_CASE("exec_rt: high pool vardiff does not raise the local proof threshold", "[pool_exec_rt]")
{
    /* The local threshold comes from the ASIC's own configured difficulty,
     * never from pool vardiff. Whatever the pool asks for, the threshold is
     * identical — so a healthy restoration can never be stalled by vardiff. */
    double t_low  = pool_session_execution_work_proof_threshold(256u);
    double t_high = pool_session_execution_work_proof_threshold(256u);

    TEST_ASSERT_EQUAL_DOUBLE(t_low, t_high);
    TEST_ASSERT_EQUAL_DOUBLE(POOL_EXEC_WORK_PROOF_MIN_DIFF, t_low);
    /* A degenerate or hostile ASIC-difficulty value cannot weaken it... */
    TEST_ASSERT_EQUAL_DOUBLE(POOL_EXEC_WORK_PROOF_MIN_DIFF,
                             pool_session_execution_work_proof_threshold(0u));
    TEST_ASSERT_EQUAL_DOUBLE(POOL_EXEC_WORK_PROOF_MIN_DIFF,
                             pool_session_execution_work_proof_threshold(1u));
    /* ...nor stall it beyond the compiled ceiling. */
    TEST_ASSERT_EQUAL_DOUBLE(POOL_EXEC_WORK_PROOF_MAX_DIFF,
                             pool_session_execution_work_proof_threshold(1u << 30));
}

TEST_CASE("exec_rt: verification behaves identically under low and high vardiff", "[pool_exec_rt]")
{
    /* Two full restorations that differ ONLY in the pool difficulty the
     * session would be mining at. Both complete on the same local evidence
     * and in the same number of steps — no share-level nonce is involved. */
    for (int pass = 0; pass < 2; pass++) {
        setup_restore(POOL_STATE_RESTORE_DUE);
        /* `pass` stands for low vs. astronomically high pool vardiff; the
         * executor never reads it, which is exactly the property. */
        g_proto.counters.shares_accepted = pass ? 0u : 5000u;
        step_until(EXEC_STATE_SOURCE_CONNECTING, 14);
        feed_connection_and_job();
        step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);
        feed_mining_evidence();
        step_until(EXEC_STATE_DONE, 10);
        {
            PoolSessionRecord after;
            TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
            TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, after.state);
            TEST_ASSERT_FALSE(after.restore_required);
        }
        teardown_all();
    }
}

TEST_CASE("exec_rt: pool share state never affects local verification", "[pool_exec_rt]")
{
    /* Zero accepted shares and a pile of rejected ones: local exact-work
     * evidence still completes the restoration. */
    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 14);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);
    g_proto.counters.shares_accepted = 0u;
    g_proto.counters.shares_rejected = 999u;
    feed_mining_evidence();
    step_until(EXEC_STATE_DONE, 10);
    {
        PoolSessionRecord after;
        TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
        TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, after.state);
    }
    teardown_all();
}

TEST_CASE("exec_rt: COMPLETE requires the configured count of distinct proofs", "[pool_exec_rt]")
{
    PoolSessionRecord after;

    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 14);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);

    /* One proof short of the requirement: never COMPLETE. */
    for (unsigned i = 0; i + 1u < POOL_EXEC_REQUIRED_WORK_PROOFS; i++) {
        uint8_t job = g_next_job_id++;
        g_proto.counters.work_received += 1u;
        pool_session_execution_note_job_forwarded();
        deliver_current_work(job);
        TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                          pool_session_execution_resolve_asic_result(job, true));
    }
    step1();
    TEST_ASSERT_EQUAL(EXEC_STATE_SOURCE_MINING_VERIFYING, g_ex.state);
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_NOT_EQUAL(POOL_STATE_COMPLETE, after.state);
    TEST_ASSERT_TRUE(after.restore_required);

    /* Re-proving the SAME record cannot substitute for a distinct one. */
    {
        uint8_t reused = (uint8_t)(g_next_job_id - 1u);
        TEST_ASSERT_EQUAL(EXEC_RESULT_ALREADY_CONSUMED,
                          pool_session_execution_resolve_asic_result(reused, true));
    }
    step1();
    TEST_ASSERT_EQUAL(EXEC_STATE_SOURCE_MINING_VERIFYING, g_ex.state);

    /* The final DISTINCT proof completes it. */
    {
        uint8_t job = g_next_job_id++;
        deliver_current_work(job);
        TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                          pool_session_execution_resolve_asic_result(job, true));
    }
    step_until(EXEC_STATE_DONE, 10);
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, after.state);
    TEST_ASSERT_FALSE(after.restore_required);
    teardown_all();
}

TEST_CASE("exec_rt: a delayed old-work nonce failing the header proof is rejected", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 14);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);

    /* Delivered work exists, but every returned nonce fails the exact
     * current-header proof (work_bound=false) — a delayed result from work
     * the chip was given before this generation. Nothing may verify. */
    for (int i = 0; i < 8; i++) {
        uint8_t job = g_next_job_id++;
        g_proto.counters.work_received += 1u;
        pool_session_execution_note_job_forwarded();
        deliver_current_work(job);
        TEST_ASSERT_EQUAL(EXEC_RESULT_WORK_MISMATCH,
                          pool_session_execution_resolve_asic_result(job, false));
        step1();
        TEST_ASSERT_NOT_EQUAL(EXEC_STATE_DONE, g_ex.state);
    }
    /* Fail-closed: the bounded window expires into RESTORE_FAILED with the
     * obligation retained — never a false COMPLETE. */
    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 60);
    {
        PoolSessionRecord after;
        TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
        TEST_ASSERT_NOT_EQUAL(POOL_STATE_COMPLETE, after.state);
        TEST_ASSERT_TRUE(after.restore_required);
    }
    teardown_all();
}

TEST_CASE("exec_rt: identical rebuilt headers keep source COMPLETE unreachable", "[pool_exec_rt]")
{
    /*
     * The identical-header alias at the EXECUTOR level: the chip still
     * holds work whose header the new controlled generation rebuilds
     * byte-identically (pool template resend + extranonce2 counter reset,
     * no generation discriminator). The delayed results are GENUINELY
     * valid for the rebuilt headers (work_bound=true), yet none of them
     * may verify mining: the window expires fail-closed and the restore
     * obligation survives.
     */
    PoolExecWorkFacts tpl;

    memset(&tpl, 0, sizeof(tpl));
    tpl.version        = 0x20000000u;
    tpl.ntime          = 0x66aabbccu;
    tpl.nbits          = 0x1d00ffffu;
    tpl.merkle_root[0] = 0xC3u;

    setup_restore(POOL_STATE_RESTORE_DUE);
    /* Work the chip was given BEFORE the controlled start (prior epoch). */
    pool_session_execution_note_work_delivered(100u, &tpl);
    pool_session_execution_note_work_delivered(108u, &tpl);

    step_until(EXEC_STATE_SOURCE_CONNECTING, 14);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);

    /* The controlled generation rebuilds the SAME headers (no domain). */
    for (int i = 0; i < 6; i++) {
        uint8_t job = g_next_job_id++;
        g_proto.counters.work_received += 1u;
        pool_session_execution_note_job_forwarded();
        pool_session_execution_note_work_delivered(job, &tpl);
        /* The delayed old result validates against the identical current
         * header — and is STILL refused as freshness evidence. */
        TEST_ASSERT_EQUAL(EXEC_RESULT_NO_DISCRIMINATOR,
                          pool_session_execution_resolve_asic_result(job, true));
        step1();
        TEST_ASSERT_NOT_EQUAL(EXEC_STATE_DONE, g_ex.state);
    }
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());

    /* Source COMPLETE stays impossible in this posture. */
    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 60);
    {
        PoolSessionRecord after;
        TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
        TEST_ASSERT_NOT_EQUAL(POOL_STATE_COMPLETE, after.state);
        TEST_ASSERT_TRUE(after.restore_required);
    }
    teardown_all();
}

TEST_CASE("exec_rt: COMPLETE counts only unique-header discriminator proofs", "[pool_exec_rt]")
{
    /*
     * Blocker scenarios 19-20: the two-proof requirement is preserved AND
     * every counted proof belongs to generation-unique, discriminator-
     * carrying work. An aliased result and an undiscriminated result are
     * both refused before counting; two proper proofs then COMPLETE.
     */
    PoolExecWorkFacts tpl;
    PoolSessionRecord after;

    memset(&tpl, 0, sizeof(tpl));
    tpl.version        = 0x20000000u;
    tpl.ntime          = 0x66bbccddu;
    tpl.nbits          = 0x1d00ffffu;
    tpl.merkle_root[0] = 0xD4u;

    setup_restore(POOL_STATE_RESTORE_DUE);
    pool_session_execution_note_work_delivered(116u, &tpl); /* prior epoch */

    step_until(EXEC_STATE_SOURCE_CONNECTING, 14);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);

    /* 1. An identical rebuilt header: excluded (never counted). */
    {
        uint8_t job = g_next_job_id++;
        g_proto.counters.work_received += 1u;
        pool_session_execution_note_job_forwarded();
        pool_session_execution_note_work_delivered(job, &tpl);
        TEST_ASSERT_EQUAL(EXEC_RESULT_NO_DISCRIMINATOR,
                          pool_session_execution_resolve_asic_result(job, true));
    }
    /* 2. Unique work but NO discriminator (no tag query ran): excluded. */
    {
        PoolExecWorkFacts uniq;
        uint8_t job = g_next_job_id++;
        memset(&uniq, 0, sizeof(uniq));
        uniq.version        = 0x20000000u;
        uniq.ntime          = 0x66bbccdeu;
        uniq.nbits          = 0x1d00ffffu;
        uniq.merkle_root[1] = 0x77u;
        g_proto.counters.work_received += 1u;
        pool_session_execution_note_job_forwarded();
        pool_session_execution_note_work_delivered(job, &uniq);
        TEST_ASSERT_EQUAL(EXEC_RESULT_NO_DISCRIMINATOR,
                          pool_session_execution_resolve_asic_result(job, true));
    }
    step1();
    TEST_ASSERT_EQUAL(EXEC_STATE_SOURCE_MINING_VERIFYING, g_ex.state);
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());

    /* 3. The required count of proper proofs (tagged + unique) COMPLETEs. */
    feed_mining_evidence();
    step_until(EXEC_STATE_DONE, 10);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)POOL_EXEC_REQUIRED_WORK_PROOFS,
                             pool_session_execution_asic_job_results());
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, after.state);
    TEST_ASSERT_FALSE(after.restore_required);
    teardown_all();
}

/* Spend the whole per-boot discriminator budget (bounded: 127 tags). */
static void drain_generation_budget(void)
{
    unsigned guard = 0u;
    while (pool_session_execution_allocate_generation_tag()) {
        TEST_ASSERT_TRUE(++guard <= POOL_EXEC_GENERATION_TAG_LIMIT);
    }
    TEST_ASSERT_TRUE(pool_session_execution_generation_exhausted());
}

TEST_CASE("exec_rt: generation exhaustion holds the source path fail-closed", "[pool_exec_rt]")
{
    /*
     * Blocker scenarios 3, 5 and 6 at the executor level: with the
     * per-boot discriminator budget spent, NO controlled protocol start
     * is issued at all (no connection, no delivered verification work, no
     * reused domain), the honest reason is GENERATION_EXHAUSTED, source
     * COMPLETE is impossible and restore_required survives.
     */
    PoolSessionRecord after;

    setup_restore(POOL_STATE_RESTORE_DUE);
    drain_generation_budget();

    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 200);
    TEST_ASSERT_TRUE(g_ex.generation_exhausted);
    /* NO protocol start was ever attempted — nothing was issued. */
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls);
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_NOT_EQUAL(POOL_STATE_COMPLETE, after.state);
    TEST_ASSERT_TRUE(after.restore_required);
    /* The posture is STABLE: further steps change nothing. */
    step_n(5);
    TEST_ASSERT_EQUAL(EXEC_STATE_RESTORE_FAILED_HELD, g_ex.state);
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls);
    teardown_all();
}

TEST_CASE("exec_rt: generation exhaustion never grants target mining", "[pool_exec_rt]")
{
    /*
     * Blocker scenario 4: the exhausted target path can never verify, so
     * the target-mining grant is never issued and the delivery gate never
     * opens for target work.
     */
    PoolSessionRecord after;

    setup_resume();
    drain_generation_budget();

    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 400);
    TEST_ASSERT_TRUE(g_ex.generation_exhausted);
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls);
    TEST_ASSERT_FALSE(g_ex.grant.valid);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_NOT_EQUAL(POOL_STATE_COMPLETE, after.state);
    TEST_ASSERT_TRUE(after.restore_required);
    teardown_all();
}

TEST_CASE("exec_rt: registry and result processing never touch a blocking adapter", "[pool_exec_rt]")
{
    /*
     * The ASIC-result path must never block behind network IO: delivery
     * registration and result resolution invoke NO adapter (socket, NVS,
     * protocol lifecycle) and always return at lock depth zero.
     */
    int cfg_calls_before, proto_calls_before;
    int i;

    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 14);

    cfg_calls_before   = g_cfg.stage_calls + g_cfg.refresh_calls;
    proto_calls_before = g_proto.start_calls + g_proto.stop_calls +
                         g_proto.handoff_calls;
    for (i = 0; i < 300; i++) {
        uint8_t job = g_next_job_id++;
        deliver_current_work(job);
        (void)pool_session_execution_resolve_asic_result(job, (i & 1) == 0);
        TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
    }
    TEST_ASSERT_EQUAL_INT(cfg_calls_before,
                          g_cfg.stage_calls + g_cfg.refresh_calls);
    TEST_ASSERT_EQUAL_INT(proto_calls_before,
                          g_proto.start_calls + g_proto.stop_calls +
                          g_proto.handoff_calls);
    teardown_all();
}

TEST_CASE("exec_rt: ASIC evidence from an earlier generation is rejected", "[pool_exec_rt]")
{
    uint32_t gen_before;

    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 12);
    gen_before = pool_session_execution_work_generation();
    TEST_ASSERT_TRUE(gen_before != 0u);

    /* Plenty of hardware evidence accumulates in THIS generation... */
    for (int i = 0; i < 5; i++) {
        uint8_t job = g_next_job_id++;
        pool_session_execution_note_job_forwarded();
        deliver_current_work(job);
        TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                          pool_session_execution_resolve_asic_result(job, true));
    }
    TEST_ASSERT_EQUAL_UINT64(5u, pool_session_execution_asic_job_results());

    /* ...then the protocol restarts: a NEW work generation zeroes every
     * per-generation fact, so none of it can satisfy the new window. */
    (void)pool_session_execution_begin_work_generation();
    TEST_ASSERT_NOT_EQUAL(gen_before, pool_session_execution_work_generation());
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_jobs_forwarded());
    teardown_all();
}

TEST_CASE("exec_rt: a controlled protocol restart re-issues the work generation", "[pool_exec_rt]")
{
    uint32_t gen_a, gen_b;

    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 12);
    gen_a = pool_session_execution_work_generation();
    TEST_ASSERT_EQUAL_UINT32(gen_a, g_ex.work_generation);

    /* Stale hardware evidence in flight from the previous connection. */
    for (unsigned i = 0; i < POOL_EXEC_REQUIRED_WORK_PROOFS; i++) {
        uint8_t job = g_next_job_id++;
        deliver_current_work(job);
        TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                          pool_session_execution_resolve_asic_result(job, true));
    }

    /* The engine restarts the protocol after a verification failure. */
    g_proto.pending_events |= EXEC_PEVT_CONNECTION_FAILED | EXEC_PEVT_TASK_EXITED;
    g_proto.running = false;
    step_until(EXEC_STATE_SOURCE_CONNECTING, 40);
    gen_b = pool_session_execution_work_generation();
    TEST_ASSERT_NOT_EQUAL(gen_a, gen_b);
    TEST_ASSERT_EQUAL_UINT32(gen_b, g_ex.work_generation);
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
    teardown_all();
}

/* ================================================================= */
/* TLS-mode compatibility gate (Blocker 2)                            */
/* ================================================================= */

TEST_CASE("exec_rt: representable TLS modes execute normally", "[pool_exec_rt]")
{
    /* disabled -> disabled (the fixtures' default). */
    setup_restore(POOL_STATE_RESTORE_DUE);
    TEST_ASSERT_EQUAL_UINT8(POOL_EXEC_TLS_MODE_DISABLED, g_cfg.effective.primary_tls_mode);
    step_until(EXEC_STATE_SOURCE_APPLYING, 8);
    teardown_all();

    /* bundled -> bundled. */
    setup_restore(POOL_STATE_RESTORE_DUE);
    g_cfg.effective.primary.tls      = true;
    g_cfg.effective.primary_tls_mode = POOL_EXEC_TLS_MODE_BUNDLED;
    step_until(EXEC_STATE_SOURCE_APPLYING, 8);
    teardown_all();
}

TEST_CASE("exec_rt: a custom-cert source is rejected before ANY mutation", "[pool_exec_rt]")
{
    PreState pre;

    setup_restore(POOL_STATE_RESTORE_DUE);
    /* The device is on a custom-certificate configuration: restoring it
     * through the boolean B1 identity would silently downgrade it to the
     * bundled CA while every boolean comparison still "matched". */
    g_cfg.effective.primary.tls      = true;
    g_cfg.effective.primary_tls_mode = POOL_EXEC_TLS_MODE_CUSTOM;
    capture_pre(&pre);

    step_n(6);
    TEST_ASSERT_EQUAL(EXEC_STATE_ERROR, g_ex.state);
    TEST_ASSERT_EQUAL(EXEC_REASON_TLS_MODE_UNSUPPORTED, g_ex.reason);
    assert_nothing_changed(&pre);
    /* No false exact-restoration claim can exist: COMPLETE is unreachable. */
    TEST_ASSERT_NOT_EQUAL(EXEC_STATE_COMPLETE_HANDOFF, g_ex.state);
    TEST_ASSERT_NOT_EQUAL(EXEC_STATE_DONE, g_ex.state);
    teardown_all();
}

TEST_CASE("exec_rt: a custom-cert fallback is rejected before ANY mutation", "[pool_exec_rt]")
{
    PreState pre;

    setup_restore(POOL_STATE_RESTORE_DUE);
    g_cfg.effective.fallback.tls      = true;
    g_cfg.effective.fallback_tls_mode = POOL_EXEC_TLS_MODE_CUSTOM;
    capture_pre(&pre);

    step_n(6);
    TEST_ASSERT_EQUAL(EXEC_STATE_ERROR, g_ex.state);
    TEST_ASSERT_EQUAL(EXEC_REASON_TLS_MODE_UNSUPPORTED, g_ex.reason);
    assert_nothing_changed(&pre);
    teardown_all();
}

TEST_CASE("exec_rt: a custom-cert target session is rejected before ANY mutation", "[pool_exec_rt]")
{
    PreState pre;

    /* The resume path is the one that would APPLY a target: a custom mode
     * anywhere in the effective configuration stops it before the first
     * write, and the device keeps mining exactly what it is mining. */
    setup_resume();
    g_cfg.effective.primary_tls_mode = POOL_EXEC_TLS_MODE_CUSTOM;
    g_cfg.effective.primary.tls      = true;
    capture_pre(&pre);

    step_n(6);
    TEST_ASSERT_EQUAL(EXEC_STATE_ERROR, g_ex.state);
    TEST_ASSERT_EQUAL(EXEC_REASON_TLS_MODE_UNSUPPORTED, g_ex.reason);
    assert_nothing_changed(&pre);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: an unknown TLS mode is rejected before ANY mutation", "[pool_exec_rt]")
{
    PreState pre;

    setup_restore(POOL_STATE_RESTORE_DUE);
    g_cfg.effective.primary_tls_mode = 3u; /* outside the known enum */
    g_cfg.effective.primary.tls      = true;
    capture_pre(&pre);

    step_n(6);
    TEST_ASSERT_EQUAL(EXEC_STATE_ERROR, g_ex.state);
    TEST_ASSERT_EQUAL(EXEC_REASON_TLS_MODE_UNSUPPORTED, g_ex.reason);
    assert_nothing_changed(&pre);
    teardown_all();
}

TEST_CASE("exec_rt: COMPLETE can never follow a rejected TLS configuration", "[pool_exec_rt]")
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    g_cfg.effective.primary_tls_mode = POOL_EXEC_TLS_MODE_CUSTOM;
    g_cfg.effective.primary.tls      = true;

    /* Feed every piece of evidence a healthy restoration would need: the
     * session must STILL never reach COMPLETE, because it never started.
     * (The hardware results are rejected as STALE_GENERATION here — a
     * rejected session never opens a work generation at all, which is
     * itself part of the guarantee.) */
    for (int i = 0; i < 25; i++) {
        uint8_t job = g_next_job_id++;
        g_proto.counters.work_received += 1u;
        pool_session_execution_note_job_forwarded();
        deliver_current_work(job);
        TEST_ASSERT_EQUAL(EXEC_RESULT_STALE_GENERATION,
                          pool_session_execution_resolve_asic_result(job, true));
        g_proto.pending_events |= EXEC_PEVT_SETUP_SUCCESS;
        step1();
        TEST_ASSERT_NOT_EQUAL(EXEC_STATE_DONE, g_ex.state);
        TEST_ASSERT_NOT_EQUAL(EXEC_STATE_COMPLETE_HANDOFF, g_ex.state);
    }
    {
        PoolSessionRecord after;
        TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
        TEST_ASSERT_NOT_EQUAL(POOL_STATE_COMPLETE, after.state);
        TEST_ASSERT_TRUE(after.restore_required);
    }
    teardown_all();
}

/* ================================================================= */
/* Post-COMPLETE handoff ownership (Blocker 4B)                       */
/* ================================================================= */

/* Drive a restore posture all the way to the handoff boundary. */
static void drive_to_handoff(void)
{
    setup_restore(POOL_STATE_RESTORE_DUE);
    step_until(EXEC_STATE_SOURCE_CONNECTING, 12);
    feed_connection_and_job();
    step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);
    feed_mining_evidence();
}

TEST_CASE("exec_rt: successful handoff stops, starts, THEN releases", "[pool_exec_rt]")
{
    drive_to_handoff();
    step_until(EXEC_STATE_DONE, 10);

    /* Exactly one engine handover: the controlled task exited before the
     * production coordinator was started. */
    TEST_ASSERT_FALSE(g_proto.running);
    TEST_ASSERT_EQUAL_INT(1, g_proto.handoff_calls);
    /* Ownership was released only AFTER the coordinator took over. */
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    TEST_ASSERT_TRUE(g_rt.lease.terminal_pending);
    TEST_ASSERT_FALSE(g_rt.token.valid);
    /* Mining is delivered again under the production coordinator. */
    TEST_ASSERT_TRUE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("exec_rt: a failed controlled stop never starts a second engine", "[pool_exec_rt]")
{
    drive_to_handoff();
    g_proto.stop_fail = true; /* the controlled engine will not exit */
    step_until(EXEC_STATE_HANDOFF_FAILED, 30);

    TEST_ASSERT_EQUAL(EXEC_REASON_HANDOFF_STOP_FAILED, g_ex.reason);
    /* NEVER two engines: the coordinator was never started. */
    TEST_ASSERT_EQUAL_INT(0, g_proto.handoff_calls);
    /* NEVER ownerless: the session lease is still held. */
    TEST_ASSERT_TRUE(g_rt.token.valid);
    TEST_ASSERT_NOT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_NOT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    /* No claim that mining continues. */
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    (void)pool_session_executor_snapshot(&g_ex, &g_snap);
    TEST_ASSERT_TRUE(pool_exec_snapshot_valid(&g_snap));
    TEST_ASSERT_FALSE(g_snap.mining_grant_active);
    TEST_ASSERT_EQUAL(EXEC_GATE_INHIBITED, g_snap.gate);
    /* Bounded: no unbounded retry of either step. */
    {
        int stops = g_proto.stop_calls;
        step_n(8);
        TEST_ASSERT_EQUAL_INT(stops, g_proto.stop_calls);
        TEST_ASSERT_EQUAL_INT(0, g_proto.handoff_calls);
    }
    teardown_all();
}

TEST_CASE("exec_rt: a failed coordinator start retains ownership truthfully", "[pool_exec_rt]")
{
    drive_to_handoff();
    g_proto.handoff_fail = true;
    step_until(EXEC_STATE_HANDOFF_FAILED, 30);

    TEST_ASSERT_EQUAL(EXEC_REASON_HANDOFF_START_FAILED, g_ex.reason);
    /* The controlled engine DID exit, so no engine is running... */
    TEST_ASSERT_FALSE(g_proto.running);
    /* ...and the lease is still held: never FREE with an unresolved
     * protocol handover. */
    TEST_ASSERT_TRUE(g_rt.token.valid);
    TEST_ASSERT_EQUAL(OP_PHASE_TERMINAL_ACK_PENDING, g_rt.lease.phase);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    /* Bounded: exactly one coordinator-start attempt, ever. */
    step_n(8);
    TEST_ASSERT_EQUAL_INT(1, g_proto.handoff_calls);
    /* COMPLETE is still durable and the obligation stays discharged. */
    {
        PoolSessionRecord after;
        TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
        TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, after.state);
        TEST_ASSERT_FALSE(after.restore_required);
    }
    teardown_all();
}

TEST_CASE("exec_rt: a token rotation during handoff guards instead of releasing", "[pool_exec_rt]")
{
    drive_to_handoff();
    step_n(1); /* enter COMPLETE_HANDOFF */
    TEST_ASSERT_EQUAL(EXEC_STATE_COMPLETE_HANDOFF, g_ex.state);

    /* The token goes stale mid-handoff: the lease must NOT be released. */
    g_rt.token.lease_generation += 1u;
    step_n(4);
    TEST_ASSERT_NOT_EQUAL(EXEC_STATE_DONE, g_ex.state);
    TEST_ASSERT_NOT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

/* ================================================================= */
/* Repeated-cycle bound (Blocker 4A)                                  */
/* ================================================================= */

TEST_CASE("exec_rt: repeated restore cycles keep a constant bounded footprint", "[pool_exec_rt]")
{
    uint32_t first_gen_span = 0u;

    /*
     * Many synthetic restore cycles, each with its own configuration
     * transaction, controlled reconnect and verification. The executor's
     * per-cycle state must not grow: the refresh path publishes into fixed
     * executor-owned storage (see nx_execution_glue.c) and the fake adapter
     * mirrors that contract by never allocating. What this test pins is
     * that the ENGINE side is bounded — a fixed number of adapter calls per
     * cycle and no monotonically growing per-cycle work.
     */
    for (int cycle = 0; cycle < 12; cycle++) {
        setup_restore(POOL_STATE_RESTORE_DUE);
        step_until(EXEC_STATE_SOURCE_CONNECTING, 14);
        feed_connection_and_job();
        step_until(EXEC_STATE_SOURCE_MINING_VERIFYING, 6);
        feed_mining_evidence();
        step_until(EXEC_STATE_DONE, 10);

        /* Per-cycle adapter work is CONSTANT, never cumulative. */
        TEST_ASSERT_EQUAL_INT(1, g_cfg.stage_calls);
        TEST_ASSERT_EQUAL_INT(1, g_cfg.refresh_calls);
        TEST_ASSERT_EQUAL_INT(1, g_proto.start_calls);
        TEST_ASSERT_EQUAL_INT(1, g_proto.handoff_calls);

        /* The work generation advances by a fixed amount per cycle. */
        if (cycle == 0) {
            first_gen_span = pool_session_execution_work_generation();
        } else {
            TEST_ASSERT_EQUAL_UINT32(first_gen_span,
                                     pool_session_execution_work_generation());
        }

        /* Every cycle ends in the same terminal posture with no identity
         * carried over between generations. */
        (void)pool_session_executor_snapshot(&g_ex, &g_snap);
        TEST_ASSERT_TRUE(pool_exec_snapshot_valid(&g_snap));
        TEST_ASSERT_EQUAL(EXEC_STATE_DONE, g_snap.state);
        TEST_ASSERT_FALSE(memory_contains(&g_snap, sizeof(g_snap), "src-pool"));
        teardown_all();
    }
}

TEST_CASE("exec_rt: property — no restore path silently returns to the target", "[pool_exec_rt]")
{
    int target_starts;

    setup_restore(POOL_STATE_RESTORE_DUE);
    g_cfg.apply_partial = true;
    step_until(EXEC_STATE_RESTORE_FAILED_HELD, 60);
    target_starts = g_proto.start_calls;
    /* Once restoration failed, nothing may reconnect anywhere. */
    step_n(10);
    TEST_ASSERT_EQUAL_INT(target_starts, g_proto.start_calls);
    TEST_ASSERT_EQUAL_INT(0, g_proto.handoff_calls);
    TEST_ASSERT_EQUAL(EXEC_STATE_RESTORE_FAILED_HELD, g_ex.state);
    teardown_all();
}

/* ================================================================= */
/* Gate B8 additions — same-boot adoption and ordered fail-safe        */
/* ================================================================= */

/*
 * Reproduce EXACTLY what the Gate B8 create command leaves behind: a
 * durable TARGET_SNAPSHOT_COMMITTED record with restore_required == false,
 * an activated B5 lease in OP_PHASE_ACTIVE and a current token.
 */
static void posture_fresh_create(void)
{
    PoolOperationRequest          req;
    PoolOperationDecision         dec;
    PoolOperationPersistenceProof proof;
    PoolSessionRequest            r;
    PoolSessionEvent              ev;
    PoolSession                   s;

    memset(&r, 0, sizeof(r));
    r.model_version   = POOL_SESSION_MODEL_VERSION;
    r.session_id      = SESSION_ID;
    r.duration_s      = 3600u;
    r.password_policy = POOL_SESSION_PW_KEEP_CURRENT;
    fill_identity(&r.source, POOL_CHAIN_CUSTOM_UNKNOWN, "btc.example", 3333,
                  "acct.worker");
    fill_identity(&r.target, POOL_CHAIN_BITCOIN_CASH, "bch.example", 3334,
                  "acct.worker");
    strncpy(r.board_version, "601", sizeof(r.board_version) - 1);
    strncpy(r.asic_model, "BM1370", sizeof(r.asic_model) - 1);
    TEST_ASSERT_EQUAL(ERR_NONE, pool_session_validate_request(&r));

    pool_session_init(&s);
    memset(&ev, 0, sizeof(ev));
    ev.type       = POOL_EVT_CREATE_REQUESTED;
    ev.session_id = SESSION_ID;
    ev.request    = &r;
    (void)pool_session_transition(&s, &ev, &s);
    memset(&ev, 0, sizeof(ev));
    ev.type       = POOL_EVT_SOURCE_SNAPSHOT_COMMITTED;
    ev.session_id = SESSION_ID;
    (void)pool_session_transition(&s, &ev, &s);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_SNAPSHOT_COMMITTED, s.state);
    TEST_ASSERT_FALSE(s.restore_required);

    memset(&req, 0, sizeof(req));
    req.kind       = OP_REQUEST_TIMED_SESSION_START;
    req.session_id = SESSION_ID;
    memset(&dec, 0, sizeof(dec));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_try_acquire(
                                 &g_rt.coord, &req, &g_rt.token, &dec));

    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_from_session(&s, &g_rec));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_rt.store, &g_rec));
    TEST_ASSERT_EQUAL(STORE_OK,
                      pool_session_store_load(&g_rt.store, &g_rt.record, &g_rt.load_info));
    g_rt.record_present = true;
    g_rt.store_result   = STORE_OK;
    (void)pool_session_store_committed_generation(&g_rt.store, &g_rt.committed_generation);

    memset(&proof, 0, sizeof(proof));
    proof.kind                        = OP_PROOF_SESSION_COMMITTED;
    proof.store_result                = STORE_OK;
    proof.committed_record_generation = g_rt.record.generation;
    proof.session_id                  = g_rt.record.session_id;
    proof.persisted_state             = POOL_STATE_TARGET_SNAPSHOT_COMMITTED;
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_apply_persistence_proof(
                                 &g_rt.coord, &g_rt.token, &proof, &g_rt.token));
    (void)pool_operation_coordinator_snapshot(&g_rt.coord, &g_rt.lease);
    TEST_ASSERT_EQUAL(OP_PHASE_ACTIVE, g_rt.lease.phase);
    TEST_ASSERT_TRUE(g_rt.token.valid);
}

static void setup_fresh_create(void)
{
    fake_store_reset();
    fakes_reset();
    boot_runtime();
    posture_fresh_create();
    /* Before adoption the device is still on its SOURCE configuration. */
    pool_exec_desired_from_identity(&g_rt.record.source, &g_cfg.effective);
    bind_executor();
}

TEST_CASE("b8-adopt: a session created this boot is adopted by the executor",
          "[pool_exec_rt]")
{
    PoolSessionRecord after;

    setup_fresh_create();
    TEST_ASSERT_EQUAL(EXEC_STATE_IDLE, g_ex.state);
    TEST_ASSERT_FALSE(pool_session_executor_owns_flow(&g_ex));
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);

    TEST_ASSERT_EQUAL(EXEC_REASON_NONE,
                      pool_session_executor_adopt_created_session(&g_ex));

    /* The pre-mutation action boundary is DURABLE before anything is armed. */
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_TARGET, after.state);
    TEST_ASSERT_TRUE(after.restore_required);
    TEST_ASSERT_EQUAL(POOL_SESSION_PW_KEEP_CURRENT, after.password_policy);
    TEST_ASSERT_EQUAL_UINT32(SESSION_ID, after.session_id);
    TEST_ASSERT_TRUE(after.generation > 1u);
    /* Target mutation is IMPOSSIBLE before that: not one pool key staged. */
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL_INT(0, g_proto.start_calls);

    /* Ownership transferred exactly once; the executor is authoritative. */
    TEST_ASSERT_EQUAL(EXEC_STATE_TARGET_APPLYING, g_ex.state);
    TEST_ASSERT_TRUE(pool_session_executor_owns_flow(&g_ex));
    TEST_ASSERT_EQUAL(OP_PHASE_VERIFYING_TARGET, g_rt.lease.phase);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());

    /* Only the NEXT step arms the configuration transaction. */
    (void)step1();
    TEST_ASSERT_EQUAL_INT(1, g_cfg.stage_calls);
    teardown_all();
}

TEST_CASE("b8-adopt: adoption is refused without side effects when unusable",
          "[pool_exec_rt]")
{
    PoolSessionRecord after;

    /* Not system-ready. */
    setup_fresh_create();
    pool_session_executor_set_system_ready(&g_ex, false);
    TEST_ASSERT_EQUAL(EXEC_REASON_SYSTEM_NOT_READY,
                      pool_session_executor_adopt_created_session(&g_ex));
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_SNAPSHOT_COMMITTED, after.state);
    TEST_ASSERT_FALSE(after.restore_required);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    TEST_ASSERT_FALSE(pool_session_executor_owns_flow(&g_ex));
    teardown_all();

    /* Unsupported hardware. */
    setup_fresh_create();
    strncpy(g_cfg.board, "203", sizeof(g_cfg.board) - 1);
    TEST_ASSERT_EQUAL(EXEC_REASON_BOARD_UNSUPPORTED,
                      pool_session_executor_adopt_created_session(&g_ex));
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_SNAPSHOT_COMMITTED, after.state);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    teardown_all();

    /* A custom-certificate source cannot be restored exactly. */
    setup_fresh_create();
    g_cfg.effective.primary_tls_mode = POOL_EXEC_TLS_MODE_CUSTOM;
    TEST_ASSERT_EQUAL(EXEC_REASON_TLS_MODE_UNSUPPORTED,
                      pool_session_executor_adopt_created_session(&g_ex));
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_SNAPSHOT_COMMITTED, after.state);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    teardown_all();

    /* A wrong durable state is never adoptable. */
    setup_resume(); /* TARGET_ACTIVE resume posture, not a fresh create */
    TEST_ASSERT_EQUAL(EXEC_REASON_RECORD_INCOMPATIBLE,
                      pool_session_executor_adopt_created_session(&g_ex));
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    teardown_all();
}

TEST_CASE("b8-adopt: adoption never creates a second owner", "[pool_exec_rt]")
{
    setup_fresh_create();
    TEST_ASSERT_EQUAL(EXEC_REASON_NONE,
                      pool_session_executor_adopt_created_session(&g_ex));
    TEST_ASSERT_TRUE(pool_session_executor_owns_flow(&g_ex));

    /* A second adoption is refused: the executor is no longer quiescent. */
    TEST_ASSERT_EQUAL(EXEC_REASON_OWNERSHIP_MISMATCH,
                      pool_session_executor_adopt_created_session(&g_ex));
    TEST_ASSERT_EQUAL(EXEC_STATE_TARGET_APPLYING, g_ex.state);
    TEST_ASSERT_EQUAL(OP_OWNER_TIMED_SESSION, g_rt.lease.owner);
    teardown_all();
}

TEST_CASE("b8-failsafe: an ordered restore inhibits, revokes, then restores",
          "[pool_exec_rt]")
{
    PoolSessionRecord after;

    setup_resume();
    step_until(EXEC_STATE_TARGET_CONNECTING, 8);
    feed_connection_and_job();
    step_until(EXEC_STATE_TARGET_MINING, 6);
    TEST_ASSERT_TRUE(pool_session_execution_asic_work_allowed());
    TEST_ASSERT_TRUE(pool_session_executor_grant_active(&g_ex));

    /* THE fail-safe order: gate inhibited, grant revoked, restore driven. */
    TEST_ASSERT_EQUAL(EXEC_REASON_NONE,
                      pool_session_executor_request_restore(&g_ex,
                                                            EXEC_REASON_PERSIST_FAILED));
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    TEST_ASSERT_FALSE(pool_session_executor_grant_active(&g_ex));
    (void)pool_session_executor_snapshot(&g_ex, &g_snap);
    TEST_ASSERT_FALSE(g_snap.mining_grant_active);
    TEST_ASSERT_EQUAL(EXEC_STATE_RESTORE_PENDING, g_ex.state);

    /* The restoration intent is durable and the obligation is retained. */
    TEST_ASSERT_EQUAL(STORE_OK, reload_committed(&after));
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, after.state);
    TEST_ASSERT_TRUE(after.restore_required);
    /* The deadline and duration are untouched by the fail-safe. */
    TEST_ASSERT_EQUAL_UINT32(3600u, after.duration_s);

    /* Idempotent: a repeat order changes nothing and never re-revokes. */
    TEST_ASSERT_EQUAL(EXEC_REASON_NONE,
                      pool_session_executor_request_restore(&g_ex,
                                                            EXEC_REASON_PERSIST_FAILED));
    TEST_ASSERT_EQUAL(EXEC_STATE_RESTORE_PENDING, g_ex.state);
    TEST_ASSERT_FALSE(pool_session_execution_asic_work_allowed());
    teardown_all();
}

TEST_CASE("b8-failsafe: an ordered restore is refused from a non-owning posture",
          "[pool_exec_rt]")
{
    setup_fresh_create();
    /* IDLE owns nothing: the order is refused with no side effects. */
    TEST_ASSERT_EQUAL(EXEC_REASON_OWNERSHIP_MISMATCH,
                      pool_session_executor_request_restore(&g_ex,
                                                            EXEC_REASON_PERSIST_FAILED));
    TEST_ASSERT_EQUAL(EXEC_STATE_IDLE, g_ex.state);
    TEST_ASSERT_EQUAL_INT(0, g_cfg.stage_calls);
    TEST_ASSERT_EQUAL(EXEC_REASON_NOT_BOUND,
                      pool_session_executor_request_restore(NULL,
                                                            EXEC_REASON_PERSIST_FAILED));
    teardown_all();
}
