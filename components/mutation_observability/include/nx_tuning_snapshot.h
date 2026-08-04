#ifndef NX_TUNING_SNAPSHOT_H_
#define NX_TUNING_SNAPSHOT_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * NeuralAxe — CANONICAL TUNING SNAPSHOT (Phase 2W, Gate W6.1).
 *
 * The pilot's tuning-equality decision. An earlier revision reduced the whole
 * configuration to one 32-bit FNV-1a digest and compared digests. That was
 * wrong as a SAFETY decision: a 32-bit non-cryptographic hash cannot be shown
 * to give every distinct valid configuration a distinct value, and tests that
 * change one field at a time do not eliminate collisions. A collision would
 * have let a real tuning change compare equal to the baseline.
 *
 * The authority is now an EXACT comparison of canonical values:
 *
 *   - every field is fixed-width and explicitly versioned;
 *   - the fan curve is the committed PARSED model, not its text;
 *   - the thermal mode is a stable enum, not its text;
 *   - equality compares every field INDIVIDUALLY. It never memcmp()s the
 *     struct, because padding bytes are not part of the value and could
 *     otherwise make two identical configurations compare unequal (or, with a
 *     sloppier layout, hide a difference).
 *
 * A digest remains available for BOUNDED DIAGNOSTICS ONLY. It is computed over
 * the canonical serialization, never stored in the snapshot, never consulted
 * by the equality decision, and never presented as collision-free.
 *
 * NO POINTER, NO ALLOCATION. Nothing here owns memory, so no temporary parse
 * buffer can survive into the baseline.
 */

#define NX_TUNING_SNAPSHOT_VERSION 1u

/*
 * Points in the canonical curve. Must equal the committed THERMAL_CURVE_POINTS.
 * This component deliberately does NOT include thermal_control.h — the single
 * translation unit that knows both models asserts the equality at compile time
 * (see main/nx_mutation_adapter.c), so drift is a build failure rather than a
 * silent truncation.
 */
#define NX_TUNING_CURVE_POINTS 4

/* Stable, storage-independent thermal mode. The adapter maps the committed
 * ThermalControlMode onto this EXPLICITLY rather than by numeric cast, so a
 * change to the committed enum's values cannot silently remap a baseline. */
typedef enum {
    NX_TUNING_MODE_UNSET = 0,   /* factory default: no mode string stored   */
    NX_TUNING_MODE_TARGET,
    NX_TUNING_MODE_CURVE,
    NX_TUNING_MODE_MANUAL,
    NX_TUNING_MODE__COUNT
} NxTuningThermalMode;

const char *nx_tuning_thermal_mode_str(NxTuningThermalMode m);

/* One canonical curve point, in the committed model's own units: whole
 * degrees Celsius and whole percent. */
typedef struct {
    uint8_t temp_c;
    uint8_t fan_pct;
} NxTuningCurvePoint;

/*
 * The canonical configuration. Fixed-width scalars only: no string, no
 * pointer, no identity and no private value can be represented here.
 *
 * `curve_point_count` is 0 when no curve is configured, otherwise exactly
 * NX_TUNING_CURVE_POINTS. Unused point slots are ZERO, so two snapshots with
 * no curve are byte-equal in the curve region as well as field-equal.
 */
typedef struct {
    uint32_t version;              /* NX_TUNING_SNAPSHOT_VERSION            */
    uint32_t size;                 /* sizeof(NxTuningSnapshot)              */
    bool     valid;                /* false => not a configuration at all   */

    uint16_t frequency_mhz_x10;    /* configured, tenths of a MHz           */
    uint16_t voltage_mv;           /* configured, millivolts                */
    uint16_t fan_mode;             /* 0 manual, 1 automatic                 */
    uint16_t fan_percent;          /* configured manual percent, NOT live   */
    uint16_t fan_min_percent;
    uint16_t fan_hysteresis_c;
    uint16_t temp_target_c;
    uint16_t overheat_mode;        /* 0 or 1                                */
    uint16_t thermal_mode;         /* NxTuningThermalMode                   */
    uint16_t curve_point_count;    /* 0 or NX_TUNING_CURVE_POINTS           */
    NxTuningCurvePoint curve[NX_TUNING_CURVE_POINTS];
} NxTuningSnapshot;

/* Bounded ranges a readable configuration must satisfy. */
#define NX_TUNING_FREQ_X10_MAX  20000u   /* 2000.0 MHz                       */
#define NX_TUNING_VOLTAGE_MAX   2000u    /* mV                               */
#define NX_TUNING_TEMP_MAX      150u     /* degrees C                        */

/* Zero to the invalid posture. Every field, including the curve, is cleared. */
void nx_tuning_snapshot_init(NxTuningSnapshot *out);

/*
 * Validate the already-populated fields and mark the snapshot usable.
 * Deterministic and total; an out-of-range field, an unknown mode or an
 * inconsistent curve point count leaves the snapshot INVALID, so a
 * configuration that could not be read properly can never masquerade as a
 * stable one.
 */
bool nx_tuning_snapshot_finalize(NxTuningSnapshot *out);

/*
 * THE SAFETY AUTHORITY. Exact field-by-field equality.
 *
 * Never a memcmp: padding bytes are not part of the value. Two invalid
 * snapshots are NEVER equal — absence is not agreement.
 */
bool nx_tuning_snapshot_equal(const NxTuningSnapshot *a,
                              const NxTuningSnapshot *b);

/* ------------------------------------------------------------------ */
/* Canonical serialization                                             */
/* ------------------------------------------------------------------ */

/*
 * Exact byte length of the canonical form: nine u16 fields, a u32 version, a
 * validity byte, a u16 point count and NX_TUNING_CURVE_POINTS point pairs.
 */
#define NX_TUNING_SNAPSHOT_BYTES (4u + 1u + (2u * 10u) + (2u * NX_TUNING_CURVE_POINTS))

/*
 * Write the canonical, BIG-ENDIAN, padding-free serialization. Field order is
 * fixed and documented in the implementation. Returns the number of bytes
 * written, or 0 on any failure (NULL, buffer too small).
 *
 * This is what the diagnostic digest is computed over; it is NOT the equality
 * authority, though comparing the complete serialized bytes AND length is an
 * equivalent decision and is asserted against the field comparison in tests.
 */
size_t nx_tuning_snapshot_serialize(const NxTuningSnapshot *s,
                                    uint8_t *buf, size_t buflen);

/*
 * DIAGNOSTIC ONLY — a 32-bit FNV-1a over the canonical serialization.
 *
 * NOT collision-free and NEVER the equality decision. It exists so a bounded
 * log line can carry a short stable token for a configuration. A matching
 * digest can never override a snapshot mismatch, because
 * nx_tuning_snapshot_equal() does not consult it.
 */
uint32_t nx_tuning_snapshot_digest(const NxTuningSnapshot *s);

/*
 * Test-only: force nx_tuning_snapshot_digest() to return a fixed value, so a
 * digest COLLISION can be simulated and the equality decision shown to be
 * unaffected by it. Never called from production code.
 */
void nx_tuning_snapshot_digest_override_for_test(bool enable, uint32_t value);

/* ------------------------------------------------------------------ */
/* Provider seam                                                       */
/* ------------------------------------------------------------------ */

/*
 * The tuning values live in the application's configuration store, and the fan
 * curve must be parsed by the subsystem that owns its grammar. This component
 * deliberately depends on neither. The application registers a READER at boot;
 * the pilot calls nx_tuning_snapshot_read().
 *
 * With no provider registered the read FAILS rather than returning zeros — an
 * unregistered authority is unavailable, not agreeable.
 */
typedef bool (*NxTuningSnapshotReader)(NxTuningSnapshot *out);

void nx_tuning_snapshot_provider_register(NxTuningSnapshotReader fn);

/* True only when a provider is registered AND produced a valid snapshot. */
bool nx_tuning_snapshot_read(NxTuningSnapshot *out);

/* True when a provider is currently registered. */
bool nx_tuning_snapshot_provider_present(void);

_Static_assert(NX_TUNING_CURVE_POINTS == 4,
               "canonical curve width changed — review the serialization, its "
               "byte length and the adapter's compile-time cross-check");
_Static_assert(NX_TUNING_SNAPSHOT_BYTES == 33u,
               "canonical serialization length changed — review the tests that "
               "pin it");

#endif /* NX_TUNING_SNAPSHOT_H_ */
