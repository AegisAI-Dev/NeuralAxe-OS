/*
 * Exhaustive deterministic tests for the trusted-time foundation (Gate B2).
 *
 * No hardware, no networking, no real NTP, no secrets. Every provider in this
 * file uses FAKE platform ops — the real ESP-IDF SNTP stack is never
 * initialized or started. All server names are synthetic "*.example"
 * fixtures. The fake raw wall clock below exists ONLY to prove pool_time
 * never reads it.
 */

#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "pool_time.h"
#include "pool_time_sntp.h"
#include "pool_session.h" /* bounds-parity asserts only; no production coupling */

/* Gate B1 / Gate B2 duration parity — checked here so the production
 * components stay uncoupled. */
_Static_assert(POOL_TIME_MIN_DURATION_S == POOL_SESSION_MIN_DURATION_S,
               "duration floor must match the Gate B1 session bounds");
_Static_assert(POOL_TIME_MAX_DURATION_S == POOL_SESSION_MAX_DURATION_S,
               "duration ceiling must match the Gate B1 session bounds");

/* ---------------- fake platform environment ---------------- */

#define EPOCH_A_S  1750000000ull /* mid-2025, inside the sanity band */
#define MONO_M_US  111222333ull

static uint64_t g_fake_mono_us;

/*
 * The mutable raw system wall clock, as Stratum's settimeofday() would see
 * it. pool_time exposes NO API that reads this variable — the isolation
 * tests mutate it aggressively and prove the trusted estimate never moves.
 */
static uint64_t g_fake_raw_wall_clock_us;

static int g_init_calls, g_start_calls, g_stop_calls, g_deinit_calls;
static int g_init_result, g_start_result, g_stop_result, g_deinit_result;

static uint64_t fake_mono(void) { return g_fake_mono_us; }
static int fake_sntp_init(const PoolTimeSntpConfig *cfg) { (void)cfg; g_init_calls++; return g_init_result; }
static int fake_sntp_start(void) { g_start_calls++; return g_start_result; }
static int fake_sntp_stop(void) { g_stop_calls++; return g_stop_result; }
static int fake_sntp_deinit(void) { g_deinit_calls++; return g_deinit_result; }

static const PoolTimeSntpPlatformOps FAKE_OPS = {
    .monotonic_us = fake_mono,
    .sntp_init    = fake_sntp_init,
    .sntp_start   = fake_sntp_start,
    .sntp_stop    = fake_sntp_stop,
    .sntp_deinit  = fake_sntp_deinit,
};

static void reset_fakes(void)
{
    g_fake_mono_us = MONO_M_US;
    g_fake_raw_wall_clock_us = 0u;
    g_init_calls = g_start_calls = g_stop_calls = g_deinit_calls = 0;
    g_init_result = g_start_result = g_stop_result = g_deinit_result = 0;
}

static PoolTimeTrustPolicy default_policy(void)
{
    PoolTimeTrustPolicy pol;
    pool_time_trust_policy_defaults(&pol);
    return pol;
}

static PoolTimeSntpConfig valid_cfg(void)
{
    PoolTimeSntpConfig c;
    pool_time_sntp_config_defaults(&c);
    c.server_count = 1;
    strncpy(c.servers[0], "ntp-a.example", sizeof(c.servers[0]) - 1);
    return c;
}

/* Init + start a provider on the fake ops (no sync yet). */
static void make_started_provider(PoolTimeSntpProvider *p)
{
    PoolTimeSntpConfig cfg = valid_cfg();
    PoolTimeTrustPolicy pol = default_policy();
    memset(p, 0, sizeof(*p));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_init(p, &FAKE_OPS, &cfg, &pol));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_start(p));
}

/* Full trusted provider: synced at EPOCH_A_S / MONO_M_US. */
static void make_trusted_provider(PoolTimeSntpProvider *p)
{
    make_started_provider(p);
    g_fake_mono_us = MONO_M_US;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(p, EPOCH_A_S, 0u));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_TRUSTED, pool_time_sntp_lifecycle(p));
}

static PoolTimeSnapshot snapshot_of(PoolTimeSntpProvider *p)
{
    PoolTimeClock clk;
    PoolTimeSnapshot s;
    PoolTimeTrustPolicy pol = default_policy();
    pool_time_sntp_get_clock(p, &clk);
    (void)pool_time_snapshot(&clk, &pol, &s);
    return s;
}

/* A valid trusted anchor for pure-predicate tests. */
static PoolTimeAnchor anchor_at(uint64_t epoch_us, uint64_t mono_us)
{
    PoolTimeAnchor a;
    memset(&a, 0, sizeof(a));
    a.valid = true;
    a.sync_completed_this_boot = true;
    a.generation = 1u;
    a.epoch_us_at_sync = epoch_us;
    a.monotonic_us_at_sync = mono_us;
    a.sync_status = POOL_TIME_SYNC_STATUS_COMPLETED;
    a.last_error = TIME_OK;
    return a;
}

/* Recovery-input fixture: source + deadline valid, trusted, mid-session. */
static PoolTimeRecoveryInput recovery_base(void)
{
    PoolTimeRecoveryInput in;
    memset(&in, 0, sizeof(in));
    in.source_snapshot_valid = true;
    in.utc_deadline_valid = true;
    in.deadline_epoch_s = EPOCH_A_S + 3600u;
    in.verified_start_epoch_valid = true;
    in.verified_start_epoch_s = EPOCH_A_S;
    in.time_trusted = true;
    in.trusted_epoch_s = EPOCH_A_S + 100u;
    in.sync_wait_elapsed_s = 0u;
    in.sync_wait_limit_s = POOL_TIME_SYNC_WAIT_DEFAULT_S;
    return in;
}

static bool bytes_contain(const void *buf, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    const unsigned char *b = (const unsigned char *)buf;
    size_t i;
    if (nlen == 0u || nlen > len) {
        return false;
    }
    for (i = 0; i + nlen <= len; i++) {
        if (memcmp(b + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static void assert_clean_token(const char *tok)
{
    const char *c;
    TEST_ASSERT_NOT_NULL(tok);
    TEST_ASSERT_TRUE(strlen(tok) > 0u && strlen(tok) < 48u);
    for (c = tok; *c != '\0'; c++) {
        TEST_ASSERT_TRUE((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_');
    }
    /* Never a hostname, marker or prose fragment. */
    TEST_ASSERT_NULL(strstr(tok, "EXAMPLE"));
    TEST_ASSERT_NULL(strstr(tok, "MARKER"));
}

/* ================================================================= */
/* A. Model / configuration tests                                     */
/* ================================================================= */

TEST_CASE("model: trust policy defaults are the compiled band and window", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    TEST_ASSERT_TRUE(pool_time_trust_policy_valid(&pol));
    TEST_ASSERT_EQUAL_UINT64(1735689600ull, pol.min_epoch_s); /* 2025-01-01T00:00:00Z */
    TEST_ASSERT_EQUAL_UINT64(4102444800ull, pol.max_epoch_s); /* 2100-01-01T00:00:00Z */
    TEST_ASSERT_EQUAL_UINT64(0ull, pol.required_min_epoch_s);
    TEST_ASSERT_EQUAL_UINT32(600u, pol.sync_wait_default_s);
    TEST_ASSERT_EQUAL_UINT32(900u, pol.sync_wait_max_s);
}

TEST_CASE("model: policy validity rejects malformed bounds", "[pool_time]")
{
    PoolTimeTrustPolicy pol;

    TEST_ASSERT_FALSE(pool_time_trust_policy_valid(NULL));

    pol = default_policy();
    pol.min_epoch_s = pol.max_epoch_s; /* inverted/empty band */
    TEST_ASSERT_FALSE(pool_time_trust_policy_valid(&pol));

    pol = default_policy();
    pol.min_epoch_s = 0u;
    TEST_ASSERT_FALSE(pool_time_trust_policy_valid(&pol));

    pol = default_policy();
    pol.sync_wait_default_s = 0u;
    TEST_ASSERT_FALSE(pool_time_trust_policy_valid(&pol));

    pol = default_policy();
    pol.sync_wait_max_s = POOL_TIME_SYNC_WAIT_MAX_S + 1u; /* above hard max */
    TEST_ASSERT_FALSE(pool_time_trust_policy_valid(&pol));

    pol = default_policy();
    pol.sync_wait_default_s = pol.sync_wait_max_s + 1u;
    pol.sync_wait_max_s = POOL_TIME_SYNC_WAIT_MAX_S;
    pol.sync_wait_default_s = POOL_TIME_SYNC_WAIT_MAX_S + 1u;
    TEST_ASSERT_FALSE(pool_time_trust_policy_valid(&pol));

    pol = default_policy();
    pol.required_min_epoch_s = pol.max_epoch_s + 1u; /* insane floor */
    TEST_ASSERT_FALSE(pool_time_trust_policy_valid(&pol));
}

TEST_CASE("model: zero-initialized anchor is untrusted", "[pool_time]")
{
    PoolTimeAnchor a;
    PoolTimeTrustPolicy pol = default_policy();
    uint64_t utc = 123u;
    memset(&a, 0, sizeof(a));
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_evaluate_trust(&a, MONO_M_US, &pol, &utc));
    TEST_ASSERT_EQUAL_UINT64(0ull, utc); /* out param cleared on failure */
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_validate_anchor(&a, &pol));
}

TEST_CASE("model: sntp config defaults require explicit servers", "[pool_time]")
{
    PoolTimeSntpConfig cfg;
    pool_time_sntp_config_defaults(&cfg);
    TEST_ASSERT_EQUAL_UINT32(0u, cfg.server_count);
    TEST_ASSERT_FALSE(cfg.accept_dhcp_ntp);
    TEST_ASSERT_FALSE(cfg.smooth_sync);
    TEST_ASSERT_EQUAL_UINT32(POOL_TIME_SYNC_WAIT_DEFAULT_S, cfg.sync_wait_s);
    /* No server list is baked into B2: defaults alone are INVALID. */
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG, pool_time_sntp_validate_config(&cfg));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_sntp_validate_config(NULL));
}

TEST_CASE("model: server-name bounds enforced", "[pool_time]")
{
    PoolTimeSntpConfig cfg = valid_cfg();
    int i;

    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_validate_config(&cfg));

    /* empty name */
    cfg = valid_cfg();
    cfg.servers[0][0] = '\0';
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG, pool_time_sntp_validate_config(&cfg));

    /* unterminated within the bound */
    cfg = valid_cfg();
    for (i = 0; i < POOL_TIME_SNTP_SERVER_HOST_MAX; i++) {
        cfg.servers[0][i] = 'a';
    }
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG, pool_time_sntp_validate_config(&cfg));

    /* maximum-length name (63 chars + NUL) accepted */
    cfg = valid_cfg();
    for (i = 0; i < POOL_TIME_SNTP_SERVER_HOST_MAX - 1; i++) {
        cfg.servers[0][i] = 'a';
    }
    cfg.servers[0][POOL_TIME_SNTP_SERVER_HOST_MAX - 1] = '\0';
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_validate_config(&cfg));

    /* whitespace / control characters rejected */
    cfg = valid_cfg();
    strncpy(cfg.servers[0], "bad name.example", sizeof(cfg.servers[0]) - 1);
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG, pool_time_sntp_validate_config(&cfg));
    cfg = valid_cfg();
    cfg.servers[0][0] = '\t';
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG, pool_time_sntp_validate_config(&cfg));
    cfg = valid_cfg();
    cfg.servers[0][3] = (char)0x1B;
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG, pool_time_sntp_validate_config(&cfg));
}

TEST_CASE("model: server-count bounds enforced", "[pool_time]")
{
    PoolTimeSntpConfig cfg = valid_cfg();
    uint32_t n;

    cfg.server_count = 0u;
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG, pool_time_sntp_validate_config(&cfg));

    cfg.server_count = POOL_TIME_SNTP_MAX_SERVERS + 1u;
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG, pool_time_sntp_validate_config(&cfg));

    /* every in-bound count with well-formed names is accepted */
    for (n = 1u; n <= POOL_TIME_SNTP_MAX_SERVERS; n++) {
        uint32_t i;
        cfg = valid_cfg();
        cfg.server_count = n;
        for (i = 0; i < n; i++) {
            char name[POOL_TIME_SNTP_SERVER_HOST_MAX];
            (void)snprintf(name, sizeof(name), "ntp-%u.example", (unsigned)i);
            strncpy(cfg.servers[i], name, sizeof(cfg.servers[i]) - 1);
        }
        TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_validate_config(&cfg));
    }
}

TEST_CASE("model: wait-window limits enforced", "[pool_time]")
{
    PoolTimeSntpConfig cfg = valid_cfg();

    cfg.sync_wait_s = 0u;
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_WAIT_WINDOW, pool_time_sntp_validate_config(&cfg));

    cfg.sync_wait_s = POOL_TIME_SYNC_WAIT_MAX_S + 1u; /* 901 */
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_WAIT_WINDOW, pool_time_sntp_validate_config(&cfg));

    cfg.sync_wait_s = 1u;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_validate_config(&cfg));
    cfg.sync_wait_s = POOL_TIME_SYNC_WAIT_DEFAULT_S; /* 600 */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_validate_config(&cfg));
    cfg.sync_wait_s = POOL_TIME_SYNC_WAIT_MAX_S; /* 900 exact boundary */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_validate_config(&cfg));
}

TEST_CASE("model: invalid enum values map to stable unknown tokens", "[pool_time]")
{
    assert_clean_token(pool_time_error_str((PoolTimeError)POOL_TIME_ERR__COUNT));
    assert_clean_token(pool_time_error_str((PoolTimeError)9999));
    assert_clean_token(pool_time_decision_str((PoolTimeRecoveryDecision)POOL_TIME_DECISION__COUNT));
    assert_clean_token(pool_time_decision_str((PoolTimeRecoveryDecision)-1));
    assert_clean_token(pool_time_sync_status_str((PoolTimeSyncStatus)77));
    assert_clean_token(pool_time_sntp_lifecycle_str((PoolTimeSntpLifecycle)77));
    TEST_ASSERT_EQUAL_STRING("TIME_ERR_UNKNOWN", pool_time_error_str((PoolTimeError)12345));
    TEST_ASSERT_EQUAL_STRING("DECISION_UNKNOWN", pool_time_decision_str((PoolTimeRecoveryDecision)12345));
}

/* ================================================================= */
/* B. Trusted-time predicate tests                                    */
/* ================================================================= */

TEST_CASE("trust: no completed sync is never trusted", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at(EPOCH_A_S * POOL_TIME_US_PER_S, MONO_M_US);
    a.valid = false; /* status/metadata alone, no captured anchor */
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_evaluate_trust(&a, MONO_M_US, &pol, NULL));
}

TEST_CASE("trust: anchor without the completed-this-boot flag is untrusted", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at(EPOCH_A_S * POOL_TIME_US_PER_S, MONO_M_US);
    a.sync_completed_this_boot = false; /* e.g. a persisted epoch, not a sync */
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_evaluate_trust(&a, MONO_M_US, &pol, NULL));
}

TEST_CASE("trust: epoch below the sanity floor is rejected", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at((POOL_TIME_EPOCH_MIN_S - 1u) * POOL_TIME_US_PER_S, MONO_M_US);
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BELOW_MIN, pool_time_evaluate_trust(&a, MONO_M_US, &pol, NULL));
    /* one microsecond below the floor still floors to MIN-1 seconds */
    a = anchor_at(POOL_TIME_EPOCH_MIN_S * POOL_TIME_US_PER_S - 1u, MONO_M_US);
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BELOW_MIN, pool_time_evaluate_trust(&a, MONO_M_US, &pol, NULL));
}

TEST_CASE("trust: epoch above the sanity ceiling is rejected", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at((POOL_TIME_EPOCH_MAX_S + 1u) * POOL_TIME_US_PER_S, MONO_M_US);
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_ABOVE_MAX, pool_time_evaluate_trust(&a, MONO_M_US, &pol, NULL));
}

TEST_CASE("trust: floor boundary epoch is trusted", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at(POOL_TIME_EPOCH_MIN_S * POOL_TIME_US_PER_S, MONO_M_US);
    uint64_t utc = 0u;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_evaluate_trust(&a, MONO_M_US, &pol, &utc));
    TEST_ASSERT_EQUAL_UINT64(POOL_TIME_EPOCH_MIN_S * POOL_TIME_US_PER_S, utc);
}

TEST_CASE("trust: ceiling boundary trusted at sync, untrusted once derived passes it", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at(POOL_TIME_EPOCH_MAX_S * POOL_TIME_US_PER_S, MONO_M_US);
    uint64_t utc = 0u;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_evaluate_trust(&a, MONO_M_US, &pol, &utc));
    TEST_ASSERT_EQUAL_UINT64(POOL_TIME_EPOCH_MAX_S * POOL_TIME_US_PER_S, utc);
    /* one full second later the derived epoch exceeds the ceiling */
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_ABOVE_MAX,
                      pool_time_evaluate_trust(&a, MONO_M_US + POOL_TIME_US_PER_S, &pol, NULL));
}

TEST_CASE("trust: monotonic regression is rejected", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at(EPOCH_A_S * POOL_TIME_US_PER_S, MONO_M_US);
    TEST_ASSERT_EQUAL(TIME_ERR_MONOTONIC_REGRESSION,
                      pool_time_evaluate_trust(&a, MONO_M_US - 1u, &pol, NULL));
}

TEST_CASE("trust: required-minimum epoch enforced for cross-reboot checks", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at(EPOCH_A_S * POOL_TIME_US_PER_S, MONO_M_US);
    uint64_t utc = 0u;

    /* derived (EPOCH_A_S) earlier than a persisted verified-start: rejected */
    pol.required_min_epoch_s = EPOCH_A_S + 50u;
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN,
                      pool_time_evaluate_trust(&a, MONO_M_US, &pol, NULL));

    /* exactly at the required minimum: trusted */
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_evaluate_trust(&a, MONO_M_US + 50u * POOL_TIME_US_PER_S, &pol, &utc));
    TEST_ASSERT_EQUAL_UINT64((EPOCH_A_S + 50u) * POOL_TIME_US_PER_S, utc);
}

TEST_CASE("trust: valid anchor yields the exact derived estimate", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at(EPOCH_A_S * POOL_TIME_US_PER_S + 250000u, MONO_M_US);
    uint64_t utc = 0u;
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_evaluate_trust(&a, MONO_M_US + 1234567u, &pol, &utc));
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S * POOL_TIME_US_PER_S + 250000u + 1234567u, utc);
}

TEST_CASE("trust: anchor-plus-elapsed overflow is detected", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at(EPOCH_A_S * POOL_TIME_US_PER_S, 100u);
    /* enormous elapsed time pushes the addition past UINT64_MAX */
    TEST_ASSERT_EQUAL(TIME_ERR_OVERFLOW,
                      pool_time_evaluate_trust(&a, UINT64_MAX, &pol, NULL));
}

TEST_CASE("trust: null arguments are rejected deterministically", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at(EPOCH_A_S * POOL_TIME_US_PER_S, MONO_M_US);
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_evaluate_trust(NULL, MONO_M_US, &pol, NULL));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_evaluate_trust(&a, MONO_M_US, NULL, NULL));
}

/* ================================================================= */
/* C. Anchor / provider tests (fake SNTP completion)                  */
/* ================================================================= */

TEST_CASE("anchor: first accepted sync publishes generation 1 and trust", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    reset_fakes();
    make_trusted_provider(&p);
    s = snapshot_of(&p);
    TEST_ASSERT_TRUE(s.trusted);
    TEST_ASSERT_EQUAL(TIME_OK, s.status);
    TEST_ASSERT_EQUAL_UINT32(1u, s.sync_generation);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S, s.trusted_epoch_s);
    TEST_ASSERT_EQUAL_UINT64(0ull, s.anchor_age_us);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("anchor: candidate below the floor is rejected — provider stays untrusted", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    reset_fakes();
    make_started_provider(&p);
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BELOW_MIN,
                      pool_time_sntp_handle_sync(&p, POOL_TIME_EPOCH_MIN_S - 10u, 0u));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_SYNC_PENDING, pool_time_sntp_lifecycle(&p));
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BELOW_MIN, pool_time_sntp_last_error(&p));
    s = snapshot_of(&p);
    TEST_ASSERT_FALSE(s.trusted);
    TEST_ASSERT_EQUAL_UINT32(0u, s.sync_generation);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("anchor: forward resync accepted — generation increments", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    reset_fakes();
    make_trusted_provider(&p);

    g_fake_mono_us = MONO_M_US + 60u * POOL_TIME_US_PER_S;
    /* server 5 seconds ahead of the local estimate: forward, accepted */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, EPOCH_A_S + 65u, 0u));
    s = snapshot_of(&p);
    TEST_ASSERT_TRUE(s.trusted);
    TEST_ASSERT_EQUAL_UINT32(2u, s.sync_generation);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 65u, s.trusted_epoch_s);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("anchor: generation saturates and never wraps", "[pool_time]")
{
    PoolTimeSntpProvider p;
    reset_fakes();
    make_trusted_provider(&p);
    p.anchor.generation = UINT32_MAX; /* white-box: force the ceiling */
    g_fake_mono_us = MONO_M_US + 10u * POOL_TIME_US_PER_S;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, EPOCH_A_S + 20u, 0u));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, p.anchor.generation);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("anchor: backward resync rejected — anchor byte-identical", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeAnchor before;
    reset_fakes();
    make_trusted_provider(&p);
    before = p.anchor;

    g_fake_mono_us = MONO_M_US + 30u * POOL_TIME_US_PER_S;
    /* server reports one hour in the past: would regress trusted time */
    TEST_ASSERT_EQUAL(TIME_ERR_TRUST_REGRESSION,
                      pool_time_sntp_handle_sync(&p, EPOCH_A_S - 3600u, 0u));
    TEST_ASSERT_EQUAL(0, memcmp(&before, &p.anchor, sizeof(before)));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_TRUSTED, pool_time_sntp_lifecycle(&p));
    /* even a 1 µs regression is rejected */
    TEST_ASSERT_EQUAL(TIME_ERR_TRUST_REGRESSION,
                      pool_time_sntp_handle_sync(&p, EPOCH_A_S + 29u, 999999u));
    TEST_ASSERT_EQUAL(0, memcmp(&before, &p.anchor, sizeof(before)));
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("anchor: out-of-band resync rejected — anchor byte-identical", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeAnchor before;
    reset_fakes();
    make_trusted_provider(&p);
    before = p.anchor;
    g_fake_mono_us = MONO_M_US + POOL_TIME_US_PER_S;
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_ABOVE_MAX,
                      pool_time_sntp_handle_sync(&p, POOL_TIME_EPOCH_MAX_S + 100u, 0u));
    TEST_ASSERT_EQUAL(0, memcmp(&before, &p.anchor, sizeof(before)));
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("anchor: deinit clears trust and the anchor", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    reset_fakes();
    make_trusted_provider(&p);
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_deinit(&p));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_UNINITIALIZED, pool_time_sntp_lifecycle(&p));
    TEST_ASSERT_FALSE(p.anchor.valid);
    s = snapshot_of(&p);
    TEST_ASSERT_FALSE(s.trusted);
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_INITIALIZED, s.status);
    TEST_ASSERT_EQUAL_UINT32(1, g_deinit_calls);
}

TEST_CASE("anchor: fresh provider after simulated reboot is untrusted", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    reset_fakes();
    make_trusted_provider(&p);
    (void)pool_time_sntp_deinit(&p);

    /* reboot: monotonic restarts near zero, previous anchor is gone */
    g_fake_mono_us = 5u;
    make_started_provider(&p);
    s = snapshot_of(&p);
    TEST_ASSERT_FALSE(s.trusted);
    TEST_ASSERT_EQUAL(TIME_ERR_SYNC_PENDING, s.status);
    TEST_ASSERT_EQUAL_UINT32(0u, s.sync_generation);
    /* a fresh sync re-establishes trust with a fresh generation */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, EPOCH_A_S + 7200u, 0u));
    s = snapshot_of(&p);
    TEST_ASSERT_TRUE(s.trusted);
    TEST_ASSERT_EQUAL_UINT32(1u, s.sync_generation);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("anchor: handle_sync after deinit is ignored safely", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSntpProvider before;
    reset_fakes();
    make_trusted_provider(&p);
    (void)pool_time_sntp_deinit(&p);
    before = p;
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_INITIALIZED,
                      pool_time_sntp_handle_sync(&p, EPOCH_A_S, 0u));
    TEST_ASSERT_EQUAL(0, memcmp(&before, &p, sizeof(before))); /* no mutation */
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_sntp_handle_sync(NULL, EPOCH_A_S, 0u));
}

/* ================================================================= */
/* D. Trusted-now and raw-system-clock isolation                      */
/* ================================================================= */

TEST_CASE("trusted-now: at the anchor instant the estimate equals the sync epoch", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    reset_fakes();
    make_trusted_provider(&p);
    g_fake_mono_us = MONO_M_US;
    s = snapshot_of(&p);
    TEST_ASSERT_TRUE(s.trusted);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S * POOL_TIME_US_PER_S, s.trusted_utc_us);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S, s.trusted_epoch_s);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("trusted-now: estimate advances exactly with monotonic time", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    reset_fakes();
    make_trusted_provider(&p);
    g_fake_mono_us = MONO_M_US + 90u * POOL_TIME_US_PER_S;
    s = snapshot_of(&p);
    TEST_ASSERT_TRUE(s.trusted);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 90u, s.trusted_epoch_s);
    TEST_ASSERT_EQUAL_UINT64(90u * POOL_TIME_US_PER_S, s.anchor_age_us);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("trusted-now: subsecond fractions carry into the estimate", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    reset_fakes();
    make_started_provider(&p);
    g_fake_mono_us = MONO_M_US;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, EPOCH_A_S, 500000u));
    g_fake_mono_us = MONO_M_US + 700000u; /* +0.7 s */
    s = snapshot_of(&p);
    TEST_ASSERT_TRUE(s.trusted);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S * POOL_TIME_US_PER_S + 1200000u, s.trusted_utc_us);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 1u, s.trusted_epoch_s); /* rolled the second */
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("trusted-now: repeated reads never decrease", "[pool_time]")
{
    PoolTimeSntpProvider p;
    uint64_t last = 0u;
    uint64_t offsets[6] = {0u, 1u, 1u, 1000u, POOL_TIME_US_PER_S, 3600u * POOL_TIME_US_PER_S};
    int i;
    reset_fakes();
    make_trusted_provider(&p);
    for (i = 0; i < 6; i++) {
        PoolTimeSnapshot s;
        g_fake_mono_us = MONO_M_US + offsets[i];
        s = snapshot_of(&p);
        TEST_ASSERT_TRUE(s.trusted);
        TEST_ASSERT_TRUE(s.trusted_utc_us >= last);
        last = s.trusted_utc_us;
    }
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("isolation: raw clock jumps can never move the trusted estimate", "[pool_time]")
{
    /* THE Stage-13 regression test: complete a sync at epoch A / monotonic M,
     * advance monotonic by X, then manipulate the raw system wall clock as a
     * Stratum settimeofday() would — forward, backward, repeatedly. The
     * trusted estimate must remain exactly A + X throughout: pool_time has
     * no code path that reads the raw clock. */
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    const uint64_t x_us = 300u * POOL_TIME_US_PER_S;
    int i;
    reset_fakes();
    make_trusted_provider(&p);
    g_fake_mono_us = MONO_M_US + x_us;

    /* huge forward raw jump */
    g_fake_raw_wall_clock_us = (POOL_TIME_EPOCH_MAX_S - 1u) * POOL_TIME_US_PER_S;
    s = snapshot_of(&p);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S * POOL_TIME_US_PER_S + x_us, s.trusted_utc_us);

    /* huge backward raw jump (pre-band epoch) */
    g_fake_raw_wall_clock_us = 1000u;
    s = snapshot_of(&p);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S * POOL_TIME_US_PER_S + x_us, s.trusted_utc_us);

    /* repeated erratic manipulation */
    for (i = 0; i < 8; i++) {
        g_fake_raw_wall_clock_us = (i % 2 == 0) ? (uint64_t)i * 1234567u
                                                : UINT64_MAX - (uint64_t)i;
        s = snapshot_of(&p);
        TEST_ASSERT_TRUE(s.trusted);
        TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S * POOL_TIME_US_PER_S + x_us, s.trusted_utc_us);
    }
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("isolation: a later valid SNTP re-anchor refreshes without regression", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot before, after;
    reset_fakes();
    make_trusted_provider(&p);

    g_fake_mono_us = MONO_M_US + 120u * POOL_TIME_US_PER_S;
    before = snapshot_of(&p);
    /* re-anchor 3 s ahead of the local estimate (crystal drift correction) */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, EPOCH_A_S + 123u, 0u));
    after = snapshot_of(&p);
    TEST_ASSERT_TRUE(after.trusted);
    TEST_ASSERT_TRUE(after.trusted_utc_us >= before.trusted_utc_us);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 123u, after.trusted_epoch_s);
    TEST_ASSERT_EQUAL_UINT32(2u, after.sync_generation);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("isolation: a rejected backward re-anchor keeps the original trusted line", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    reset_fakes();
    make_trusted_provider(&p);

    g_fake_mono_us = MONO_M_US + 200u * POOL_TIME_US_PER_S;
    TEST_ASSERT_EQUAL(TIME_ERR_TRUST_REGRESSION,
                      pool_time_sntp_handle_sync(&p, EPOCH_A_S + 100u, 0u)); /* 100 < 200 */
    s = snapshot_of(&p);
    TEST_ASSERT_TRUE(s.trusted);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 200u, s.trusted_epoch_s); /* original anchor */
    TEST_ASSERT_EQUAL_UINT32(1u, s.sync_generation);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("trusted-now: arithmetic overflow surfaces as untrusted", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    reset_fakes();
    make_trusted_provider(&p);
    p.anchor.monotonic_us_at_sync = 0u; /* white-box: maximize elapsed */
    g_fake_mono_us = UINT64_MAX;
    s = snapshot_of(&p);
    TEST_ASSERT_FALSE(s.trusted);
    TEST_ASSERT_EQUAL(TIME_ERR_OVERFLOW, s.status);
    (void)pool_time_sntp_deinit(&p);
}

/* ================================================================= */
/* E. Monotonic deadline tests                                        */
/* ================================================================= */

TEST_CASE("deadline-mono: arm at the 15-minute minimum", "[pool_time]")
{
    PoolTimeMonotonicDeadline d;
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_arm_monotonic_deadline(MONO_M_US, POOL_TIME_MIN_DURATION_S, &d));
    TEST_ASSERT_TRUE(d.valid);
    TEST_ASSERT_EQUAL_UINT64(MONO_M_US, d.armed_at_monotonic_us);
    TEST_ASSERT_EQUAL_UINT64(MONO_M_US + 900ull * POOL_TIME_US_PER_S, d.deadline_monotonic_us);
    TEST_ASSERT_EQUAL_UINT32(900u, d.requested_duration_s);
}

TEST_CASE("deadline-mono: arm at the 24-hour maximum", "[pool_time]")
{
    PoolTimeMonotonicDeadline d;
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_arm_monotonic_deadline(MONO_M_US, POOL_TIME_MAX_DURATION_S, &d));
    TEST_ASSERT_TRUE(d.valid);
    TEST_ASSERT_EQUAL_UINT64(MONO_M_US + 86400ull * POOL_TIME_US_PER_S, d.deadline_monotonic_us);
}

TEST_CASE("deadline-mono: out-of-bound durations rejected", "[pool_time]")
{
    PoolTimeMonotonicDeadline d;
    TEST_ASSERT_EQUAL(TIME_ERR_DURATION_INVALID, pool_time_arm_monotonic_deadline(0u, 0u, &d));
    TEST_ASSERT_FALSE(d.valid);
    TEST_ASSERT_EQUAL(TIME_ERR_DURATION_INVALID,
                      pool_time_arm_monotonic_deadline(0u, POOL_TIME_MIN_DURATION_S - 1u, &d));
    TEST_ASSERT_EQUAL(TIME_ERR_DURATION_INVALID,
                      pool_time_arm_monotonic_deadline(0u, POOL_TIME_MAX_DURATION_S + 1u, &d));
    /* UINT32_MAX seconds could overflow a µs conversion — the duration bound
     * rejects it first (and the conversion of any VALID duration is
     * statically proven overflow-free in pool_time.h) */
    TEST_ASSERT_EQUAL(TIME_ERR_DURATION_INVALID,
                      pool_time_arm_monotonic_deadline(0u, UINT32_MAX, &d));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT,
                      pool_time_arm_monotonic_deadline(0u, 3600u, NULL));
}

TEST_CASE("deadline-mono: arming near the monotonic ceiling detects overflow", "[pool_time]")
{
    PoolTimeMonotonicDeadline d;
    TEST_ASSERT_EQUAL(TIME_ERR_OVERFLOW,
                      pool_time_arm_monotonic_deadline(UINT64_MAX - 1000u, 900u, &d));
    TEST_ASSERT_FALSE(d.valid);
}

TEST_CASE("deadline-mono: expiry boundary behavior is exact", "[pool_time]")
{
    PoolTimeMonotonicDeadline d;
    PoolTimeDeadlineCheck c;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_arm_monotonic_deadline(MONO_M_US, 900u, &d));

    /* one microsecond before expiry */
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_check_monotonic_deadline(&d, d.deadline_monotonic_us - 1u, &c));
    TEST_ASSERT_FALSE(c.expired);
    TEST_ASSERT_EQUAL_UINT64(1ull, c.remaining_us);
    TEST_ASSERT_EQUAL_UINT32(1u, c.remaining_s); /* ceiling */

    /* exact expiry */
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_check_monotonic_deadline(&d, d.deadline_monotonic_us, &c));
    TEST_ASSERT_TRUE(c.expired);
    TEST_ASSERT_EQUAL_UINT64(0ull, c.remaining_us);
    TEST_ASSERT_EQUAL_UINT32(0u, c.remaining_s);

    /* after expiry */
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_check_monotonic_deadline(&d, d.deadline_monotonic_us + 5u, &c));
    TEST_ASSERT_TRUE(c.expired);
}

TEST_CASE("deadline-mono: monotonic regression fails SAFE toward restore", "[pool_time]")
{
    PoolTimeMonotonicDeadline d;
    PoolTimeDeadlineCheck c;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_arm_monotonic_deadline(MONO_M_US, 900u, &d));
    TEST_ASSERT_EQUAL(TIME_ERR_MONOTONIC_REGRESSION,
                      pool_time_check_monotonic_deadline(&d, MONO_M_US - 1u, &c));
    /* fail-safe: reported due, never extra target time */
    TEST_ASSERT_TRUE(c.expired);
    TEST_ASSERT_EQUAL_UINT64(0ull, c.remaining_us);
}

TEST_CASE("deadline-mono: remaining-time ceiling rounding contract", "[pool_time]")
{
    PoolTimeMonotonicDeadline d;
    PoolTimeDeadlineCheck c;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_arm_monotonic_deadline(0u, 3600u, &d));

    /* exactly 1.000000 s remaining -> 1 s (not 2) */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_check_monotonic_deadline(
                                   &d, d.deadline_monotonic_us - POOL_TIME_US_PER_S, &c));
    TEST_ASSERT_EQUAL_UINT32(1u, c.remaining_s);

    /* 1.000001 s remaining -> 2 s */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_check_monotonic_deadline(
                                   &d, d.deadline_monotonic_us - POOL_TIME_US_PER_S - 1u, &c));
    TEST_ASSERT_EQUAL_UINT32(2u, c.remaining_s);

    /* 0.999999 s remaining -> 1 s */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_check_monotonic_deadline(
                                   &d, d.deadline_monotonic_us - POOL_TIME_US_PER_S + 1u, &c));
    TEST_ASSERT_EQUAL_UINT32(1u, c.remaining_s);

    /* full duration remaining */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_check_monotonic_deadline(&d, 0u, &c));
    TEST_ASSERT_EQUAL_UINT32(3600u, c.remaining_s);
    TEST_ASSERT_EQUAL_UINT64(3600ull * POOL_TIME_US_PER_S, c.remaining_us);
}

TEST_CASE("deadline-mono: invalid deadline structs rejected", "[pool_time]")
{
    PoolTimeMonotonicDeadline d;
    PoolTimeDeadlineCheck c;
    memset(&d, 0, sizeof(d)); /* not armed */
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT,
                      pool_time_check_monotonic_deadline(&d, MONO_M_US, &c));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT,
                      pool_time_check_monotonic_deadline(NULL, MONO_M_US, &c));
    /* corrupt: deadline earlier than the arming instant */
    d.valid = true;
    d.armed_at_monotonic_us = 1000u;
    d.deadline_monotonic_us = 999u;
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT,
                      pool_time_check_monotonic_deadline(&d, MONO_M_US, &c));
}

/* ================================================================= */
/* F. UTC deadline tests                                              */
/* ================================================================= */

TEST_CASE("deadline-utc: created only from a trusted snapshot", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    PoolTimeUtcDeadline d;
    PoolTimeTrustPolicy pol = default_policy();
    reset_fakes();
    make_trusted_provider(&p);
    g_fake_mono_us = MONO_M_US + 10u * POOL_TIME_US_PER_S;
    s = snapshot_of(&p);
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));
    TEST_ASSERT_TRUE(d.valid);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 10u + 3600u, d.deadline_epoch_s);
    TEST_ASSERT_EQUAL_UINT32(3600u, d.requested_duration_s);
    TEST_ASSERT_EQUAL_UINT32(1u, d.created_generation);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("deadline-utc: from-clock path reads the provider-owned anchor", "[pool_time]")
{
    /* The misuse-resistant path: no caller-supplied snapshot at all. */
    PoolTimeSntpProvider p;
    PoolTimeClock clk;
    PoolTimeSnapshot used;
    PoolTimeUtcDeadline d;
    PoolTimeTrustPolicy pol = default_policy();
    reset_fakes();

    /* trusted provider: deadline minted from the accepted anchor */
    make_trusted_provider(&p);
    g_fake_mono_us = MONO_M_US + 20u * POOL_TIME_US_PER_S;
    pool_time_sntp_get_clock(&p, &clk);
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_create_utc_deadline_from_clock(&clk, &pol, 3600u, &used, &d));
    TEST_ASSERT_TRUE(d.valid);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 20u + 3600u, d.deadline_epoch_s);
    TEST_ASSERT_TRUE(used.trusted);
    TEST_ASSERT_EQUAL_UINT32(1u, used.sync_generation);
    (void)pool_time_sntp_deinit(&p);

    /* pending (never-synced) provider: rejected with the snapshot's reason */
    g_fake_mono_us = 7u;
    make_started_provider(&p);
    pool_time_sntp_get_clock(&p, &clk);
    TEST_ASSERT_EQUAL(TIME_ERR_SYNC_PENDING,
                      pool_time_create_utc_deadline_from_clock(&clk, &pol, 3600u, NULL, &d));
    TEST_ASSERT_FALSE(d.valid);
    (void)pool_time_sntp_deinit(&p);

    /* null/malformed clock: rejected, never a deadline */
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_INITIALIZED,
                      pool_time_create_utc_deadline_from_clock(NULL, &pol, 3600u, NULL, &d));
    TEST_ASSERT_FALSE(d.valid);
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT,
                      pool_time_create_utc_deadline_from_clock(NULL, &pol, 3600u, NULL, NULL));
}

TEST_CASE("deadline-utc: duration bounds enforced", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    PoolTimeUtcDeadline d;
    PoolTimeTrustPolicy pol = default_policy();
    reset_fakes();
    make_trusted_provider(&p);
    s = snapshot_of(&p);
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_create_utc_deadline(&s, &pol, POOL_TIME_MIN_DURATION_S, &d));
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 900u, d.deadline_epoch_s);
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_create_utc_deadline(&s, &pol, POOL_TIME_MAX_DURATION_S, &d));
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 86400u, d.deadline_epoch_s);
    TEST_ASSERT_EQUAL(TIME_ERR_DURATION_INVALID,
                      pool_time_create_utc_deadline(&s, &pol, POOL_TIME_MIN_DURATION_S - 1u, &d));
    TEST_ASSERT_FALSE(d.valid);
    TEST_ASSERT_EQUAL(TIME_ERR_DURATION_INVALID,
                      pool_time_create_utc_deadline(&s, &pol, POOL_TIME_MAX_DURATION_S + 1u, &d));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_create_utc_deadline(&s, &pol, 3600u, NULL));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_create_utc_deadline(NULL, &pol, 3600u, &d));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_create_utc_deadline(&s, NULL, 3600u, &d));
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("deadline-utc: untrusted snapshots can never mint a deadline", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    PoolTimeUtcDeadline d;
    PoolTimeTrustPolicy pol = default_policy();
    reset_fakes();
    make_started_provider(&p); /* pending, never synced */
    s = snapshot_of(&p);
    TEST_ASSERT_FALSE(s.trusted);
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));
    TEST_ASSERT_FALSE(d.valid);

    /* a forged "trusted" flag with a failed status is still rejected */
    s.trusted = true;
    s.status = TIME_ERR_NOT_SYNCED;
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("deadline-utc: manually inconsistent snapshots are rejected", "[pool_time]")
{
    /* CONSISTENCY CHECKING, not authentication: every combination below is
     * internally inconsistent and therefore detectable by pure code. A FULLY
     * self-consistent fabricated snapshot is NOT detectable — no MAC or
     * signature exists — and no test here claims otherwise; that boundary is
     * documented in pool_time.h. */
    PoolTimeSnapshot good;
    PoolTimeSnapshot s;
    PoolTimeUtcDeadline d;
    PoolTimeTrustPolicy pol = default_policy();

    memset(&good, 0, sizeof(good));
    good.monotonic_now_us = MONO_M_US;
    good.trusted          = true;
    good.status           = TIME_OK;
    good.trusted_epoch_s  = EPOCH_A_S;
    good.trusted_utc_us   = EPOCH_A_S * POOL_TIME_US_PER_S;
    good.anchor_age_us    = 0u;
    good.sync_generation  = 1u;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_create_utc_deadline(&good, &pol, 3600u, &d));

    /* trusted=true but status != TIME_OK */
    s = good;
    s.status = TIME_ERR_SYNC_PENDING;
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));
    TEST_ASSERT_FALSE(d.valid);

    /* trusted=true with sync_generation == 0 (no accepted sync can do this) */
    s = good;
    s.sync_generation = 0u;
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));

    /* trusted=true with an epoch below the sanity floor (fields consistent) */
    s = good;
    s.trusted_epoch_s = POOL_TIME_EPOCH_MIN_S - 10u;
    s.trusted_utc_us  = s.trusted_epoch_s * POOL_TIME_US_PER_S;
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BELOW_MIN, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));

    /* trusted=true with an epoch above the sanity ceiling */
    s = good;
    s.trusted_epoch_s = POOL_TIME_EPOCH_MAX_S + 10u;
    s.trusted_utc_us  = s.trusted_epoch_s * POOL_TIME_US_PER_S;
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_ABOVE_MAX, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));

    /* trusted=true with disagreeing second/microsecond views */
    s = good;
    s.trusted_utc_us = (EPOCH_A_S + 5u) * POOL_TIME_US_PER_S;
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));

    /* trusted=true with an impossible anchor age (older than the boot) */
    s = good;
    s.anchor_age_us = s.monotonic_now_us + 1u;
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));

    /* extreme forged epoch: near the u64 ceiling the second/microsecond
     * views CANNOT be made to agree (epoch*1e6 no longer fits u64), so the
     * field-agreement check rejects it — and a CONSISTENT pair is capped at
     * ~1.8e13 s, which the sanity band rejects. Either way the (still
     * present) addition-overflow guard is structurally unreachable. */
    s = good;
    s.trusted_epoch_s = UINT64_MAX - 100u;
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));
    s = good;
    s.trusted_utc_us  = UINT64_MAX;                          /* consistent pair: */
    s.trusted_epoch_s = UINT64_MAX / POOL_TIME_US_PER_S;     /* ~1.8e13 s        */
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_ABOVE_MAX, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));

    /* trusted=false with an otherwise fully plausible epoch */
    s = good;
    s.trusted = false;
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));

    /* previous-boot / required-minimum mismatch: a persisted floor (Gate
     * B3/B4 supplies the latest accepted trusted epoch) rejects a snapshot
     * whose epoch sits behind it */
    s = good;
    pol.required_min_epoch_s = EPOCH_A_S + 1u;
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN,
                      pool_time_create_utc_deadline(&s, &pol, 3600u, &d));
    TEST_ASSERT_FALSE(d.valid);
}

TEST_CASE("deadline-utc: a previous-boot anchor cannot mint a deadline", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    PoolTimeUtcDeadline d;
    PoolTimeTrustPolicy pol = default_policy();
    reset_fakes();
    make_trusted_provider(&p);
    /* "reboot": the in-memory anchor does not survive */
    (void)pool_time_sntp_deinit(&p);
    g_fake_mono_us = 3u;
    make_started_provider(&p);
    s = snapshot_of(&p);
    TEST_ASSERT_FALSE(s.trusted);
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_create_utc_deadline(&s, &pol, 3600u, &d));
    TEST_ASSERT_FALSE(d.valid);
    (void)pool_time_sntp_deinit(&p);
}

/* ================================================================= */
/* G. Synchronization-window constants                                */
/* ================================================================= */

TEST_CASE("sync-window: bounded defaults are 10 min default / 15 min hard max", "[pool_time]")
{
    TEST_ASSERT_EQUAL_UINT32(600u, POOL_TIME_SYNC_WAIT_DEFAULT_S);
    TEST_ASSERT_EQUAL_UINT32(900u, POOL_TIME_SYNC_WAIT_MAX_S);
    TEST_ASSERT_TRUE(POOL_TIME_SYNC_WAIT_DEFAULT_S <= POOL_TIME_SYNC_WAIT_MAX_S);
}

/* ================================================================= */
/* H. Reboot/recovery decision tests                                  */
/* ================================================================= */

TEST_CASE("recovery: no source snapshot means RECOVERY_REQUIRED, never resume", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;
    in.source_snapshot_valid = false;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_RECOVERY_REQUIRED, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_NO_SOURCE_SNAPSHOT, r.status);
    TEST_ASSERT_TRUE(r.terminal_for_time);
    TEST_ASSERT_FALSE(r.requires_trusted_time);
    TEST_ASSERT_FALSE(r.remaining_valid);
}

TEST_CASE("recovery: no persisted UTC deadline fails safe immediately", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;
    in.utc_deadline_valid = false;
    in.time_trusted = false; /* even with no trust: no waiting at all */
    in.sync_wait_elapsed_s = 0u;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_FAIL_SAFE_RESTORE, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_NO_PERSISTED_DEADLINE, r.status);
    TEST_ASSERT_TRUE(r.terminal_for_time);
    TEST_ASSERT_FALSE(r.requires_trusted_time); /* fail-safe needs no clock */
}

TEST_CASE("recovery: trusted before the deadline resumes with exact remaining", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;
    in.trusted_epoch_s = in.deadline_epoch_s - 1234u;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_RESUME_TARGET_WITH_REMAINING_TIME, r.decision);
    TEST_ASSERT_EQUAL(TIME_OK, r.status);
    TEST_ASSERT_TRUE(r.remaining_valid);
    TEST_ASSERT_EQUAL_UINT64(1234ull, r.remaining_s);
    TEST_ASSERT_TRUE(r.requires_trusted_time);
}

TEST_CASE("recovery: trusted exactly at the deadline restores", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;
    in.trusted_epoch_s = in.deadline_epoch_s;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_RESTORE_DUE, r.decision);
    TEST_ASSERT_EQUAL(TIME_OK, r.status);
    TEST_ASSERT_TRUE(r.remaining_valid);
    TEST_ASSERT_EQUAL_UINT64(0ull, r.remaining_s);
}

TEST_CASE("recovery: trusted after the deadline restores", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;
    in.trusted_epoch_s = in.deadline_epoch_s + 7200u;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_RESTORE_DUE, r.decision);
    TEST_ASSERT_TRUE(r.terminal_for_time);
}

TEST_CASE("recovery: untrusted inside the window waits for trusted time", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;
    in.time_trusted = false;
    in.sync_wait_elapsed_s = in.sync_wait_limit_s - 1u;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_WAIT_FOR_TRUSTED_TIME, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, r.status);
    TEST_ASSERT_TRUE(r.requires_trusted_time);
    TEST_ASSERT_FALSE(r.terminal_for_time);
}

TEST_CASE("recovery: untrusted at and beyond the window fails safe", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;
    in.time_trusted = false;

    in.sync_wait_elapsed_s = in.sync_wait_limit_s; /* exact boundary */
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_FAIL_SAFE_RESTORE, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_SYNC_TIMEOUT, r.status);
    TEST_ASSERT_TRUE(r.terminal_for_time);
    TEST_ASSERT_FALSE(r.requires_trusted_time); /* restore needs no clock */

    in.sync_wait_elapsed_s = in.sync_wait_limit_s + 500u;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_FAIL_SAFE_RESTORE, r.decision);
}

TEST_CASE("recovery: trusted time before verified-start is rejected trust", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;
    /* claims trust, but earlier than the persisted verified start */
    in.trusted_epoch_s = in.verified_start_epoch_s - 10u;

    in.sync_wait_elapsed_s = 0u; /* still inside the window: keep waiting */
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_WAIT_FOR_TRUSTED_TIME, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN, r.status);

    in.sync_wait_elapsed_s = in.sync_wait_limit_s; /* window over: fail safe */
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_FAIL_SAFE_RESTORE, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN, r.status);
}

TEST_CASE("recovery: DNS/NTP unavailability is the bounded untrusted path", "[pool_time]")
{
    /* DNS failure, unreachable servers and sync timeouts all surface as
     * "time not trusted"; the decision layer must never allow indefinite
     * target mining out of them. */
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;
    uint32_t elapsed;
    in.time_trusted = false;
    for (elapsed = 0u; elapsed <= in.sync_wait_limit_s + 60u; elapsed += 60u) {
        in.sync_wait_elapsed_s = elapsed;
        r = pool_time_decide_recovery(&in, &pol);
        TEST_ASSERT_TRUE(r.decision == POOL_TIME_DECISION_WAIT_FOR_TRUSTED_TIME ||
                         r.decision == POOL_TIME_DECISION_FAIL_SAFE_RESTORE);
        if (elapsed >= in.sync_wait_limit_s) {
            TEST_ASSERT_EQUAL(POOL_TIME_DECISION_FAIL_SAFE_RESTORE, r.decision);
        }
        TEST_ASSERT_TRUE(r.decision != POOL_TIME_DECISION_RESUME_TARGET_WITH_REMAINING_TIME);
    }
}

TEST_CASE("recovery: corrupt wait window fails safe while the source is intact", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;

    in.sync_wait_limit_s = 0u;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_FAIL_SAFE_RESTORE, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_WAIT_WINDOW, r.status);

    in = recovery_base();
    in.sync_wait_limit_s = POOL_TIME_SYNC_WAIT_MAX_S + 1u;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_FAIL_SAFE_RESTORE, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_WAIT_WINDOW, r.status);

    /* corrupt window AND no source snapshot: recovery wins (checked first) */
    in.source_snapshot_valid = false;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_RECOVERY_REQUIRED, r.decision);
}

TEST_CASE("recovery: corrupt persisted epochs fail safe and never resume", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in;
    PoolTimeRecoveryResult r;

    /* deadline below the sanity floor */
    in = recovery_base();
    in.deadline_epoch_s = pol.min_epoch_s - 1u;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_FAIL_SAFE_RESTORE, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BELOW_MIN, r.status);

    /* deadline impossibly far above the ceiling */
    in = recovery_base();
    in.deadline_epoch_s = pol.max_epoch_s + (uint64_t)POOL_TIME_MAX_DURATION_S + 1u;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_FAIL_SAFE_RESTORE, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_ABOVE_MAX, r.status);

    /* verified start outside the band */
    in = recovery_base();
    in.verified_start_epoch_s = pol.min_epoch_s - 100u;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_FAIL_SAFE_RESTORE, r.decision);

    /* verified start AFTER the deadline: internally inconsistent */
    in = recovery_base();
    in.verified_start_epoch_s = in.deadline_epoch_s + 1u;
    r = pool_time_decide_recovery(&in, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_FAIL_SAFE_RESTORE, r.decision);
    TEST_ASSERT_TRUE(r.decision != POOL_TIME_DECISION_RESUME_TARGET_WITH_REMAINING_TIME);
}

TEST_CASE("recovery: null or invalid inputs demand recovery, never resume", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;

    r = pool_time_decide_recovery(NULL, &pol);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_RECOVERY_REQUIRED, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, r.status);
    TEST_ASSERT_TRUE(r.terminal_for_time);

    r = pool_time_decide_recovery(&in, NULL);
    TEST_ASSERT_EQUAL(POOL_TIME_DECISION_RECOVERY_REQUIRED, r.decision);
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, r.status);
}

TEST_CASE("recovery: remaining time is exact across magnitudes", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeRecoveryInput in = recovery_base();
    PoolTimeRecoveryResult r;
    uint64_t remains[4] = {1u, 900u, 43200u, 86400u};
    int i;
    for (i = 0; i < 4; i++) {
        in.deadline_epoch_s = EPOCH_A_S + 86500u;
        in.verified_start_epoch_s = EPOCH_A_S;
        in.trusted_epoch_s = in.deadline_epoch_s - remains[i];
        r = pool_time_decide_recovery(&in, &pol);
        TEST_ASSERT_EQUAL(POOL_TIME_DECISION_RESUME_TARGET_WITH_REMAINING_TIME, r.decision);
        TEST_ASSERT_TRUE(r.remaining_valid);
        TEST_ASSERT_EQUAL_UINT64(remains[i], r.remaining_s);
    }
}

/* ================================================================= */
/* I. Lifecycle / idempotency tests                                   */
/* ================================================================= */

TEST_CASE("lifecycle: initializing twice is a deterministic error", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSntpConfig cfg = valid_cfg();
    PoolTimeTrustPolicy pol = default_policy();
    reset_fakes();
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_init(&p, &FAKE_OPS, &cfg, &pol));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_INITIALIZED, pool_time_sntp_lifecycle(&p));
    TEST_ASSERT_EQUAL(TIME_ERR_SNTP_INIT, pool_time_sntp_init(&p, &FAKE_OPS, &cfg, &pol));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_INITIALIZED, pool_time_sntp_lifecycle(&p));
    TEST_ASSERT_EQUAL_UINT32(1, g_init_calls); /* second init never hit the platform */
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("lifecycle: start is deterministic and repeatable", "[pool_time]")
{
    PoolTimeSntpProvider p;
    reset_fakes();
    make_started_provider(&p);
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_SYNC_PENDING, pool_time_sntp_lifecycle(&p));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_start(&p)); /* restart */
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_SYNC_PENDING, pool_time_sntp_lifecycle(&p));
    TEST_ASSERT_EQUAL_UINT32(2, g_start_calls);
    /* restart with an accepted anchor keeps trust */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, EPOCH_A_S, 0u));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_start(&p));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_TRUSTED, pool_time_sntp_lifecycle(&p));
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("lifecycle: stop before start and double stop are deterministic", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSntpConfig cfg = valid_cfg();
    PoolTimeTrustPolicy pol = default_policy();
    reset_fakes();
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_init(&p, &FAKE_OPS, &cfg, &pol));
    /* stop before start: valid, ends in STOPPED */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_stop(&p));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_STOPPED, pool_time_sntp_lifecycle(&p));
    /* double stop: idempotent no-op, no extra platform call */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_stop(&p));
    TEST_ASSERT_EQUAL_UINT32(1, g_stop_calls);
    /* start again after stop */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_start(&p));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_SYNC_PENDING, pool_time_sntp_lifecycle(&p));
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("lifecycle: deinit twice is idempotent", "[pool_time]")
{
    PoolTimeSntpProvider p;
    reset_fakes();
    make_trusted_provider(&p);
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_deinit(&p));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_deinit(&p));
    TEST_ASSERT_EQUAL_UINT32(1, g_deinit_calls); /* second deinit is a pure no-op */
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_sntp_deinit(NULL));
}

TEST_CASE("lifecycle: operations before init are rejected", "[pool_time]")
{
    PoolTimeSntpProvider p;
    reset_fakes();
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_INITIALIZED, pool_time_sntp_start(&p));
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_INITIALIZED, pool_time_sntp_stop(&p));
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_INITIALIZED, pool_time_sntp_handle_sync(&p, EPOCH_A_S, 0u));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_sntp_start(NULL));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_sntp_stop(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, g_start_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_stop_calls);
}

TEST_CASE("lifecycle: init argument validation is total", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSntpConfig cfg = valid_cfg();
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeSntpPlatformOps broken = FAKE_OPS;
    reset_fakes();
    memset(&p, 0, sizeof(p));

    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_sntp_init(NULL, &FAKE_OPS, &cfg, &pol));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_sntp_init(&p, NULL, &cfg, &pol));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_sntp_init(&p, &FAKE_OPS, &cfg, NULL));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_sntp_init(&p, &FAKE_OPS, NULL, &pol));

    broken.monotonic_us = NULL;
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_sntp_init(&p, &broken, &cfg, &pol));

    /* invalid config / policy propagate their specific codes */
    cfg.server_count = 0u;
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG, pool_time_sntp_init(&p, &FAKE_OPS, &cfg, &pol));
    cfg = valid_cfg();
    cfg.sync_wait_s = POOL_TIME_SYNC_WAIT_MAX_S + 1u;
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_WAIT_WINDOW, pool_time_sntp_init(&p, &FAKE_OPS, &cfg, &pol));
    pol.min_epoch_s = 0u;
    cfg = valid_cfg();
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, pool_time_sntp_init(&p, &FAKE_OPS, &cfg, &pol));

    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_UNINITIALIZED, pool_time_sntp_lifecycle(&p));
    TEST_ASSERT_EQUAL_UINT32(0, g_init_calls);

    /* malformed sync fractions are rejected without anchor mutation */
    reset_fakes();
    make_started_provider(&p);
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT,
                      pool_time_sntp_handle_sync(&p, EPOCH_A_S, 1000000u));
    TEST_ASSERT_FALSE(p.anchor.valid);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("lifecycle: platform op failures land in a deterministic ERROR state", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSntpConfig cfg = valid_cfg();
    PoolTimeTrustPolicy pol = default_policy();
    reset_fakes();

    /* init op failure */
    memset(&p, 0, sizeof(p));
    g_init_result = -1;
    TEST_ASSERT_EQUAL(TIME_ERR_SNTP_INIT, pool_time_sntp_init(&p, &FAKE_OPS, &cfg, &pol));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_ERROR, pool_time_sntp_lifecycle(&p));
    TEST_ASSERT_EQUAL(TIME_ERR_SNTP_INIT, pool_time_sntp_last_error(&p));
    /* only deinit recovers */
    TEST_ASSERT_EQUAL(TIME_ERR_SNTP_START, pool_time_sntp_start(&p));
    TEST_ASSERT_EQUAL(TIME_ERR_SNTP_STOP, pool_time_sntp_stop(&p));
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_sntp_handle_sync(&p, EPOCH_A_S, 0u));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_deinit(&p));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_UNINITIALIZED, pool_time_sntp_lifecycle(&p));

    /* start op failure */
    reset_fakes();
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_init(&p, &FAKE_OPS, &cfg, &pol));
    g_start_result = -1;
    TEST_ASSERT_EQUAL(TIME_ERR_SNTP_START, pool_time_sntp_start(&p));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_ERROR, pool_time_sntp_lifecycle(&p));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_deinit(&p));

    /* stop op failure */
    reset_fakes();
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_init(&p, &FAKE_OPS, &cfg, &pol));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_start(&p));
    g_stop_result = -1;
    TEST_ASSERT_EQUAL(TIME_ERR_SNTP_STOP, pool_time_sntp_stop(&p));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_ERROR, pool_time_sntp_lifecycle(&p));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_deinit(&p));
}

TEST_CASE("lifecycle: snapshot states across the sync journey", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    PoolTimeSnapshot trusted_before;
    reset_fakes();

    /* during pending sync: untrusted, SYNC_PENDING */
    make_started_provider(&p);
    s = snapshot_of(&p);
    TEST_ASSERT_FALSE(s.trusted);
    TEST_ASSERT_EQUAL(TIME_ERR_SYNC_PENDING, s.status);

    /* after accepted sync: trusted */
    g_fake_mono_us = MONO_M_US;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, EPOCH_A_S, 0u));
    s = snapshot_of(&p);
    TEST_ASSERT_TRUE(s.trusted);

    /* after a rejected resync: the prior trusted line is intact */
    g_fake_mono_us = MONO_M_US + 50u * POOL_TIME_US_PER_S;
    trusted_before = snapshot_of(&p);
    TEST_ASSERT_EQUAL(TIME_ERR_TRUST_REGRESSION,
                      pool_time_sntp_handle_sync(&p, EPOCH_A_S, 0u)); /* now stale */
    s = snapshot_of(&p);
    TEST_ASSERT_TRUE(s.trusted);
    TEST_ASSERT_EQUAL_UINT64(trusted_before.trusted_utc_us, s.trusted_utc_us);
    TEST_ASSERT_EQUAL_UINT32(trusted_before.sync_generation, s.sync_generation);
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("lifecycle: stop retains the anchor — trust persists until deinit", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    reset_fakes();
    make_trusted_provider(&p);
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_stop(&p));
    TEST_ASSERT_EQUAL(POOL_TIME_SNTP_STOPPED, pool_time_sntp_lifecycle(&p));
    g_fake_mono_us = MONO_M_US + 5u * POOL_TIME_US_PER_S;
    s = snapshot_of(&p);
    TEST_ASSERT_TRUE(s.trusted); /* the in-boot anchor is still the truth */
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 5u, s.trusted_epoch_s);
    /* but a sync arriving while stopped is ignored without mutation */
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED, pool_time_sntp_handle_sync(&p, EPOCH_A_S + 100u, 0u));
    (void)pool_time_sntp_deinit(&p);
}

/* ================================================================= */
/* J. Privacy tests                                                   */
/* ================================================================= */

TEST_CASE("privacy: every status token is a clean machine token", "[pool_time]")
{
    int i;
    for (i = 0; i < (int)POOL_TIME_ERR__COUNT; i++) {
        assert_clean_token(pool_time_error_str((PoolTimeError)i));
    }
    for (i = 0; i < (int)POOL_TIME_DECISION__COUNT; i++) {
        assert_clean_token(pool_time_decision_str((PoolTimeRecoveryDecision)i));
    }
    for (i = 0; i < (int)POOL_TIME_SYNC_STATUS__COUNT; i++) {
        assert_clean_token(pool_time_sync_status_str((PoolTimeSyncStatus)i));
    }
    for (i = 0; i < (int)POOL_TIME_SNTP_LIFECYCLE__COUNT; i++) {
        assert_clean_token(pool_time_sntp_lifecycle_str((PoolTimeSntpLifecycle)i));
    }
}

TEST_CASE("privacy: server names never reach anchors, snapshots or tokens", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeSnapshot s;
    PoolTimeSntpConfig cfg = valid_cfg();
    PoolTimeTrustPolicy pol = default_policy();
    const char *marker = "ntp-marker.example";
    int i;

    reset_fakes();
    strncpy(cfg.servers[0], marker, sizeof(cfg.servers[0]) - 1);
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_init(&p, &FAKE_OPS, &cfg, &pol));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_start(&p));
    g_fake_mono_us = MONO_M_US;
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, EPOCH_A_S, 0u));
    s = snapshot_of(&p);

    /* the config is the marker's only legitimate home */
    TEST_ASSERT_TRUE(bytes_contain(&p.config, sizeof(p.config), "marker"));
    /* it must never appear in the anchor, the snapshot, or any token */
    TEST_ASSERT_FALSE(bytes_contain(&p.anchor, sizeof(p.anchor), "marker"));
    TEST_ASSERT_FALSE(bytes_contain(&s, sizeof(s), "marker"));
    for (i = 0; i < (int)POOL_TIME_ERR__COUNT; i++) {
        TEST_ASSERT_NULL(strstr(pool_time_error_str((PoolTimeError)i), "marker"));
        TEST_ASSERT_NULL(strstr(pool_time_error_str((PoolTimeError)i), "ntp"));
    }
    (void)pool_time_sntp_deinit(&p);
}

/* ================================================================= */
/* K. Property-style tests                                            */
/* ================================================================= */

TEST_CASE("property: all error values map to distinct stable tokens", "[pool_time]")
{
    int i, j;
    for (i = 0; i < (int)POOL_TIME_ERR__COUNT; i++) {
        const char *a = pool_time_error_str((PoolTimeError)i);
        /* deterministic: same pointer on every call */
        TEST_ASSERT_EQUAL_PTR(a, pool_time_error_str((PoolTimeError)i));
        for (j = i + 1; j < (int)POOL_TIME_ERR__COUNT; j++) {
            TEST_ASSERT_TRUE(strcmp(a, pool_time_error_str((PoolTimeError)j)) != 0);
        }
    }
}

TEST_CASE("property: recovery decisions are total and never unsafe", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    const uint64_t epochs[4] = {POOL_TIME_EPOCH_MIN_S, EPOCH_A_S,
                                EPOCH_A_S + 86400u, POOL_TIME_EPOCH_MAX_S};
    const uint32_t elapsed[4] = {0u, 599u, 600u, 900u};
    int src, dl, vs, tr, e1, e2, e3;

    for (src = 0; src <= 1; src++)
    for (dl = 0; dl <= 1; dl++)
    for (vs = 0; vs <= 1; vs++)
    for (tr = 0; tr <= 1; tr++)
    for (e1 = 0; e1 < 4; e1++)
    for (e2 = 0; e2 < 4; e2++)
    for (e3 = 0; e3 < 4; e3++) {
        PoolTimeRecoveryInput in;
        PoolTimeRecoveryInput copy;
        PoolTimeRecoveryResult r;
        memset(&in, 0, sizeof(in));
        in.source_snapshot_valid = (src == 1);
        in.utc_deadline_valid = (dl == 1);
        in.deadline_epoch_s = epochs[e1];
        in.verified_start_epoch_valid = (vs == 1);
        in.verified_start_epoch_s = epochs[e2];
        in.time_trusted = (tr == 1);
        in.trusted_epoch_s = epochs[(e1 + e2) % 4];
        in.sync_wait_elapsed_s = elapsed[e3];
        in.sync_wait_limit_s = POOL_TIME_SYNC_WAIT_DEFAULT_S;
        copy = in;

        r = pool_time_decide_recovery(&in, &pol);

        /* totality: always a valid decision and a valid status token */
        TEST_ASSERT_TRUE(r.decision < POOL_TIME_DECISION__COUNT);
        TEST_ASSERT_TRUE(r.status < POOL_TIME_ERR__COUNT);
        /* purity: the input is byte-identical afterwards */
        TEST_ASSERT_EQUAL(0, memcmp(&copy, &in, sizeof(copy)));
        /* safety: resume ONLY on trusted time, a valid source AND deadline,
         * strictly before the deadline */
        if (r.decision == POOL_TIME_DECISION_RESUME_TARGET_WITH_REMAINING_TIME) {
            TEST_ASSERT_TRUE(in.source_snapshot_valid);
            TEST_ASSERT_TRUE(in.utc_deadline_valid);
            TEST_ASSERT_TRUE(in.time_trusted);
            TEST_ASSERT_TRUE(in.trusted_epoch_s < in.deadline_epoch_s);
            TEST_ASSERT_TRUE(r.remaining_valid);
        }
        /* safety: without a source snapshot the only answer is recovery */
        if (!in.source_snapshot_valid) {
            TEST_ASSERT_EQUAL(POOL_TIME_DECISION_RECOVERY_REQUIRED, r.decision);
        }
        /* liveness: WAIT is only ever offered inside the bounded window */
        if (r.decision == POOL_TIME_DECISION_WAIT_FOR_TRUSTED_TIME) {
            TEST_ASSERT_TRUE(in.sync_wait_elapsed_s < in.sync_wait_limit_s);
            TEST_ASSERT_FALSE(r.terminal_for_time);
        }
    }
}

TEST_CASE("property: deadline helpers never wrap for edge inputs", "[pool_time]")
{
    const uint64_t monos[5] = {0u, 1u, UINT64_MAX / 2u, UINT64_MAX - 1u, UINT64_MAX};
    const uint32_t durs[3] = {POOL_TIME_MIN_DURATION_S, 3600u, POOL_TIME_MAX_DURATION_S};
    int i, j;
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 3; j++) {
            PoolTimeMonotonicDeadline d;
            PoolTimeError e = pool_time_arm_monotonic_deadline(monos[i], durs[j], &d);
            if (e == TIME_OK) {
                TEST_ASSERT_TRUE(d.valid);
                TEST_ASSERT_TRUE(d.deadline_monotonic_us > monos[i]); /* no wrap */
                TEST_ASSERT_TRUE(d.deadline_monotonic_us - monos[i] ==
                                 (uint64_t)durs[j] * POOL_TIME_US_PER_S);
            } else {
                TEST_ASSERT_EQUAL(TIME_ERR_OVERFLOW, e);
                TEST_ASSERT_FALSE(d.valid);
            }
        }
    }
}

TEST_CASE("property: trusted estimate is nondecreasing in monotonic time", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeAnchor a = anchor_at(EPOCH_A_S * POOL_TIME_US_PER_S, MONO_M_US);
    const uint64_t steps[7] = {0u, 1u, 2u, 999u, 1000000u, 86400000000u, 999999999999u};
    uint64_t last = 0u;
    int i;
    for (i = 0; i < 7; i++) {
        uint64_t utc = 0u;
        TEST_ASSERT_EQUAL(TIME_OK,
                          pool_time_evaluate_trust(&a, MONO_M_US + steps[i], &pol, &utc));
        TEST_ASSERT_TRUE(utc >= last);
        last = utc;
    }
}

TEST_CASE("property: invalid anchors are never trusted", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    int variant;
    for (variant = 0; variant < 5; variant++) {
        PoolTimeAnchor a = anchor_at(EPOCH_A_S * POOL_TIME_US_PER_S, MONO_M_US);
        switch (variant) {
        case 0: a.valid = false; break;
        case 1: a.sync_completed_this_boot = false; break;
        case 2: a.epoch_us_at_sync = (POOL_TIME_EPOCH_MIN_S - 1000u) * POOL_TIME_US_PER_S; break;
        case 3: a.epoch_us_at_sync = (POOL_TIME_EPOCH_MAX_S + 1000u) * POOL_TIME_US_PER_S; break;
        case 4: a.epoch_us_at_sync = 0u; break;
        default: break;
        }
        TEST_ASSERT_TRUE(pool_time_evaluate_trust(&a, MONO_M_US + 1000u, &pol, NULL) != TIME_OK);
        TEST_ASSERT_TRUE(pool_time_validate_anchor(&a, &pol) != TIME_OK);
    }
}

TEST_CASE("property: rejected resyncs never change the accepted anchor", "[pool_time]")
{
    PoolTimeSntpProvider p;
    PoolTimeAnchor accepted;
    int i;
    reset_fakes();
    make_trusted_provider(&p);
    accepted = p.anchor;
    g_fake_mono_us = MONO_M_US + 400u * POOL_TIME_US_PER_S;

    for (i = 0; i < 6; i++) {
        uint64_t bad_epochs[6] = {
            EPOCH_A_S,                       /* stale: regression       */
            EPOCH_A_S + 100u,                /* still behind the line   */
            POOL_TIME_EPOCH_MIN_S - 5u,      /* below the floor         */
            POOL_TIME_EPOCH_MAX_S + 5u,      /* above the ceiling       */
            1u,                              /* absurd past             */
            EPOCH_A_S + 399u,                /* one second short        */
        };
        TEST_ASSERT_TRUE(pool_time_sntp_handle_sync(&p, bad_epochs[i], 0u) != TIME_OK);
        TEST_ASSERT_EQUAL(0, memcmp(&accepted, &p.anchor, sizeof(accepted)));
    }
    /* the provider still reports the untouched trusted line */
    {
        PoolTimeSnapshot s = snapshot_of(&p);
        TEST_ASSERT_TRUE(s.trusted);
        TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 400u, s.trusted_epoch_s);
    }
    (void)pool_time_sntp_deinit(&p);
}

TEST_CASE("property: pure evaluators never mutate their inputs", "[pool_time]")
{
    PoolTimeTrustPolicy pol = default_policy();
    PoolTimeTrustPolicy pol_copy = pol;
    PoolTimeAnchor a = anchor_at(EPOCH_A_S * POOL_TIME_US_PER_S + 123u, MONO_M_US);
    PoolTimeAnchor a_copy = a;
    PoolTimeMonotonicDeadline d;
    PoolTimeMonotonicDeadline d_copy;
    PoolTimeSnapshot s;
    PoolTimeSnapshot s_copy;
    PoolTimeDeadlineCheck c;
    PoolTimeUtcDeadline u;

    (void)pool_time_evaluate_trust(&a, MONO_M_US + 5u, &pol, NULL);
    TEST_ASSERT_EQUAL(0, memcmp(&a_copy, &a, sizeof(a)));
    TEST_ASSERT_EQUAL(0, memcmp(&pol_copy, &pol, sizeof(pol)));

    TEST_ASSERT_EQUAL(TIME_OK, pool_time_arm_monotonic_deadline(MONO_M_US, 3600u, &d));
    d_copy = d;
    (void)pool_time_check_monotonic_deadline(&d, MONO_M_US + 5u, &c);
    TEST_ASSERT_EQUAL(0, memcmp(&d_copy, &d, sizeof(d)));

    memset(&s, 0, sizeof(s));
    s.trusted = true;
    s.status = TIME_OK;
    s.trusted_epoch_s = EPOCH_A_S;
    s.trusted_utc_us = EPOCH_A_S * POOL_TIME_US_PER_S;
    s.sync_generation = 3u;
    s_copy = s;
    (void)pool_time_create_utc_deadline(&s, &pol, 3600u, &u);
    TEST_ASSERT_EQUAL(0, memcmp(&s_copy, &s, sizeof(s)));
    TEST_ASSERT_EQUAL(0, memcmp(&pol_copy, &pol, sizeof(pol)));

    (void)pool_time_validate_reanchor(&a, &a, &pol);
    TEST_ASSERT_EQUAL(0, memcmp(&a_copy, &a, sizeof(a)));
}
