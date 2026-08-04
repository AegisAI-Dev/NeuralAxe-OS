/*
 * NeuralAxe — pilot mutation baseline (Gate W6.1).
 * See include/nx_mutation_baseline.h for the lifecycle contract.
 *
 * One RAM-only record and two permanent latches. No persistence, no task, no
 * timer, no allocation, and no way for anything outside the explicit test
 * hook to replace a baseline once it is READY.
 */

#include <string.h>

#include "sdkconfig.h"
#include "nx_mutation_baseline.h"

static NxMutationBaseline s_baseline;

const char *nx_mutation_baseline_state_str(NxMutationBaselineState s)
{
    switch (s) {
    case NX_MUT_BASELINE_NONE:    return "BASELINE_NONE";
    case NX_MUT_BASELINE_PENDING: return "BASELINE_PENDING";
    case NX_MUT_BASELINE_READY:   return "BASELINE_READY";
    case NX_MUT_BASELINE_REFUSED: return "BASELINE_REFUSED";
    case NX_MUT_BASELINE__COUNT:
    default:                      return "BASELINE_UNKNOWN";
    }
}

const char *nx_mutation_baseline_block_str(NxBaselineBlock b)
{
    switch (b) {
    case NX_BASELINE_READY_OK:                return "READY_OK";
    case NX_BASELINE_BLOCK_ALREADY_READY:     return "ALREADY_READY";
    case NX_BASELINE_BLOCK_VIOLATION_LATCHED: return "VIOLATION_LATCHED";
    case NX_BASELINE_BLOCK_HISTORY_LOST:      return "HISTORY_LOST";
    case NX_BASELINE_BLOCK_SETTLING:          return "SETTLING";
    case NX_BASELINE_BLOCK_CONFIG_NOT_LOADED: return "CONFIG_NOT_LOADED";
    case NX_BASELINE_BLOCK_HOST_TASK_INVALID: return "HOST_TASK_INVALID";
    case NX_BASELINE_BLOCK_RUNTIME_UNAVAILABLE: return "RUNTIME_UNAVAILABLE";
    case NX_BASELINE_BLOCK_OWNER_PRESENT:     return "OWNER_PRESENT";
    case NX_BASELINE_BLOCK_PROTOCOL_POSTURE:  return "PROTOCOL_POSTURE";
    case NX_BASELINE_BLOCK_SESSION_POSTURE:   return "SESSION_POSTURE";
    case NX_BASELINE_BLOCK_TERMINAL_PENDING:  return "TERMINAL_PENDING";
    case NX_BASELINE_BLOCK_COUNTERS_UNREADABLE: return "COUNTERS_UNREADABLE";
    case NX_BASELINE_BLOCK_COUNTER_SATURATED: return "COUNTER_SATURATED";
    case NX_BASELINE_BLOCK_COUNTER_NONZERO:   return "COUNTER_NONZERO";
    case NX_BASELINE_BLOCK_TUNING_UNREADABLE: return "TUNING_UNREADABLE";
    case NX_BASELINE_BLOCK__COUNT:
    default:                                  return "BLOCK_UNKNOWN";
    }
}

static bool snapshot_all_zero(const NxMutationSnapshot *s)
{
    unsigned i;

    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        if (s->counters[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* True for the blocks that can never clear for the rest of this boot. */
static bool block_is_permanent(NxBaselineBlock b)
{
    return b == NX_BASELINE_BLOCK_VIOLATION_LATCHED ||
           b == NX_BASELINE_BLOCK_HISTORY_LOST ||
           b == NX_BASELINE_BLOCK_COUNTER_SATURATED ||
           b == NX_BASELINE_BLOCK_COUNTER_NONZERO;
}

NxBaselineBlock nx_mutation_baseline_evaluate(uint64_t now_us,
                                              const NxBaselinePrereq *pre)
{
    NxMutationSnapshot  snap;
    NxTuningSnapshot    fp;

    if (s_baseline.state == NX_MUT_BASELINE_READY) {
        return NX_BASELINE_BLOCK_ALREADY_READY;
    }
    /*
     * The permanent latches come FIRST. A device that has already violated the
     * pilot posture, or whose counter history is unreconstructable, must never
     * be able to acquire a fresh reference point and start looking clean.
     */
    if (s_baseline.violation_latched) {
        return NX_BASELINE_BLOCK_VIOLATION_LATCHED;
    }
    if (s_baseline.history_lost_latched) {
        return NX_BASELINE_BLOCK_HISTORY_LOST;
    }

    /* A NULL prerequisite block is not "no objection"; it is no evidence. */
    if (pre == NULL) {
        return NX_BASELINE_BLOCK_CONFIG_NOT_LOADED;
    }

    if (now_us < NX_MUTATION_BASELINE_SETTLE_US) {
        return NX_BASELINE_BLOCK_SETTLING;
    }
    if (!pre->config_loaded) {
        return NX_BASELINE_BLOCK_CONFIG_NOT_LOADED;
    }
    if (!pre->host_task_valid) {
        return NX_BASELINE_BLOCK_HOST_TASK_INVALID;
    }
    if (!pre->runtime_snapshot_valid) {
        return NX_BASELINE_BLOCK_RUNTIME_UNAVAILABLE;
    }
    if (!pre->owner_none) {
        return NX_BASELINE_BLOCK_OWNER_PRESENT;
    }
    if (!pre->protocol_normal_source) {
        return NX_BASELINE_BLOCK_PROTOCOL_POSTURE;
    }
    if (!pre->session_posture_clean) {
        return NX_BASELINE_BLOCK_SESSION_POSTURE;
    }
    if (!pre->no_terminal_pending) {
        return NX_BASELINE_BLOCK_TERMINAL_PENDING;
    }

    if (!nx_mutation_counters_snapshot(&snap, sizeof(snap)) || !snap.valid) {
        return NX_BASELINE_BLOCK_COUNTERS_UNREADABLE;
    }
    if (nx_mutation_snapshot_saturated(&snap)) {
        return NX_BASELINE_BLOCK_COUNTER_SATURATED;
    }
    /*
     * EVERY counter must still be zero. Counters only rise, so this is what
     * makes "wait and retry until it succeeds" impossible: a configuration
     * mutation before the window opened blocks the baseline permanently
     * rather than being absorbed into a later reference point.
     */
    if (!snapshot_all_zero(&snap)) {
        return NX_BASELINE_BLOCK_COUNTER_NONZERO;
    }
    if (!nx_tuning_snapshot_read(&fp)) {
        return NX_BASELINE_BLOCK_TUNING_UNREADABLE;
    }
    return NX_BASELINE_READY_OK;
}

NxMutationBaselineState nx_mutation_baseline_capture(uint64_t now_us,
                                                     const NxBaselinePrereq *pre)
{
    NxMutationSnapshot  snap;
    NxTuningSnapshot    fp;
    NxBaselineBlock     block;

    /* Immutable once established. */
    if (s_baseline.state == NX_MUT_BASELINE_READY) {
        s_baseline.last_block = NX_BASELINE_BLOCK_ALREADY_READY;
        return NX_MUT_BASELINE_READY;
    }

    if (s_baseline.attempts < UINT32_MAX) {
        s_baseline.attempts++;
    }

    block = nx_mutation_baseline_evaluate(now_us, pre);
    s_baseline.last_block = block;

    if (block != NX_BASELINE_READY_OK) {
        /* Store NOTHING: no counters, no tuning, no timestamp. There is
         * no state in which a half-formed baseline can be compared against. */
        if (block_is_permanent(block)) {
            if (s_baseline.refusals < UINT32_MAX) {
                s_baseline.refusals++;
            }
            s_baseline.state = NX_MUT_BASELINE_REFUSED;
            return NX_MUT_BASELINE_REFUSED;
        }
        s_baseline.state = NX_MUT_BASELINE_PENDING;
        return NX_MUT_BASELINE_PENDING;
    }

    /*
     * Re-read both authorities for the value actually stored. evaluate()
     * proved they are readable and clean; these are the bytes recorded.
     */
    if (!nx_mutation_counters_snapshot(&snap, sizeof(snap)) || !snap.valid ||
        !snapshot_all_zero(&snap) || !nx_tuning_snapshot_read(&fp)) {
        /* Raced with a mutation between predicate and read: refuse, store
         * nothing, and let the permanent counter rule take over next time. */
        if (s_baseline.refusals < UINT32_MAX) {
            s_baseline.refusals++;
        }
        s_baseline.last_block = NX_BASELINE_BLOCK_COUNTER_NONZERO;
        s_baseline.state      = NX_MUT_BASELINE_REFUSED;
        return NX_MUT_BASELINE_REFUSED;
    }

    s_baseline.snapshot        = snap;
    s_baseline.tuning          = fp;
    s_baseline.captured_us     = now_us;
    s_baseline.zero_at_capture = true;   /* an invariant, not a variable */
    s_baseline.state           = NX_MUT_BASELINE_READY;
    return NX_MUT_BASELINE_READY;
}

NxMutationBaselineState nx_mutation_baseline_state(void)
{
    return s_baseline.state;
}

const NxMutationBaseline *nx_mutation_baseline_get(void)
{
    return &s_baseline;
}

void nx_mutation_baseline_note_violation(void)
{
    s_baseline.violation_latched = true;
}

bool nx_mutation_baseline_compare(NxMutationComparison *out)
{
    NxMutationSnapshot  now;
    NxTuningSnapshot    fp;
    bool                healthy;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (s_baseline.state != NX_MUT_BASELINE_READY) {
        return false;   /* nothing to compare against */
    }
    out->baseline_ready    = true;
    out->violation_latched = s_baseline.violation_latched;

    if (nx_mutation_counters_snapshot(&now, sizeof(now))) {
        out->counters_readable = true;
        if (nx_mutation_delta_compute(&s_baseline.snapshot, &now, &out->delta)) {
            out->counters_unchanged = nx_mutation_delta_clean(&out->delta);
        } else {
            /* Both reasons mean the same thing to a pilot: what happened
             * between the two observations cannot be reconstructed. */
            out->history_lost = out->delta.regressed || out->delta.saturated;
        }
    }

    /* The tuning is read independently of the counters: a configuration
     * that changed without passing the counted boundary must still be caught,
     * and a counted change with unchanged tuning must still fail. */
    if (nx_tuning_snapshot_read(&fp)) {
        out->tuning_readable  = true;
        out->tuning_unchanged = nx_tuning_snapshot_equal(&s_baseline.tuning, &fp);
    }

    healthy = out->counters_readable && out->counters_unchanged &&
              !out->history_lost && out->tuning_readable && out->tuning_unchanged;

    /*
     * A violation already seen this boot is FINAL. The baseline is all-zero by
     * construction, so a counter restored to zero would otherwise compare
     * equal and the pilot would announce itself healthy again after having
     * observed a mutation. Recovery-by-restoration is not a thing.
     */
    if (out->violation_latched) {
        healthy = false;
    }

    /*
     * Latch what was observed. A violated or unreconstructable device must
     * never be able to acquire a fresh baseline later — which is exactly what
     * "wait, reset, look clean" would otherwise allow.
     */
    if (out->history_lost) {
        s_baseline.history_lost_latched = true;
    }
    if (!healthy) {
        s_baseline.violation_latched = true;
    }
    return healthy;
}

void nx_mutation_baseline_reset_for_test(void)
{
    memset(&s_baseline, 0, sizeof(s_baseline));
}
