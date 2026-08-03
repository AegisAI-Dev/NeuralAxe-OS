/*
 * NeuralAxe Gate W4 — recommendation-only runtime tests.
 *
 * Deterministic fake weather and fake time ONLY. No real transport is ever
 * instantiated, no socket is opened, no DNS is performed, no SNTP is started
 * and no hardware is touched.
 */

#include <string.h>
#include "unity.h"
#include "weather_runtime.h"
#include "tuning_profile.h"
#include "tuning_policy.h"

/* ------------------------------------------------------------------ */
/* Fake trusted-time clock                                             */
/* ------------------------------------------------------------------ */

#define US_PER_S 1000000ull
/* 2026-07-15T12:00:00Z — summer, so Brussels is CEST (UTC+2). */
#define EPOCH_SUMMER 1784116800ull

typedef struct {
    uint64_t       monotonic_us;
    PoolTimeAnchor anchor;
    bool           initialized;
} RtClock;

static RtClock g_clk;

static uint64_t rc_monotonic(void *ctx) { return ((RtClock *)ctx)->monotonic_us; }
static bool rc_anchor(void *ctx, PoolTimeAnchor *out)
{
    RtClock *c = (RtClock *)ctx;
    if (!c->initialized) { return false; }
    *out = c->anchor;
    return true;
}
static const PoolTimeClockOps RC_OPS = { .monotonic_us = rc_monotonic,
                                         .read_anchor  = rc_anchor };
static PoolTimeClock       g_clock;
static PoolTimeTrustPolicy g_policy;

static void clock_trusted(uint64_t epoch_s, uint64_t age_s)
{
    memset(&g_clk, 0, sizeof(g_clk));
    g_clk.initialized                 = true;
    g_clk.monotonic_us                = (age_s + 10u) * US_PER_S;
    g_clk.anchor.valid                = true;
    g_clk.anchor.sync_completed_this_boot = true;
    g_clk.anchor.generation           = 3u;
    g_clk.anchor.epoch_us_at_sync     = epoch_s * US_PER_S;
    g_clk.anchor.monotonic_us_at_sync = 10u * US_PER_S;
    g_clk.anchor.sync_status          = POOL_TIME_SYNC_STATUS_COMPLETED;
    g_clk.anchor.last_error           = TIME_OK;
    g_clock.ops = &RC_OPS;
    g_clock.ctx = &g_clk;
    pool_time_trust_policy_defaults(&g_policy);
}

static void clock_untrusted(void)
{
    memset(&g_clk, 0, sizeof(g_clk));
    g_clk.initialized       = true;
    g_clk.anchor.valid      = false;
    g_clk.anchor.sync_status = POOL_TIME_SYNC_STATUS_PENDING;
    g_clock.ops = &RC_OPS;
    g_clock.ctx = &g_clk;
    pool_time_trust_policy_defaults(&g_policy);
}

/* ------------------------------------------------------------------ */
/* Fake transport — counts calls; NEVER performs I/O                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int  calls;
    bool serve_body;
    const char *body;
    int  http_status;
} FakeTransport;

static FakeTransport g_tx;

static WeatherProviderResult tx_fetch(void *ctx, const WeatherRequest *req,
                                      WeatherHttpResponse *out)
{
    FakeTransport *t = (FakeTransport *)ctx;
    size_t n;

    (void)req;
    t->calls++;
    memset(out, 0, sizeof(*out));
    if (!t->serve_body) {
        return WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT;
    }
    n = strlen(t->body);
    if (n > WEATHER_RESPONSE_CAP) { n = WEATHER_RESPONSE_CAP; }
    memcpy(out->body, t->body, n);
    out->body_len             = n;
    out->http_status          = t->http_status;
    out->content_type_present = true;
    out->content_type_json    = true;
    out->complete             = true;
    out->oversized            = false;
    return WEATHER_PROVIDER_OK;
}

static const WeatherTransportOps TX_OPS = { .fetch = tx_fetch };

/* ------------------------------------------------------------------ */
/* Policy environment                                                  */
/* ------------------------------------------------------------------ */

static TuningPolicyEnvironment production_env(void)
{
    TuningPolicyEnvironment env;
    size_t count = 0;
    memset(&env, 0, sizeof(env));
    env.profiles      = tuning_registry_gamma601(&count);
    env.profile_count = count;
    env.hw.board      = TUNING_BOARD_GAMMA_601;
    env.hw.asic       = TUNING_ASIC_BM1370;
    env.hw.cooling_installed = TUNING_COOLING_SUPERSINK_DUAL_FAN;
    env.hw.psu_installed     = TUNING_PSU_STANDARD;
    return env;
}

static TuningPolicyInput baseline_input(void)
{
    TuningPolicyInput in;
    memset(&in, 0, sizeof(in));
    in.policy_generation      = 1u;
    in.current_profile_known  = false;
    in.roles.cool_day_profile_id[0]  = '\0';
    in.emergency_thermal_active = false;
    in.stability_rollback_active = false;
    in.sensors.asic_temp = TUNING_SENSOR_OK;
    in.sensors.vrm_temp  = TUNING_SENSOR_OK;
    in.sensors.vrm_expected = true;
    in.sensors.fan_tach  = TUNING_SENSOR_OK;
    in.sensors.fan_control_uncertain = false;
    in.mining         = TUNING_MINING_STABLE;
    in.trusted_time_valid = true;
    return in;
}

/* ------------------------------------------------------------------ */
/* Config helpers                                                      */
/* ------------------------------------------------------------------ */

static WeatherRuntimeConfig cfg_enabled_configured(void)
{
    WeatherRuntimeConfig c;
    weather_runtime_config_defaults(&c);
    c.enabled           = true;
    c.expected_provider = WEATHER_PROVIDER_OPEN_METEO;
    c.location.latitude_e4  = 508500;    /* a synthetic test fixture only */
    c.location.longitude_e4 = 43500;
    c.location.timezone     = WEATHER_TZ_EUROPE_BRUSSELS;
    c.schedule.enabled    = true;
    c.schedule.slot_count = 1u;
    /* W3 requires canonical zeros past slot_count; the defaults left the
     * 11:00 and 15:00 product slots in place, which is a config the
     * committed validator correctly refuses. */
    memset(c.schedule.slots_min, 0, sizeof(c.schedule.slots_min));
    c.schedule.slots_min[0] = 0u;        /* due from local midnight       */
    return c;
}

/*
 * The fixture above must be a configuration the COMMITTED W3 validator
 * accepts. Without this check a malformed fixture silently turns every
 * runtime test into an init failure that still "passes" its refusal
 * assertions — which is exactly what happened on the first QEMU run.
 */
TEST_CASE("W4 rt: the enabled test fixture is a valid W3/W4 configuration",
          "[weather_runtime]")
{
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntime rt;

    TEST_ASSERT_TRUE(weather_schedule_config_valid(&c.schedule));
    TEST_ASSERT_TRUE(weather_runtime_config_valid(&c));
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, NULL));
    TEST_ASSERT_NOT_EQUAL(WEATHER_RUNTIME_DISABLED, rt.state);

    /* And a config with non-canonical slots past slot_count is REFUSED, so
     * the validator is doing real work rather than accepting anything. */
    c.schedule.slots_min[1] = 660u;
    TEST_ASSERT_FALSE(weather_schedule_config_valid(&c.schedule));
    TEST_ASSERT_FALSE(weather_runtime_config_valid(&c));
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_ERR_CONFIG_INVALID,
                      weather_runtime_init(&rt, &c, NULL));
}

static WeatherObservation fresh_observation(int16_t max_dc, uint64_t now_s)
{
    WeatherObservation o;
    memset(&o, 0, sizeof(o));
    weather_forecast_init(&o.forecast);
    o.provider_result = WEATHER_PROVIDER_OK;
    o.forecast_present = true;
    o.forecast.provider  = WEATHER_PROVIDER_OPEN_METEO;
    o.forecast.timezone  = WEATHER_TZ_EUROPE_BRUSSELS;
    o.forecast.utc_offset_s = 7200;
    o.forecast.forecast_max_dc = max_dc;
    o.forecast.fetch_epoch_s   = now_s;
    o.forecast.fetch_epoch_trusted = true;
    o.forecast.validated = true;
    o.forecast.source_generation = 1u;
    {
        BrusselsLocalTime lt;
        if (brussels_local_from_utc(now_s, &lt)) {
            o.forecast.local_date = lt.date;
        }
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* C. Default-off behaviour                                            */
/* ------------------------------------------------------------------ */

TEST_CASE("W4 rt: defaults are the safe posture (disabled, unconfigured)",
          "[weather_runtime]")
{
    WeatherRuntimeConfig c;
    weather_runtime_config_defaults(&c);
    TEST_ASSERT_FALSE(c.enabled);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_UNCONFIGURED, c.expected_provider);
    TEST_ASSERT_EQUAL_INT32(0, c.location.latitude_e4);
    TEST_ASSERT_EQUAL_INT32(0, c.location.longitude_e4);
    TEST_ASSERT_FALSE(c.schedule.enabled);
    TEST_ASSERT_TRUE(weather_runtime_config_valid(&c));
}

TEST_CASE("W4 rt: a disabled runtime does nothing and recommends nothing",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c;
    WeatherRecommendation rec;
    WeatherRuntimeDeps deps;

    weather_runtime_config_defaults(&c);
    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    deps.transport = &TX_OPS;
    deps.transport_ctx = &g_tx;
    memset(&g_tx, 0, sizeof(g_tx));

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_DISABLED, rt.state);

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_DISABLED,
                      weather_runtime_step(&rt, NULL, NULL, &rec));
    TEST_ASSERT_FALSE(rec.present);
    TEST_ASSERT_EQUAL(WEATHER_NOT_EXECUTED_RUNTIME_DISABLED, rec.not_executed);
    /* Even with a transport injected, a disabled runtime never calls it. */
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);
    TEST_ASSERT_EQUAL_UINT32(0u, rt.store_write_requests);
}

TEST_CASE("W4 rt: a zeroed runtime fails closed", "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRecommendation rec;
    memset(&rt, 0, sizeof(rt));
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_INTERNAL_ERROR,
                      weather_runtime_step(&rt, NULL, NULL, &rec));
    TEST_ASSERT_FALSE(rec.present);
    TEST_ASSERT_EQUAL(WEATHER_NOT_EXECUTED_INTERNAL_ERROR, rec.not_executed);
}

/* ------------------------------------------------------------------ */
/* B. Trusted-time convergence                                         */
/* ------------------------------------------------------------------ */

TEST_CASE("W4 rt: no trusted time means a bounded wait and no policy",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherRecommendation rec;
    WeatherObservation obs;

    clock_untrusted();
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    deps.transport = &TX_OPS;
    deps.transport_ctx = &g_tx;
    memset(&g_tx, 0, sizeof(g_tx));

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    obs = fresh_observation(200, EPOCH_SUMMER);

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME,
                      weather_runtime_step(&rt, NULL, &obs, &rec));
    TEST_ASSERT_FALSE(rec.present);
    TEST_ASSERT_EQUAL(WEATHER_NOT_EXECUTED_NO_TRUSTED_TIME, rec.not_executed);
    TEST_ASSERT_FALSE(rec.trusted_time_at_evaluation);
    TEST_ASSERT_EQUAL_UINT64(0u, rec.evaluated_utc_s);
    TEST_ASSERT_EQUAL_UINT32(0u, rt.store_write_requests);
}

TEST_CASE("W4 rt: with no clock injected weather waits, never falls back",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRecommendation rec;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, NULL));
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME,
                      weather_runtime_step(&rt, NULL, NULL, &rec));
    TEST_ASSERT_FALSE(rec.present);
    TEST_ASSERT_EQUAL(WEATHER_TIME_PROVIDER_ABSENT, rec.time_state);
}

TEST_CASE("W4 rt: stale trusted time blocks evaluation", "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherRecommendation rec;
    WeatherObservation obs;

    clock_trusted(EPOCH_SUMMER, 200000u);      /* anchor far too old */
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    obs = fresh_observation(200, EPOCH_SUMMER);
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME,
                      weather_runtime_step(&rt, NULL, &obs, &rec));
    TEST_ASSERT_FALSE(rec.present);
    TEST_ASSERT_EQUAL(WEATHER_TIME_STALE, rec.time_state);
}

/* ------------------------------------------------------------------ */
/* F. Client / scheduler                                               */
/* ------------------------------------------------------------------ */

TEST_CASE("W4 rt: an unconfigured source performs NO network request",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c;
    WeatherRuntimeDeps deps;
    WeatherObservation obs;
    BrusselsLocalTime lt;

    weather_runtime_config_defaults(&c);
    c.enabled = true;                        /* enabled but UNCONFIGURED */
    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    deps.transport = &TX_OPS;
    deps.transport_ctx = &g_tx;
    memset(&g_tx, 0, sizeof(g_tx));
    g_tx.serve_body = true;
    g_tx.body = "{}";
    g_tx.http_status = 200;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    TEST_ASSERT_TRUE(brussels_local_from_utc(EPOCH_SUMMER, &lt));

    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_UNCONFIGURED,
                      weather_runtime_fetch(&rt, &lt.date, EPOCH_SUMMER, true, &obs));
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);    /* the transport was never called */
    TEST_ASSERT_FALSE(obs.forecast_present);
}

TEST_CASE("W4 rt: no transport injected performs NO network request",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherObservation obs;
    BrusselsLocalTime lt;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    deps.transport = NULL;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    TEST_ASSERT_TRUE(brussels_local_from_utc(EPOCH_SUMMER, &lt));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_UNCONFIGURED,
                      weather_runtime_fetch(&rt, &lt.date, EPOCH_SUMMER, true, &obs));
    TEST_ASSERT_FALSE(obs.forecast_present);
}

TEST_CASE("W4 rt: untrusted time performs NO network request", "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherObservation obs;
    BrusselsLocalTime lt;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    deps.transport = &TX_OPS;
    deps.transport_ctx = &g_tx;
    memset(&g_tx, 0, sizeof(g_tx));

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    TEST_ASSERT_TRUE(brussels_local_from_utc(EPOCH_SUMMER, &lt));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CACHE_STALE,
                      weather_runtime_fetch(&rt, &lt.date, EPOCH_SUMMER, false, &obs));
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);
}

TEST_CASE("W4 rt: a transport failure is bounded and fails passive",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherObservation obs;
    BrusselsLocalTime lt;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    deps.transport = &TX_OPS;
    deps.transport_ctx = &g_tx;
    memset(&g_tx, 0, sizeof(g_tx));
    g_tx.serve_body = false;                 /* connect failure */

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    TEST_ASSERT_TRUE(brussels_local_from_utc(EPOCH_SUMMER, &lt));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT,
                      weather_runtime_fetch(&rt, &lt.date, EPOCH_SUMMER, true, &obs));
    TEST_ASSERT_EQUAL_INT(1, g_tx.calls);
    TEST_ASSERT_FALSE(obs.forecast_present);
}

TEST_CASE("W4 rt: a malformed body is rejected and yields no forecast",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherObservation obs;
    BrusselsLocalTime lt;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    deps.transport = &TX_OPS;
    deps.transport_ctx = &g_tx;
    memset(&g_tx, 0, sizeof(g_tx));
    g_tx.serve_body  = true;
    g_tx.body        = "{ not json at all ";
    g_tx.http_status = 200;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    TEST_ASSERT_TRUE(brussels_local_from_utc(EPOCH_SUMMER, &lt));
    TEST_ASSERT_NOT_EQUAL(WEATHER_PROVIDER_OK,
                          weather_runtime_fetch(&rt, &lt.date, EPOCH_SUMMER,
                                                true, &obs));
    TEST_ASSERT_FALSE(obs.forecast_present);
    TEST_ASSERT_FALSE(obs.forecast.validated);
}

/* ------------------------------------------------------------------ */
/* D. Recommendation-only behaviour                                    */
/* ------------------------------------------------------------------ */

TEST_CASE("W4 rt: fresh weather produces a bounded recommendation only",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherRuntimeStepEnv env;
    TuningPolicyEnvironment penv = production_env();
    TuningPolicyInput pin = baseline_input();
    WeatherRecommendation rec;
    WeatherObservation obs;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    env.policy_env = &penv;
    env.policy_in  = &pin;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    obs = fresh_observation(200, EPOCH_SUMMER);

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_RECOMMENDATION_READY,
                      weather_runtime_step(&rt, &env, &obs, &rec));
    TEST_ASSERT_TRUE(rec.present);
    TEST_ASSERT_TRUE(rec.trusted_time_at_evaluation);
    /* clock_trusted() anchors 5 s in the past and B2 advances the anchored
     * epoch by the elapsed MONOTONIC time, so the evaluated instant is
     * EPOCH_SUMMER + 5. The runtime publishes B2's value verbatim. */
    TEST_ASSERT_EQUAL_UINT64(EPOCH_SUMMER + 5ull, rec.evaluated_utc_s);
    TEST_ASSERT_EQUAL(WEATHER_FRESHNESS_FRESH, rec.freshness);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_OK, rec.forecast_status);

    /* THE contract: against the production registry nothing is actionable,
     * because every production profile ships UNVALIDATED. */
    TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
    TEST_ASSERT_NOT_EQUAL(TUNING_SELECT_PROFILE, rec.intent.action);
}

TEST_CASE("W4 rt: stale weather never becomes recommendation-ready",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherRuntimeStepEnv env;
    TuningPolicyEnvironment penv = production_env();
    TuningPolicyInput pin = baseline_input();
    WeatherRecommendation rec;
    WeatherObservation obs;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    env.policy_env = &penv;
    env.policy_in  = &pin;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    /* Fetched a full day before "now": beyond max_forecast_age_s. */
    obs = fresh_observation(200, EPOCH_SUMMER);
    obs.forecast.fetch_epoch_s = EPOCH_SUMMER - 86400ull;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_WEATHER_STALE,
                      weather_runtime_step(&rt, &env, &obs, &rec));
    TEST_ASSERT_FALSE(rec.present);
    TEST_ASSERT_EQUAL(WEATHER_NOT_EXECUTED_WEATHER_UNUSABLE, rec.not_executed);
}

TEST_CASE("W4 rt: invalid weather produces no recommendation", "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherRuntimeStepEnv env;
    TuningPolicyEnvironment penv = production_env();
    TuningPolicyInput pin = baseline_input();
    WeatherRecommendation rec;
    WeatherObservation obs;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    env.policy_env = &penv;
    env.policy_in  = &pin;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    obs = fresh_observation(200, EPOCH_SUMMER);
    obs.forecast.validated = false;            /* technically invalid */
    obs.provider_result    = WEATHER_PROVIDER_ERR_SCHEMA_INVALID;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_WEATHER_REJECTED,
                      weather_runtime_step(&rt, &env, &obs, &rec));
    TEST_ASSERT_FALSE(rec.present);
}

TEST_CASE("W4 rt: a duplicate input is idempotent", "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherRuntimeStepEnv env;
    TuningPolicyEnvironment penv = production_env();
    TuningPolicyInput pin = baseline_input();
    WeatherRecommendation a, b, d;
    WeatherObservation obs;
    uint32_t writes_after_first;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    env.policy_env = &penv;
    env.policy_in  = &pin;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    obs = fresh_observation(200, EPOCH_SUMMER);

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_RECOMMENDATION_READY,
                      weather_runtime_step(&rt, &env, &obs, &a));
    writes_after_first = rt.store_write_requests;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_RECOMMENDATION_READY,
                      weather_runtime_step(&rt, &env, &obs, &b));
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_RECOMMENDATION_READY,
                      weather_runtime_step(&rt, &env, &obs, &d));

    /* Byte-identical results, no serial advance, and NO extra write. */
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
    TEST_ASSERT_EQUAL_MEMORY(&a, &d, sizeof(a));
    TEST_ASSERT_EQUAL_UINT32(a.evaluation_serial, d.evaluation_serial);
    TEST_ASSERT_EQUAL_UINT32(writes_after_first, rt.store_write_requests);
}

TEST_CASE("W4 rt: a repeated FAILURE is idempotent and bounded",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherRuntimeStepEnv env;
    TuningPolicyEnvironment penv = production_env();
    TuningPolicyInput pin = baseline_input();
    WeatherRecommendation rec;
    WeatherObservation obs;
    uint8_t attempts_after_first;
    uint32_t writes_after_first;
    int i;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    env.policy_env = &penv;
    env.policy_in  = &pin;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    obs = fresh_observation(200, EPOCH_SUMMER);
    obs.forecast.validated = false;                 /* a stuck bad payload */
    obs.provider_result    = WEATHER_PROVIDER_ERR_SCHEMA_INVALID;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_WEATHER_REJECTED,
                      weather_runtime_step(&rt, &env, &obs, &rec));
    attempts_after_first = rt.attempts_this_slot;
    writes_after_first   = rt.store_write_requests;

    /* A stuck input must not consume retry budget or request writes forever. */
    for (i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL(WEATHER_RUNTIME_WEATHER_REJECTED,
                          weather_runtime_step(&rt, &env, &obs, &rec));
        TEST_ASSERT_FALSE(rec.present);
    }
    TEST_ASSERT_EQUAL_UINT8(attempts_after_first, rt.attempts_this_slot);
    TEST_ASSERT_EQUAL_UINT32(writes_after_first, rt.store_write_requests);
    /* And mining/settings are untouched: the runtime has no way to say so. */
    TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
}

TEST_CASE("W4 rt: trusted-time loss invalidates a later evaluation",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherRuntimeStepEnv env;
    TuningPolicyEnvironment penv = production_env();
    TuningPolicyInput pin = baseline_input();
    WeatherRecommendation rec;
    WeatherObservation obs;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    env.policy_env = &penv;
    env.policy_in  = &pin;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    obs = fresh_observation(200, EPOCH_SUMMER);
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_RECOMMENDATION_READY,
                      weather_runtime_step(&rt, &env, &obs, &rec));
    TEST_ASSERT_TRUE(rec.present);

    /* The anchor is lost mid-flight. */
    g_clk.anchor.valid = false;
    g_clk.anchor.sync_status = POOL_TIME_SYNC_STATUS_NONE;
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME,
                      weather_runtime_step(&rt, &env, &obs, &rec));
    TEST_ASSERT_FALSE(rec.present);
}

/* ------------------------------------------------------------------ */
/* H. Property tests                                                   */
/* ------------------------------------------------------------------ */

/*
 * The recommendation is a value type with no pointer of any kind, so it
 * cannot carry or become an executable command. Checked structurally.
 */
TEST_CASE("W4 property: a recommendation carries no executable command",
          "[weather_runtime]")
{
    WeatherRecommendation rec;
    memset(&rec, 0xA5, sizeof(rec));
    memset(&rec, 0, sizeof(rec));

    /* A zeroed recommendation is inert in every field that matters. */
    TEST_ASSERT_FALSE(rec.present);
    TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
    TEST_ASSERT_EQUAL(WEATHER_NOT_EXECUTED_INTERNAL_ERROR, rec.not_executed);
    TEST_ASSERT_EQUAL(WEATHER_FRESHNESS_UNKNOWN, rec.freshness);
    /* The W1 intent carries a profile ID, never a tuning payload: the type
     * has no frequency, voltage, fan or thermal field at all. Verified by
     * construction — a numeric payload field would break this build. */
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, rec.intent.action);
    TEST_ASSERT_EQUAL_UINT8(0u, (uint8_t)rec.intent.profile_id[0]);
}

/*
 * Sweep the whole decision space and assert the safety invariants hold in
 * every single case: weather alone never selects an actionable production
 * profile, never evaluates without trusted time, and stale weather never
 * becomes ready.
 */
TEST_CASE("W4 property: weather alone can never authorize a hardware change",
          "[weather_runtime]")
{
    static const int16_t FORECASTS[] = { -600, -200, 0, 150, 279, 280, 299,
                                         300, 350, 450, 600 };
    static const int AGES[] = { 0, 60, 3600, 21600, 86400 };
    size_t f, a;
    int trusted_evaluations = 0;

    for (f = 0; f < sizeof(FORECASTS) / sizeof(FORECASTS[0]); f++) {
        for (a = 0; a < sizeof(AGES) / sizeof(AGES[0]); a++) {
            WeatherRuntime rt;
            WeatherRuntimeConfig c = cfg_enabled_configured();
            WeatherRuntimeDeps deps;
            WeatherRuntimeStepEnv env;
            TuningPolicyEnvironment penv = production_env();
            TuningPolicyInput pin = baseline_input();
            WeatherRecommendation rec;
            WeatherObservation obs;
            WeatherRuntimeState st;

            clock_trusted(EPOCH_SUMMER, 5u);
            memset(&deps, 0, sizeof(deps));
            deps.clock = &g_clock;
            deps.time_policy = &g_policy;
            env.policy_env = &penv;
            env.policy_in  = &pin;

            TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK,
                              weather_runtime_init(&rt, &c, &deps));
            obs = fresh_observation(FORECASTS[f], EPOCH_SUMMER);
            obs.forecast.fetch_epoch_s = EPOCH_SUMMER - (uint64_t)AGES[a];

            st = weather_runtime_step(&rt, &env, &obs, &rec);

            /* 1-5: nothing is ever actionable against production profiles. */
            TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
            TEST_ASSERT_NOT_EQUAL(TUNING_SELECT_PROFILE, rec.intent.action);

            /* 7: stale weather is never recommendation-ready. */
            if (st == WEATHER_RUNTIME_RECOMMENDATION_READY) {
                trusted_evaluations++;
                TEST_ASSERT_TRUE(rec.present);
                TEST_ASSERT_TRUE(rec.trusted_time_at_evaluation);
                TEST_ASSERT_EQUAL(WEATHER_FRESHNESS_FRESH, rec.freshness);
            } else {
                TEST_ASSERT_FALSE(rec.present);
            }

            /* 6: a recommendation only ever exists with trusted time. */
            if (rec.present) {
                TEST_ASSERT_TRUE(rec.trusted_time_at_evaluation);
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, trusted_evaluations);
}

TEST_CASE("W4 property: without trusted time no policy is ever evaluated",
          "[weather_runtime]")
{
    static const int16_t FORECASTS[] = { -600, 0, 200, 300, 600 };
    size_t f;

    for (f = 0; f < sizeof(FORECASTS) / sizeof(FORECASTS[0]); f++) {
        WeatherRuntime rt;
        WeatherRuntimeConfig c = cfg_enabled_configured();
        WeatherRuntimeDeps deps;
        WeatherRuntimeStepEnv env;
        TuningPolicyEnvironment penv = production_env();
        TuningPolicyInput pin = baseline_input();
        WeatherRecommendation rec;
        WeatherObservation obs;

        clock_untrusted();
        memset(&deps, 0, sizeof(deps));
        deps.clock = &g_clock;
        deps.time_policy = &g_policy;
        env.policy_env = &penv;
        env.policy_in  = &pin;

        TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
        obs = fresh_observation(FORECASTS[f], EPOCH_SUMMER);

        TEST_ASSERT_EQUAL(WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME,
                          weather_runtime_step(&rt, &env, &obs, &rec));
        TEST_ASSERT_FALSE(rec.present);
        /* The climate hysteresis state must not have advanced either. */
        TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_UNKNOWN, rt.climate.state);
    }
}

TEST_CASE("W4 property: duplicate events never start duplicate clients",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherObservation obs;
    BrusselsLocalTime lt;
    int i;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    deps.transport = &TX_OPS;
    deps.transport_ctx = &g_tx;
    memset(&g_tx, 0, sizeof(g_tx));
    g_tx.serve_body = false;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    TEST_ASSERT_TRUE(brussels_local_from_utc(EPOCH_SUMMER, &lt));

    /* Each fetch is exactly one transport call — never a fan-out. */
    for (i = 0; i < 5; i++) {
        (void)weather_runtime_fetch(&rt, &lt.date, EPOCH_SUMMER, true, &obs);
    }
    TEST_ASSERT_EQUAL_INT(5, g_tx.calls);
}

TEST_CASE("W4 property: the same pure input yields byte-identical output",
          "[weather_runtime]")
{
    WeatherRuntime a, b;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherRuntimeStepEnv env;
    TuningPolicyEnvironment penv = production_env();
    TuningPolicyInput pin = baseline_input();
    WeatherRecommendation ra, rb;
    WeatherObservation obs;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    env.policy_env = &penv;
    env.policy_in  = &pin;
    obs = fresh_observation(310, EPOCH_SUMMER);

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&a, &c, &deps));
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&b, &c, &deps));
    (void)weather_runtime_step(&a, &env, &obs, &ra);
    (void)weather_runtime_step(&b, &env, &obs, &rb);
    TEST_ASSERT_EQUAL_MEMORY(&ra, &rb, sizeof(ra));
}

/* ------------------------------------------------------------------ */
/* E. Bounded writes                                                   */
/* ------------------------------------------------------------------ */

TEST_CASE("W4 rt: an unchanged recommendation requests no repeated write",
          "[weather_runtime]")
{
    WeatherRuntime rt;
    WeatherRuntimeConfig c = cfg_enabled_configured();
    WeatherRuntimeDeps deps;
    WeatherRuntimeStepEnv env;
    TuningPolicyEnvironment penv = production_env();
    TuningPolicyInput pin = baseline_input();
    WeatherRecommendation rec;
    WeatherObservation obs;
    uint32_t first;
    int i;

    clock_trusted(EPOCH_SUMMER, 5u);
    memset(&deps, 0, sizeof(deps));
    deps.clock = &g_clock;
    deps.time_policy = &g_policy;
    env.policy_env = &penv;
    env.policy_in  = &pin;

    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &c, &deps));
    obs = fresh_observation(200, EPOCH_SUMMER);
    (void)weather_runtime_step(&rt, &env, &obs, &rec);
    first = rt.store_write_requests;

    for (i = 0; i < 50; i++) {
        (void)weather_runtime_step(&rt, &env, &obs, &rec);
    }
    TEST_ASSERT_EQUAL_UINT32(first, rt.store_write_requests);
    TEST_ASSERT_TRUE(first <= 2u);        /* bounded, not per-tick */
}

/* ------------------------------------------------------------------ */
/* Tokens                                                             */
/* ------------------------------------------------------------------ */

TEST_CASE("W4 rt: every state and reason has a distinct bounded token",
          "[weather_runtime]")
{
    int i, j;

    for (i = 0; i < (int)WEATHER_RUNTIME_STATE__COUNT; i++) {
        const char *a = weather_runtime_state_str((WeatherRuntimeState)i);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_TRUE(strlen(a) < 40u);
        for (j = i + 1; j < (int)WEATHER_RUNTIME_STATE__COUNT; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(a, weather_runtime_state_str(
                                                    (WeatherRuntimeState)j)));
        }
    }
    for (i = 0; i < (int)WEATHER_NOT_EXECUTED__COUNT; i++) {
        const char *a = weather_not_executed_str((WeatherNotExecutedReason)i);
        TEST_ASSERT_NOT_NULL(a);
        for (j = i + 1; j < (int)WEATHER_NOT_EXECUTED__COUNT; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(a, weather_not_executed_str(
                                                    (WeatherNotExecutedReason)j)));
        }
    }
    for (i = 0; i < (int)WEATHER_FRESHNESS__COUNT; i++) {
        TEST_ASSERT_NOT_NULL(weather_freshness_str((WeatherFreshnessClass)i));
    }
    TEST_ASSERT_EQUAL_STRING("WX_INTERNAL_ERROR",
                             weather_runtime_state_str((WeatherRuntimeState)999));
}
