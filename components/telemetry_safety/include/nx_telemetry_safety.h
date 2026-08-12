#ifndef NX_TELEMETRY_SAFETY_H_
#define NX_TELEMETRY_SAFETY_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * NeuralAxe — COHERENT TELEMETRY SAFETY SNAPSHOT (Phase 2W, Gate W6.3T-B).
 *
 * WHY THIS EXISTS. The safety-relevant telemetry a policy layer needs is
 * written by TWO independent tasks with no shared synchronisation:
 *
 *   power_management_task  -> chip_temp_avg, vr_temp, VRM read validity
 *   fan_controller_task    -> fan_rpm, fan-control fault
 *
 * A consumer reading those fields one by one out of PowerManagementModule can
 * observe a set that never existed as a whole: a temperature from one cycle
 * beside an RPM from another, or a partially written pair. Gate W6.3.1 must
 * build ONE TuningPolicyInput from them, and a safety decision assembled from
 * a torn read is not a decision anyone can defend.
 *
 * This module is the smallest fix: each producer publishes ITS OWN facts as a
 * single bounded update, and any reader takes one bounded copy. Nothing here
 * samples hardware, and nothing here decides anything.
 *
 * WHAT IT IS NOT. It holds FACTS, never decisions. There is no profile, no
 * recommendation, no actionable flag, no mining policy, no weather state, no
 * persistence and no wall clock. It performs no I2C/SMBus transaction, adds no
 * task, no timer and no polling loop — it consumes results the two existing
 * producers have already computed.
 *
 * COHERENCE, STATED PRECISELY. This is the distinction that matters most:
 *
 *   GUARANTEED: a reader receives one internally consistent struct copy; no
 *   field is ever observed half-written; the power-owned facts in a snapshot
 *   all come from ONE power publication; the fan-owned facts all come from ONE
 *   fan publication; the two generation counters say exactly which publication
 *   each side came from.
 *
 *   NOT CLAIMED, AND NEVER TO BE INFERRED: that the ASIC, VRM and fan sensors
 *   were physically sampled at the same instant. They are not — the two
 *   producers run on independent ~100 ms cycles and never share a measurement
 *   instant. `power_generation == fan_generation` is a coincidence of counting,
 *   NOT evidence of simultaneity, and a consumer must never read it that way.
 *   The snapshot removes torn reads; it does not manufacture a common clock.
 */

/* ------------------------------------------------------------------ */
/* The bounded snapshot                                                */
/* ------------------------------------------------------------------ */

/*
 * Every member is a bounded scalar. There is no pointer and no character
 * array, so a private configuration value, hostname or credential cannot be
 * carried through this type even by mistake.
 *
 * A generation of ZERO means that producer has never published. That is the
 * load-bearing property of this whole type: TUNING_SENSOR_OK is the zero of
 * the W1 sensor enum, so an unpublished fact must never be mistakable for a
 * confirmed-healthy one. A zeroed snapshot reports `fan_expected = false` and
 * `fan_generation = 0`, and the committed W1 classifier turns that into
 * MISSING — never OK.
 */
typedef struct {
    /* ---- power-owned (power_management_task, ~100 ms) ---- */
    int32_t  asic_temp_dc;              /* deci-C; meaningless unless valid  */
    bool     asic_temp_valid;           /* false for the -1 / not-init case  */
    int32_t  vrm_temp_dc;               /* deci-C; may be a CACHED value     */
    bool     vrm_read_ok;               /* the acquisition freshness fact    */
    bool     vrm_expected;              /* declared TPS546 presence          */
    bool     emergency_thermal_active;  /* authoritative overheat state      */
    uint32_t power_generation;          /* 0 = never published               */

    /* ---- fan-owned (fan_controller_task, ~100 ms) ---- */
    uint16_t fan_rpm;                   /* 0 is the "no tach signal" answer  */
    bool     fan_expected;              /* declared EMC2101/2103/2302        */
    bool     fan_control_fault;         /* PWM write failure (hardware_fault) */
    uint32_t fan_generation;            /* 0 = never published               */
} NxTelemetrySafetySnapshot;

/* The power producer's contribution, published as one unit. */
typedef struct {
    int32_t asic_temp_dc;
    bool    asic_temp_valid;
    int32_t vrm_temp_dc;
    bool    vrm_read_ok;
    bool    vrm_expected;
    bool    emergency_thermal_active;
} NxTelemetryPowerFacts;

/* The fan producer's contribution, published as one unit. */
typedef struct {
    uint16_t fan_rpm;
    bool     fan_expected;
    bool     fan_control_fault;
} NxTelemetryFanFacts;

/*
 * Generation ceiling. A counter that reached it STOPS THERE rather than
 * wrapping to zero, because wrapping would let a stale snapshot impersonate a
 * fresh one and would recreate "never published" out of a long-running device.
 *
 * TERMINAL BEHAVIOUR, STATED: once saturated, the counter no longer
 * distinguishes successive publications, so change detection by generation is
 * no longer available. Every validity flag stays live and correct, so the
 * snapshot remains safe to consume — it simply stops answering "is this
 * newer?". At the observed ~100 ms cadence this is ~13.6 years of continuous
 * uptime, so it is a defined terminal state rather than an expected one.
 */
#define NX_TELEMETRY_GENERATION_MAX 0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* Producer API — called ONLY from the owning task                     */
/* ------------------------------------------------------------------ */

/*
 * Publish the power-owned facts for one cycle. Called from
 * power_management_task after its temperature acquisition for that cycle is
 * complete. The generation advances only after every field has been stored,
 * so a reader that sees generation N sees all of publication N.
 *
 * Bounded, non-blocking, allocation-free. NULL is a no-op.
 */
void nx_telemetry_safety_publish_power(const NxTelemetryPowerFacts *f);

/*
 * Publish the fan-owned facts for one cycle. Called from fan_controller_task
 * after its tach read for that cycle. Same guarantees as above.
 */
void nx_telemetry_safety_publish_fan(const NxTelemetryFanFacts *f);

/* ------------------------------------------------------------------ */
/* Consumer API                                                        */
/* ------------------------------------------------------------------ */

/*
 * Take ONE coherent copy. Returns false only for a NULL argument; a snapshot
 * from a device where neither producer has run yet is a legitimate result and
 * reports itself through the two zero generations rather than through failure.
 *
 * The caller owns the copy. No pointer into mutable global storage is returned
 * and no lock lifetime escapes this call, so a consumer cannot accidentally
 * hold the producers off while it thinks.
 */
bool nx_telemetry_safety_read(NxTelemetrySafetySnapshot *out);

/* Has each side published at least once? Pure, NULL-safe. */
bool nx_telemetry_safety_power_published(const NxTelemetrySafetySnapshot *s);
bool nx_telemetry_safety_fan_published(const NxTelemetrySafetySnapshot *s);

/*
 * Return the store to its unpublished state. Test-only in practice: nothing in
 * production calls it, and a reboot achieves the same thing by construction
 * (the store is RAM-only and is never persisted).
 */
void nx_telemetry_safety_reset(void);

#endif /* NX_TELEMETRY_SAFETY_H_ */
