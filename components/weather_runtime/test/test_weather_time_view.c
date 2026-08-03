/*
 * NeuralAxe Gate W4 — trusted-time convergence tests.
 *
 * Deterministic fakes only: a fake PoolTimeClock, no SNTP, no network, no
 * wall clock, no hardware. The central proof is the NARROWING property —
 * weather can only ever refuse what Gate B2 accepted, never grant what B2
 * refused.
 */

#include <string.h>
#include "unity.h"
#include "weather_time_view.h"

/* ------------------------------------------------------------------ */
/* Deterministic fake clock (the ONLY time source in this file)        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t       monotonic_us;
    PoolTimeAnchor anchor;
    bool           initialized;
    int            monotonic_calls;
    int            anchor_calls;
} FakeClock;

static FakeClock g_fc;

static uint64_t fc_monotonic(void *ctx)
{
    FakeClock *c = (FakeClock *)ctx;
    c->monotonic_calls++;
    return c->monotonic_us;
}

static bool fc_read_anchor(void *ctx, PoolTimeAnchor *out)
{
    FakeClock *c = (FakeClock *)ctx;
    c->anchor_calls++;
    if (!c->initialized) {
        return false;
    }
    *out = c->anchor;
    return true;
}

static const PoolTimeClockOps FC_OPS = {
    .monotonic_us = fc_monotonic,
    .read_anchor  = fc_read_anchor,
};

#define US_PER_S 1000000ull

static PoolTimeTrustPolicy policy(void)
{
    PoolTimeTrustPolicy p;
    pool_time_trust_policy_defaults(&p);
    return p;
}

static PoolTimeClock clock_handle(void)
{
    PoolTimeClock c;
    c.ops = &FC_OPS;
    c.ctx = &g_fc;
    return c;
}

/* A healthy anchor: synced now, epoch well inside the sanity band. */
static void fake_synced(uint64_t epoch_s, uint64_t age_s)
{
    memset(&g_fc, 0, sizeof(g_fc));
    g_fc.initialized              = true;
    g_fc.monotonic_us             = (age_s + 10u) * US_PER_S;
    g_fc.anchor.valid             = true;
    g_fc.anchor.sync_completed_this_boot = true;
    g_fc.anchor.generation        = 7u;
    g_fc.anchor.epoch_us_at_sync  = epoch_s * US_PER_S;
    g_fc.anchor.monotonic_us_at_sync = 10u * US_PER_S;
    g_fc.anchor.sync_status       = POOL_TIME_SYNC_STATUS_COMPLETED;
    g_fc.anchor.last_error        = TIME_OK;
}

/* A plausible committed epoch inside the B2 sanity band. */
#define EPOCH_2026 1767225600ull   /* 2026-01-01T00:00:00Z */

/* ------------------------------------------------------------------ */

TEST_CASE("W4 time: a zeroed view fails closed", "[weather_runtime]")
{
    WeatherTimeView v;
    memset(&v, 0, sizeof(v));
    TEST_ASSERT_EQUAL(WEATHER_TIME_INTERNAL_ERROR, v.state);
    TEST_ASSERT_FALSE(v.trusted_time_available);
    TEST_ASSERT_EQUAL_STRING("TIME_INTERNAL_ERROR", weather_time_state_str(v.state));
}

TEST_CASE("W4 time: no clock injected means PROVIDER_ABSENT, never a fallback",
          "[weather_runtime]")
{
    WeatherTimeView v;
    PoolTimeTrustPolicy p = policy();

    TEST_ASSERT_EQUAL(WEATHER_TIME_PROVIDER_ABSENT,
                      weather_time_view_read(NULL, &p,
                                             WEATHER_TIME_MAX_SYNC_AGE_S_DEFAULT, &v));
    TEST_ASSERT_FALSE(v.trusted_time_available);
    TEST_ASSERT_EQUAL(0u, v.trusted_utc_s);

    /* A policy without a clock, and a clock without a policy, both refuse. */
    PoolTimeClock c = clock_handle();
    TEST_ASSERT_EQUAL(WEATHER_TIME_PROVIDER_ABSENT,
                      weather_time_view_read(&c, NULL,
                                             WEATHER_TIME_MAX_SYNC_AGE_S_DEFAULT, &v));
    TEST_ASSERT_FALSE(v.trusted_time_available);
}

TEST_CASE("W4 time: an uninitialized provider is not trusted", "[weather_runtime]")
{
    WeatherTimeView v;
    PoolTimeTrustPolicy p = policy();
    PoolTimeClock c = clock_handle();

    memset(&g_fc, 0, sizeof(g_fc));
    g_fc.initialized = false;           /* read_anchor returns false */

    TEST_ASSERT_EQUAL(WEATHER_TIME_PROVIDER_UNINITIALIZED,
                      weather_time_view_read(&c, &p,
                                             WEATHER_TIME_MAX_SYNC_AGE_S_DEFAULT, &v));
    TEST_ASSERT_FALSE(v.trusted_time_available);
}

TEST_CASE("W4 time: pending and not-synced anchors are not trusted",
          "[weather_runtime]")
{
    WeatherTimeView v;
    PoolTimeTrustPolicy p = policy();
    PoolTimeClock c = clock_handle();

    memset(&g_fc, 0, sizeof(g_fc));
    g_fc.initialized        = true;
    g_fc.anchor.valid       = false;
    g_fc.anchor.sync_status = POOL_TIME_SYNC_STATUS_PENDING;
    TEST_ASSERT_EQUAL(WEATHER_TIME_SYNC_PENDING,
                      weather_time_view_read(&c, &p,
                                             WEATHER_TIME_MAX_SYNC_AGE_S_DEFAULT, &v));
    TEST_ASSERT_FALSE(v.trusted_time_available);

    g_fc.anchor.sync_status = POOL_TIME_SYNC_STATUS_NONE;
    TEST_ASSERT_EQUAL(WEATHER_TIME_NOT_SYNCED,
                      weather_time_view_read(&c, &p,
                                             WEATHER_TIME_MAX_SYNC_AGE_S_DEFAULT, &v));
    TEST_ASSERT_FALSE(v.trusted_time_available);
}

TEST_CASE("W4 time: a healthy anchor yields TRUSTED with the B2 epoch",
          "[weather_runtime]")
{
    WeatherTimeView v;
    PoolTimeTrustPolicy p = policy();
    PoolTimeClock c = clock_handle();

    fake_synced(EPOCH_2026, 30u);
    TEST_ASSERT_EQUAL(WEATHER_TIME_TRUSTED,
                      weather_time_view_read(&c, &p,
                                             WEATHER_TIME_MAX_SYNC_AGE_S_DEFAULT, &v));
    TEST_ASSERT_TRUE(v.trusted_time_available);
    TEST_ASSERT_TRUE(v.sync_age_valid);
    /* B2 advances the anchored epoch by the elapsed MONOTONIC time, so a
     * 30 s old anchor reads 30 s later. The view echoes that; it never
     * recomputes or freezes the epoch. */
    TEST_ASSERT_EQUAL_UINT64(EPOCH_2026 + 30ull, v.trusted_utc_s);
    TEST_ASSERT_EQUAL_UINT64(30u, v.sync_age_s);
    TEST_ASSERT_EQUAL_UINT32(7u, v.sync_generation);
    TEST_ASSERT_EQUAL(TIME_OK, v.provider_status);
    TEST_ASSERT_EQUAL_STRING("TIME_TRUSTED", weather_time_state_str(v.state));
}

TEST_CASE("W4 time: a too-old anchor is STALE even though B2 trusted it",
          "[weather_runtime]")
{
    WeatherTimeView v;
    PoolTimeTrustPolicy p = policy();
    PoolTimeClock c = clock_handle();
    PoolTimeSnapshot snap;

    fake_synced(EPOCH_2026, 100000u);   /* > the 86400 s default ceiling */

    /* B2 itself still trusts it: the ceiling is a W4 narrowing, not a B2 rule. */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_snapshot(&c, &p, &snap));
    TEST_ASSERT_TRUE(snap.trusted);

    TEST_ASSERT_EQUAL(WEATHER_TIME_STALE,
                      weather_time_view_read(&c, &p,
                                             WEATHER_TIME_MAX_SYNC_AGE_S_DEFAULT, &v));
    TEST_ASSERT_FALSE(v.trusted_time_available);
    TEST_ASSERT_FALSE(v.sync_age_valid);
    TEST_ASSERT_EQUAL_UINT64(0u, v.trusted_utc_s);   /* never published */
}

TEST_CASE("W4 time: the age ceiling boundary is exact", "[weather_runtime]")
{
    WeatherTimeView v;
    PoolTimeTrustPolicy p = policy();
    PoolTimeClock c = clock_handle();

    fake_synced(EPOCH_2026, 600u);
    TEST_ASSERT_EQUAL(WEATHER_TIME_TRUSTED,
                      weather_time_view_read(&c, &p, 600u, &v));   /* == */
    TEST_ASSERT_TRUE(v.trusted_time_available);

    TEST_ASSERT_EQUAL(WEATHER_TIME_STALE,
                      weather_time_view_read(&c, &p, 599u, &v));   /* >  */
    TEST_ASSERT_FALSE(v.trusted_time_available);
}

TEST_CASE("W4 time: an invalid age ceiling never widens trust", "[weather_runtime]")
{
    WeatherTimeView v;
    PoolTimeTrustPolicy p = policy();
    PoolTimeClock c = clock_handle();

    fake_synced(EPOCH_2026, 10u);
    TEST_ASSERT_FALSE(weather_time_max_sync_age_valid(0u));
    TEST_ASSERT_FALSE(weather_time_max_sync_age_valid(
        WEATHER_TIME_MAX_SYNC_AGE_S_LIMIT + 1u));
    TEST_ASSERT_TRUE(weather_time_max_sync_age_valid(
        WEATHER_TIME_MAX_SYNC_AGE_S_LIMIT));

    TEST_ASSERT_EQUAL(WEATHER_TIME_INTERNAL_ERROR,
                      weather_time_view_read(&c, &p, 0u, &v));
    TEST_ASSERT_FALSE(v.trusted_time_available);
    TEST_ASSERT_EQUAL(WEATHER_TIME_INTERNAL_ERROR,
                      weather_time_view_read(&c, &p,
                                             WEATHER_TIME_MAX_SYNC_AGE_S_LIMIT + 1u, &v));
    TEST_ASSERT_FALSE(v.trusted_time_available);
}

TEST_CASE("W4 time: a backwards epoch is refused by B2, not by weather",
          "[weather_runtime]")
{
    WeatherTimeView v;
    PoolTimeTrustPolicy p = policy();
    PoolTimeClock c = clock_handle();

    /* The B2 anti-regression floor is the existing, committed mechanism. */
    fake_synced(EPOCH_2026, 10u);
    p.required_min_epoch_s = EPOCH_2026 + 86400ull;

    TEST_ASSERT_EQUAL(WEATHER_TIME_REJECTED,
                      weather_time_view_read(&c, &p,
                                             WEATHER_TIME_MAX_SYNC_AGE_S_DEFAULT, &v));
    TEST_ASSERT_FALSE(v.trusted_time_available);
    /* The verdict is ECHOED from B2 — weather implements no floor of its own. */
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN, v.provider_status);
}

TEST_CASE("W4 time: an out-of-band epoch is refused by B2", "[weather_runtime]")
{
    WeatherTimeView v;
    PoolTimeTrustPolicy p = policy();
    PoolTimeClock c = clock_handle();

    fake_synced(1000ull, 10u);          /* far below the sanity floor */
    TEST_ASSERT_EQUAL(WEATHER_TIME_REJECTED,
                      weather_time_view_read(&c, &p,
                                             WEATHER_TIME_MAX_SYNC_AGE_S_DEFAULT, &v));
    TEST_ASSERT_FALSE(v.trusted_time_available);
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BELOW_MIN, v.provider_status);
}

/*
 * THE narrowing proof. Over a wide sweep of anchor states, epochs, ages and
 * ceilings, weather may never claim trust that Gate B2 did not grant.
 */
TEST_CASE("W4 time: PROPERTY trusted(weather) implies trusted(B2), always",
          "[weather_runtime]")
{
    static const uint64_t EPOCHS[] = {
        0ull, 1000ull, 1600000000ull, EPOCH_2026, EPOCH_2026 + 31536000ull,
        4102444800ull, 0xFFFFFFFFull,
    };
    static const uint64_t AGES[] = { 0ull, 1ull, 599ull, 600ull, 86399ull,
                                     86400ull, 86401ull, 1000000ull };
    static const uint32_t CEILINGS[] = { 1u, 600u, 86400u,
                                         WEATHER_TIME_MAX_SYNC_AGE_S_LIMIT };
    size_t e, a, k, s;
    int trusted_cases = 0;

    for (e = 0; e < sizeof(EPOCHS) / sizeof(EPOCHS[0]); e++) {
        for (a = 0; a < sizeof(AGES) / sizeof(AGES[0]); a++) {
            for (k = 0; k < sizeof(CEILINGS) / sizeof(CEILINGS[0]); k++) {
                for (s = 0; s < 3u; s++) {
                    WeatherTimeView v;
                    PoolTimeSnapshot snap;
                    PoolTimeTrustPolicy p = policy();
                    PoolTimeClock c = clock_handle();
                    PoolTimeError st;

                    fake_synced(EPOCHS[e], AGES[a]);
                    /* s: 0 = valid anchor, 1 = invalid+pending, 2 = uninit */
                    if (s == 1u) {
                        g_fc.anchor.valid = false;
                        g_fc.anchor.sync_status = POOL_TIME_SYNC_STATUS_PENDING;
                    } else if (s == 2u) {
                        g_fc.initialized = false;
                    }

                    st = pool_time_snapshot(&c, &p, &snap);
                    (void)weather_time_view_read(&c, &p, CEILINGS[k], &v);

                    if (v.trusted_time_available) {
                        trusted_cases++;
                        /* The implication. */
                        TEST_ASSERT_TRUE(snap.trusted);
                        TEST_ASSERT_EQUAL(TIME_OK, st);
                        /* And the epoch is B2's, never recomputed. */
                        TEST_ASSERT_EQUAL_UINT64(snap.trusted_epoch_s,
                                                 v.trusted_utc_s);
                        TEST_ASSERT_EQUAL(WEATHER_TIME_TRUSTED, v.state);
                    } else {
                        TEST_ASSERT_EQUAL_UINT64(0u, v.trusted_utc_s);
                        TEST_ASSERT_NOT_EQUAL(WEATHER_TIME_TRUSTED, v.state);
                    }
                }
            }
        }
    }
    /* Non-vacuity: the sweep really did produce trusted cases. */
    TEST_ASSERT_GREATER_THAN_INT(0, trusted_cases);
}

TEST_CASE("W4 time: the same snapshot yields a byte-identical view",
          "[weather_runtime]")
{
    WeatherTimeView a, b;
    PoolTimeTrustPolicy p = policy();
    PoolTimeClock c = clock_handle();

    fake_synced(EPOCH_2026, 42u);
    (void)weather_time_view_read(&c, &p, 86400u, &a);
    fake_synced(EPOCH_2026, 42u);
    (void)weather_time_view_read(&c, &p, 86400u, &b);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
}

TEST_CASE("W4 time: the projection reads the clock but starts no service",
          "[weather_runtime]")
{
    WeatherTimeView v;
    PoolTimeTrustPolicy p = policy();
    PoolTimeClock c = clock_handle();

    fake_synced(EPOCH_2026, 5u);
    g_fc.monotonic_calls = 0;
    g_fc.anchor_calls    = 0;
    (void)weather_time_view_read(&c, &p, 86400u, &v);

    /* Exactly the two committed B2 reads: no extra probing, no writer. */
    TEST_ASSERT_EQUAL_INT(1, g_fc.monotonic_calls);
    TEST_ASSERT_EQUAL_INT(1, g_fc.anchor_calls);
    /* The fake anchor is untouched: this path never writes an anchor. */
    TEST_ASSERT_TRUE(g_fc.anchor.valid);
    TEST_ASSERT_EQUAL_UINT32(7u, g_fc.anchor.generation);
}

TEST_CASE("W4 time: every state has a distinct bounded token", "[weather_runtime]")
{
    int i, j;
    for (i = 0; i < (int)WEATHER_TIME_STATE__COUNT; i++) {
        const char *a = weather_time_state_str((WeatherTimeState)i);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_TRUE(strlen(a) > 0u && strlen(a) < 40u);
        for (j = i + 1; j < (int)WEATHER_TIME_STATE__COUNT; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(a, weather_time_state_str(
                                                    (WeatherTimeState)j)));
        }
    }
    /* Out-of-range fails closed rather than reading past the table. */
    TEST_ASSERT_EQUAL_STRING("TIME_INTERNAL_ERROR",
                             weather_time_state_str((WeatherTimeState)999));
}
