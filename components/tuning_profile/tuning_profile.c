/*
 * NeuralAxe Weather-Aware Tuning — pure tuning-profile model (Gate W1).
 * PURE: no I/O, no heap, no clock, no logging, no globals mutated.
 */

#include <string.h>

#include "tuning_profile.h"

/* ------------------------------------------------------------------ */
/* Board capability table                                              */
/* ------------------------------------------------------------------ */

/*
 * Gamma 601: the frequency window documents the current UI/product option
 * ceiling (BM1370 dropdown maximum) — NOT a hardware-safety claim — and is
 * never raised by this feature. NO voltage window is encoded: no
 * owner-validated safe voltage ceiling exists, and regulator acceptance
 * limits are not safe tuning limits, so voltage payloads fail closed.
 */
static const TuningBoardCapability s_capabilities[] = {
    {
        .board = TUNING_BOARD_GAMMA_601,
        .asic = TUNING_ASIC_BM1370,
        .min_frequency_mhz = TUNING_GAMMA601_MIN_FREQUENCY_MHZ,
        .max_frequency_mhz = TUNING_GAMMA601_MAX_FREQUENCY_MHZ,
        .voltage_window_known = false,
        .min_core_voltage_mv = 0,
        .max_core_voltage_mv = 0,
    },
};

const TuningBoardCapability *tuning_board_capability(TuningBoardClass board,
                                                     TuningAsicClass asic)
{
    for (size_t i = 0; i < sizeof(s_capabilities) / sizeof(s_capabilities[0]); i++) {
        if (s_capabilities[i].board == board && s_capabilities[i].asic == asic) {
            return &s_capabilities[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Gamma 601 registry                                                  */
/* ------------------------------------------------------------------ */

/*
 * Production descriptors: ids, ranks, compatibility metadata and the
 * rollback chain ONLY. All three are UNVALIDATED, evidence-free and
 * payload-free — a currently running operating point is NOT validation
 * evidence, and no tuning payload is trusted until a later validated
 * profile source supplies one under committed, owner-approved physical
 * validation evidence. None of these is automatically selectable in W1
 * (proven by test).
 */
static const TuningProfile s_registry_gamma601[TUNING_REGISTRY_GAMMA601_COUNT] = {
    {
        .model_version = TUNING_PROFILE_MODEL_VERSION,
        .id = TUNING_PROFILE_ID_SUPERSINK_MAX,
        .revision = 2,
        .board = TUNING_BOARD_GAMMA_601,
        .asic = TUNING_ASIC_BM1370,
        .required_cooling = TUNING_COOLING_SUPERSINK_DUAL_FAN,
        .required_psu = TUNING_PSU_UNSPECIFIED,
        .validation = TUNING_VALIDATION_UNVALIDATED,
        .evidence_fingerprint = 0,
        .rank = 2,
        .payload_present = false,
        .rollback_profile_id = TUNING_PROFILE_ID_HOT_WEATHER,
        .disabled = false,
    },
    {
        .model_version = TUNING_PROFILE_MODEL_VERSION,
        .id = TUNING_PROFILE_ID_HOT_WEATHER,
        .revision = 2,
        .board = TUNING_BOARD_GAMMA_601,
        .asic = TUNING_ASIC_BM1370,
        .required_cooling = TUNING_COOLING_UNSPECIFIED,
        .required_psu = TUNING_PSU_UNSPECIFIED,
        .validation = TUNING_VALIDATION_UNVALIDATED,
        .evidence_fingerprint = 0,
        .rank = 1,
        .payload_present = false,
        .rollback_profile_id = TUNING_PROFILE_ID_EMERGENCY,
        .disabled = false,
    },
    {
        .model_version = TUNING_PROFILE_MODEL_VERSION,
        .id = TUNING_PROFILE_ID_EMERGENCY,
        .revision = 2,
        .board = TUNING_BOARD_GAMMA_601,
        .asic = TUNING_ASIC_BM1370,
        .required_cooling = TUNING_COOLING_UNSPECIFIED,
        .required_psu = TUNING_PSU_UNSPECIFIED,
        .validation = TUNING_VALIDATION_UNVALIDATED,
        .evidence_fingerprint = 0,
        .rank = 0,
        .payload_present = false,
        .rollback_profile_id = "",
        .disabled = false,
    },
};

const TuningProfile *tuning_registry_gamma601(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = TUNING_REGISTRY_GAMMA601_COUNT;
    }
    return s_registry_gamma601;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Bounded NUL check: true iff a terminator exists within cap bytes. */
static bool str_bounded(const char *s, size_t cap)
{
    for (size_t i = 0; i < cap; i++) {
        if (s[i] == '\0') {
            return true;
        }
    }
    return false;
}

/* Stable-id charset: lowercase [a-z0-9-], non-empty, bounded. */
static bool id_valid(const char *id, size_t cap)
{
    if (!str_bounded(id, cap) || id[0] == '\0') {
        return false;
    }
    for (size_t i = 0; id[i] != '\0'; i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool ids_equal(const char *a, const char *b)
{
    return strncmp(a, b, TUNING_PROFILE_ID_MAX) == 0;
}

/* Validate one OPTIONAL thermal limit (0 = unspecified is always legal). */
static bool thermal_limit_ok(int16_t dc)
{
    if (dc == 0) {
        return true;
    }
    return dc >= TUNING_THERMAL_LIMIT_MIN_DC && dc <= TUNING_THERMAL_LIMIT_MAX_DC;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

TuningProfileError tuning_profile_validate_with_capability(
    const TuningProfile *p, const TuningBoardCapability *cap)
{
    if (p == NULL) {
        return TUNING_PROFILE_ERR_NULL;
    }
    if (p->model_version != TUNING_PROFILE_MODEL_VERSION) {
        return TUNING_PROFILE_ERR_MODEL_VERSION;
    }
    if (!id_valid(p->id, sizeof(p->id))) {
        return TUNING_PROFILE_ERR_ID_INVALID;
    }
    if (p->revision == 0) {
        return TUNING_PROFILE_ERR_REVISION_ZERO;
    }
    if ((unsigned)p->board >= TUNING_BOARD__COUNT ||
        (unsigned)p->asic >= TUNING_ASIC__COUNT ||
        (unsigned)p->required_cooling >= TUNING_COOLING__COUNT ||
        (unsigned)p->required_psu >= TUNING_PSU__COUNT ||
        (unsigned)p->validation >= TUNING_VALIDATION__COUNT) {
        return TUNING_PROFILE_ERR_ENUM_RANGE;
    }
    if (p->board == TUNING_BOARD_UNKNOWN) {
        return TUNING_PROFILE_ERR_BOARD_UNKNOWN;
    }
    if (p->asic == TUNING_ASIC_UNKNOWN) {
        return TUNING_PROFILE_ERR_ASIC_UNKNOWN;
    }
    if (cap == NULL) {
        return TUNING_PROFILE_ERR_NO_CAPABILITY;
    }

    if (p->rank > TUNING_PROFILE_RANK_MAX) {
        return TUNING_PROFILE_ERR_RANK_RANGE;
    }

    if (!p->payload_present) {
        /* Payload-free descriptor (every production profile in W1): all
         * payload fields must be zero — no covert tuning values. */
        if (p->frequency_mhz != 0 || p->core_voltage_mv != 0 ||
            p->fan_curve_count != 0 ||
            p->asic_warn_dc != 0 || p->asic_crit_dc != 0 ||
            p->vrm_warn_dc != 0 || p->vrm_crit_dc != 0) {
            return TUNING_PROFILE_ERR_PAYLOAD_NOT_EMPTY;
        }
    } else {
        if (p->frequency_mhz < cap->min_frequency_mhz ||
            p->frequency_mhz > cap->max_frequency_mhz) {
            return TUNING_PROFILE_ERR_FREQUENCY_RANGE;
        }
        if (!cap->voltage_window_known) {
            /* No owner-validated voltage window exists for this board:
             * any voltage-bearing payload fails closed. */
            return TUNING_PROFILE_ERR_VOLTAGE_UNPROVEN;
        }
        if (p->core_voltage_mv < cap->min_core_voltage_mv ||
            p->core_voltage_mv > cap->max_core_voltage_mv) {
            return TUNING_PROFILE_ERR_VOLTAGE_RANGE;
        }

        if (p->fan_curve_count != 0 &&
            p->fan_curve_count != TUNING_FAN_CURVE_MAX_POINTS) {
            return TUNING_PROFILE_ERR_FAN_CURVE_COUNT;
        }
        for (uint8_t i = 0; i < p->fan_curve_count; i++) {
            if (p->fan_curve[i].temp_c > TUNING_FAN_CURVE_TEMP_MAX_C ||
                p->fan_curve[i].fan_percent > TUNING_FAN_CURVE_PCT_MAX) {
                return TUNING_PROFILE_ERR_FAN_CURVE_POINT;
            }
            if (i > 0) {
                if (p->fan_curve[i].temp_c <= p->fan_curve[i - 1].temp_c) {
                    return TUNING_PROFILE_ERR_FAN_CURVE_ORDER;
                }
                if (p->fan_curve[i].fan_percent < p->fan_curve[i - 1].fan_percent) {
                    return TUNING_PROFILE_ERR_FAN_CURVE_ORDER;
                }
            }
        }

        if (!thermal_limit_ok(p->asic_warn_dc) || !thermal_limit_ok(p->asic_crit_dc) ||
            !thermal_limit_ok(p->vrm_warn_dc) || !thermal_limit_ok(p->vrm_crit_dc)) {
            return TUNING_PROFILE_ERR_THERMAL_RANGE;
        }
        if (p->asic_warn_dc != 0 && p->asic_crit_dc != 0 &&
            p->asic_warn_dc >= p->asic_crit_dc) {
            return TUNING_PROFILE_ERR_THERMAL_ORDER;
        }
        if (p->vrm_warn_dc != 0 && p->vrm_crit_dc != 0 &&
            p->vrm_warn_dc >= p->vrm_crit_dc) {
            return TUNING_PROFILE_ERR_THERMAL_ORDER;
        }
    }

    if (!str_bounded(p->rollback_profile_id, sizeof(p->rollback_profile_id))) {
        return TUNING_PROFILE_ERR_ROLLBACK_INVALID;
    }
    if (p->rollback_profile_id[0] != '\0') {
        if (!id_valid(p->rollback_profile_id, sizeof(p->rollback_profile_id))) {
            return TUNING_PROFILE_ERR_ROLLBACK_INVALID;
        }
        if (ids_equal(p->rollback_profile_id, p->id)) {
            return TUNING_PROFILE_ERR_ROLLBACK_INVALID;
        }
    }

    if (p->validation == TUNING_VALIDATION_VALIDATED &&
        p->evidence_fingerprint == 0) {
        return TUNING_PROFILE_ERR_EVIDENCE_MISSING;
    }

    return TUNING_PROFILE_OK;
}

TuningProfileError tuning_profile_validate(const TuningProfile *p)
{
    return tuning_profile_validate_with_capability(
        p, p == NULL ? NULL : tuning_board_capability(p->board, p->asic));
}

/* ------------------------------------------------------------------ */
/* Compatibility + eligibility                                         */
/* ------------------------------------------------------------------ */

TuningCompatibilityResult tuning_profile_compatible(const TuningProfile *p,
                                                    const TuningHardwareContext *hw)
{
    if (p == NULL || hw == NULL) {
        return TUNING_COMPAT_ERR_NULL;
    }
    if (hw->board == TUNING_BOARD_UNKNOWN || hw->asic == TUNING_ASIC_UNKNOWN ||
        (unsigned)hw->board >= TUNING_BOARD__COUNT ||
        (unsigned)hw->asic >= TUNING_ASIC__COUNT ||
        (unsigned)hw->cooling_installed >= TUNING_COOLING__COUNT ||
        (unsigned)hw->psu_installed >= TUNING_PSU__COUNT) {
        return TUNING_COMPAT_ERR_CONTEXT_UNKNOWN;
    }
    if (p->board != hw->board) {
        return TUNING_COMPAT_ERR_BOARD_MISMATCH;
    }
    if (p->asic != hw->asic) {
        return TUNING_COMPAT_ERR_ASIC_MISMATCH;
    }
    /* Tier semantics: a requirement of UNSPECIFIED always passes; any real
     * requirement needs installed >= required (installed UNSPECIFIED fails). */
    if (p->required_cooling != TUNING_COOLING_UNSPECIFIED &&
        (hw->cooling_installed == TUNING_COOLING_UNSPECIFIED ||
         hw->cooling_installed < p->required_cooling)) {
        return TUNING_COMPAT_ERR_COOLING_INSUFFICIENT;
    }
    if (p->required_psu != TUNING_PSU_UNSPECIFIED &&
        (hw->psu_installed == TUNING_PSU_UNSPECIFIED ||
         hw->psu_installed < p->required_psu)) {
        return TUNING_COMPAT_ERR_PSU_INSUFFICIENT;
    }
    return TUNING_COMPAT_OK;
}

TuningEligibilityResult tuning_profile_auto_eligible(const TuningProfile *p,
                                                     const TuningHardwareContext *hw)
{
    if (p == NULL || hw == NULL) {
        return TUNING_ELIGIBLE_ERR_NULL;
    }
    if (tuning_profile_validate(p) != TUNING_PROFILE_OK) {
        return TUNING_ELIGIBLE_ERR_INVALID;
    }
    if (p->validation == TUNING_VALIDATION_RETIRED) {
        return TUNING_ELIGIBLE_ERR_RETIRED;
    }
    if (p->validation == TUNING_VALIDATION_INCOMPATIBLE) {
        return TUNING_ELIGIBLE_ERR_MARKED_INCOMPATIBLE;
    }
    if (p->validation != TUNING_VALIDATION_VALIDATED) {
        return TUNING_ELIGIBLE_ERR_NOT_VALIDATED;
    }
    if (p->disabled) {
        return TUNING_ELIGIBLE_ERR_DISABLED;
    }
    if (tuning_profile_compatible(p, hw) != TUNING_COMPAT_OK) {
        return TUNING_ELIGIBLE_ERR_INCOMPATIBLE;
    }
    return TUNING_ELIGIBLE_OK;
}

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

TuningRegistryResult tuning_registry_validate(const TuningProfile *profiles,
                                              size_t count)
{
    if (profiles == NULL) {
        return TUNING_REGISTRY_ERR_NULL;
    }
    if (count == 0) {
        return TUNING_REGISTRY_ERR_EMPTY;
    }
    for (size_t i = 0; i < count; i++) {
        if (tuning_profile_validate(&profiles[i]) != TUNING_PROFILE_OK) {
            return TUNING_REGISTRY_ERR_PROFILE_INVALID;
        }
    }
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (ids_equal(profiles[i].id, profiles[j].id)) {
                return TUNING_REGISTRY_ERR_DUPLICATE_ID;
            }
            if (profiles[i].rank == profiles[j].rank) {
                return TUNING_REGISTRY_ERR_DUPLICATE_RANK;
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (profiles[i].rollback_profile_id[0] == '\0') {
            continue;
        }
        const TuningProfile *rb =
            tuning_registry_find(profiles, count, profiles[i].rollback_profile_id);
        if (rb == NULL) {
            return TUNING_REGISTRY_ERR_ROLLBACK_UNRESOLVED;
        }
        if (rb->rank >= profiles[i].rank) {
            return TUNING_REGISTRY_ERR_ROLLBACK_NOT_SAFER;
        }
    }
    return TUNING_REGISTRY_OK;
}

const TuningProfile *tuning_registry_find(const TuningProfile *profiles,
                                          size_t count,
                                          const char *profile_id)
{
    if (profiles == NULL || profile_id == NULL || profile_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (ids_equal(profiles[i].id, profile_id)) {
            return &profiles[i];
        }
    }
    return NULL;
}

const char *tuning_profile_display_label(const char *profile_id)
{
    if (profile_id == NULL) {
        return "Unknown Profile";
    }
    if (ids_equal(profile_id, TUNING_PROFILE_ID_SUPERSINK_MAX)) {
        return "SuperSink Max";
    }
    if (ids_equal(profile_id, TUNING_PROFILE_ID_HOT_WEATHER)) {
        return "Hot Weather Safe";
    }
    if (ids_equal(profile_id, TUNING_PROFILE_ID_EMERGENCY)) {
        return "Emergency Thermal Safe";
    }
    return "Unknown Profile";
}

/* ------------------------------------------------------------------ */
/* Token strings                                                       */
/* ------------------------------------------------------------------ */

const char *tuning_profile_error_str(TuningProfileError e)
{
    switch (e) {
    case TUNING_PROFILE_OK: return "OK";
    case TUNING_PROFILE_ERR_NULL: return "ERR_NULL";
    case TUNING_PROFILE_ERR_MODEL_VERSION: return "ERR_MODEL_VERSION";
    case TUNING_PROFILE_ERR_ID_INVALID: return "ERR_ID_INVALID";
    case TUNING_PROFILE_ERR_REVISION_ZERO: return "ERR_REVISION_ZERO";
    case TUNING_PROFILE_ERR_ENUM_RANGE: return "ERR_ENUM_RANGE";
    case TUNING_PROFILE_ERR_BOARD_UNKNOWN: return "ERR_BOARD_UNKNOWN";
    case TUNING_PROFILE_ERR_ASIC_UNKNOWN: return "ERR_ASIC_UNKNOWN";
    case TUNING_PROFILE_ERR_NO_CAPABILITY: return "ERR_NO_CAPABILITY";
    case TUNING_PROFILE_ERR_FREQUENCY_RANGE: return "ERR_FREQUENCY_RANGE";
    case TUNING_PROFILE_ERR_VOLTAGE_RANGE: return "ERR_VOLTAGE_RANGE";
    case TUNING_PROFILE_ERR_RANK_RANGE: return "ERR_RANK_RANGE";
    case TUNING_PROFILE_ERR_FAN_CURVE_COUNT: return "ERR_FAN_CURVE_COUNT";
    case TUNING_PROFILE_ERR_FAN_CURVE_POINT: return "ERR_FAN_CURVE_POINT";
    case TUNING_PROFILE_ERR_FAN_CURVE_ORDER: return "ERR_FAN_CURVE_ORDER";
    case TUNING_PROFILE_ERR_THERMAL_RANGE: return "ERR_THERMAL_RANGE";
    case TUNING_PROFILE_ERR_THERMAL_ORDER: return "ERR_THERMAL_ORDER";
    case TUNING_PROFILE_ERR_ROLLBACK_INVALID: return "ERR_ROLLBACK_INVALID";
    case TUNING_PROFILE_ERR_EVIDENCE_MISSING: return "ERR_EVIDENCE_MISSING";
    case TUNING_PROFILE_ERR_PAYLOAD_NOT_EMPTY: return "ERR_PAYLOAD_NOT_EMPTY";
    case TUNING_PROFILE_ERR_VOLTAGE_UNPROVEN: return "ERR_VOLTAGE_UNPROVEN";
    default: return "ERR_UNKNOWN";
    }
}

const char *tuning_compat_str(TuningCompatibilityResult r)
{
    switch (r) {
    case TUNING_COMPAT_OK: return "OK";
    case TUNING_COMPAT_ERR_NULL: return "ERR_NULL";
    case TUNING_COMPAT_ERR_CONTEXT_UNKNOWN: return "ERR_CONTEXT_UNKNOWN";
    case TUNING_COMPAT_ERR_BOARD_MISMATCH: return "ERR_BOARD_MISMATCH";
    case TUNING_COMPAT_ERR_ASIC_MISMATCH: return "ERR_ASIC_MISMATCH";
    case TUNING_COMPAT_ERR_COOLING_INSUFFICIENT: return "ERR_COOLING_INSUFFICIENT";
    case TUNING_COMPAT_ERR_PSU_INSUFFICIENT: return "ERR_PSU_INSUFFICIENT";
    default: return "ERR_UNKNOWN";
    }
}

const char *tuning_eligibility_str(TuningEligibilityResult r)
{
    switch (r) {
    case TUNING_ELIGIBLE_OK: return "OK";
    case TUNING_ELIGIBLE_ERR_NULL: return "ERR_NULL";
    case TUNING_ELIGIBLE_ERR_INVALID: return "ERR_INVALID";
    case TUNING_ELIGIBLE_ERR_NOT_VALIDATED: return "ERR_NOT_VALIDATED";
    case TUNING_ELIGIBLE_ERR_RETIRED: return "ERR_RETIRED";
    case TUNING_ELIGIBLE_ERR_MARKED_INCOMPATIBLE: return "ERR_MARKED_INCOMPATIBLE";
    case TUNING_ELIGIBLE_ERR_DISABLED: return "ERR_DISABLED";
    case TUNING_ELIGIBLE_ERR_INCOMPATIBLE: return "ERR_INCOMPATIBLE";
    default: return "ERR_UNKNOWN";
    }
}

const char *tuning_registry_result_str(TuningRegistryResult r)
{
    switch (r) {
    case TUNING_REGISTRY_OK: return "OK";
    case TUNING_REGISTRY_ERR_NULL: return "ERR_NULL";
    case TUNING_REGISTRY_ERR_EMPTY: return "ERR_EMPTY";
    case TUNING_REGISTRY_ERR_PROFILE_INVALID: return "ERR_PROFILE_INVALID";
    case TUNING_REGISTRY_ERR_DUPLICATE_ID: return "ERR_DUPLICATE_ID";
    case TUNING_REGISTRY_ERR_DUPLICATE_RANK: return "ERR_DUPLICATE_RANK";
    case TUNING_REGISTRY_ERR_ROLLBACK_UNRESOLVED: return "ERR_ROLLBACK_UNRESOLVED";
    case TUNING_REGISTRY_ERR_ROLLBACK_NOT_SAFER: return "ERR_ROLLBACK_NOT_SAFER";
    default: return "ERR_UNKNOWN";
    }
}

const char *tuning_validation_state_str(TuningValidationState s)
{
    switch (s) {
    case TUNING_VALIDATION_UNVALIDATED: return "UNVALIDATED";
    case TUNING_VALIDATION_VALIDATED: return "VALIDATED";
    case TUNING_VALIDATION_RETIRED: return "RETIRED";
    case TUNING_VALIDATION_INCOMPATIBLE: return "INCOMPATIBLE";
    default: return "UNKNOWN";
    }
}
