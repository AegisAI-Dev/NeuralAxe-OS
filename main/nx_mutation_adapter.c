/*
 * NeuralAxe Gate W6.1 — canonical tuning-snapshot provider.
 *
 * READ ONLY. It calls nvs_config getters and the committed thermal parsers and
 * nothing else: no setter, no NVS write, no restart, no task, timer, queue or
 * endpoint.
 *
 * IT REPORTS WHAT IS CONFIGURED, NOT WHAT IS RUNNING. The pilot's question is
 * whether the stored tuning changed, so the live ASIC ramp value, the live
 * regulator output and the live fan duty are deliberately NOT read — they move
 * constantly during healthy operation and would turn ordinary thermal control
 * into a false violation.
 *
 * TEXT NEVER REACHES THE BASELINE. The two settings stored as strings are
 * parsed HERE, by the subsystem that owns their grammar, into fixed-width
 * canonical values:
 *
 *   - the fan curve goes through the committed thermal_curve_parse(), so the
 *     baseline holds an ordered, validated, bounded array of points rather
 *     than a serialization whose formatting could differ;
 *   - the thermal mode goes through the committed strict parser and is stored
 *     as a stable enum.
 *
 * Every temporary string is freed on every path, and no pointer or allocation
 * survives into the snapshot.
 *
 * The whole file compiles to a single empty function without
 * CONFIG_NX_MUTATION_OBSERVABILITY.
 */

#include "sdkconfig.h"
#include "nx_mutation_adapter.h"

#ifdef CONFIG_NX_MUTATION_OBSERVABILITY

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nvs_config.h"
#include "nx_tuning_snapshot.h"
#include "thermal_control.h"

/*
 * This is the ONE translation unit that knows both models, so it is where the
 * canonical curve width is pinned to the committed one. Drift becomes a build
 * failure rather than a silent truncation of the safety comparison.
 */
_Static_assert(NX_TUNING_CURVE_POINTS == THERMAL_CURVE_POINTS,
               "canonical curve width must match the committed ThermalCurve");

/* Scale the stored float to tenths of a MHz, rejecting anything that is not a
 * finite in-range number. A rejected value stays 0, which makes the whole
 * snapshot invalid — an unreadable configuration must never look stable. */
static uint16_t frequency_x10(void)
{
    float f = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);
    float scaled;

    if (!isfinite(f) || f <= 0.0f) {
        return 0u;
    }
    scaled = f * 10.0f + 0.5f;   /* round to nearest tenth */
    if (scaled > (float)NX_TUNING_FREQ_X10_MAX) {
        return 0u;
    }
    return (uint16_t)scaled;
}

/*
 * Parse the stored fan curve into the canonical model.
 *
 * An empty value means "no curve configured" and yields a point count of 0
 * with every slot zeroed, so "unset" has exactly one representation. Anything
 * that is present but does not parse and validate — malformed, truncated,
 * out of range, mis-ordered — FAILS CLOSED: this firmware could not have
 * written it, so it is not a configuration the pilot will vouch for.
 *
 * Because the committed parser produces an ordered fixed-width array,
 * formatting differences that the grammar permits cannot change the canonical
 * result; the text is discarded here and never reaches the baseline.
 */
static bool read_curve(NxTuningSnapshot *out)
{
    char        *text = nvs_config_get_string(NVS_CONFIG_FAN_CURVE);
    ThermalCurve curve;
    bool         ok = false;
    unsigned     i;

    if (text == NULL) {
        return false;   /* unreadable: invalid key, wrong type, or no memory */
    }
    if (text[0] == '\0') {
        out->curve_point_count = 0u;      /* genuinely not configured */
        ok = true;
    } else if (strnlen(text, THERMAL_CURVE_STR_MAX + 1u) <= THERMAL_CURVE_STR_MAX &&
               thermal_curve_parse(text, &curve) == THERMAL_CURVE_OK &&
               thermal_curve_validate(&curve) == THERMAL_CURVE_OK) {
        for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
            out->curve[i].temp_c  = curve.temp_c[i];
            out->curve[i].fan_pct = curve.fan_pct[i];
        }
        out->curve_point_count = (uint16_t)NX_TUNING_CURVE_POINTS;
        ok = true;
    }
    free(text);   /* freed on every path */
    return ok;
}

/*
 * Parse the stored thermal mode into the stable canonical enum.
 *
 * The mode IS reachable from the generic settings PATCH loop, which does not
 * enforce the settings table's length range, so the committed strict parser is
 * the authority: exactly "target", "curve" or "manual". An empty value is the
 * factory default and maps to UNSET. The mapping is EXPLICIT rather than a
 * numeric cast, so a change to the committed enum's values cannot silently
 * remap a stored baseline.
 */
static bool read_mode(NxTuningSnapshot *out)
{
    char              *text = nvs_config_get_string(NVS_CONFIG_THERMAL_MODE);
    ThermalControlMode mode;
    bool               ok = false;

    if (text == NULL) {
        return false;
    }
    if (text[0] == '\0') {
        out->thermal_mode = (uint16_t)NX_TUNING_MODE_UNSET;
        ok = true;
    } else if (thermal_mode_from_string(text, &mode)) {
        switch (mode) {
        case THERMAL_MODE_TARGET:
            out->thermal_mode = (uint16_t)NX_TUNING_MODE_TARGET; ok = true; break;
        case THERMAL_MODE_CURVE:
            out->thermal_mode = (uint16_t)NX_TUNING_MODE_CURVE;  ok = true; break;
        case THERMAL_MODE_MANUAL:
            out->thermal_mode = (uint16_t)NX_TUNING_MODE_MANUAL; ok = true; break;
        default:
            ok = false; break;   /* a mode this build does not recognise */
        }
    }
    free(text);   /* freed on every path */
    return ok;
}

static bool read_tuning(NxTuningSnapshot *out)
{
    if (out == NULL) {
        return false;
    }
    nx_tuning_snapshot_init(out);

    out->frequency_mhz_x10 = frequency_x10();
    out->voltage_mv        = nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
    out->fan_mode          = nvs_config_get_bool(NVS_CONFIG_AUTO_FAN_SPEED) ? 1u : 0u;
    out->fan_percent       = nvs_config_get_u16(NVS_CONFIG_MANUAL_FAN_SPEED);
    out->fan_min_percent   = nvs_config_get_u16(NVS_CONFIG_MIN_FAN_SPEED);
    out->fan_hysteresis_c  = nvs_config_get_u16(NVS_CONFIG_FAN_CURVE_HYSTERESIS);
    out->temp_target_c     = nvs_config_get_u16(NVS_CONFIG_TEMP_TARGET);
    out->overheat_mode     = nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE) ? 1u : 0u;

    /* Either parse failing leaves NOTHING partial behind. */
    if (!read_curve(out) || !read_mode(out)) {
        nx_tuning_snapshot_init(out);
        return false;
    }
    return nx_tuning_snapshot_finalize(out);
}

void nx_mutation_adapter_install(void)
{
    nx_tuning_snapshot_provider_register(read_tuning);
}

#else  /* observability not compiled in */

void nx_mutation_adapter_install(void) { }

#endif /* CONFIG_NX_MUTATION_OBSERVABILITY */
