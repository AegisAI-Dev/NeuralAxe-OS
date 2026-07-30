/*
 * NeuralAxe Weather-Aware Tuning — pure climate policy + precedence
 * arbitration (Gate W1). PURE: no I/O, no heap, no clock reads, no logging.
 */

#include <string.h>

#include "tuning_policy.h"

/* ------------------------------------------------------------------ */
/* Climate hysteresis FSM                                              */
/* ------------------------------------------------------------------ */

void tuning_climate_thresholds_defaults(TuningClimateThresholds *out)
{
    if (out == NULL) {
        return;
    }
    out->hot_threshold_dc = TUNING_DEFAULT_HOT_THRESHOLD_DC;
    out->cool_threshold_dc = TUNING_DEFAULT_COOL_THRESHOLD_DC;
}

bool tuning_climate_thresholds_valid(const TuningClimateThresholds *t)
{
    if (t == NULL) {
        return false;
    }
    if (t->hot_threshold_dc <= t->cool_threshold_dc) {
        return false;
    }
    if (t->hot_threshold_dc < TUNING_FORECAST_SANITY_MIN_DC ||
        t->hot_threshold_dc > TUNING_FORECAST_SANITY_MAX_DC ||
        t->cool_threshold_dc < TUNING_FORECAST_SANITY_MIN_DC ||
        t->cool_threshold_dc > TUNING_FORECAST_SANITY_MAX_DC) {
        return false;
    }
    return true;
}

void tuning_climate_state_init(TuningClimateState *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = TUNING_WEATHER_STATE_UNKNOWN;
}

TuningPolicyError tuning_climate_step(const TuningClimateState *cur,
                                      const TuningClimateThresholds *th,
                                      const TuningForecastInput *in,
                                      TuningClimateState *next,
                                      TuningClimateRequest *out_request)
{
    if (cur == NULL || th == NULL || in == NULL || next == NULL ||
        out_request == NULL) {
        return TUNING_POLICY_ERR_NULL;
    }
    if (!tuning_climate_thresholds_valid(th)) {
        return TUNING_POLICY_ERR_THRESHOLDS;
    }
    if ((unsigned)cur->state >= TUNING_WEATHER_STATE__COUNT ||
        (unsigned)in->status >= TUNING_FORECAST__COUNT) {
        return TUNING_POLICY_ERR_ENUM_RANGE;
    }

    /* Alias-safe: copy the input state before writing any output. */
    TuningClimateState n = *cur;
    TuningClimateRequest req;

    bool usable = (in->status == TUNING_FORECAST_OK) &&
                  in->forecast_max_dc >= TUNING_FORECAST_SANITY_MIN_DC &&
                  in->forecast_max_dc <= TUNING_FORECAST_SANITY_MAX_DC;

    if (!usable) {
        /* Unusable forecast NEVER changes the hysteresis state and NEVER
         * enables an upgrade — it only requests the fail-safe stance. */
        n.last_forecast_valid = false;
        req = TUNING_CLIMATE_REQ_FAIL_SAFE;
    } else {
        int16_t t = in->forecast_max_dc;
        n.last_forecast_valid = true;
        n.last_forecast_max_dc = t;

        TuningWeatherState ns;
        if (t >= th->hot_threshold_dc) {
            ns = TUNING_WEATHER_STATE_HOT;
        } else if (t <= th->cool_threshold_dc) {
            ns = TUNING_WEATHER_STATE_NORMAL;
        } else {
            ns = n.state; /* dead band: retain existing stance */
        }

        if (ns != n.state) {
            n.state = ns;
            if (n.transition_count != UINT32_MAX) {
                n.transition_count++;
            }
        }

        switch (n.state) {
        case TUNING_WEATHER_STATE_HOT:
            req = TUNING_CLIMATE_REQ_HOT_PROFILE;
            break;
        case TUNING_WEATHER_STATE_NORMAL:
            req = TUNING_CLIMATE_REQ_COOL_PROFILE;
            break;
        default:
            /* UNKNOWN + dead-band forecast: no basis to decide. */
            req = TUNING_CLIMATE_REQ_RETAIN;
            break;
        }
    }

    *next = n;
    *out_request = req;
    return TUNING_POLICY_OK;
}

/* ------------------------------------------------------------------ */
/* Sensor-health model                                                 */
/* ------------------------------------------------------------------ */

TuningSensorStatus tuning_classify_asic_temp_dc(int32_t temp_dc, bool read_error)
{
    if (read_error) {
        return TUNING_SENSOR_INVALID;
    }
    /* The firmware's -1 sentinel (read failure / ASIC powered down) and any
     * non-positive value are invalid — a raw -1 would otherwise silently
     * sit below every threshold (Gate W0 §14). */
    if (temp_dc <= 0) {
        return TUNING_SENSOR_INVALID;
    }
    if (temp_dc > TUNING_ASIC_TEMP_PLAUSIBLE_MAX_DC) {
        /* EMC2101 diode-fault codes decode to ~127 C (Gate W0 §14). */
        return TUNING_SENSOR_IMPLAUSIBLE;
    }
    return TUNING_SENSOR_OK;
}

TuningSensorStatus tuning_classify_vrm_temp_dc(int32_t temp_dc, bool read_error,
                                               uint32_t unchanged_streak,
                                               uint32_t stale_streak_limit)
{
    if (read_error) {
        return TUNING_SENSOR_INVALID;
    }
    /* TPS546_get_temperature returns a cached last value (initially 0) on
     * SMBus failure — non-positive is invalid (Gate W0 §15). */
    if (temp_dc <= 0) {
        return TUNING_SENSOR_INVALID;
    }
    if (temp_dc > TUNING_VRM_TEMP_PLAUSIBLE_MAX_DC) {
        return TUNING_SENSOR_IMPLAUSIBLE;
    }
    /* A frozen reading is the cached-last-value failure signature. */
    if (stale_streak_limit != 0 && unchanged_streak >= stale_streak_limit) {
        return TUNING_SENSOR_STALE;
    }
    return TUNING_SENSOR_OK;
}

static bool sensor_status_in_range(TuningSensorStatus s)
{
    return (unsigned)s < TUNING_SENSOR__COUNT;
}

bool tuning_sensor_health_upgrade_ok(const TuningSensorHealth *h)
{
    if (h == NULL) {
        return false;
    }
    if (!sensor_status_in_range(h->asic_temp) ||
        !sensor_status_in_range(h->vrm_temp) ||
        !sensor_status_in_range(h->fan_tach)) {
        return false;
    }
    if (h->asic_temp != TUNING_SENSOR_OK) {
        return false;
    }
    if (h->vrm_expected && h->vrm_temp != TUNING_SENSOR_OK) {
        return false;
    }
    if (h->fan_tach != TUNING_SENSOR_OK) {
        return false;
    }
    if (h->fan_control_uncertain) {
        return false;
    }
    return true;
}

bool tuning_sensor_health_integrity_failed(const TuningSensorHealth *h)
{
    if (h == NULL) {
        return true; /* fail closed */
    }
    if (!sensor_status_in_range(h->asic_temp) ||
        !sensor_status_in_range(h->vrm_temp) ||
        !sensor_status_in_range(h->fan_tach)) {
        return true;
    }
    if (h->asic_temp != TUNING_SENSOR_OK) {
        return true;
    }
    if (h->vrm_expected && h->vrm_temp != TUNING_SENSOR_OK) {
        return true;
    }
    if (h->fan_control_uncertain) {
        return true;
    }
    /* A tach-only problem blocks upgrades (upgrade_ok false) but is not an
     * integrity failure forcing a downgrade — one fan is structurally
     * unobservable on 601 regardless (Gate W0 §16). */
    return false;
}

/* ------------------------------------------------------------------ */
/* Manual override                                                     */
/* ------------------------------------------------------------------ */

TuningOverrideResult tuning_override_eligible(const TuningManualOverride *ov,
                                              const TuningProfile *requested,
                                              const TuningHardwareContext *hw,
                                              uint8_t ceiling_rank,
                                              bool ceiling_valid,
                                              uint8_t current_rank,
                                              bool current_rank_valid,
                                              bool cooldown_active,
                                              uint64_t now_epoch_s,
                                              bool now_trusted)
{
    if (ov == NULL || hw == NULL) {
        return TUNING_OVERRIDE_ERR_NULL;
    }
    if (!ov->active) {
        return TUNING_OVERRIDE_ERR_INACTIVE;
    }
    if (ov->actor != TUNING_ACTOR_MANUAL_API &&
        ov->actor != TUNING_ACTOR_OPERATOR_RECOVERY) {
        return TUNING_OVERRIDE_ERR_ACTOR;
    }
    if (ov->expires_epoch_s == 0) {
        return TUNING_OVERRIDE_ERR_NO_EXPIRATION;
    }
    if (ov->expires_epoch_s <= ov->created_epoch_s) {
        return TUNING_OVERRIDE_ERR_EXPIRATION_ORDER;
    }
    if (!now_trusted) {
        return TUNING_OVERRIDE_ERR_TIME_UNTRUSTED;
    }
    if (now_epoch_s >= ov->expires_epoch_s) {
        return TUNING_OVERRIDE_ERR_EXPIRED;
    }
    if (requested == NULL) {
        return TUNING_OVERRIDE_ERR_PROFILE_UNKNOWN;
    }
    if (tuning_profile_auto_eligible(requested, hw) != TUNING_ELIGIBLE_OK) {
        /* Unvalidated, retired, disabled, structurally invalid or
         * hardware-incompatible — never overridable (boundary 13). */
        return TUNING_OVERRIDE_ERR_NOT_ELIGIBLE;
    }
    if (!ceiling_valid) {
        return TUNING_OVERRIDE_ERR_CEILING_UNKNOWN; /* fail closed */
    }
    if (requested->rank > ceiling_rank) {
        return TUNING_OVERRIDE_ERR_ABOVE_CEILING;
    }
    /* Conservative reading of boundaries 15/16: an override that would
     * RAISE rank during an active cooldown is rejected; safer-or-equal is
     * always allowed. Unknown current rank counts as an upgrade. */
    bool is_upgrade = current_rank_valid ? (requested->rank > current_rank) : true;
    if (cooldown_active && is_upgrade) {
        return TUNING_OVERRIDE_ERR_COOLDOWN_UPGRADE;
    }
    return TUNING_OVERRIDE_OK;
}

/* ------------------------------------------------------------------ */
/* Precedence arbitration                                              */
/* ------------------------------------------------------------------ */

static void copy_id(char *dst, const char *src)
{
    memset(dst, 0, TUNING_PROFILE_ID_MAX);
    if (src != NULL) {
        strncpy(dst, src, TUNING_PROFILE_ID_MAX - 1);
    }
}

/* Resolve a role id to an auto-eligible profile, else NULL. */
static const TuningProfile *resolve_eligible(const TuningPolicyEnvironment *env,
                                             const char *profile_id)
{
    const TuningProfile *p =
        tuning_registry_find(env->profiles, env->profile_count, profile_id);
    if (p == NULL) {
        return NULL;
    }
    if (tuning_profile_auto_eligible(p, &env->hw) != TUNING_ELIGIBLE_OK) {
        return NULL;
    }
    return p;
}

/* Fill rank-direction flags for a select of `target` vs known current. */
static void set_direction(TuningSelectionIntent *r, const TuningProfile *target,
                          const TuningProfile *cur)
{
    r->is_upgrade = (cur != NULL) && target->rank > cur->rank;
    r->is_downgrade = (cur != NULL) && target->rank < cur->rank;
}

/*
 * Shared safety-source resolution (precedence 1-4 and weather-HOT):
 * SELECT only a strict downgrade (or a move from unknown current); an
 * already-active or safer current is retained. Returns true when it fully
 * decided the intent; false when the target was NOT eligible (caller
 * applies its per-source fallback).
 */
static bool safety_select(const TuningPolicyEnvironment *env,
                          const TuningProfile *cur,
                          const char *target_id,
                          TuningActorClass actor,
                          TuningPrecedenceSource source,
                          TuningReasonCode primary,
                          TuningSelectionIntent *r)
{
    const TuningProfile *p = resolve_eligible(env, target_id);
    if (p == NULL) {
        return false;
    }
    r->actor = actor;
    r->winning_source = source;
    r->reason = primary;
    if (cur != NULL) {
        if (p == cur) {
            r->action = TUNING_SELECT_NONE;
            r->secondary_reason = TUNING_REASON_ALREADY_ACTIVE;
            return true;
        }
        if (p->rank < cur->rank) {
            r->action = TUNING_SELECT_PROFILE;
            copy_id(r->profile_id, p->id);
            set_direction(r, p, cur);
            return true;
        }
        /* Target is not a strict downgrade — safety sources never move up
         * (e.g. current is emergency, fail-safe wants hot: retain). */
        r->action = TUNING_SELECT_NONE;
        r->secondary_reason = TUNING_REASON_RETAINED_SAFER_CURRENT;
        return true;
    }
    /* Unknown current: moving to a known-safe profile is permitted. */
    r->action = TUNING_SELECT_PROFILE;
    copy_id(r->profile_id, p->id);
    r->secondary_reason = TUNING_REASON_CURRENT_UNKNOWN;
    return true;
}

bool tuning_policy_effective_ceiling(const TuningPolicyEnvironment *env,
                                     const TuningPolicyInput *in,
                                     uint8_t *out_rank)
{
    if (env == NULL || in == NULL || out_rank == NULL ||
        env->profiles == NULL || env->profile_count == 0) {
        return false;
    }
    const char *ref_id = NULL;

    if (in->emergency_thermal_active) {
        ref_id = in->roles.emergency_profile_id;
    } else if (in->stability_rollback_active) {
        ref_id = in->stability_rollback_profile_id;
    } else if (tuning_sensor_health_integrity_failed(&in->sensors) ||
               !in->trusted_time_valid ||
               in->climate_request == TUNING_CLIMATE_REQ_FAIL_SAFE ||
               in->climate_request == TUNING_CLIMATE_REQ_HOT_PROFILE) {
        ref_id = in->roles.hot_day_profile_id;
    } else if (in->climate_request == TUNING_CLIMATE_REQ_COOL_PROFILE) {
        ref_id = in->roles.cool_day_profile_id;
    } else {
        /* RETAIN: known current profile bounds the ceiling; otherwise be
         * conservative and use the hot-day profile. */
        if (in->current_profile_known) {
            const TuningProfile *cur = tuning_registry_find(
                env->profiles, env->profile_count, in->current_profile_id);
            if (cur != NULL) {
                *out_rank = cur->rank;
                return true;
            }
        }
        ref_id = in->roles.hot_day_profile_id;
    }

    const TuningProfile *p = resolve_eligible(env, ref_id);
    if (p == NULL) {
        return false; /* fail closed — overrides will be rejected */
    }
    *out_rank = p->rank;
    return true;
}

TuningPolicyError tuning_policy_evaluate(const TuningPolicyEnvironment *env,
                                         const TuningPolicyInput *in,
                                         TuningSelectionIntent *out)
{
    if (env == NULL || in == NULL || out == NULL) {
        return TUNING_POLICY_ERR_NULL;
    }
    if (env->profiles == NULL || env->profile_count == 0) {
        return TUNING_POLICY_ERR_ENVIRONMENT;
    }
    if ((unsigned)in->climate_request >= TUNING_CLIMATE_REQ__COUNT ||
        (unsigned)in->mining >= TUNING_MINING__COUNT ||
        (unsigned)in->cooldown.source >= TUNING_ACTOR__COUNT ||
        !sensor_status_in_range(in->sensors.asic_temp) ||
        !sensor_status_in_range(in->sensors.vrm_temp) ||
        !sensor_status_in_range(in->sensors.fan_tach) ||
        (in->manual_present && (unsigned)in->manual.actor >= TUNING_ACTOR__COUNT)) {
        return TUNING_POLICY_ERR_ENUM_RANGE;
    }

    TuningSelectionIntent r;
    memset(&r, 0, sizeof(r));
    r.policy_generation = in->policy_generation;
    /* An active override that a safety source (1-4) preempts was never
     * evaluated — report NOT_EVALUATED, not "inactive". Level 5 overwrites
     * this with the real evaluation result when it is reached. */
    r.override_result = (in->manual_present && in->manual.active)
                            ? TUNING_OVERRIDE_NOT_EVALUATED
                            : TUNING_OVERRIDE_ERR_INACTIVE;

    const TuningProfile *cur = NULL;
    if (in->current_profile_known) {
        cur = tuning_registry_find(env->profiles, env->profile_count,
                                   in->current_profile_id);
    }

    /* ---- 1. EMERGENCY_THERMAL ---------------------------------------- */
    if (in->emergency_thermal_active) {
        if (!safety_select(env, cur, in->roles.emergency_profile_id,
                           TUNING_ACTOR_THERMAL,
                           TUNING_SOURCE_EMERGENCY_THERMAL,
                           TUNING_REASON_EMERGENCY_THERMAL_ACTIVE, &r)) {
            /* No eligible emergency profile: the existing hardcoded
             * firmware protection remains authoritative; surface the gap. */
            r.action = TUNING_SELECT_OPERATOR_RECOVERY;
            r.actor = TUNING_ACTOR_THERMAL;
            r.winning_source = TUNING_SOURCE_EMERGENCY_THERMAL;
            r.reason = TUNING_REASON_EMERGENCY_THERMAL_ACTIVE;
            r.secondary_reason = TUNING_REASON_NO_VALID_EMERGENCY_PROFILE;
        }
        *out = r;
        return TUNING_POLICY_OK;
    }

    /* ---- 2. STABILITY_OR_REBOOT_ROLLBACK ----------------------------- */
    if (in->stability_rollback_active) {
        if (in->stability_rollback_profile_id[0] == '\0' ||
            !safety_select(env, cur, in->stability_rollback_profile_id,
                           TUNING_ACTOR_STABILITY,
                           TUNING_SOURCE_STABILITY_ROLLBACK,
                           TUNING_REASON_STABILITY_ROLLBACK_ACTIVE, &r)) {
            r.action = TUNING_SELECT_RETAIN_INHIBIT;
            r.actor = TUNING_ACTOR_STABILITY;
            r.winning_source = TUNING_SOURCE_STABILITY_ROLLBACK;
            r.reason = TUNING_REASON_STABILITY_ROLLBACK_ACTIVE;
            r.secondary_reason = TUNING_REASON_NO_VALID_ROLLBACK_PROFILE;
        }
        *out = r;
        return TUNING_POLICY_OK;
    }

    /* ---- 3. SENSOR_INTEGRITY_FAILURE --------------------------------- */
    if (tuning_sensor_health_integrity_failed(&in->sensors)) {
        if (safety_select(env, cur, in->roles.hot_day_profile_id,
                          TUNING_ACTOR_SENSOR_SAFETY,
                          TUNING_SOURCE_SENSOR_INTEGRITY,
                          TUNING_REASON_SENSOR_INTEGRITY_FAILED, &r)) {
            *out = r;
            return TUNING_POLICY_OK;
        }
        /* Hot-day role ineligible: escalate to the emergency role. */
        if (safety_select(env, cur, in->roles.emergency_profile_id,
                          TUNING_ACTOR_SENSOR_SAFETY,
                          TUNING_SOURCE_SENSOR_INTEGRITY,
                          TUNING_REASON_SENSOR_INTEGRITY_FAILED, &r)) {
            r.secondary_reason = TUNING_REASON_NO_VALID_HOT_PROFILE;
            *out = r;
            return TUNING_POLICY_OK;
        }
        r.action = TUNING_SELECT_RETAIN_INHIBIT;
        r.actor = TUNING_ACTOR_SENSOR_SAFETY;
        r.winning_source = TUNING_SOURCE_SENSOR_INTEGRITY;
        r.reason = TUNING_REASON_SENSOR_INTEGRITY_FAILED;
        r.secondary_reason = TUNING_REASON_NO_VALID_HOT_PROFILE;
        *out = r;
        return TUNING_POLICY_OK;
    }

    /* ---- 4. WEATHER_API_OR_TIME_FAIL_SAFE ---------------------------- */
    if (!in->trusted_time_valid ||
        in->climate_request == TUNING_CLIMATE_REQ_FAIL_SAFE) {
        TuningReasonCode primary = in->trusted_time_valid
                                       ? TUNING_REASON_WEATHER_FAIL_SAFE
                                       : TUNING_REASON_TIME_UNTRUSTED;
        if (!safety_select(env, cur, in->roles.hot_day_profile_id,
                           TUNING_ACTOR_WEATHER_FAIL_SAFE,
                           TUNING_SOURCE_WEATHER_FAIL_SAFE, primary, &r)) {
            /* Task brief: no valid Hot Weather Safe -> retain last-known-
             * safe, inhibit upgrades, surface operator recovery. */
            r.action = TUNING_SELECT_OPERATOR_RECOVERY;
            r.actor = TUNING_ACTOR_WEATHER_FAIL_SAFE;
            r.winning_source = TUNING_SOURCE_WEATHER_FAIL_SAFE;
            r.reason = primary;
            r.secondary_reason = TUNING_REASON_NO_VALID_HOT_PROFILE;
        }
        *out = r;
        return TUNING_POLICY_OK;
    }

    /* ---- 5. MANUAL_OVERRIDE_WITHIN_SAFETY_CEILING -------------------- */
    if (in->manual_present && in->manual.active) {
        uint8_t ceiling = 0;
        bool ceiling_valid = tuning_policy_effective_ceiling(env, in, &ceiling);
        const TuningProfile *req = tuning_registry_find(
            env->profiles, env->profile_count, in->manual.profile_id);
        TuningOverrideResult ovr = tuning_override_eligible(
            &in->manual, req, &env->hw, ceiling, ceiling_valid,
            cur != NULL ? cur->rank : 0, cur != NULL, in->cooldown.active,
            in->now_epoch_s, in->trusted_time_valid);
        r.override_result = ovr;
        if (ovr == TUNING_OVERRIDE_OK) {
            r.actor = in->manual.actor;
            r.winning_source = TUNING_SOURCE_MANUAL_OVERRIDE;
            r.reason = TUNING_REASON_OVERRIDE_APPLIED;
            if (cur != NULL && req == cur) {
                r.action = TUNING_SELECT_NONE;
                r.secondary_reason = TUNING_REASON_ALREADY_ACTIVE;
            } else {
                r.action = TUNING_SELECT_PROFILE;
                copy_id(r.profile_id, req->id);
                set_direction(&r, req, cur);
                if (cur == NULL) {
                    r.secondary_reason = TUNING_REASON_CURRENT_UNKNOWN;
                }
            }
            *out = r;
            return TUNING_POLICY_OK;
        }
        /* A rejected override never blocks normal policy; the rejection is
         * surfaced via override_result on the final intent. */
    }

    /* ---- 6. NORMAL_WEATHER_POLICY ------------------------------------ */
    if (in->climate_request == TUNING_CLIMATE_REQ_HOT_PROFILE) {
        if (!safety_select(env, cur, in->roles.hot_day_profile_id,
                           TUNING_ACTOR_WEATHER_POLICY,
                           TUNING_SOURCE_WEATHER_POLICY,
                           TUNING_REASON_WEATHER_HOT_FORECAST, &r)) {
            r.action = TUNING_SELECT_OPERATOR_RECOVERY;
            r.actor = TUNING_ACTOR_WEATHER_POLICY;
            r.winning_source = TUNING_SOURCE_WEATHER_POLICY;
            r.reason = TUNING_REASON_WEATHER_HOT_FORECAST;
            r.secondary_reason = TUNING_REASON_NO_VALID_HOT_PROFILE;
        }
        *out = r;
        return TUNING_POLICY_OK;
    }

    if (in->climate_request == TUNING_CLIMATE_REQ_COOL_PROFILE) {
        r.actor = TUNING_ACTOR_WEATHER_POLICY;
        r.winning_source = TUNING_SOURCE_WEATHER_POLICY;
        r.reason = TUNING_REASON_WEATHER_COOL_ELIGIBLE;
        r.action = TUNING_SELECT_NONE;

        const TuningProfile *p =
            resolve_eligible(env, in->roles.cool_day_profile_id);
        if (p == NULL) {
            r.secondary_reason = TUNING_REASON_PROFILE_NOT_ELIGIBLE;
        } else if (cur != NULL && p == cur) {
            r.secondary_reason = TUNING_REASON_ALREADY_ACTIVE;
        } else if (cur == NULL) {
            /* An upgrade from an unknown current is never automatic. */
            r.secondary_reason = TUNING_REASON_CURRENT_UNKNOWN;
        } else if (p->rank < cur->rank) {
            /* Cool-day role configured milder than current: a downgrade,
             * allowed immediately. */
            r.action = TUNING_SELECT_PROFILE;
            copy_id(r.profile_id, p->id);
            set_direction(&r, p, cur);
        } else {
            /* Upgrade path — boundary-16 gates, first failure named.
             * (Trusted time already passed: source 4 catches untrusted.) */
            if (!tuning_sensor_health_upgrade_ok(&in->sensors)) {
                r.secondary_reason = TUNING_REASON_SENSORS_NOT_UPGRADE_OK;
            } else if (in->cooldown.active) {
                r.secondary_reason = TUNING_REASON_COOLDOWN_ACTIVE;
            } else if (in->mining != TUNING_MINING_STABLE) {
                r.secondary_reason = TUNING_REASON_MINING_NOT_STABLE;
            } else if (in->upgrade_inhibited) {
                r.secondary_reason = TUNING_REASON_UPGRADE_INHIBITED;
            } else {
                r.action = TUNING_SELECT_PROFILE;
                copy_id(r.profile_id, p->id);
                set_direction(&r, p, cur);
            }
        }
        *out = r;
        return TUNING_POLICY_OK;
    }

    /* ---- 7. CURRENT_VALIDATED_PROFILE (climate RETAIN) --------------- */
    r.action = TUNING_SELECT_NONE;
    r.actor = TUNING_ACTOR_NONE;
    r.winning_source = TUNING_SOURCE_CURRENT_PROFILE;
    r.reason = TUNING_REASON_CURRENT_RETAINED;
    r.secondary_reason = TUNING_REASON_CLIMATE_UNKNOWN;
    *out = r;
    return TUNING_POLICY_OK;
}

/* ------------------------------------------------------------------ */
/* Token strings                                                       */
/* ------------------------------------------------------------------ */

const char *tuning_actor_str(TuningActorClass a)
{
    switch (a) {
    case TUNING_ACTOR_NONE: return "NONE";
    case TUNING_ACTOR_THERMAL: return "THERMAL";
    case TUNING_ACTOR_STABILITY: return "STABILITY";
    case TUNING_ACTOR_SENSOR_SAFETY: return "SENSOR_SAFETY";
    case TUNING_ACTOR_WEATHER_POLICY: return "WEATHER_POLICY";
    case TUNING_ACTOR_WEATHER_FAIL_SAFE: return "WEATHER_FAIL_SAFE";
    case TUNING_ACTOR_MANUAL_API: return "MANUAL_API";
    case TUNING_ACTOR_BOOT_RECOVERY: return "BOOT_RECOVERY";
    case TUNING_ACTOR_OPERATOR_RECOVERY: return "OPERATOR_RECOVERY";
    default: return "UNKNOWN";
    }
}

const char *tuning_source_str(TuningPrecedenceSource s)
{
    switch (s) {
    case TUNING_SOURCE_NONE: return "NONE";
    case TUNING_SOURCE_EMERGENCY_THERMAL: return "EMERGENCY_THERMAL";
    case TUNING_SOURCE_STABILITY_ROLLBACK: return "STABILITY_ROLLBACK";
    case TUNING_SOURCE_SENSOR_INTEGRITY: return "SENSOR_INTEGRITY";
    case TUNING_SOURCE_WEATHER_FAIL_SAFE: return "WEATHER_FAIL_SAFE";
    case TUNING_SOURCE_MANUAL_OVERRIDE: return "MANUAL_OVERRIDE";
    case TUNING_SOURCE_WEATHER_POLICY: return "WEATHER_POLICY";
    case TUNING_SOURCE_CURRENT_PROFILE: return "CURRENT_PROFILE";
    default: return "UNKNOWN";
    }
}

const char *tuning_weather_state_str(TuningWeatherState s)
{
    switch (s) {
    case TUNING_WEATHER_STATE_UNKNOWN: return "UNKNOWN";
    case TUNING_WEATHER_STATE_NORMAL: return "NORMAL";
    case TUNING_WEATHER_STATE_HOT: return "HOT";
    default: return "INVALID";
    }
}

const char *tuning_forecast_status_str(TuningForecastStatus s)
{
    switch (s) {
    case TUNING_FORECAST_OK: return "OK";
    case TUNING_FORECAST_STALE: return "STALE";
    case TUNING_FORECAST_INVALID: return "INVALID";
    case TUNING_FORECAST_UNAVAILABLE: return "UNAVAILABLE";
    default: return "UNKNOWN";
    }
}

const char *tuning_climate_request_str(TuningClimateRequest r)
{
    switch (r) {
    case TUNING_CLIMATE_REQ_RETAIN: return "RETAIN";
    case TUNING_CLIMATE_REQ_COOL_PROFILE: return "COOL_PROFILE";
    case TUNING_CLIMATE_REQ_HOT_PROFILE: return "HOT_PROFILE";
    case TUNING_CLIMATE_REQ_FAIL_SAFE: return "FAIL_SAFE";
    default: return "UNKNOWN";
    }
}

const char *tuning_sensor_status_str(TuningSensorStatus s)
{
    switch (s) {
    case TUNING_SENSOR_OK: return "OK";
    case TUNING_SENSOR_MISSING: return "MISSING";
    case TUNING_SENSOR_INVALID: return "INVALID";
    case TUNING_SENSOR_STALE: return "STALE";
    case TUNING_SENSOR_IMPLAUSIBLE: return "IMPLAUSIBLE";
    default: return "UNKNOWN";
    }
}

const char *tuning_mining_health_str(TuningMiningHealth m)
{
    switch (m) {
    case TUNING_MINING_UNKNOWN: return "UNKNOWN";
    case TUNING_MINING_STABLE: return "STABLE";
    case TUNING_MINING_DEGRADED: return "DEGRADED";
    case TUNING_MINING_UNSTABLE: return "UNSTABLE";
    default: return "INVALID";
    }
}

const char *tuning_selection_action_str(TuningSelectionAction a)
{
    switch (a) {
    case TUNING_SELECT_NONE: return "NONE";
    case TUNING_SELECT_PROFILE: return "SELECT_PROFILE";
    case TUNING_SELECT_RETAIN_INHIBIT: return "RETAIN_INHIBIT";
    case TUNING_SELECT_OPERATOR_RECOVERY: return "OPERATOR_RECOVERY";
    default: return "UNKNOWN";
    }
}

const char *tuning_reason_str(TuningReasonCode r)
{
    switch (r) {
    case TUNING_REASON_NONE: return "NONE";
    case TUNING_REASON_EMERGENCY_THERMAL_ACTIVE: return "EMERGENCY_THERMAL_ACTIVE";
    case TUNING_REASON_STABILITY_ROLLBACK_ACTIVE: return "STABILITY_ROLLBACK_ACTIVE";
    case TUNING_REASON_SENSOR_INTEGRITY_FAILED: return "SENSOR_INTEGRITY_FAILED";
    case TUNING_REASON_WEATHER_FAIL_SAFE: return "WEATHER_FAIL_SAFE";
    case TUNING_REASON_TIME_UNTRUSTED: return "TIME_UNTRUSTED";
    case TUNING_REASON_WEATHER_HOT_FORECAST: return "WEATHER_HOT_FORECAST";
    case TUNING_REASON_WEATHER_COOL_ELIGIBLE: return "WEATHER_COOL_ELIGIBLE";
    case TUNING_REASON_WEATHER_RETAIN_BAND: return "WEATHER_RETAIN_BAND";
    case TUNING_REASON_CLIMATE_UNKNOWN: return "CLIMATE_UNKNOWN";
    case TUNING_REASON_ALREADY_ACTIVE: return "ALREADY_ACTIVE";
    case TUNING_REASON_RETAINED_SAFER_CURRENT: return "RETAINED_SAFER_CURRENT";
    case TUNING_REASON_CURRENT_UNKNOWN: return "CURRENT_UNKNOWN";
    case TUNING_REASON_PROFILE_NOT_ELIGIBLE: return "PROFILE_NOT_ELIGIBLE";
    case TUNING_REASON_NO_VALID_HOT_PROFILE: return "NO_VALID_HOT_PROFILE";
    case TUNING_REASON_NO_VALID_EMERGENCY_PROFILE: return "NO_VALID_EMERGENCY_PROFILE";
    case TUNING_REASON_NO_VALID_ROLLBACK_PROFILE: return "NO_VALID_ROLLBACK_PROFILE";
    case TUNING_REASON_COOLDOWN_ACTIVE: return "COOLDOWN_ACTIVE";
    case TUNING_REASON_MINING_NOT_STABLE: return "MINING_NOT_STABLE";
    case TUNING_REASON_UPGRADE_INHIBITED: return "UPGRADE_INHIBITED";
    case TUNING_REASON_SENSORS_NOT_UPGRADE_OK: return "SENSORS_NOT_UPGRADE_OK";
    case TUNING_REASON_OVERRIDE_APPLIED: return "OVERRIDE_APPLIED";
    case TUNING_REASON_OVERRIDE_REJECTED: return "OVERRIDE_REJECTED";
    case TUNING_REASON_CURRENT_RETAINED: return "CURRENT_RETAINED";
    default: return "UNKNOWN";
    }
}

const char *tuning_policy_error_str(TuningPolicyError e)
{
    switch (e) {
    case TUNING_POLICY_OK: return "OK";
    case TUNING_POLICY_ERR_NULL: return "ERR_NULL";
    case TUNING_POLICY_ERR_THRESHOLDS: return "ERR_THRESHOLDS";
    case TUNING_POLICY_ERR_ENUM_RANGE: return "ERR_ENUM_RANGE";
    case TUNING_POLICY_ERR_ENVIRONMENT: return "ERR_ENVIRONMENT";
    default: return "ERR_UNKNOWN";
    }
}

const char *tuning_override_result_str(TuningOverrideResult r)
{
    switch (r) {
    case TUNING_OVERRIDE_OK: return "OK";
    case TUNING_OVERRIDE_ERR_NULL: return "ERR_NULL";
    case TUNING_OVERRIDE_ERR_INACTIVE: return "ERR_INACTIVE";
    case TUNING_OVERRIDE_ERR_ACTOR: return "ERR_ACTOR";
    case TUNING_OVERRIDE_ERR_NO_EXPIRATION: return "ERR_NO_EXPIRATION";
    case TUNING_OVERRIDE_ERR_EXPIRATION_ORDER: return "ERR_EXPIRATION_ORDER";
    case TUNING_OVERRIDE_ERR_EXPIRED: return "ERR_EXPIRED";
    case TUNING_OVERRIDE_ERR_TIME_UNTRUSTED: return "ERR_TIME_UNTRUSTED";
    case TUNING_OVERRIDE_ERR_PROFILE_UNKNOWN: return "ERR_PROFILE_UNKNOWN";
    case TUNING_OVERRIDE_ERR_NOT_ELIGIBLE: return "ERR_NOT_ELIGIBLE";
    case TUNING_OVERRIDE_ERR_ABOVE_CEILING: return "ERR_ABOVE_CEILING";
    case TUNING_OVERRIDE_ERR_CEILING_UNKNOWN: return "ERR_CEILING_UNKNOWN";
    case TUNING_OVERRIDE_ERR_COOLDOWN_UPGRADE: return "ERR_COOLDOWN_UPGRADE";
    case TUNING_OVERRIDE_NOT_EVALUATED: return "NOT_EVALUATED";
    default: return "ERR_UNKNOWN";
    }
}
