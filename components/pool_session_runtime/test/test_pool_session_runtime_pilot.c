/*
 * Deterministic tests for the Gate B10.1 observation-pilot diagnostics: the
 * pure invariant checker, the bounded event/summary step, the monotonic
 * 60-second rate limit, the bounded formatters and their privacy properties,
 * plus the adapter-level flag and observation contracts.
 *
 * NOTHING here contacts a network, an NTP server, DNS, a pool, OTA, a restart
 * path, physical NVS or hardware. Every clock is a fake monotonic counter,
 * every identity is a synthetic "*.example" fixture, and no test resolves,
 * formats or asserts a real hostname.
 */

#include <string.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pool_session_runtime.h"
#include "pool_session_runtime_admission.h"
#include "pool_session_runtime_pilot.h"

#define B101_SRC       "ntp-b101.example"
#define B101_LONG_LINE 512

static PoolPilotState        g_st;
static PoolPilotObservation  g_obs;
static PoolPilotStepResult   g_step;
static PoolPilotInvariantInput g_in;
static PoolPilotInvariantReport g_rep;
static char                  g_line[B101_LONG_LINE];

/* ================================================================= */
/* Helpers                                                            */
/* ================================================================= */

/* THE healthy observation-pilot posture: an empty store on a device that is
 * mining its normal source pool and owns nothing. */
static void b101_healthy_input(PoolPilotInvariantInput *in)
{
    memset(in, 0, sizeof(*in));
    in->snapshot_model_version      = POOL_RUNTIME_MODEL_VERSION;
    in->snapshot_structurally_valid = true;
    in->store_result                = STORE_EMPTY;
    in->session_present             = false;
    in->lease_owner                 = OP_OWNER_NONE;
    in->restore_required            = false;
    in->session_write_count         = 0u;
    in->heartbeat_write_count       = 0u;
    in->execution_compiled          = false;
    in->execution_hook_registered   = false;
    in->api_compiled                = false;
    in->api_hook_registered         = false;
    in->target_mining_authorized    = false;
    in->pool_mutation_permitted     = false;
    in->protocol                    = POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE;
    in->runtime_state               = RUNTIME_FREE;
    in->time_state                  = TIME_SOURCE_TRUSTED;
}

static void b101_fresh_obs(void)
{
    memset(&g_obs, 0, sizeof(g_obs));
    g_obs.network_ready  = false;
    g_obs.link_known     = false;
    g_obs.source_state   = TIME_SOURCE_UNCONFIGURED;
    g_obs.attempt_count  = 0u;
    g_obs.monotonic_us   = 0ull;
    g_obs.invariant_mask = 0u;
}

static bool b101_has_event(const PoolPilotStepResult *r, PoolPilotEvent e)
{
    uint32_t i;
    for (i = 0u; i < r->count; i++) {
        if (r->events[i] == e) {
            return true;
        }
    }
    return false;
}

/* Assert a formatted line carries no forbidden substring. The candidate
 * source below is the SAME synthetic fixture the adapter tests configure. */
static void b101_assert_private_free(const char *line)
{
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_NULL(strstr(line, B101_SRC));
    TEST_ASSERT_NULL(strstr(line, "example"));
    TEST_ASSERT_NULL(strstr(line, "."));      /* no hostname, no IPv4 literal */
    TEST_ASSERT_NULL(strstr(line, ":"));      /* no IPv6 literal, no port     */
    TEST_ASSERT_NULL(strstr(line, "@"));      /* no account or credential     */
    TEST_ASSERT_NULL(strstr(line, "1750000000")); /* no raw wall-clock epoch  */
    TEST_ASSERT_NULL(strstr(line, "stratum"));
    TEST_ASSERT_NULL(strstr(line, "pool"));
    TEST_ASSERT_NULL(strstr(line, "password"));
}

/* ================================================================= */
/* A. Diagnostics flag                                                */
/* ================================================================= */

TEST_CASE("b101 flag: the pilot link binding follows the compile-time flag",
          "[pool_runtime_b101]")
{
    PoolSessionRuntimeDeps deps;
    PoolStoreNvsBackend    backend;

    memset(&backend, 0, sizeof(backend));
    pool_session_runtime_production_deps(&deps, &backend);

#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS
    /* Only a pilot build reads a link fact at all. */
    TEST_ASSERT_NOT_NULL(deps.link_up);
#else
    /* The shipped default: no link binding exists, so no Wi-Fi call and no
     * link event can occur. */
    TEST_ASSERT_NULL(deps.link_up);
#endif
}

TEST_CASE("b101 flag: the pilot diagnostics flag can never imply authorization",
          "[pool_runtime_b101]")
{
    /* The flag is a diagnostic switch: even compiled in, the healthy posture
     * it validates forbids execution and API reachability outright. */
    b101_healthy_input(&g_in);
    g_in.execution_compiled = true;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_EXECUTION_REACHABLE, g_rep.first);

    b101_healthy_input(&g_in);
    g_in.api_compiled = true;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_API_REACHABLE, g_rep.first);
}

TEST_CASE("b101 flag: every event and invariant token is stable and dot-free",
          "[pool_runtime_b101]")
{
    int i;

    for (i = 0; i < (int)POOL_PILOT_EVENT__COUNT; i++) {
        const char *t = pool_pilot_event_token((PoolPilotEvent)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_TRUE(strlen(t) > 0u);
        TEST_ASSERT_NULL(strchr(t, '.'));
        TEST_ASSERT_NULL(strchr(t, ' '));
        TEST_ASSERT_NULL(strchr(t, '='));
    }
    for (i = 0; i < (int)POOL_PILOT_INV__COUNT; i++) {
        const char *t = pool_pilot_invariant_token((PoolPilotInvariant)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_TRUE(strlen(t) > 0u);
        TEST_ASSERT_NULL(strchr(t, '.'));
        TEST_ASSERT_NULL(strchr(t, ' '));
        TEST_ASSERT_NULL(strchr(t, '='));
    }
    /* Out-of-range values never resolve to a healthy or authorizing token. */
    TEST_ASSERT_EQUAL_STRING("PILOT_EVENT_UNKNOWN",
                             pool_pilot_event_token((PoolPilotEvent)9999));
    TEST_ASSERT_EQUAL_STRING("PILOT_INV_UNKNOWN",
                             pool_pilot_invariant_token((PoolPilotInvariant)9999));
}

TEST_CASE("b101 flag: the required pilot token vocabulary is exactly as specified",
          "[pool_runtime_b101]")
{
    TEST_ASSERT_EQUAL_STRING("PILOT_TIME_OBSERVE_BOOT",
        pool_pilot_event_token(POOL_PILOT_EVENT_TIME_OBSERVE_BOOT));
    TEST_ASSERT_EQUAL_STRING("PILOT_TIME_SOURCE_UNCONFIGURED",
        pool_pilot_event_token(POOL_PILOT_EVENT_TIME_SOURCE_UNCONFIGURED));
    TEST_ASSERT_EQUAL_STRING("PILOT_TIME_SOURCE_INVALID",
        pool_pilot_event_token(POOL_PILOT_EVENT_TIME_SOURCE_INVALID));
    TEST_ASSERT_EQUAL_STRING("PILOT_NETWORK_READY",
        pool_pilot_event_token(POOL_PILOT_EVENT_NETWORK_READY));
    TEST_ASSERT_EQUAL_STRING("PILOT_SNTP_START_ATTEMPT",
        pool_pilot_event_token(POOL_PILOT_EVENT_SNTP_START_ATTEMPT));
    TEST_ASSERT_EQUAL_STRING("PILOT_SNTP_SYNCING",
        pool_pilot_event_token(POOL_PILOT_EVENT_SNTP_SYNCING));
    TEST_ASSERT_EQUAL_STRING("PILOT_SNTP_TRUSTED",
        pool_pilot_event_token(POOL_PILOT_EVENT_SNTP_TRUSTED));
    TEST_ASSERT_EQUAL_STRING("PILOT_SNTP_REJECTED",
        pool_pilot_event_token(POOL_PILOT_EVENT_SNTP_REJECTED));
    TEST_ASSERT_EQUAL_STRING("PILOT_SNTP_TIMEOUT",
        pool_pilot_event_token(POOL_PILOT_EVENT_SNTP_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("PILOT_SNTP_ERROR",
        pool_pilot_event_token(POOL_PILOT_EVENT_SNTP_ERROR));
    TEST_ASSERT_EQUAL_STRING("PILOT_WIFI_LOST",
        pool_pilot_event_token(POOL_PILOT_EVENT_WIFI_LOST));
    TEST_ASSERT_EQUAL_STRING("PILOT_WIFI_READY",
        pool_pilot_event_token(POOL_PILOT_EVENT_WIFI_READY));
    TEST_ASSERT_EQUAL_STRING("PILOT_OBSERVATION_SUMMARY",
        pool_pilot_event_token(POOL_PILOT_EVENT_OBSERVATION_SUMMARY));
    TEST_ASSERT_EQUAL_STRING("PILOT_INVARIANT_VIOLATION",
        pool_pilot_event_token(POOL_PILOT_EVENT_INVARIANT_VIOLATION));
}

/* ================================================================= */
/* B. Diagnostic events                                               */
/* ================================================================= */

TEST_CASE("b101 evt: the first step announces boot and emits one summary",
          "[pool_runtime_b101]")
{
    pool_pilot_state_init(&g_st);
    b101_fresh_obs();

    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(b101_has_event(&g_step, POOL_PILOT_EVENT_TIME_OBSERVE_BOOT));
    TEST_ASSERT_TRUE(g_step.summary_due);
    TEST_ASSERT_EQUAL_UINT32(1u, g_step.sequence);

    /* Boot is a one-shot marker. */
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_TIME_OBSERVE_BOOT));
}

TEST_CASE("b101 evt: an unconfigured and an invalid source each announce once",
          "[pool_runtime_b101]")
{
    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    g_obs.source_state = TIME_SOURCE_UNCONFIGURED;

    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(b101_has_event(&g_step, POOL_PILOT_EVENT_TIME_SOURCE_UNCONFIGURED));
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_TIME_SOURCE_UNCONFIGURED));

    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    g_obs.source_state = TIME_SOURCE_INVALID;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(b101_has_event(&g_step, POOL_PILOT_EVENT_TIME_SOURCE_INVALID));
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_TIME_SOURCE_INVALID));
}

TEST_CASE("b101 evt: network readiness is announced exactly once",
          "[pool_runtime_b101]")
{
    int i;

    pool_pilot_state_init(&g_st);
    b101_fresh_obs();

    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_NETWORK_READY));

    g_obs.network_ready = true;
    g_obs.monotonic_us  = 1000000ull;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(b101_has_event(&g_step, POOL_PILOT_EVENT_NETWORK_READY));

    for (i = 0; i < 10; i++) {
        g_obs.monotonic_us += 1000000ull;
        pool_pilot_step(&g_st, &g_obs, &g_step);
        TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_NETWORK_READY));
    }
}

TEST_CASE("b101 evt: every SNTP lifecycle transition emits its own token",
          "[pool_runtime_b101]")
{
    const PoolTimeSourceState states[] = {
        TIME_SOURCE_START_PENDING, TIME_SOURCE_SYNCING, TIME_SOURCE_TRUSTED,
        TIME_SOURCE_REJECTED, TIME_SOURCE_TIMEOUT, TIME_SOURCE_ERROR,
    };
    const PoolPilotEvent expect[] = {
        POOL_PILOT_EVENT_SNTP_START_ATTEMPT, POOL_PILOT_EVENT_SNTP_SYNCING,
        POOL_PILOT_EVENT_SNTP_TRUSTED, POOL_PILOT_EVENT_SNTP_REJECTED,
        POOL_PILOT_EVENT_SNTP_TIMEOUT, POOL_PILOT_EVENT_SNTP_ERROR,
    };
    unsigned i;

    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    pool_pilot_step(&g_st, &g_obs, &g_step); /* consume boot + first summary */

    for (i = 0u; i < sizeof(states) / sizeof(states[0]); i++) {
        g_obs.monotonic_us += 1000000ull;
        g_obs.source_state = states[i];
        pool_pilot_step(&g_st, &g_obs, &g_step);
        TEST_ASSERT_TRUE(b101_has_event(&g_step, expect[i]));
        /* A steady state repeats nothing. */
        g_obs.monotonic_us += 1000000ull;
        pool_pilot_step(&g_st, &g_obs, &g_step);
        TEST_ASSERT_FALSE(b101_has_event(&g_step, expect[i]));
    }
}

TEST_CASE("b101 evt: a consumed start attempt emits exactly one token",
          "[pool_runtime_b101]")
{
    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    pool_pilot_step(&g_st, &g_obs, &g_step);

    g_obs.monotonic_us += 1000000ull;
    g_obs.attempt_count = 1u;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(b101_has_event(&g_step, POOL_PILOT_EVENT_SNTP_START_ATTEMPT));

    g_obs.monotonic_us += 1000000ull;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_SNTP_START_ATTEMPT));

    g_obs.monotonic_us += 1000000ull;
    g_obs.attempt_count = 2u;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(b101_has_event(&g_step, POOL_PILOT_EVENT_SNTP_START_ATTEMPT));
}

TEST_CASE("b101 evt: link loss and recovery are edge-triggered",
          "[pool_runtime_b101]")
{
    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    g_obs.link_known = true;
    g_obs.link_up    = true;

    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(b101_has_event(&g_step, POOL_PILOT_EVENT_WIFI_READY));

    g_obs.monotonic_us += 1000000ull;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_WIFI_READY));

    g_obs.monotonic_us += 1000000ull;
    g_obs.link_up = false;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(b101_has_event(&g_step, POOL_PILOT_EVENT_WIFI_LOST));
    TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_WIFI_READY));

    g_obs.monotonic_us += 1000000ull;
    g_obs.link_up = true;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(b101_has_event(&g_step, POOL_PILOT_EVENT_WIFI_READY));
}

TEST_CASE("b101 evt: an unavailable link fact never produces a guessed event",
          "[pool_runtime_b101]")
{
    int i;

    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    g_obs.link_known = false;
    g_obs.link_up    = false;

    for (i = 0; i < 20; i++) {
        g_obs.monotonic_us += 1000000ull;
        pool_pilot_step(&g_st, &g_obs, &g_step);
        TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_WIFI_LOST));
        TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_WIFI_READY));
    }
}

TEST_CASE("b101 evt: the summary is bounded to once per 60 monotonic seconds",
          "[pool_runtime_b101]")
{
    int      i;
    uint32_t summaries = 0u;

    pool_pilot_state_init(&g_st);
    b101_fresh_obs();

    /* 600 one-second ticks = 10 minutes of observation. */
    for (i = 0; i < 600; i++) {
        pool_pilot_step(&g_st, &g_obs, &g_step);
        if (g_step.summary_due) {
            summaries++;
        }
        g_obs.monotonic_us += 1000000ull;
    }
    /* One at t=0 plus one per completed minute inside the window. */
    TEST_ASSERT_EQUAL_UINT32(10u, summaries);
    TEST_ASSERT_EQUAL_UINT32(10u, g_st.sequence);
}

TEST_CASE("b101 evt: a steady state emits nothing at all between summaries",
          "[pool_runtime_b101]")
{
    int      i;
    uint32_t events = 0u;

    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    g_obs.link_known   = true;
    g_obs.link_up      = true;
    g_obs.network_ready = true;
    g_obs.source_state = TIME_SOURCE_TRUSTED;

    pool_pilot_step(&g_st, &g_obs, &g_step); /* the first burst */

    for (i = 0; i < 59; i++) {
        g_obs.monotonic_us += 1000000ull;
        pool_pilot_step(&g_st, &g_obs, &g_step);
        events += g_step.count;
        TEST_ASSERT_FALSE(g_step.summary_due);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, events); /* no log flood */
}

TEST_CASE("b101 evt: a monotonic regression never emits an early summary",
          "[pool_runtime_b101]")
{
    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    g_obs.monotonic_us = 100ull * 1000000ull;

    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(g_step.summary_due);

    /* The clock jumps BACKWARDS: the window re-anchors, it does not fire. */
    g_obs.monotonic_us = 10ull * 1000000ull;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_FALSE(g_step.summary_due);

    /* 59 s after the new anchor: still not due. */
    g_obs.monotonic_us = 69ull * 1000000ull;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_FALSE(g_step.summary_due);

    /* A full period after the new anchor: due. */
    g_obs.monotonic_us = 70ull * 1000000ull;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(g_step.summary_due);
}

TEST_CASE("b101 evt: one step never exceeds the bounded event capacity",
          "[pool_runtime_b101]")
{
    int i;

    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    /* The busiest possible first tick. */
    g_obs.network_ready  = true;
    g_obs.link_known     = true;
    g_obs.link_up        = true;
    g_obs.attempt_count  = 1u;
    g_obs.source_state   = TIME_SOURCE_TRUSTED;
    g_obs.invariant_mask = 0xFFFFFFFFu;

    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(g_step.count <= POOL_PILOT_EVENTS_MAX);
    TEST_ASSERT_TRUE(g_step.count >= 5u);

    for (i = 0; i < 200; i++) {
        g_obs.monotonic_us += 1000000ull;
        g_obs.link_up       = ((i % 2) == 0);
        g_obs.source_state  = (PoolTimeSourceState)(i % (int)POOL_TIME_SOURCE_STATE__COUNT);
        pool_pilot_step(&g_st, &g_obs, &g_step);
        TEST_ASSERT_TRUE(g_step.count <= POOL_PILOT_EVENTS_MAX);
    }
}

TEST_CASE("b101 evt: an unchanging violation set is not re-logged every tick",
          "[pool_runtime_b101]")
{
    int      i;
    uint32_t violations = 0u;

    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    g_obs.invariant_mask = 1u << (uint32_t)POOL_PILOT_INV_OWNER_PRESENT;

    for (i = 0; i < 120; i++) {
        pool_pilot_step(&g_st, &g_obs, &g_step);
        if (b101_has_event(&g_step, POOL_PILOT_EVENT_INVARIANT_VIOLATION)) {
            violations++;
        }
        g_obs.monotonic_us += 1000000ull;
    }
    /* First occurrence plus one per bounded summary — never 120. */
    TEST_ASSERT_EQUAL_UINT32(2u, violations);
}

TEST_CASE("b101 evt: a changed violation set is reported immediately",
          "[pool_runtime_b101]")
{
    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    g_obs.invariant_mask = 1u << (uint32_t)POOL_PILOT_INV_OWNER_PRESENT;

    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(b101_has_event(&g_step, POOL_PILOT_EVENT_INVARIANT_VIOLATION));

    g_obs.monotonic_us += 1000000ull;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_INVARIANT_VIOLATION));

    g_obs.monotonic_us += 1000000ull;
    g_obs.invariant_mask |= 1u << (uint32_t)POOL_PILOT_INV_SESSION_WRITE;
    pool_pilot_step(&g_st, &g_obs, &g_step);
    TEST_ASSERT_TRUE(b101_has_event(&g_step, POOL_PILOT_EVENT_INVARIANT_VIOLATION));
}

TEST_CASE("b101 evt: a healthy posture never emits a violation",
          "[pool_runtime_b101]")
{
    int i;

    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    for (i = 0; i < 200; i++) {
        pool_pilot_step(&g_st, &g_obs, &g_step);
        TEST_ASSERT_FALSE(b101_has_event(&g_step, POOL_PILOT_EVENT_INVARIANT_VIOLATION));
        g_obs.monotonic_us += 1000000ull;
    }
}

TEST_CASE("b101 evt: NULL arguments are an inert no-op", "[pool_runtime_b101]")
{
    pool_pilot_state_init(&g_st);
    b101_fresh_obs();

    memset(&g_step, 0xAA, sizeof(g_step));
    pool_pilot_step(&g_st, NULL, &g_step);
    TEST_ASSERT_EQUAL_UINT32(0u, g_step.count);
    TEST_ASSERT_FALSE(g_step.summary_due);

    memset(&g_step, 0xAA, sizeof(g_step));
    pool_pilot_step(NULL, &g_obs, &g_step);
    TEST_ASSERT_EQUAL_UINT32(0u, g_step.count);
    TEST_ASSERT_FALSE(g_step.summary_due);

    pool_pilot_step(&g_st, &g_obs, NULL); /* must not crash */
    pool_pilot_state_init(NULL);          /* must not crash */
}

/* ================================================================= */
/* C. Invariant checker                                               */
/* ================================================================= */

TEST_CASE("b101 inv: the healthy EMPTY posture reports no violation",
          "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_TRUE(g_rep.healthy);
    TEST_ASSERT_EQUAL_UINT32(0u, g_rep.mask);
    TEST_ASSERT_EQUAL_UINT32(0u, g_rep.count);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_OK, g_rep.first);
}

TEST_CASE("b101 inv: the healthy CLEARED posture reports no violation",
          "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.store_result = STORE_CLEARED;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_TRUE(g_rep.healthy);
}

TEST_CASE("b101 inv: an unexpected store outcome is a violation",
          "[pool_runtime_b101]")
{
    const PoolStoreResult bad[] = {
        STORE_OK, STORE_NOT_INITIALIZED, STORE_IO_ERROR, STORE_CORRUPT,
        STORE_RECOVERY_REQUIRED, STORE_COMMIT_UNCERTAIN,
    };
    unsigned i;

    for (i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        b101_healthy_input(&g_in);
        g_in.store_result = bad[i];
        pool_pilot_invariants_check(&g_in, &g_rep);
        TEST_ASSERT_FALSE(g_rep.healthy);
        TEST_ASSERT_EQUAL(POOL_PILOT_INV_STORE_NOT_EMPTY, g_rep.first);
    }
}

TEST_CASE("b101 inv: an unexpected session record is a violation",
          "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.session_present = true;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_SESSION_RECORD_PRESENT, g_rep.first);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rep.count);
}

TEST_CASE("b101 inv: any mutating owner is a violation", "[pool_runtime_b101]")
{
    int i;

    for (i = 1; i < (int)OP_OWNER__COUNT; i++) {
        b101_healthy_input(&g_in);
        g_in.lease_owner = (PoolOperationOwner)i;
        pool_pilot_invariants_check(&g_in, &g_rep);
        TEST_ASSERT_FALSE(g_rep.healthy);
        TEST_ASSERT_EQUAL(POOL_PILOT_INV_OWNER_PRESENT, g_rep.first);
    }
}

TEST_CASE("b101 inv: a restore obligation is a violation", "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.restore_required = true;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_RESTORE_REQUIRED, g_rep.first);
}

TEST_CASE("b101 inv: any session write is a violation", "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.session_write_count = 1u;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_SESSION_WRITE, g_rep.first);
}

TEST_CASE("b101 inv: any heartbeat write is a violation", "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.heartbeat_write_count = 1u;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_HEARTBEAT_WRITE, g_rep.first);
}

TEST_CASE("b101 inv: a reachable executor is a violation", "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.execution_hook_registered = true;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_EXECUTION_REACHABLE, g_rep.first);
}

TEST_CASE("b101 inv: a reachable API mailbox is a violation", "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.api_hook_registered = true;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_API_REACHABLE, g_rep.first);
}

TEST_CASE("b101 inv: a target mining grant is a violation", "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.target_mining_authorized = true;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_MINING_GRANT, g_rep.first);
}

TEST_CASE("b101 inv: a pool-mutation permission is a violation", "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.pool_mutation_permitted = true;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_POOL_MUTATION, g_rep.first);
}

TEST_CASE("b101 inv: a protocol hold on an empty store is a violation",
          "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.protocol = POOL_RUNTIME_PROTOCOL_HOLD;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_PROTOCOL_HELD, g_rep.first);
}

TEST_CASE("b101 inv: any non-FREE runtime posture is a violation",
          "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.runtime_state = RUNTIME_RECOVERY_GUARD;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_RUNTIME_NOT_FREE, g_rep.first);
}

TEST_CASE("b101 inv: an out-of-range enum fails closed", "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.time_state = (PoolTimeSourceState)77;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_ENUM_OUT_OF_RANGE, g_rep.first);

    b101_healthy_input(&g_in);
    g_in.lease_owner = (PoolOperationOwner)99;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    /* An unknown owner is ALSO not OP_OWNER_NONE, so it fails the semantic
     * check first — the important property is that it is never healthy. */
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_OWNER_PRESENT, g_rep.first);
    TEST_ASSERT_TRUE((g_rep.mask & (1u << (uint32_t)POOL_PILOT_INV_ENUM_OUT_OF_RANGE)) != 0u);

    b101_healthy_input(&g_in);
    g_in.store_result = (PoolStoreResult)123;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_TRUE((g_rep.mask & (1u << (uint32_t)POOL_PILOT_INV_ENUM_OUT_OF_RANGE)) != 0u);
}

TEST_CASE("b101 inv: an invalid snapshot fails closed", "[pool_runtime_b101]")
{
    b101_healthy_input(&g_in);
    g_in.snapshot_structurally_valid = false;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_SNAPSHOT_INVALID, g_rep.first);

    b101_healthy_input(&g_in);
    g_in.snapshot_model_version = POOL_RUNTIME_MODEL_VERSION + 1u;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_SNAPSHOT_INVALID, g_rep.first);
}

TEST_CASE("b101 inv: a NULL input fails closed", "[pool_runtime_b101]")
{
    pool_pilot_invariants_check(NULL, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_SNAPSHOT_INVALID, g_rep.first);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rep.count);

    pool_pilot_invariants_check(&g_in, NULL); /* must not crash */
}

TEST_CASE("b101 inv: the verdict is deterministic and order-stable",
          "[pool_runtime_b101]")
{
    PoolPilotInvariantReport a, b;
    int i;

    b101_healthy_input(&g_in);
    g_in.session_present     = true;
    g_in.lease_owner         = OP_OWNER_TIMED_SESSION;
    g_in.restore_required    = true;
    g_in.session_write_count = 3u;

    for (i = 0; i < 10; i++) {
        pool_pilot_invariants_check(&g_in, &a);
        pool_pilot_invariants_check(&g_in, &b);
        TEST_ASSERT_EQUAL_UINT32(a.mask, b.mask);
        TEST_ASSERT_EQUAL_UINT32(a.count, b.count);
        TEST_ASSERT_EQUAL(a.first, b.first);
    }
    /* SESSION_RECORD_PRESENT is the lowest violated code here. */
    TEST_ASSERT_EQUAL(POOL_PILOT_INV_SESSION_RECORD_PRESENT, a.first);
    TEST_ASSERT_EQUAL_UINT32(4u, a.count);
}

TEST_CASE("b101 inv: the checker never mutates its input", "[pool_runtime_b101]")
{
    PoolPilotInvariantInput before;

    b101_healthy_input(&g_in);
    g_in.session_present = true;
    before = g_in;

    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_EQUAL_INT(0, memcmp(&before, &g_in, sizeof(g_in)));
}

/* ================================================================= */
/* D/E. Formatting, memory fields and privacy                         */
/* ================================================================= */

static void b101_fill_summary(PoolPilotSummary *s)
{
    memset(s, 0, sizeof(*s));
    s->sequence                 = 4294967295u;
    s->uptime_s                 = 4294967295u;
    s->source_state             = TIME_SOURCE_START_PENDING; /* longest token */
    s->trusted_available        = true;
    s->trusted_operational      = true;
    s->attempt_count            = 4294967295u;
    s->sync_age_valid           = true;
    s->sync_age_s               = 4294967295u;
    s->protocol                 = POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE;
    s->runtime_state            = RUNTIME_WAITING_FOR_TRUSTED_TIME; /* longest */
    s->lease_owner              = OP_OWNER_SESSION_ACKNOWLEDGE;     /* longest */
    s->restore_required         = true;
    s->session_write_count      = 4294967295u;
    s->heartbeat_write_count    = 4294967295u;
    s->execution_reachable      = true;
    s->api_reachable            = true;
    s->free_internal_heap_b     = 4294967295u;
    s->min_free_internal_heap_b = 4294967295u;
    s->owner_task_stack_hwm     = 4294967295u;
    /* The widest possible verdict: the longest invariant token and a count
     * saturated far beyond anything the checker can actually produce. */
    s->invariants.healthy = false;
    s->invariants.mask    = 4294967295u;
    s->invariants.count   = 4294967295u;
    s->invariants.first   = POOL_PILOT_INV_SESSION_RECORD_PRESENT;
}

TEST_CASE("b101 fmt: the worst-case summary fits the bounded buffer",
          "[pool_runtime_b101]")
{
    PoolPilotSummary s;
    char             buf[POOL_PILOT_SUMMARY_MAX];
    uint32_t         n;

    b101_fill_summary(&s); /* every field at its widest, longest tokens */

    n = pool_pilot_summary_format(&s, buf, (uint32_t)sizeof(buf));
    TEST_ASSERT_TRUE(n > 0u);
    TEST_ASSERT_TRUE(n < POOL_PILOT_SUMMARY_MAX);
    TEST_ASSERT_EQUAL_UINT32(n, (uint32_t)strlen(buf));
}

TEST_CASE("b101 fmt: a too-small buffer yields an empty line, never a truncated one",
          "[pool_runtime_b101]")
{
    PoolPilotSummary s;
    char             small[16];

    b101_fill_summary(&s);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_pilot_summary_format(&s, small, (uint32_t)sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);

    TEST_ASSERT_EQUAL_UINT32(0u, pool_pilot_summary_format(&s, NULL, 64u));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_pilot_summary_format(NULL, small, (uint32_t)sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
}

TEST_CASE("b101 fmt: the summary carries every required bounded field",
          "[pool_runtime_b101]")
{
    PoolPilotSummary s;

    memset(&s, 0, sizeof(s));
    s.sequence                 = 7u;
    s.uptime_s                 = 725u;
    s.source_state             = TIME_SOURCE_TRUSTED;
    s.trusted_available        = true;
    s.trusted_operational      = true;
    s.attempt_count            = 1u;
    s.sync_age_valid           = true;
    s.sync_age_s               = 302u;
    s.protocol                 = POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE;
    s.runtime_state            = RUNTIME_FREE;
    s.lease_owner              = OP_OWNER_NONE;
    s.free_internal_heap_b     = 145328u;
    s.min_free_internal_heap_b = 138992u;
    s.owner_task_stack_hwm     = 4720u;
    b101_healthy_input(&g_in);
    pool_pilot_invariants_check(&g_in, &s.invariants);

    TEST_ASSERT_TRUE(pool_pilot_summary_format(&s, g_line, (uint32_t)sizeof(g_line)) > 0u);

    TEST_ASSERT_NOT_NULL(strstr(g_line, "PILOT_OBSERVATION_SUMMARY"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "seq=7"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "up=725"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "src=TIME_SOURCE_TRUSTED"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "avail=1"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "oper=1"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "att=1"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "agev=1"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "age=302"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "proto=protocol_allow_source"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "rt=runtime_free"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "owner=OWNER_NONE"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "restore=0"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "b3w=0"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "hbw=0"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "exec=0"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "api=0"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "heap=145328"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "heapmin=138992"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "hwm=4720"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "inv=PILOT_INV_OK"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "invn=0"));
}

TEST_CASE("b101 fmt: memory fields never wrap or go negative",
          "[pool_runtime_b101]")
{
    PoolPilotSummary s;

    memset(&s, 0, sizeof(s));
    s.free_internal_heap_b     = 0u;
    s.min_free_internal_heap_b = 0u;
    s.owner_task_stack_hwm     = 0u;
    TEST_ASSERT_TRUE(pool_pilot_summary_format(&s, g_line, (uint32_t)sizeof(g_line)) > 0u);
    TEST_ASSERT_NOT_NULL(strstr(g_line, "heap=0"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "heapmin=0"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "hwm=0"));
    TEST_ASSERT_NULL(strstr(g_line, "-1"));

    b101_fill_summary(&s);
    TEST_ASSERT_TRUE(pool_pilot_summary_format(&s, g_line, (uint32_t)sizeof(g_line)) > 0u);
    TEST_ASSERT_NOT_NULL(strstr(g_line, "heap=4294967295"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "heapmin=4294967295"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "hwm=4294967295"));
    TEST_ASSERT_NULL(strstr(g_line, "-"));
}

TEST_CASE("b101 fmt: the violation line names the first code and the full mask",
          "[pool_runtime_b101]")
{
    char buf[POOL_PILOT_VIOLATION_MAX];

    b101_healthy_input(&g_in);
    g_in.lease_owner         = OP_OWNER_TIMED_SESSION;
    g_in.session_write_count = 2u;
    pool_pilot_invariants_check(&g_in, &g_rep);

    TEST_ASSERT_TRUE(pool_pilot_violation_format(9u, 61u, &g_rep, buf, (uint32_t)sizeof(buf)) > 0u);
    TEST_ASSERT_NOT_NULL(strstr(buf, "PILOT_INVARIANT_VIOLATION"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "seq=9"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "up=61"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "first=PILOT_INV_OWNER_PRESENT"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "n=2"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "mask=0x"));

    TEST_ASSERT_EQUAL_UINT32(0u, pool_pilot_violation_format(0u, 0u, NULL, buf,
                                                             (uint32_t)sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_pilot_violation_format(0u, 0u, &g_rep, buf, 8u));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

TEST_CASE("b101 fmt: every event line fits its bounded buffer",
          "[pool_runtime_b101]")
{
    char buf[POOL_PILOT_EVENT_MAX];
    int  i;

    for (i = 0; i < (int)POOL_PILOT_EVENT__COUNT; i++) {
        uint32_t n = pool_pilot_event_format((PoolPilotEvent)i, 4294967295u, buf,
                                             (uint32_t)sizeof(buf));
        TEST_ASSERT_TRUE(n > 0u);
        TEST_ASSERT_EQUAL_UINT32(n, (uint32_t)strlen(buf));
        TEST_ASSERT_NOT_NULL(strstr(buf, "up=4294967295"));
    }
    TEST_ASSERT_EQUAL_UINT32(0u, pool_pilot_event_format(POOL_PILOT_EVENT_NONE, 0u, buf, 4u));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

/* ================================================================= */
/* G. Property tests                                                  */
/* ================================================================= */

TEST_CASE("b101 prop: no summary line can carry a hostname, identity or epoch",
          "[pool_runtime_b101]")
{
    PoolPilotSummary s;
    int              state, owner;

    /* Sweep the whole cross product of published enums; there is no string
     * input anywhere, so no identity can enter a line by construction. */
    for (state = 0; state < (int)POOL_TIME_SOURCE_STATE__COUNT; state++) {
        for (owner = 0; owner < (int)OP_OWNER__COUNT; owner++) {
            b101_fill_summary(&s);
            s.source_state = (PoolTimeSourceState)state;
            s.lease_owner  = (PoolOperationOwner)owner;
            s.sync_age_s   = 1750000000u; /* a value that LOOKS like an epoch */
            TEST_ASSERT_TRUE(pool_pilot_summary_format(&s, g_line,
                                                       (uint32_t)sizeof(g_line)) > 0u);
            TEST_ASSERT_NULL(strstr(g_line, B101_SRC));
            TEST_ASSERT_NULL(strstr(g_line, "example"));
            TEST_ASSERT_NULL(strchr(g_line, '.'));
            TEST_ASSERT_NULL(strchr(g_line, ':'));
            TEST_ASSERT_NULL(strchr(g_line, '@'));
        }
    }
}

TEST_CASE("b101 prop: no violation or event line can carry an identity",
          "[pool_runtime_b101]")
{
    char buf[POOL_PILOT_VIOLATION_MAX];
    int  i;

    for (i = 0; i < (int)POOL_PILOT_INV__COUNT; i++) {
        g_rep.healthy = false;
        g_rep.mask    = 1u << (uint32_t)i;
        g_rep.count   = 1u;
        g_rep.first   = (PoolPilotInvariant)i;
        TEST_ASSERT_TRUE(pool_pilot_violation_format(1u, 1u, &g_rep, buf,
                                                     (uint32_t)sizeof(buf)) > 0u);
        b101_assert_private_free(buf);
    }
    for (i = 0; i < (int)POOL_PILOT_EVENT__COUNT; i++) {
        char ebuf[POOL_PILOT_EVENT_MAX];
        TEST_ASSERT_TRUE(pool_pilot_event_format((PoolPilotEvent)i, 42u, ebuf,
                                                 (uint32_t)sizeof(ebuf)) > 0u);
        b101_assert_private_free(ebuf);
    }
}

TEST_CASE("b101 prop: diagnostics never mutate any runtime state",
          "[pool_runtime_b101]")
{
    PoolPilotObservation before;
    int                  i;

    pool_pilot_state_init(&g_st);
    b101_fresh_obs();
    g_obs.network_ready = true;
    g_obs.link_known    = true;
    g_obs.link_up       = true;

    for (i = 0; i < 50; i++) {
        before = g_obs;
        pool_pilot_step(&g_st, &g_obs, &g_step);
        /* The observation the adapter gathered is never written back. */
        TEST_ASSERT_EQUAL_INT(0, memcmp(&before, &g_obs, sizeof(g_obs)));
        g_obs.monotonic_us += 1000000ull;
    }
}

TEST_CASE("b101 prop: a diagnostic failure never creates or implies a session",
          "[pool_runtime_b101]")
{
    int i;

    /* Every single-code violation, and the all-codes case, must still leave
     * the report a pure statement — no field of it can authorize anything. */
    for (i = 0; i < (int)POOL_PILOT_INV__COUNT; i++) {
        pool_pilot_state_init(&g_st);
        b101_fresh_obs();
        g_obs.invariant_mask = 1u << (uint32_t)i;
        pool_pilot_step(&g_st, &g_obs, &g_step);
        TEST_ASSERT_TRUE(g_step.count <= POOL_PILOT_EVENTS_MAX);
        /* The step reports; it has no field through which it could act. */
        TEST_ASSERT_TRUE(g_step.summary_due);
    }

    /* And the checker itself never turns an empty store into a session. */
    b101_healthy_input(&g_in);
    g_in.snapshot_structurally_valid = false;
    pool_pilot_invariants_check(&g_in, &g_rep);
    TEST_ASSERT_FALSE(g_rep.healthy);
    TEST_ASSERT_EQUAL(STORE_EMPTY, g_in.store_result);
    TEST_ASSERT_FALSE(g_in.session_present);
}

TEST_CASE("b101 prop: observation mode leaves protocol permission untouched",
          "[pool_runtime_b101]")
{
    const PoolStoreResult ok[] = { STORE_EMPTY, STORE_CLEARED };
    const PoolTimeSourceState every[] = {
        TIME_SOURCE_UNCONFIGURED, TIME_SOURCE_CONFIGURED, TIME_SOURCE_INVALID,
        TIME_SOURCE_START_PENDING, TIME_SOURCE_SYNCING, TIME_SOURCE_TRUSTED,
        TIME_SOURCE_REJECTED, TIME_SOURCE_TIMEOUT, TIME_SOURCE_STOPPED,
        TIME_SOURCE_ERROR,
    };
    unsigned a, b;

    /* Whatever the trusted-time outcome is, an EMPTY/CLEARED store with
     * ALLOW_SOURCE stays healthy: a time failure is never a mining failure. */
    for (a = 0u; a < sizeof(ok) / sizeof(ok[0]); a++) {
        for (b = 0u; b < sizeof(every) / sizeof(every[0]); b++) {
            b101_healthy_input(&g_in);
            g_in.store_result = ok[a];
            g_in.time_state   = every[b];
            pool_pilot_invariants_check(&g_in, &g_rep);
            TEST_ASSERT_TRUE(g_rep.healthy);
        }
    }
}

/* ================================================================= */
/* Adapter-level observation contract                                 */
/* ================================================================= */

/*
 * These reuse the committed Gate B10 adapter with the SAME fake platform ops
 * pattern: no network, no NVS hardware, no pool, no restart. They prove the
 * pilot tick observes without changing anything, on whichever build runs.
 */

#define B101_FK_SLOTS 3
#define B101_FK_MAX   512

typedef struct {
    bool    present;
    size_t  len;
    uint8_t bytes[B101_FK_MAX];
} B101Blob;

typedef struct {
    bool     opened;
    B101Blob committed[B101_FK_SLOTS];
    B101Blob staged[B101_FK_SLOTS];
    bool     staged_dirty[B101_FK_SLOTS];
    int      writes, commits;
} B101FakeNvs;

static B101FakeNvs g_b101_nvs;

static int b101_index(const char *key)
{
    if (key == NULL) return -1;
    if (strcmp(key, POOL_STORE_KEY_SLOT_A) == 0) return 0;
    if (strcmp(key, POOL_STORE_KEY_SLOT_B) == 0) return 1;
    if (strcmp(key, POOL_STORE_KEY_ACTIVE) == 0) return 2;
    return -1;
}

static int b101_open(void *ctx)
{
    (void)ctx;
    g_b101_nvs.opened = true;
    return POOL_STORE_BACKEND_OK;
}

static int b101_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len)
{
    int             idx = b101_index(key);
    const B101Blob *v;

    (void)ctx;
    if (!g_b101_nvs.opened || idx < 0 || out_len == NULL) return POOL_STORE_BACKEND_IO;
    v = g_b101_nvs.staged_dirty[idx] ? &g_b101_nvs.staged[idx] : &g_b101_nvs.committed[idx];
    if (!v->present) return POOL_STORE_BACKEND_NOT_FOUND;
    *out_len = v->len;
    if (v->len > cap) return POOL_STORE_BACKEND_OK;
    memcpy(buf, v->bytes, v->len);
    return POOL_STORE_BACKEND_OK;
}

static int b101_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    int idx = b101_index(key);

    (void)ctx;
    g_b101_nvs.writes++;
    if (!g_b101_nvs.opened || idx < 0 || buf == NULL || len == 0 || len > B101_FK_MAX) {
        return POOL_STORE_BACKEND_IO;
    }
    g_b101_nvs.staged[idx].present = true;
    g_b101_nvs.staged[idx].len     = len;
    memcpy(g_b101_nvs.staged[idx].bytes, buf, len);
    g_b101_nvs.staged_dirty[idx] = true;
    return POOL_STORE_BACKEND_OK;
}

static int b101_commit(void *ctx)
{
    int i;
    (void)ctx;
    g_b101_nvs.commits++;
    if (!g_b101_nvs.opened) return POOL_STORE_BACKEND_IO;
    for (i = 0; i < B101_FK_SLOTS; i++) {
        if (g_b101_nvs.staged_dirty[i]) {
            g_b101_nvs.committed[i]    = g_b101_nvs.staged[i];
            g_b101_nvs.staged_dirty[i] = false;
        }
    }
    return POOL_STORE_BACKEND_OK;
}

static int b101_close(void *ctx)
{
    (void)ctx;
    g_b101_nvs.opened = false;
    return POOL_STORE_BACKEND_OK;
}

static const PoolStoreBackendOps g_b101_store_ops = {
    .open = b101_open, .read_blob = b101_read, .write_blob = b101_write,
    .commit = b101_commit, .close = b101_close,
};

static uint64_t g_b101_mono_us;
static int      g_b101_inits, g_b101_starts;
static bool     g_b101_link_up;
static int      g_b101_link_reads;

static int32_t  b101_reset_reason(void) { return POOL_RESET_RAW_POWERON; }
static uint64_t b101_monotonic(void)    { return g_b101_mono_us; }
static bool     b101_link_up(void)      { g_b101_link_reads++; return g_b101_link_up; }

static int b101_sntp_init(const PoolTimeSntpConfig *cfg) { (void)cfg; g_b101_inits++; return 0; }
static int b101_sntp_start(void)  { g_b101_starts++; return 0; }
static int b101_sntp_stop(void)   { return 0; }
static int b101_sntp_deinit(void) { return 0; }

static const PoolTimeSntpPlatformOps g_b101_sntp_ops = {
    .monotonic_us = b101_monotonic, .sntp_init = b101_sntp_init,
    .sntp_start = b101_sntp_start, .sntp_stop = b101_sntp_stop,
    .sntp_deinit = b101_sntp_deinit,
};

static PoolSessionRuntime     g_b101_rt;
static PoolSessionRuntimeDeps g_b101_deps;
static PoolRuntimeSnapshot    g_b101_snap;

static void b101_rt_fresh(bool with_link)
{
    memset(&g_b101_nvs, 0, sizeof(g_b101_nvs));
    g_b101_mono_us    = 5ull * 1000000ull;
    g_b101_inits      = 0;
    g_b101_starts     = 0;
    g_b101_link_up    = true;
    g_b101_link_reads = 0;

    memset(&g_b101_deps, 0, sizeof(g_b101_deps));
    g_b101_deps.store_ops             = &g_b101_store_ops;
    g_b101_deps.sntp_ops              = &g_b101_sntp_ops;
    g_b101_deps.read_reset_reason_raw = b101_reset_reason;
    g_b101_deps.monotonic_us          = b101_monotonic;
    g_b101_deps.ntp_server            = B101_SRC;
    g_b101_deps.sync_wait_limit_s     = 600u;
    g_b101_deps.observe_enabled       = true;
    g_b101_deps.link_up               = with_link ? b101_link_up : NULL;

    memset(&g_b101_rt, 0, sizeof(g_b101_rt));
}

TEST_CASE("b101 rt: the pilot tick observes without changing anything",
          "[pool_runtime_b101]")
{
    int i;

    b101_rt_fresh(true);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b101_rt, &g_b101_deps));
    (void)pool_session_runtime_boot(&g_b101_rt);

    /* The proven-safe empty posture the pilot exists to watch. */
    TEST_ASSERT_EQUAL(STORE_EMPTY, g_b101_rt.store_result);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_b101_rt));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b101_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_b101_rt, RUNTIME_EVENT_NETWORK_READY));

    /* Several bounded ticks of real owner-task time. */
    for (i = 0; i < 25; i++) {
        g_b101_mono_us += 1000000ull;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* NOTHING the pilot could have touched has moved. */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_b101_rt));
    TEST_ASSERT_EQUAL(RUNTIME_FREE, g_b101_rt.control.state);
    TEST_ASSERT_EQUAL(0, g_b101_nvs.writes);
    TEST_ASSERT_EQUAL(0, g_b101_nvs.commits);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_b101_rt.lease.owner);
    TEST_ASSERT_FALSE(g_b101_rt.lease.restore_required);
    TEST_ASSERT_EQUAL_UINT32(0u, g_b101_rt.tracker.commit_count);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_b101_rt, &g_b101_snap));
    TEST_ASSERT_FALSE(g_b101_snap.session_present);
    TEST_ASSERT_FALSE(g_b101_snap.target_mining_authorized);
    TEST_ASSERT_FALSE(g_b101_snap.pool_mutation_permitted);
    TEST_ASSERT_EQUAL_UINT32(0u, g_b101_snap.proposal_commits);

#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS
    /* The pilot really did run: it read the injected link fact. */
    TEST_ASSERT_TRUE(g_b101_link_reads > 0);
#else
    /* Without the flag the pilot code does not exist, so nothing reads it. */
    TEST_ASSERT_EQUAL_INT(0, g_b101_link_reads);
#endif

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b101_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_deinit(&g_b101_rt));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_task_count());
}

TEST_CASE("b101 rt: a missing link binding leaves the pilot silent and safe",
          "[pool_runtime_b101]")
{
    int i;

    b101_rt_fresh(false); /* deps.link_up == NULL, the shipped binding */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b101_rt, &g_b101_deps));
    (void)pool_session_runtime_boot(&g_b101_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b101_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_b101_rt, RUNTIME_EVENT_NETWORK_READY));

    for (i = 0; i < 15; i++) {
        g_b101_mono_us += 1000000ull;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    TEST_ASSERT_EQUAL_INT(0, g_b101_link_reads);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_b101_rt));
    TEST_ASSERT_EQUAL(0, g_b101_nvs.writes);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_b101_rt.lease.owner);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b101_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_deinit(&g_b101_rt));
}

/*
 * ROLLBACK AVAILABILITY. A pilot build compiles CONFIG_NX_TIMED_SESSIONS in,
 * so the Gate B7 mutation fence sits in front of the AxeOS OTA route the
 * owner must use to roll back. These cases prove the fence ADMITS on the
 * empty posture the pilot runs in — and that it fails CLOSED before the
 * runtime has bootstrapped, which is the documented rollback caveat.
 */

TEST_CASE("b101 rollback: an empty store admits OTA, restart and pool patch",
          "[pool_runtime_b101]")
{
    b101_rt_fresh(true);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b101_rt, &g_b101_deps));
    (void)pool_session_runtime_boot(&g_b101_rt);
    TEST_ASSERT_EQUAL(STORE_EMPTY, g_b101_rt.store_result);

    /* The owner's web-UI rollback path stays open on the pilot posture. */
    TEST_ASSERT_EQUAL(NX_ADMIT_ALLOW,
                      nx_admission_evaluate(&g_b101_rt.coord, NX_MUTATION_OTA_UPDATE));
    TEST_ASSERT_EQUAL(NX_ADMIT_ALLOW,
                      nx_admission_evaluate(&g_b101_rt.coord, NX_MUTATION_DEVICE_RESTART));
    TEST_ASSERT_EQUAL(NX_ADMIT_ALLOW,
                      nx_admission_evaluate(&g_b101_rt.coord, NX_MUTATION_POOL_CONFIG));

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_deinit(&g_b101_rt));
}

TEST_CASE("b101 rollback: an un-bootstrapped coordinator fences OTA closed",
          "[pool_runtime_b101]")
{
    PoolOperationCoordinator coord;

    /* The documented caveat: until ownership is coherent the fence denies,
     * so a failed bootstrap makes the web-UI rollback unavailable and serial
     * recovery is the guaranteed path. */
    memset(&coord, 0, sizeof(coord));
    TEST_ASSERT_EQUAL(NX_ADMIT_DENY_UNBOOTSTRAPPED,
                      nx_admission_evaluate(&coord, NX_MUTATION_OTA_UPDATE));
    TEST_ASSERT_EQUAL(NX_ADMIT_DENY_INVALID,
                      nx_admission_evaluate(NULL, NX_MUTATION_OTA_UPDATE));
}

TEST_CASE("b101 rt: an unconfigured source keeps the pilot inert and mining allowed",
          "[pool_runtime_b101]")
{
    int i;

    b101_rt_fresh(true);
    g_b101_deps.ntp_server = ""; /* nothing to contact at all */

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_b101_rt, &g_b101_deps));
    (void)pool_session_runtime_boot(&g_b101_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_b101_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_b101_rt, RUNTIME_EVENT_NETWORK_READY));

    for (i = 0; i < 15; i++) {
        g_b101_mono_us += 1000000ull;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    TEST_ASSERT_EQUAL(0, g_b101_inits);  /* no network request of any kind */
    TEST_ASSERT_EQUAL(0, g_b101_starts);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_b101_rt));
    TEST_ASSERT_EQUAL(0, g_b101_nvs.writes);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_stop_task(&g_b101_rt));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_deinit(&g_b101_rt));
}
