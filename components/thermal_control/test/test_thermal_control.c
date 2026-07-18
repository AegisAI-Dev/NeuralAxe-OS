#include <string.h>
#include "unity.h"
#include "thermal_control.h"

/* Default board-601 curve: 45:25 52:45 58:70 64:100 */

static ThermalCurve default_curve(void)
{
    ThermalCurve curve;
    thermal_curve_default(&curve);
    return curve;
}

/* ---- mode resolution and migration ---- */

TEST_CASE("mode: strict string parse", "[thermal_control]")
{
    ThermalControlMode mode;
    TEST_ASSERT_TRUE(thermal_mode_from_string("target", &mode));
    TEST_ASSERT_EQUAL(THERMAL_MODE_TARGET, mode);
    TEST_ASSERT_TRUE(thermal_mode_from_string("curve", &mode));
    TEST_ASSERT_EQUAL(THERMAL_MODE_CURVE, mode);
    TEST_ASSERT_TRUE(thermal_mode_from_string("manual", &mode));
    TEST_ASSERT_EQUAL(THERMAL_MODE_MANUAL, mode);

    TEST_ASSERT_FALSE(thermal_mode_from_string("", &mode));
    TEST_ASSERT_FALSE(thermal_mode_from_string("Curve", &mode));
    TEST_ASSERT_FALSE(thermal_mode_from_string("auto", &mode));
    TEST_ASSERT_FALSE(thermal_mode_from_string(NULL, &mode));
}

TEST_CASE("mode: legacy migration derives from autofanspeed", "[thermal_control]")
{
    /* Existing installations have no stored mode: autofanspeed=true must map
     * to TARGET, false to MANUAL. CURVE must never be entered implicitly. */
    TEST_ASSERT_EQUAL(THERMAL_MODE_TARGET, thermal_mode_resolve("", true));
    TEST_ASSERT_EQUAL(THERMAL_MODE_MANUAL, thermal_mode_resolve("", false));
    TEST_ASSERT_EQUAL(THERMAL_MODE_TARGET, thermal_mode_resolve(NULL, true));
    TEST_ASSERT_EQUAL(THERMAL_MODE_MANUAL, thermal_mode_resolve(NULL, false));
    /* Corrupt stored value is treated exactly like a legacy install. */
    TEST_ASSERT_EQUAL(THERMAL_MODE_TARGET, thermal_mode_resolve("garbage", true));
    TEST_ASSERT_EQUAL(THERMAL_MODE_MANUAL, thermal_mode_resolve("garbage", false));
    /* Explicit values win over autofanspeed. */
    TEST_ASSERT_EQUAL(THERMAL_MODE_CURVE, thermal_mode_resolve("curve", true));
    TEST_ASSERT_EQUAL(THERMAL_MODE_CURVE, thermal_mode_resolve("curve", false));
    TEST_ASSERT_EQUAL(THERMAL_MODE_MANUAL, thermal_mode_resolve("manual", true));
    TEST_ASSERT_EQUAL(THERMAL_MODE_TARGET, thermal_mode_resolve("target", false));
}

/* ---- curve validation ---- */

TEST_CASE("curve: default curve is valid and within board bounds", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    TEST_ASSERT_EQUAL(THERMAL_CURVE_OK, thermal_curve_validate(&curve));
    TEST_ASSERT_TRUE(curve.temp_c[THERMAL_CURVE_POINTS - 1] <= THERMAL_CURVE_TEMP_MAX_C);
    TEST_ASSERT_EQUAL(100, curve.fan_pct[THERMAL_CURVE_POINTS - 1]);
}

TEST_CASE("curve: validation rejects bad curves", "[thermal_control]")
{
    ThermalCurve curve;

    /* duplicate temperature */
    curve = default_curve();
    curve.temp_c[1] = curve.temp_c[0];
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_TEMP_ORDER, thermal_curve_validate(&curve));

    /* descending temperature */
    curve = default_curve();
    curve.temp_c[2] = 40;
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_TEMP_ORDER, thermal_curve_validate(&curve));

    /* descending fan percent */
    curve = default_curve();
    curve.fan_pct[3] = 10;
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_FAN_ORDER, thermal_curve_validate(&curve));

    /* temperature above the board limit */
    curve = default_curve();
    curve.temp_c[3] = THERMAL_CURVE_TEMP_MAX_C + 1;
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_TEMP_RANGE, thermal_curve_validate(&curve));

    /* temperature below the board minimum */
    curve = default_curve();
    curve.temp_c[0] = THERMAL_CURVE_TEMP_MIN_C - 1;
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_TEMP_RANGE, thermal_curve_validate(&curve));

    /* fan above 100 */
    curve = default_curve();
    curve.fan_pct[3] = 101;
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_FAN_RANGE, thermal_curve_validate(&curve));

    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_EMPTY, thermal_curve_validate(NULL));
}

/* ---- serialization ---- */

TEST_CASE("curve: serialize and parse round-trip", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    char buf[THERMAL_CURVE_STR_MAX];
    int len = thermal_curve_serialize(&curve, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_STRING("v1;45:25;52:45;58:70;64:100", buf);

    ThermalCurve parsed;
    TEST_ASSERT_EQUAL(THERMAL_CURVE_OK, thermal_curve_parse(buf, &parsed));
    TEST_ASSERT_EQUAL_MEMORY(&curve, &parsed, sizeof(curve));
}

TEST_CASE("curve: parse rejects malformed input", "[thermal_control]")
{
    ThermalCurve out;
    memset(&out, 0xAA, sizeof(out));
    ThermalCurve untouched;
    memcpy(&untouched, &out, sizeof(out));

    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_EMPTY, thermal_curve_parse(NULL, &out));
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_EMPTY, thermal_curve_parse("", &out));
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_VERSION, thermal_curve_parse("v2;45:25;52:45;58:70;64:100", &out));
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_VERSION, thermal_curve_parse("45:25;52:45;58:70;64:100", &out));
    /* too few points */
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_POINT_COUNT, thermal_curve_parse("v1;45:25;52:45;58:70", &out));
    /* too many points */
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_POINT_COUNT, thermal_curve_parse("v1;45:25;52:45;58:70;64:100;66:100", &out));
    /* garbage separators / non-numeric */
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_FORMAT, thermal_curve_parse("v1;45-25;52:45;58:70;64:100", &out));
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_FORMAT, thermal_curve_parse("v1;abc:25;52:45;58:70;64:100", &out));
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_FORMAT, thermal_curve_parse("v1;45:25;52:45;58:70;64:100x", &out));
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_FORMAT, thermal_curve_parse("v1;45:25;52:45;58:70;64:1000", &out));
    /* semantically invalid after parse */
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_TEMP_ORDER, thermal_curve_parse("v1;52:25;45:45;58:70;64:100", &out));
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_FAN_ORDER, thermal_curve_parse("v1;45:45;52:25;58:70;64:100", &out));
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_TEMP_RANGE, thermal_curve_parse("v1;10:25;52:45;58:70;64:100", &out));
    TEST_ASSERT_EQUAL(THERMAL_CURVE_ERR_FAN_RANGE, thermal_curve_parse("v1;45:25;52:45;58:70;64:101", &out));

    /* on every failure the output struct is untouched */
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &out, sizeof(out));
}

TEST_CASE("curve: serialize detects short buffer", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    char buf[8];
    TEST_ASSERT_EQUAL(-1, thermal_curve_serialize(&curve, buf, sizeof(buf)));
}

/* ---- stateless evaluation ---- */

TEST_CASE("eval: exact curve points", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    int8_t seg;
    TEST_ASSERT_EQUAL(25, thermal_curve_eval(&curve, 45.0f, 0, &seg));
    TEST_ASSERT_EQUAL(45, thermal_curve_eval(&curve, 52.0f, 0, &seg));
    TEST_ASSERT_EQUAL(70, thermal_curve_eval(&curve, 58.0f, 0, &seg));
    TEST_ASSERT_EQUAL(100, thermal_curve_eval(&curve, 64.0f, 0, &seg));
    TEST_ASSERT_EQUAL(THERMAL_CURVE_POINTS, seg);
}

TEST_CASE("eval: interpolation between every adjacent point", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    int8_t seg;

    /* midpoint of 45..52 (25%..45%): 48.5 -> 35% */
    TEST_ASSERT_EQUAL(35, thermal_curve_eval(&curve, 48.5f, 0, &seg));
    TEST_ASSERT_EQUAL(1, seg);
    /* midpoint of 52..58 (45%..70%): 55 -> 57.5 -> rounds to 58 */
    TEST_ASSERT_EQUAL(58, thermal_curve_eval(&curve, 55.0f, 0, &seg));
    TEST_ASSERT_EQUAL(2, seg);
    /* midpoint of 58..64 (70%..100%): 61 -> 85% */
    TEST_ASSERT_EQUAL(85, thermal_curve_eval(&curve, 61.0f, 0, &seg));
    TEST_ASSERT_EQUAL(3, seg);
}

TEST_CASE("eval: below first point uses first percent with min-fan floor", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    int8_t seg;
    TEST_ASSERT_EQUAL(25, thermal_curve_eval(&curve, 30.0f, 0, &seg));
    TEST_ASSERT_EQUAL(0, seg);
    /* the configured minimum fan stays authoritative as a floor */
    TEST_ASSERT_EQUAL(40, thermal_curve_eval(&curve, 30.0f, 40, &seg));
    /* the floor applies across the whole curve, not just below it */
    TEST_ASSERT_EQUAL(50, thermal_curve_eval(&curve, 48.5f, 50, &seg));
}

TEST_CASE("eval: above final point holds final percent", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    int8_t seg;
    TEST_ASSERT_EQUAL(100, thermal_curve_eval(&curve, 69.0f, 0, &seg));
    TEST_ASSERT_EQUAL(THERMAL_CURVE_POINTS, seg);

    /* a curve whose final point is below 100 stays at that value */
    ThermalCurve low = default_curve();
    low.fan_pct[3] = 80;
    TEST_ASSERT_EQUAL(80, thermal_curve_eval(&low, 69.0f, 0, &seg));
}

TEST_CASE("eval: min fan floor is clamped to 100", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    TEST_ASSERT_EQUAL(100, thermal_curve_eval(&curve, 30.0f, 255, NULL));
}

/* ---- stateful control step: hysteresis ---- */

TEST_CASE("step: startup before valid telemetry uses the proven fallback", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    ThermalCurveState state;
    thermal_curve_state_init(&state);

    /* sensor not ready (ASIC uninitialized reports -1) */
    ThermalCurveDecision d = thermal_curve_step(&curve, &state, -1.0f, true, 25, 2);
    TEST_ASSERT_EQUAL(THERMAL_FALLBACK_FAN_PCT, d.applied_pct);
    TEST_ASSERT_EQUAL(THERMAL_REASON_SENSOR_INVALID, d.reason);
    TEST_ASSERT_EQUAL(-1, d.segment);

    /* explicitly flagged invalid */
    d = thermal_curve_step(&curve, &state, 50.0f, false, 25, 2);
    TEST_ASSERT_EQUAL(THERMAL_FALLBACK_FAN_PCT, d.applied_pct);
    TEST_ASSERT_EQUAL(THERMAL_REASON_SENSOR_INVALID, d.reason);

    /* zero must not be treated as a valid cool temperature */
    d = thermal_curve_step(&curve, &state, 0.0f, true, 25, 2);
    TEST_ASSERT_EQUAL(THERMAL_FALLBACK_FAN_PCT, d.applied_pct);
    TEST_ASSERT_EQUAL(THERMAL_REASON_SENSOR_INVALID, d.reason);
}

TEST_CASE("step: sensor loss mid-run resets to fallback", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    ThermalCurveState state;
    thermal_curve_state_init(&state);

    ThermalCurveDecision d = thermal_curve_step(&curve, &state, 61.0f, true, 25, 2);
    TEST_ASSERT_EQUAL(85, d.applied_pct);

    d = thermal_curve_step(&curve, &state, -1.0f, true, 25, 2);
    TEST_ASSERT_EQUAL(THERMAL_FALLBACK_FAN_PCT, d.applied_pct);
    TEST_ASSERT_EQUAL(THERMAL_REASON_SENSOR_INVALID, d.reason);
    TEST_ASSERT_FALSE(state.has_applied);
}

TEST_CASE("step: impossible temperature fails high to full fan", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    ThermalCurveState state;
    thermal_curve_state_init(&state);

    ThermalCurveDecision d = thermal_curve_step(&curve, &state, 300.0f, true, 25, 2);
    TEST_ASSERT_EQUAL(100, d.applied_pct);
    TEST_ASSERT_EQUAL(THERMAL_REASON_TEMP_IMPLAUSIBLE, d.reason);
}

TEST_CASE("step: rising temperature raises fan immediately, no delay", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    ThermalCurveState state;
    thermal_curve_state_init(&state);

    ThermalCurveDecision d = thermal_curve_step(&curve, &state, 50.0f, true, 25, 2);
    TEST_ASSERT_EQUAL(39, d.applied_pct); /* 25 + (50-45)/(52-45)*20 = 39.28 -> 39 */

    /* a sudden dangerous rise must escalate in the SAME step */
    d = thermal_curve_step(&curve, &state, 64.0f, true, 25, 2);
    TEST_ASSERT_EQUAL(100, d.applied_pct);
    TEST_ASSERT_FALSE(d.hysteresis_holding);
    TEST_ASSERT_EQUAL(THERMAL_REASON_ABOVE_CURVE, d.reason);

    /* even while a hysteresis hold is active, rising demand applies at once */
    d = thermal_curve_step(&curve, &state, 63.0f, true, 25, 2); /* falling: hold */
    TEST_ASSERT_TRUE(d.hysteresis_holding);
    TEST_ASSERT_EQUAL(100, d.applied_pct);
    d = thermal_curve_step(&curve, &state, 65.0f, true, 25, 2); /* rising again */
    TEST_ASSERT_EQUAL(100, d.applied_pct);
    TEST_ASSERT_FALSE(d.hysteresis_holding);
}

TEST_CASE("step: falling temperature is gated by hysteresis", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    ThermalCurveState state;
    thermal_curve_state_init(&state);

    ThermalCurveDecision d = thermal_curve_step(&curve, &state, 58.0f, true, 25, 2);
    TEST_ASSERT_EQUAL(70, d.applied_pct);

    /* 0.5 degC dip: requested falls but the applied duty holds */
    d = thermal_curve_step(&curve, &state, 57.5f, true, 25, 2);
    TEST_ASSERT_LESS_THAN(70, d.requested_pct);
    TEST_ASSERT_EQUAL(70, d.applied_pct);
    TEST_ASSERT_TRUE(d.hysteresis_holding);
    TEST_ASSERT_EQUAL(THERMAL_REASON_HYSTERESIS_HOLD, d.reason);

    /* still inside the band */
    d = thermal_curve_step(&curve, &state, 56.5f, true, 25, 2);
    TEST_ASSERT_EQUAL(70, d.applied_pct);
    TEST_ASSERT_TRUE(d.hysteresis_holding);

    /* cooled a full 2 degC below the last-apply temperature: step down */
    d = thermal_curve_step(&curve, &state, 56.0f, true, 25, 2);
    TEST_ASSERT_FALSE(d.hysteresis_holding);
    TEST_ASSERT_EQUAL(d.requested_pct, d.applied_pct);
    TEST_ASSERT_EQUAL(62, d.applied_pct); /* 45 + (56-52)/(58-52)*25 = 61.67 -> 62 */
}

TEST_CASE("step: rapid alternating temperatures do not oscillate", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    ThermalCurveState state;
    thermal_curve_state_init(&state);

    thermal_curve_step(&curve, &state, 58.0f, true, 25, 2);
    uint8_t duty_at_peak = state.applied_pct;
    TEST_ASSERT_EQUAL(70, duty_at_peak);

    /* temperature flutters +-0.6 degC around the 58 degC point: the applied
     * duty must stay pinned at the ratcheted maximum the whole time */
    for (int i = 0; i < 50; i++) {
        float temp = (i % 2 == 0) ? 57.4f : 58.6f;
        ThermalCurveDecision d = thermal_curve_step(&curve, &state, temp, true, 25, 2);
        TEST_ASSERT_GREATER_OR_EQUAL(duty_at_peak, d.applied_pct);
        duty_at_peak = state.applied_pct;
    }
    /* 58.6 interpolates to 73; the duty ratchets there and holds */
    TEST_ASSERT_EQUAL(73, state.applied_pct);
}

TEST_CASE("step: controlled downward staircase during steady cooling", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    ThermalCurveState state;
    thermal_curve_state_init(&state);

    thermal_curve_step(&curve, &state, 64.0f, true, 25, 2);
    TEST_ASSERT_EQUAL(100, state.applied_pct);

    /* cool smoothly from 64 to below the first point (past the hysteresis
     * band): duty must fall monotonically in gated steps and settle at the
     * first-point value at the bottom */
    uint8_t previous = 100;
    for (float temp = 63.5f; temp >= 43.0f; temp -= 0.5f) {
        ThermalCurveDecision d = thermal_curve_step(&curve, &state, temp, true, 25, 2);
        TEST_ASSERT_LESS_OR_EQUAL(previous, d.applied_pct);
        TEST_ASSERT_GREATER_OR_EQUAL(d.requested_pct, d.applied_pct);
        previous = d.applied_pct;
    }
    TEST_ASSERT_EQUAL(25, state.applied_pct);
}

TEST_CASE("step: zero hysteresis follows the curve directly", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    ThermalCurveState state;
    thermal_curve_state_init(&state);

    thermal_curve_step(&curve, &state, 58.0f, true, 25, 0);
    TEST_ASSERT_EQUAL(70, state.applied_pct);
    ThermalCurveDecision d = thermal_curve_step(&curve, &state, 57.0f, true, 25, 0);
    TEST_ASSERT_EQUAL(d.requested_pct, d.applied_pct);
    TEST_ASSERT_FALSE(d.hysteresis_holding);
}

TEST_CASE("step: hysteresis parameter is clamped to the safe bound", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    ThermalCurveState state;
    thermal_curve_state_init(&state);

    thermal_curve_step(&curve, &state, 58.0f, true, 25, 255);
    /* falling by more than THERMAL_HYSTERESIS_MAX_C must step down even
     * though the caller passed an absurd hysteresis value */
    ThermalCurveDecision d = thermal_curve_step(&curve, &state, 58.0f - (float) THERMAL_HYSTERESIS_MAX_C, true, 25, 255);
    TEST_ASSERT_FALSE(d.hysteresis_holding);
    TEST_ASSERT_EQUAL(d.requested_pct, d.applied_pct);
}

TEST_CASE("step: min fan floor enforced in curve mode", "[thermal_control]")
{
    ThermalCurve curve = default_curve();
    ThermalCurveState state;
    thermal_curve_state_init(&state);

    ThermalCurveDecision d = thermal_curve_step(&curve, &state, 30.0f, true, 40, 2);
    TEST_ASSERT_EQUAL(40, d.applied_pct);
    TEST_ASSERT_EQUAL(THERMAL_REASON_BELOW_CURVE, d.reason);
    TEST_ASSERT_EQUAL(0, d.segment);
}
