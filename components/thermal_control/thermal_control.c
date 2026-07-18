#include "thermal_control.h"

#include <string.h>
#include <stdio.h>

/* ---- mode ---- */

bool thermal_mode_from_string(const char *s, ThermalControlMode *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    if (strcmp(s, "target") == 0) {
        *out = THERMAL_MODE_TARGET;
        return true;
    }
    if (strcmp(s, "curve") == 0) {
        *out = THERMAL_MODE_CURVE;
        return true;
    }
    if (strcmp(s, "manual") == 0) {
        *out = THERMAL_MODE_MANUAL;
        return true;
    }
    return false;
}

const char *thermal_mode_to_string(ThermalControlMode mode)
{
    switch (mode) {
        case THERMAL_MODE_TARGET: return "target";
        case THERMAL_MODE_CURVE:  return "curve";
        case THERMAL_MODE_MANUAL: return "manual";
        default:                  return "target";
    }
}

ThermalControlMode thermal_mode_resolve(const char *mode_str, bool autofanspeed)
{
    ThermalControlMode mode;
    if (thermal_mode_from_string(mode_str, &mode)) {
        return mode;
    }
    return autofanspeed ? THERMAL_MODE_TARGET : THERMAL_MODE_MANUAL;
}

/* ---- curve ---- */

void thermal_curve_default(ThermalCurve *out)
{
    static const uint8_t temps[THERMAL_CURVE_POINTS] = {45, 52, 58, 64};
    static const uint8_t fans[THERMAL_CURVE_POINTS] = {25, 45, 70, 100};
    if (out == NULL) {
        return;
    }
    memcpy(out->temp_c, temps, sizeof(temps));
    memcpy(out->fan_pct, fans, sizeof(fans));
}

ThermalCurveStatus thermal_curve_validate(const ThermalCurve *curve)
{
    if (curve == NULL) {
        return THERMAL_CURVE_ERR_EMPTY;
    }
    for (int i = 0; i < THERMAL_CURVE_POINTS; i++) {
        if (curve->temp_c[i] < THERMAL_CURVE_TEMP_MIN_C || curve->temp_c[i] > THERMAL_CURVE_TEMP_MAX_C) {
            return THERMAL_CURVE_ERR_TEMP_RANGE;
        }
        if (curve->fan_pct[i] > THERMAL_CURVE_FAN_MAX_PCT) {
            return THERMAL_CURVE_ERR_FAN_RANGE;
        }
        if (i > 0) {
            if (curve->temp_c[i] <= curve->temp_c[i - 1]) {
                return THERMAL_CURVE_ERR_TEMP_ORDER;
            }
            if (curve->fan_pct[i] < curve->fan_pct[i - 1]) {
                return THERMAL_CURVE_ERR_FAN_ORDER;
            }
        }
    }
    return THERMAL_CURVE_OK;
}

/* Parse a non-negative integer of at most three digits; advances *p.
 * Returns -1 on malformed input. */
static int parse_uint3(const char **p)
{
    const char *s = *p;
    int value = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
        if (++digits > 3) {
            return -1;
        }
    }
    if (digits == 0) {
        return -1;
    }
    *p = s;
    return value;
}

ThermalCurveStatus thermal_curve_parse(const char *str, ThermalCurve *out)
{
    if (str == NULL || str[0] == '\0') {
        return THERMAL_CURVE_ERR_EMPTY;
    }
    if (out == NULL) {
        return THERMAL_CURVE_ERR_EMPTY;
    }

    const char *p = str;
    size_t prefix_len = strlen(THERMAL_CURVE_VERSION_PREFIX);
    if (strncmp(p, THERMAL_CURVE_VERSION_PREFIX, prefix_len) != 0) {
        return THERMAL_CURVE_ERR_VERSION;
    }
    p += prefix_len;

    ThermalCurve parsed;
    for (int i = 0; i < THERMAL_CURVE_POINTS; i++) {
        if (*p != ';') {
            return (*p == '\0') ? THERMAL_CURVE_ERR_POINT_COUNT : THERMAL_CURVE_ERR_FORMAT;
        }
        p++;
        int temp = parse_uint3(&p);
        if (temp < 0 || *p != ':') {
            return THERMAL_CURVE_ERR_FORMAT;
        }
        p++;
        int fan = parse_uint3(&p);
        if (fan < 0) {
            return THERMAL_CURVE_ERR_FORMAT;
        }
        if (temp > 255 || fan > 255) {
            return THERMAL_CURVE_ERR_FORMAT;
        }
        parsed.temp_c[i] = (uint8_t) temp;
        parsed.fan_pct[i] = (uint8_t) fan;
    }
    if (*p != '\0') {
        return (*p == ';') ? THERMAL_CURVE_ERR_POINT_COUNT : THERMAL_CURVE_ERR_FORMAT;
    }

    ThermalCurveStatus status = thermal_curve_validate(&parsed);
    if (status != THERMAL_CURVE_OK) {
        return status;
    }
    *out = parsed;
    return THERMAL_CURVE_OK;
}

int thermal_curve_serialize(const ThermalCurve *curve, char *buf, size_t buflen)
{
    if (curve == NULL || buf == NULL) {
        return -1;
    }
    int written = snprintf(buf, buflen, THERMAL_CURVE_VERSION_PREFIX ";%u:%u;%u:%u;%u:%u;%u:%u",
                           curve->temp_c[0], curve->fan_pct[0],
                           curve->temp_c[1], curve->fan_pct[1],
                           curve->temp_c[2], curve->fan_pct[2],
                           curve->temp_c[3], curve->fan_pct[3]);
    if (written < 0 || (size_t) written >= buflen) {
        return -1;
    }
    return written;
}

const char *thermal_curve_status_str(ThermalCurveStatus status)
{
    switch (status) {
        case THERMAL_CURVE_OK:              return "ok";
        case THERMAL_CURVE_ERR_EMPTY:       return "empty";
        case THERMAL_CURVE_ERR_FORMAT:      return "malformed";
        case THERMAL_CURVE_ERR_VERSION:     return "unknown version";
        case THERMAL_CURVE_ERR_POINT_COUNT: return "wrong point count";
        case THERMAL_CURVE_ERR_TEMP_RANGE:  return "temperature out of range";
        case THERMAL_CURVE_ERR_TEMP_ORDER:  return "temperatures not ascending";
        case THERMAL_CURVE_ERR_FAN_RANGE:   return "fan percent out of range";
        case THERMAL_CURVE_ERR_FAN_ORDER:   return "fan percents decreasing";
        default:                            return "invalid";
    }
}

const char *thermal_curve_reason_str(ThermalCurveReason reason)
{
    switch (reason) {
        case THERMAL_REASON_NONE:             return "none";
        case THERMAL_REASON_CURVE_ACTIVE:     return "curve active";
        case THERMAL_REASON_BELOW_CURVE:      return "below curve, minimum fan";
        case THERMAL_REASON_ABOVE_CURVE:      return "above final curve point";
        case THERMAL_REASON_HYSTERESIS_HOLD:  return "hysteresis hold";
        case THERMAL_REASON_SENSOR_INVALID:   return "waiting for valid sensor data";
        case THERMAL_REASON_TEMP_IMPLAUSIBLE: return "implausible temperature, full fan";
        default:                              return "none";
    }
}

uint8_t thermal_curve_eval(const ThermalCurve *curve, float temp_c,
                           uint8_t min_fan_pct, int8_t *segment_out)
{
    int8_t segment;
    uint8_t pct;

    if (min_fan_pct > THERMAL_CURVE_FAN_MAX_PCT) {
        min_fan_pct = THERMAL_CURVE_FAN_MAX_PCT;
    }

    if (temp_c < (float) curve->temp_c[0]) {
        segment = 0;
        pct = curve->fan_pct[0];
    } else if (temp_c >= (float) curve->temp_c[THERMAL_CURVE_POINTS - 1]) {
        segment = THERMAL_CURVE_POINTS;
        pct = curve->fan_pct[THERMAL_CURVE_POINTS - 1];
    } else {
        segment = 1;
        pct = curve->fan_pct[0];
        for (int i = 1; i < THERMAL_CURVE_POINTS; i++) {
            if (temp_c < (float) curve->temp_c[i]) {
                float t0 = (float) curve->temp_c[i - 1];
                float t1 = (float) curve->temp_c[i];
                float p0 = (float) curve->fan_pct[i - 1];
                float p1 = (float) curve->fan_pct[i];
                /* validate() guarantees t1 > t0 */
                float interp = p0 + (temp_c - t0) * (p1 - p0) / (t1 - t0);
                pct = (uint8_t) (interp + 0.5f);
                segment = (int8_t) i;
                break;
            }
        }
    }

    if (pct < min_fan_pct) {
        pct = min_fan_pct;
    }
    if (pct > THERMAL_CURVE_FAN_MAX_PCT) {
        pct = THERMAL_CURVE_FAN_MAX_PCT;
    }
    if (segment_out != NULL) {
        *segment_out = segment;
    }
    return pct;
}

void thermal_curve_state_init(ThermalCurveState *state)
{
    if (state == NULL) {
        return;
    }
    state->has_applied = false;
    state->last_apply_temp_c = 0.0f;
    state->applied_pct = 0;
}

ThermalCurveDecision thermal_curve_step(const ThermalCurve *curve,
                                        ThermalCurveState *state,
                                        float temp_c, bool temp_valid,
                                        uint8_t min_fan_pct,
                                        uint8_t hysteresis_c)
{
    ThermalCurveDecision decision = {
        .requested_pct = THERMAL_FALLBACK_FAN_PCT,
        .applied_pct = THERMAL_FALLBACK_FAN_PCT,
        .segment = -1,
        .reason = THERMAL_REASON_SENSOR_INVALID,
        .hysteresis_holding = false,
    };

    if (hysteresis_c > THERMAL_HYSTERESIS_MAX_C) {
        hysteresis_c = THERMAL_HYSTERESIS_MAX_C;
    }

    /* No valid control temperature: the proven TARGET-mode fallback duty,
     * applied immediately. State resets so recovery starts fresh. */
    if (!temp_valid || !(temp_c > 0.0f)) {
        thermal_curve_state_init(state);
        return decision;
    }

    /* Impossible reading: fail high, immediately. */
    if (temp_c > THERMAL_TEMP_PLAUSIBLE_MAX_C) {
        decision.requested_pct = THERMAL_CURVE_FAN_MAX_PCT;
        decision.applied_pct = THERMAL_CURVE_FAN_MAX_PCT;
        decision.reason = THERMAL_REASON_TEMP_IMPLAUSIBLE;
        state->has_applied = true;
        state->last_apply_temp_c = temp_c;
        state->applied_pct = THERMAL_CURVE_FAN_MAX_PCT;
        return decision;
    }

    int8_t segment = -1;
    uint8_t requested = thermal_curve_eval(curve, temp_c, min_fan_pct, &segment);
    decision.requested_pct = requested;
    decision.segment = segment;
    if (segment == 0) {
        decision.reason = THERMAL_REASON_BELOW_CURVE;
    } else if (segment >= THERMAL_CURVE_POINTS) {
        decision.reason = THERMAL_REASON_ABOVE_CURVE;
    } else {
        decision.reason = THERMAL_REASON_CURVE_ACTIVE;
    }

    if (!state->has_applied || requested >= state->applied_pct) {
        /* First decision, equal demand, or RISING demand: apply immediately.
         * Upward transitions are never delayed. */
        if (!state->has_applied || requested != state->applied_pct) {
            state->last_apply_temp_c = temp_c;
        }
        state->has_applied = true;
        state->applied_pct = requested;
        decision.applied_pct = requested;
        return decision;
    }

    /* Falling demand: only step down once the temperature has genuinely
     * cooled past the hysteresis band; otherwise hold the higher duty. */
    if (temp_c <= state->last_apply_temp_c - (float) hysteresis_c) {
        state->last_apply_temp_c = temp_c;
        state->applied_pct = requested;
        decision.applied_pct = requested;
        return decision;
    }

    decision.applied_pct = state->applied_pct;
    decision.hysteresis_holding = true;
    decision.reason = THERMAL_REASON_HYSTERESIS_HOLD;
    return decision;
}
