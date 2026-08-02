/*
 * Deterministic tests for the Gate B10 trusted-time wiring: production SNTP
 * lifecycle, the bounded sync-callback contract, the anti-regression floor,
 * the bounded start budget and observation-only pilot mode.
 *
 * NOTHING here contacts a network, an NTP server, DNS, a pool, OTA, a
 * restart path, physical NVS or hardware. The SNTP platform ops are fakes,
 * synchronization is injected by calling the same ingestion function the
 * real ESP-IDF callback calls, and every identity is a synthetic "*.example"
 * fixture. Every fake op asserts that no B5 coordinator call is in progress.
 */

#include <string.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pool_session_runtime.h"
#include "pool_session_runtime_boot.h"
#include "pool_time_source.h"

#define B10_EPOCH_S    1750000000ull
#define B10_SESSION_ID 4343u
#define B10_SRC        "ntp-b10.example"

/* ================================================================= */
/* Fake persistence backend (staged-vs-committed, like the B3 model)  */
/* ================================================================= */

#define B10_FK_A 0
#define B10_FK_B 1
#define B10_FK_P 2
#define B10_FK_SLOTS 3
#define B10_FK_MAX 512

typedef struct {
    bool    present;
    size_t  len;
    uint8_t bytes[B10_FK_MAX];
} B10Blob;

typedef struct {
    bool    opened;
    B10Blob committed[B10_FK_SLOTS];
    B10Blob staged[B10_FK_SLOTS];
    bool    staged_dirty[B10_FK_SLOTS];
    int     opens, reads, writes, commits, closes;
} B10FakeNvs;

static B10FakeNvs g_b10_nvs;

static void b10_assert_no_coord_lock(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_coordinator_depth());
}

static int b10_index(const char *key)
{
    if (key == NULL) return -1;
    if (strcmp(key, POOL_STORE_KEY_SLOT_A) == 0) return B10_FK_A;
    if (strcmp(key, POOL_STORE_KEY_SLOT_B) == 0) return B10_FK_B;
    if (strcmp(key, POOL_STORE_KEY_ACTIVE) == 0) return B10_FK_P;
    return -1;
}

static int b10_open(void *ctx)
{
    (void)ctx;
    b10_assert_no_coord_lock();
    g_b10_nvs.opens++;
    g_b10_nvs.opened = true;
    return POOL_STORE_BACKEND_OK;
}

static int b10_read(void *ctx, const char *key, uint8_t *buf, size_t cap,
                    size_t *out_len)
{
    int            idx = b10_index(key);
    const B10Blob *v;

    (void)ctx;
    b10_assert_no_coord_lock();
    g_b10_nvs.reads++;
    if (!g_b10_nvs.opened || idx < 0 || out_len == NULL) return POOL_STORE_BACKEND_IO;
    v = g_b10_nvs.staged_dirty[idx] ? &g_b10_nvs.staged[idx] : &g_b10_nvs.committed[idx];
    if (!v->present) return POOL_STORE_BACKEND_NOT_FOUND;
    *out_len = v->len;
    if (v->len > cap) return POOL_STORE_BACKEND_OK; /* buf untouched by contract */
    memcpy(buf, v->bytes, v->len);
    return POOL_STORE_BACKEND_OK;
}

static int b10_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    int idx = b10_index(key);

    (void)ctx;
    b10_assert_no_coord_lock();
    g_b10_nvs.writes++;
    if (!g_b10_nvs.opened || idx < 0 || buf == NULL || len == 0 || len > B10_FK_MAX) {
        return POOL_STORE_BACKEND_IO;
    }
    g_b10_nvs.staged[idx].present = true;
    g_b10_nvs.staged[idx].len     = len;
    memcpy(g_b10_nvs.staged[idx].bytes, buf, len);
    g_b10_nvs.staged_dirty[idx] = true;
    return POOL_STORE_BACKEND_OK;
}

static int b10_commit(void *ctx)
{
    int i;
    (void)ctx;
    b10_assert_no_coord_lock();
    g_b10_nvs.commits++;
    if (!g_b10_nvs.opened) return POOL_STORE_BACKEND_IO;
    for (i = 0; i < B10_FK_SLOTS; i++) {
        if (g_b10_nvs.staged_dirty[i]) {
            g_b10_nvs.committed[i]    = g_b10_nvs.staged[i];
            g_b10_nvs.staged_dirty[i] = false;
        }
    }
    return POOL_STORE_BACKEND_OK;
}

static int b10_close(void *ctx)
{
    (void)ctx;
    b10_assert_no_coord_lock();
    g_b10_nvs.closes++;
    g_b10_nvs.opened = false;
    return POOL_STORE_BACKEND_OK;
}

static const PoolStoreBackendOps g_b10_store_ops = {
    .open = b10_open, .read_blob = b10_read, .write_blob = b10_write,
    .commit = b10_commit, .close = b10_close,
};

/* ================================================================= */
/* Fake platform ops                                                  */
/* ================================================================= */

static uint64_t g_b10_mono_us;
static int32_t  g_b10_reset_raw;
static int      g_b10_inits, g_b10_starts, g_b10_stops, g_b10_deinits;
static uint32_t g_b10_last_server_count;
static char     g_b10_last_server[POOL_TIME_SNTP_SERVER_HOST_MAX];

static int32_t  b10_reset_reason(void) { return g_b10_reset_raw; }
static uint64_t b10_monotonic(void)    { return g_b10_mono_us; }

static int b10_sntp_init(const PoolTimeSntpConfig *cfg)
{
    b10_assert_no_coord_lock();
    g_b10_inits++;
    if (cfg != NULL) {
        g_b10_last_server_count = cfg->server_count;
        strncpy(g_b10_last_server, cfg->servers[0], sizeof(g_b10_last_server) - 1u);
        g_b10_last_server[sizeof(g_b10_last_server) - 1u] = '\0';
    }
    return 0;
}
static int b10_sntp_start(void)  { b10_assert_no_coord_lock(); g_b10_starts++;  return 0; }
static int b10_sntp_stop(void)   { b10_assert_no_coord_lock(); g_b10_stops++;   return 0; }
static int b10_sntp_deinit(void) { b10_assert_no_coord_lock(); g_b10_deinits++; return 0; }

static const PoolTimeSntpPlatformOps g_b10_sntp_ops = {
    .monotonic_us = b10_monotonic, .sntp_init = b10_sntp_init,
    .sntp_start = b10_sntp_start, .sntp_stop = b10_sntp_stop,
    .sntp_deinit = b10_sntp_deinit,
};

/* ================================================================= */
/* Fixtures                                                           */
/* ================================================================= */

static PoolSessionRuntime      g_b10_rt;
static PoolSessionRuntimeDeps  g_b10_deps;
static PoolSessionRecord       g_b10_rec;
static PoolRuntimeSnapshot     g_b10_snap;
static PoolTimeSourceDiagnostics g_b10_diag;

static void b10_fill_identity(PoolConfigIdentity *c, PoolChainType chain,
                              const char *host, uint16_t port)
{
    memset(c, 0, sizeof(*c));
    c->chain = chain;
    strncpy(c->primary.host, host, sizeof(c->primary.host) - 1);
    c->primary.port = port;
    strncpy(c->primary.user, "acct.worker", sizeof(c->primary.user) - 1);
    c->primary.protocol = POOL_PROTO_STRATUM_V1;
}

static void b10_make_rec(PoolSessionRecord *r, PoolSessionState st,
                         bool restore_required)
{
    pool_session_record_init(r);
    r->session_id       = B10_SESSION_ID;
    r->b1_model_version = POOL_SESSION_MODEL_VERSION;
    r->state            = st;
    b10_fill_identity(&r->source, POOL_CHAIN_BITCOIN, "btc.example", 3333);
    b10_fill_identity(&r->target, POOL_CHAIN_BITCOIN_CASH, "bch.example", 3334);
    r->password_policy  = POOL_SESSION_PW_KEEP_CURRENT;
    r->restore_required = restore_required;
    r->target_verify.connection_observed = true;
    r->target_verify.mining_observed     = true;
    r->target_verify.identity_verified   = true;
    r->duration_s               = 3600u;
    r->verified_start_valid     = true;
    r->verified_start_epoch_s   = B10_EPOCH_S;
    r->deadline_valid           = true;
    r->deadline_epoch_s         = B10_EPOCH_S + 3600u;
    r->deadline_sync_generation = 1u;
    r->latest_trusted_valid     = true;
    r->latest_trusted_epoch_s   = B10_EPOCH_S + 100u;
    if (st == POOL_STATE_COMPLETE) {
        r->restore_verify.connection_observed = true;
        r->restore_verify.mining_observed     = true;
        r->restore_verify.identity_verified   = true;
    }
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(r));
}

static void b10_seed_record(const PoolSessionRecord *rec)
{
    PoolSessionStore  st;
    PoolSessionRecord copy = *rec;
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&st, &g_b10_store_ops, NULL));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&st, &copy));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&st));
}

static void b10_fresh(void)
{
    memset(&g_b10_nvs, 0, sizeof(g_b10_nvs));
    g_b10_mono_us   = 5ull * 1000000ull;
    g_b10_reset_raw = POOL_RESET_RAW_POWERON;
    g_b10_inits = g_b10_starts = g_b10_stops = g_b10_deinits = 0;
    g_b10_last_server_count = 0u;
    memset(g_b10_last_server, 0, sizeof(g_b10_last_server));

    memset(&g_b10_deps, 0, sizeof(g_b10_deps));
    g_b10_deps.store_ops             = &g_b10_store_ops;
    g_b10_deps.sntp_ops              = &g_b10_sntp_ops;
    g_b10_deps.read_reset_reason_raw = b10_reset_reason;
    g_b10_deps.monotonic_us          = b10_monotonic;
    g_b10_deps.ntp_server            = ""; /* the shipped default: no source */
    g_b10_deps.sync_wait_limit_s     = 600u;
    g_b10_deps.observe_enabled       = false; /* the shipped default: off */

    memset(&g_b10_rt, 0, sizeof(g_b10_rt));
}

static void b10_teardown(void)
{
    (void)pool_session_runtime_deinit(&g_b10_rt);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_task_count());
}

/* Deliver the network-ready fact and let the owner task act on it. */
static void b10_network_ready(void)
{
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_b10_rt, RUNTIME_EVENT_NETWORK_READY));
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* Inject a synchronization exactly as the real ESP-IDF callback does. */
static PoolTimeError b10_deliver_sync(uint64_t epoch_s)
{
    return pool_time_sntp_handle_sync(&g_b10_rt.time_provider, epoch_s, 0u);
}

/* ================================================================= */
/* C. Provider lifecycle                                              */
/* ================================================================= */

TEST_CASE("b10 rt: boot alone never touches the network", "[pool_runtime_b10]")
{
    b10_fresh();
    g_b10_deps.ntp_server = B10_SRC;
    b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
    b10_seed_record(&g_b10_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);

    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_b10_rt.decision.state);
    TEST_ASSERT_EQUAL(0, g_b10_inits);
    TEST_ASSERT_EQUAL(0, g_b10_starts);
    TEST_ASSERT_FALSE(g_b10_rt.time_provider_initialized);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_time_start_attempts(&g_b10_rt));

    /* The source is validated at bind time — with no DNS and no networking. */
    TEST_ASSERT_TRUE(g_b10_rt.time_source_configured);
    TEST_ASSERT_EQUAL(TIME_SOURCE_CONFIGURED, g_b10_rt.time_source_validation.state);
    b10_teardown();
}

TEST_CASE("b10 rt: exactly one server is configured after network-ready",
          "[pool_runtime_b10]")
{
    b10_fresh();
    g_b10_deps.ntp_server = B10_SRC;
    b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
    b10_seed_record(&g_b10_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();

    TEST_ASSERT_EQUAL(1, g_b10_inits);
    TEST_ASSERT_EQUAL(1, g_b10_starts);
    TEST_ASSERT_EQUAL_UINT32(1u, g_b10_last_server_count); /* never a pool list */
    TEST_ASSERT_EQUAL_STRING(B10_SRC, g_b10_last_server);
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_runtime_time_start_attempts(&g_b10_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    b10_teardown();
}

TEST_CASE("b10 rt: duplicate network-ready never creates a second SNTP client",
          "[pool_runtime_b10]")
{
    int i;

    b10_fresh();
    g_b10_deps.ntp_server = B10_SRC;
    b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
    b10_seed_record(&g_b10_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));

    for (i = 0; i < 6; i++) {
        b10_network_ready();
    }
    /* One provider, one start, one consumed attempt — no matter how many
     * times the event arrives or how many ticks elapse. */
    TEST_ASSERT_EQUAL(1, g_b10_inits);
    TEST_ASSERT_EQUAL(1, g_b10_starts);
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_runtime_time_start_attempts(&g_b10_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    b10_teardown();
}

TEST_CASE("b10 rt: deinit stops the service and drops in-boot trust",
          "[pool_runtime_b10]")
{
    b10_fresh();
    g_b10_deps.ntp_server = B10_SRC;
    b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
    b10_seed_record(&g_b10_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();
    TEST_ASSERT_TRUE(g_b10_rt.time_provider_started);
    TEST_ASSERT_EQUAL(TIME_OK, b10_deliver_sync(B10_EPOCH_S + 500u));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_deinit(&g_b10_rt));
    TEST_ASSERT_EQUAL(1, g_b10_deinits);
    TEST_ASSERT_FALSE(g_b10_rt.time_provider_initialized);
    TEST_ASSERT_FALSE(g_b10_rt.time_provider_started);

    /* A fresh init after a complete teardown models a reboot: untrusted
     * again, with no anchor and no attempts carried over. */
    b10_seed_record(&g_b10_rec);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_FALSE(g_b10_rt.time_snapshot.trusted);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_time_start_attempts(&g_b10_rt));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_time_sync_callbacks(&g_b10_rt));
    b10_teardown();
}

TEST_CASE("b10 rt: an invalid configured source behaves exactly like none",
          "[pool_runtime_b10]")
{
    static const char *bad[] = { "http://ntp.example", "ntp .example", "localhost",
                                 "ntp.example:123", "user@ntp.example", "010.0.2.1" };
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        b10_fresh();
        g_b10_deps.ntp_server = bad[i];
        b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
        b10_seed_record(&g_b10_rec);

        TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
        (void)pool_session_runtime_boot(&g_b10_rt);
        TEST_ASSERT_FALSE(g_b10_rt.time_source_configured);
        TEST_ASSERT_EQUAL(TIME_SOURCE_INVALID, g_b10_rt.time_source_validation.state);

        TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
        b10_network_ready();

        /* No DNS, no SNTP init, no start — and the hold still stands. */
        TEST_ASSERT_EQUAL(0, g_b10_inits);
        TEST_ASSERT_EQUAL(0, g_b10_starts);
        TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                          pool_session_runtime_protocol_permission(&g_b10_rt));

        TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_time_diagnostics(&g_b10_rt,
                                                                            &g_b10_diag));
        TEST_ASSERT_EQUAL(TIME_SOURCE_INVALID, g_b10_diag.state);
        TEST_ASSERT_FALSE(g_b10_diag.source_configured);
        TEST_ASSERT_FALSE(g_b10_diag.trusted_time_available);

        TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
        b10_teardown();
    }
}

/* ================================================================= */
/* F/G. Sync callback, trust acceptance and the anti-regression floor  */
/* ================================================================= */

TEST_CASE("b10 rt: an accepted sync notifies the owner task and lifts the wait",
          "[pool_runtime_b10]")
{
    int i;

    b10_fresh();
    g_b10_deps.ntp_server = B10_SRC;
    b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
    b10_seed_record(&g_b10_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_b10_rt.control.state);

    /* A trusted epoch inside the session window, above the persisted floor. */
    g_b10_mono_us += 3ull * 1000000ull;
    TEST_ASSERT_EQUAL(TIME_OK, b10_deliver_sync(B10_EPOCH_S + 600u));

    /* The callback observer is the ONLY thing that woke the owner task: the
     * bounded wait is 600 s, so nothing else could have moved this fast. */
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_runtime_time_sync_callbacks(&g_b10_rt));
    for (i = 0; i < 30 && g_b10_rt.control.state == RUNTIME_WAITING_FOR_TRUSTED_TIME;
         i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    TEST_ASSERT_NOT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_b10_rt.control.state);
    TEST_ASSERT_TRUE(g_b10_rt.control.wait_elapsed_s < 30u); /* far below the bound */
    TEST_ASSERT_TRUE(g_b10_rt.time_snapshot.trusted);

    /* Trusted time alone is NEVER a mining grant. */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_b10_rt, &g_b10_snap));
    TEST_ASSERT_FALSE(g_b10_snap.target_mining_authorized);
    TEST_ASSERT_FALSE(g_b10_snap.pool_mutation_permitted);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    b10_teardown();
}

TEST_CASE("b10 rt: an epoch below the persisted floor never becomes trusted",
          "[pool_runtime_b10]")
{
    b10_fresh();
    g_b10_deps.ntp_server = B10_SRC;
    b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
    b10_seed_record(&g_b10_rec); /* floor = B10_EPOCH_S + 100 */

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL_UINT64(B10_EPOCH_S + 100u, g_b10_rt.time_policy.required_min_epoch_s);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();

    /* One second below the floor: a plausible, in-band, but REGRESSED epoch. */
    g_b10_mono_us += 1000000ull;
    (void)b10_deliver_sync(B10_EPOCH_S + 99u);
    vTaskDelay(pdMS_TO_TICKS(120));

    TEST_ASSERT_FALSE(g_b10_rt.time_snapshot.trusted);
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN, g_b10_rt.time_snapshot.status);
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_b10_rt.control.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_b10_rt));

    /* Exactly AT the floor is acceptable — the floor is a minimum, not a gap. */
    g_b10_mono_us += 1000000ull;
    TEST_ASSERT_EQUAL(TIME_OK, b10_deliver_sync(B10_EPOCH_S + 100u));
    vTaskDelay(pdMS_TO_TICKS(120));
    TEST_ASSERT_TRUE(g_b10_rt.time_snapshot.trusted);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    b10_teardown();
}

TEST_CASE("b10 rt: an out-of-band candidate is refused and stays untrusted",
          "[pool_runtime_b10]")
{
    b10_fresh();
    g_b10_deps.ntp_server = B10_SRC;
    b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
    b10_seed_record(&g_b10_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();

    g_b10_mono_us += 1000000ull;
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BELOW_MIN, b10_deliver_sync(1000ull));
    g_b10_mono_us += 1000000ull;
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_ABOVE_MAX,
                      b10_deliver_sync(POOL_TIME_EPOCH_MAX_S + 1ull));
    vTaskDelay(pdMS_TO_TICKS(120));

    TEST_ASSERT_FALSE(g_b10_rt.time_snapshot.trusted);
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_b10_rt.control.state);
    /* Both refusals were still observed — a rejection is a real event. */
    TEST_ASSERT_EQUAL_UINT32(2u, pool_session_runtime_time_sync_callbacks(&g_b10_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_time_diagnostics(&g_b10_rt,
                                                                        &g_b10_diag));
    TEST_ASSERT_EQUAL(TIME_SOURCE_REJECTED, g_b10_diag.state);
    TEST_ASSERT_FALSE(g_b10_diag.trusted_time_available);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    b10_teardown();
}

TEST_CASE("b10 rt: the sync callback writes no NVS and takes no lease",
          "[pool_runtime_b10]")
{
    int writes_before, commits_before, opens_before;
    PoolOperationState lease_before, lease_after;

    b10_fresh();
    g_b10_deps.ntp_server = B10_SRC;
    b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
    b10_seed_record(&g_b10_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();
    TEST_ASSERT_TRUE(g_b10_rt.time_provider_started);

    /*
     * ISOLATE the callback. The owner task is stopped first, so nothing the
     * task legitimately does afterwards (re-planning, persistence, lease
     * reconciliation) can be mistaken for work done BY the callback. The
     * provider stays started, so the ingestion path is exactly the one the
     * real ESP-IDF callback takes.
     */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));

    writes_before  = g_b10_nvs.writes;
    commits_before = g_b10_nvs.commits;
    opens_before   = g_b10_nvs.opens;
    lease_before   = g_b10_rt.lease;

    /* Every backend op asserts the coordinator depth is zero, and none of
     * them may run at all here. */
    g_b10_mono_us += 1000000ull;
    TEST_ASSERT_EQUAL(TIME_OK, b10_deliver_sync(B10_EPOCH_S + 400u));

    TEST_ASSERT_EQUAL(writes_before, g_b10_nvs.writes);   /* no NVS write   */
    TEST_ASSERT_EQUAL(commits_before, g_b10_nvs.commits); /* no NVS commit  */
    TEST_ASSERT_EQUAL(opens_before, g_b10_nvs.opens);     /* no NVS open    */
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_coordinator_depth());
    lease_after = g_b10_rt.lease;
    TEST_ASSERT_EQUAL(lease_before.owner, lease_after.owner);
    TEST_ASSERT_EQUAL(lease_before.phase, lease_after.phase);
    TEST_ASSERT_EQUAL(lease_before.lease_generation, lease_after.lease_generation);

    /* The callback WAS observed and the anchor WAS published — the work it
     * skipped is deferred to the owner task, not lost. */
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_runtime_time_sync_callbacks(&g_b10_rt));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_TRUSTED,
                      pool_time_sntp_lifecycle(&g_b10_rt.time_provider));

    b10_teardown();
}

TEST_CASE("b10 rt: a callback with no owner task is dropped, never latched",
          "[pool_runtime_b10]")
{
    b10_fresh();
    g_b10_deps.ntp_server = B10_SRC;
    b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
    b10_seed_record(&g_b10_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);

    /* No task exists yet: the callback path must NOT read-modify-write the
     * control block from a foreign thread. */
    TEST_ASSERT_EQUAL(RUNTIME_ERR_NOT_INITIALIZED,
                      pool_session_runtime_notify_from_callback(
                          &g_b10_rt, RUNTIME_EVENT_TIME_SYNC_CHANGED));
    TEST_ASSERT_EQUAL_UINT32(0u, g_b10_rt.control.pending_events);

    /* Unknown bits are dropped, and a NULL runtime is refused. */
    TEST_ASSERT_EQUAL(RUNTIME_ERR_UNSUPPORTED_EVENT,
                      pool_session_runtime_notify_from_callback(&g_b10_rt, 0x8000u));
    TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT,
                      pool_session_runtime_notify_from_callback(
                          NULL, RUNTIME_EVENT_TIME_SYNC_CHANGED));
    b10_teardown();
}

TEST_CASE("b10 rt: with no configured source recovery stays held",
          "[pool_runtime_b10]")
{
    b10_fresh(); /* ntp_server = "" — the shipped default */
    b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
    b10_seed_record(&g_b10_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();

    TEST_ASSERT_EQUAL(0, g_b10_inits);
    TEST_ASSERT_EQUAL(0, g_b10_starts);
    TEST_ASSERT_FALSE(g_b10_rt.time_snapshot.trusted);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_b10_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_time_diagnostics(&g_b10_rt,
                                                                        &g_b10_diag));
    TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, g_b10_diag.state);
    TEST_ASSERT_FALSE(g_b10_diag.source_configured);
    TEST_ASSERT_TRUE(pool_time_source_diagnostics_valid(&g_b10_diag));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    b10_teardown();
}

/* ================================================================= */
/* E/I. Observation-only mode                                         */
/* ================================================================= */

/*
 * Observation is compiled out unless CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
 * is set, so the behavioural tests below assert the appropriate contract for
 * each build: with the flag OFF the provider must never start on an empty
 * store; with it ON it may start but must still change nothing.
 */

TEST_CASE("b10 obs: an empty store keeps protocol allowed and mining unchanged",
          "[pool_runtime_b10]")
{
    int writes_before, commits_before;

    b10_fresh();
    g_b10_deps.ntp_server      = B10_SRC;
    g_b10_deps.observe_enabled = true; /* no record is seeded: an EMPTY store */

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);

    /* The proven-safe empty posture: normal source mining is permitted. */
    TEST_ASSERT_EQUAL(STORE_EMPTY, g_b10_rt.store_result);
    TEST_ASSERT_EQUAL(RUNTIME_FREE, g_b10_rt.decision.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_b10_rt));

    writes_before  = g_b10_nvs.writes;
    commits_before = g_b10_nvs.commits;

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();

#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    TEST_ASSERT_EQUAL(1, g_b10_inits);
    TEST_ASSERT_EQUAL(1, g_b10_starts);
    TEST_ASSERT_TRUE(pool_session_runtime_time_observation_active(&g_b10_rt));

    /* A successful observation still changes NOTHING. */
    g_b10_mono_us += 1000000ull;
    TEST_ASSERT_EQUAL(TIME_OK, b10_deliver_sync(B10_EPOCH_S + 900u));
    vTaskDelay(pdMS_TO_TICKS(150));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_time_diagnostics(&g_b10_rt,
                                                                        &g_b10_diag));
    TEST_ASSERT_EQUAL(TIME_SOURCE_TRUSTED, g_b10_diag.state);
    TEST_ASSERT_TRUE(g_b10_diag.trusted_time_available);
    TEST_ASSERT_TRUE(g_b10_diag.trusted_time_operational);
    TEST_ASSERT_TRUE(g_b10_diag.observation_mode_enabled);
#else
    /* With the flag off the provider must not start on an empty store. */
    TEST_ASSERT_EQUAL(0, g_b10_inits);
    TEST_ASSERT_EQUAL(0, g_b10_starts);
    TEST_ASSERT_FALSE(pool_session_runtime_time_observation_active(&g_b10_rt));
#endif

    /* THE observation invariants, asserted on BOTH builds. */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_b10_rt));
    TEST_ASSERT_EQUAL(RUNTIME_FREE, g_b10_rt.control.state);
    TEST_ASSERT_EQUAL(writes_before, g_b10_nvs.writes);     /* no B3 write     */
    TEST_ASSERT_EQUAL(commits_before, g_b10_nvs.commits);   /* no B3 commit    */
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_b10_rt.lease.owner); /* no B5 owner     */
    TEST_ASSERT_FALSE(g_b10_rt.lease.restore_required);
    TEST_ASSERT_EQUAL_UINT32(0u, g_b10_rt.tracker.commit_count); /* no heartbeat */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_b10_rt, &g_b10_snap));
    TEST_ASSERT_FALSE(g_b10_snap.session_present);
    TEST_ASSERT_FALSE(g_b10_snap.target_mining_authorized);
    TEST_ASSERT_FALSE(g_b10_snap.pool_mutation_permitted);
    TEST_ASSERT_FALSE(g_b10_snap.restore_required);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    b10_teardown();
}

TEST_CASE("b10 obs: an unconfigured source in observation mode is inert",
          "[pool_runtime_b10]")
{
    b10_fresh();
    g_b10_deps.observe_enabled = true;
    g_b10_deps.ntp_server      = ""; /* nothing to contact */

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();

    TEST_ASSERT_EQUAL(0, g_b10_inits); /* no network request of any kind */
    TEST_ASSERT_EQUAL(0, g_b10_starts);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_time_diagnostics(&g_b10_rt,
                                                                        &g_b10_diag));
    TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, g_b10_diag.state);
    /* Normal source mining is completely unaffected. */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_b10_rt));
    TEST_ASSERT_EQUAL(0, g_b10_nvs.writes);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    b10_teardown();
}

TEST_CASE("b10 obs: an observation timeout is diagnostic only",
          "[pool_runtime_b10]")
{
    int i;

    b10_fresh();
    g_b10_deps.observe_enabled   = true;
    g_b10_deps.ntp_server        = B10_SRC;
    g_b10_deps.sync_wait_limit_s = 1u; /* a one-tick observation window */

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();

    /* No synchronization is ever delivered: let the bounded window expire. */
    for (i = 0; i < 40; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_time_diagnostics(&g_b10_rt,
                                                                        &g_b10_diag));
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    TEST_ASSERT_EQUAL(TIME_SOURCE_TIMEOUT, g_b10_diag.state);
#endif
    TEST_ASSERT_FALSE(g_b10_diag.trusted_time_available);
    TEST_ASSERT_TRUE(pool_time_source_diagnostics_valid(&g_b10_diag));

    /* The timeout changed nothing: still FREE, still allowed, still empty. */
    TEST_ASSERT_EQUAL(RUNTIME_FREE, g_b10_rt.control.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_b10_rt));
    TEST_ASSERT_EQUAL(0, g_b10_nvs.writes);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_b10_rt.lease.owner);
    TEST_ASSERT_EQUAL(STORE_EMPTY, g_b10_rt.store_result);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    b10_teardown();
}

TEST_CASE("b10 obs: observation never runs alongside a session owner",
          "[pool_runtime_b10]")
{
    b10_fresh();
    g_b10_deps.observe_enabled = true;
    g_b10_deps.ntp_server      = B10_SRC;
    /* A safe retained terminal: a record exists, but no trusted time is
     * required. Observation must NOT claim this device. */
    b10_make_rec(&g_b10_rec, POOL_STATE_COMPLETE, false);
    b10_seed_record(&g_b10_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_TRUE(g_b10_rt.record_present);
    TEST_ASSERT_FALSE(g_b10_rt.plan.trusted_time_required);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();

    TEST_ASSERT_EQUAL(0, g_b10_inits);
    TEST_ASSERT_EQUAL(0, g_b10_starts);
    TEST_ASSERT_FALSE(pool_session_runtime_time_observation_active(&g_b10_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    b10_teardown();
}

TEST_CASE("b10 obs: observation never turns an empty store into a session store",
          "[pool_runtime_b10]")
{
    PoolSessionStore  verify;
    PoolSessionRecord loaded;
    PoolStoreLoadInfo info;
    PoolStoreResult   r;
    int i;

    b10_fresh();
    g_b10_deps.observe_enabled = true;
    g_b10_deps.ntp_server      = B10_SRC;

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();

    /* Repeated synchronizations, accepted and refused, over many ticks. */
    for (i = 0; i < 5; i++) {
        g_b10_mono_us += 1000000ull;
        (void)b10_deliver_sync(B10_EPOCH_S + 1000u + (uint64_t)i);
        g_b10_mono_us += 1000000ull;
        (void)b10_deliver_sync(5ull); /* refused: below the sanity band */
        vTaskDelay(pdMS_TO_TICKS(60));
    }

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));

    /* The persisted store is still EMPTY — read back independently. */
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&verify, &g_b10_store_ops, NULL));
    r = pool_session_store_load(&verify, &loaded, &info);
    TEST_ASSERT_EQUAL(STORE_EMPTY, r);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&verify));
    TEST_ASSERT_EQUAL(0, g_b10_nvs.writes);
    TEST_ASSERT_EQUAL(0, g_b10_nvs.commits);
    b10_teardown();
}

/* ================================================================= */
/* Feature-flag and privacy guards                                    */
/* ================================================================= */

TEST_CASE("b10 flags: observation cannot enable without the runtime feature",
          "[pool_runtime_b10]")
{
    /* The production default of the runtime flag decides whether an instance
     * exists at all; observation is strictly downstream of it. */
    if (!pool_session_runtime_feature_enabled()) {
        TEST_ASSERT_NULL(pool_session_runtime_default_instance());
    } else {
        TEST_ASSERT_NOT_NULL(pool_session_runtime_default_instance());
    }
    TEST_ASSERT_FALSE(pool_runtime_boot_action_for_feature(false).bootstrap_required);
    TEST_ASSERT_TRUE(pool_runtime_boot_action_for_feature(false).protocol_allowed);

    /* The pure rule agrees: observe alone can never start a provider. */
    {
        PoolTimeSourceStartInput in;
        memset(&in, 0, sizeof(in));
        in.observe_enabled = true;
        in.network_ready   = true;
        in.source_present  = true;
        in.source_usable   = true;
        TEST_ASSERT_FALSE(pool_time_source_decide_start(&in).start_provider);
    }
}

TEST_CASE("b10 privacy: the diagnostics view carries no source or session data",
          "[pool_runtime_b10]")
{
    static const char *needles[] = { B10_SRC, "btc.example", "bch.example",
                                     "acct.worker" };
    uint32_t sid = B10_SESSION_ID;
    size_t   n, i;

    b10_fresh();
    g_b10_deps.ntp_server = B10_SRC;
    b10_make_rec(&g_b10_rec, POOL_STATE_TARGET_ACTIVE, true);
    b10_seed_record(&g_b10_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b10_rt, &g_b10_deps));
    (void)pool_session_runtime_boot(&g_b10_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b10_rt));
    b10_network_ready();
    g_b10_mono_us += 1000000ull;
    TEST_ASSERT_EQUAL(TIME_OK, b10_deliver_sync(B10_EPOCH_S + 700u));
    vTaskDelay(pdMS_TO_TICKS(150));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_time_diagnostics(&g_b10_rt,
                                                                        &g_b10_diag));
    TEST_ASSERT_TRUE(pool_time_source_diagnostics_valid(&g_b10_diag));

    for (n = 0; n < sizeof(needles) / sizeof(needles[0]); n++) {
        size_t nl = strlen(needles[n]);
        const uint8_t *h = (const uint8_t *)&g_b10_diag;
        for (i = 0; i + nl <= sizeof(g_b10_diag); i++) {
            TEST_ASSERT_FALSE(memcmp(h + i, needles[n], nl) == 0);
        }
    }
    for (i = 0; i + sizeof(sid) <= sizeof(g_b10_diag); i++) {
        TEST_ASSERT_FALSE(memcmp((const uint8_t *)&g_b10_diag + i, &sid, sizeof(sid)) == 0);
    }
    /* The trusted epoch never appears: only a monotonic AGE is published. */
    {
        uint64_t epoch = g_b10_rt.time_snapshot.trusted_epoch_s;
        TEST_ASSERT_TRUE(epoch > 0u);
        for (i = 0; i + sizeof(epoch) <= sizeof(g_b10_diag); i++) {
            TEST_ASSERT_FALSE(memcmp((const uint8_t *)&g_b10_diag + i, &epoch,
                                     sizeof(epoch)) == 0);
        }
    }

    /*
     * The record generation and the lease generation are deliberately NOT
     * byte-scanned for: they are small integers that would collide with
     * ordinary counters by chance and so prove nothing either way. The real
     * guarantee is STRUCTURAL — the model is exactly its named scalar fields
     * (none of which is a generation, session id, host or address) with no
     * string, buffer or spare storage anywhere in it. The size bound below
     * makes that concrete: the whole view is smaller than a single hostname
     * buffer, so no identity could physically fit inside it.
     */
    TEST_ASSERT_TRUE(sizeof(g_b10_diag) < (size_t)POOL_TIME_SNTP_SERVER_HOST_MAX);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b10_rt));
    b10_teardown();
}

TEST_CASE("b10 diag: a NULL runtime yields the fail-closed view",
          "[pool_runtime_b10]")
{
    TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT,
                      pool_session_runtime_time_diagnostics(NULL, &g_b10_diag));
    TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, g_b10_diag.state);
    TEST_ASSERT_FALSE(g_b10_diag.trusted_time_available);
    TEST_ASSERT_TRUE(pool_time_source_diagnostics_valid(&g_b10_diag));

    TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT,
                      pool_session_runtime_time_diagnostics(&g_b10_rt, NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_time_start_attempts(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_time_sync_callbacks(NULL));
    TEST_ASSERT_FALSE(pool_session_runtime_time_observation_active(NULL));
}
