#ifndef TUNING_PROFILE_H_
#define TUNING_PROFILE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * NeuralAxe Weather-Aware Tuning — pure tuning-profile metadata model
 * (Gate W1). Board 601 / BM1370 only for the initial registry.
 *
 * This module is PURE: no NVS, no network, no wall clock, no FreeRTOS, no
 * HTTP, no logging dependency, no heap allocation, no restart, no profile
 * application. It only models WHAT a tuning profile is, whether it is
 * structurally valid, whether it is compatible with declared hardware, and
 * whether automatic selection is permitted. Applying a profile is a later
 * gate (W4); persistence is Gate W2.
 *
 * Architectural source of truth: the committed Gate W0 audit report
 * (Weather-Aware Tuning architecture audit).
 *
 * SAFETY INVARIANTS (Gate W0 hard boundaries + W1 owner corrections):
 *  - EVERY production registry profile ships UNVALIDATED and payload-free.
 *    A currently running operating point is NOT validation evidence;
 *    browser-local Stability Lab records are NOT firmware evidence. No
 *    production weather profile is automatically selectable until the
 *    owner records committed physical validation evidence and promotes it
 *    (a later gate). Only synthetic TEST fixtures may use VALIDATED.
 *  - Production profile descriptors carry NO trusted tuning payload
 *    (payload_present == false, all payload fields zero). The climate
 *    policy selects stable profile IDs only; frequency/voltage payloads
 *    appear exclusively in synthetic test fixtures until a validated
 *    profile source supplies them (later gate).
 *  - The 625 MHz maximum in the capability table documents the CURRENT
 *    UI/PRODUCT OPTION CEILING (the BM1370 dropdown maximum,
 *    main/device_config.h BM1370_FREQUENCY_OPTIONS) — it is NOT proof of
 *    a hardware-safe operating point, and this feature never raises it.
 *  - NO board voltage window is encoded: no owner-validated safe voltage
 *    ceiling exists for Gamma 601, and TPS546 regulator acceptance limits
 *    are NOT safe tuning limits. A payload carrying a core voltage fails
 *    closed (ERR_VOLTAGE_UNPROVEN) unless an explicitly injected
 *    capability window is supplied — synthetic test fixtures only in W1.
 *  - Automatic selection requires VALIDATED + compatible + enabled. An
 *    UNVALIDATED profile is NEVER auto-selectable — not even for the
 *    emergency role. Local hardcoded thermal protection (75 C / 105 C stop
 *    in power_management_task.c) remains authoritative and is not modeled,
 *    replaced or weakened here.
 *  - Profiles that leave fan-curve / thermal-limit fields unspecified (0)
 *    RETAIN the existing firmware thermal configuration; a profile can
 *    therefore never clear or weaken thermal safety by omission.
 *
 * SERIALIZATION BOUNDARY (Gate W2, not W1): these structs are NOT an
 * on-flash format. Persistence must go through an explicit versioned
 * field-by-field serializer (B3 pattern, new namespace). Enum numeric
 * values below are pinned by _Static_assert because Gate W2 will persist
 * them; renumbering must break the build, never the flash format.
 */

#define TUNING_PROFILE_MODEL_VERSION 1u

/* Bounded field sizes (buffer sizes including the NUL terminator). */
#define TUNING_PROFILE_ID_MAX 24 /* stable machine id, [a-z0-9-], strlen<=23 */

/* Fan-curve bound. Mirrors the firmware's THERMAL_CURVE_POINTS (4) without
 * importing thermal_control; a profile carrying a curve must carry exactly
 * 0 (retain existing) or TUNING_FAN_CURVE_MAX_POINTS points. */
#define TUNING_FAN_CURVE_MAX_POINTS 4

/* Profile rank bound. Rank is a RELATIVE performance/aggressiveness order
 * within one registry (0 = safest). Never a frequency. */
#define TUNING_PROFILE_RANK_MAX 15u

/* Thermal-limit sanity band, deci-degrees Celsius, for OPTIONAL profile
 * thermal-limit fields (0 = unspecified/retain existing — always legal).
 * These bound nonsense, they do not authorize values. */
#define TUNING_THERMAL_LIMIT_MIN_DC 200   /*  20.0 C */
#define TUNING_THERMAL_LIMIT_MAX_DC 1500  /* 150.0 C */

/* Fan-curve point sanity bounds (temp C, duty %). */
#define TUNING_FAN_CURVE_TEMP_MAX_C 120
#define TUNING_FAN_CURVE_PCT_MAX 100

/* ------------------------------------------------------------------ */
/* Enumerations (numeric values persistence-pinned for Gate W2)        */
/* ------------------------------------------------------------------ */

/* Board model compatibility class. UNKNOWN fails closed everywhere. */
typedef enum {
    TUNING_BOARD_UNKNOWN = 0,
    TUNING_BOARD_GAMMA_601 = 1,
    TUNING_BOARD__COUNT
} TuningBoardClass;

/* ASIC compatibility class. UNKNOWN fails closed everywhere. */
typedef enum {
    TUNING_ASIC_UNKNOWN = 0,
    TUNING_ASIC_BM1370 = 1,
    TUNING_ASIC__COUNT
} TuningAsicClass;

/*
 * Cooling capability tiers, ordered: a profile requiring tier N is
 * compatible only when the installed tier is >= N. UNSPECIFIED (0) as a
 * requirement means "no cooling requirement"; as an installed value it
 * satisfies ONLY profiles with no requirement (fail closed).
 */
typedef enum {
    TUNING_COOLING_UNSPECIFIED = 0,
    TUNING_COOLING_STOCK = 1,
    TUNING_COOLING_SUPERSINK_DUAL_FAN = 2,
    TUNING_COOLING__COUNT
} TuningCoolingClass;

/* Power-supply capability tiers, same ordering semantics as cooling. */
typedef enum {
    TUNING_PSU_UNSPECIFIED = 0,
    TUNING_PSU_STANDARD = 1,
    TUNING_PSU__COUNT
} TuningPsuClass;

/* Validation state (Gate W0 contract). Only VALIDATED is auto-selectable. */
typedef enum {
    TUNING_VALIDATION_UNVALIDATED = 0,
    TUNING_VALIDATION_VALIDATED = 1,
    TUNING_VALIDATION_RETIRED = 2,
    TUNING_VALIDATION_INCOMPATIBLE = 3,
    TUNING_VALIDATION__COUNT
} TuningValidationState;

/* Persistence pins (Gate W2 will store these numeric values). EVERY value
 * and count is pinned: inserting or renumbering must break the build. */
_Static_assert(TUNING_BOARD_GAMMA_601 == 1, "board enum pinned");
_Static_assert(TUNING_ASIC_BM1370 == 1, "asic enum pinned");
_Static_assert(TUNING_COOLING_STOCK == 1 &&
               TUNING_COOLING_SUPERSINK_DUAL_FAN == 2, "cooling enum pinned");
_Static_assert(TUNING_PSU_STANDARD == 1, "psu enum pinned");
_Static_assert(TUNING_VALIDATION_UNVALIDATED == 0 &&
               TUNING_VALIDATION_VALIDATED == 1 &&
               TUNING_VALIDATION_RETIRED == 2 &&
               TUNING_VALIDATION_INCOMPATIBLE == 3,
               "validation state enum pinned");
_Static_assert(TUNING_BOARD__COUNT == 2 && TUNING_ASIC__COUNT == 2 &&
               TUNING_COOLING__COUNT == 3 && TUNING_PSU__COUNT == 2 &&
               TUNING_VALIDATION__COUNT == 4,
               "enum counts pinned — adding a value forces persistence review");

/* ------------------------------------------------------------------ */
/* Models                                                              */
/* ------------------------------------------------------------------ */

/* One fan-curve point (temp C -> duty %). */
typedef struct {
    uint8_t temp_c;
    uint8_t fan_percent;
} TuningCurvePoint;

/*
 * Tuning profile metadata. The stable `id` is the ONLY key persistence and
 * policy logic may use; display labels are presentation values resolved
 * separately (tuning_profile_display_label). All strings are bounded and
 * NUL-terminated; ids are lowercase [a-z0-9-].
 *
 * PAYLOAD SEPARATION: policy selection operates on IDs and metadata only —
 * never on raw payload values. `payload_present == false` (ALL production
 * descriptors in W1) requires every payload field to be zero; the tuning
 * payload for a production profile is supplied only by a later validated
 * profile source. Synthetic test fixtures may carry payloads to exercise
 * the bounded payload mechanics.
 *
 * Optional fields use 0 = "unspecified — retain existing firmware
 * configuration": fan_curve_count, all *_dc thermal limits, and an empty
 * rollback_profile_id (no rollback reference).
 */
typedef struct {
    uint16_t model_version; /* TUNING_PROFILE_MODEL_VERSION */
    char id[TUNING_PROFILE_ID_MAX];
    uint16_t revision;      /* >= 1; bump on any metadata change */

    TuningBoardClass board;
    TuningAsicClass asic;
    TuningCoolingClass required_cooling;
    TuningPsuClass required_psu;

    TuningValidationState validation;
    /* Opaque stable evidence tag. Required nonzero when VALIDATED.
     * NOT a recomputable hash — a stable reference to recorded evidence. */
    uint32_t evidence_fingerprint;

    uint8_t rank; /* 0 = safest; unique within a registry */

    /* Tuning payload (see PAYLOAD SEPARATION above). */
    bool payload_present;   /* false: all payload fields MUST be zero    */
    uint16_t frequency_mhz;
    uint16_t core_voltage_mv;

    uint8_t fan_curve_count; /* 0 = retain existing fan configuration */
    TuningCurvePoint fan_curve[TUNING_FAN_CURVE_MAX_POINTS];

    /* Optional thermal limits, deci-C; 0 = unspecified/retain existing. */
    int16_t asic_warn_dc;
    int16_t asic_crit_dc;
    int16_t vrm_warn_dc;
    int16_t vrm_crit_dc;

    char rollback_profile_id[TUNING_PROFILE_ID_MAX]; /* "" = none */

    bool disabled; /* operational kill-switch, independent of validation */
} TuningProfile;

/*
 * Declared hardware context for compatibility checks. `board`/`asic` come
 * from the DECLARED build target (neuralaxe_identity.h) cross-checked by
 * the integrator against the runtime ASICModel — never from the runtime
 * boardVersion string alone (Phase 2K supported-gate rule). Cooling and
 * PSU are owner-declared installation facts. UNKNOWN fails closed.
 */
typedef struct {
    TuningBoardClass board;
    TuningAsicClass asic;
    TuningCoolingClass cooling_installed;
    TuningPsuClass psu_installed;
} TuningHardwareContext;

/*
 * Per-board payload-bounds window. The frequency window documents the
 * CURRENT PRODUCT OPTION CEILING (the served dropdown maximum — not a
 * hardware-safety claim); this feature never raises it (Gate W0 hard
 * boundary 1). `voltage_window_known == false` means NO owner-validated
 * safe voltage window exists for the board: any payload carrying a core
 * voltage then fails closed (ERR_VOLTAGE_UNPROVEN). Gamma 601 ships with
 * voltage_window_known == false — regulator acceptance limits are not
 * safe tuning limits, and W1 invents no voltage number. An explicitly
 * injected window (tuning_profile_validate_with_capability) is for
 * synthetic test fixtures and, later, an owner-approved validated
 * profile source.
 */
typedef struct {
    TuningBoardClass board;
    TuningAsicClass asic;
    uint16_t min_frequency_mhz;
    uint16_t max_frequency_mhz;   /* product option ceiling, not safety */
    bool voltage_window_known;    /* false = fail closed on any voltage */
    uint16_t min_core_voltage_mv; /* meaningful only when window known  */
    uint16_t max_core_voltage_mv; /* meaningful only when window known  */
} TuningBoardCapability;

#define TUNING_GAMMA601_MIN_FREQUENCY_MHZ 50u
/* Current UI/product option ceiling (dropdown max) — NOT hardware-safe
 * proof, and never raised by this feature. */
#define TUNING_GAMMA601_MAX_FREQUENCY_MHZ 625u

/* ------------------------------------------------------------------ */
/* Result codes (bounded machine codes; string helpers below)          */
/* ------------------------------------------------------------------ */

typedef enum {
    TUNING_PROFILE_OK = 0,
    TUNING_PROFILE_ERR_NULL,
    TUNING_PROFILE_ERR_MODEL_VERSION,
    TUNING_PROFILE_ERR_ID_INVALID,       /* empty, unterminated or bad chars */
    TUNING_PROFILE_ERR_REVISION_ZERO,
    TUNING_PROFILE_ERR_ENUM_RANGE,
    TUNING_PROFILE_ERR_BOARD_UNKNOWN,
    TUNING_PROFILE_ERR_ASIC_UNKNOWN,
    TUNING_PROFILE_ERR_NO_CAPABILITY,    /* no capability row for board/asic */
    TUNING_PROFILE_ERR_FREQUENCY_RANGE,  /* outside board capability window  */
    TUNING_PROFILE_ERR_VOLTAGE_RANGE,    /* outside board capability window  */
    TUNING_PROFILE_ERR_RANK_RANGE,
    TUNING_PROFILE_ERR_FAN_CURVE_COUNT,  /* not 0 and not exactly max points */
    TUNING_PROFILE_ERR_FAN_CURVE_POINT,  /* bounds violated                  */
    TUNING_PROFILE_ERR_FAN_CURVE_ORDER,  /* temps not strictly ascending or
                                            duty not non-decreasing          */
    TUNING_PROFILE_ERR_THERMAL_RANGE,    /* nonzero limit outside sanity band */
    TUNING_PROFILE_ERR_THERMAL_ORDER,    /* warn >= crit when both specified */
    TUNING_PROFILE_ERR_ROLLBACK_INVALID, /* bad chars / self-reference       */
    TUNING_PROFILE_ERR_EVIDENCE_MISSING, /* VALIDATED with fingerprint == 0  */
    TUNING_PROFILE_ERR_PAYLOAD_NOT_EMPTY,/* payload_present false but a
                                            payload field is nonzero         */
    TUNING_PROFILE_ERR_VOLTAGE_UNPROVEN, /* payload voltage present but the
                                            board has no owner-validated
                                            voltage window (fails closed)    */
    TUNING_PROFILE_ERR__COUNT
} TuningProfileError;

typedef enum {
    TUNING_COMPAT_OK = 0,
    TUNING_COMPAT_ERR_NULL,
    TUNING_COMPAT_ERR_CONTEXT_UNKNOWN,     /* ctx board/asic UNKNOWN         */
    TUNING_COMPAT_ERR_BOARD_MISMATCH,
    TUNING_COMPAT_ERR_ASIC_MISMATCH,
    TUNING_COMPAT_ERR_COOLING_INSUFFICIENT,
    TUNING_COMPAT_ERR_PSU_INSUFFICIENT,
    TUNING_COMPAT__COUNT
} TuningCompatibilityResult;

typedef enum {
    TUNING_ELIGIBLE_OK = 0,
    TUNING_ELIGIBLE_ERR_NULL,
    TUNING_ELIGIBLE_ERR_INVALID,        /* structural validation failed      */
    TUNING_ELIGIBLE_ERR_NOT_VALIDATED,  /* UNVALIDATED — never auto-selected */
    TUNING_ELIGIBLE_ERR_RETIRED,
    TUNING_ELIGIBLE_ERR_MARKED_INCOMPATIBLE,
    TUNING_ELIGIBLE_ERR_DISABLED,
    TUNING_ELIGIBLE_ERR_INCOMPATIBLE,   /* hardware compatibility failed     */
    TUNING_ELIGIBLE__COUNT
} TuningEligibilityResult;

typedef enum {
    TUNING_REGISTRY_OK = 0,
    TUNING_REGISTRY_ERR_NULL,
    TUNING_REGISTRY_ERR_EMPTY,
    TUNING_REGISTRY_ERR_PROFILE_INVALID,
    TUNING_REGISTRY_ERR_DUPLICATE_ID,
    TUNING_REGISTRY_ERR_DUPLICATE_RANK,
    TUNING_REGISTRY_ERR_ROLLBACK_UNRESOLVED, /* names a profile not present  */
    TUNING_REGISTRY_ERR_ROLLBACK_NOT_SAFER,  /* rollback rank not strictly
                                                lower (prevents cycles)      */
    TUNING_REGISTRY__COUNT
} TuningRegistryResult;

/* ------------------------------------------------------------------ */
/* Gamma 601 registry (compile-time declared, Gate W1 MVP)             */
/* ------------------------------------------------------------------ */

#define TUNING_REGISTRY_GAMMA601_COUNT 3u

/* Stable profile ids (machine keys — labels are presentation only). */
#define TUNING_PROFILE_ID_SUPERSINK_MAX  "supersink-max"
#define TUNING_PROFILE_ID_HOT_WEATHER    "hot-weather-safe"
#define TUNING_PROFILE_ID_EMERGENCY      "emergency-thermal-safe"

/*
 * Returns the static Gamma 601 registry (count via out param, never NULL).
 * ALL THREE production descriptors are UNVALIDATED, evidence-free and
 * PAYLOAD-FREE (ids, ranks, compatibility metadata and rollback chain
 * only — no frequency/voltage values). None is automatically selectable
 * in W1; promotion to VALIDATED requires committed, owner-approved
 * physical validation evidence supplied by a later validated profile
 * source. A currently running operating point is NOT such evidence.
 *  - "supersink-max"          rank 2; requires SUPERSINK_DUAL_FAN cooling;
 *                             rollback -> "hot-weather-safe".
 *  - "hot-weather-safe"       rank 1; rollback -> "emergency-thermal-safe".
 *  - "emergency-thermal-safe" rank 0; no rollback (already safest).
 * All profiles retain existing fan/thermal configuration (unspecified).
 */
const TuningProfile *tuning_registry_gamma601(size_t *out_count);

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Structural validation of a single profile (bounds, ids, payload rules,
 * curve/thermal sanity, evidence rule) against the STATIC board capability
 * table. On Gamma 601 any payload carrying a core voltage fails closed
 * (ERR_VOLTAGE_UNPROVEN — no validated voltage window exists). Pure. */
TuningProfileError tuning_profile_validate(const TuningProfile *p);

/* Same validation against an explicitly INJECTED capability window — for
 * synthetic test fixtures and, later, an owner-approved validated profile
 * source. `cap` NULL fails closed (ERR_NO_CAPABILITY). */
TuningProfileError tuning_profile_validate_with_capability(
    const TuningProfile *p, const TuningBoardCapability *cap);

/* Board/asic capability lookup; NULL when no row exists (fails closed). */
const TuningBoardCapability *tuning_board_capability(TuningBoardClass board,
                                                     TuningAsicClass asic);

/* Hardware compatibility (declared context; UNKNOWN fails closed). */
TuningCompatibilityResult tuning_profile_compatible(const TuningProfile *p,
                                                    const TuningHardwareContext *hw);

/*
 * May this profile be AUTOMATICALLY selected (weather policy, fail-safe,
 * sensor safety, emergency role, rollback execution)? Requires: structurally
 * valid, VALIDATED, not disabled, hardware-compatible. Manual overrides use
 * the same rule via the policy layer (tuning_policy.h). This function never
 * consults weather, time or sensors — those gates live in the policy layer.
 */
TuningEligibilityResult tuning_profile_auto_eligible(const TuningProfile *p,
                                                     const TuningHardwareContext *hw);

/* Registry integrity: every profile valid, ids unique, ranks unique,
 * rollback references resolve to a STRICTLY lower rank (no cycles). */
TuningRegistryResult tuning_registry_validate(const TuningProfile *profiles,
                                              size_t count);

/* Find by stable id (bounded compare). NULL when absent or inputs NULL. */
const TuningProfile *tuning_registry_find(const TuningProfile *profiles,
                                          size_t count,
                                          const char *profile_id);

/* Presentation label for a known registry id; "Unknown Profile" otherwise.
 * Labels are NEVER used as keys. */
const char *tuning_profile_display_label(const char *profile_id);

/* Bounded token strings for logs/tests (never longer than 40 chars). */
const char *tuning_profile_error_str(TuningProfileError e);
const char *tuning_compat_str(TuningCompatibilityResult r);
const char *tuning_eligibility_str(TuningEligibilityResult r);
const char *tuning_registry_result_str(TuningRegistryResult r);
const char *tuning_validation_state_str(TuningValidationState s);

#endif /* TUNING_PROFILE_H_ */
