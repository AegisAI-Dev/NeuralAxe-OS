/*
 * NeuralAxe — Gate W6.3.1 production W1 input projection tests.
 *
 * No hardware, no network, no weather, no clock. The telemetry snapshot is
 * driven through the same publication API the real producer tasks use, and the
 * registry is the REAL production Gamma 601 registry — because the load-bearing
 * claim of this gate is about what the real, UNVALIDATED profiles do.
 */

#include <string.h>

#include "unity.h"

#include "nx_tuning_input.h"
#include "nx_telemetry_safety.h"
#include "tuning_policy.h"
#include "tuning_profile.h"

#define TI_ASIC_DC   552      /* 55.2 C, synthetic */
#define TI_VRM_DC    610      /* 61.0 C, synthetic */
#define TI_RPM      3200u

static void pub_power(int32_t asic_dc, bool asic_ok, int32_t vrm_dc,
                      bool vrm_ok, bool emergency)
{
    NxTelemetryPowerFacts f;
    memset(&f, 0, sizeof(f));
    f.asic_temp_dc = asic_dc; f.asic_temp_valid = asic_ok;
    f.vrm_temp_dc = vrm_dc;   f.vrm_read_ok = vrm_ok;
    f.vrm_expected = true;    f.emergency_thermal_active = emergency;
    nx_telemetry_safety_publish_power(&f);
}

static void pub_fan(uint16_t rpm, bool expected, bool fault)
{
    NxTelemetryFanFacts f;
    memset(&f, 0, sizeof(f));
    f.fan_rpm = rpm; f.fan_expected = expected; f.fan_control_fault = fault;
    nx_telemetry_safety_publish_fan(&f);
}

/* Project from whatever the store currently holds, against the REAL registry. */
static NxTuningInputFact project(TuningPolicyEnvironment *env,
                                 TuningPolicyInput *in,
                                 NxTuningInputDiag *diag)
{
    NxTelemetrySafetySnapshot s;
    NxTuningStructuralFacts   facts;
    const TuningProfile      *reg;
    size_t n = 0;

    TEST_ASSERT_TRUE(nx_telemetry_safety_read(&s));
    nx_tuning_structural_facts_gamma601(&facts);
    reg = tuning_registry_gamma601(&n);
    return nx_tuning_input_project(&s, &facts, reg, n, env, in, diag);
}

/* ================================================================== */
/* A — telemetry publication states (4)                                */
/* ================================================================== */

TEST_CASE("w631: no telemetry published is ABSENT and unusable", "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;

    nx_telemetry_safety_reset();
    TEST_ASSERT_EQUAL(NX_W1_INPUT_TELEMETRY_ABSENT, project(&env, &in, &d));
    TEST_ASSERT_FALSE(nx_tuning_input_usable(NX_W1_INPUT_TELEMETRY_ABSENT));
    /* Nothing may look healthy. */
    TEST_ASSERT_NOT_EQUAL(TUNING_SENSOR_OK, in.sensors.asic_temp);
    TEST_ASSERT_NOT_EQUAL(TUNING_SENSOR_OK, in.sensors.vrm_temp);
    TEST_ASSERT_NOT_EQUAL(TUNING_SENSOR_OK, in.sensors.fan_tach);
    TEST_ASSERT_TRUE(in.sensors.fan_control_uncertain);
}

TEST_CASE("w631: only power published is PARTIAL and unusable", "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;

    nx_telemetry_safety_reset();
    pub_power(TI_ASIC_DC, true, TI_VRM_DC, true, false);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_TELEMETRY_PARTIAL, project(&env, &in, &d));
    TEST_ASSERT_FALSE(nx_tuning_input_usable(NX_W1_INPUT_TELEMETRY_PARTIAL));
    /* The unpublished fan side cannot become healthy. */
    TEST_ASSERT_EQUAL(TUNING_SENSOR_MISSING, in.sensors.fan_tach);
    TEST_ASSERT_TRUE(d.telemetry_power_published);
    TEST_ASSERT_FALSE(d.telemetry_fan_published);
}

TEST_CASE("w631: only fan published is PARTIAL and unusable", "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;

    nx_telemetry_safety_reset();
    pub_fan(TI_RPM, true, false);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_TELEMETRY_PARTIAL, project(&env, &in, &d));
    TEST_ASSERT_NOT_EQUAL(TUNING_SENSOR_OK, in.sensors.asic_temp);
    TEST_ASSERT_NOT_EQUAL(TUNING_SENSOR_OK, in.sensors.vrm_temp);
}

TEST_CASE("w631: a healthy complete snapshot projects OK", "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;

    nx_telemetry_safety_reset();
    pub_power(TI_ASIC_DC, true, TI_VRM_DC, true, false);
    pub_fan(TI_RPM, true, false);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project(&env, &in, &d));
    TEST_ASSERT_TRUE(nx_tuning_input_usable(NX_W1_INPUT_OK));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_OK, in.sensors.asic_temp);
    TEST_ASSERT_EQUAL(TUNING_SENSOR_OK, in.sensors.vrm_temp);
    TEST_ASSERT_EQUAL(TUNING_SENSOR_OK, in.sensors.fan_tach);
    TEST_ASSERT_TRUE(d.sensors_upgrade_ok);
    TEST_ASSERT_FALSE(d.sensors_integrity_failed);
}

/* ================================================================== */
/* B — degraded sensor directions (6)                                  */
/* ================================================================== */

TEST_CASE("w631: ASIC invalid fails closed", "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;

    nx_telemetry_safety_reset();
    pub_power(-1, false, TI_VRM_DC, true, false);   /* the -1 sentinel */
    pub_fan(TI_RPM, true, false);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project(&env, &in, &d));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_INVALID, in.sensors.asic_temp);
    TEST_ASSERT_TRUE(d.sensors_integrity_failed);
    TEST_ASSERT_FALSE(d.sensors_upgrade_ok);
}

TEST_CASE("w631: a cached in-band VRM value with read_ok=false is INVALID",
          "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;

    nx_telemetry_safety_reset();
    /* 61.0 C is perfectly plausible; it came from the SMBus-failure cache. */
    pub_power(TI_ASIC_DC, true, TI_VRM_DC, false, false);
    pub_fan(TI_RPM, true, false);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project(&env, &in, &d));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_INVALID, in.sensors.vrm_temp);
    TEST_ASSERT_TRUE(d.sensors_integrity_failed);
}

TEST_CASE("w631: zero fan rpm is INVALID, never implicit health", "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;

    nx_telemetry_safety_reset();
    pub_power(TI_ASIC_DC, true, TI_VRM_DC, true, false);
    pub_fan(0u, true, false);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project(&env, &in, &d));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_INVALID, in.sensors.fan_tach);
    TEST_ASSERT_FALSE(d.sensors_upgrade_ok);
}

TEST_CASE("w631: a board declaring no fan is MISSING", "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;

    nx_telemetry_safety_reset();
    pub_power(TI_ASIC_DC, true, TI_VRM_DC, true, false);
    pub_fan(0u, false, false);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project(&env, &in, &d));
    TEST_ASSERT_EQUAL(TUNING_SENSOR_MISSING, in.sensors.fan_tach);
}

TEST_CASE("w631: a fan-control fault reaches W1 as uncertain control", "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;

    nx_telemetry_safety_reset();
    pub_power(TI_ASIC_DC, true, TI_VRM_DC, true, false);
    pub_fan(TI_RPM, true, true);      /* PWM write failed */
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project(&env, &in, &d));
    TEST_ASSERT_TRUE(in.sensors.fan_control_uncertain);
    TEST_ASSERT_TRUE(d.sensors_integrity_failed);
}

TEST_CASE("w631: emergency thermal state reaches W1", "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;

    nx_telemetry_safety_reset();
    pub_power(TI_ASIC_DC, true, TI_VRM_DC, true, true);
    pub_fan(TI_RPM, true, false);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project(&env, &in, &d));
    TEST_ASSERT_TRUE(in.emergency_thermal_active);
}

/* ================================================================== */
/* C — structural sentinels (2)                                        */
/* ================================================================== */

TEST_CASE("w631: structural sentinels are the documented fail-closed ones",
          "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;
    NxTuningStructuralFacts facts;

    nx_telemetry_safety_reset();
    pub_power(TI_ASIC_DC, true, TI_VRM_DC, true, false);
    pub_fan(TI_RPM, true, false);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project(&env, &in, &d));

    /* mining health: UNKNOWN, and it must fail closed for upgrades. */
    TEST_ASSERT_EQUAL(TUNING_MINING_UNKNOWN, in.mining);
    /* current profile: unknown, never inferred from frequency/voltage. */
    TEST_ASSERT_FALSE(in.current_profile_known);
    TEST_ASSERT_EQUAL_STRING("", in.current_profile_id);
    /* roles: unconfigured sentinels. */
    TEST_ASSERT_EQUAL_STRING("", in.roles.cool_day_profile_id);
    TEST_ASSERT_EQUAL_STRING("", in.roles.hot_day_profile_id);
    TEST_ASSERT_EQUAL_STRING("", in.roles.emergency_profile_id);
    /* absent mechanisms. */
    TEST_ASSERT_FALSE(in.manual_present);
    TEST_ASSERT_FALSE(in.cooldown.active);
    TEST_ASSERT_FALSE(in.upgrade_inhibited);
    TEST_ASSERT_FALSE(in.stability_rollback_active);

    /* installation facts: UNSPECIFIED, never guessed. */
    nx_tuning_structural_facts_gamma601(&facts);
    TEST_ASSERT_EQUAL(TUNING_COOLING_UNSPECIFIED, facts.cooling_installed);
    TEST_ASSERT_EQUAL(TUNING_PSU_UNSPECIFIED, facts.psu_installed);
    TEST_ASSERT_EQUAL(TUNING_BOARD_GAMMA_601, facts.board);
    TEST_ASSERT_EQUAL(TUNING_ASIC_BM1370, facts.asic);
    TEST_ASSERT_EQUAL(TUNING_COOLING_UNSPECIFIED, env.hw.cooling_installed);
    TEST_ASSERT_EQUAL(TUNING_PSU_UNSPECIFIED, env.hw.psu_installed);
}

TEST_CASE("w631: weather-owned fields are left for the runtime to set",
          "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;

    nx_telemetry_safety_reset();
    pub_power(TI_ASIC_DC, true, TI_VRM_DC, true, false);
    pub_fan(TI_RPM, true, false);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project(&env, &in, &d));

    /* weather_runtime_step() overwrites these three; the projection must not
     * become a competing authority for them. */
    TEST_ASSERT_EQUAL(TUNING_CLIMATE_REQ_RETAIN, in.climate_request);
    TEST_ASSERT_FALSE(in.trusted_time_valid);
    TEST_ASSERT_EQUAL_UINT64(0u, in.now_epoch_s);
}

/* ================================================================== */
/* D — THE load-bearing UNVALIDATED proof (3)                          */
/* ================================================================== */

TEST_CASE("w631: the real registry is wired and every profile is UNVALIDATED",
          "[tuning_input_projection]")
{
    const TuningProfile *reg;
    size_t n = 0, i;

    reg = tuning_registry_gamma601(&n);
    TEST_ASSERT_NOT_NULL(reg);
    TEST_ASSERT_TRUE(n > 0u);
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL(TUNING_VALIDATION_UNVALIDATED, reg[i].validation);
        TEST_ASSERT_FALSE(reg[i].payload_present);
        TEST_ASSERT_EQUAL_UINT32(0u, reg[i].evidence_fingerprint);
    }
}

/*
 * tuning_policy_evaluate() genuinely EXECUTES against the projection, and no
 * UNVALIDATED profile becomes automatically eligible — whatever the policy
 * decides. This is the assertion the whole gate exists for.
 */
TEST_CASE("w631: policy evaluates against the projection and authorizes nothing",
          "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;
    TuningSelectionIntent intent;
    TuningPolicyError err;
    size_t i;

    nx_telemetry_safety_reset();
    pub_power(TI_ASIC_DC, true, TI_VRM_DC, true, false);
    pub_fan(TI_RPM, true, false);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project(&env, &in, &d));

    memset(&intent, 0, sizeof(intent));
    err = tuning_policy_evaluate(&env, &in, &intent);
    /* It RAN: not a null/range refusal. */
    TEST_ASSERT_NOT_EQUAL(TUNING_POLICY_ERR_NULL, err);
    TEST_ASSERT_NOT_EQUAL(TUNING_POLICY_ERR_ENVIRONMENT, err);
    TEST_ASSERT_NOT_EQUAL(TUNING_POLICY_ERR_ENUM_RANGE, err);

    /* Whatever it selected, no UNVALIDATED profile may be auto-eligible. */
    for (i = 0; i < env.profile_count; i++) {
        TEST_ASSERT_NOT_EQUAL(TUNING_ELIGIBLE_OK,
                              tuning_profile_auto_eligible(&env.profiles[i],
                                                           &env.hw));
    }
    /* And with a climate request that WOULD ask for an upgrade, still nothing
     * becomes eligible. */
    in.climate_request = TUNING_CLIMATE_REQ_HOT_PROFILE;
    in.trusted_time_valid = true;
    memset(&intent, 0, sizeof(intent));
    (void)tuning_policy_evaluate(&env, &in, &intent);
    for (i = 0; i < env.profile_count; i++) {
        TEST_ASSERT_NOT_EQUAL(TUNING_ELIGIBLE_OK,
                              tuning_profile_auto_eligible(&env.profiles[i],
                                                           &env.hw));
    }
}

TEST_CASE("w631: an emergency posture still authorizes no profile", "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;
    TuningSelectionIntent intent;
    size_t i;

    nx_telemetry_safety_reset();
    pub_power(TI_ASIC_DC, true, TI_VRM_DC, true, true);   /* emergency */
    pub_fan(0u, true, true);                              /* fan broken */
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project(&env, &in, &d));

    memset(&intent, 0, sizeof(intent));
    (void)tuning_policy_evaluate(&env, &in, &intent);
    for (i = 0; i < env.profile_count; i++) {
        TEST_ASSERT_NOT_EQUAL(TUNING_ELIGIBLE_OK,
                              tuning_profile_auto_eligible(&env.profiles[i],
                                                           &env.hw));
    }
}

/* ================================================================== */
/* totality                                                            */
/* ================================================================== */

TEST_CASE("w631: NULL and no-registry are total and fail closed", "[tuning_input_projection]")
{
    TuningPolicyEnvironment env; TuningPolicyInput in; NxTuningInputDiag d;
    NxTelemetrySafetySnapshot s;
    NxTuningStructuralFacts facts;
    int i;

    nx_telemetry_safety_reset();
    TEST_ASSERT_TRUE(nx_telemetry_safety_read(&s));
    nx_tuning_structural_facts_gamma601(&facts);

    TEST_ASSERT_EQUAL(NX_W1_INPUT_UNAVAILABLE,
                      nx_tuning_input_project(NULL, &facts, NULL, 0, &env, &in, &d));
    TEST_ASSERT_EQUAL(NX_W1_INPUT_UNAVAILABLE,
                      nx_tuning_input_project(&s, NULL, NULL, 0, &env, &in, &d));
    TEST_ASSERT_EQUAL(NX_W1_INPUT_NO_REGISTRY,
                      nx_tuning_input_project(&s, &facts, NULL, 0, &env, &in, &d));
    /* No output combination may claim a healthy sensor. */
    TEST_ASSERT_NOT_EQUAL(TUNING_SENSOR_OK, in.sensors.fan_tach);

    nx_tuning_structural_facts_gamma601(NULL);   /* no-op, must not fault */
    for (i = 0; i < NX_W1_INPUT__COUNT + 3; i++) {
        TEST_ASSERT_NOT_NULL(nx_tuning_input_fact_str((NxTuningInputFact)i));
    }
    TEST_ASSERT_FALSE(nx_tuning_input_usable(NX_W1_INPUT_UNAVAILABLE));
    TEST_ASSERT_TRUE(nx_tuning_input_usable(NX_W1_INPUT_OK));
}
