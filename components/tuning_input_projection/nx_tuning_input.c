/*
 * NeuralAxe — production W1 policy input projection (Gate W6.3.1).
 *
 * Pure and total. No task, no lock, no hardware access, no NVS, no network, no
 * clock, no weather dependency. Every safety fact is either classified by a
 * committed W1 classifier or set to the sentinel the W1 contract documents for
 * a mechanism this posture does not have.
 */

#include <string.h>

#include "nx_tuning_input.h"

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */

const char *nx_tuning_input_fact_str(NxTuningInputFact f)
{
    switch (f) {
    case NX_W1_INPUT_UNAVAILABLE:       return "W1IN_UNAVAILABLE";
    case NX_W1_INPUT_OK:                return "W1IN_OK";
    case NX_W1_INPUT_TELEMETRY_PARTIAL: return "W1IN_TELEMETRY_PARTIAL";
    case NX_W1_INPUT_TELEMETRY_ABSENT:  return "W1IN_TELEMETRY_ABSENT";
    case NX_W1_INPUT_NO_REGISTRY:       return "W1IN_NO_REGISTRY";
    default:                            return "W1IN_UNAVAILABLE";
    }
}

bool nx_tuning_input_usable(NxTuningInputFact f)
{
    /*
     * ONLY a fully published snapshot yields an evaluable input. A partial one
     * is deliberately refused: the missing producer's fields would be the
     * zeroes of their types, and TUNING_SENSOR_OK is zero — so evaluating a
     * partial snapshot is precisely the "unpublished becomes healthy" failure
     * this whole chain of gates exists to prevent.
     */
    return f == NX_W1_INPUT_OK;
}

void nx_tuning_structural_facts_gamma601(NxTuningStructuralFacts *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    /* The DECLARED build target, not a runtime board string. */
    out->board = TUNING_BOARD_GAMMA_601;
    out->asic  = TUNING_ASIC_BM1370;
    /*
     * Owner-declared installation facts with no authoritative production
     * source. UNSPECIFIED is the documented fail-closed value and it makes the
     * W1 compatibility check stricter, never looser. The firmware does not
     * guess which cooler or PSU the owner fitted.
     */
    out->cooling_installed = TUNING_COOLING_UNSPECIFIED;
    out->psu_installed     = TUNING_PSU_UNSPECIFIED;
    out->policy_generation = 0u;
}

/* ------------------------------------------------------------------ */
/* The projection                                                      */
/* ------------------------------------------------------------------ */

NxTuningInputFact nx_tuning_input_project(const NxTelemetrySafetySnapshot *snap,
                                          const NxTuningStructuralFacts *facts,
                                          const TuningProfile *profiles,
                                          size_t profile_count,
                                          TuningPolicyEnvironment *out_env,
                                          TuningPolicyInput *out_in,
                                          NxTuningInputDiag *out_diag)
{
    NxTuningInputFact  fact;
    TuningSensorHealth h;
    bool power_ok, fan_ok;

    /* Fail closed FIRST: every output is a refusal until proven otherwise. */
    if (out_env != NULL) {
        memset(out_env, 0, sizeof(*out_env));
    }
    if (out_in != NULL) {
        memset(out_in, 0, sizeof(*out_in));
        /* A zeroed TuningSensorHealth would claim every sensor OK, so the
         * refusal path states the opposite explicitly. */
        out_in->sensors.asic_temp = TUNING_SENSOR_MISSING;
        out_in->sensors.vrm_temp  = TUNING_SENSOR_MISSING;
        out_in->sensors.fan_tach  = TUNING_SENSOR_MISSING;
        out_in->mining            = TUNING_MINING_UNKNOWN;
    }
    if (out_diag != NULL) {
        memset(out_diag, 0, sizeof(*out_diag));
        out_diag->fact             = (uint8_t)NX_W1_INPUT_UNAVAILABLE;
        out_diag->asic_temp_status = (uint8_t)TUNING_SENSOR_MISSING;
        out_diag->vrm_temp_status  = (uint8_t)TUNING_SENSOR_MISSING;
        out_diag->fan_tach_status  = (uint8_t)TUNING_SENSOR_MISSING;
        out_diag->mining_health    = (uint8_t)TUNING_MINING_UNKNOWN;
    }

    if (snap == NULL || facts == NULL || out_env == NULL || out_in == NULL) {
        return NX_W1_INPUT_UNAVAILABLE;
    }
    if (profiles == NULL || profile_count == 0u) {
        if (out_diag != NULL) {
            out_diag->fact = (uint8_t)NX_W1_INPUT_NO_REGISTRY;
        }
        return NX_W1_INPUT_NO_REGISTRY;
    }

    power_ok = nx_telemetry_safety_power_published(snap);
    fan_ok   = nx_telemetry_safety_fan_published(snap);
    if (!power_ok && !fan_ok) {
        fact = NX_W1_INPUT_TELEMETRY_ABSENT;
    } else if (!power_ok || !fan_ok) {
        fact = NX_W1_INPUT_TELEMETRY_PARTIAL;
    } else {
        fact = NX_W1_INPUT_OK;
    }

    /* ---- environment: registry + declared hardware ---- */
    out_env->profiles      = profiles;
    out_env->profile_count = profile_count;
    out_env->hw.board             = facts->board;
    out_env->hw.asic              = facts->asic;
    out_env->hw.cooling_installed = facts->cooling_installed;
    out_env->hw.psu_installed     = facts->psu_installed;

    /* ---- sensors: committed classifiers only ---- */
    memset(&h, 0, sizeof(h));
    /*
     * ASIC. An unpublished or invalid reading is handed to the classifier as
     * the firmware's own -1 sentinel, so the committed rule decides — this
     * module never decides validity itself.
     */
    h.asic_temp = tuning_classify_asic_temp_dc(
                      (power_ok && snap->asic_temp_valid) ? snap->asic_temp_dc : -1,
                      !(power_ok && snap->asic_temp_valid));
    /*
     * VRM. `read_ok == false` is passed as the classifier's read_error, which
     * returns INVALID regardless of how plausible the cached number is. There
     * is no unchanged-streak heuristic and stale_streak_limit is 0: for
     * Gamma 601 / TPS546 the successful acquisition IS the freshness
     * authority, per the committed W6.3T-A contract.
     */
    h.vrm_temp     = tuning_classify_vrm_temp_dc(power_ok ? snap->vrm_temp_dc : 0,
                                                 !(power_ok && snap->vrm_read_ok),
                                                 0u, 0u);
    h.vrm_expected = power_ok && snap->vrm_expected;
    /*
     * FAN. An unpublished fan side carries fan_expected = false, which the
     * committed classifier maps to MISSING — never OK, despite OK being the
     * enum's zero.
     */
    h.fan_tach = tuning_classify_fan_tach(fan_ok ? snap->fan_rpm : (uint16_t)0,
                                          fan_ok && snap->fan_expected,
                                          false);
    h.fan_control_uncertain = fan_ok ? snap->fan_control_fault
                                     : true;   /* unknown control state fails closed */

    out_in->sensors = h;

    /* ---- precedence 1-2: runtime facts ---- */
    out_in->emergency_thermal_active = power_ok && snap->emergency_thermal_active;
    /*
     * No stability-rollback mechanism exists in this posture, so "no rollback
     * is active" is the structural truth rather than an assumption, and the
     * profile id stays the documented empty sentinel.
     */
    out_in->stability_rollback_active = false;
    out_in->stability_rollback_profile_id[0] = '\0';

    /* ---- current profile ---- */
    /*
     * Nothing in this firmware has ever applied a governed profile, and there
     * is no authoritative applied-profile identity to read. `false` is the
     * documented sentinel ("id unset or not committed") and it fails closed
     * for upgrades. A current frequency or voltage is NOT evidence that a
     * named profile was applied, so no inference is attempted.
     */
    out_in->current_profile_known = false;
    out_in->current_profile_id[0] = '\0';

    /* ---- roles ---- */
    /*
     * Unconfigured. "" is the committed "role not configured" sentinel, and
     * resolve_eligible() returns NULL for it, so a safety role simply finds no
     * candidate and the policy degrades honestly. Inventing default role ids
     * would be manufacturing a policy decision to produce a recommendation.
     */
    out_in->roles.cool_day_profile_id[0]  = '\0';
    out_in->roles.hot_day_profile_id[0]   = '\0';
    out_in->roles.emergency_profile_id[0] = '\0';

    /* ---- manual override / cooldown / inhibit ---- */
    /* None of these mechanisms exist in this posture; the structural answer is
     * "absent", which is what the zeroed fields already say. */
    out_in->manual_present    = false;
    out_in->cooldown.active   = false;
    out_in->cooldown.source   = TUNING_ACTOR_NONE;   /* no actor started one */
    out_in->upgrade_inhibited = false;

    /* ---- mining health ---- */
    /*
     * No authoritative mining-health classifier exists. UNKNOWN is the honest
     * answer and it fails closed for upgrades by the committed W1 rule, so the
     * absence of a classifier cannot open an upgrade path. Building one is not
     * this gate's work.
     */
    out_in->mining = TUNING_MINING_UNKNOWN;

    out_in->policy_generation = facts->policy_generation;

    /*
     * DELIBERATELY LEFT ALONE: climate_request, trusted_time_valid and
     * now_epoch_s. weather_runtime_step() overwrites all three from the
     * committed trusted-time view and the W1 climate chain; writing them here
     * would create a competing authority. They keep their fail-safe zeroes:
     * TUNING_CLIMATE_REQ_RETAIN and untrusted.
     */

    if (out_diag != NULL) {
        out_diag->fact                       = (uint8_t)fact;
        out_diag->telemetry_power_published  = power_ok;
        out_diag->telemetry_fan_published    = fan_ok;
        out_diag->telemetry_power_generation = snap->power_generation;
        out_diag->telemetry_fan_generation   = snap->fan_generation;
        out_diag->asic_temp_status           = (uint8_t)h.asic_temp;
        out_diag->vrm_temp_status            = (uint8_t)h.vrm_temp;
        out_diag->fan_tach_status            = (uint8_t)h.fan_tach;
        out_diag->fan_control_uncertain      = h.fan_control_uncertain;
        out_diag->emergency_thermal_active   = out_in->emergency_thermal_active;
        out_diag->sensors_integrity_failed   = tuning_sensor_health_integrity_failed(&h);
        out_diag->sensors_upgrade_ok         = tuning_sensor_health_upgrade_ok(&h);
        out_diag->mining_health              = (uint8_t)out_in->mining;
        out_diag->current_profile_known      = out_in->current_profile_known;
        out_diag->profile_count              = (uint32_t)profile_count;
    }
    return fact;
}
