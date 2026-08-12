#ifndef TUNING_POLICY_H_
#define TUNING_POLICY_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "tuning_profile.h"

/*
 * NeuralAxe Weather-Aware Tuning — pure climate policy, precedence
 * arbitration, sensor-health model and manual-override eligibility
 * (Gate W1).
 *
 * This module is PURE: no NVS, no network, no scheduler, no wall clock
 * reads, no FreeRTOS, no logging dependency, no heap, no profile apply.
 * Every function is a deterministic decision over caller-supplied inputs;
 * the output is a bounded SELECTION INTENT that a later gate (W4) executes
 * under operation ownership. Trusted epochs/monotonic values are inputs —
 * this module never reads a clock (the system wall clock is pool-ntime
 * controlled and MUST NOT be consumed here; see Gate W0 §20).
 *
 * PRECEDENCE CONTRACT (Gate W0 §28 — smaller value = higher priority):
 *   1 EMERGENCY_THERMAL          (existing firmware overheat protection —
 *                                 modeled as an input flag; the hardcoded
 *                                 75 C / 105 C stop is NOT replaced here)
 *   2 STABILITY_OR_REBOOT_ROLLBACK
 *   3 SENSOR_INTEGRITY_FAILURE
 *   4 WEATHER_API_OR_TIME_FAIL_SAFE
 *   5 MANUAL_OVERRIDE_WITHIN_SAFETY_CEILING
 *   6 NORMAL_WEATHER_POLICY
 *   7 CURRENT_VALIDATED_PROFILE
 * A lower-priority source never overrides an active higher-priority one.
 * Safety sources (1-4) may only move DOWN in rank (or from unknown), never
 * up. Weather/API/time failure can therefore never cause an upgrade.
 *
 * OWNERSHIP: upgrade execution additionally requires operation-ownership
 * acquisition (Gate B5 lease) — that is a RUNTIME gate enforced in W4; the
 * pure engine only emits the intent.
 */

/* ------------------------------------------------------------------ */
/* Scaled-integer temperature contract                                 */
/* ------------------------------------------------------------------ */

/* All policy temperatures are deci-degrees Celsius (dc): 30.0 C == 300. */
#define TUNING_FORECAST_SANITY_MIN_DC (-600) /* -60.0 C */
#define TUNING_FORECAST_SANITY_MAX_DC 600    /* +60.0 C */

/* Default climate thresholds (Gate W0 §34 / task brief): enter hot at
 * >= 30.0 C, leave hot only at <= 28.0 C. */
#define TUNING_DEFAULT_HOT_THRESHOLD_DC 300
#define TUNING_DEFAULT_COOL_THRESHOLD_DC 280

/* Sensor plausibility caps, deci-C (Gate W0 §14/§15: EMC2101 diode faults
 * read ~127 C; TPS546 datasheet max 145 C). */
#define TUNING_ASIC_TEMP_PLAUSIBLE_MAX_DC 1200 /* mirrors thermal_control 120C */
#define TUNING_VRM_TEMP_PLAUSIBLE_MAX_DC 1500

/* ------------------------------------------------------------------ */
/* Enumerations (numeric values persistence-pinned for Gate W2)        */
/* ------------------------------------------------------------------ */

/* Actor classes (task brief). NONE=0 so a zeroed record is fail-safe. */
typedef enum {
    TUNING_ACTOR_NONE = 0,
    TUNING_ACTOR_THERMAL = 1,
    TUNING_ACTOR_STABILITY = 2,
    TUNING_ACTOR_SENSOR_SAFETY = 3,
    TUNING_ACTOR_WEATHER_POLICY = 4,
    TUNING_ACTOR_WEATHER_FAIL_SAFE = 5,
    TUNING_ACTOR_MANUAL_API = 6,
    TUNING_ACTOR_BOOT_RECOVERY = 7,
    TUNING_ACTOR_OPERATOR_RECOVERY = 8,
    TUNING_ACTOR__COUNT
} TuningActorClass;

/* Winning precedence source; numeric value == priority (1 highest). */
typedef enum {
    TUNING_SOURCE_NONE = 0,
    TUNING_SOURCE_EMERGENCY_THERMAL = 1,
    TUNING_SOURCE_STABILITY_ROLLBACK = 2,
    TUNING_SOURCE_SENSOR_INTEGRITY = 3,
    TUNING_SOURCE_WEATHER_FAIL_SAFE = 4,
    TUNING_SOURCE_MANUAL_OVERRIDE = 5,
    TUNING_SOURCE_WEATHER_POLICY = 6,
    TUNING_SOURCE_CURRENT_PROFILE = 7,
    TUNING_SOURCE__COUNT
} TuningPrecedenceSource;

/* Weather (climate) hysteresis state. UNKNOWN until a first decisive
 * forecast; survives reboot via Gate W2 persistence. */
typedef enum {
    TUNING_WEATHER_STATE_UNKNOWN = 0,
    TUNING_WEATHER_STATE_NORMAL = 1,
    TUNING_WEATHER_STATE_HOT = 2,
    TUNING_WEATHER_STATE__COUNT
} TuningWeatherState;

/* Forecast input status as classified by the (W3) provider/validator. */
typedef enum {
    TUNING_FORECAST_OK = 0,
    TUNING_FORECAST_STALE = 1,
    TUNING_FORECAST_INVALID = 2,
    TUNING_FORECAST_UNAVAILABLE = 3,
    TUNING_FORECAST__COUNT
} TuningForecastStatus;

/* Climate-policy request produced by the hysteresis FSM. */
typedef enum {
    TUNING_CLIMATE_REQ_RETAIN = 0,       /* keep existing stance            */
    TUNING_CLIMATE_REQ_COOL_PROFILE = 1, /* cool-day profile may be ELIGIBLE */
    TUNING_CLIMATE_REQ_HOT_PROFILE = 2,  /* hot-day profile requested        */
    TUNING_CLIMATE_REQ_FAIL_SAFE = 3,    /* forecast unusable — fail safe    */
    TUNING_CLIMATE_REQ__COUNT
} TuningClimateRequest;

/* Per-sensor health status (inputs classified by helpers below). */
typedef enum {
    TUNING_SENSOR_OK = 0,
    TUNING_SENSOR_MISSING = 1,     /* expected but absent                 */
    TUNING_SENSOR_INVALID = 2,     /* read error / -1 sentinel / <= 0     */
    TUNING_SENSOR_STALE = 3,       /* frozen value (TPS546 cached-last)   */
    TUNING_SENSOR_IMPLAUSIBLE = 4, /* beyond plausibility cap (127C fault) */
    TUNING_SENSOR__COUNT
} TuningSensorStatus;

/* Mining stability classification (input from Stability-Lab-style health;
 * UNKNOWN fails closed for upgrades). */
typedef enum {
    TUNING_MINING_UNKNOWN = 0,
    TUNING_MINING_STABLE = 1,
    TUNING_MINING_DEGRADED = 2,
    TUNING_MINING_UNSTABLE = 3,
    TUNING_MINING__COUNT
} TuningMiningHealth;

/* Arbitration output action. */
typedef enum {
    TUNING_SELECT_NONE = 0,              /* retain current, nothing to do   */
    TUNING_SELECT_PROFILE = 1,           /* request apply of profile_id     */
    TUNING_SELECT_RETAIN_INHIBIT = 2,    /* retain + inhibit upgrades       */
    TUNING_SELECT_OPERATOR_RECOVERY = 3, /* retain + surface operator help  */
    TUNING_SELECT__COUNT
} TuningSelectionAction;

/* Bounded machine reason codes (logs, status, audit). */
typedef enum {
    TUNING_REASON_NONE = 0,
    TUNING_REASON_EMERGENCY_THERMAL_ACTIVE,
    TUNING_REASON_STABILITY_ROLLBACK_ACTIVE,
    TUNING_REASON_SENSOR_INTEGRITY_FAILED,
    TUNING_REASON_WEATHER_FAIL_SAFE,
    TUNING_REASON_TIME_UNTRUSTED,
    TUNING_REASON_WEATHER_HOT_FORECAST,
    TUNING_REASON_WEATHER_COOL_ELIGIBLE,
    TUNING_REASON_WEATHER_RETAIN_BAND,
    TUNING_REASON_CLIMATE_UNKNOWN,
    TUNING_REASON_ALREADY_ACTIVE,
    TUNING_REASON_RETAINED_SAFER_CURRENT, /* target not a downgrade — kept  */
    TUNING_REASON_CURRENT_UNKNOWN,
    TUNING_REASON_PROFILE_NOT_ELIGIBLE,
    TUNING_REASON_NO_VALID_HOT_PROFILE,
    TUNING_REASON_NO_VALID_EMERGENCY_PROFILE,
    TUNING_REASON_NO_VALID_ROLLBACK_PROFILE,
    TUNING_REASON_COOLDOWN_ACTIVE,
    TUNING_REASON_MINING_NOT_STABLE,
    TUNING_REASON_UPGRADE_INHIBITED,
    TUNING_REASON_SENSORS_NOT_UPGRADE_OK,
    TUNING_REASON_OVERRIDE_APPLIED,
    TUNING_REASON_OVERRIDE_REJECTED,
    TUNING_REASON_CURRENT_RETAINED,
    TUNING_REASON__COUNT
} TuningReasonCode;

/* Policy API error codes (contract misuse, not decisions). */
typedef enum {
    TUNING_POLICY_OK = 0,
    TUNING_POLICY_ERR_NULL,
    TUNING_POLICY_ERR_THRESHOLDS,
    TUNING_POLICY_ERR_ENUM_RANGE,
    TUNING_POLICY_ERR_ENVIRONMENT,
    TUNING_POLICY__ERR_COUNT
} TuningPolicyError;

/* Manual-override eligibility results (task-brief rejection list). */
typedef enum {
    TUNING_OVERRIDE_OK = 0,
    TUNING_OVERRIDE_ERR_NULL,
    TUNING_OVERRIDE_ERR_INACTIVE,
    TUNING_OVERRIDE_ERR_ACTOR,           /* not MANUAL_API/OPERATOR_RECOVERY */
    TUNING_OVERRIDE_ERR_NO_EXPIRATION,
    TUNING_OVERRIDE_ERR_EXPIRATION_ORDER,/* expires <= created               */
    TUNING_OVERRIDE_ERR_EXPIRED,
    TUNING_OVERRIDE_ERR_TIME_UNTRUSTED,
    TUNING_OVERRIDE_ERR_PROFILE_UNKNOWN,
    TUNING_OVERRIDE_ERR_NOT_ELIGIBLE,    /* unvalidated/retired/disabled/
                                            incompatible (see detail)        */
    TUNING_OVERRIDE_ERR_ABOVE_CEILING,
    TUNING_OVERRIDE_ERR_CEILING_UNKNOWN, /* no valid ceiling — fail closed   */
    TUNING_OVERRIDE_ERR_COOLDOWN_UPGRADE,/* upgrade during active cooldown   */
    TUNING_OVERRIDE_NOT_EVALUATED,       /* override is active but a higher-
                                            priority safety source won — it
                                            was preempted, never evaluated   */
    TUNING_OVERRIDE__COUNT
} TuningOverrideResult;

/* Persistence pins for Gate W2. EVERY value and count is pinned: inserting
 * or renumbering must break the build, never the flash format. */
_Static_assert(TUNING_ACTOR_NONE == 0 && TUNING_ACTOR_THERMAL == 1 &&
               TUNING_ACTOR_STABILITY == 2 && TUNING_ACTOR_SENSOR_SAFETY == 3 &&
               TUNING_ACTOR_WEATHER_POLICY == 4 &&
               TUNING_ACTOR_WEATHER_FAIL_SAFE == 5 &&
               TUNING_ACTOR_MANUAL_API == 6 && TUNING_ACTOR_BOOT_RECOVERY == 7 &&
               TUNING_ACTOR_OPERATOR_RECOVERY == 8 && TUNING_ACTOR__COUNT == 9,
               "actor enum fully pinned");
_Static_assert(TUNING_SOURCE_NONE == 0 && TUNING_SOURCE_EMERGENCY_THERMAL == 1 &&
               TUNING_SOURCE_STABILITY_ROLLBACK == 2 &&
               TUNING_SOURCE_SENSOR_INTEGRITY == 3 &&
               TUNING_SOURCE_WEATHER_FAIL_SAFE == 4 &&
               TUNING_SOURCE_MANUAL_OVERRIDE == 5 &&
               TUNING_SOURCE_WEATHER_POLICY == 6 &&
               TUNING_SOURCE_CURRENT_PROFILE == 7 && TUNING_SOURCE__COUNT == 8,
               "source enum fully pinned (value == priority)");
_Static_assert(TUNING_WEATHER_STATE_UNKNOWN == 0 &&
               TUNING_WEATHER_STATE_NORMAL == 1 &&
               TUNING_WEATHER_STATE_HOT == 2 &&
               TUNING_WEATHER_STATE__COUNT == 3, "weather state fully pinned");
_Static_assert(TUNING_FORECAST_OK == 0 && TUNING_FORECAST_STALE == 1 &&
               TUNING_FORECAST_INVALID == 2 &&
               TUNING_FORECAST_UNAVAILABLE == 3 && TUNING_FORECAST__COUNT == 4,
               "forecast status fully pinned");
_Static_assert(TUNING_CLIMATE_REQ_RETAIN == 0 &&
               TUNING_CLIMATE_REQ_COOL_PROFILE == 1 &&
               TUNING_CLIMATE_REQ_HOT_PROFILE == 2 &&
               TUNING_CLIMATE_REQ_FAIL_SAFE == 3 &&
               TUNING_CLIMATE_REQ__COUNT == 4, "climate request fully pinned");
_Static_assert(TUNING_SENSOR_OK == 0 && TUNING_SENSOR_MISSING == 1 &&
               TUNING_SENSOR_INVALID == 2 && TUNING_SENSOR_STALE == 3 &&
               TUNING_SENSOR_IMPLAUSIBLE == 4 && TUNING_SENSOR__COUNT == 5,
               "sensor status fully pinned");
_Static_assert(TUNING_MINING_UNKNOWN == 0 && TUNING_MINING_STABLE == 1 &&
               TUNING_MINING_DEGRADED == 2 && TUNING_MINING_UNSTABLE == 3 &&
               TUNING_MINING__COUNT == 4, "mining health fully pinned");
_Static_assert(TUNING_SELECT_NONE == 0 && TUNING_SELECT_PROFILE == 1 &&
               TUNING_SELECT_RETAIN_INHIBIT == 2 &&
               TUNING_SELECT_OPERATOR_RECOVERY == 3 &&
               TUNING_SELECT__COUNT == 4, "selection action fully pinned");
_Static_assert(TUNING_REASON__COUNT == 24,
               "reason count pinned — review W2 encoding on change");
_Static_assert(TUNING_OVERRIDE_NOT_EVALUATED == 13 &&
               TUNING_OVERRIDE__COUNT == 14, "override result fully pinned");

/* ------------------------------------------------------------------ */
/* Climate hysteresis FSM                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int16_t hot_threshold_dc;  /* enter HOT at forecast >= hot            */
    int16_t cool_threshold_dc; /* leave HOT only at forecast <= cool      */
} TuningClimateThresholds;

typedef struct {
    TuningWeatherState state;
    int16_t last_forecast_max_dc; /* meaningful only when last_valid      */
    bool last_forecast_valid;
    uint32_t transition_count;    /* diagnostics; saturating              */
} TuningClimateState;

typedef struct {
    TuningForecastStatus status;
    int16_t forecast_max_dc; /* consumed only when status == OK           */
} TuningForecastInput;

/* Defaults: 30.0 C / 28.0 C. */
void tuning_climate_thresholds_defaults(TuningClimateThresholds *out);

/* Valid iff hot > cool and both within the forecast sanity band. */
bool tuning_climate_thresholds_valid(const TuningClimateThresholds *t);

/* Zero-initialize to UNKNOWN. */
void tuning_climate_state_init(TuningClimateState *out);

/*
 * One hysteresis step. Deterministic and idempotent: feeding the same
 * forecast repeatedly yields the same state and request, and
 * transition_count increments only on a real state change. `next` may
 * alias `cur` (the input is copied first). On any error the outputs are
 * left untouched. Rules:
 *  - status != OK           -> state unchanged, request FAIL_SAFE;
 *  - forecast out of sanity -> treated as invalid (FAIL_SAFE), no change;
 *  - forecast >= hot        -> HOT, request HOT_PROFILE;
 *  - forecast <= cool       -> NORMAL, request COOL_PROFILE;
 *  - in between             -> state retained; HOT -> HOT_PROFILE,
 *                              NORMAL -> COOL_PROFILE,
 *                              UNKNOWN -> RETAIN (no basis to decide).
 */
TuningPolicyError tuning_climate_step(const TuningClimateState *cur,
                                      const TuningClimateThresholds *th,
                                      const TuningForecastInput *in,
                                      TuningClimateState *next,
                                      TuningClimateRequest *out_request);

/* ------------------------------------------------------------------ */
/* Sensor-health model                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    TuningSensorStatus asic_temp;
    TuningSensorStatus vrm_temp;
    bool vrm_expected;             /* Gamma 601: true                     */
    TuningSensorStatus fan_tach;   /* the single observable fan (601)     */
    bool fan_control_uncertain;    /* PWM write fault / hardware_fault    */
} TuningSensorHealth;

/*
 * Classify an ASIC temperature reading (deci-C). Encodes the audited
 * firmware realities: read errors and the -1/-not-initialized sentinel are
 * INVALID (a raw -1 would otherwise silently sit below every threshold);
 * beyond-plausible values (EMC2101 diode faults read ~127 C) are
 * IMPLAUSIBLE.
 */
TuningSensorStatus tuning_classify_asic_temp_dc(int32_t temp_dc, bool read_error);

/*
 * Classify a VRM temperature reading (deci-C). `unchanged_streak` is the
 * number of consecutive identical samples (the TPS546 driver returns a
 * cached last value on SMBus failure, so a frozen reading is the failure
 * signature); `stale_streak_limit` 0 disables the staleness check.
 */
TuningSensorStatus tuning_classify_vrm_temp_dc(int32_t temp_dc, bool read_error,
                                               uint32_t unchanged_streak,
                                               uint32_t stale_streak_limit);

/*
 * Classify a fan TACHOMETER reading (Gate W6.3T).
 *
 * WHY THIS EXISTS. Gate W6.3.1 could not honestly build TuningSensorHealth
 * because `fan_tach` had no classifier, and TUNING_SENSOR_OK is the ZERO of
 * this enum — so a zero-initialised TuningSensorHealth silently asserts a
 * healthy fan. This function is the authority that removes the guess.
 *
 * IT INVENTS NO RPM THRESHOLD. The audited driver semantics already make 0 the
 * committed "no tach signal" answer, and it is the answer for BOTH failure
 * directions at once:
 *   - EMC2101_get_fan_speed() returns 0 when either TACH register read fails;
 *   - it also maps the 0xFFFF idle reading (which computes to 82 RPM) to 0,
 *     i.e. "not rotating";
 *   - Thermal_get_fan_speed() returns 0 when the board declares no fan
 *     controller at all.
 * W1 already defines TUNING_SENSOR_INVALID as "read error / -1 sentinel /
 * <= 0", so mapping rpm == 0 onto INVALID restates two committed contracts
 * rather than adding a third.
 *
 * `fan_expected` is the DECLARED installation fact (DeviceConfig EMC2101 /
 * EMC2103 / EMC2302), never a runtime guess. `read_error` is for callers whose
 * acquisition path can distinguish a bus failure; pass false when it cannot,
 * because rpm == 0 already fails closed for that case.
 *
 * FAIL CLOSED, AND ZERO-SAFE BY CONSTRUCTION: the all-zero argument set
 * (rpm 0, not expected, no error) returns MISSING, never OK. There is no
 * argument combination that yields OK without a strictly positive measured
 * RPM on a board that declares a fan.
 */
TuningSensorStatus tuning_classify_fan_tach(uint16_t rpm, bool fan_expected,
                                            bool read_error);

/* All sensors healthy enough to permit an UPGRADE (fail closed). */
bool tuning_sensor_health_upgrade_ok(const TuningSensorHealth *h);

/* Sensor integrity failure — precedence source 3 (forces safe stance):
 * ASIC temp not OK, or expected VRM not OK, or fan control uncertain.
 * A tach-only problem blocks upgrades but is not an integrity failure. */
bool tuning_sensor_health_integrity_failed(const TuningSensorHealth *h);

/* ------------------------------------------------------------------ */
/* Manual override                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    bool active;
    char profile_id[TUNING_PROFILE_ID_MAX];
    TuningActorClass actor;        /* MANUAL_API or OPERATOR_RECOVERY     */
    uint64_t created_epoch_s;      /* trusted epoch at creation           */
    uint64_t expires_epoch_s;      /* REQUIRED: bounded expiration        */
    uint64_t monotonic_expiry_us;  /* current-boot anchor; 0 = not armed  */
    uint16_t reason_code;          /* caller-scoped bounded tag           */
    uint32_t policy_generation;
} TuningManualOverride;

/*
 * Manual-override eligibility (task brief "MANUAL OVERRIDE" rejects).
 * `requested` is the registry-resolved profile (NULL -> PROFILE_UNKNOWN).
 * `ceiling_rank`/`ceiling_valid` is the current effective safety ceiling
 * expressed as a rank (see tuning_policy_evaluate — an invalid ceiling
 * fails closed). `now_epoch_s`/`now_trusted` come from the trusted time
 * provider (B2); an untrusted now is rejected. A safer-or-equal request
 * (rank <= current) is never blocked by cooldown; an upgrade during an
 * active cooldown is rejected (conservative reading of boundary 15/16).
 * `current_rank_valid` false treats the cooldown-upgrade check as an
 * upgrade (fail closed).
 */
TuningOverrideResult tuning_override_eligible(const TuningManualOverride *ov,
                                              const TuningProfile *requested,
                                              const TuningHardwareContext *hw,
                                              uint8_t ceiling_rank,
                                              bool ceiling_valid,
                                              uint8_t current_rank,
                                              bool current_rank_valid,
                                              bool cooldown_active,
                                              uint64_t now_epoch_s,
                                              bool now_trusted);

/* ------------------------------------------------------------------ */
/* Precedence arbitration                                              */
/* ------------------------------------------------------------------ */

/* Profile-role configuration (W2 persistence later; W1 defaults are the
 * registry ids). Empty string = role not configured. */
typedef struct {
    char cool_day_profile_id[TUNING_PROFILE_ID_MAX];
    char hot_day_profile_id[TUNING_PROFILE_ID_MAX];
    char emergency_profile_id[TUNING_PROFILE_ID_MAX];
} TuningPolicyRoles;

/* Registry + declared hardware the arbitration resolves ids against. */
typedef struct {
    const TuningProfile *profiles;
    size_t profile_count;
    TuningHardwareContext hw;
} TuningPolicyEnvironment;

typedef struct {
    bool active;
    TuningActorClass source; /* actor that started the cooldown           */
} TuningCooldownState;

typedef struct {
    uint32_t policy_generation;

    char current_profile_id[TUNING_PROFILE_ID_MAX];
    bool current_profile_known; /* false: id unset or not committed       */

    TuningPolicyRoles roles;

    /* Precedence 1-2 inputs (runtime facts, higher layers own them). */
    bool emergency_thermal_active;
    bool stability_rollback_active;
    char stability_rollback_profile_id[TUNING_PROFILE_ID_MAX]; /* ""=none */

    /* Precedence 3 input. */
    TuningSensorHealth sensors;

    /* Precedence 4/6 inputs. */
    bool trusted_time_valid;
    TuningClimateRequest climate_request;

    /* Precedence 5 input. */
    bool manual_present;
    TuningManualOverride manual;
    uint64_t now_epoch_s; /* trusted now for override expiry evaluation  */

    /* Upgrade gates (boundary 16). */
    TuningMiningHealth mining;
    TuningCooldownState cooldown;
    bool upgrade_inhibited; /* external latch (e.g. operator-recovery)   */
} TuningPolicyInput;

/*
 * Arbitration result — a pure INTENT. Nothing is applied here; W4 executes
 * it under ownership, persistence-before-action and health verification.
 */
typedef struct {
    TuningSelectionAction action;
    char profile_id[TUNING_PROFILE_ID_MAX]; /* set iff SELECT_PROFILE     */
    TuningActorClass actor;
    TuningPrecedenceSource winning_source;
    TuningReasonCode reason;
    TuningReasonCode secondary_reason; /* NONE when unused                */
    /* Manual-override surface: OK, a rejection code, ERR_INACTIVE when no
     * active override was supplied, or NOT_EVALUATED when an active
     * override was preempted by a higher-priority safety source.         */
    TuningOverrideResult override_result;
    bool is_upgrade;   /* rank strictly above known current               */
    bool is_downgrade; /* rank strictly below known current               */
    uint32_t policy_generation; /* echoed from input                      */
} TuningSelectionIntent;

/*
 * Evaluate the full precedence stack. Deterministic; on error the output
 * is untouched. Safety sources (1-4) only ever SELECT a strict downgrade
 * (or a move from an unknown current); the weather policy may SELECT an
 * upgrade only when every boundary-16 gate passes:
 * trusted time, sensors upgrade-ok, cooldown expired, mining STABLE,
 * no upgrade inhibit, known current profile, target auto-eligible, and
 * target rank strictly above current. Ownership acquisition is a W4
 * runtime gate on top of this intent.
 */
TuningPolicyError tuning_policy_evaluate(const TuningPolicyEnvironment *env,
                                         const TuningPolicyInput *in,
                                         TuningSelectionIntent *out);

/* Effective safety ceiling as a rank for override validation: what the
 * policy would allow at most right now, derived from the climate stance
 * (HOT -> hot-day rank; COOL -> cool-day rank; RETAIN -> known current
 * rank, else hot-day rank). False when no valid reference resolves. */
bool tuning_policy_effective_ceiling(const TuningPolicyEnvironment *env,
                                     const TuningPolicyInput *in,
                                     uint8_t *out_rank);

/* Bounded token strings. */
const char *tuning_actor_str(TuningActorClass a);
const char *tuning_source_str(TuningPrecedenceSource s);
const char *tuning_weather_state_str(TuningWeatherState s);
const char *tuning_forecast_status_str(TuningForecastStatus s);
const char *tuning_climate_request_str(TuningClimateRequest r);
const char *tuning_sensor_status_str(TuningSensorStatus s);
const char *tuning_mining_health_str(TuningMiningHealth m);
const char *tuning_selection_action_str(TuningSelectionAction a);
const char *tuning_reason_str(TuningReasonCode r);
const char *tuning_policy_error_str(TuningPolicyError e);
const char *tuning_override_result_str(TuningOverrideResult r);

#endif /* TUNING_POLICY_H_ */
