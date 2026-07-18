#ifndef THERMAL_CONTROL_H_
#define THERMAL_CONTROL_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * NeuralAxe thermal-control core (Phase 2H, board 601 / BM1370).
 *
 * Pure calculation logic only: no hardware access, no NVS, no FreeRTOS, no
 * heap allocation. The fan controller task feeds temperatures in and writes
 * the resulting percentage to PWM itself. Everything here is deterministic
 * and host/QEMU testable.
 *
 * Safety contract: this module decides NORMAL-operation fan duty only.
 * Emergency thermal protection (overheat mode -> 100% fan, mining stop,
 * VRM limits, watchdogs) lives above this module in the fan controller and
 * power management tasks and always overrides its output.
 */

#define THERMAL_CURVE_POINTS 4

/* Serialized curve: "v1;45:25;52:45;58:70;64:100" */
#define THERMAL_CURVE_STR_MAX 48
#define THERMAL_CURVE_VERSION_PREFIX "v1"

/* Board-601 curve bounds. The ASIC hard-throttle limit is 75 degC
 * (power_management_task.c THROTTLE_TEMP); curve temperatures stop 5 degC
 * below it so normal control never reaches into the protection band. */
#define THERMAL_CURVE_TEMP_MIN_C 20
#define THERMAL_CURVE_TEMP_MAX_C 70
#define THERMAL_CURVE_FAN_MIN_PCT 0
#define THERMAL_CURVE_FAN_MAX_PCT 100

#define THERMAL_HYSTERESIS_DEFAULT_C 2
#define THERMAL_HYSTERESIS_MAX_C 8

/* Fail-safe duty when no valid control temperature exists. Identical to the
 * existing TARGET-mode behavior ("Startup" context, fan_controller_task.c). */
#define THERMAL_FALLBACK_FAN_PCT 70

/* Control temperatures above this are treated as sensor faults and drive the
 * fan to 100% (fail-high). The BM1370 diode cannot legitimately read this. */
#define THERMAL_TEMP_PLAUSIBLE_MAX_C 120.0f

typedef enum {
    THERMAL_MODE_TARGET = 0, /* existing PID target-temperature control */
    THERMAL_MODE_CURVE  = 1, /* NeuralAxe fan curve (opt-in) */
    THERMAL_MODE_MANUAL = 2, /* existing manual fan percentage */
} ThermalControlMode;

typedef struct {
    uint8_t temp_c[THERMAL_CURVE_POINTS];
    uint8_t fan_pct[THERMAL_CURVE_POINTS];
} ThermalCurve;

typedef enum {
    THERMAL_CURVE_OK = 0,
    THERMAL_CURVE_ERR_EMPTY,       /* NULL/empty string */
    THERMAL_CURVE_ERR_FORMAT,      /* malformed serialization */
    THERMAL_CURVE_ERR_VERSION,     /* unknown format version */
    THERMAL_CURVE_ERR_POINT_COUNT, /* not exactly THERMAL_CURVE_POINTS */
    THERMAL_CURVE_ERR_TEMP_RANGE,  /* point outside [TEMP_MIN, TEMP_MAX] */
    THERMAL_CURVE_ERR_TEMP_ORDER,  /* duplicate or descending temperature */
    THERMAL_CURVE_ERR_FAN_RANGE,   /* percent outside [0, 100] */
    THERMAL_CURVE_ERR_FAN_ORDER,   /* descending fan percentage */
} ThermalCurveStatus;

typedef enum {
    THERMAL_REASON_NONE = 0,
    THERMAL_REASON_CURVE_ACTIVE,     /* interpolating between two points */
    THERMAL_REASON_BELOW_CURVE,      /* below first point: min-fan floor */
    THERMAL_REASON_ABOVE_CURVE,      /* at/above final point */
    THERMAL_REASON_HYSTERESIS_HOLD,  /* holding higher duty until temp falls */
    THERMAL_REASON_SENSOR_INVALID,   /* no valid temperature: fallback duty */
    THERMAL_REASON_TEMP_IMPLAUSIBLE, /* impossible reading: full fan */
} ThermalCurveReason;

/* Hysteresis state. Owned by the caller (fan controller task); this module
 * never allocates. */
typedef struct {
    bool has_applied;
    float last_apply_temp_c; /* control temperature when duty last changed */
    uint8_t applied_pct;
} ThermalCurveState;

typedef struct {
    uint8_t requested_pct; /* raw curve output after min-fan floor */
    uint8_t applied_pct;   /* after the hysteresis policy */
    /* -1 none/invalid, 0 below first point, k in 1..3 between point k-1 and
     * point k, THERMAL_CURVE_POINTS at/above the final point. */
    int8_t segment;
    ThermalCurveReason reason;
    bool hysteresis_holding;
} ThermalCurveDecision;

/* ---- mode ---- */

/* Strict parse of "target" | "curve" | "manual". Returns false otherwise. */
bool thermal_mode_from_string(const char *s, ThermalControlMode *out);
const char *thermal_mode_to_string(ThermalControlMode mode);

/* Resolve the effective configured mode. An empty/unknown mode string means
 * the installation predates the mode field: derive from the legacy
 * autofanspeed flag (true -> TARGET, false -> MANUAL). CURVE is therefore
 * strictly opt-in: it requires an explicitly stored "curve" mode string. */
ThermalControlMode thermal_mode_resolve(const char *mode_str, bool autofanspeed);

/* ---- curve ---- */

/* Documented board-601 defaults: 45:25 52:45 58:70 64:100. The first point
 * matches the stock minimum-fan default (25%), the final point reaches 100%
 * at 64 degC, 11 degC below the 75 degC hard-throttle limit. */
void thermal_curve_default(ThermalCurve *out);

ThermalCurveStatus thermal_curve_validate(const ThermalCurve *curve);

/* Strict parse of the "v1;T:P;T:P;T:P;T:P" form, then validate.
 * On any error *out is left untouched. */
ThermalCurveStatus thermal_curve_parse(const char *str, ThermalCurve *out);

/* Serialize into buf; returns length written, or -1 if buf is too small.
 * Round-trips exactly through thermal_curve_parse. */
int thermal_curve_serialize(const ThermalCurve *curve, char *buf, size_t buflen);

const char *thermal_curve_status_str(ThermalCurveStatus status);
const char *thermal_curve_reason_str(ThermalCurveReason reason);

/* Stateless curve evaluation: linear interpolation between points, first
 * point's percent below the curve, final point's percent above it, then the
 * min-fan floor. Deterministic; rounds to the nearest integer percent. */
uint8_t thermal_curve_eval(const ThermalCurve *curve, float temp_c,
                           uint8_t min_fan_pct, int8_t *segment_out);

void thermal_curve_state_init(ThermalCurveState *state);

/*
 * One control decision. Policy (documented, tested):
 *  - invalid temperature (temp_valid false or temp <= 0):
 *      THERMAL_FALLBACK_FAN_PCT immediately, state reset — identical to the
 *      existing TARGET-mode startup/sensor-failure behavior;
 *  - implausible temperature (> THERMAL_TEMP_PLAUSIBLE_MAX_C): 100% fan
 *      immediately (fail-high);
 *  - rising demand (requested > applied): applied IMMEDIATELY — upward
 *      transitions are never delayed by hysteresis;
 *  - falling demand (requested < applied): applied only once the control
 *      temperature has dropped at least hysteresis_c below the temperature
 *      recorded at the last duty change; until then the higher duty holds.
 * hysteresis_c is clamped to [0, THERMAL_HYSTERESIS_MAX_C].
 */
ThermalCurveDecision thermal_curve_step(const ThermalCurve *curve,
                                        ThermalCurveState *state,
                                        float temp_c, bool temp_valid,
                                        uint8_t min_fan_pct,
                                        uint8_t hysteresis_c);

#endif /* THERMAL_CONTROL_H_ */
