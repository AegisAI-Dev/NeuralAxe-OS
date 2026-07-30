/*
 * Exhaustive deterministic tests for the pure tuning-profile model (W1).
 * No hardware, no network. Production registry entries are UNVALIDATED,
 * payload-free descriptors; every VALIDATED profile and every tuning
 * payload below is a SYNTHETIC TEST FIXTURE — nothing here applies tuning
 * to anything, and no fixture value is a production safety claim.
 */

#include <string.h>
#include "unity.h"
#include "tuning_profile.h"

/* ---------------- fixtures ---------------- */

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

/* SYNTHETIC injected capability window for payload-mechanics tests only.
 * The voltage numbers are test fixtures, NOT validated board limits — the
 * production static table has voltage_window_known == false. */
static TuningBoardCapability synthetic_cap(void)
{
    TuningBoardCapability cap;
    memset(&cap, 0, sizeof(cap));
    cap.board = TUNING_BOARD_GAMMA_601;
    cap.asic = TUNING_ASIC_BM1370;
    cap.min_frequency_mhz = 50;
    cap.max_frequency_mhz = 625;
    cap.voltage_window_known = true;
    cap.min_core_voltage_mv = 1000;
    cap.max_core_voltage_mv = 1250;
    return cap;
}

/* Payload-free synthetic descriptor (passes plain validate). */
static TuningProfile descriptor_profile(void)
{
    TuningProfile p;
    memset(&p, 0, sizeof(p));
    p.model_version = TUNING_PROFILE_MODEL_VERSION;
    strncpy(p.id, "fixture-profile", sizeof(p.id) - 1);
    p.revision = 1;
    p.board = TUNING_BOARD_GAMMA_601;
    p.asic = TUNING_ASIC_BM1370;
    p.required_cooling = TUNING_COOLING_UNSPECIFIED;
    p.required_psu = TUNING_PSU_UNSPECIFIED;
    p.validation = TUNING_VALIDATION_VALIDATED; /* synthetic fixture only */
    p.evidence_fingerprint = 0xF00Du;           /* synthetic tag          */
    p.rank = 3;
    p.payload_present = false;
    p.rollback_profile_id[0] = '\0';
    p.disabled = false;
    return p;
}

/* Payload-bearing synthetic fixture (validated with synthetic_cap()). */
static TuningProfile payload_profile(void)
{
    TuningProfile p = descriptor_profile();
    p.payload_present = true;
    p.frequency_mhz = 525;
    p.core_voltage_mv = 1150;
    return p;
}

/* ---------------- registry ---------------- */

TEST_CASE("registry: three profiles, expected ids, integrity OK", "[tuning_profile]")
{
    size_t n = 0;
    const TuningProfile *r = tuning_registry_gamma601(&n);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(TUNING_REGISTRY_GAMMA601_COUNT, n);
    TEST_ASSERT_EQUAL(TUNING_REGISTRY_OK, tuning_registry_validate(r, n));
    TEST_ASSERT_NOT_NULL(tuning_registry_find(r, n, TUNING_PROFILE_ID_SUPERSINK_MAX));
    TEST_ASSERT_NOT_NULL(tuning_registry_find(r, n, TUNING_PROFILE_ID_HOT_WEATHER));
    TEST_ASSERT_NOT_NULL(tuning_registry_find(r, n, TUNING_PROFILE_ID_EMERGENCY));
}

TEST_CASE("registry: every production profile is UNVALIDATED with no evidence", "[tuning_profile]")
{
    size_t n = 0;
    const TuningProfile *r = tuning_registry_gamma601(&n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL(TUNING_VALIDATION_UNVALIDATED, r[i].validation);
        TEST_ASSERT_EQUAL_UINT32(0, r[i].evidence_fingerprint);
    }
}

TEST_CASE("registry: production descriptors are payload-free (no tuning values)", "[tuning_profile]")
{
    size_t n = 0;
    const TuningProfile *r = tuning_registry_gamma601(&n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_FALSE(r[i].payload_present);
        TEST_ASSERT_EQUAL_UINT16(0, r[i].frequency_mhz);
        TEST_ASSERT_EQUAL_UINT16(0, r[i].core_voltage_mv);
        TEST_ASSERT_EQUAL_UINT8(0, r[i].fan_curve_count);
        TEST_ASSERT_EQUAL_INT16(0, r[i].asic_warn_dc);
        TEST_ASSERT_EQUAL_INT16(0, r[i].asic_crit_dc);
        TEST_ASSERT_EQUAL_INT16(0, r[i].vrm_warn_dc);
        TEST_ASSERT_EQUAL_INT16(0, r[i].vrm_crit_dc);
        TEST_ASSERT_FALSE(r[i].disabled);
    }
    /* Ranks stay a pure relative order, never a frequency. */
    const TuningProfile *ss = tuning_registry_find(r, n, TUNING_PROFILE_ID_SUPERSINK_MAX);
    const TuningProfile *hot = tuning_registry_find(r, n, TUNING_PROFILE_ID_HOT_WEATHER);
    const TuningProfile *em = tuning_registry_find(r, n, TUNING_PROFILE_ID_EMERGENCY);
    TEST_ASSERT_EQUAL_UINT8(2, ss->rank);
    TEST_ASSERT_EQUAL_UINT8(1, hot->rank);
    TEST_ASSERT_EQUAL_UINT8(0, em->rank);
}

TEST_CASE("registry: rollback chain descends rank and terminates", "[tuning_profile]")
{
    size_t n = 0;
    const TuningProfile *r = tuning_registry_gamma601(&n);
    const TuningProfile *ss = tuning_registry_find(r, n, TUNING_PROFILE_ID_SUPERSINK_MAX);
    const TuningProfile *hot = tuning_registry_find(r, n, TUNING_PROFILE_ID_HOT_WEATHER);
    const TuningProfile *em = tuning_registry_find(r, n, TUNING_PROFILE_ID_EMERGENCY);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, ss->rollback_profile_id);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_EMERGENCY, hot->rollback_profile_id);
    TEST_ASSERT_EQUAL_STRING("", em->rollback_profile_id);
}

/* ---------------- board capability ---------------- */

TEST_CASE("capability: gamma601 has the 625 option ceiling and NO voltage window", "[tuning_profile]")
{
    const TuningBoardCapability *cap =
        tuning_board_capability(TUNING_BOARD_GAMMA_601, TUNING_ASIC_BM1370);
    TEST_ASSERT_NOT_NULL(cap);
    TEST_ASSERT_EQUAL_UINT16(50, cap->min_frequency_mhz);
    TEST_ASSERT_EQUAL_UINT16(625, cap->max_frequency_mhz);
    /* No owner-validated voltage window exists — must fail closed. */
    TEST_ASSERT_FALSE(cap->voltage_window_known);
    TEST_ASSERT_EQUAL_UINT16(0, cap->min_core_voltage_mv);
    TEST_ASSERT_EQUAL_UINT16(0, cap->max_core_voltage_mv);
}

TEST_CASE("capability: unknown combinations have no row (fail closed)", "[tuning_profile]")
{
    TEST_ASSERT_NULL(tuning_board_capability(TUNING_BOARD_UNKNOWN, TUNING_ASIC_BM1370));
    TEST_ASSERT_NULL(tuning_board_capability(TUNING_BOARD_GAMMA_601, TUNING_ASIC_UNKNOWN));
    TEST_ASSERT_NULL(tuning_board_capability(TUNING_BOARD_UNKNOWN, TUNING_ASIC_UNKNOWN));
}

/* ---------------- validation: descriptors ---------------- */

TEST_CASE("validate: NULL and model version", "[tuning_profile]")
{
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_NULL, tuning_profile_validate(NULL));
    TuningProfile p = descriptor_profile();
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK, tuning_profile_validate(&p));
    p.model_version = 99;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_MODEL_VERSION, tuning_profile_validate(&p));
}

TEST_CASE("validate: id charset, emptiness and termination", "[tuning_profile]")
{
    TuningProfile p = descriptor_profile();
    p.id[0] = '\0';
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ID_INVALID, tuning_profile_validate(&p));

    p = descriptor_profile();
    memset(p.id, 'a', sizeof(p.id)); /* no terminator */
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ID_INVALID, tuning_profile_validate(&p));

    const char *bad[] = { "UpperCase", "with space", "under_score", "dot.id" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        p = descriptor_profile();
        memset(p.id, 0, sizeof(p.id));
        strncpy(p.id, bad[i], sizeof(p.id) - 1);
        TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ID_INVALID, tuning_profile_validate(&p));
    }
}

TEST_CASE("validate: revision and enum ranges", "[tuning_profile]")
{
    TuningProfile p = descriptor_profile();
    p.revision = 0;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_REVISION_ZERO, tuning_profile_validate(&p));

    p = descriptor_profile();
    p.board = (TuningBoardClass)TUNING_BOARD__COUNT;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ENUM_RANGE, tuning_profile_validate(&p));
    p = descriptor_profile();
    p.asic = (TuningAsicClass)99;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ENUM_RANGE, tuning_profile_validate(&p));
    p = descriptor_profile();
    p.required_cooling = (TuningCoolingClass)TUNING_COOLING__COUNT;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ENUM_RANGE, tuning_profile_validate(&p));
    p = descriptor_profile();
    p.required_psu = (TuningPsuClass)TUNING_PSU__COUNT;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ENUM_RANGE, tuning_profile_validate(&p));
    p = descriptor_profile();
    p.validation = (TuningValidationState)TUNING_VALIDATION__COUNT;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ENUM_RANGE, tuning_profile_validate(&p));

    p = descriptor_profile();
    p.board = TUNING_BOARD_UNKNOWN;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_BOARD_UNKNOWN, tuning_profile_validate(&p));
    p = descriptor_profile();
    p.asic = TUNING_ASIC_UNKNOWN;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ASIC_UNKNOWN, tuning_profile_validate(&p));
}

TEST_CASE("validate: payload-free descriptor rejects covert tuning values", "[tuning_profile]")
{
    TuningProfile p = descriptor_profile();
    p.frequency_mhz = 525; /* payload_present is false */
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_PAYLOAD_NOT_EMPTY, tuning_profile_validate(&p));

    p = descriptor_profile();
    p.core_voltage_mv = 1150;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_PAYLOAD_NOT_EMPTY, tuning_profile_validate(&p));

    p = descriptor_profile();
    p.fan_curve_count = TUNING_FAN_CURVE_MAX_POINTS;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_PAYLOAD_NOT_EMPTY, tuning_profile_validate(&p));

    p = descriptor_profile();
    p.asic_crit_dc = 750;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_PAYLOAD_NOT_EMPTY, tuning_profile_validate(&p));
}

TEST_CASE("validate: rank bound", "[tuning_profile]")
{
    TuningProfile p = descriptor_profile();
    p.rank = TUNING_PROFILE_RANK_MAX;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK, tuning_profile_validate(&p));
    p.rank = TUNING_PROFILE_RANK_MAX + 1;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_RANK_RANGE, tuning_profile_validate(&p));
}

TEST_CASE("validate: rollback id rules", "[tuning_profile]")
{
    TuningProfile p = descriptor_profile();
    strncpy(p.rollback_profile_id, p.id, sizeof(p.rollback_profile_id) - 1);
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ROLLBACK_INVALID, tuning_profile_validate(&p));

    p = descriptor_profile();
    strncpy(p.rollback_profile_id, "Bad Chars", sizeof(p.rollback_profile_id) - 1);
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ROLLBACK_INVALID, tuning_profile_validate(&p));

    p = descriptor_profile();
    memset(p.rollback_profile_id, 'x', sizeof(p.rollback_profile_id));
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_ROLLBACK_INVALID, tuning_profile_validate(&p));

    p = descriptor_profile();
    strncpy(p.rollback_profile_id, "other-profile", sizeof(p.rollback_profile_id) - 1);
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK, tuning_profile_validate(&p));
}

TEST_CASE("validate: VALIDATED requires nonzero evidence", "[tuning_profile]")
{
    TuningProfile p = descriptor_profile();
    p.evidence_fingerprint = 0;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_EVIDENCE_MISSING, tuning_profile_validate(&p));
    p.validation = TUNING_VALIDATION_UNVALIDATED;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK, tuning_profile_validate(&p));
    /* Evidence on a non-VALIDATED profile is permitted (e.g. RETIRED keeps
     * its history). */
    p.validation = TUNING_VALIDATION_RETIRED;
    p.evidence_fingerprint = 0xBEEF;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK, tuning_profile_validate(&p));
}

/* ---------------- validation: payload mechanics (synthetic) ---------------- */

TEST_CASE("validate: 626 MHz payload is rejected — option ceiling holds", "[tuning_profile]")
{
    /* Static table: the 625 product option ceiling binds payloads even
     * before the voltage fail-closed rule. */
    TuningProfile p = payload_profile();
    p.frequency_mhz = 626;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_FREQUENCY_RANGE, tuning_profile_validate(&p));

    /* Synthetic injected window: bounded mechanics fully testable. */
    TuningBoardCapability cap = synthetic_cap();
    p = payload_profile();
    p.frequency_mhz = 626;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_FREQUENCY_RANGE,
                      tuning_profile_validate_with_capability(&p, &cap));
    p.frequency_mhz = 625;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK,
                      tuning_profile_validate_with_capability(&p, &cap));
    p.frequency_mhz = 49;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_FREQUENCY_RANGE,
                      tuning_profile_validate_with_capability(&p, &cap));
    p.frequency_mhz = 50;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK,
                      tuning_profile_validate_with_capability(&p, &cap));
    p.frequency_mhz = 0;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_FREQUENCY_RANGE,
                      tuning_profile_validate_with_capability(&p, &cap));
}

TEST_CASE("validate: unknown voltage capability fails closed", "[tuning_profile]")
{
    /* Production static table: no validated voltage window on Gamma 601 —
     * ANY voltage-bearing payload is refused, whatever its value. */
    uint16_t volts[] = { 1000, 1150, 1250, 900, 2000 };
    for (size_t i = 0; i < sizeof(volts) / sizeof(volts[0]); i++) {
        TuningProfile p = payload_profile();
        p.core_voltage_mv = volts[i];
        TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_VOLTAGE_UNPROVEN,
                          tuning_profile_validate(&p));
    }
    /* Injected window with the known-flag cleared behaves identically. */
    TuningBoardCapability cap = synthetic_cap();
    cap.voltage_window_known = false;
    TuningProfile p = payload_profile();
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_VOLTAGE_UNPROVEN,
                      tuning_profile_validate_with_capability(&p, &cap));
    /* NULL capability fails closed too. */
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_NO_CAPABILITY,
                      tuning_profile_validate_with_capability(&p, NULL));
}

TEST_CASE("validate: synthetic voltage window bounds payloads", "[tuning_profile]")
{
    TuningBoardCapability cap = synthetic_cap();
    TuningProfile p = payload_profile();
    p.core_voltage_mv = 999;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_VOLTAGE_RANGE,
                      tuning_profile_validate_with_capability(&p, &cap));
    p.core_voltage_mv = 1251;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_VOLTAGE_RANGE,
                      tuning_profile_validate_with_capability(&p, &cap));
    p.core_voltage_mv = 1000;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK,
                      tuning_profile_validate_with_capability(&p, &cap));
    p.core_voltage_mv = 1250;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK,
                      tuning_profile_validate_with_capability(&p, &cap));
}

TEST_CASE("validate: fan curve count, bounds and ordering (synthetic)", "[tuning_profile]")
{
    TuningBoardCapability cap = synthetic_cap();
    TuningProfile p = payload_profile();
    for (uint8_t c = 1; c < TUNING_FAN_CURVE_MAX_POINTS; c++) {
        p.fan_curve_count = c;
        TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_FAN_CURVE_COUNT,
                          tuning_profile_validate_with_capability(&p, &cap));
    }

    p = payload_profile();
    p.fan_curve_count = TUNING_FAN_CURVE_MAX_POINTS;
    TuningCurvePoint good[TUNING_FAN_CURVE_MAX_POINTS] = {
        { 45, 25 }, { 52, 45 }, { 58, 70 }, { 64, 100 }
    };
    memcpy(p.fan_curve, good, sizeof(good));
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK,
                      tuning_profile_validate_with_capability(&p, &cap));

    p.fan_curve[3].temp_c = 121; /* > plausibility cap */
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_FAN_CURVE_POINT,
                      tuning_profile_validate_with_capability(&p, &cap));
    memcpy(p.fan_curve, good, sizeof(good));
    p.fan_curve[3].fan_percent = 101;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_FAN_CURVE_POINT,
                      tuning_profile_validate_with_capability(&p, &cap));

    memcpy(p.fan_curve, good, sizeof(good));
    p.fan_curve[2].temp_c = 52; /* not strictly ascending */
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_FAN_CURVE_ORDER,
                      tuning_profile_validate_with_capability(&p, &cap));
    memcpy(p.fan_curve, good, sizeof(good));
    p.fan_curve[2].fan_percent = 40; /* duty decreases */
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_FAN_CURVE_ORDER,
                      tuning_profile_validate_with_capability(&p, &cap));
}

TEST_CASE("validate: thermal limit sanity band and ordering (synthetic)", "[tuning_profile]")
{
    TuningBoardCapability cap = synthetic_cap();
    TuningProfile p = payload_profile();
    p.asic_warn_dc = TUNING_THERMAL_LIMIT_MIN_DC - 1;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_THERMAL_RANGE,
                      tuning_profile_validate_with_capability(&p, &cap));
    p = payload_profile();
    p.vrm_crit_dc = TUNING_THERMAL_LIMIT_MAX_DC + 1;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_THERMAL_RANGE,
                      tuning_profile_validate_with_capability(&p, &cap));

    p = payload_profile();
    p.asic_warn_dc = 700;
    p.asic_crit_dc = 700; /* warn >= crit */
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_THERMAL_ORDER,
                      tuning_profile_validate_with_capability(&p, &cap));
    p.asic_crit_dc = 750;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK,
                      tuning_profile_validate_with_capability(&p, &cap));

    p = payload_profile();
    p.vrm_warn_dc = 1050;
    p.vrm_crit_dc = 900;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_ERR_THERMAL_ORDER,
                      tuning_profile_validate_with_capability(&p, &cap));

    /* One-sided limits are legal (0 = unspecified). */
    p = payload_profile();
    p.asic_warn_dc = 650;
    TEST_ASSERT_EQUAL(TUNING_PROFILE_OK,
                      tuning_profile_validate_with_capability(&p, &cap));
}

/* ---------------- compatibility ---------------- */

TEST_CASE("compatible: NULL and unknown context fail closed", "[tuning_profile]")
{
    TuningProfile p = descriptor_profile();
    TuningHardwareContext hw = hw_full();
    TEST_ASSERT_EQUAL(TUNING_COMPAT_ERR_NULL, tuning_profile_compatible(NULL, &hw));
    TEST_ASSERT_EQUAL(TUNING_COMPAT_ERR_NULL, tuning_profile_compatible(&p, NULL));

    hw = hw_full();
    hw.board = TUNING_BOARD_UNKNOWN;
    TEST_ASSERT_EQUAL(TUNING_COMPAT_ERR_CONTEXT_UNKNOWN, tuning_profile_compatible(&p, &hw));
    hw = hw_full();
    hw.asic = TUNING_ASIC_UNKNOWN;
    TEST_ASSERT_EQUAL(TUNING_COMPAT_ERR_CONTEXT_UNKNOWN, tuning_profile_compatible(&p, &hw));
    hw = hw_full();
    hw.cooling_installed = (TuningCoolingClass)TUNING_COOLING__COUNT;
    TEST_ASSERT_EQUAL(TUNING_COMPAT_ERR_CONTEXT_UNKNOWN, tuning_profile_compatible(&p, &hw));
    hw = hw_full();
    hw.psu_installed = (TuningPsuClass)TUNING_PSU__COUNT;
    TEST_ASSERT_EQUAL(TUNING_COMPAT_ERR_CONTEXT_UNKNOWN, tuning_profile_compatible(&p, &hw));
}

TEST_CASE("compatible: cooling tiers are ordered and fail closed", "[tuning_profile]")
{
    TuningProfile p = descriptor_profile();
    TuningHardwareContext hw = hw_full();

    /* No requirement always passes, even with unspecified installation. */
    p.required_cooling = TUNING_COOLING_UNSPECIFIED;
    hw.cooling_installed = TUNING_COOLING_UNSPECIFIED;
    TEST_ASSERT_EQUAL(TUNING_COMPAT_OK, tuning_profile_compatible(&p, &hw));

    /* A real requirement needs installed >= required. */
    p.required_cooling = TUNING_COOLING_SUPERSINK_DUAL_FAN;
    hw.cooling_installed = TUNING_COOLING_STOCK;
    TEST_ASSERT_EQUAL(TUNING_COMPAT_ERR_COOLING_INSUFFICIENT,
                      tuning_profile_compatible(&p, &hw));
    hw.cooling_installed = TUNING_COOLING_UNSPECIFIED;
    TEST_ASSERT_EQUAL(TUNING_COMPAT_ERR_COOLING_INSUFFICIENT,
                      tuning_profile_compatible(&p, &hw));
    hw.cooling_installed = TUNING_COOLING_SUPERSINK_DUAL_FAN;
    TEST_ASSERT_EQUAL(TUNING_COMPAT_OK, tuning_profile_compatible(&p, &hw));

    p.required_cooling = TUNING_COOLING_STOCK;
    hw.cooling_installed = TUNING_COOLING_SUPERSINK_DUAL_FAN;
    TEST_ASSERT_EQUAL(TUNING_COMPAT_OK, tuning_profile_compatible(&p, &hw));
}

TEST_CASE("compatible: psu tiers", "[tuning_profile]")
{
    TuningProfile p = descriptor_profile();
    TuningHardwareContext hw = hw_full();
    p.required_psu = TUNING_PSU_STANDARD;
    hw.psu_installed = TUNING_PSU_UNSPECIFIED;
    TEST_ASSERT_EQUAL(TUNING_COMPAT_ERR_PSU_INSUFFICIENT,
                      tuning_profile_compatible(&p, &hw));
    hw.psu_installed = TUNING_PSU_STANDARD;
    TEST_ASSERT_EQUAL(TUNING_COMPAT_OK, tuning_profile_compatible(&p, &hw));
}

/* ---------------- auto eligibility ---------------- */

TEST_CASE("eligible: every production profile is refused for auto-selection", "[tuning_profile]")
{
    size_t n = 0;
    const TuningProfile *r = tuning_registry_gamma601(&n);
    TuningHardwareContext hw = hw_full();
    const TuningProfile *ss = tuning_registry_find(r, n, TUNING_PROFILE_ID_SUPERSINK_MAX);
    const TuningProfile *hot = tuning_registry_find(r, n, TUNING_PROFILE_ID_HOT_WEATHER);
    const TuningProfile *em = tuning_registry_find(r, n, TUNING_PROFILE_ID_EMERGENCY);

    /* SuperSink Max: refused. The physical miner currently running at this
     * profile's future operating point is NOT validation evidence. */
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_NOT_VALIDATED,
                      tuning_profile_auto_eligible(ss, &hw));
    /* Hot Weather Safe: refused. */
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_NOT_VALIDATED,
                      tuning_profile_auto_eligible(hot, &hw));
    /* Emergency Thermal Safe: refused. */
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_NOT_VALIDATED,
                      tuning_profile_auto_eligible(em, &hw));
}

TEST_CASE("eligible: matching the running miner's numbers confers nothing", "[tuning_profile]")
{
    TuningHardwareContext hw = hw_full();
    /* A synthetic UNVALIDATED profile whose payload equals the currently
     * running configuration (625 MHz / 1150 mV) is still refused — via the
     * voltage fail-closed rule AND the validation state. */
    TuningProfile p = payload_profile();
    memset(p.id, 0, sizeof(p.id));
    strncpy(p.id, "looks-like-current", sizeof(p.id) - 1);
    p.validation = TUNING_VALIDATION_UNVALIDATED;
    p.evidence_fingerprint = 0;
    p.frequency_mhz = 625;
    p.core_voltage_mv = 1150;
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_INVALID, /* voltage unproven */
                      tuning_profile_auto_eligible(&p, &hw));

    /* Payload-free variant of the same idea: still NOT_VALIDATED. */
    p = descriptor_profile();
    p.validation = TUNING_VALIDATION_UNVALIDATED;
    p.evidence_fingerprint = 0;
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_NOT_VALIDATED,
                      tuning_profile_auto_eligible(&p, &hw));
}

TEST_CASE("eligible: synthetic validated fixture passes; cooling tier gates it", "[tuning_profile]")
{
    TuningHardwareContext hw = hw_full();
    /* Synthetic promotion of the production supersink descriptor. */
    size_t n = 0;
    const TuningProfile *r = tuning_registry_gamma601(&n);
    TuningProfile promoted = *tuning_registry_find(r, n, TUNING_PROFILE_ID_SUPERSINK_MAX);
    promoted.validation = TUNING_VALIDATION_VALIDATED;
    promoted.evidence_fingerprint = 0xE0F0u; /* synthetic tag */
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_OK, tuning_profile_auto_eligible(&promoted, &hw));

    hw.cooling_installed = TUNING_COOLING_STOCK;
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_INCOMPATIBLE,
                      tuning_profile_auto_eligible(&promoted, &hw));
}

TEST_CASE("eligible: retired/incompatible/disabled/invalid are refused", "[tuning_profile]")
{
    TuningHardwareContext hw = hw_full();
    TuningProfile p = descriptor_profile();
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_OK, tuning_profile_auto_eligible(&p, &hw));

    p.validation = TUNING_VALIDATION_RETIRED;
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_RETIRED, tuning_profile_auto_eligible(&p, &hw));
    p.validation = TUNING_VALIDATION_INCOMPATIBLE;
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_MARKED_INCOMPATIBLE,
                      tuning_profile_auto_eligible(&p, &hw));

    p = descriptor_profile();
    p.disabled = true;
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_DISABLED, tuning_profile_auto_eligible(&p, &hw));

    p = descriptor_profile();
    p.frequency_mhz = 9999; /* covert payload on a payload-free descriptor */
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_INVALID, tuning_profile_auto_eligible(&p, &hw));

    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_NULL, tuning_profile_auto_eligible(NULL, &hw));
    TEST_ASSERT_EQUAL(TUNING_ELIGIBLE_ERR_NULL, tuning_profile_auto_eligible(&p, NULL));
}

/* ---------------- registry validation ---------------- */

TEST_CASE("registry validate: rejects structural problems", "[tuning_profile]")
{
    TEST_ASSERT_EQUAL(TUNING_REGISTRY_ERR_NULL, tuning_registry_validate(NULL, 1));
    TuningProfile one = descriptor_profile();
    TEST_ASSERT_EQUAL(TUNING_REGISTRY_ERR_EMPTY, tuning_registry_validate(&one, 0));

    TuningProfile bad = descriptor_profile();
    bad.revision = 0;
    TEST_ASSERT_EQUAL(TUNING_REGISTRY_ERR_PROFILE_INVALID,
                      tuning_registry_validate(&bad, 1));

    TuningProfile pair[2];
    pair[0] = descriptor_profile();
    pair[1] = descriptor_profile(); /* same id */
    pair[1].rank = 4;
    TEST_ASSERT_EQUAL(TUNING_REGISTRY_ERR_DUPLICATE_ID,
                      tuning_registry_validate(pair, 2));

    pair[1] = descriptor_profile();
    memset(pair[1].id, 0, sizeof(pair[1].id));
    strncpy(pair[1].id, "other-id", sizeof(pair[1].id) - 1);
    /* same rank now */
    TEST_ASSERT_EQUAL(TUNING_REGISTRY_ERR_DUPLICATE_RANK,
                      tuning_registry_validate(pair, 2));
}

TEST_CASE("registry validate: rollback must resolve and be strictly safer", "[tuning_profile]")
{
    TuningProfile pair[2];
    pair[0] = descriptor_profile();
    strncpy(pair[0].rollback_profile_id, "missing-profile",
            sizeof(pair[0].rollback_profile_id) - 1);
    TEST_ASSERT_EQUAL(TUNING_REGISTRY_ERR_ROLLBACK_UNRESOLVED,
                      tuning_registry_validate(pair, 1));

    pair[0] = descriptor_profile(); /* rank 3 */
    pair[1] = descriptor_profile();
    memset(pair[1].id, 0, sizeof(pair[1].id));
    strncpy(pair[1].id, "higher-rank", sizeof(pair[1].id) - 1);
    pair[1].rank = 5;
    memset(pair[0].rollback_profile_id, 0, sizeof(pair[0].rollback_profile_id));
    strncpy(pair[0].rollback_profile_id, "higher-rank",
            sizeof(pair[0].rollback_profile_id) - 1);
    TEST_ASSERT_EQUAL(TUNING_REGISTRY_ERR_ROLLBACK_NOT_SAFER,
                      tuning_registry_validate(pair, 2));

    pair[1].rank = 1; /* strictly safer -> OK */
    TEST_ASSERT_EQUAL(TUNING_REGISTRY_OK, tuning_registry_validate(pair, 2));
}

/* ---------------- lookup + labels + strings ---------------- */

TEST_CASE("find: bounded lookup semantics", "[tuning_profile]")
{
    size_t n = 0;
    const TuningProfile *r = tuning_registry_gamma601(&n);
    TEST_ASSERT_NULL(tuning_registry_find(NULL, n, TUNING_PROFILE_ID_EMERGENCY));
    TEST_ASSERT_NULL(tuning_registry_find(r, n, NULL));
    TEST_ASSERT_NULL(tuning_registry_find(r, n, ""));
    TEST_ASSERT_NULL(tuning_registry_find(r, n, "no-such-profile"));
    const TuningProfile *p = tuning_registry_find(r, n, TUNING_PROFILE_ID_HOT_WEATHER);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, p->id);
}

TEST_CASE("labels: presentation values only", "[tuning_profile]")
{
    TEST_ASSERT_EQUAL_STRING("SuperSink Max",
                             tuning_profile_display_label(TUNING_PROFILE_ID_SUPERSINK_MAX));
    TEST_ASSERT_EQUAL_STRING("Hot Weather Safe",
                             tuning_profile_display_label(TUNING_PROFILE_ID_HOT_WEATHER));
    TEST_ASSERT_EQUAL_STRING("Emergency Thermal Safe",
                             tuning_profile_display_label(TUNING_PROFILE_ID_EMERGENCY));
    TEST_ASSERT_EQUAL_STRING("Unknown Profile", tuning_profile_display_label("nope"));
    TEST_ASSERT_EQUAL_STRING("Unknown Profile", tuning_profile_display_label(NULL));
}

TEST_CASE("strings: every enum value has a distinct token", "[tuning_profile]")
{
    for (int i = 0; i < TUNING_PROFILE_ERR__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("ERR_UNKNOWN",
                                        tuning_profile_error_str((TuningProfileError)i)));
    }
    TEST_ASSERT_EQUAL_STRING("ERR_UNKNOWN",
                             tuning_profile_error_str((TuningProfileError)999));
    for (int i = 0; i < TUNING_COMPAT__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("ERR_UNKNOWN",
                                        tuning_compat_str((TuningCompatibilityResult)i)));
    }
    for (int i = 0; i < TUNING_ELIGIBLE__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("ERR_UNKNOWN",
                                        tuning_eligibility_str((TuningEligibilityResult)i)));
    }
    for (int i = 0; i < TUNING_REGISTRY__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("ERR_UNKNOWN",
                                        tuning_registry_result_str((TuningRegistryResult)i)));
    }
    for (int i = 0; i < TUNING_VALIDATION__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN",
                                        tuning_validation_state_str((TuningValidationState)i)));
    }
}
