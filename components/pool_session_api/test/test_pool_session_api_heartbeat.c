/*
 * Deterministic tests for the Gate B8 PURE TARGET_ACTIVE heartbeat
 * scheduler. No clock is read here: monotonic microseconds are an input.
 */

#include <string.h>
#include "unity.h"
#include "pool_session_api_heartbeat.h"
#include "pool_session_record.h"

#define EPOCH_BASE 1750000000ull
#define US_PER_S   1000000ull

static PoolApiHeartbeatState    g_hb;
static PoolApiHeartbeatInput    g_in;
static PoolApiHeartbeatDecision g_out;

static void fresh(void)
{
    pool_api_heartbeat_init(&g_hb);
    memset(&g_in, 0, sizeof(g_in));
    g_in.durable_target_active   = true;
    g_in.mining_grant_active     = true;
    g_in.recovery_guard          = false;
    g_in.trusted_time_valid      = true;
    g_in.trusted_epoch_s         = EPOCH_BASE;
    g_in.monotonic_us            = 10ull * US_PER_S;
    g_in.persisted_epoch_valid   = true;
    g_in.persisted_epoch_floor_s = EPOCH_BASE - 600ull;
}

static PoolApiHeartbeatStatus evaluate(void)
{
    return pool_api_heartbeat_evaluate(&g_hb, &g_in, &g_out);
}

TEST_CASE("hb: not applicable without a durable TARGET_ACTIVE session or a grant",
          "[pool_api_hb]")
{
    fresh();
    g_in.durable_target_active = false;
    TEST_ASSERT_EQUAL(API_HB_NOT_APPLICABLE, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    fresh();
    g_in.mining_grant_active = false;
    TEST_ASSERT_EQUAL(API_HB_NOT_APPLICABLE, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    /* NULL arguments never commit. */
    TEST_ASSERT_EQUAL(API_HB_NOT_APPLICABLE,
                      pool_api_heartbeat_evaluate(NULL, &g_in, &g_out));
    TEST_ASSERT_FALSE(g_out.commit_now);
    TEST_ASSERT_EQUAL(API_HB_NOT_APPLICABLE,
                      pool_api_heartbeat_evaluate(&g_hb, NULL, &g_out));
    TEST_ASSERT_FALSE(g_out.commit_now);
}

TEST_CASE("hb: a recovery guard and untrusted time never write", "[pool_api_hb]")
{
    fresh();
    g_in.recovery_guard = true;
    TEST_ASSERT_EQUAL(API_HB_RECOVERY_GUARD, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    fresh();
    g_in.trusted_time_valid = false;
    TEST_ASSERT_EQUAL(API_HB_TIME_UNTRUSTED, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    /* An epoch outside the committed B3 sanity band is not trusted time. */
    fresh();
    g_in.trusted_epoch_s = POOL_RECORD_EPOCH_MIN_S - 1ull;
    TEST_ASSERT_EQUAL(API_HB_TIME_UNTRUSTED, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    fresh();
    g_in.trusted_epoch_s = POOL_RECORD_EPOCH_MAX_S + 1ull;
    TEST_ASSERT_EQUAL(API_HB_TIME_UNTRUSTED, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);
}

TEST_CASE("hb: the first applicable evaluation only arms — it never writes",
          "[pool_api_hb]")
{
    fresh();
    TEST_ASSERT_FALSE(g_hb.armed);
    TEST_ASSERT_EQUAL(API_HB_WAITING, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    pool_api_heartbeat_arm(&g_hb, g_in.monotonic_us);
    TEST_ASSERT_TRUE(g_hb.armed);
    /* Immediately after arming the window is closed. */
    TEST_ASSERT_EQUAL(API_HB_WAITING, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);
}

TEST_CASE("hb: the cadence boundary is exact and monotonic", "[pool_api_hb]")
{
    fresh();
    pool_api_heartbeat_arm(&g_hb, 10ull * US_PER_S);

    /* One second before the boundary. */
    g_in.monotonic_us = (10ull + POOL_API_HEARTBEAT_PERIOD_S - 1ull) * US_PER_S;
    TEST_ASSERT_EQUAL(API_HB_WAITING, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    /* Exactly at the boundary. */
    g_in.monotonic_us = (10ull + POOL_API_HEARTBEAT_PERIOD_S) * US_PER_S;
    TEST_ASSERT_EQUAL(API_HB_DUE, evaluate());
    TEST_ASSERT_TRUE(g_out.commit_now);
    TEST_ASSERT_EQUAL_UINT64(g_in.trusted_epoch_s, g_out.epoch_s);

    /* Well after the boundary but still inside the durable-age bound. */
    g_in.monotonic_us = (10ull + 2ull * POOL_API_HEARTBEAT_PERIOD_S) * US_PER_S;
    TEST_ASSERT_EQUAL(API_HB_DUE, evaluate());
    TEST_ASSERT_TRUE(g_out.commit_now);

    /* A regressed monotonic clock never makes a heartbeat due. */
    g_in.monotonic_us = 1ull * US_PER_S;
    TEST_ASSERT_EQUAL(API_HB_WAITING, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);
    TEST_ASSERT_EQUAL_UINT64(0u, pool_api_heartbeat_elapsed_s(&g_hb, 1ull * US_PER_S));
}

TEST_CASE("hb: the trusted epoch must advance by the configured minimum",
          "[pool_api_hb]")
{
    fresh();
    pool_api_heartbeat_arm(&g_hb, 0u);
    g_in.monotonic_us = 2ull * POOL_API_HEARTBEAT_PERIOD_S * US_PER_S;

    /* Equal to the persisted floor: not an advance, never a write. */
    g_in.persisted_epoch_floor_s = g_in.trusted_epoch_s;
    TEST_ASSERT_EQUAL(API_HB_WAITING, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    /* Below the persisted floor: never a write (the floor never lowers). */
    g_in.persisted_epoch_floor_s = g_in.trusted_epoch_s + 100ull;
    TEST_ASSERT_EQUAL(API_HB_WAITING, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    /* One second short of the minimum advance. */
    g_in.persisted_epoch_floor_s =
        g_in.trusted_epoch_s - (POOL_API_HEARTBEAT_MIN_EPOCH_ADVANCE_S - 1ull);
    TEST_ASSERT_EQUAL(API_HB_WAITING, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    /* Exactly the minimum advance. */
    g_in.persisted_epoch_floor_s =
        g_in.trusted_epoch_s - POOL_API_HEARTBEAT_MIN_EPOCH_ADVANCE_S;
    TEST_ASSERT_EQUAL(API_HB_DUE, evaluate());
    TEST_ASSERT_TRUE(g_out.commit_now);

    /* No persisted floor at all: any in-band epoch is an advance. */
    g_in.persisted_epoch_valid = false;
    TEST_ASSERT_EQUAL(API_HB_DUE, evaluate());
    TEST_ASSERT_TRUE(g_out.commit_now);
}

TEST_CASE("hb: a duplicate tick after a commit does not commit again",
          "[pool_api_hb]")
{
    uint64_t t;

    fresh();
    pool_api_heartbeat_arm(&g_hb, 0u);
    t = POOL_API_HEARTBEAT_PERIOD_S * US_PER_S;
    g_in.monotonic_us = t;
    TEST_ASSERT_EQUAL(API_HB_DUE, evaluate());

    pool_api_heartbeat_record_commit(&g_hb, t, g_in.trusted_epoch_s);
    TEST_ASSERT_EQUAL_UINT32(1u, g_hb.commits);
    TEST_ASSERT_EQUAL(API_HB_COMMITTED, g_hb.status);

    /* The very same tick, and every tick before the next boundary, is a
     * no-op: the cadence AND the epoch advance both fail. */
    TEST_ASSERT_EQUAL(API_HB_WAITING, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);
    g_in.monotonic_us = t + (POOL_API_HEARTBEAT_PERIOD_S - 1ull) * US_PER_S;
    TEST_ASSERT_EQUAL(API_HB_WAITING, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    /* Next boundary WITHOUT a newer epoch: still nothing to write. */
    g_in.monotonic_us = t + POOL_API_HEARTBEAT_PERIOD_S * US_PER_S;
    TEST_ASSERT_EQUAL(API_HB_WAITING, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    /* Next boundary WITH a newer epoch: due exactly once. */
    g_in.trusted_epoch_s += POOL_API_HEARTBEAT_MIN_EPOCH_ADVANCE_S;
    TEST_ASSERT_EQUAL(API_HB_DUE, evaluate());
    TEST_ASSERT_TRUE(g_out.commit_now);
    TEST_ASSERT_EQUAL_UINT32(1u, g_hb.commits); /* evaluate() never counts */
}

TEST_CASE("hb: a durable epoch only ratchets upward", "[pool_api_hb]")
{
    fresh();
    pool_api_heartbeat_record_commit(&g_hb, 0u, EPOCH_BASE + 1000ull);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_BASE + 1000ull, g_hb.last_committed_epoch_s);

    /* An older value can never replace the durable one. */
    pool_api_heartbeat_record_commit(&g_hb, 0u, EPOCH_BASE);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_BASE + 1000ull, g_hb.last_committed_epoch_s);
    TEST_ASSERT_EQUAL_UINT32(2u, g_hb.commits);
}

TEST_CASE("hb: a failure re-arms but never claims durable liveness",
          "[pool_api_hb]")
{
    uint64_t t = POOL_API_HEARTBEAT_PERIOD_S * US_PER_S;

    fresh();
    pool_api_heartbeat_arm(&g_hb, 0u);
    g_in.monotonic_us = t;
    TEST_ASSERT_EQUAL(API_HB_DUE, evaluate());

    pool_api_heartbeat_record_failure(&g_hb, t, false);
    TEST_ASSERT_EQUAL(API_HB_PERSIST_FAILED, g_hb.status);
    TEST_ASSERT_FALSE(g_hb.have_committed_epoch);
    TEST_ASSERT_EQUAL_UINT32(0u, g_hb.commits);
    /* Re-armed: a failing store is never hammered on every tick. */
    TEST_ASSERT_EQUAL(API_HB_WAITING, evaluate());
    TEST_ASSERT_FALSE(g_out.commit_now);

    /* An uncertain outcome is the fail-closed recovery posture. */
    pool_api_heartbeat_record_failure(&g_hb, t, true);
    TEST_ASSERT_EQUAL(API_HB_RECOVERY_GUARD, g_hb.status);
    TEST_ASSERT_FALSE(g_hb.have_committed_epoch);
    TEST_ASSERT_EQUAL_UINT32(0u, g_hb.commits);
}

TEST_CASE("hb: identical inputs produce byte-identical decisions", "[pool_api_hb]")
{
    PoolApiHeartbeatDecision a, b;
    PoolApiHeartbeatState    s1, s2;

    fresh();
    pool_api_heartbeat_arm(&g_hb, 0u);
    g_in.monotonic_us = 2ull * POOL_API_HEARTBEAT_PERIOD_S * US_PER_S;
    s1 = g_hb;
    s2 = g_hb;

    (void)pool_api_heartbeat_evaluate(&s1, &g_in, &a);
    (void)pool_api_heartbeat_evaluate(&s2, &g_in, &b);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
    /* Evaluation is pure: it never mutates the scheduler state. */
    TEST_ASSERT_EQUAL_MEMORY(&s1, &s2, sizeof(s1));
    TEST_ASSERT_EQUAL_MEMORY(&g_hb, &s1, sizeof(s1));
}

TEST_CASE("hb: status tokens are stable and dot-free", "[pool_api_hb]")
{
    unsigned v;

    for (v = 0; v < (unsigned)POOL_API_HB__COUNT; v++) {
        const char *s = pool_api_heartbeat_str((PoolApiHeartbeatStatus)v);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_NULL(strchr(s, '.'));
        TEST_ASSERT_NULL(strchr(s, ' '));
    }
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", pool_api_heartbeat_str((PoolApiHeartbeatStatus)99));
}
