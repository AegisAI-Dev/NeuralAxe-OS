/*
 * Deterministic tests for the Gate B8 bounded command mailbox and the
 * owner-task command processor.
 *
 * Every dependency is injected: a fake persistence backend (the B3 model of
 * staged-vs-committed values), fake SNTP platform ops, a fake monotonic
 * clock and a fake source-capture adapter. NOTHING here contacts a network,
 * an NTP server, DNS, a pool, OTA, a restart path or hardware; every
 * identity is a synthetic "*.example" fixture and no password value exists
 * anywhere in the harness.
 */

#include <string.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "pool_session_command.h"
#include "pool_session_api_status.h"
#include "pool_session_runtime.h"
#include "pool_operation_http_policy.h"
#include "pool_session_runtime_admission.h"
#include "pool_session_execution.h"

#define EPOCH_A_S  1750000000ull
#define US_PER_S   1000000ull

/* ---------------- fake persistence backend ---------------- */

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
    int     fail_write_after;
    int     fail_commit_after;
    int     fail_active_read_after;
    int     writes, commits;
} FakeNvs;

static FakeNvs g_fake;

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
    (void)ctx;
    g_fake.opened = true;
    return POOL_STORE_BACKEND_OK;
}

static int fk_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len)
{
    int idx = fk_index(key);
    const FakeVal *v;
    (void)ctx;
    if (idx == FK_P && fk_should_fail(&g_fake.fail_active_read_after)) {
        return POOL_STORE_BACKEND_IO;
    }
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
    int idx = fk_index(key);
    (void)ctx;
    g_fake.writes++;
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
    int i;
    (void)ctx;
    g_fake.commits++;
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
    g_fake.opened = false;
    return POOL_STORE_BACKEND_OK;
}

static const PoolStoreBackendOps g_fake_ops = {
    .open = fk_open, .read_blob = fk_read, .write_blob = fk_write,
    .commit = fk_commit, .close = fk_close,
};

static void fake_reset(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.fail_write_after = -1;
    g_fake.fail_commit_after = -1;
    g_fake.fail_active_read_after = -1;
}

/* ---------------- fake platform ---------------- */

static uint64_t g_monotonic_us;

static int32_t fake_reset_reason(void) { return POOL_RESET_RAW_POWERON; }
static uint64_t fake_monotonic(void)   { return g_monotonic_us; }

static uint64_t fs_monotonic(void) { return g_monotonic_us; }
static int fs_init(const PoolTimeSntpConfig *cfg) { (void)cfg; return 0; }
static int fs_start(void)  { return 0; }
static int fs_stop(void)   { return 0; }
static int fs_deinit(void) { return 0; }

static const PoolTimeSntpPlatformOps g_fake_sntp_ops = {
    .monotonic_us = fs_monotonic, .sntp_init = fs_init, .sntp_start = fs_start,
    .sntp_stop = fs_stop, .sntp_deinit = fs_deinit,
};

/* ---------------- fake source-capture adapter ---------------- */

static PoolExecEffectiveConfig g_effective;
static bool                    g_identity_ok;
static bool                    g_password_retained;
static const char             *g_board;
static const char             *g_asic;
static int                     g_read_effective_calls;

static bool src_device_identity(void *ctx, char *board, size_t bcap,
                                char *asic, size_t acap)
{
    (void)ctx;
    if (!g_identity_ok) return false;
    strncpy(board, g_board, bcap - 1u); board[bcap - 1u] = '\0';
    strncpy(asic, g_asic, acap - 1u);   asic[acap - 1u] = '\0';
    return true;
}

static int g_fail_effective_after;

static void src_read_effective(void *ctx, PoolExecEffectiveConfig *out)
{
    (void)ctx;
    g_read_effective_calls++;
    *out = g_effective;
    /* Injectable failure of the Nth capture (0 = the next one). */
    if (g_fail_effective_after >= 0) {
        if (g_fail_effective_after == 0) {
            g_fail_effective_after = -1;
            out->valid = false;
        } else {
            g_fail_effective_after--;
        }
    }
}

/* Returns a BOOLEAN only — no password value exists in this harness. */
static bool src_password_retained(void *ctx)
{
    (void)ctx;
    return g_password_retained;
}

static const PoolApiSourceOps g_src_ops = {
    .device_identity          = src_device_identity,
    .read_effective           = src_read_effective,
    .source_password_retained = src_password_retained,
};

/* ---------------- fake Gate B7 executor adapters ---------------- */

/*
 * A real PoolSessionExecutor bound to deterministic fakes. It proves the
 * same-boot adoption end to end WITHOUT any pool, protocol or hardware
 * access: `stage_calls` counts every attempted pool-configuration write and
 * `protocol_calls` counts every protocol lifecycle call, so a test can
 * assert that Gate B8 itself performs neither.
 */
static PoolSessionExecutor g_ex;
static PoolExecEffectiveConfig g_exec_effective;
static int g_stage_calls;
static int g_protocol_calls;
static bool g_exec_running;

static bool xc_device_identity(void *ctx, char *board, size_t bcap,
                               char *asic, size_t acap)
{
    return src_device_identity(ctx, board, bcap, asic, acap);
}

static bool xc_stage_apply(void *ctx, const PoolConfigIdentity *identity)
{
    (void)ctx;
    g_stage_calls++;
    pool_exec_desired_from_identity(identity, &g_exec_effective);
    return true;
}

static void xc_read_effective(void *ctx, PoolExecEffectiveConfig *out)
{
    (void)ctx;
    *out = g_exec_effective;
}

static bool xc_refresh_live(void *ctx, const PoolConfigIdentity *identity)
{
    (void)ctx;
    (void)identity;
    return true;
}

static const PoolExecConfigOps g_exec_cfg_ops = {
    .device_identity = xc_device_identity,
    .stage_apply     = xc_stage_apply,
    .read_effective  = xc_read_effective,
    .refresh_live    = xc_refresh_live,
};

static bool xp_start(void *ctx, PoolSessionProtocol protocol)
{
    (void)ctx;
    (void)protocol;
    g_protocol_calls++;
    g_exec_running = true;
    return true;
}
static bool xp_stop(void *ctx)
{
    (void)ctx;
    g_protocol_calls++;
    g_exec_running = false;
    return true;
}
static bool xp_running(void *ctx) { (void)ctx; return g_exec_running; }
static uint32_t xp_poll(void *ctx) { (void)ctx; return 0u; }
static void xp_counters(void *ctx, PoolExecProtocolCounters *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
}
static bool xp_handoff(void *ctx) { (void)ctx; g_protocol_calls++; return true; }

static const PoolExecProtocolOps g_exec_proto_ops = {
    .start = xp_start, .stop = xp_stop, .running = xp_running,
    .poll_events = xp_poll, .counters = xp_counters, .handoff_source = xp_handoff,
};

/* ---------------- fixtures ---------------- */

static PoolSessionRuntime      g_rt;
static PoolSessionRuntimeDeps  g_deps;
static PoolSessionApiProcessor g_proc;
static PoolApiStatus           g_status;

static void source_defaults(void)
{
    memset(&g_effective, 0, sizeof(g_effective));
    g_effective.valid = true;
    strncpy(g_effective.primary.host, "btc.example",
            sizeof(g_effective.primary.host) - 1);
    g_effective.primary.port = 3333;
    strncpy(g_effective.primary.user, "acct.worker",
            sizeof(g_effective.primary.user) - 1);
    g_effective.primary.protocol = POOL_PROTO_STRATUM_V1;
    g_effective.primary.tls      = false;
    g_effective.primary_tls_mode = POOL_EXEC_TLS_MODE_DISABLED;
    g_effective.fallback_tls_mode = POOL_EXEC_TLS_MODE_DISABLED;
    g_effective.use_fallback     = false;
    g_identity_ok       = true;
    g_password_retained = true;
    g_board = POOL_SESSION_SUPPORTED_BOARD;
    g_asic  = POOL_SESSION_SUPPORTED_ASIC;
    g_read_effective_calls = 0;
    g_fail_effective_after = -1;
    /* The executor sees the same device configuration (still the source). */
    g_exec_effective  = g_effective;
    g_stage_calls     = 0;
    g_protocol_calls  = 0;
    g_exec_running    = false;
}

static void make_create_cmd(PoolApiCommand *c, uint32_t duration_s)
{
    memset(c, 0, sizeof(*c));
    c->kind  = POOL_API_CMD_CREATE_SESSION;
    c->actor = POOL_API_ACTOR_LOCAL_HTTP;
    c->create.duration_s = duration_s;
    strncpy(c->create.target_host, "bch.example", sizeof(c->create.target_host) - 1);
    c->create.target_port = 3334;
    strncpy(c->create.target_user, "acct.worker", sizeof(c->create.target_user) - 1);
    c->create.target_protocol = POOL_PROTO_STRATUM_V1;
    c->create.target_tls_mode = POOL_API_TLS_DISABLED;
    c->create.target_chain    = POOL_CHAIN_BITCOIN_CASH;
}

static void make_action_cmd(PoolApiCommand *c, PoolApiCommandKind kind)
{
    memset(c, 0, sizeof(*c));
    c->kind  = kind;
    c->actor = POOL_API_ACTOR_LOCAL_HTTP;
}

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

static void make_rec(PoolSessionRecord *r, PoolSessionState st, bool restore_required)
{
    pool_session_record_init(r);
    r->session_id       = 4242u;
    r->b1_model_version = POOL_SESSION_MODEL_VERSION;
    r->state            = st;
    fill_identity(&r->source, POOL_CHAIN_CUSTOM_UNKNOWN, "btc.example", 3333, "acct.worker");
    fill_identity(&r->target, POOL_CHAIN_BITCOIN_CASH, "bch.example", 3334, "acct.worker");
    r->password_policy  = POOL_SESSION_PW_KEEP_CURRENT;
    r->restore_required = restore_required;
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
    if (st == POOL_STATE_COMPLETE) {
        r->restore_verify.connection_observed = true;
        r->restore_verify.mining_observed     = true;
        r->restore_verify.identity_verified   = true;
    }
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(r));
}

static void seed_fake_record(const PoolSessionRecord *rec)
{
    PoolSessionStore  st;
    PoolSessionRecord copy = *rec;
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&st, &g_fake_ops, NULL));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&st, &copy));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&st));
}

static void teardown(void)
{
    pool_api_processor_deinit(&g_proc);
    pool_session_executor_deinit(&g_ex);
    pool_session_execution_gate_reset();
    (void)pool_session_runtime_deinit(&g_rt);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_task_count());
}

/* Boot a runtime over the fake backend and bind a bound processor. */
static void boot_and_bind(void)
{
    memset(&g_deps, 0, sizeof(g_deps));
    g_deps.store_ops             = &g_fake_ops;
    g_deps.sntp_ops              = &g_fake_sntp_ops;
    g_deps.read_reset_reason_raw = fake_reset_reason;
    g_deps.monotonic_us          = fake_monotonic;
    g_deps.ntp_server            = "";
    g_deps.sync_wait_limit_s     = 600u;

    memset(&g_rt, 0, sizeof(g_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    pool_session_executor_init(&g_ex);
    TEST_ASSERT_EQUAL(EXEC_REASON_NONE,
                      pool_session_executor_bind(&g_ex, &g_rt, &g_exec_cfg_ops, NULL,
                                                 &g_exec_proto_ops, NULL, NULL));
    pool_session_executor_set_system_ready(&g_ex, true);

    pool_api_processor_init(&g_proc);
    TEST_ASSERT_EQUAL(API_SUBMIT_ACCEPTED,
                      pool_api_processor_bind(&g_proc, &g_rt, &g_src_ops, NULL, &g_ex));
}

/* The same runtime with NO executor bound (adoption is impossible). */
static void boot_and_bind_no_executor(void)
{
    memset(&g_deps, 0, sizeof(g_deps));
    g_deps.store_ops             = &g_fake_ops;
    g_deps.sntp_ops              = &g_fake_sntp_ops;
    g_deps.read_reset_reason_raw = fake_reset_reason;
    g_deps.monotonic_us          = fake_monotonic;
    g_deps.ntp_server            = "";
    g_deps.sync_wait_limit_s     = 600u;

    memset(&g_rt, 0, sizeof(g_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    pool_api_processor_init(&g_proc);
    TEST_ASSERT_EQUAL(API_SUBMIT_ACCEPTED,
                      pool_api_processor_bind(&g_proc, &g_rt, &g_src_ops, NULL, NULL));
}

static void fresh_free_device(void)
{
    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    boot_and_bind();
    TEST_ASSERT_EQUAL(STORE_EMPTY, g_rt.store_result);
}

/* ---- concurrency fixture (file scope: no nested functions) ---- */

static PoolApiCommandQueue g_race_queue;
static SemaphoreHandle_t   g_race_go;
static SemaphoreHandle_t   g_race_done;
static uint32_t            g_race_seqs[POOL_API_COMMAND_QUEUE_DEPTH * 2u];
static uint32_t            g_race_ok;

static void race_producer(void *arg)
{
    PoolApiCommand c;
    uint32_t       s;
    uint32_t       slot;
    unsigned       k;

    (void)arg;
    xSemaphoreTake(g_race_go, portMAX_DELAY);
    for (k = 0; k < POOL_API_COMMAND_QUEUE_DEPTH; k++) {
        make_create_cmd(&c, 900u);
        s = 0u;
        if (pool_api_queue_submit(&g_race_queue, &c, &s) == API_SUBMIT_ACCEPTED) {
            slot = __atomic_fetch_add(&g_race_ok, 1u, __ATOMIC_SEQ_CST);
            if (slot < (uint32_t)(POOL_API_COMMAND_QUEUE_DEPTH * 2u)) {
                g_race_seqs[slot] = s;
            }
        }
    }
    xSemaphoreGive(g_race_done);
    vTaskDelete(NULL);
}

/* Submit + drive one owner-task step. */
static PoolApiCommandResult run_command(const PoolApiCommand *cmd)
{
    uint32_t seq = 0u;
    TEST_ASSERT_EQUAL(API_SUBMIT_ACCEPTED,
                      pool_api_processor_submit(&g_proc, cmd, &seq));
    TEST_ASSERT_NOT_EQUAL(0u, seq);
    (void)pool_api_processor_step(&g_proc);
    return g_proc.last_result;
}

/* ================================================================= */
/* A. Mailbox                                                         */
/* ================================================================= */

TEST_CASE("api cmd: the mailbox is bounded, ordered and zeroes consumed slots",
          "[pool_api_cmd]")
{
    PoolApiCommandQueue q;
    PoolApiCommand      c, out;
    uint32_t            seq = 0u;
    unsigned            i;

    pool_api_queue_init(&q);
    TEST_ASSERT_EQUAL_UINT8(0u, pool_api_queue_depth(&q));
    TEST_ASSERT_FALSE(pool_api_queue_take(&q, &out));

    for (i = 0; i < POOL_API_COMMAND_QUEUE_DEPTH; i++) {
        make_create_cmd(&c, 900u + i);
        TEST_ASSERT_EQUAL(API_SUBMIT_ACCEPTED, pool_api_queue_submit(&q, &c, &seq));
        TEST_ASSERT_EQUAL_UINT32(i + 1u, seq);
    }
    TEST_ASSERT_EQUAL_UINT8((uint8_t)POOL_API_COMMAND_QUEUE_DEPTH,
                            pool_api_queue_depth(&q));

    /* Queue full: nothing is mutated and nothing is dropped. */
    make_create_cmd(&c, 1200u);
    TEST_ASSERT_EQUAL(API_SUBMIT_QUEUE_FULL, pool_api_queue_submit(&q, &c, &seq));
    TEST_ASSERT_EQUAL_UINT32(0u, seq);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)POOL_API_COMMAND_QUEUE_DEPTH,
                            pool_api_queue_depth(&q));
    TEST_ASSERT_EQUAL_UINT32(1u, q.rejected_full);

    /* FIFO order, and the consumed slot is fully zeroed. */
    for (i = 0; i < POOL_API_COMMAND_QUEUE_DEPTH; i++) {
        uint8_t head = q.head;
        TEST_ASSERT_TRUE(pool_api_queue_take(&q, &out));
        TEST_ASSERT_EQUAL_UINT32(900u + i, out.create.duration_s);
        TEST_ASSERT_EQUAL_UINT32(i + 1u, out.submission_sequence);
        TEST_ASSERT_TRUE(pool_api_queue_slot_is_zero(&q, head));
    }
    TEST_ASSERT_FALSE(pool_api_queue_take(&q, &out));
    TEST_ASSERT_EQUAL_UINT32(POOL_API_COMMAND_QUEUE_DEPTH, q.consumed);
}

TEST_CASE("api cmd: invalid commands are refused before they can be queued",
          "[pool_api_cmd]")
{
    PoolApiCommandQueue q;
    PoolApiCommand      c;
    uint32_t            seq = 1u;

    pool_api_queue_init(&q);
    TEST_ASSERT_EQUAL(API_SUBMIT_INVALID, pool_api_queue_submit(&q, NULL, &seq));
    TEST_ASSERT_EQUAL_UINT32(0u, seq);

    memset(&c, 0, sizeof(c));               /* kind NONE */
    TEST_ASSERT_EQUAL(API_SUBMIT_INVALID, pool_api_queue_submit(&q, &c, &seq));

    make_create_cmd(&c, 900u);
    c.actor = POOL_API_ACTOR_UNKNOWN;       /* fail-closed actor */
    TEST_ASSERT_EQUAL(API_SUBMIT_INVALID, pool_api_queue_submit(&q, &c, &seq));

    make_create_cmd(&c, 899u);              /* below the B1 minimum */
    TEST_ASSERT_EQUAL(API_SUBMIT_INVALID, pool_api_queue_submit(&q, &c, &seq));

    make_create_cmd(&c, 86401u);            /* above the B1 maximum */
    TEST_ASSERT_EQUAL(API_SUBMIT_INVALID, pool_api_queue_submit(&q, &c, &seq));

    make_create_cmd(&c, 900u);
    c.create.target_host[0] = '\0';
    TEST_ASSERT_EQUAL(API_SUBMIT_INVALID, pool_api_queue_submit(&q, &c, &seq));

    make_create_cmd(&c, 900u);
    c.create.target_port = 0u;
    TEST_ASSERT_EQUAL(API_SUBMIT_INVALID, pool_api_queue_submit(&q, &c, &seq));

    make_create_cmd(&c, 900u);
    c.create.target_tls_mode = 2u;          /* custom certificate */
    TEST_ASSERT_EQUAL(API_SUBMIT_INVALID, pool_api_queue_submit(&q, &c, &seq));

    make_create_cmd(&c, 900u);
    c.create.target_chain = (PoolChainType)9;
    TEST_ASSERT_EQUAL(API_SUBMIT_INVALID, pool_api_queue_submit(&q, &c, &seq));

    TEST_ASSERT_EQUAL_UINT8(0u, pool_api_queue_depth(&q));
    TEST_ASSERT_EQUAL_UINT32(0u, q.submitted);
}

TEST_CASE("api cmd: no command can carry a password byte", "[pool_api_cmd]")
{
    /*
     * Structural proof: the command is a fixed-size value aggregate with no
     * pointer and no password member, so a secret cannot be transported at
     * all. The mailbox stores commands BY VALUE, so no pointer into caller
     * memory is ever retained either.
     */
    PoolApiCommandQueue q;
    PoolApiCommand      c, out;
    const uint8_t      *raw;
    size_t              i;
    uint32_t            seq = 0u;

    make_create_cmd(&c, 900u);
    raw = (const uint8_t *)&c;
    for (i = 0; i + 7u <= sizeof(c); i++) {
        TEST_ASSERT_FALSE(memcmp(raw + i, "hunter2", 7) == 0);
    }
    /* Every byte of the command is accounted for by its declared members. */
    TEST_ASSERT_TRUE(sizeof(PoolApiCommand) >=
                     sizeof(PoolApiCreateRequest) + 2u * sizeof(uint32_t));
    TEST_ASSERT_TRUE(sizeof(PoolApiCommand) < 512u);

    /* A queued command is copied by value: mutating the caller's copy after
     * submission can never change what the owner task later consumes. */
    pool_api_queue_init(&q);
    TEST_ASSERT_EQUAL(API_SUBMIT_ACCEPTED, pool_api_queue_submit(&q, &c, &seq));
    memset(c.create.target_host, 'Z', sizeof(c.create.target_host) - 1u);
    TEST_ASSERT_TRUE(pool_api_queue_take(&q, &out));
    TEST_ASSERT_EQUAL_STRING("bch.example", out.create.target_host);
}

/* ================================================================= */
/* B. Create flow                                                     */
/* ================================================================= */

TEST_CASE("api cmd: a FREE device creates a durable pre-mutation session",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    fresh_free_device();
    make_create_cmd(&c, 3600u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));

    /*
     * The create ran the durable pre-mutation snapshot AND handed the
     * session to the executor in the SAME call, so the committed state is
     * already the APPLYING_TARGET action boundary with the restore
     * obligation durable — and no pool key has been written yet.
     */
    TEST_ASSERT_TRUE(g_rt.record_present);
    TEST_ASSERT_EQUAL(STORE_OK, g_rt.store_result);
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_TARGET, g_rt.record.state);
    TEST_ASSERT_TRUE(g_rt.record.restore_required);
    TEST_ASSERT_EQUAL_INT(0, g_stage_calls);
    TEST_ASSERT_EQUAL_INT(0, g_protocol_calls);
    TEST_ASSERT_EQUAL(POOL_SESSION_PW_KEEP_CURRENT, g_rt.record.password_policy);
    TEST_ASSERT_EQUAL_UINT32(3600u, g_rt.record.duration_s);
    TEST_ASSERT_NOT_EQUAL(0u, g_rt.record.generation);

    /* The SOURCE was captured internally from the device configuration. */
    TEST_ASSERT_EQUAL_STRING("btc.example", g_rt.record.source.primary.host);
    TEST_ASSERT_EQUAL_UINT16(3333u, g_rt.record.source.primary.port);
    TEST_ASSERT_EQUAL_STRING("acct.worker", g_rt.record.source.primary.user);
    /* The TARGET is exactly what the command carried. */
    TEST_ASSERT_EQUAL_STRING("bch.example", g_rt.record.target.primary.host);
    TEST_ASSERT_EQUAL_UINT16(3334u, g_rt.record.target.primary.port);
    TEST_ASSERT_EQUAL(POOL_CHAIN_BITCOIN_CASH, g_rt.record.target.chain);
    /* B8 accepts no target fallback. */
    TEST_ASSERT_FALSE(g_rt.record.target.fallback_enabled);

    /* The B5 reservation was activated by the SESSION_COMMITTED proof and
     * then moved onto the target-verification phase by the executor. */
    TEST_ASSERT_EQUAL(OP_OWNER_TIMED_SESSION, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_VERIFYING_TARGET, g_rt.lease.phase);
    TEST_ASSERT_TRUE(g_rt.lease.durable_claim);
    TEST_ASSERT_TRUE(g_rt.token.valid);

    /* Ownership transferred exactly once: the API flag is clear and the
     * executor is authoritative, with no interval owned by neither. */
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);
    TEST_ASSERT_TRUE(pool_session_executor_owns_flow(&g_ex));
    TEST_ASSERT_TRUE(pool_api_processor_step(&g_proc));

    /* The source snapshot is captured under the lease, and re-captured once
     * during the pre-flight — never supplied by a client. */
    TEST_ASSERT_TRUE(g_read_effective_calls >= 2);
    teardown();
}

TEST_CASE("api cmd: the committed source snapshot is immutable afterwards",
          "[pool_api_cmd]")
{
    PoolApiCommand    c;
    PoolSessionRecord before;

    fresh_free_device();
    make_create_cmd(&c, 900u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
    before = g_rt.record;

    /* The device configuration changes underneath: the DURABLE snapshot
     * must not follow it. */
    strncpy(g_effective.primary.host, "moved.example",
            sizeof(g_effective.primary.host) - 1);
    g_effective.primary.port = 4444;
    (void)pool_api_processor_step(&g_proc);
    (void)pool_api_processor_step(&g_proc);

    /* The committed SOURCE snapshot never follows the live configuration. */
    TEST_ASSERT_EQUAL_STRING("btc.example", g_rt.record.source.primary.host);
    TEST_ASSERT_EQUAL_UINT16(3333u, g_rt.record.source.primary.port);
    TEST_ASSERT_EQUAL_STRING("acct.worker", g_rt.record.source.primary.user);
    TEST_ASSERT_EQUAL_UINT32(before.session_id, g_rt.record.session_id);
    TEST_ASSERT_TRUE(pool_config_identity_equal(&before.source, &g_rt.record.source));
    teardown();
}

TEST_CASE("api cmd: a second create is refused while a session exists",
          "[pool_api_cmd]")
{
    PoolApiCommand c;
    uint32_t       gen;

    fresh_free_device();
    make_create_cmd(&c, 900u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
    gen = g_rt.record.generation;

    make_create_cmd(&c, 1800u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_CONFLICT, run_command(&c));
    /* The first session is completely untouched by the refused second one. */
    TEST_ASSERT_EQUAL_UINT32(gen, g_rt.record.generation);
    TEST_ASSERT_EQUAL_UINT32(900u, g_rt.record.duration_s);
    TEST_ASSERT_NOT_EQUAL(0u, g_proc.last_conflict_code);
    teardown();
}

TEST_CASE("api cmd: unsupported hardware never reserves a lease", "[pool_api_cmd]")
{
    PoolApiCommand c;

    fresh_free_device();
    g_board = "203";
    make_create_cmd(&c, 900u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_HARDWARE, run_command(&c));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    TEST_ASSERT_FALSE(g_rt.record_present);
    TEST_ASSERT_EQUAL_INT(0, g_fake.writes);

    g_board = POOL_SESSION_SUPPORTED_BOARD;
    g_asic  = "BM1368";
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_HARDWARE, run_command(&c));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);

    g_asic = POOL_SESSION_SUPPORTED_ASIC;
    g_identity_ok = false;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_HARDWARE, run_command(&c));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL_INT(0, g_fake.writes);
    teardown();
}

TEST_CASE("api cmd: an unrestorable source is refused before any reservation",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    make_create_cmd(&c, 900u);

    /* The readback itself failed. */
    fresh_free_device();
    g_effective.valid = false;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED, run_command(&c));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL_INT(0, g_fake.writes);
    teardown();

    /* A custom-certificate TLS mode cannot be restored exactly. */
    fresh_free_device();
    g_effective.primary_tls_mode = POOL_EXEC_TLS_MODE_CUSTOM;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED, run_command(&c));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL_INT(0, g_fake.writes);
    teardown();

    /* Currently mining on the FALLBACK endpoint: the committed B7
     * transaction pins use_fallback false, so the exact role is not
     * restorable. */
    fresh_free_device();
    g_effective.use_fallback = true;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED, run_command(&c));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    teardown();

    /* Keep-current-password requires the stored password to be retained. */
    fresh_free_device();
    g_password_retained = false;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED, run_command(&c));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL_INT(0, g_fake.writes);
    teardown();
}

TEST_CASE("api cmd: a target equal to the current pool is refused",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    fresh_free_device();
    make_create_cmd(&c, 900u);
    strncpy(c.create.target_host, "btc.example", sizeof(c.create.target_host) - 1);
    c.create.target_port = 3333;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_TARGET_UNSUPPORTED, run_command(&c));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL_INT(0, g_fake.writes);
    teardown();
}

TEST_CASE("api cmd: an uncertain commit enters the recovery guard",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    fresh_free_device();
    make_create_cmd(&c, 900u);
    /* Slot write + commit succeed, the POINTER commit fails: the committed
     * truth is unknowable until a reload. */
    g_fake.fail_commit_after = 1;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_RECOVERY_GUARD, run_command(&c));
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_rt.lease.phase);
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);

    /* A guarded device never becomes FREE and never accepts a new session. */
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_CONFLICT, run_command(&c));
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_rt.lease.phase);
    teardown();
}

TEST_CASE("api cmd: a failed independent readback never activates the session",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    fresh_free_device();
    make_create_cmd(&c, 900u);
    /* Let the commit fully succeed (it reads the pointer twice), then fail
     * the INDEPENDENT reload's pointer read. */
    g_fake.fail_active_read_after = 2;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_FAILED_READBACK, run_command(&c));
    TEST_ASSERT_NOT_EQUAL(OP_PHASE_ACTIVE, g_rt.lease.phase);
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);
    teardown();
}

/* ================================================================= */
/* C. Restore Now                                                     */
/* ================================================================= */

TEST_CASE("api cmd: Restore Now is refused without an active session",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    fresh_free_device();
    make_action_cmd(&c, POOL_API_CMD_RESTORE_NOW);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_STATE, run_command(&c));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)OP_HTTP_OPERATION_NO_ACTIVE_SESSION,
                            g_proc.last_conflict_code);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    teardown();
}

TEST_CASE("api cmd: Restore Now controls the SAME lease, never a second one",
          "[pool_api_cmd]")
{
    PoolApiCommand c;
    uint32_t       lease_gen;

    fresh_free_device();
    make_create_cmd(&c, 900u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
    lease_gen = g_rt.lease.lease_generation;

    make_action_cmd(&c, POOL_API_CMD_RESTORE_NOW);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));

    /* The adopted session is past the apply intent, so Restore Now becomes a
     * real restoration: the obligation is retained and the SAME lease was
     * controlled — never a second one. */
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, g_rt.record.state);
    TEST_ASSERT_TRUE(g_rt.record.restore_required);
    TEST_ASSERT_EQUAL(OP_OWNER_SOURCE_RESTORE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_RESTORING_SOURCE, g_rt.lease.phase);
    TEST_ASSERT_TRUE(g_rt.lease.lease_generation > lease_gen);
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);
    teardown();
}

TEST_CASE("api cmd: Restore Now is idempotent while already restoring",
          "[pool_api_cmd]")
{
    PoolApiCommand    c;
    PoolSessionRecord rec;
    uint32_t          gen;

    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    make_rec(&rec, POOL_STATE_RESTORE_DUE, true);
    seed_fake_record(&rec);
    boot_and_bind();

    TEST_ASSERT_EQUAL(STORE_OK, g_rt.store_result);
    TEST_ASSERT_EQUAL(OP_PHASE_RESTORING_SOURCE, g_rt.lease.phase);
    gen = g_rt.record.generation;

    make_action_cmd(&c, POOL_API_CMD_RESTORE_NOW);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
    /* Idempotent: nothing changed, no second lease, no new commit. */
    TEST_ASSERT_EQUAL_UINT32(gen, g_rt.record.generation);
    TEST_ASSERT_EQUAL(OP_PHASE_RESTORING_SOURCE, g_rt.lease.phase);
    TEST_ASSERT_TRUE(g_rt.record.restore_required);

    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
    TEST_ASSERT_EQUAL_UINT32(gen, g_rt.record.generation);
    teardown();
}

TEST_CASE("api cmd: Restore Now is refused on a terminal record", "[pool_api_cmd]")
{
    PoolApiCommand    c;
    PoolSessionRecord rec;

    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    make_rec(&rec, POOL_STATE_COMPLETE, false);
    seed_fake_record(&rec);
    boot_and_bind();

    make_action_cmd(&c, POOL_API_CMD_RESTORE_NOW);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_STATE, run_command(&c));
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, g_rt.record.state);
    teardown();
}

/* ================================================================= */
/* D. Terminal acknowledgement                                        */
/* ================================================================= */

TEST_CASE("api cmd: a COMPLETE result is acknowledged only through a proven tombstone",
          "[pool_api_cmd]")
{
    PoolApiCommand    c;
    PoolSessionRecord rec;

    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    make_rec(&rec, POOL_STATE_COMPLETE, false);
    seed_fake_record(&rec);
    boot_and_bind();

    TEST_ASSERT_TRUE(g_rt.lease.terminal_pending);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);

    make_action_cmd(&c, POOL_API_CMD_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));

    TEST_ASSERT_EQUAL(STORE_CLEARED, g_rt.store_result);
    TEST_ASSERT_FALSE(g_rt.record_present);
    TEST_ASSERT_FALSE(g_rt.lease.terminal_pending);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    /* Acknowledgement never starts another session. */
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);
    teardown();
}

TEST_CASE("api cmd: a refused adoption cancels safely and is acknowledgeable",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    fresh_free_device();
    /*
     * Adoption is refused (the executor is not system-ready), so the create
     * takes the committed pre-mutation CANCELLED path: no pool touched, no
     * restoration owed, and a SAFE terminal result retained. The create is
     * NEVER reported as accepted.
     */
    pool_session_executor_set_system_ready(&g_ex, false);
    make_create_cmd(&c, 900u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ADOPTION_FAILED, run_command(&c));
    TEST_ASSERT_EQUAL(POOL_STATE_CANCELLED, g_rt.record.state);
    TEST_ASSERT_FALSE(g_rt.record.restore_required);
    TEST_ASSERT_EQUAL_INT(0, g_stage_calls);
    TEST_ASSERT_EQUAL_INT(0, g_protocol_calls);
    TEST_ASSERT_TRUE(g_rt.lease.terminal_pending);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);

    make_action_cmd(&c, POOL_API_CMD_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
    TEST_ASSERT_EQUAL(STORE_CLEARED, g_rt.store_result);
    TEST_ASSERT_FALSE(g_rt.lease.terminal_pending);

    /* Only after a proven clear may a new session be created — and this one
     * adopts normally. */
    pool_session_executor_set_system_ready(&g_ex, true);
    make_create_cmd(&c, 1800u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_TARGET, g_rt.record.state);
    teardown();
}

TEST_CASE("api cmd: unsafe terminal states can never be acknowledged",
          "[pool_api_cmd]")
{
    static const PoolSessionState UNSAFE[] = {
        POOL_STATE_RESTORE_FAILED, POOL_STATE_RECOVERY_REQUIRED,
        POOL_STATE_TARGET_FAILED, POOL_STATE_TARGET_ACTIVE,
    };
    PoolApiCommand    c;
    PoolSessionRecord rec;
    PoolSession       s;
    size_t            i;

    for (i = 0; i < sizeof(UNSAFE) / sizeof(UNSAFE[0]); i++) {
        fake_reset();
        source_defaults();
        g_monotonic_us = 5ull * US_PER_S;
        make_rec(&rec, UNSAFE[i], true);
        seed_fake_record(&rec);
        boot_and_bind();

        make_action_cmd(&c, POOL_API_CMD_ACKNOWLEDGE_TERMINAL);
        TEST_ASSERT_NOT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
        /*
         * The record survives and is never cleared. Its STATE may legally
         * have been advanced by the committed B4 boot-recovery table (for
         * example RESTORE_FAILED -> RESTORE_DUE), but the acknowledgement
         * path itself must have changed nothing: no tombstone, a record
         * still present, and the restore obligation still owed.
         */
        TEST_ASSERT_NOT_EQUAL(STORE_CLEARED, g_rt.store_result);
        TEST_ASSERT_TRUE(g_rt.record_present);
        TEST_ASSERT_TRUE(g_rt.record.restore_required);
        TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_to_session(&g_rt.record, &s));
        TEST_ASSERT_FALSE(pool_session_is_acknowledgeable_terminal(&s));
        teardown();
    }
}

TEST_CASE("api cmd: an obligated record never even reaches the tombstone path",
          "[pool_api_cmd]")
{
    PoolApiCommand    c;
    PoolSessionRecord rec;
    int               commits_before;

    /*
     * The committed B3 model forbids a CANCELLED record that still owes a
     * restoration (RECORD_ERR_OBLIGATION_VIOLATION), so the representable
     * obligated terminal is RESTORE_FAILED. Acknowledging it must be
     * refused BEFORE any store write: not one commit may occur.
     */
    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    make_rec(&rec, POOL_STATE_RESTORE_FAILED, true);
    seed_fake_record(&rec);
    boot_and_bind();
    commits_before = g_fake.commits;

    make_action_cmd(&c, POOL_API_CMD_ACKNOWLEDGE_TERMINAL);
    /*
     * Refused before any store write. The refusal is a CONFLICT because an
     * obligated record reconstructs a SOURCE_RESTORE owner — B5 never
     * publishes terminal_pending while a restoration is still owed — so the
     * ownership precondition fires first. Either refusal is correct; what
     * matters is that no commit happened.
     */
    TEST_ASSERT_NOT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_CONFLICT, g_proc.last_result);
    TEST_ASSERT_EQUAL_INT(commits_before, g_fake.commits);
    TEST_ASSERT_NOT_EQUAL(STORE_CLEARED, g_rt.store_result);
    TEST_ASSERT_TRUE(g_rt.record_present);
    TEST_ASSERT_TRUE(g_rt.record.restore_required);
    /* And a direct B3 clear is equally refused: the obligation is durable. */
    TEST_ASSERT_EQUAL(STORE_STATE_CONFLICT, pool_session_store_commit_clear(&g_rt.store));
    teardown();
}

TEST_CASE("api cmd: an uncertain clear never clears the terminal state",
          "[pool_api_cmd]")
{
    PoolApiCommand    c;
    PoolSessionRecord rec;

    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    make_rec(&rec, POOL_STATE_COMPLETE, false);
    seed_fake_record(&rec);
    boot_and_bind();

    g_fake.fail_commit_after = 1; /* the tombstone POINTER commit fails */
    make_action_cmd(&c, POOL_API_CMD_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_RECOVERY_GUARD, run_command(&c));

    TEST_ASSERT_NOT_EQUAL(STORE_CLEARED, g_rt.store_result);
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_rt.lease.phase);
    /* A new session stays blocked. */
    make_create_cmd(&c, 900u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_CONFLICT, run_command(&c));
    teardown();
}

/* ================================================================= */
/* E. Heartbeat through the committed B6 persistence engine            */
/* ================================================================= */

TEST_CASE("api cmd: a heartbeat raises only the trusted-epoch floor",
          "[pool_api_cmd]")
{
    PoolSessionRecord rec;
    uint64_t          deadline_before;
    uint32_t          duration_before;
    uint32_t          gen_before;
    PoolSessionState  state_before;

    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    make_rec(&rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&rec);
    boot_and_bind();
    TEST_ASSERT_EQUAL(STORE_OK, g_rt.store_result);

    deadline_before = g_rt.record.deadline_epoch_s;
    duration_before = g_rt.record.duration_s;
    state_before    = (PoolSessionState)g_rt.record.state;
    gen_before      = g_rt.record.generation;

    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_commit_epoch_heartbeat(
                          &g_rt, EPOCH_A_S + 500ull));

    /* Only the floor moved. */
    TEST_ASSERT_TRUE(g_rt.record.latest_trusted_valid);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 500ull, g_rt.record.latest_trusted_epoch_s);
    /* The deadline, the duration and the state are untouched — extending a
     * deadline is structurally impossible. */
    TEST_ASSERT_EQUAL_UINT64(deadline_before, g_rt.record.deadline_epoch_s);
    TEST_ASSERT_EQUAL_UINT32(duration_before, g_rt.record.duration_s);
    TEST_ASSERT_EQUAL(state_before, g_rt.record.state);
    TEST_ASSERT_TRUE(g_rt.record.restore_required);
    TEST_ASSERT_TRUE(g_rt.record.generation > gen_before);
    teardown();
}

TEST_CASE("api cmd: a duplicate heartbeat never writes twice", "[pool_api_cmd]")
{
    PoolSessionRecord rec;
    uint32_t          gen;
    int               writes;

    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    make_rec(&rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&rec);
    boot_and_bind();

    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_commit_epoch_heartbeat(
                          &g_rt, EPOCH_A_S + 500ull));
    gen    = g_rt.record.generation;
    writes = g_fake.writes;

    /* The identical epoch is already durable: not an error, NOT a write. */
    TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_DUPLICATE,
                      pool_session_runtime_commit_epoch_heartbeat(
                          &g_rt, EPOCH_A_S + 500ull));
    TEST_ASSERT_EQUAL_UINT32(gen, g_rt.record.generation);
    TEST_ASSERT_EQUAL_INT(writes, g_fake.writes);

    /* An OLDER epoch would lower the floor: refused, and never a write. */
    TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_DUPLICATE,
                      pool_session_runtime_commit_epoch_heartbeat(&g_rt, EPOCH_A_S));
    TEST_ASSERT_EQUAL_UINT32(gen, g_rt.record.generation);
    TEST_ASSERT_EQUAL_INT(writes, g_fake.writes);

    /* A strictly newer epoch commits exactly once. */
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_commit_epoch_heartbeat(
                          &g_rt, EPOCH_A_S + 1000ull));
    TEST_ASSERT_TRUE(g_rt.record.generation > gen);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 1000ull, g_rt.record.latest_trusted_epoch_s);
    teardown();
}

TEST_CASE("api cmd: a heartbeat is impossible without a committed session",
          "[pool_api_cmd]")
{
    fresh_free_device();
    TEST_ASSERT_EQUAL(RUNTIME_ERR_INTERNAL_CONSISTENCY,
                      pool_session_runtime_commit_epoch_heartbeat(
                          &g_rt, EPOCH_A_S + 500ull));
    TEST_ASSERT_EQUAL(RUNTIME_ERR_NOT_INITIALIZED,
                      pool_session_runtime_commit_epoch_heartbeat(NULL, EPOCH_A_S));
    TEST_ASSERT_EQUAL_INT(0, g_fake.writes);
    teardown();
}

/* ================================================================= */
/* F. Status, privacy and concurrency                                 */
/* ================================================================= */

TEST_CASE("api cmd: the published status carries no identity marker",
          "[pool_api_cmd]")
{
    static const char *const MARKERS[] = { "btc.example", "bch.example", "acct.worker" };
    PoolApiCommand c;
    const uint8_t *raw;
    size_t         i, j, n;

    fresh_free_device();
    make_create_cmd(&c, 900u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));

    pool_api_processor_status(&g_proc, &g_status);
    TEST_ASSERT_TRUE(pool_api_status_valid(&g_status));
    TEST_ASSERT_TRUE(g_status.session_present);
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_TARGET, g_status.durable_state);
    TEST_ASSERT_EQUAL(POOL_API_CMD_CREATE_SESSION, g_status.last_command);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, g_status.last_command_result);
    TEST_ASSERT_FALSE(g_status.target_mining_grant_active);
    TEST_ASSERT_TRUE(g_status.restore_required);

    raw = (const uint8_t *)&g_status;
    for (i = 0; i < sizeof(MARKERS) / sizeof(MARKERS[0]); i++) {
        n = strlen(MARKERS[i]);
        for (j = 0; j + n <= sizeof(g_status); j++) {
            TEST_ASSERT_FALSE_MESSAGE(memcmp(raw + j, MARKERS[i], n) == 0,
                                      "identity marker found in the API status");
        }
    }
    /*
     * The session id — a large, distinctive derived value — appears nowhere
     * in the published status. (Lease and record generations are small
     * counters that would collide with legitimate bounded counters in a
     * byte scan, so they are excluded STRUCTURALLY instead: no generation
     * field exists in PoolApiStatus at all, which the OpenAPI schema and
     * the status-builder input model both pin.)
     */
    TEST_ASSERT_NOT_EQUAL(0u, g_rt.record.session_id);
    for (j = 0; j + sizeof(uint32_t) <= sizeof(g_status); j++) {
        uint32_t v;
        memcpy(&v, raw + j, sizeof(v));
        TEST_ASSERT_NOT_EQUAL(g_rt.record.session_id, v);
    }
    teardown();
}

TEST_CASE("api cmd: an unbound processor accepts nothing and claims nothing",
          "[pool_api_cmd]")
{
    PoolSessionApiProcessor p;
    PoolApiCommand          c;
    PoolApiStatus           s;
    uint32_t                seq = 1u;

    memset(&p, 0, sizeof(p));
    make_create_cmd(&c, 900u);
    TEST_ASSERT_EQUAL(API_SUBMIT_NOT_READY, pool_api_processor_submit(&p, &c, &seq));
    TEST_ASSERT_FALSE(pool_api_processor_step(&p));

    pool_api_processor_init(&p);
    TEST_ASSERT_EQUAL(API_SUBMIT_NOT_READY, pool_api_processor_submit(&p, &c, &seq));
    TEST_ASSERT_FALSE(pool_api_processor_step(&p));

    /* An incomplete adapter fails the bind closed. */
    fresh_free_device();
    {
        PoolApiSourceOps broken = g_src_ops;
        broken.read_effective = NULL;
        TEST_ASSERT_EQUAL(API_SUBMIT_INVALID,
                          pool_api_processor_bind(&p, &g_rt, &broken, NULL, NULL));
        TEST_ASSERT_EQUAL(API_SUBMIT_INVALID,
                          pool_api_processor_bind(&p, &g_rt, NULL, NULL, NULL));
    }
    pool_api_processor_status(&p, &s);
    TEST_ASSERT_TRUE(pool_api_status_valid(&s));
    TEST_ASSERT_FALSE(s.session_present);
    teardown();

    pool_api_processor_status(NULL, &s);
    TEST_ASSERT_TRUE(pool_api_status_valid(&s));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_api_processor_processed(NULL));
}

TEST_CASE("api cmd: exactly one of two racing creates is accepted",
          "[pool_api_cmd]")
{
    PoolApiCommand c;
    unsigned       accepted = 0u;
    unsigned       i;

    fresh_free_device();
    /* Two identical creates are queued (a lost HTTP response followed by a
     * duplicate); the SINGLE consumer processes them in order. */
    make_create_cmd(&c, 900u);
    {
        uint32_t s1 = 0u, s2 = 0u;
        TEST_ASSERT_EQUAL(API_SUBMIT_ACCEPTED,
                          pool_api_processor_submit(&g_proc, &c, &s1));
        TEST_ASSERT_EQUAL(API_SUBMIT_ACCEPTED,
                          pool_api_processor_submit(&g_proc, &c, &s2));
        TEST_ASSERT_NOT_EQUAL(s1, s2);
    }
    for (i = 0; i < 4u; i++) {
        (void)pool_api_processor_step(&g_proc);
        if (g_proc.last_kind == POOL_API_CMD_CREATE_SESSION &&
            g_proc.last_result == API_CMD_RESULT_ACCEPTED) {
            accepted++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1u, accepted);
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_TARGET, g_rt.record.state);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_CONFLICT, g_proc.last_result);
    /* Exactly one session and exactly one lease exist. */
    TEST_ASSERT_EQUAL(OP_OWNER_TIMED_SESSION, g_rt.lease.owner);
    teardown();
}

TEST_CASE("api cmd: the mailbox is safe for concurrent producers",
          "[pool_api_cmd]")
{
    /*
     * Two FreeRTOS producers submit into the bounded mailbox behind a
     * deterministic semaphore barrier (no sleep-based racing). Exactly
     * QUEUE_DEPTH submissions may succeed, every accepted sequence is
     * unique, and the queue never overflows.
     */
    unsigned i, j;

    pool_api_queue_init(&g_race_queue);
    memset(g_race_seqs, 0, sizeof(g_race_seqs));
    g_race_ok   = 0u;
    g_race_go   = xSemaphoreCreateCounting(2, 0);
    g_race_done = xSemaphoreCreateCounting(2, 0);
    TEST_ASSERT_NOT_NULL(g_race_go);
    TEST_ASSERT_NOT_NULL(g_race_done);

    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(race_producer, "nx_p1", 4096, NULL, 5, NULL));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(race_producer, "nx_p2", 4096, NULL, 5, NULL));
    xSemaphoreGive(g_race_go);
    xSemaphoreGive(g_race_go);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(g_race_done, pdMS_TO_TICKS(5000)));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(g_race_done, pdMS_TO_TICKS(5000)));

    TEST_ASSERT_EQUAL_UINT32((uint32_t)POOL_API_COMMAND_QUEUE_DEPTH, g_race_ok);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)POOL_API_COMMAND_QUEUE_DEPTH,
                            pool_api_queue_depth(&g_race_queue));
    for (i = 0; i < POOL_API_COMMAND_QUEUE_DEPTH; i++) {
        TEST_ASSERT_NOT_EQUAL(0u, g_race_seqs[i]);
        for (j = i + 1u; j < POOL_API_COMMAND_QUEUE_DEPTH; j++) {
            TEST_ASSERT_NOT_EQUAL(g_race_seqs[i], g_race_seqs[j]);
        }
    }
    vSemaphoreDelete(g_race_go);
    vSemaphoreDelete(g_race_done);
    g_race_go = NULL;
    g_race_done = NULL;
}

TEST_CASE("api cmd: the derived session id is deterministic and never zero",
          "[pool_api_cmd]")
{
    TEST_ASSERT_EQUAL_UINT32(pool_api_derive_session_id(0u, 1u),
                             pool_api_derive_session_id(0u, 1u));
    TEST_ASSERT_NOT_EQUAL(pool_api_derive_session_id(0u, 1u),
                          pool_api_derive_session_id(0u, 2u));
    TEST_ASSERT_NOT_EQUAL(pool_api_derive_session_id(1u, 1u),
                          pool_api_derive_session_id(2u, 1u));
    TEST_ASSERT_NOT_EQUAL(0u, pool_api_derive_session_id(0u, 0u));
    TEST_ASSERT_NOT_EQUAL(0u, pool_api_derive_session_id(0xFFFFFFFFu, 0xFFFFFFFFu));
}

/* ================================================================= */
/* G. Audited no-mutation lease release                               */
/* ================================================================= */

TEST_CASE("api rel: a definite record-write failure RELEASES the reservation",
          "[pool_api_cmd]")
{
    PoolApiCommand c;
    uint32_t       gen_before;

    fresh_free_device();
    gen_before = g_rt.lease.lease_generation;
    make_create_cmd(&c, 900u);
    g_fake.fail_write_after = 0; /* the first slot write fails definitively */
    TEST_ASSERT_EQUAL(API_CMD_RESULT_FAILED_PERSIST, run_command(&c));

    /* Nothing became durable AND no lease is left behind. */
    TEST_ASSERT_FALSE(g_rt.record_present);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    TEST_ASSERT_TRUE(g_rt.lease.lease_generation > gen_before);
    TEST_ASSERT_FALSE(g_rt.token.valid); /* our copy is invalidated too */
    TEST_ASSERT_FALSE(g_rt.lease.terminal_pending);
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);

    /* No client retry was needed to clean ownership: a PATCH is admitted
     * again immediately, and a later create acquires normally. */
    TEST_ASSERT_EQUAL(NX_ADMIT_ALLOW,
                      nx_admission_evaluate(&g_rt.coord, NX_MUTATION_POOL_CONFIG));
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_TARGET, g_rt.record.state);
    teardown();
}

TEST_CASE("api rel: an unprovable store state never releases on a guess",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    fresh_free_device();
    make_create_cmd(&c, 900u);
    /*
     * The slot write succeeds but the slot COMMIT fails, leaving a staged
     * slot with no committed pointer. The independent reload can no longer
     * prove the store is EXACTLY as it was, so the audited abort REFUSES:
     * the reservation is retained rather than released on a guess. That is
     * the fail-closed half of the contract.
     */
    g_fake.fail_commit_after = 0;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_FAILED_PERSIST, run_command(&c));

    TEST_ASSERT_EQUAL(OP_OWNER_TIMED_SESSION, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_RESERVED_PENDING_PERSISTENCE, g_rt.lease.phase);
    TEST_ASSERT_FALSE(g_rt.record_present);
    /*
     * The retained posture is explicit, OWNED and fenced — never an unowned
     * orphan — so a concurrent PATCH/OTA/restart is still refused, and the
     * coordinator state is RAM-only so a reboot always resolves it. A
     * re-submitted create REUSES this same reservation and never acquires a
     * second lease, whatever the store then reports.
     */
    TEST_ASSERT_NOT_EQUAL(NX_ADMIT_ALLOW,
                          nx_admission_evaluate(&g_rt.coord, NX_MUTATION_POOL_CONFIG));
    (void)run_command(&c);
    TEST_ASSERT_EQUAL(OP_OWNER_TIMED_SESSION, g_rt.lease.owner);
    TEST_ASSERT_NOT_EQUAL(API_CMD_RESULT_ACCEPTED, g_proc.last_result);
    teardown();
}

TEST_CASE("api rel: a source failure under the lease RELEASES the reservation",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    fresh_free_device();
    make_create_cmd(&c, 900u);
    /* Pass the pre-flight (call 1), then fail the UNDER-LEASE re-capture. */
    g_fail_effective_after = 1;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED, run_command(&c));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    TEST_ASSERT_FALSE(g_rt.record_present);
    TEST_ASSERT_EQUAL_INT(0, g_fake.commits);
    teardown();
}

TEST_CASE("api rel: an uncertain commit GUARDS and never releases",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    fresh_free_device();
    make_create_cmd(&c, 900u);
    /* Slot write + commit succeed, the POINTER commit fails: unknowable. */
    g_fake.fail_commit_after = 1;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_RECOVERY_GUARD, run_command(&c));
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_rt.lease.phase);
    TEST_ASSERT_NOT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    /* A guarded device denies every mutation, including a new create. */
    TEST_ASSERT_EQUAL(NX_ADMIT_DENY_RECOVERY_GUARD,
                      nx_admission_evaluate(&g_rt.coord, NX_MUTATION_POOL_CONFIG));
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_CONFLICT, run_command(&c));
    teardown();
}

TEST_CASE("api rel: an ambiguous readback GUARDS and never releases",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    fresh_free_device();
    make_create_cmd(&c, 900u);
    /* Let the commit fully succeed (two pointer reads), then fail the
     * INDEPENDENT reload's pointer read: the truth is ambiguous. */
    g_fake.fail_active_read_after = 2;
    TEST_ASSERT_EQUAL(API_CMD_RESULT_FAILED_READBACK, run_command(&c));
    TEST_ASSERT_NOT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    TEST_ASSERT_NOT_EQUAL(OP_PHASE_ACTIVE, g_rt.lease.phase);
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);
    teardown();
}

TEST_CASE("api rel: a definite acknowledgement failure releases and retains",
          "[pool_api_cmd]")
{
    PoolApiCommand    c;
    PoolSessionRecord rec;
    uint32_t          gen_before;

    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    make_rec(&rec, POOL_STATE_COMPLETE, false);
    seed_fake_record(&rec);
    boot_and_bind();
    TEST_ASSERT_TRUE(g_rt.lease.terminal_pending);
    gen_before = g_rt.lease.lease_generation;

    /* The tombstone slot write fails definitively. */
    g_fake.fail_write_after = 0;
    make_action_cmd(&c, POOL_API_CMD_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_FAILED_PERSIST, run_command(&c));

    /* The short-lived acknowledgement lease is gone... */
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    TEST_ASSERT_TRUE(g_rt.lease.lease_generation > gen_before);
    /* ...and the terminal result is RETAINED for a later retry. */
    TEST_ASSERT_TRUE(g_rt.lease.terminal_pending);
    TEST_ASSERT_NOT_EQUAL(STORE_CLEARED, g_rt.store_result);
    TEST_ASSERT_TRUE(g_rt.record_present);
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, g_rt.record.state);

    /* A later acknowledgement — a NEW request, not the same one — succeeds. */
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
    TEST_ASSERT_EQUAL(STORE_CLEARED, g_rt.store_result);
    TEST_ASSERT_FALSE(g_rt.lease.terminal_pending);
    teardown();
}

TEST_CASE("api rel: an uncertain acknowledgement guards and keeps the terminal",
          "[pool_api_cmd]")
{
    PoolApiCommand    c;
    PoolSessionRecord rec;

    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    make_rec(&rec, POOL_STATE_COMPLETE, false);
    seed_fake_record(&rec);
    boot_and_bind();

    g_fake.fail_commit_after = 1; /* the tombstone POINTER commit fails */
    make_action_cmd(&c, POOL_API_CMD_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_RECOVERY_GUARD, run_command(&c));
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_rt.lease.phase);
    TEST_ASSERT_TRUE(g_rt.lease.terminal_pending);
    TEST_ASSERT_NOT_EQUAL(STORE_CLEARED, g_rt.store_result);
    teardown();
}

TEST_CASE("api rel: no failure path mutates pool, protocol or ASIC state",
          "[pool_api_cmd]")
{
    static const int WRITE_FAILS[]  = { 0, 1 };
    PoolApiCommand   c;
    size_t           i;

    /* Across every create failure mode the fake source adapter is only ever
     * READ, no protocol call exists in this component at all, and the ASIC
     * gate is never touched by Gate B8. */
    for (i = 0; i < sizeof(WRITE_FAILS) / sizeof(WRITE_FAILS[0]); i++) {
        fresh_free_device();
        make_create_cmd(&c, 900u);
        g_fake.fail_write_after = WRITE_FAILS[i];
        (void)run_command(&c);
        TEST_ASSERT_EQUAL_INT(0, g_stage_calls);   /* never a pool write   */
        TEST_ASSERT_EQUAL_INT(0, g_protocol_calls);/* never a protocol call */
        teardown();
    }
}

/* ================================================================= */
/* H. Same-boot executor adoption                                     */
/* ================================================================= */

TEST_CASE("api adopt: a create with no executor is refused at submission",
          "[pool_api_cmd]")
{
    PoolApiCommand c;
    uint32_t       seq = 1u;

    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    boot_and_bind_no_executor();

    make_create_cmd(&c, 900u);
    /* Requirement: never accept a create for a session with no viable
     * executor-adoption path. */
    TEST_ASSERT_EQUAL(API_SUBMIT_NOT_READY,
                      pool_api_processor_submit(&g_proc, &c, &seq));
    TEST_ASSERT_EQUAL_UINT32(0u, seq);
    TEST_ASSERT_EQUAL_UINT8(0u, pool_api_queue_depth(&g_proc.queue));
    TEST_ASSERT_FALSE(g_rt.record_present);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_rt.lease.owner);

    /* Action commands remain available (they need no adoption). */
    make_action_cmd(&c, POOL_API_CMD_RESTORE_NOW);
    TEST_ASSERT_EQUAL(API_SUBMIT_ACCEPTED,
                      pool_api_processor_submit(&g_proc, &c, &seq));
    teardown();
}

TEST_CASE("api adopt: api_owns_flow is false on every completed command path",
          "[pool_api_cmd]")
{
    PoolApiCommand c;

    /* Accepted create. */
    fresh_free_device();
    make_create_cmd(&c, 900u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_ACCEPTED, run_command(&c));
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);
    /* ...but the flow IS owned, by the executor. */
    TEST_ASSERT_TRUE(pool_api_processor_step(&g_proc));
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);

    /* Rejected create. */
    make_create_cmd(&c, 900u);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_REJECTED_CONFLICT, run_command(&c));
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);

    /* Rejected action. */
    make_action_cmd(&c, POOL_API_CMD_ACKNOWLEDGE_TERMINAL);
    (void)run_command(&c);
    TEST_ASSERT_FALSE(g_proc.api_owns_flow);
    teardown();
}

/* ================================================================= */
/* I. Bounded heartbeat fail-safe                                      */
/* ================================================================= */

TEST_CASE("api hb: the deadline and duration survive every heartbeat byte-identical",
          "[pool_api_cmd]")
{
    PoolSessionRecord rec;
    uint64_t          deadline, verified_start;
    uint32_t          duration, sync_gen;
    unsigned          i;

    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    make_rec(&rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&rec);
    boot_and_bind();

    deadline       = g_rt.record.deadline_epoch_s;
    duration       = g_rt.record.duration_s;
    verified_start = g_rt.record.verified_start_epoch_s;
    sync_gen       = g_rt.record.deadline_sync_generation;

    for (i = 1; i <= 5u; i++) {
        TEST_ASSERT_EQUAL(RUNTIME_OK,
                          pool_session_runtime_commit_epoch_heartbeat(
                              &g_rt, EPOCH_A_S + 500ull * i));
        TEST_ASSERT_EQUAL_UINT64(deadline, g_rt.record.deadline_epoch_s);
        TEST_ASSERT_EQUAL_UINT32(duration, g_rt.record.duration_s);
        TEST_ASSERT_EQUAL_UINT64(verified_start, g_rt.record.verified_start_epoch_s);
        TEST_ASSERT_EQUAL_UINT32(sync_gen, g_rt.record.deadline_sync_generation);
        TEST_ASSERT_TRUE(g_rt.record.deadline_valid);
        TEST_ASSERT_EQUAL(POOL_STATE_TARGET_ACTIVE, g_rt.record.state);
        TEST_ASSERT_TRUE(g_rt.record.restore_required);
    }
    teardown();
}

TEST_CASE("api hb: a reboot uses the persisted floor, not a RAM counter",
          "[pool_api_cmd]")
{
    PoolSessionRecord rec;

    fake_reset();
    source_defaults();
    g_monotonic_us = 5ull * US_PER_S;
    make_rec(&rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&rec);
    boot_and_bind();
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_commit_epoch_heartbeat(
                          &g_rt, EPOCH_A_S + 500ull));
    teardown();

    /* A fresh boot over the SAME flash: every RAM counter is gone. */
    source_defaults();
    boot_and_bind();
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 500ull, g_rt.record.latest_trusted_epoch_s);
    TEST_ASSERT_FALSE(g_proc.hb.have_success);
    TEST_ASSERT_EQUAL_UINT8(0u, g_proc.hb.consecutive_failures);
    /* An epoch below the PERSISTED floor is still refused after the reboot. */
    TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_DUPLICATE,
                      pool_session_runtime_commit_epoch_heartbeat(&g_rt, EPOCH_A_S));
    teardown();
}

TEST_CASE("api cmd: the API feature flag is default n", "[pool_api_cmd]")
{
#ifdef CONFIG_NX_TIMED_SESSIONS_API
    TEST_ASSERT_TRUE(pool_api_feature_enabled());
    TEST_ASSERT_NOT_NULL(pool_api_default_processor());
#else
    /* Default build: no singleton storage and nothing reachable. */
    TEST_ASSERT_FALSE(pool_api_feature_enabled());
    TEST_ASSERT_NULL(pool_api_default_processor());
#endif
}
