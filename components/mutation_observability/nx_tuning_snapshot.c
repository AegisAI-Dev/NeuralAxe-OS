/*
 * NeuralAxe — canonical tuning snapshot (Gate W6.1).
 * See include/nx_tuning_snapshot.h for why exact comparison, and not a
 * 32-bit digest, is the safety authority.
 */

#include <string.h>

#include "sdkconfig.h"
#include "nx_tuning_snapshot.h"

const char *nx_tuning_thermal_mode_str(NxTuningThermalMode m)
{
    switch (m) {
    case NX_TUNING_MODE_UNSET:  return "MODE_UNSET";
    case NX_TUNING_MODE_TARGET: return "MODE_TARGET";
    case NX_TUNING_MODE_CURVE:  return "MODE_CURVE";
    case NX_TUNING_MODE_MANUAL: return "MODE_MANUAL";
    case NX_TUNING_MODE__COUNT:
    default:                    return "MODE_UNKNOWN";
    }
}

void nx_tuning_snapshot_init(NxTuningSnapshot *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->version = NX_TUNING_SNAPSHOT_VERSION;
        out->size    = (uint32_t)sizeof(*out);
        out->valid   = false;
    }
}

bool nx_tuning_snapshot_finalize(NxTuningSnapshot *out)
{
    unsigned i;

    if (out == NULL) {
        return false;
    }
    out->version = NX_TUNING_SNAPSHOT_VERSION;
    out->size    = (uint32_t)sizeof(*out);

    if (out->frequency_mhz_x10 == 0u ||
        out->frequency_mhz_x10 > NX_TUNING_FREQ_X10_MAX ||
        out->voltage_mv == 0u || out->voltage_mv > NX_TUNING_VOLTAGE_MAX ||
        out->fan_mode > 1u || out->fan_percent > 100u ||
        out->fan_min_percent > 100u ||
        out->fan_hysteresis_c > NX_TUNING_TEMP_MAX ||
        out->temp_target_c == 0u || out->temp_target_c > NX_TUNING_TEMP_MAX ||
        out->overheat_mode > 1u ||
        out->thermal_mode >= (uint16_t)NX_TUNING_MODE__COUNT) {
        out->valid = false;
        return false;
    }
    /* The curve is present in full or not at all: a partially populated curve
     * is not a configuration this firmware could have written. */
    if (out->curve_point_count != 0u &&
        out->curve_point_count != (uint16_t)NX_TUNING_CURVE_POINTS) {
        out->valid = false;
        return false;
    }
    if (out->curve_point_count == 0u) {
        /* Unused slots stay ZERO so "no curve" has exactly one canonical
         * representation. */
        for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
            if (out->curve[i].temp_c != 0u || out->curve[i].fan_pct != 0u) {
                out->valid = false;
                return false;
            }
        }
    } else {
        for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
            if (out->curve[i].fan_pct > 100u) {
                out->valid = false;
                return false;
            }
        }
    }
    out->valid = true;
    return true;
}

bool nx_tuning_snapshot_equal(const NxTuningSnapshot *a,
                              const NxTuningSnapshot *b)
{
    unsigned i;

    if (a == NULL || b == NULL) {
        return false;
    }
    /* Two invalid snapshots are NOT equal: absence is not agreement. */
    if (!a->valid || !b->valid) {
        return false;
    }
    if (a->version != b->version || a->size != b->size) {
        return false;
    }
    /*
     * EVERY field, explicitly. Deliberately not a memcmp: padding bytes are
     * not part of the configuration, and comparing them would make two
     * identical configurations differ for a reason that has no meaning.
     */
    if (a->frequency_mhz_x10 != b->frequency_mhz_x10) return false;
    if (a->voltage_mv        != b->voltage_mv)        return false;
    if (a->fan_mode          != b->fan_mode)          return false;
    if (a->fan_percent       != b->fan_percent)       return false;
    if (a->fan_min_percent   != b->fan_min_percent)   return false;
    if (a->fan_hysteresis_c  != b->fan_hysteresis_c)  return false;
    if (a->temp_target_c     != b->temp_target_c)     return false;
    if (a->overheat_mode     != b->overheat_mode)     return false;
    if (a->thermal_mode      != b->thermal_mode)      return false;
    if (a->curve_point_count != b->curve_point_count) return false;

    for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
        if (a->curve[i].temp_c  != b->curve[i].temp_c)  return false;
        if (a->curve[i].fan_pct != b->curve[i].fan_pct) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Canonical serialization                                             */
/* ------------------------------------------------------------------ */

static size_t put_u16(uint8_t *b, size_t o, uint16_t v)
{
    b[o]     = (uint8_t)(v >> 8);
    b[o + 1] = (uint8_t)(v & 0xFFu);
    return o + 2u;
}

size_t nx_tuning_snapshot_serialize(const NxTuningSnapshot *s,
                                    uint8_t *buf, size_t buflen)
{
    size_t   o = 0;
    unsigned i;

    if (s == NULL || buf == NULL || buflen < NX_TUNING_SNAPSHOT_BYTES) {
        return 0u;
    }
    memset(buf, 0, NX_TUNING_SNAPSHOT_BYTES);

    /*
     * FIXED FIELD ORDER, BIG-ENDIAN, no padding. Total 33 bytes:
     *   [0..3]   version              [17..18] temp_target_c
     *   [4]      valid                [19..20] overheat_mode
     *   [5..6]   frequency_mhz_x10    [21..22] thermal_mode
     *   [7..8]   voltage_mv           [23..24] curve_point_count
     *   [9..10]  fan_mode             [25..32] 4 x (temp_c, fan_pct)
     *   [11..12] fan_percent
     *   [13..14] fan_min_percent
     *   [15..16] fan_hysteresis_c
     */
    buf[o++] = (uint8_t)(s->version >> 24);
    buf[o++] = (uint8_t)(s->version >> 16);
    buf[o++] = (uint8_t)(s->version >> 8);
    buf[o++] = (uint8_t)(s->version & 0xFFu);
    buf[o++] = s->valid ? 1u : 0u;

    o = put_u16(buf, o, s->frequency_mhz_x10);
    o = put_u16(buf, o, s->voltage_mv);
    o = put_u16(buf, o, s->fan_mode);
    o = put_u16(buf, o, s->fan_percent);
    o = put_u16(buf, o, s->fan_min_percent);
    o = put_u16(buf, o, s->fan_hysteresis_c);
    o = put_u16(buf, o, s->temp_target_c);
    o = put_u16(buf, o, s->overheat_mode);
    o = put_u16(buf, o, s->thermal_mode);
    o = put_u16(buf, o, s->curve_point_count);

    for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
        buf[o++] = s->curve[i].temp_c;
        buf[o++] = s->curve[i].fan_pct;
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* Diagnostic digest — never the equality decision                     */
/* ------------------------------------------------------------------ */

static bool     s_digest_override;
static uint32_t s_digest_override_value;

void nx_tuning_snapshot_digest_override_for_test(bool enable, uint32_t value)
{
    s_digest_override       = enable;
    s_digest_override_value = value;
}

uint32_t nx_tuning_snapshot_digest(const NxTuningSnapshot *s)
{
    uint8_t  buf[NX_TUNING_SNAPSHOT_BYTES];
    uint32_t h = 2166136261u;
    size_t   n, i;

    if (s_digest_override) {
        return s_digest_override_value;
    }
    n = nx_tuning_snapshot_serialize(s, buf, sizeof(buf));
    if (n == 0u) {
        return 0u;
    }
    for (i = 0; i < n; i++) {
        h ^= buf[i];
        h *= 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Provider seam                                                       */
/* ------------------------------------------------------------------ */

static NxTuningSnapshotReader s_reader;

void nx_tuning_snapshot_provider_register(NxTuningSnapshotReader fn)
{
    s_reader = fn;
}

bool nx_tuning_snapshot_provider_present(void)
{
    return s_reader != NULL;
}

bool nx_tuning_snapshot_read(NxTuningSnapshot *out)
{
    if (out == NULL) {
        return false;
    }
    nx_tuning_snapshot_init(out);
    if (s_reader == NULL) {
        return false;   /* unregistered authority: unavailable, not agreeable */
    }
    if (!s_reader(out) || !out->valid) {
        /* A provider that failed leaves nothing usable behind — in
         * particular, no partially populated curve. */
        nx_tuning_snapshot_init(out);
        return false;
    }
    return true;
}
