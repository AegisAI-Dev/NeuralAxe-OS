/*
 * NeuralAxe — authoritative mutation observability (Gate W6.1).
 * See include/nx_mutation_counters.h for the contract and the semantic
 * boundary between configuration mutation and normal runtime activity.
 *
 * This component observes. It never logs (a pilot's logging belongs to the
 * diagnostics layer), never persists, creates no task, timer, queue or
 * endpoint, and holds nothing but nine counters and one function pointer.
 */

#include <string.h>

#include "sdkconfig.h"
#include "nx_mutation_counters.h"

const char *nx_mutation_counter_str(NxMutationCounterId id)
{
    switch (id) {
    case NX_MUT_FREQUENCY_CONFIG: return "MUT_FREQUENCY_CONFIG";
    case NX_MUT_VOLTAGE_CONFIG:   return "MUT_VOLTAGE_CONFIG";
    case NX_MUT_FAN_CONFIG:       return "MUT_FAN_CONFIG";
    case NX_MUT_THERMAL_CONFIG:   return "MUT_THERMAL_CONFIG";
    case NX_MUT_POOL_CONFIG:      return "MUT_POOL_CONFIG";
    case NX_MUT_PROTOCOL_CONFIG:  return "MUT_PROTOCOL_CONFIG";
    case NX_MUT_RESTART_REQUEST:  return "MUT_RESTART_REQUEST";
    case NX_MUT_OTA_FIRMWARE:     return "MUT_OTA_FIRMWARE";
    case NX_MUT_OTA_WEB:          return "MUT_OTA_WEB";
    case NX_MUT__COUNT:
    default:                      return "MUT_UNKNOWN";
    }
}

#ifdef CONFIG_NX_MUTATION_OBSERVABILITY

#include "freertos/FreeRTOS.h"

/*
 * One spinlock guards the whole set so a snapshot is mutually COHERENT: a
 * pilot comparing nine counters must never see some from before an increment
 * and some from after. The locked sections are a handful of integer
 * operations — no allocation, no I/O, nothing that can block.
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_counters[NX_MUT__COUNT];

void nx_mutation_counter_note(NxMutationCounterId id)
{
    if ((unsigned)id >= (unsigned)NX_MUT__COUNT) {
        return;   /* an unknown id is dropped, never mis-attributed */
    }
    portENTER_CRITICAL(&s_lock);
    if (s_counters[id] < UINT32_MAX) {
        s_counters[id]++;
    }
    /* At UINT32_MAX it STAYS there. Wrapping to zero would let a saturated
     * counter compare equal to a pilot baseline and read as "nothing
     * happened", which is the one answer it must never give. */
    portEXIT_CRITICAL(&s_lock);
}

bool nx_mutation_counters_snapshot(NxMutationSnapshot *out, size_t out_size)
{
    unsigned i;

    if (out == NULL || out_size != sizeof(*out)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->version = NX_MUTATION_SNAPSHOT_VERSION;
    out->size    = (uint32_t)sizeof(*out);

    portENTER_CRITICAL(&s_lock);
    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        out->counters[i] = s_counters[i];
    }
    portEXIT_CRITICAL(&s_lock);

    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        if (out->counters[i] == UINT32_MAX) {
            out->saturated_mask |= (uint32_t)1u << i;
        }
    }
    out->valid = true;
    return true;
}

void nx_mutation_counters_reset_for_test(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_counters, 0, sizeof(s_counters));
    portEXIT_CRITICAL(&s_lock);
}

void nx_mutation_counters_preset_for_test(NxMutationCounterId id, uint32_t v)
{
    if ((unsigned)id >= (unsigned)NX_MUT__COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_counters[id] = v;
    portEXIT_CRITICAL(&s_lock);
}

#else  /* observability not compiled in */

bool nx_mutation_counters_snapshot(NxMutationSnapshot *out, size_t out_size)
{
    /*
     * Without the flag there is no authority to read. The snapshot is zeroed
     * and INVALID, which the pilot must treat as unavailable — not as nine
     * reassuring zeros.
     */
    if (out != NULL && out_size == sizeof(*out)) {
        memset(out, 0, sizeof(*out));
        out->version = NX_MUTATION_SNAPSHOT_VERSION;
        out->size    = (uint32_t)sizeof(*out);
    }
    return false;
}

void nx_mutation_counters_reset_for_test(void) { }

void nx_mutation_counters_preset_for_test(NxMutationCounterId id, uint32_t v)
{
    (void)id;
    (void)v;
}

#endif /* CONFIG_NX_MUTATION_OBSERVABILITY */

/* ------------------------------------------------------------------ */
/* Pure snapshot comparison (available in every posture)               */
/* ------------------------------------------------------------------ */

bool nx_mutation_snapshot_saturated(const NxMutationSnapshot *s)
{
    return s != NULL && s->saturated_mask != 0u;
}

/* Shared precondition: two snapshots that may be compared at all. */
static bool snapshots_comparable(const NxMutationSnapshot *a,
                                 const NxMutationSnapshot *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    /* An invalid or mismatched snapshot proves nothing. */
    if (!a->valid || !b->valid) {
        return false;
    }
    return a->version == b->version && a->size == b->size &&
           a->version == NX_MUTATION_SNAPSHOT_VERSION;
}

bool nx_mutation_snapshot_unchanged(const NxMutationSnapshot *baseline,
                                    const NxMutationSnapshot *now,
                                    NxMutationCounterId *first)
{
    unsigned i;

    if (first != NULL) {
        *first = NX_MUT__COUNT;
    }
    if (!snapshots_comparable(baseline, now)) {
        return false;
    }
    /* Saturation has lost the history, so equality would be meaningless. */
    if (baseline->saturated_mask != 0u || now->saturated_mask != 0u) {
        if (first != NULL) {
            for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
                if (((baseline->saturated_mask | now->saturated_mask) >> i) & 1u) {
                    *first = (NxMutationCounterId)i;
                    break;
                }
            }
        }
        return false;
    }
    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        /* A regression is as disqualifying as an increase: it means a reset
         * or a different authority, so "nothing happened" is unprovable. */
        if (now->counters[i] != baseline->counters[i]) {
            if (first != NULL) {
                *first = (NxMutationCounterId)i;
            }
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Deltas                                                              */
/* ------------------------------------------------------------------ */

static uint32_t sat_add(uint32_t a, uint32_t b)
{
    return (a > UINT32_MAX - b) ? UINT32_MAX : (uint32_t)(a + b);
}

bool nx_mutation_delta_compute(const NxMutationSnapshot *baseline,
                               const NxMutationSnapshot *now,
                               NxMutationDelta *out)
{
    unsigned i;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (!snapshots_comparable(baseline, now)) {
        return false;
    }
    if (baseline->saturated_mask != 0u || now->saturated_mask != 0u) {
        out->saturated = true;
        return false;
    }
    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        if (now->counters[i] < baseline->counters[i]) {
            out->regressed = true;
            return false;   /* the deltas would be meaningless: report nothing */
        }
        out->counters[i] = now->counters[i] - baseline->counters[i];
    }

    out->hardware_total = sat_add(sat_add(out->counters[NX_MUT_FREQUENCY_CONFIG],
                                          out->counters[NX_MUT_VOLTAGE_CONFIG]),
                                  sat_add(out->counters[NX_MUT_FAN_CONFIG],
                                          out->counters[NX_MUT_THERMAL_CONFIG]));
    out->pool_total     = out->counters[NX_MUT_POOL_CONFIG];
    out->protocol_total = out->counters[NX_MUT_PROTOCOL_CONFIG];
    out->restart_total  = out->counters[NX_MUT_RESTART_REQUEST];
    out->ota_total      = sat_add(out->counters[NX_MUT_OTA_FIRMWARE],
                                  out->counters[NX_MUT_OTA_WEB]);
    out->valid = true;
    return true;
}

bool nx_mutation_delta_clean(const NxMutationDelta *d)
{
    if (d == NULL || !d->valid || d->regressed || d->saturated) {
        return false;
    }
    return d->hardware_total == 0u && d->pool_total == 0u &&
           d->protocol_total == 0u && d->restart_total == 0u &&
           d->ota_total == 0u;
}
