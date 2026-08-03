/*
 * Deterministic tests for the W3 -> W1 climate-policy bridge.
 *
 * These tests run the FULL committed W1 chain (bridge -> hysteresis ->
 * precedence arbitration) over synthetic weather facts. No network, no
 * clocks, no hardware: every epoch and every sensor reading is an input.
 *
 * Two safety properties are proven here end to end:
 *  1. the informational current outdoor temperature can never influence a
 *     selection intent;
 *  2. the PRODUCTION profile registry stays unselectable no matter what the
 *     weather says (all production profiles ship UNVALIDATED and
 *     payload-free).
 */

#include <string.h>
#include "unity.h"
#include "weather_bridge.h"
#include "weather_cache.h"
#include "tuning_policy.h"

#define NOW_S 1785000000ull /* synthetic trusted epoch (2026-07) */
#define MAX_AGE_S 21600u

static WeatherLocalDate d(uint16_t y, uint8_t m, uint8_t dd)
{
    WeatherLocalDate x = { y, m, dd };
    return x;
}

static WeatherForecast forecast_of(int16_t max_dc)
{
    WeatherForecast f;
    weather_forecast_init(&f);
    f.provider = WEATHER_PROVIDER_OPEN_METEO;
    f.local_date = d(2026, 7, 15);
    f.forecast_max_dc = max_dc;
    f.current_valid = true;
    f.current_dc = 250;
    f.timezone = WEATHER_TZ_EUROPE_BRUSSELS;
    f.utc_offset_s = 7200;
    f.fetch_epoch_s = NOW_S - 60ull;
    f.fetch_epoch_trusted = true;
    f.source_generation = 5u;
    f.validated = true;
    return f;
}

static void bridge_today(const WeatherForecast *f, WeatherProviderResult r,
                         TuningForecastInput *out)
{
    WeatherLocalDate today = d(2026, 7, 15);
    weather_bridge_to_policy(f, r, WEATHER_PROVIDER_OPEN_METEO, &today, NOW_S,
                             true, MAX_AGE_S, out);
}

/* ---------------- W1 fixtures (mirroring the committed W1 tests) -------- */

static TuningHardwareContext hw_full(void)
{
    TuningHardwareContext hw;
    memset(&hw, 0, sizeof(hw));
    hw.board = TUNING_BOARD_GAMMA_601;
    hw.asic = TUNING_ASIC_BM1370;
    hw.cooling_installed = TUNING_COOLING_SUPERSINK_DUAL_FAN;
    hw.psu_installed = TUNING_PSU_STANDARD;
    return hw;
}

/* SYNTHETIC control registry: the production trio promoted to VALIDATED so
 * an upgrade path demonstrably EXISTS. Production ships UNVALIDATED. */
static void reg_validated(TuningProfile out[TUNING_REGISTRY_GAMMA601_COUNT])
{
    size_t n = 0;
    size_t i;
    const TuningProfile *r = tuning_registry_gamma601(&n);
    memcpy(out, r, sizeof(TuningProfile) * TUNING_REGISTRY_GAMMA601_COUNT);
    for (i = 0; i < TUNING_REGISTRY_GAMMA601_COUNT; i++) {
        out[i].validation = TUNING_VALIDATION_VALIDATED;
        if (out[i].evidence_fingerprint == 0) {
            out[i].evidence_fingerprint = 0xE3F0u + (uint32_t)i;
        }
    }
}

static TuningPolicyInput policy_in(const char *current)
{
    TuningPolicyInput in;
    memset(&in, 0, sizeof(in));
    in.policy_generation = 3;
    strncpy(in.current_profile_id, current, sizeof(in.current_profile_id) - 1);
    in.current_profile_known = true;
    strncpy(in.roles.cool_day_profile_id, TUNING_PROFILE_ID_SUPERSINK_MAX,
            sizeof(in.roles.cool_day_profile_id) - 1);
    strncpy(in.roles.hot_day_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(in.roles.hot_day_profile_id) - 1);
    strncpy(in.roles.emergency_profile_id, TUNING_PROFILE_ID_EMERGENCY,
            sizeof(in.roles.emergency_profile_id) - 1);
    in.sensors.asic_temp = TUNING_SENSOR_OK;
    in.sensors.vrm_temp = TUNING_SENSOR_OK;
    in.sensors.vrm_expected = true;
    in.sensors.fan_tach = TUNING_SENSOR_OK;
    in.trusted_time_valid = true;
    in.climate_request = TUNING_CLIMATE_REQ_RETAIN;
    in.mining = TUNING_MINING_STABLE;
    in.now_epoch_s = NOW_S;
    return in;
}

/* Run the whole chain: weather facts -> bridge -> hysteresis -> arbitration. */
static void run_chain(const WeatherForecast *f, WeatherProviderResult r,
                      const TuningProfile *registry, size_t count,
                      const char *current_id,
                      TuningForecastInput *out_input,
                      TuningClimateRequest *out_request,
                      TuningSelectionIntent *out_intent)
{
    TuningClimateState st, next;
    TuningClimateThresholds th;
    TuningPolicyEnvironment env;
    TuningPolicyInput in;

    bridge_today(f, r, out_input);

    tuning_climate_state_init(&st);
    tuning_climate_thresholds_defaults(&th);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK,
                      tuning_climate_step(&st, &th, out_input, &next, out_request));

    memset(&env, 0, sizeof(env));
    env.profiles = registry;
    env.profile_count = count;
    env.hw = hw_full();
    in = policy_in(current_id);
    in.climate_request = *out_request;
    in.trusted_time_valid = true;

    memset(out_intent, 0, sizeof(*out_intent));
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK,
                      tuning_policy_evaluate(&env, &in, out_intent));
}

/* ================================================================== */
/* Mapping matrix                                                      */
/* ================================================================== */

TEST_CASE("bridge: a usable forecast passes only the maximum to policy",
          "[weather_bridge]")
{
    WeatherForecast f = forecast_of(324);
    TuningForecastInput out;

    bridge_today(&f, WEATHER_PROVIDER_OK, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_OK, out.status);
    TEST_ASSERT_EQUAL_INT16(324, out.forecast_max_dc);

    /* The bridge output is the ONLY channel into the policy layer, and it
     * carries a status plus the daily maximum — the fetch epoch, the
     * provider identity, the timezone/offset, the generation counter and
     * the current outdoor reading all stop here. */
    f.current_dc = 599;
    f.source_generation = 0xFFFFFFFFu;
    f.utc_offset_s = 3600;
    {
        TuningForecastInput again;
        bridge_today(&f, WEATHER_PROVIDER_OK, &again);
        TEST_ASSERT_EQUAL_MEMORY(&out, &again, sizeof(again));
    }
}

TEST_CASE("bridge: every provider failure maps to a fail-safe status",
          "[weather_bridge]")
{
    TuningForecastInput out;
    int i;

    /* technically invalid responses */
    static const WeatherProviderResult invalid[] = {
        WEATHER_PROVIDER_ERR_JSON_INVALID, WEATHER_PROVIDER_ERR_SCHEMA_INVALID,
        WEATHER_PROVIDER_ERR_UNITS_INVALID, WEATHER_PROVIDER_ERR_TIMEZONE_INVALID,
        WEATHER_PROVIDER_ERR_VALUE_INVALID
    };
    /* date/staleness problems */
    static const WeatherProviderResult stale[] = {
        WEATHER_PROVIDER_ERR_DATE_INVALID, WEATHER_PROVIDER_ERR_CACHE_STALE
    };

    for (i = 0; i < (int)(sizeof(invalid) / sizeof(invalid[0])); i++) {
        bridge_today(NULL, invalid[i], &out);
        TEST_ASSERT_EQUAL(TUNING_FORECAST_INVALID, out.status);
        TEST_ASSERT_EQUAL_INT16(0, out.forecast_max_dc);
    }
    for (i = 0; i < (int)(sizeof(stale) / sizeof(stale[0])); i++) {
        bridge_today(NULL, stale[i], &out);
        TEST_ASSERT_EQUAL(TUNING_FORECAST_STALE, out.status);
        TEST_ASSERT_EQUAL_INT16(0, out.forecast_max_dc);
    }

    /* every remaining result is UNAVAILABLE — and no result of any kind
     * can ever produce an OK status without a usable forecast */
    for (i = 0; i < WEATHER_PROVIDER_RESULT__COUNT; i++) {
        WeatherProviderResult r = (WeatherProviderResult)i;
        bridge_today(NULL, r, &out);
        TEST_ASSERT_NOT_EQUAL(TUNING_FORECAST_OK, out.status);
        TEST_ASSERT_EQUAL_INT16(0, out.forecast_max_dc);
    }

    /* NULL output must not crash */
    weather_bridge_to_policy(NULL, WEATHER_PROVIDER_OK,
                             WEATHER_PROVIDER_OPEN_METEO, NULL, NOW_S, true,
                             MAX_AGE_S, NULL);
}

TEST_CASE("bridge: an OK result still has to survive the usability rule",
          "[weather_bridge]")
{
    TuningForecastInput out;
    WeatherForecast f;

    /* yesterday's forecast */
    f = forecast_of(324);
    f.local_date = d(2026, 7, 14);
    bridge_today(&f, WEATHER_PROVIDER_OK, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_STALE, out.status);

    /* over-age fetch */
    f = forecast_of(324);
    f.fetch_epoch_s = NOW_S - (uint64_t)MAX_AGE_S - 1ull;
    bridge_today(&f, WEATHER_PROVIDER_OK, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_STALE, out.status);

    /* future fetch epoch */
    f = forecast_of(324);
    f.fetch_epoch_s = NOW_S + 1ull;
    bridge_today(&f, WEATHER_PROVIDER_OK, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_STALE, out.status);

    /* untrusted fetch epoch */
    f = forecast_of(324);
    f.fetch_epoch_trusted = false;
    bridge_today(&f, WEATHER_PROVIDER_OK, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_STALE, out.status);

    /* wrong provider / wrong timezone / not validated */
    f = forecast_of(324);
    f.provider = WEATHER_PROVIDER_UNCONFIGURED;
    bridge_today(&f, WEATHER_PROVIDER_OK, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_STALE, out.status);
    f = forecast_of(324);
    f.timezone = WEATHER_TZ_UNSPECIFIED;
    bridge_today(&f, WEATHER_PROVIDER_OK, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_STALE, out.status);
    f = forecast_of(324);
    f.validated = false;
    bridge_today(&f, WEATHER_PROVIDER_OK, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_STALE, out.status);

    /* untrusted NOW is always fail-safe, even for a perfect forecast */
    {
        WeatherLocalDate today = d(2026, 7, 15);
        f = forecast_of(324);
        weather_bridge_to_policy(&f, WEATHER_PROVIDER_OK,
                                 WEATHER_PROVIDER_OPEN_METEO, &today, NOW_S,
                                 false, MAX_AGE_S, &out);
        TEST_ASSERT_EQUAL(TUNING_FORECAST_STALE, out.status);
        TEST_ASSERT_EQUAL_INT16(0, out.forecast_max_dc);
    }
}

/* ================================================================== */
/* Current outdoor temperature is informational only                   */
/* ================================================================== */

TEST_CASE("bridge: the current outdoor temperature cannot change any decision",
          "[weather_bridge]")
{
    static const int16_t currents[] = { -600, -200, 0, 150, 280, 300, 450, 600 };
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    TuningForecastInput ref_in, in;
    TuningClimateRequest ref_req, req;
    TuningSelectionIntent ref_out, out;
    WeatherForecast f;
    size_t i;

    reg_validated(reg);

    /* Reference run A — the UPGRADE direction. A cool daily maximum from the
     * safest current profile is the only path in W1 that can move up, so it
     * is the strictest control for "the current reading changes nothing". */
    f = forecast_of(200); /* 20.0 C -> cool */
    f.current_valid = false;
    f.current_dc = 0;
    run_chain(&f, WEATHER_PROVIDER_OK, reg, TUNING_REGISTRY_GAMMA601_COUNT,
              TUNING_PROFILE_ID_EMERGENCY, &ref_in, &ref_req, &ref_out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_OK, ref_in.status);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_COOL_PROFILE, ref_req);
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, ref_out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_SUPERSINK_MAX, ref_out.profile_id);
    TEST_ASSERT_TRUE(ref_out.is_upgrade);

    /* Sweep the whole informational band — including readings far hotter
     * than every threshold: the policy input, the hysteresis request and the
     * full selection intent are identical every time. */
    for (i = 0; i < sizeof(currents) / sizeof(currents[0]); i++) {
        f = forecast_of(200);
        f.current_valid = true;
        f.current_dc = currents[i];
        run_chain(&f, WEATHER_PROVIDER_OK, reg, TUNING_REGISTRY_GAMMA601_COUNT,
                  TUNING_PROFILE_ID_EMERGENCY, &in, &req, &out);

        TEST_ASSERT_EQUAL_MEMORY(&ref_in, &in, sizeof(in));
        TEST_ASSERT_EQUAL(ref_req, req);
        TEST_ASSERT_EQUAL(ref_out.action, out.action);
        TEST_ASSERT_EQUAL_STRING(ref_out.profile_id, out.profile_id);
        TEST_ASSERT_EQUAL(ref_out.actor, out.actor);
        TEST_ASSERT_EQUAL(ref_out.winning_source, out.winning_source);
        TEST_ASSERT_EQUAL(ref_out.reason, out.reason);
        TEST_ASSERT_EQUAL(ref_out.secondary_reason, out.secondary_reason);
        TEST_ASSERT_EQUAL(ref_out.override_result, out.override_result);
        TEST_ASSERT_EQUAL(ref_out.is_upgrade, out.is_upgrade);
        TEST_ASSERT_EQUAL(ref_out.is_downgrade, out.is_downgrade);
    }

    /* Reference run B — the DOWNGRADE direction: a hot daily maximum from
     * the most aggressive profile. Again the current reading is irrelevant. */
    f = forecast_of(320); /* 32.0 C -> hot */
    f.current_valid = false;
    f.current_dc = 0;
    run_chain(&f, WEATHER_PROVIDER_OK, reg, TUNING_REGISTRY_GAMMA601_COUNT,
              TUNING_PROFILE_ID_SUPERSINK_MAX, &ref_in, &ref_req, &ref_out);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_HOT_PROFILE, ref_req);
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, ref_out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, ref_out.profile_id);
    TEST_ASSERT_TRUE(ref_out.is_downgrade);

    for (i = 0; i < sizeof(currents) / sizeof(currents[0]); i++) {
        f = forecast_of(320);
        f.current_valid = true;
        f.current_dc = currents[i];
        run_chain(&f, WEATHER_PROVIDER_OK, reg, TUNING_REGISTRY_GAMMA601_COUNT,
                  TUNING_PROFILE_ID_SUPERSINK_MAX, &in, &req, &out);
        TEST_ASSERT_EQUAL_MEMORY(&ref_in, &in, sizeof(in));
        TEST_ASSERT_EQUAL(ref_req, req);
        TEST_ASSERT_EQUAL(ref_out.action, out.action);
        TEST_ASSERT_EQUAL_STRING(ref_out.profile_id, out.profile_id);
        TEST_ASSERT_EQUAL(ref_out.is_upgrade, out.is_upgrade);
        TEST_ASSERT_EQUAL(ref_out.is_downgrade, out.is_downgrade);
    }

    /* A 60.0 C reading "right now" next to a 20.0 C daily maximum still
     * follows the maximum — the informational value never wins. */
    f = forecast_of(200);
    f.current_valid = true;
    f.current_dc = 600;
    run_chain(&f, WEATHER_PROVIDER_OK, reg, TUNING_REGISTRY_GAMMA601_COUNT,
              TUNING_PROFILE_ID_EMERGENCY, &in, &req, &out);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_COOL_PROFILE, req);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_SUPERSINK_MAX, out.profile_id);
}

/* ================================================================== */
/* Production profiles stay unselectable                               */
/* ================================================================== */

TEST_CASE("bridge: no weather input can select a production profile",
          "[weather_bridge]")
{
    size_t n = 0;
    const TuningProfile *prod = tuning_registry_gamma601(&n);
    TuningHardwareContext hw = hw_full();
    static const int16_t maxima[] = { -600, 0, 200, 279, 280, 299, 300, 350, 600 };
    size_t i;

    TEST_ASSERT_EQUAL_UINT32((uint32_t)TUNING_REGISTRY_GAMMA601_COUNT, (uint32_t)n);

    /* Every production descriptor is UNVALIDATED and payload-free. */
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL(TUNING_VALIDATION_UNVALIDATED, prod[i].validation);
        TEST_ASSERT_FALSE(prod[i].payload_present);
        TEST_ASSERT_EQUAL_UINT16(0, prod[i].frequency_mhz);
        TEST_ASSERT_EQUAL_UINT16(0, prod[i].core_voltage_mv);
        TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_NOT_VALIDATED,
                          tuning_profile_auto_eligible(&prod[i], &hw));
    }

    /* Sweep the whole forecast band through the real chain: not one input
     * produces a profile selection or an upgrade. */
    for (i = 0; i < sizeof(maxima) / sizeof(maxima[0]); i++) {
        TuningForecastInput in;
        TuningClimateRequest req;
        TuningSelectionIntent out;
        WeatherForecast f = forecast_of(maxima[i]);
        run_chain(&f, WEATHER_PROVIDER_OK, prod, n,
                  TUNING_PROFILE_ID_EMERGENCY, &in, &req, &out);
        TEST_ASSERT_EQUAL(TUNING_FORECAST_OK, in.status);
        TEST_ASSERT_NOT_EQUAL(TUNING_SELECT_PROFILE, out.action);
        TEST_ASSERT_FALSE(out.is_upgrade);
    }
}

/* ================================================================== */
/* Stale data can never authorize an upgrade                           */
/* ================================================================== */

TEST_CASE("bridge: stale, wrong-date and untrusted inputs never upgrade",
          "[weather_bridge]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    TuningForecastInput in;
    TuningClimateRequest req;
    TuningSelectionIntent out;
    WeatherForecast f;

    reg_validated(reg); /* the ONLY configuration in which upgrades exist */

    /* previous-day forecast carrying a hot maximum */
    f = forecast_of(320);
    f.local_date = d(2026, 7, 14);
    run_chain(&f, WEATHER_PROVIDER_OK, reg, TUNING_REGISTRY_GAMMA601_COUNT,
              TUNING_PROFILE_ID_EMERGENCY, &in, &req, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_STALE, in.status);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_FAIL_SAFE, req);
    TEST_ASSERT_FALSE(out.is_upgrade);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_WEATHER_FAIL_SAFE, out.winning_source);

    /* over-age fetch */
    f = forecast_of(320);
    f.fetch_epoch_s = NOW_S - (uint64_t)MAX_AGE_S - 1ull;
    run_chain(&f, WEATHER_PROVIDER_OK, reg, TUNING_REGISTRY_GAMMA601_COUNT,
              TUNING_PROFILE_ID_EMERGENCY, &in, &req, &out);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_FAIL_SAFE, req);
    TEST_ASSERT_FALSE(out.is_upgrade);

    /* transport failure with no forecast at all */
    run_chain(NULL, WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT, reg,
              TUNING_REGISTRY_GAMMA601_COUNT, TUNING_PROFILE_ID_EMERGENCY,
              &in, &req, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_UNAVAILABLE, in.status);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_FAIL_SAFE, req);
    TEST_ASSERT_FALSE(out.is_upgrade);

    /* From the most aggressive profile the same failure moves DOWN, never
     * up: weather unavailability is a safety direction. */
    run_chain(NULL, WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT, reg,
              TUNING_REGISTRY_GAMMA601_COUNT, TUNING_PROFILE_ID_SUPERSINK_MAX,
              &in, &req, &out);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_FAIL_SAFE, req);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_WEATHER_FAIL_SAFE, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_TRUE(out.is_downgrade);
    TEST_ASSERT_FALSE(out.is_upgrade);
}

/* ================================================================== */
/* Cache-sourced facts obey the same rule                              */
/* ================================================================== */

TEST_CASE("bridge: a cached forecast is bridged only inside its valid window",
          "[weather_bridge]")
{
    static WeatherForecastCache cache;
    WeatherLocalDate today = d(2026, 7, 15);
    WeatherLocalDate tomorrow = d(2026, 7, 16);
    WeatherForecast f = forecast_of(310);
    TuningForecastInput out;
    WeatherProviderResult r;

    weather_cache_init(&cache);
    TEST_ASSERT_TRUE(weather_cache_store(&cache, &f));

    /* inside the window: the cache serves the policy */
    r = weather_cache_usable(&cache, WEATHER_PROVIDER_OPEN_METEO, &today, NOW_S,
                             true, MAX_AGE_S);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, r);
    weather_bridge_to_policy(&cache.forecast, r, WEATHER_PROVIDER_OPEN_METEO,
                             &today, NOW_S, true, MAX_AGE_S, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_OK, out.status);
    TEST_ASSERT_EQUAL_INT16(310, out.forecast_max_dc);

    /* the next local day: the same entry is stale and carries no value */
    r = weather_cache_usable(&cache, WEATHER_PROVIDER_OPEN_METEO, &tomorrow,
                             NOW_S + 86400ull, true, MAX_AGE_S);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CACHE_STALE, r);
    weather_bridge_to_policy(&cache.forecast, r, WEATHER_PROVIDER_OPEN_METEO,
                             &tomorrow, NOW_S + 86400ull, true, MAX_AGE_S, &out);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_STALE, out.status);
    TEST_ASSERT_EQUAL_INT16(0, out.forecast_max_dc);
}
