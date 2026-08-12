/*
 * NeuralAxe — coherent telemetry safety snapshot (Gate W6.3T-B).
 *
 * One RAM-only latest-state store, one portMUX, three bounded operations.
 *
 * NOTHING inside the critical section performs I/O, logs, allocates, touches
 * NVS, blocks or evaluates policy. Each protected region is a fixed number of
 * scalar stores or one bounded struct copy, which is why this can be published
 * from the middle of the fan and power control cycles without perturbing them.
 *
 * `volatile` appears nowhere: mutual exclusion plus the barriers implied by
 * portENTER/EXIT_CRITICAL are the synchronisation, and volatile would neither
 * order these accesses nor make a multi-field update atomic.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"

#include "nx_telemetry_safety.h"

/*
 * THE store and its lock. One spinlock guards every field, so there is exactly
 * one lock in this module and therefore no lock ordering to get wrong: no
 * function here acquires a second lock, and neither producer holds any other
 * lock when it calls in (audited: power_management_task and fan_controller_task
 * take no mutex or semaphore on their publication paths).
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static NxTelemetrySafetySnapshot s_snap;

/* Saturating advance. A generation that reached the ceiling stays there rather
 * than wrapping, so a stale snapshot can never impersonate a fresh one and a
 * long-running device can never re-enter "never published". */
static uint32_t bump_generation(uint32_t g)
{
    return (g < NX_TELEMETRY_GENERATION_MAX) ? (g + 1u) : NX_TELEMETRY_GENERATION_MAX;
}

void nx_telemetry_safety_publish_power(const NxTelemetryPowerFacts *f)
{
    if (f == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_snap.asic_temp_dc             = f->asic_temp_dc;
    s_snap.asic_temp_valid          = f->asic_temp_valid;
    s_snap.vrm_temp_dc              = f->vrm_temp_dc;
    s_snap.vrm_read_ok              = f->vrm_read_ok;
    s_snap.vrm_expected             = f->vrm_expected;
    s_snap.emergency_thermal_active = f->emergency_thermal_active;
    /* LAST: the generation is the publication's commit point, so a reader that
     * observes generation N has necessarily observed all of publication N. */
    s_snap.power_generation         = bump_generation(s_snap.power_generation);
    portEXIT_CRITICAL(&s_lock);
}

void nx_telemetry_safety_publish_fan(const NxTelemetryFanFacts *f)
{
    if (f == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_snap.fan_rpm           = f->fan_rpm;
    s_snap.fan_expected      = f->fan_expected;
    s_snap.fan_control_fault = f->fan_control_fault;
    s_snap.fan_generation    = bump_generation(s_snap.fan_generation);
    portEXIT_CRITICAL(&s_lock);
}

bool nx_telemetry_safety_read(NxTelemetrySafetySnapshot *out)
{
    if (out == NULL) {
        return false;
    }
    /* ONE bounded copy of a fixed-size struct. The caller owns the result and
     * holds no lock after this returns. */
    portENTER_CRITICAL(&s_lock);
    *out = s_snap;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool nx_telemetry_safety_power_published(const NxTelemetrySafetySnapshot *s)
{
    return (s != NULL) && (s->power_generation != 0u);
}

bool nx_telemetry_safety_fan_published(const NxTelemetrySafetySnapshot *s)
{
    return (s != NULL) && (s->fan_generation != 0u);
}

void nx_telemetry_safety_reset(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_snap, 0, sizeof(s_snap));
    portEXIT_CRITICAL(&s_lock);
}
