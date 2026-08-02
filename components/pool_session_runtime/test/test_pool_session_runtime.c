/*
 * Deterministic tests for the Gate B6 ESP-IDF runtime adapter.
 *
 * Every dependency is injected: a fake persistence backend (the B3 model of
 * staged-vs-committed values), fake SNTP platform ops, a fake monotonic
 * clock and a counting fake reset-reason reader. The final section boots the
 * adapter against the REAL ESP-IDF NVS backend on the isolated QEMU test
 * partition — never a physical device.
 *
 * NOTHING here contacts a network, an NTP server, DNS, a pool, OTA, a
 * restart path or hardware. All identities are synthetic "*.example"
 * fixtures. Every fake op asserts that no B5 coordinator call is in
 * progress, proving contract 11 (no NVS/SNTP/callback under the lock).
 */

#include <string.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "pool_session_runtime.h"
#include "pool_session_runtime_boot.h"

#define EPOCH_A_S  1750000000ull
#define SESSION_ID 4242u

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
    bool    fail_open;
    bool    fail_load_read;
    int     fail_write_after;       /* -1 never; 0 fail on the next write   */
    int     fail_commit_after;      /* -1 never; 0 fail on the next commit  */
    int     fail_active_read_after; /* -1 never; 0 fail the next "active"
                                     * read — lets a commit fully succeed
                                     * while the INDEPENDENT reload fails   */
    int     opens, reads, writes, commits, closes;
} FakeNvs;

static FakeNvs g_fake;

/* Contract 11: no store operation may ever run inside a coordinator call. */
static void assert_no_coordinator_lock(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_coordinator_depth());
}

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
    assert_no_coordinator_lock();
    g_fake.opens++;
    if (g_fake.fail_open) return POOL_STORE_BACKEND_IO;
    g_fake.opened = true;
    return POOL_STORE_BACKEND_OK;
}

static int fk_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len)
{
    int idx = fk_index(key);
    const FakeVal *v;
    (void)ctx;
    assert_no_coordinator_lock();
    g_fake.reads++;
    if (g_fake.fail_load_read) return POOL_STORE_BACKEND_IO;
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
    assert_no_coordinator_lock();
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
    assert_no_coordinator_lock();
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
    assert_no_coordinator_lock();
    g_fake.closes++;
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

/* ---------------- fake platform ops ---------------- */

static int32_t  g_reset_raw;
static uint32_t g_reset_reads;
static uint64_t g_monotonic_us;

static int32_t fake_reset_reason(void)
{
    g_reset_reads++;
    return g_reset_raw;
}

static uint64_t fake_monotonic(void)
{
    return g_monotonic_us;
}

static int  g_sntp_inits, g_sntp_starts, g_sntp_stops, g_sntp_deinits;
static bool g_sntp_fail_init;

static uint64_t fs_monotonic(void) { return g_monotonic_us; }
static int fs_init(const PoolTimeSntpConfig *cfg)
{
    (void)cfg;
    assert_no_coordinator_lock();
    g_sntp_inits++;
    return g_sntp_fail_init ? 1 : 0;
}
static int fs_start(void)  { assert_no_coordinator_lock(); g_sntp_starts++;  return 0; }
static int fs_stop(void)   { assert_no_coordinator_lock(); g_sntp_stops++;   return 0; }
static int fs_deinit(void) { assert_no_coordinator_lock(); g_sntp_deinits++; return 0; }

static const PoolTimeSntpPlatformOps g_fake_sntp_ops = {
    .monotonic_us = fs_monotonic, .sntp_init = fs_init, .sntp_start = fs_start,
    .sntp_stop = fs_stop, .sntp_deinit = fs_deinit,
};

static void platform_reset(void)
{
    g_reset_raw = POOL_RESET_RAW_POWERON;
    g_reset_reads = 0u;
    g_monotonic_us = 5ull * 1000000ull;
    g_sntp_inits = g_sntp_starts = g_sntp_stops = g_sntp_deinits = 0;
    g_sntp_fail_init = false;
}

/* ---------------- fixtures ---------------- */

static PoolSessionRuntime     g_rt;
static PoolSessionRuntimeDeps g_deps;
static PoolSessionRecord      g_rec;
static PoolRuntimeSnapshot    g_snap;

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
    r->session_id       = SESSION_ID;
    r->b1_model_version = POOL_SESSION_MODEL_VERSION;
    r->state            = st;
    fill_identity(&r->source, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    fill_identity(&r->target, POOL_CHAIN_BITCOIN_CASH, "bch.example", 3334, "acct.worker");
    r->password_policy  = POOL_SESSION_PW_KEEP_CURRENT;
    r->restore_required = restore_required;
    r->target_verify.connection_observed = true;
    r->target_verify.mining_observed     = true;
    r->target_verify.identity_verified   = true;
    r->duration_s               = 3600u;
    r->verified_start_valid     = true;
    r->verified_start_epoch_s   = EPOCH_A_S;
    r->deadline_valid           = true;
    r->deadline_epoch_s         = EPOCH_A_S + 3600u;
    r->deadline_sync_generation = 1u;
    r->latest_trusted_valid     = true;
    r->latest_trusted_epoch_s   = EPOCH_A_S + 100u;
    if (st == POOL_STATE_COMPLETE) {
        r->restore_verify.connection_observed = true;
        r->restore_verify.mining_observed     = true;
        r->restore_verify.identity_verified   = true;
    }
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(r));
}

/* Seed the fake backend with a committed record through the real B3 store. */
static void seed_fake_record(const PoolSessionRecord *rec)
{
    PoolSessionStore  st;
    PoolSessionRecord copy = *rec;
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&st, &g_fake_ops, NULL));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&st, &copy));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&st));
}

static void deps_defaults(void)
{
    memset(&g_deps, 0, sizeof(g_deps));
    g_deps.store_ops             = &g_fake_ops;
    g_deps.store_ctx             = NULL;
    g_deps.sntp_ops              = &g_fake_sntp_ops;
    g_deps.read_reset_reason_raw = fake_reset_reason;
    g_deps.monotonic_us          = fake_monotonic;
    g_deps.ntp_server            = ""; /* NO product-configured source by default */
    g_deps.sync_wait_limit_s     = 600u;
}

static void runtime_teardown(void)
{
    (void)pool_session_runtime_deinit(&g_rt);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_task_count());
}

static void fresh_runtime(void)
{
    fake_reset();
    platform_reset();
    deps_defaults();
    memset(&g_rt, 0, sizeof(g_rt));
}

/* ================================================================= */
/* Lifecycle                                                          */
/* ================================================================= */

TEST_CASE("rt: lifecycle is deterministic and fail-closed", "[pool_runtime_rt]")
{
    fresh_runtime();

    TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT, pool_session_runtime_init(NULL, &g_deps));
    TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT, pool_session_runtime_init(&g_rt, NULL));

    /* Missing mandatory ops are rejected. */
    {
        PoolSessionRuntimeDeps bad = g_deps;
        bad.store_ops = NULL;
        TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT, pool_session_runtime_init(&g_rt, &bad));
        bad = g_deps;
        bad.read_reset_reason_raw = NULL;
        TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT, pool_session_runtime_init(&g_rt, &bad));
    }

    /* Un-booted and un-initialized instances HOLD. */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, pool_session_runtime_protocol_permission(NULL));
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, pool_session_runtime_protocol_permission(&g_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    TEST_ASSERT_EQUAL(RUNTIME_ERR_ALREADY_INITIALIZED, pool_session_runtime_init(&g_rt, &g_deps));
    /* Initialized but not booted still HOLDS. */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, pool_session_runtime_protocol_permission(&g_rt));
    /* No task may be created before a successful boot. */
    TEST_ASSERT_EQUAL(RUNTIME_ERR_NOT_INITIALIZED, pool_session_runtime_start_task(&g_rt));

    runtime_teardown();
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_deinit(&g_rt)); /* idempotent */
}

TEST_CASE("rt: esp_reset_reason is read exactly once per boot", "[pool_runtime_rt]")
{
    fresh_runtime();
    g_reset_raw = POOL_RESET_RAW_TASK_WDT;

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    TEST_ASSERT_EQUAL_UINT32(0u, g_reset_reads); /* init must not read it */

    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL_UINT32(1u, g_reset_reads);
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_runtime_reset_reason_reads(&g_rt));

    /* A repeated boot is refused and never re-reads. */
    TEST_ASSERT_EQUAL(RUNTIME_ERR_ALREADY_INITIALIZED, pool_session_runtime_boot(&g_rt));
    TEST_ASSERT_EQUAL_UINT32(1u, g_reset_reads);

    /* The class is published; the raw value never is. */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_rt, &g_snap));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_TASK_WATCHDOG, g_snap.reset_class);
    runtime_teardown();
}

/* ================================================================= */
/* Boot postures                                                      */
/* ================================================================= */

TEST_CASE("rt: an empty store boots FREE and permits protocol start", "[pool_runtime_rt]")
{
    fresh_runtime();
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    TEST_ASSERT_EQUAL(STORE_EMPTY, g_rt.store_result);
    TEST_ASSERT_EQUAL(RUNTIME_FREE, g_rt.decision.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_rt));
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_rt.lease.phase);
    /* No time provider was touched and no session exists. */
    TEST_ASSERT_EQUAL(0, g_sntp_inits);
    TEST_ASSERT_EQUAL(0, g_sntp_starts);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_rt, &g_snap));
    TEST_ASSERT_TRUE(pool_runtime_snapshot_valid(&g_snap));
    TEST_ASSERT_FALSE(g_snap.session_present);
    runtime_teardown();
}

TEST_CASE("rt: a store that fails to open guards and holds, never STORE_EMPTY",
          "[pool_runtime_rt]")
{
    fresh_runtime();
    g_fake.fail_open = true;
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    TEST_ASSERT_FALSE(g_rt.store_opened);
    TEST_ASSERT_NOT_EQUAL(STORE_EMPTY, g_rt.store_result);
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_rt.decision.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: a retained COMPLETE record boots TERMINAL_PENDING and allows",
          "[pool_runtime_rt]")
{
    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_COMPLETE, false);
    seed_fake_record(&g_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    TEST_ASSERT_EQUAL(STORE_OK, g_rt.store_result);
    TEST_ASSERT_EQUAL(RUNTIME_TERMINAL_PENDING, g_rt.decision.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_rt));
    TEST_ASSERT_TRUE(g_rt.lease.terminal_pending);
    runtime_teardown();
}

TEST_CASE("rt: an interrupted TARGET_ACTIVE session holds protocol start",
          "[pool_runtime_rt]")
{
    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&g_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    /* No trusted time this boot: bounded wait, mining inhibited, HELD. */
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME, g_rt.plan.decision);
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_rt.decision.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    TEST_ASSERT_EQUAL(OP_PHASE_WAITING_FOR_TRUSTED_TIME, g_rt.lease.phase);
    /* Nothing was started: no SNTP, no network, no pool work. */
    TEST_ASSERT_EQUAL(0, g_sntp_inits);
    TEST_ASSERT_EQUAL(0, g_sntp_starts);
    runtime_teardown();
}

TEST_CASE("rt: a corrupt active pointer guards and holds", "[pool_runtime_rt]")
{
    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&g_rec);
    /* Corrupt the committed pointer blob. */
    g_fake.committed[FK_P].bytes[0] ^= 0xFFu;

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    TEST_ASSERT_NOT_EQUAL(STORE_OK, g_rt.store_result);
    TEST_ASSERT_NOT_EQUAL(STORE_EMPTY, g_rt.store_result);
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_rt.decision.state);
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_rt.lease.phase);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    runtime_teardown();
}

/* ================================================================= */
/* Persistence before action                                          */
/* ================================================================= */

TEST_CASE("rt: a mandatory proposal is committed, read back and proven once",
          "[pool_runtime_rt]")
{
    uint32_t gen_before;

    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_APPLYING_TARGET, true);
    seed_fake_record(&g_rec);
    gen_before = 1u;

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    TEST_ASSERT_TRUE(g_rt.persist_attempted);
    TEST_ASSERT_EQUAL(STORE_OK, g_rt.persist_result);
    /* Exactly one durable proposal commit this boot. */
    TEST_ASSERT_EQUAL_UINT32(1u, g_rt.tracker.commit_count);
    /* It really landed: a strictly newer committed generation. */
    TEST_ASSERT_TRUE(g_rt.record.generation > gen_before);
    /* The obligation survived the write. */
    TEST_ASSERT_TRUE(g_rt.record.restore_required);
    /* B5 saw the proof: the persistence barrier is discharged. */
    TEST_ASSERT_FALSE(g_rt.lease.persistence_required_before_action);
    /* Still HELD — persisting a proposal is not permission to act. */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: a failed proposal commit holds and never advances", "[pool_runtime_rt]")
{
    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_APPLYING_TARGET, true);
    seed_fake_record(&g_rec);
    g_fake.fail_write_after = 0; /* fail the very first proposal write */

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    TEST_ASSERT_TRUE(g_rt.persist_attempted);
    TEST_ASSERT_FALSE(g_rt.persist_verified);
    TEST_ASSERT_EQUAL_UINT32(0u, g_rt.tracker.commit_count);
    TEST_ASSERT_FALSE(g_rt.tracker.proven);
    TEST_ASSERT_EQUAL(RUNTIME_PERSISTENCE_PENDING, g_rt.decision.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: an uncertain proposal commit creates the recovery guard",
          "[pool_runtime_rt]")
{
    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_APPLYING_TARGET, true);
    seed_fake_record(&g_rec);
    /* Let the slot write + commit succeed, then fail the POINTER commit:
     * that is exactly the B3 STORE_COMMIT_UNCERTAIN window. */
    g_fake.fail_commit_after = 1;

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    TEST_ASSERT_EQUAL(STORE_COMMIT_UNCERTAIN, g_rt.persist_result);
    TEST_ASSERT_FALSE(g_rt.persist_verified);
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_rt.decision.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_STORE_UNCERTAIN, g_rt.decision.status);
    /* B5 escalated to the guard, and ownership was NOT released. */
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_rt.lease.phase);
    TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: the same proposal is never written twice", "[pool_runtime_rt]")
{
    int writes_after_boot;

    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_APPLYING_TARGET, true);
    seed_fake_record(&g_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rt.tracker.commit_count);
    writes_after_boot = g_fake.writes;

    /* The identical proposal is proven durable and can never be written
     * again — but the tracker is generation-aware, not a blanket per-boot
     * ceiling: only THIS semantic proposal from THIS source generation is
     * deduplicated. */
    TEST_ASSERT_TRUE(g_rt.tracker.proven);
    TEST_ASSERT_TRUE(pool_runtime_proposal_already_proven(&g_rt.tracker, &g_rt.proposal));
    TEST_ASSERT_EQUAL(writes_after_boot, g_fake.writes);

    /* Nor does the running task chain writes from repeated events: the
     * re-derived plan normalizes to the already-accounted per-boot facts,
     * so nothing new needs to be persisted. */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_NETWORK_READY));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_MONOTONIC_BOUNDARY));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_MONOTONIC_BOUNDARY));
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL(writes_after_boot, g_fake.writes);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rt.tracker.commit_count);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

/* ================================================================= */
/* Generation-aware persistence proposals (corrective Gate B6)        */
/* ================================================================= */

/* Boot a WAITING_FOR_TRUSTED_TIME posture with a configured time source and
 * a running task, and hand the provider to the test (network-ready started
 * it). One boot proposal (the reboot increment) is already durable. */
static void boot_waiting_with_source(void)
{
    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&g_rec);
    g_deps.ntp_server = "time.example";

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_rt.decision.state);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rt.tracker.commit_count);
    TEST_ASSERT_EQUAL_UINT8(1u, g_rt.record.reboot_count);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_NETWORK_READY));
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_TRUE(g_rt.time_provider_started);
}

/*
 * Feed one accepted SNTP completion through the designed fake-completion
 * seam, then let the task observe the change.
 *
 * Gate B10 note: the ingestion function now invokes the registered sync
 * observer, which posts RUNTIME_EVENT_TIME_SYNC_CHANGED to the owner task
 * exactly as the production ESP-IDF callback does. Posting the event
 * manually here as well would deliver the SAME event TWICE and drive a
 * second re-evaluation, so it is deliberately no longer done.
 */
static void inject_trusted_sync(uint64_t epoch_s)
{
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_sntp_handle_sync(&g_rt.time_provider, epoch_s, 0u));
    vTaskDelay(pdMS_TO_TICKS(200));
}

TEST_CASE("rt: five duplicate evaluations never recommit the identical proposal",
          "[pool_runtime_rt]")
{
    int writes_after_boot;
    int i;

    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&g_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_rt.decision.state);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rt.tracker.commit_count);
    writes_after_boot = g_fake.writes;

    /* Five explicit re-evaluations of the SAME context: the re-derived plan
     * normalizes to the already-accounted per-boot facts, so nothing is
     * ever rewritten and the counters never move twice. */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));
    for (i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(RUNTIME_OK,
                          pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_TIME_SYNC_CHANGED));
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    TEST_ASSERT_EQUAL(writes_after_boot, g_fake.writes);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rt.tracker.commit_count);
    TEST_ASSERT_EQUAL_UINT8(1u, g_rt.record.reboot_count);
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_rt.control.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: a distinct trusted-time proposal commits exactly once after boot",
          "[pool_runtime_rt]")
{
    boot_waiting_with_source();

    /* A trusted sync ABOVE the persisted floor: the raised trusted-epoch
     * floor is a DISTINCT proposal — committed, read back and proven before
     * the state advances to target verification. */
    inject_trusted_sync(EPOCH_A_S + 200u);

    TEST_ASSERT_EQUAL_UINT32(2u, g_rt.tracker.commit_count);
    TEST_ASSERT_TRUE(g_rt.record.latest_trusted_valid);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 200u, g_rt.record.latest_trusted_epoch_s);
    /* The same physical boot never accounts its reboot increment twice. */
    TEST_ASSERT_EQUAL_UINT8(1u, g_rt.record.reboot_count);
    TEST_ASSERT_EQUAL_UINT8(0u, g_rt.record.recovery_attempt_count);
    /* Advancement happened only AFTER the proof: verification-only, held. */
    TEST_ASSERT_EQUAL(RUNTIME_VERIFY_TARGET_PENDING, g_rt.control.state);
    TEST_ASSERT_EQUAL(OP_PHASE_VERIFYING_TARGET, g_rt.lease.phase);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    /* Exactly one matching B5 proof per distinct successful proposal. */
    TEST_ASSERT_EQUAL_UINT32(2u, g_rt.tracker.proof_count);
    TEST_ASSERT_EQUAL_UINT32(g_rt.tracker.commit_count, g_rt.tracker.proof_count);
    /* No proposal path authorizes mining or permits pool mutation. */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_rt, &g_snap));
    TEST_ASSERT_FALSE(g_snap.target_mining_authorized);
    TEST_ASSERT_FALSE(g_snap.pool_mutation_permitted);

    /* A duplicate TIME_SYNC_CHANGED event never duplicates the proposal. */
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_TIME_SYNC_CHANGED));
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL_UINT32(2u, g_rt.tracker.commit_count);
    TEST_ASSERT_EQUAL_UINT32(2u, g_rt.tracker.proof_count);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: the wait-expiry restore proposal is committed, not suppressed",
          "[pool_runtime_rt]")
{
    int i;

    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&g_rec);
    g_deps.sync_wait_limit_s = 1u; /* bounded window of one tick */

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_rt.decision.state);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rt.tracker.commit_count);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));
    for (i = 0; i < 60 && g_rt.control.state == RUNTIME_WAITING_FOR_TRUSTED_TIME; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* The fail-safe restore plan is a DIFFERENT later proposal: its state
     * update and recovery-attempt increment were preserved and committed
     * during the SAME physical boot, while its raw duplicate reboot
     * increment was normalized away. */
    TEST_ASSERT_EQUAL(RUNTIME_RESTORE_SOURCE_PENDING, g_rt.control.state);
    TEST_ASSERT_EQUAL_UINT32(2u, g_rt.tracker.commit_count);
    TEST_ASSERT_EQUAL_UINT32(2u, g_rt.tracker.proof_count);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, g_rt.record.state);
    TEST_ASSERT_EQUAL_UINT8(1u, g_rt.record.recovery_attempt_count);
    TEST_ASSERT_EQUAL_UINT8(1u, g_rt.record.reboot_count); /* once per boot */
    TEST_ASSERT_TRUE(g_rt.record.restore_required);
    TEST_ASSERT_EQUAL(OP_PHASE_RESTORING_SOURCE, g_rt.lease.phase);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_rt, &g_snap));
    TEST_ASSERT_FALSE(g_snap.target_mining_authorized);
    TEST_ASSERT_FALSE(g_snap.pool_mutation_permitted);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: a new physical boot permits exactly one new reboot increment",
          "[pool_runtime_rt]")
{
    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_APPLYING_TARGET, true);
    seed_fake_record(&g_rec);

    /* Physical boot 1: the restore proposal commits with reboot 0 -> 1. */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rt.tracker.commit_count);
    TEST_ASSERT_EQUAL_UINT8(1u, g_rt.record.reboot_count);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, g_rt.record.state);

    /* RAM loss on a real reboot clears the tracker naturally: model it with
     * a full instance teardown; the fake NVS keeps the committed state. */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_deinit(&g_rt));
    memset(&g_rt, 0, sizeof(g_rt));

    /* Physical boot 2: exactly one NEW increment (1 -> 2), never a second
     * increment within either boot. */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rt.tracker.commit_count);
    TEST_ASSERT_EQUAL_UINT8(2u, g_rt.record.reboot_count);
    TEST_ASSERT_EQUAL_UINT8(2u, g_rt.record.recovery_attempt_count);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: a failed later commit blocks advancement until a retry proves it",
          "[pool_runtime_rt]")
{
    boot_waiting_with_source();

    /* Fail the SECOND proposal's first write: the raised-floor proposal
     * cannot commit, so WAITING must NOT advance to VERIFY_TARGET_PENDING. */
    g_fake.fail_write_after = 0;
    inject_trusted_sync(EPOCH_A_S + 200u);

    TEST_ASSERT_EQUAL(RUNTIME_PERSISTENCE_PENDING, g_rt.control.state);
    TEST_ASSERT_EQUAL(OP_PHASE_WAITING_FOR_TRUSTED_TIME, g_rt.lease.phase);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rt.tracker.commit_count);
    TEST_ASSERT_FALSE(g_rt.persist_verified);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 100u, g_rt.record.latest_trusted_epoch_s);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));

    /* The pending proposal is retried on an explicit event only; once the
     * store works again it commits, is proven, and ONLY THEN advances. */
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_MONOTONIC_BOUNDARY));
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_EQUAL_UINT32(2u, g_rt.tracker.commit_count);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 200u, g_rt.record.latest_trusted_epoch_s);
    TEST_ASSERT_EQUAL(RUNTIME_VERIFY_TARGET_PENDING, g_rt.control.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: an independent readback failure blocks advancement",
          "[pool_runtime_rt]")
{
    boot_waiting_with_source();

    /* Let the second proposal's COMMIT fully succeed, then fail the
     * INDEPENDENT reload's pointer read: commit ok, reload not ok — the
     * proposal stays unproven and pending. The B3 commit itself performs
     * TWO pointer reads (committed-base validation + post-write verify),
     * so the THIRD read after arming is the independent reload's. */
    g_fake.fail_active_read_after = 2;
    inject_trusted_sync(EPOCH_A_S + 200u);

    TEST_ASSERT_EQUAL(RUNTIME_PERSISTENCE_PENDING, g_rt.control.state);
    TEST_ASSERT_EQUAL(OP_PHASE_WAITING_FOR_TRUSTED_TIME, g_rt.lease.phase);
    TEST_ASSERT_FALSE(g_rt.persist_verified);
    TEST_ASSERT_FALSE(g_rt.tracker.proven &&
                      pool_runtime_proposal_already_proven(&g_rt.tracker, &g_rt.proposal));
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: an uncertain later commit enters the recovery guard",
          "[pool_runtime_rt]")
{
    boot_waiting_with_source();

    /* Slot commit succeeds, POINTER commit fails: STORE_COMMIT_UNCERTAIN on
     * the SECOND proposal escalates to the B5 recovery guard, retains
     * ownership evidence and never advances toward execution. */
    g_fake.fail_commit_after = 1;
    inject_trusted_sync(EPOCH_A_S + 200u);

    TEST_ASSERT_EQUAL(STORE_COMMIT_UNCERTAIN, g_rt.persist_result);
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_rt.control.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_STORE_UNCERTAIN, g_rt.decision.status);
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_rt.lease.phase);
    TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_rt.lease.owner);
    TEST_ASSERT_TRUE(g_rt.lease.durable_claim); /* evidence retained */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: a forced fingerprint collision still commits the distinct proposal",
          "[pool_runtime_rt]")
{
    PoolRuntimeProposal expect;

    boot_waiting_with_source();

    /* Predict the epoch-ratchet proposal the coming trusted sync will
     * produce (the proposal captures persisted-field changes only, so it is
     * identical whether derived from the WAIT or the RESUME plan), then
     * FORCE the stored diagnostic hash to collide with it while the stored
     * EXACT key remains the boot plan proposal. */
    TEST_ASSERT_EQUAL(RUNTIME_PROPOSAL_EPOCH_FLOOR,
                      pool_runtime_proposal_build(&g_rt.plan, &g_rt.record,
                                                  &g_rt.tracker,
                                                  g_rt.committed_generation,
                                                  true, EPOCH_A_S + 200u, &expect));
    g_rt.tracker.last_fingerprint = pool_runtime_proposal_fingerprint(&expect);
    TEST_ASSERT_FALSE(pool_runtime_proposal_equal(&g_rt.tracker.last_proposal, &expect));
    TEST_ASSERT_FALSE(pool_runtime_proposal_already_proven(&g_rt.tracker, &expect));

    /* The colliding hash suppresses NOTHING: the distinct proposal earns its
     * own commit, independent readback and B5 proof, and only then advances. */
    inject_trusted_sync(EPOCH_A_S + 200u);

    TEST_ASSERT_EQUAL_UINT32(2u, g_rt.tracker.commit_count);
    TEST_ASSERT_EQUAL_UINT32(2u, g_rt.tracker.proof_count);
    TEST_ASSERT_TRUE(g_rt.record.latest_trusted_valid);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 200u, g_rt.record.latest_trusted_epoch_s);
    TEST_ASSERT_EQUAL_UINT8(1u, g_rt.record.reboot_count);
    TEST_ASSERT_EQUAL(RUNTIME_VERIFY_TARGET_PENDING, g_rt.control.state);
    /* No collision case authorizes mining or permits pool mutation. */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_rt, &g_snap));
    TEST_ASSERT_FALSE(g_snap.target_mining_authorized);
    TEST_ASSERT_FALSE(g_snap.pool_mutation_permitted);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: an unowned retained terminal commits boot accounting without a proof",
          "[pool_runtime_rt]")
{
    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_COMPLETE, false);
    seed_fake_record(&g_rec);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    /* The reboot accounting still lands durably, but B5 proofs are
     * token-gated by design: the FREE + retained-terminal posture holds no
     * lease, so NO proof is applied and none is needed. */
    TEST_ASSERT_EQUAL_UINT32(1u, g_rt.tracker.commit_count);
    TEST_ASSERT_EQUAL_UINT32(0u, g_rt.tracker.proof_count);
    TEST_ASSERT_TRUE(g_rt.tracker.proven);
    TEST_ASSERT_EQUAL_UINT8(1u, g_rt.record.reboot_count);
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, g_rt.record.state);
    TEST_ASSERT_FALSE(g_rt.record.restore_required);
    /* The committed baseline posture is preserved exactly. */
    TEST_ASSERT_EQUAL(RUNTIME_TERMINAL_PENDING, g_rt.decision.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: identical inputs and tracker state yield byte-identical output",
          "[pool_runtime_rt]")
{
    PoolRuntimeSnapshot        snap1, snap2;
    PoolRuntimeProposalTracker trk1;

    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_APPLYING_TARGET, true);
    seed_fake_record(&g_rec);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_rt, &snap1));
    trk1 = g_rt.tracker;
    runtime_teardown();

    /* The identical committed store + identical platform inputs reproduce a
     * byte-identical snapshot AND byte-identical tracker state. */
    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_APPLYING_TARGET, true);
    seed_fake_record(&g_rec);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_rt, &snap2));
    TEST_ASSERT_EQUAL(0, memcmp(&snap1, &snap2, sizeof(snap1)));
    TEST_ASSERT_EQUAL(0, memcmp(&trk1, &g_rt.tracker, sizeof(trk1)));
    runtime_teardown();
}

/* ================================================================= */
/* The single runtime task                                            */
/* ================================================================= */

TEST_CASE("rt: exactly one runtime task exists module-wide", "[pool_runtime_rt]")
{
    static PoolSessionRuntime rt2;

    fresh_runtime();
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_task_count());
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_runtime_task_count());

    /* Measure the REAL stack high-water mark rather than assuming headroom:
     * every large working struct lives in the instance, so the statically
     * allocated 4 KB must stay comfortably unused. */
    vTaskDelay(pdMS_TO_TICKS(1200)); /* one full bounded tick + logging */
    {
        UBaseType_t free_bytes =
            uxTaskGetStackHighWaterMark((TaskHandle_t)g_rt.task_handle);
        printf("runtime task stack: %u bytes free of 4096 (high-water)\n",
               (unsigned)free_bytes);
        TEST_ASSERT_TRUE(free_bytes > 512u);
    }

    /* A second start on the SAME instance is refused. */
    TEST_ASSERT_EQUAL(RUNTIME_ERR_TASK_ALREADY_RUNNING,
                      pool_session_runtime_start_task(&g_rt));

    /* A second instance cannot create a second runtime task either. */
    memset(&rt2, 0, sizeof(rt2));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&rt2, &g_deps));
    (void)pool_session_runtime_boot(&rt2);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_TASK_ALREADY_RUNNING,
                      pool_session_runtime_start_task(&rt2));
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_runtime_task_count());
    (void)pool_session_runtime_deinit(&rt2);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_task_count());
    /* Stopping never releases the recorded posture. */
    TEST_ASSERT_EQUAL(RUNTIME_FREE, g_rt.control.state);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt)); /* idempotent */
    runtime_teardown();
}

TEST_CASE("rt: duplicate and unknown events are safe for the running task",
          "[pool_runtime_rt]")
{
    fresh_runtime();
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));

    /* Unknown bits are dropped at the boundary. */
    TEST_ASSERT_EQUAL(RUNTIME_ERR_UNSUPPORTED_EVENT,
                      pool_session_runtime_notify(&g_rt, 0xFFFFFF00u));
    TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT, pool_session_runtime_notify(NULL, 1u));

    /* Duplicates are idempotent and never mutate anything. */
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_NETWORK_READY));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_NETWORK_READY));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_MONOTONIC_BOUNDARY));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* A FREE posture never starts a time provider, whatever arrives. */
    TEST_ASSERT_EQUAL(0, g_sntp_inits);
    TEST_ASSERT_EQUAL(0, g_sntp_starts);
    TEST_ASSERT_EQUAL(RUNTIME_FREE, g_rt.control.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: events latched before the task exists are delivered once",
          "[pool_runtime_rt]")
{
    fresh_runtime();
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    /* No task yet: the bits latch instead of being lost. */
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_NETWORK_READY));
    TEST_ASSERT_EQUAL(RUNTIME_EVENT_NETWORK_READY, g_rt.control.pending_events);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT32(0u, g_rt.control.pending_events);
    TEST_ASSERT_TRUE(g_rt.control.network_ready);
    TEST_ASSERT_TRUE(g_rt.control.bootstrap_complete);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

/* ================================================================= */
/* Trusted time                                                       */
/* ================================================================= */

TEST_CASE("rt: with no configured time source nothing starts and the hold stays",
          "[pool_runtime_rt]")
{
    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&g_rec);
    g_deps.ntp_server = ""; /* the shipped default: no source chosen */

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_rt.decision.state);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_NETWORK_READY));
    vTaskDelay(pdMS_TO_TICKS(100));

    /* No SNTP service was configured or started, and no trust was invented. */
    TEST_ASSERT_EQUAL(0, g_sntp_inits);
    TEST_ASSERT_EQUAL(0, g_sntp_starts);
    TEST_ASSERT_FALSE(g_rt.time_provider_initialized);
    TEST_ASSERT_FALSE(g_rt.time_snapshot.trusted);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: a configured source starts only after network-ready while waiting",
          "[pool_runtime_rt]")
{
    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&g_rec);
    g_deps.ntp_server = "time.example";

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_rt.decision.state);
    /* Boot alone must NOT touch the network. */
    TEST_ASSERT_EQUAL(0, g_sntp_inits);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_NETWORK_READY));
    vTaskDelay(pdMS_TO_TICKS(100));

    TEST_ASSERT_EQUAL(1, g_sntp_inits);
    TEST_ASSERT_EQUAL(1, g_sntp_starts);
    TEST_ASSERT_TRUE(g_rt.time_provider_started);
    /* Starting a clock is not permission to mine: the hold stands. */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

TEST_CASE("rt: the bounded wait expires into restore-pending and still holds",
          "[pool_runtime_rt]")
{
    int i;

    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&g_rec);
    g_deps.sync_wait_limit_s = 1u; /* bounded window of one tick */

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_rt.decision.state);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));
    for (i = 0; i < 60 && g_rt.control.state == RUNTIME_WAITING_FOR_TRUSTED_TIME; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Fail-safe toward restore — never toward extra target time, and never
     * toward a released protocol hold. */
    TEST_ASSERT_EQUAL(RUNTIME_RESTORE_SOURCE_PENDING, g_rt.control.state);
    TEST_ASSERT_EQUAL(OP_PHASE_RESTORING_SOURCE, g_rt.lease.phase);
    TEST_ASSERT_EQUAL(OP_OWNER_SOURCE_RESTORE, g_rt.lease.owner);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_rt, &g_snap));
    TEST_ASSERT_TRUE(g_snap.restore_required);
    TEST_ASSERT_FALSE(g_snap.target_mining_authorized);
    TEST_ASSERT_FALSE(g_snap.pool_mutation_permitted);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();
}

/* ================================================================= */
/* Privacy                                                            */
/* ================================================================= */

TEST_CASE("rt: the published snapshot carries no identity or session id",
          "[pool_runtime_rt]")
{
    const char *needles[] = { "btc.example", "bch.example", "acct.worker", "time.example" };
    size_t n, i;
    uint32_t sid = SESSION_ID;

    fresh_runtime();
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    seed_fake_record(&g_rec);
    g_deps.ntp_server = "time.example";

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_rt, &g_snap));
    TEST_ASSERT_TRUE(pool_runtime_snapshot_valid(&g_snap));

    for (n = 0; n < sizeof(needles) / sizeof(needles[0]); n++) {
        size_t nl = strlen(needles[n]);
        const uint8_t *h = (const uint8_t *)&g_snap;
        for (i = 0; i + nl <= sizeof(g_snap); i++) {
            TEST_ASSERT_FALSE(memcmp(h + i, needles[n], nl) == 0);
        }
    }
    for (i = 0; i + sizeof(sid) <= sizeof(g_snap); i++) {
        TEST_ASSERT_FALSE(memcmp((const uint8_t *)&g_snap + i, &sid, sizeof(sid)) == 0);
    }
    /* Presence is published, identity is not. */
    TEST_ASSERT_TRUE(g_snap.session_present);
    runtime_teardown();
}

/* ================================================================= */
/* Real ESP-IDF NVS (isolated QEMU partition; never a device)          */
/* ================================================================= */

static void erase_real_namespace(void)
{
    nvs_handle_t h;
    if (nvs_open(POOL_STORE_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_all(h);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

TEST_CASE("rt: boots against the real NVS backend on the isolated test partition",
          "[pool_runtime_rt]")
{
    static PoolStoreNvsBackend backend;
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        TEST_ASSERT_EQUAL(ESP_OK, nvs_flash_erase());
        err = nvs_flash_init();
    }
    TEST_ASSERT_EQUAL(ESP_OK, err);
    erase_real_namespace();

    fresh_runtime();
    memset(&backend, 0, sizeof(backend));
    g_deps.store_ops = pool_store_nvs_ops();
    g_deps.store_ctx = &backend;

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    /* An empty dedicated namespace is a proven-safe posture. */
    TEST_ASSERT_TRUE(g_rt.store_opened);
    TEST_ASSERT_EQUAL(STORE_EMPTY, g_rt.store_result);
    TEST_ASSERT_EQUAL(RUNTIME_FREE, g_rt.decision.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_rt));
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_runtime_reset_reason_reads(&g_rt));

    /* The runtime task runs against real NVS without touching anything else. */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_rt));
    runtime_teardown();

    erase_real_namespace();
}

TEST_CASE("rt: a real-NVS committed session is reloaded and holds protocol start",
          "[pool_runtime_rt]")
{
    static PoolStoreNvsBackend backend;
    PoolSessionStore  st;
    PoolSessionRecord copy;

    TEST_ASSERT_EQUAL(ESP_OK, nvs_flash_init());
    erase_real_namespace();

    /* Seed one committed session through the real B3 store + real NVS. */
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    copy = g_rec;
    memset(&backend, 0, sizeof(backend));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&st, pool_store_nvs_ops(), &backend));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&st, &copy));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&st));

    fresh_runtime();
    memset(&backend, 0, sizeof(backend));
    g_deps.store_ops = pool_store_nvs_ops();
    g_deps.store_ctx = &backend;

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);

    TEST_ASSERT_EQUAL(STORE_OK, g_rt.store_result);
    TEST_ASSERT_TRUE(g_rt.record_present);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_ACTIVE, g_rt.record.state);
    TEST_ASSERT_TRUE(g_rt.record.restore_required);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_session_runtime_protocol_permission(&g_rt));
    runtime_teardown();

    erase_real_namespace();
}
