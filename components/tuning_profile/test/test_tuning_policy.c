/*
 * Exhaustive deterministic tests for the pure climate policy, sensor-health
 * model, manual-override eligibility and precedence arbitration (W1).
 * No hardware, no network, no clocks — all times are synthetic inputs.
 */

#include <string.h>
#include "unity.h"
#include "tuning_policy.h"

/* ---------------- fixtures ---------------- */

#define NOW_S 1800000000ull /* synthetic trusted epoch (2027) */

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

/* SYNTHETIC TEST REGISTRY: the production gamma601 trio promoted to
 * VALIDATED with synthetic evidence tags so the successful selection paths
 * can be exercised. In production ALL THREE ship UNVALIDATED and are never
 * auto-selectable (proven in test_tuning_profile.c and in the production
 * sweep below) — this promotion exists only inside tests. */
static void reg_validated(TuningProfile out[TUNING_REGISTRY_GAMMA601_COUNT])
{
    size_t n = 0;
    const TuningProfile *r = tuning_registry_gamma601(&n);
    memcpy(out, r, sizeof(TuningProfile) * TUNING_REGISTRY_GAMMA601_COUNT);
    for (size_t i = 0; i < TUNING_REGISTRY_GAMMA601_COUNT; i++) {
        out[i].validation = TUNING_VALIDATION_VALIDATED;
        if (out[i].evidence_fingerprint == 0) {
            out[i].evidence_fingerprint = 0xE0F0u + (uint32_t)i;
        }
    }
}

static TuningPolicyEnvironment env_of(const TuningProfile *p, size_t n)
{
    TuningPolicyEnvironment e;
    memset(&e, 0, sizeof(e));
    e.profiles = p;
    e.profile_count = n;
    e.hw = hw_full();
    return e;
}

static TuningSensorHealth sensors_ok(void)
{
    TuningSensorHealth s;
    memset(&s, 0, sizeof(s));
    s.asic_temp = TUNING_SENSOR_OK;
    s.vrm_temp = TUNING_SENSOR_OK;
    s.vrm_expected = true;
    s.fan_tach = TUNING_SENSOR_OK;
    s.fan_control_uncertain = false;
    return s;
}

/* Baseline input: healthy device on `current`, trusted time, climate RETAIN. */
static TuningPolicyInput base_in(const char *current)
{
    TuningPolicyInput in;
    memset(&in, 0, sizeof(in));
    in.policy_generation = 7;
    if (current != NULL) {
        strncpy(in.current_profile_id, current, sizeof(in.current_profile_id) - 1);
        in.current_profile_known = true;
    }
    strncpy(in.roles.cool_day_profile_id, TUNING_PROFILE_ID_SUPERSINK_MAX,
            sizeof(in.roles.cool_day_profile_id) - 1);
    strncpy(in.roles.hot_day_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(in.roles.hot_day_profile_id) - 1);
    strncpy(in.roles.emergency_profile_id, TUNING_PROFILE_ID_EMERGENCY,
            sizeof(in.roles.emergency_profile_id) - 1);
    in.sensors = sensors_ok();
    in.trusted_time_valid = true;
    in.climate_request = TUNING_CLIMATE_REQ_RETAIN;
    in.mining = TUNING_MINING_STABLE;
    in.now_epoch_s = NOW_S;
    return in;
}

static TuningManualOverride ov_valid(const char *id)
{
    TuningManualOverride ov;
    memset(&ov, 0, sizeof(ov));
    ov.active = true;
    strncpy(ov.profile_id, id, sizeof(ov.profile_id) - 1);
    ov.actor = TUNING_ACTOR_MANUAL_API;
    ov.created_epoch_s = NOW_S - 100;
    ov.expires_epoch_s = NOW_S + 3600;
    ov.reason_code = 1;
    ov.policy_generation = 7;
    return ov;
}

static TuningClimateThresholds th_default(void)
{
    TuningClimateThresholds t;
    tuning_climate_thresholds_defaults(&t);
    return t;
}

static TuningClimateState st_of(TuningWeatherState s)
{
    TuningClimateState c;
    tuning_climate_state_init(&c);
    c.state = s;
    return c;
}

static TuningForecastInput fc_ok(int16_t dc)
{
    TuningForecastInput f;
    memset(&f, 0, sizeof(f));
    f.status = TUNING_FORECAST_OK;
    f.forecast_max_dc = dc;
    return f;
}

/* ---------------- climate thresholds + init ---------------- */

TEST_CASE("climate: defaults are 30.0/28.0 C and valid", "[tuning_policy]")
{
    TuningClimateThresholds t = th_default();
    TEST_ASSERT_EQUAL_INT16(300, t.hot_threshold_dc);
    TEST_ASSERT_EQUAL_INT16(280, t.cool_threshold_dc);
    TEST_ASSERT_TRUE(tuning_climate_thresholds_valid(&t));
}

TEST_CASE("climate: threshold ordering and band are enforced", "[tuning_policy]")
{
    TEST_ASSERT_FALSE(tuning_climate_thresholds_valid(NULL));
    TuningClimateThresholds t = { .hot_threshold_dc = 280, .cool_threshold_dc = 280 };
    TEST_ASSERT_FALSE(tuning_climate_thresholds_valid(&t)); /* hot == cool */
    t.hot_threshold_dc = 270;
    TEST_ASSERT_FALSE(tuning_climate_thresholds_valid(&t)); /* hot < cool */
    t = th_default();
    t.hot_threshold_dc = TUNING_FORECAST_SANITY_MAX_DC + 1;
    TEST_ASSERT_FALSE(tuning_climate_thresholds_valid(&t));
    t = th_default();
    t.cool_threshold_dc = TUNING_FORECAST_SANITY_MIN_DC - 1;
    TEST_ASSERT_FALSE(tuning_climate_thresholds_valid(&t));
}

TEST_CASE("climate: init is UNKNOWN with zero transitions", "[tuning_policy]")
{
    TuningClimateState c;
    memset(&c, 0xFF, sizeof(c));
    tuning_climate_state_init(&c);
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_UNKNOWN, c.state);
    TEST_ASSERT_EQUAL_UINT32(0, c.transition_count);
    TEST_ASSERT_FALSE(c.last_forecast_valid);
}

/* ---------------- climate step ---------------- */

TEST_CASE("climate step: contract errors leave outputs untouched", "[tuning_policy]")
{
    TuningClimateThresholds t = th_default();
    TuningClimateState cur = st_of(TUNING_WEATHER_STATE_NORMAL);
    TuningForecastInput f = fc_ok(290);
    TuningClimateState next;
    TuningClimateRequest req = (TuningClimateRequest)77;
    memset(&next, 0xAB, sizeof(next));

    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_NULL,
                      tuning_climate_step(NULL, &t, &f, &next, &req));
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_NULL,
                      tuning_climate_step(&cur, NULL, &f, &next, &req));
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_NULL,
                      tuning_climate_step(&cur, &t, NULL, &next, &req));
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_NULL,
                      tuning_climate_step(&cur, &t, &f, NULL, &req));
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_NULL,
                      tuning_climate_step(&cur, &t, &f, &next, NULL));

    TuningClimateThresholds bad = { .hot_threshold_dc = 100, .cool_threshold_dc = 200 };
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_THRESHOLDS,
                      tuning_climate_step(&cur, &bad, &f, &next, &req));

    TuningClimateState badstate = cur;
    badstate.state = (TuningWeatherState)TUNING_WEATHER_STATE__COUNT;
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_ENUM_RANGE,
                      tuning_climate_step(&badstate, &t, &f, &next, &req));

    TuningForecastInput badf = f;
    badf.status = (TuningForecastStatus)TUNING_FORECAST__COUNT;
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_ENUM_RANGE,
                      tuning_climate_step(&cur, &t, &badf, &next, &req));

    /* outputs untouched across all error paths */
    TEST_ASSERT_EQUAL(77, (int)req);
    uint8_t probe[sizeof(next)];
    memset(probe, 0xAB, sizeof(probe));
    TEST_ASSERT_EQUAL_MEMORY(probe, &next, sizeof(next));
}

TEST_CASE("climate step: entering HOT at exactly the hot threshold", "[tuning_policy]")
{
    TuningClimateThresholds t = th_default();
    TuningClimateState cur = st_of(TUNING_WEATHER_STATE_UNKNOWN);
    TuningClimateState next;
    TuningClimateRequest req;
    TuningForecastInput f = fc_ok(300); /* 30.0 C */
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK,
                      tuning_climate_step(&cur, &t, &f, &next, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_HOT, next.state);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_HOT_PROFILE, req);
    TEST_ASSERT_EQUAL_UINT32(1, next.transition_count);
    TEST_ASSERT_TRUE(next.last_forecast_valid);
    TEST_ASSERT_EQUAL_INT16(300, next.last_forecast_max_dc);
}

TEST_CASE("climate step: cool at exactly the cool threshold", "[tuning_policy]")
{
    TuningClimateThresholds t = th_default();
    TuningClimateState cur = st_of(TUNING_WEATHER_STATE_UNKNOWN);
    TuningClimateState next;
    TuningClimateRequest req;
    TuningForecastInput f = fc_ok(280); /* 28.0 C */
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK,
                      tuning_climate_step(&cur, &t, &f, &next, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_NORMAL, next.state);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_COOL_PROFILE, req);
}

TEST_CASE("climate step: dead band retains stance (UNKNOWN stays undecided)", "[tuning_policy]")
{
    TuningClimateThresholds t = th_default();
    TuningClimateState next;
    TuningClimateRequest req;

    TuningClimateState cur = st_of(TUNING_WEATHER_STATE_UNKNOWN);
    TuningForecastInput f = fc_ok(290); /* 29.0 C — inside the band */
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK,
                      tuning_climate_step(&cur, &t, &f, &next, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_UNKNOWN, next.state);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_RETAIN, req);
    TEST_ASSERT_EQUAL_UINT32(0, next.transition_count);

    cur = st_of(TUNING_WEATHER_STATE_NORMAL);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK,
                      tuning_climate_step(&cur, &t, &f, &next, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_NORMAL, next.state);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_COOL_PROFILE, req);

    cur = st_of(TUNING_WEATHER_STATE_HOT);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK,
                      tuning_climate_step(&cur, &t, &f, &next, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_HOT, next.state);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_HOT_PROFILE, req);
}

TEST_CASE("climate step: hysteresis — HOT holds through 29.9, exits at 28.0", "[tuning_policy]")
{
    TuningClimateThresholds t = th_default();
    TuningClimateState s = st_of(TUNING_WEATHER_STATE_HOT);
    TuningClimateRequest req;

    TuningForecastInput f = fc_ok(299);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_HOT, s.state);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_HOT_PROFILE, req);

    f = fc_ok(281);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_HOT, s.state); /* still hot */

    f = fc_ok(280);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_NORMAL, s.state);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_COOL_PROFILE, req);
    TEST_ASSERT_EQUAL_UINT32(1, s.transition_count);

    /* NORMAL re-enters HOT only at >= 300 */
    f = fc_ok(299);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_NORMAL, s.state);
    f = fc_ok(300);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_HOT, s.state);
    TEST_ASSERT_EQUAL_UINT32(2, s.transition_count);
}

TEST_CASE("climate step: repeated identical forecasts are idempotent", "[tuning_policy]")
{
    TuningClimateThresholds t = th_default();
    TuningClimateState s = st_of(TUNING_WEATHER_STATE_UNKNOWN);
    TuningClimateRequest req;
    TuningForecastInput f = fc_ok(324);

    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TuningClimateState after_first = s;
    TuningClimateRequest req1 = req;
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
        TEST_ASSERT_EQUAL(req1, req);
        TEST_ASSERT_EQUAL_MEMORY(&after_first, &s, sizeof(s));
    }
}

TEST_CASE("climate step: unusable forecast fail-safes without state change", "[tuning_policy]")
{
    TuningClimateThresholds t = th_default();
    TuningClimateRequest req;
    TuningForecastStatus bads[] = { TUNING_FORECAST_STALE, TUNING_FORECAST_INVALID,
                                    TUNING_FORECAST_UNAVAILABLE };

    for (size_t i = 0; i < sizeof(bads) / sizeof(bads[0]); i++) {
        TuningClimateState s = st_of(TUNING_WEATHER_STATE_HOT);
        s.last_forecast_valid = true;
        TuningForecastInput f = { .status = bads[i], .forecast_max_dc = 100 };
        TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
        TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_HOT, s.state); /* unchanged */
        TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_FAIL_SAFE, req);
        TEST_ASSERT_FALSE(s.last_forecast_valid);
        TEST_ASSERT_EQUAL_UINT32(0, s.transition_count);
    }

    /* Status OK but outside the sanity band is equally unusable. */
    TuningClimateState s = st_of(TUNING_WEATHER_STATE_NORMAL);
    TuningForecastInput f = fc_ok(TUNING_FORECAST_SANITY_MAX_DC + 1);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_FAIL_SAFE, req);
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_NORMAL, s.state);
    f = fc_ok(TUNING_FORECAST_SANITY_MIN_DC - 1);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_FAIL_SAFE, req);
}

TEST_CASE("climate step: transition counter saturates", "[tuning_policy]")
{
    TuningClimateThresholds t = th_default();
    TuningClimateState s = st_of(TUNING_WEATHER_STATE_NORMAL);
    s.transition_count = UINT32_MAX;
    TuningClimateRequest req;
    TuningForecastInput f = fc_ok(350);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_HOT, s.state);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.transition_count);
}

/* ---------------- sensor classifiers ---------------- */

TEST_CASE("classify asic: -1 sentinel and faults are never OK", "[tuning_policy]")
{
    TEST_ASSERT_EQUAL(TUNING_SENSOR_INVALID, tuning_classify_asic_temp_dc(600, true));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_INVALID, tuning_classify_asic_temp_dc(-10, false));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_INVALID, tuning_classify_asic_temp_dc(0, false));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_OK, tuning_classify_asic_temp_dc(600, false));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_OK,
                      tuning_classify_asic_temp_dc(TUNING_ASIC_TEMP_PLAUSIBLE_MAX_DC, false));
    /* EMC2101 diode-fault codes decode to ~127.0 C = 1270 dc. */
    TEST_ASSERT_EQUAL(TUNING_SENSOR_IMPLAUSIBLE,
                      tuning_classify_asic_temp_dc(1270, false));
}

TEST_CASE("classify vrm: cached-zero, implausible and frozen readings", "[tuning_policy]")
{
    TEST_ASSERT_EQUAL(TUNING_SENSOR_INVALID, tuning_classify_vrm_temp_dc(450, true, 0, 0));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_INVALID, tuning_classify_vrm_temp_dc(0, false, 0, 0));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_IMPLAUSIBLE,
                      tuning_classify_vrm_temp_dc(TUNING_VRM_TEMP_PLAUSIBLE_MAX_DC + 1,
                                                  false, 0, 0));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_OK, tuning_classify_vrm_temp_dc(450, false, 3, 0));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_OK, tuning_classify_vrm_temp_dc(450, false, 3, 10));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_STALE, tuning_classify_vrm_temp_dc(450, false, 10, 10));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_STALE, tuning_classify_vrm_temp_dc(450, false, 99, 10));
}

/* ---------------- sensor health predicates ---------------- */

TEST_CASE("sensor health: upgrade gate fails closed", "[tuning_policy]")
{
    TEST_ASSERT_FALSE(tuning_sensor_health_upgrade_ok(NULL));
    TuningSensorHealth s = sensors_ok();
    TEST_ASSERT_TRUE(tuning_sensor_health_upgrade_ok(&s));

    s = sensors_ok();
    s.asic_temp = TUNING_SENSOR_INVALID;
    TEST_ASSERT_FALSE(tuning_sensor_health_upgrade_ok(&s));
    s = sensors_ok();
    s.vrm_temp = TUNING_SENSOR_STALE;
    TEST_ASSERT_FALSE(tuning_sensor_health_upgrade_ok(&s));
    s = sensors_ok();
    s.vrm_temp = TUNING_SENSOR_STALE;
    s.vrm_expected = false; /* board without VRM sensor: not required */
    TEST_ASSERT_TRUE(tuning_sensor_health_upgrade_ok(&s));
    s = sensors_ok();
    s.fan_tach = TUNING_SENSOR_STALE;
    TEST_ASSERT_FALSE(tuning_sensor_health_upgrade_ok(&s));
    s = sensors_ok();
    s.fan_control_uncertain = true;
    TEST_ASSERT_FALSE(tuning_sensor_health_upgrade_ok(&s));
    s = sensors_ok();
    s.asic_temp = (TuningSensorStatus)TUNING_SENSOR__COUNT;
    TEST_ASSERT_FALSE(tuning_sensor_health_upgrade_ok(&s));
}

TEST_CASE("sensor health: integrity failure definition", "[tuning_policy]")
{
    TEST_ASSERT_TRUE(tuning_sensor_health_integrity_failed(NULL));
    TuningSensorHealth s = sensors_ok();
    TEST_ASSERT_FALSE(tuning_sensor_health_integrity_failed(&s));

    s = sensors_ok();
    s.asic_temp = TUNING_SENSOR_INVALID;
    TEST_ASSERT_TRUE(tuning_sensor_health_integrity_failed(&s));
    s = sensors_ok();
    s.asic_temp = TUNING_SENSOR_MISSING;
    TEST_ASSERT_TRUE(tuning_sensor_health_integrity_failed(&s));
    s = sensors_ok();
    s.asic_temp = TUNING_SENSOR_IMPLAUSIBLE;
    TEST_ASSERT_TRUE(tuning_sensor_health_integrity_failed(&s));

    s = sensors_ok();
    s.vrm_temp = TUNING_SENSOR_INVALID;
    TEST_ASSERT_TRUE(tuning_sensor_health_integrity_failed(&s));
    s.vrm_expected = false;
    TEST_ASSERT_FALSE(tuning_sensor_health_integrity_failed(&s));

    /* Tach-only loss blocks upgrades but is NOT an integrity failure. */
    s = sensors_ok();
    s.fan_tach = TUNING_SENSOR_STALE;
    TEST_ASSERT_FALSE(tuning_sensor_health_integrity_failed(&s));

    s = sensors_ok();
    s.fan_control_uncertain = true;
    TEST_ASSERT_TRUE(tuning_sensor_health_integrity_failed(&s));

    s = sensors_ok();
    s.vrm_temp = (TuningSensorStatus)99;
    TEST_ASSERT_TRUE(tuning_sensor_health_integrity_failed(&s));
}

/* ---------------- manual override eligibility ---------------- */

TEST_CASE("override: rejection matrix", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningHardwareContext hw = hw_full();
    const TuningProfile *hot =
        tuning_registry_find(reg, TUNING_REGISTRY_GAMMA601_COUNT,
                             TUNING_PROFILE_ID_HOT_WEATHER);
    TEST_ASSERT_NOT_NULL(hot);

    TuningManualOverride ov = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);

    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_NULL,
                      tuning_override_eligible(NULL, hot, &hw, 2, true, 2, true,
                                               false, NOW_S, true));
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_NULL,
                      tuning_override_eligible(&ov, hot, NULL, 2, true, 2, true,
                                               false, NOW_S, true));

    ov.active = false;
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_INACTIVE,
                      tuning_override_eligible(&ov, hot, &hw, 2, true, 2, true,
                                               false, NOW_S, true));

    ov = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    ov.actor = TUNING_ACTOR_THERMAL;
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_ACTOR,
                      tuning_override_eligible(&ov, hot, &hw, 2, true, 2, true,
                                               false, NOW_S, true));

    ov = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    ov.expires_epoch_s = 0;
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_NO_EXPIRATION,
                      tuning_override_eligible(&ov, hot, &hw, 2, true, 2, true,
                                               false, NOW_S, true));

    ov = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    ov.expires_epoch_s = ov.created_epoch_s;
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_EXPIRATION_ORDER,
                      tuning_override_eligible(&ov, hot, &hw, 2, true, 2, true,
                                               false, NOW_S, true));

    ov = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_TIME_UNTRUSTED,
                      tuning_override_eligible(&ov, hot, &hw, 2, true, 2, true,
                                               false, NOW_S, false));
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_EXPIRED,
                      tuning_override_eligible(&ov, hot, &hw, 2, true, 2, true,
                                               false, ov.expires_epoch_s, true));

    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_PROFILE_UNKNOWN,
                      tuning_override_eligible(&ov, NULL, &hw, 2, true, 2, true,
                                               false, NOW_S, true));

    /* Production (UNVALIDATED) hot profile is never overridable. */
    size_t n = 0;
    const TuningProfile *prod = tuning_registry_gamma601(&n);
    const TuningProfile *prod_hot =
        tuning_registry_find(prod, n, TUNING_PROFILE_ID_HOT_WEATHER);
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_NOT_ELIGIBLE,
                      tuning_override_eligible(&ov, prod_hot, &hw, 2, true, 2, true,
                                               false, NOW_S, true));

    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_CEILING_UNKNOWN,
                      tuning_override_eligible(&ov, hot, &hw, 0, false, 2, true,
                                               false, NOW_S, true));

    /* rank 1 above ceiling 0 */
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_ABOVE_CEILING,
                      tuning_override_eligible(&ov, hot, &hw, 0, true, 2, true,
                                               false, NOW_S, true));

    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_OK,
                      tuning_override_eligible(&ov, hot, &hw, 2, true, 2, true,
                                               false, NOW_S, true));
}

TEST_CASE("override: cooldown blocks upgrades only", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningHardwareContext hw = hw_full();
    const TuningProfile *ss =
        tuning_registry_find(reg, TUNING_REGISTRY_GAMMA601_COUNT,
                             TUNING_PROFILE_ID_SUPERSINK_MAX);
    const TuningProfile *hot =
        tuning_registry_find(reg, TUNING_REGISTRY_GAMMA601_COUNT,
                             TUNING_PROFILE_ID_HOT_WEATHER);
    TuningManualOverride ov = ov_valid(TUNING_PROFILE_ID_SUPERSINK_MAX);

    /* Upgrade (rank 2 from current 1) during cooldown: rejected. */
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_COOLDOWN_UPGRADE,
                      tuning_override_eligible(&ov, ss, &hw, 2, true, 1, true,
                                               true, NOW_S, true));
    /* Same request without cooldown: OK. */
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_OK,
                      tuning_override_eligible(&ov, ss, &hw, 2, true, 1, true,
                                               false, NOW_S, true));
    /* Safer request (rank 1 from current 2) during cooldown: OK. */
    ov = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_OK,
                      tuning_override_eligible(&ov, hot, &hw, 2, true, 2, true,
                                               true, NOW_S, true));
    /* Unknown current rank counts as an upgrade (fail closed). */
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_COOLDOWN_UPGRADE,
                      tuning_override_eligible(&ov, hot, &hw, 2, true, 0, false,
                                               true, NOW_S, true));
}

/* ---------------- arbitration: contract ---------------- */

TEST_CASE("evaluate: contract errors", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    TuningSelectionIntent out;

    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_NULL, tuning_policy_evaluate(NULL, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_NULL, tuning_policy_evaluate(&env, NULL, &out));
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_NULL, tuning_policy_evaluate(&env, &in, NULL));

    TuningPolicyEnvironment bad = env;
    bad.profiles = NULL;
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_ENVIRONMENT,
                      tuning_policy_evaluate(&bad, &in, &out));
    bad = env;
    bad.profile_count = 0;
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_ENVIRONMENT,
                      tuning_policy_evaluate(&bad, &in, &out));

    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = (TuningClimateRequest)TUNING_CLIMATE_REQ__COUNT;
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_ENUM_RANGE,
                      tuning_policy_evaluate(&env, &in, &out));
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.mining = (TuningMiningHealth)TUNING_MINING__COUNT;
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_ENUM_RANGE,
                      tuning_policy_evaluate(&env, &in, &out));
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.sensors.fan_tach = (TuningSensorStatus)TUNING_SENSOR__COUNT;
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_ENUM_RANGE,
                      tuning_policy_evaluate(&env, &in, &out));
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.manual_present = true;
    in.manual = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    in.manual.actor = (TuningActorClass)TUNING_ACTOR__COUNT;
    TEST_ASSERT_EQUAL(TUNING_POLICY_ERR_ENUM_RANGE,
                      tuning_policy_evaluate(&env, &in, &out));
}

/* ---------------- arbitration: precedence 1 (emergency) ---------------- */

TEST_CASE("evaluate: emergency selects the emergency profile (downgrade)", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.emergency_thermal_active = true;
    TuningSelectionIntent out;

    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_EMERGENCY, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_ACTOR_THERMAL, out.actor);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_EMERGENCY_THERMAL, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_REASON_EMERGENCY_THERMAL_ACTIVE, out.reason);
    TEST_ASSERT_TRUE(out.is_downgrade);
    TEST_ASSERT_FALSE(out.is_upgrade);
    TEST_ASSERT_EQUAL_UINT32(7, out.policy_generation);
}

TEST_CASE("evaluate: emergency already active retains", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_EMERGENCY);
    in.emergency_thermal_active = true;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_ALREADY_ACTIVE, out.secondary_reason);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_EMERGENCY_THERMAL, out.winning_source);
}

TEST_CASE("evaluate: emergency with no eligible emergency profile surfaces operator", "[tuning_policy]")
{
    /* Production registry: emergency role is UNVALIDATED -> never selected. */
    size_t n = 0;
    const TuningProfile *prod = tuning_registry_gamma601(&n);
    TuningPolicyEnvironment env = env_of(prod, n);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.emergency_thermal_active = true;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_OPERATOR_RECOVERY, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_EMERGENCY_THERMAL_ACTIVE, out.reason);
    TEST_ASSERT_EQUAL(TUNING_REASON_NO_VALID_EMERGENCY_PROFILE, out.secondary_reason);
    TEST_ASSERT_EQUAL_STRING("", out.profile_id);
}

TEST_CASE("evaluate: emergency beats every lower source", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.emergency_thermal_active = true;
    in.stability_rollback_active = true;
    strncpy(in.stability_rollback_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(in.stability_rollback_profile_id) - 1);
    in.sensors.asic_temp = TUNING_SENSOR_INVALID;
    in.trusted_time_valid = false;
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    in.manual_present = true;
    in.manual = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SOURCE_EMERGENCY_THERMAL, out.winning_source);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_EMERGENCY, out.profile_id);
}

/* ---------------- arbitration: precedence 2 (stability) ---------------- */

TEST_CASE("evaluate: stability rollback selects the recorded profile", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.stability_rollback_active = true;
    strncpy(in.stability_rollback_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(in.stability_rollback_profile_id) - 1);
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_ACTOR_STABILITY, out.actor);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_STABILITY_ROLLBACK, out.winning_source);
    TEST_ASSERT_TRUE(out.is_downgrade);
}

TEST_CASE("evaluate: stability with no usable rollback retains + inhibits", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningSelectionIntent out;

    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.stability_rollback_active = true; /* empty rollback id */
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_RETAIN_INHIBIT, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_STABILITY_ROLLBACK_ACTIVE, out.reason);
    TEST_ASSERT_EQUAL(TUNING_REASON_NO_VALID_ROLLBACK_PROFILE, out.secondary_reason);

    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.stability_rollback_active = true;
    strncpy(in.stability_rollback_profile_id, "no-such-profile",
            sizeof(in.stability_rollback_profile_id) - 1);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_RETAIN_INHIBIT, out.action);
}

TEST_CASE("evaluate: stability beats sensors/fail-safe/manual/weather", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.stability_rollback_active = true;
    strncpy(in.stability_rollback_profile_id, TUNING_PROFILE_ID_EMERGENCY,
            sizeof(in.stability_rollback_profile_id) - 1);
    in.sensors.asic_temp = TUNING_SENSOR_INVALID;
    in.trusted_time_valid = false;
    in.climate_request = TUNING_CLIMATE_REQ_HOT_PROFILE;
    in.manual_present = true;
    in.manual = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SOURCE_STABILITY_ROLLBACK, out.winning_source);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_EMERGENCY, out.profile_id);
}

/* ---------------- arbitration: precedence 3 (sensors) ---------------- */

TEST_CASE("evaluate: sensor failure downgrades to the hot-day profile", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.sensors.asic_temp = TUNING_SENSOR_INVALID;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_ACTOR_SENSOR_SAFETY, out.actor);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_SENSOR_INTEGRITY, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_REASON_SENSOR_INTEGRITY_FAILED, out.reason);
    TEST_ASSERT_TRUE(out.is_downgrade);
}

TEST_CASE("evaluate: sensor failure never upgrades an already-safer device", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_EMERGENCY); /* rank 0 */
    in.sensors.vrm_temp = TUNING_SENSOR_STALE;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    /* hot-day is rank 1 > current 0: retained, never an upgrade */
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_RETAINED_SAFER_CURRENT, out.secondary_reason);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_SENSOR_INTEGRITY, out.winning_source);
}

TEST_CASE("evaluate: sensor failure escalates to emergency when hot role invalid", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    /* Break the hot role: point it at a nonexistent id. */
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    memset(in.roles.hot_day_profile_id, 0, sizeof(in.roles.hot_day_profile_id));
    strncpy(in.roles.hot_day_profile_id, "missing", sizeof(in.roles.hot_day_profile_id) - 1);
    in.sensors.fan_control_uncertain = true;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_EMERGENCY, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_REASON_NO_VALID_HOT_PROFILE, out.secondary_reason);

    /* Neither role usable: retain + inhibit. */
    memset(in.roles.emergency_profile_id, 0, sizeof(in.roles.emergency_profile_id));
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_RETAIN_INHIBIT, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_SENSOR_INTEGRITY_FAILED, out.reason);
    TEST_ASSERT_EQUAL(TUNING_REASON_NO_VALID_HOT_PROFILE, out.secondary_reason);
}

/* ---------------- arbitration: precedence 4 (fail-safe) ---------------- */

TEST_CASE("evaluate: untrusted time forces the hot-day stance", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.trusted_time_valid = false;
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE; /* ignored */
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_ACTOR_WEATHER_FAIL_SAFE, out.actor);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_WEATHER_FAIL_SAFE, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_REASON_TIME_UNTRUSTED, out.reason);
}

TEST_CASE("evaluate: forecast fail-safe requests hot; retains emergency", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningSelectionIntent out;

    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_FAIL_SAFE;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_REASON_WEATHER_FAIL_SAFE, out.reason);

    /* Already on Emergency: retain Emergency (task-brief rule). */
    in = base_in(TUNING_PROFILE_ID_EMERGENCY);
    in.climate_request = TUNING_CLIMATE_REQ_FAIL_SAFE;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_RETAINED_SAFER_CURRENT, out.secondary_reason);
}

TEST_CASE("evaluate: fail-safe without a valid hot profile surfaces operator", "[tuning_policy]")
{
    /* Production registry: hot role UNVALIDATED -> never fall back to
     * SuperSink Max; retain + operator recovery instead. */
    size_t n = 0;
    const TuningProfile *prod = tuning_registry_gamma601(&n);
    TuningPolicyEnvironment env = env_of(prod, n);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_FAIL_SAFE;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_OPERATOR_RECOVERY, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_WEATHER_FAIL_SAFE, out.reason);
    TEST_ASSERT_EQUAL(TUNING_REASON_NO_VALID_HOT_PROFILE, out.secondary_reason);
    TEST_ASSERT_EQUAL_STRING("", out.profile_id); /* never SuperSink Max */
}

/* ---------------- arbitration: precedence 5 (manual override) ---------------- */

TEST_CASE("evaluate: valid override within ceiling wins", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE; /* ceiling = rank 2 */
    in.manual_present = true;
    in.manual = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_ACTOR_MANUAL_API, out.actor);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_MANUAL_OVERRIDE, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_REASON_OVERRIDE_APPLIED, out.reason);
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_OK, out.override_result);
    TEST_ASSERT_TRUE(out.is_downgrade);
}

TEST_CASE("evaluate: override above the hot-stance ceiling is rejected", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_HOT_PROFILE; /* ceiling = rank 1 */
    in.manual_present = true;
    in.manual = ov_valid(TUNING_PROFILE_ID_SUPERSINK_MAX); /* rank 2 */
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    /* Override rejected; the weather HOT stance decides instead. */
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_ABOVE_CEILING, out.override_result);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_WEATHER_POLICY, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, out.profile_id);
}

TEST_CASE("evaluate: expired override falls through with its rejection recorded", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    in.manual_present = true;
    in.manual = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    in.manual.expires_epoch_s = NOW_S - 1;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_EXPIRED, out.override_result);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_WEATHER_POLICY, out.winning_source);
    /* COOL + already on cool profile -> retain */
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_ALREADY_ACTIVE, out.secondary_reason);
}

TEST_CASE("evaluate: override to the current profile is a no-op", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    in.manual_present = true;
    in.manual = ov_valid(TUNING_PROFILE_ID_SUPERSINK_MAX);
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_OK, out.override_result);
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_OVERRIDE_APPLIED, out.reason);
    TEST_ASSERT_EQUAL(TUNING_REASON_ALREADY_ACTIVE, out.secondary_reason);
}

/* ---------------- arbitration: precedence 6 (weather) ---------------- */

TEST_CASE("evaluate: hot forecast downgrades to hot-day profile", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_HOT_PROFILE;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_ACTOR_WEATHER_POLICY, out.actor);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_WEATHER_POLICY, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_REASON_WEATHER_HOT_FORECAST, out.reason);

    /* Already on hot profile: retain. */
    in = base_in(TUNING_PROFILE_ID_HOT_WEATHER);
    in.climate_request = TUNING_CLIMATE_REQ_HOT_PROFILE;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_ALREADY_ACTIVE, out.secondary_reason);

    /* On emergency: hot request is not a downgrade -> retain emergency. */
    in = base_in(TUNING_PROFILE_ID_EMERGENCY);
    in.climate_request = TUNING_CLIMATE_REQ_HOT_PROFILE;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_RETAINED_SAFER_CURRENT, out.secondary_reason);
}

TEST_CASE("evaluate: cool upgrade happy path", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_HOT_WEATHER);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_SUPERSINK_MAX, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_WEATHER_POLICY, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_REASON_WEATHER_COOL_ELIGIBLE, out.reason);
    TEST_ASSERT_TRUE(out.is_upgrade);
    TEST_ASSERT_FALSE(out.is_downgrade);
}

TEST_CASE("evaluate: every upgrade gate blocks and is named", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningSelectionIntent out;

    /* sensors */
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_HOT_WEATHER);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    in.sensors.fan_tach = TUNING_SENSOR_STALE; /* upgrade-blocking, not integrity */
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_SENSORS_NOT_UPGRADE_OK, out.secondary_reason);

    /* cooldown */
    in = base_in(TUNING_PROFILE_ID_HOT_WEATHER);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    in.cooldown.active = true;
    in.cooldown.source = TUNING_ACTOR_THERMAL;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_COOLDOWN_ACTIVE, out.secondary_reason);

    /* mining health (UNKNOWN and DEGRADED both fail closed) */
    in = base_in(TUNING_PROFILE_ID_HOT_WEATHER);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    in.mining = TUNING_MINING_UNKNOWN;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_REASON_MINING_NOT_STABLE, out.secondary_reason);
    in.mining = TUNING_MINING_DEGRADED;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_REASON_MINING_NOT_STABLE, out.secondary_reason);

    /* external inhibit */
    in = base_in(TUNING_PROFILE_ID_HOT_WEATHER);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    in.upgrade_inhibited = true;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_REASON_UPGRADE_INHIBITED, out.secondary_reason);

    /* unknown current */
    in = base_in(NULL);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_CURRENT_UNKNOWN, out.secondary_reason);

    /* ineligible cool role (production registry has hot/emergency
     * unvalidated, but cool=supersink is validated there; use a bogus id) */
    in = base_in(TUNING_PROFILE_ID_HOT_WEATHER);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    memset(in.roles.cool_day_profile_id, 0, sizeof(in.roles.cool_day_profile_id));
    strncpy(in.roles.cool_day_profile_id, "missing", sizeof(in.roles.cool_day_profile_id) - 1);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_PROFILE_NOT_ELIGIBLE, out.secondary_reason);

    /* already on the cool profile */
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_ALREADY_ACTIVE, out.secondary_reason);
}

TEST_CASE("evaluate: cool role milder than current is an immediate downgrade", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    /* Operator configured the cool-day role as the hot profile. */
    memset(in.roles.cool_day_profile_id, 0, sizeof(in.roles.cool_day_profile_id));
    strncpy(in.roles.cool_day_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(in.roles.cool_day_profile_id) - 1);
    /* Gates that would block an upgrade must not block this downgrade. */
    in.mining = TUNING_MINING_UNSTABLE;
    in.cooldown.active = true;
    in.upgrade_inhibited = true;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, out.profile_id);
    TEST_ASSERT_TRUE(out.is_downgrade);
}

TEST_CASE("evaluate: production registry can never emit a profile selection", "[tuning_policy]")
{
    /* The REAL registry: all three descriptors UNVALIDATED. Across every
     * policy stance, no intent may ever carry a profile id — nothing is
     * automatically selectable until the owner records validation
     * evidence, regardless of what the miner currently runs. */
    size_t n = 0;
    const TuningProfile *prod = tuning_registry_gamma601(&n);
    TuningPolicyEnvironment env = env_of(prod, n);
    TuningSelectionIntent out;

    /* Sensor failure: hot + emergency roles ineligible -> retain+inhibit. */
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.sensors.asic_temp = TUNING_SENSOR_IMPLAUSIBLE;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_RETAIN_INHIBIT, out.action);
    TEST_ASSERT_EQUAL_STRING("", out.profile_id);

    /* Emergency thermal: operator recovery, never a selection. */
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.emergency_thermal_active = true;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_OPERATOR_RECOVERY, out.action);
    TEST_ASSERT_EQUAL_STRING("", out.profile_id);

    /* API fail-safe: operator recovery, never a selection. */
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_FAIL_SAFE;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_OPERATOR_RECOVERY, out.action);
    TEST_ASSERT_EQUAL_STRING("", out.profile_id);

    /* Hot forecast: operator recovery, never a selection. */
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_HOT_PROFILE;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_OPERATOR_RECOVERY, out.action);
    TEST_ASSERT_EQUAL_STRING("", out.profile_id);

    /* Cool forecast: the cool role itself is ineligible -> no upgrade. */
    in = base_in(TUNING_PROFILE_ID_HOT_WEATHER);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_REASON_PROFILE_NOT_ELIGIBLE, out.secondary_reason);
    TEST_ASSERT_EQUAL_STRING("", out.profile_id);

    /* Manual override to a production profile: rejected as not eligible;
     * the arbitration still selects nothing. */
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.manual_present = true;
    in.manual = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_NOT_ELIGIBLE, out.override_result);
    TEST_ASSERT_NOT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING("", out.profile_id);
}

/* ---------------- arbitration: precedence 7 + generation ---------------- */

TEST_CASE("evaluate: climate RETAIN falls to current-profile source", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.policy_generation = 41;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, out.action);
    TEST_ASSERT_EQUAL(TUNING_ACTOR_NONE, out.actor);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_CURRENT_PROFILE, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_REASON_CURRENT_RETAINED, out.reason);
    TEST_ASSERT_EQUAL(TUNING_REASON_CLIMATE_UNKNOWN, out.secondary_reason);
    TEST_ASSERT_EQUAL_STRING("", out.profile_id);
    TEST_ASSERT_EQUAL_UINT32(41, out.policy_generation);
}

/* ---------------- effective ceiling ---------------- */

TEST_CASE("ceiling: derived from stance, fails closed when unresolvable", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    uint8_t rank = 0xFF;

    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_HOT_WEATHER);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    TEST_ASSERT_TRUE(tuning_policy_effective_ceiling(&env, &in, &rank));
    TEST_ASSERT_EQUAL_UINT8(2, rank); /* cool role = supersink */

    in.climate_request = TUNING_CLIMATE_REQ_HOT_PROFILE;
    TEST_ASSERT_TRUE(tuning_policy_effective_ceiling(&env, &in, &rank));
    TEST_ASSERT_EQUAL_UINT8(1, rank);

    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX); /* RETAIN, current known */
    TEST_ASSERT_TRUE(tuning_policy_effective_ceiling(&env, &in, &rank));
    TEST_ASSERT_EQUAL_UINT8(2, rank);

    in = base_in(NULL); /* RETAIN, current unknown -> hot-day reference */
    TEST_ASSERT_TRUE(tuning_policy_effective_ceiling(&env, &in, &rank));
    TEST_ASSERT_EQUAL_UINT8(1, rank);

    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.emergency_thermal_active = true;
    TEST_ASSERT_TRUE(tuning_policy_effective_ceiling(&env, &in, &rank));
    TEST_ASSERT_EQUAL_UINT8(0, rank);

    /* Unresolvable reference -> false. */
    in = base_in(NULL);
    in.climate_request = TUNING_CLIMATE_REQ_HOT_PROFILE;
    memset(in.roles.hot_day_profile_id, 0, sizeof(in.roles.hot_day_profile_id));
    TEST_ASSERT_FALSE(tuning_policy_effective_ceiling(&env, &in, &rank));
    TEST_ASSERT_FALSE(tuning_policy_effective_ceiling(NULL, &in, &rank));
    TEST_ASSERT_FALSE(tuning_policy_effective_ceiling(&env, NULL, &rank));
    TEST_ASSERT_FALSE(tuning_policy_effective_ceiling(&env, &in, NULL));
}

/* ---------------- review-driven coverage (W1 adversarial pass) -------- */

TEST_CASE("evaluate: safety sources preempt an active manual override", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningSelectionIntent out;

    /* 3 > 5: dead ASIC sensor beats an operator upgrade request. */
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.sensors.asic_temp = TUNING_SENSOR_INVALID;
    in.manual_present = true;
    in.manual = ov_valid(TUNING_PROFILE_ID_SUPERSINK_MAX);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SOURCE_SENSOR_INTEGRITY, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_NOT_EVALUATED, out.override_result);

    /* 4 > 5: forecast fail-safe beats an active override. */
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_FAIL_SAFE;
    in.manual_present = true;
    in.manual = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SOURCE_WEATHER_FAIL_SAFE, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_NOT_EVALUATED, out.override_result);

    /* No override supplied at all still reads ERR_INACTIVE. */
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.sensors.asic_temp = TUNING_SENSOR_INVALID;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_ERR_INACTIVE, out.override_result);
}

TEST_CASE("evaluate: hot forecast with no valid hot profile surfaces operator", "[tuning_policy]")
{
    /* Production registry: hot role UNVALIDATED. A HOT forecast must never
     * fall back to the cool-day profile or select an UNVALIDATED one. */
    size_t n = 0;
    const TuningProfile *prod = tuning_registry_gamma601(&n);
    TuningPolicyEnvironment env = env_of(prod, n);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_HOT_PROFILE;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_OPERATOR_RECOVERY, out.action);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_WEATHER_POLICY, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_REASON_WEATHER_HOT_FORECAST, out.reason);
    TEST_ASSERT_EQUAL(TUNING_REASON_NO_VALID_HOT_PROFILE, out.secondary_reason);
    TEST_ASSERT_EQUAL_STRING("", out.profile_id);
}

TEST_CASE("climate step: custom thresholds are consumed, not the defaults", "[tuning_policy]")
{
    TuningClimateThresholds t = { .hot_threshold_dc = 400, .cool_threshold_dc = 350 };
    TEST_ASSERT_TRUE(tuning_climate_thresholds_valid(&t));
    TuningClimateState s = st_of(TUNING_WEATHER_STATE_UNKNOWN);
    TuningClimateRequest req;

    /* 38.0 C is HOT under the defaults but in-band here. */
    TuningForecastInput f = fc_ok(380);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_UNKNOWN, s.state);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_RETAIN, req);

    f = fc_ok(400);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_HOT, s.state);

    f = fc_ok(351); /* would exit under defaults; still hot here */
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_HOT, s.state);

    f = fc_ok(350);
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_climate_step(&s, &t, &f, &s, &req));
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_NORMAL, s.state);
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_COOL_PROFILE, req);
}

TEST_CASE("evaluate: safety select from an unknown current profile", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningSelectionIntent out;

    /* Fresh device: no committed profile at all. */
    TuningPolicyInput in = base_in(NULL);
    in.sensors.asic_temp = TUNING_SENSOR_INVALID;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_REASON_CURRENT_UNKNOWN, out.secondary_reason);
    TEST_ASSERT_FALSE(out.is_upgrade);
    TEST_ASSERT_FALSE(out.is_downgrade);

    /* Current id set but absent from the registry: treated as unknown. */
    in = base_in("ghost-profile");
    in.sensors.asic_temp = TUNING_SENSOR_INVALID;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_REASON_CURRENT_UNKNOWN, out.secondary_reason);
}

TEST_CASE("evaluate: manual override may upgrade despite weather-only gates", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_HOT_WEATHER);
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE; /* ceiling rank 2 */
    in.manual_present = true;
    in.manual = ov_valid(TUNING_PROFILE_ID_SUPERSINK_MAX);
    /* Weather-upgrade gates that do NOT bind a manual override: */
    in.mining = TUNING_MINING_DEGRADED;
    in.upgrade_inhibited = true;
    TuningSelectionIntent out;
    TEST_ASSERT_EQUAL(TUNING_POLICY_OK, tuning_policy_evaluate(&env, &in, &out));
    TEST_ASSERT_EQUAL(TUNING_SOURCE_MANUAL_OVERRIDE, out.winning_source);
    TEST_ASSERT_EQUAL(TUNING_SELECT_PROFILE, out.action);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_SUPERSINK_MAX, out.profile_id);
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_OK, out.override_result);
    TEST_ASSERT_TRUE(out.is_upgrade);
}

TEST_CASE("ceiling: safety stances bound the ceiling", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningPolicyEnvironment env = env_of(reg, TUNING_REGISTRY_GAMMA601_COUNT);
    uint8_t rank = 0xFF;

    /* Stability rollback: ceiling = rollback target rank. */
    TuningPolicyInput in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.stability_rollback_active = true;
    strncpy(in.stability_rollback_profile_id, TUNING_PROFILE_ID_EMERGENCY,
            sizeof(in.stability_rollback_profile_id) - 1);
    TEST_ASSERT_TRUE(tuning_policy_effective_ceiling(&env, &in, &rank));
    TEST_ASSERT_EQUAL_UINT8(0, rank);

    /* Untrusted time: hot-day rank even when the climate says COOL. */
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.trusted_time_valid = false;
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    TEST_ASSERT_TRUE(tuning_policy_effective_ceiling(&env, &in, &rank));
    TEST_ASSERT_EQUAL_UINT8(1, rank);

    /* Forecast fail-safe. */
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.climate_request = TUNING_CLIMATE_REQ_FAIL_SAFE;
    TEST_ASSERT_TRUE(tuning_policy_effective_ceiling(&env, &in, &rank));
    TEST_ASSERT_EQUAL_UINT8(1, rank);

    /* Sensor integrity failure. */
    in = base_in(TUNING_PROFILE_ID_SUPERSINK_MAX);
    in.sensors.asic_temp = TUNING_SENSOR_IMPLAUSIBLE;
    in.climate_request = TUNING_CLIMATE_REQ_COOL_PROFILE;
    TEST_ASSERT_TRUE(tuning_policy_effective_ceiling(&env, &in, &rank));
    TEST_ASSERT_EQUAL_UINT8(1, rank);

    /* RETAIN with a current id that is not in the registry: hot fallback. */
    in = base_in("ghost-profile");
    TEST_ASSERT_TRUE(tuning_policy_effective_ceiling(&env, &in, &rank));
    TEST_ASSERT_EQUAL_UINT8(1, rank);
}

TEST_CASE("override: OPERATOR_RECOVERY actor is accepted", "[tuning_policy]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningHardwareContext hw = hw_full();
    const TuningProfile *hot =
        tuning_registry_find(reg, TUNING_REGISTRY_GAMMA601_COUNT,
                             TUNING_PROFILE_ID_HOT_WEATHER);
    TuningManualOverride ov = ov_valid(TUNING_PROFILE_ID_HOT_WEATHER);
    ov.actor = TUNING_ACTOR_OPERATOR_RECOVERY;
    TEST_ASSERT_EQUAL(TUNING_OVERRIDE_OK,
                      tuning_override_eligible(&ov, hot, &hw, 2, true, 2, true,
                                               false, NOW_S, true));
}

/* ---------------- token strings ---------------- */

TEST_CASE("strings: policy enums all have distinct tokens", "[tuning_policy]")
{
    for (int i = 0; i < TUNING_ACTOR__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN", tuning_actor_str((TuningActorClass)i)));
    }
    for (int i = 0; i < TUNING_SOURCE__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN", tuning_source_str((TuningPrecedenceSource)i)));
    }
    for (int i = 0; i < TUNING_WEATHER_STATE__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("INVALID", tuning_weather_state_str((TuningWeatherState)i)));
    }
    for (int i = 0; i < TUNING_FORECAST__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN", tuning_forecast_status_str((TuningForecastStatus)i)));
    }
    for (int i = 0; i < TUNING_CLIMATE_REQ__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN", tuning_climate_request_str((TuningClimateRequest)i)));
    }
    for (int i = 0; i < TUNING_SENSOR__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN", tuning_sensor_status_str((TuningSensorStatus)i)));
    }
    for (int i = 0; i < TUNING_MINING__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("INVALID", tuning_mining_health_str((TuningMiningHealth)i)));
    }
    for (int i = 0; i < TUNING_SELECT__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN", tuning_selection_action_str((TuningSelectionAction)i)));
    }
    for (int i = 0; i < TUNING_REASON__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN", tuning_reason_str((TuningReasonCode)i)));
    }
    for (int i = 0; i < TUNING_POLICY__ERR_COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("ERR_UNKNOWN", tuning_policy_error_str((TuningPolicyError)i)));
    }
    for (int i = 0; i < TUNING_OVERRIDE__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("ERR_UNKNOWN", tuning_override_result_str((TuningOverrideResult)i)));
    }
    /* out-of-range values fall back to their sentinel token */
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", tuning_actor_str((TuningActorClass)250));
    TEST_ASSERT_EQUAL_STRING("ERR_UNKNOWN", tuning_policy_error_str((TuningPolicyError)250));
}
