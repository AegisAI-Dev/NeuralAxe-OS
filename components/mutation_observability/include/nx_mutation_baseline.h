#ifndef NX_MUTATION_BASELINE_H_
#define NX_MUTATION_BASELINE_H_

#include "nx_mutation_counters.h"
#include "nx_tuning_snapshot.h"

/*
 * NeuralAxe — PILOT MUTATION BASELINE (Phase 2W, Gate W6.1).
 *
 * A pilot proves "nothing changed SINCE a reference point". This module owns
 * that reference point and nothing else.
 *
 * A DELAY IS NOT A READINESS TEST. An earlier revision captured the baseline
 * on a 120-second monotonic delay alone. That is not sufficient: the device
 * can be up for two minutes and still have an unloaded configuration, an
 * unreadable counter set, a timed-session owner holding a lease, a held
 * protocol, or a mutation that already happened. The delay is now ONE
 * prerequisite among FOURTEEN, and the full predicate is explicit, pure and
 * separately testable (nx_mutation_baseline_evaluate).
 *
 * RAM ONLY. The baseline is never persisted, never restored and never
 * survives a reboot — which is deliberate. After a restart the counters start
 * at zero again and a stored baseline from the previous boot would compare
 * against a different, unrelated history. A REBOOT BEGINS A COMPLETELY NEW
 * LIFECYCLE: no state, no latch and no attempt count carries across it.
 *
 * CAPTURED ONCE, NEVER SILENTLY REPLACED. Once a baseline is READY, every
 * further capture attempt returns it unchanged. Nothing in this component can
 * overwrite it; only the explicit test hook clears it.
 *
 * NEVER RETRIED INTO SUCCESS. Two latches make "wait and try again until it
 * works" impossible:
 *   - a counted mutation makes `counters_zero` false FOREVER for this boot,
 *     because the counters only rise;
 *   - an observed violation or lost history latches permanently and blocks
 *     capture outright.
 * So a device that mutated before the window opened never gets a baseline,
 * and therefore never gets to claim it proved anything.
 *
 * FAIL CLOSED, PARTIAL NEVER STORED. A refusal writes NOTHING into the
 * baseline: not the counters, not the tuning, not the timestamp. There
 * is no state in which a half-formed baseline can be compared against.
 */

/* ------------------------------------------------------------------ */
/* Readiness predicate                                                 */
/* ------------------------------------------------------------------ */

/*
 * Facts this module cannot obtain itself, supplied by the integrator from the
 * subsystems that own them. Every field is a bounded bool; nothing here can
 * carry an identity or a private value.
 *
 * A zeroed struct is entirely UNREADY — every field must be positively
 * asserted, so a caller that forgets to fill one blocks capture rather than
 * passing it.
 */
typedef struct {
    bool config_loaded;          /* boot configuration fully loaded         */
    bool runtime_snapshot_valid; /* B5/B6 published snapshot obtained       */
    bool owner_none;             /* B5 lease owner is exactly NONE          */
    bool protocol_normal_source; /* approved normal-source protocol posture */
    bool session_posture_clean;  /* no timed-session mutation or recovery   */
    bool no_terminal_pending;    /* no retained terminal result             */
    bool host_task_valid;        /* the observing host task is the expected one */
} NxBaselinePrereq;

/*
 * Why a capture was blocked. Reported in a FIXED order so a given unready
 * device always yields the same code — a property tests can pin.
 */
typedef enum {
    NX_BASELINE_READY_OK = 0,
    NX_BASELINE_BLOCK_ALREADY_READY,      /* not a failure: already captured */
    NX_BASELINE_BLOCK_VIOLATION_LATCHED,
    NX_BASELINE_BLOCK_HISTORY_LOST,
    NX_BASELINE_BLOCK_SETTLING,           /* uptime below the settle floor   */
    NX_BASELINE_BLOCK_CONFIG_NOT_LOADED,
    NX_BASELINE_BLOCK_HOST_TASK_INVALID,
    NX_BASELINE_BLOCK_RUNTIME_UNAVAILABLE,
    NX_BASELINE_BLOCK_OWNER_PRESENT,
    NX_BASELINE_BLOCK_PROTOCOL_POSTURE,
    NX_BASELINE_BLOCK_SESSION_POSTURE,
    NX_BASELINE_BLOCK_TERMINAL_PENDING,
    NX_BASELINE_BLOCK_COUNTERS_UNREADABLE,
    NX_BASELINE_BLOCK_COUNTER_SATURATED,
    NX_BASELINE_BLOCK_COUNTER_NONZERO,    /* something already mutated       */
    NX_BASELINE_BLOCK_TUNING_UNREADABLE,
    NX_BASELINE_BLOCK__COUNT
} NxBaselineBlock;

const char *nx_mutation_baseline_block_str(NxBaselineBlock b);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    NX_MUT_BASELINE_NONE = 0,  /* never attempted                            */
    NX_MUT_BASELINE_PENDING,   /* attempted, prerequisites not yet met       */
    NX_MUT_BASELINE_READY,     /* captured; immutable for the rest of the boot */
    NX_MUT_BASELINE_REFUSED,   /* attempted, and permanently blocked         */
    NX_MUT_BASELINE__COUNT
} NxMutationBaselineState;

const char *nx_mutation_baseline_state_str(NxMutationBaselineState s);

/* How long the boot must have been running before a baseline is meaningful.
 * ONE prerequisite among FOURTEEN — never sufficient on its own. */
#define NX_MUTATION_BASELINE_SETTLE_US (120ull * 1000000ull)

typedef struct {
    NxMutationBaselineState state;
    NxBaselineBlock         last_block;    /* why the last attempt did not capture */
    uint64_t                captured_us;   /* monotonic; 0 unless READY      */
    uint32_t                attempts;
    uint32_t                refusals;      /* attempts blocked by a permanent latch */
    bool                    zero_at_capture; /* ALWAYS true when READY       */
    bool                    violation_latched;
    bool                    history_lost_latched;
    NxMutationSnapshot      snapshot;
    NxTuningSnapshot        tuning;   /* canonical; exact-compared */
} NxMutationBaseline;

/*
 * Evaluate the readiness predicate WITHOUT capturing. Pure apart from reading
 * the live counter snapshot and the registered tuning provider; changes
 * no state at all. Exposed so the predicate can be tested independently of
 * the capture it gates.
 */
NxBaselineBlock nx_mutation_baseline_evaluate(uint64_t now_us,
                                              const NxBaselinePrereq *pre);

/*
 * Attempt to establish the baseline. `now_us` is MONOTONIC (never a wall
 * clock). Captures only when nx_mutation_baseline_evaluate() returns
 * NX_BASELINE_READY_OK; otherwise stores NOTHING and records why.
 *
 * PENDING means "not yet, and it may still happen" (settling, configuration
 * not yet loaded, host task not yet valid, runtime snapshot not yet
 * published). REFUSED means "not any more, ever this boot" (a violation or
 * lost history is latched, or a counter has already left zero).
 */
NxMutationBaselineState nx_mutation_baseline_capture(uint64_t now_us,
                                                     const NxBaselinePrereq *pre);

/* Current state without attempting a capture. */
NxMutationBaselineState nx_mutation_baseline_state(void);

/* Read-only view. Never NULL; inspect `state` before trusting the contents. */
const NxMutationBaseline *nx_mutation_baseline_get(void);

/*
 * Latch a violation the integrator observed elsewhere (a B5 owner appearing,
 * an execution surface becoming reachable). Permanent for the boot: once
 * latched, no baseline can be captured, so a violated device can never be
 * re-baselined into looking healthy. Idempotent.
 */
void nx_mutation_baseline_note_violation(void);

/* ------------------------------------------------------------------ */
/* Comparison                                                          */
/* ------------------------------------------------------------------ */

/*
 * The full verdict of one comparison. Each answer is separate on purpose: a
 * caller that cannot distinguish "unreadable" from "changed" would report a
 * mutation that never happened, or hide one that did.
 */
typedef struct {
    bool baseline_ready;      /* a reference point exists at all            */
    bool violation_latched;   /* a violation was ALREADY observed this boot */
    bool counters_readable;   /* a fresh coherent snapshot was obtained     */
    bool counters_unchanged;  /* every class delta is exactly zero          */
    bool history_lost;        /* a counter regressed or saturated           */
    bool tuning_readable;     /* the canonical provider answered            */
    bool tuning_unchanged;    /* EXACT field-by-field canonical equality    */
    NxMutationDelta delta;    /* zeroed and invalid unless computable       */
} NxMutationComparison;

/*
 * Compare the live counters and tuning against the baseline. Does not modify
 * the baseline itself; it DOES latch the violation and lost-history flags,
 * because an observed violation must permanently prevent re-baselining.
 *
 * `out` is fully written on every path, so a caller can always report WHY. A
 * NULL `out` is REFUSED rather than answered: a bare verdict with no way to
 * report why it failed is the kind of result a pilot would misread.
 * ONCE A VIOLATION HAS BEEN OBSERVED THIS BOOT, THIS NEVER RETURNS TRUE
 * AGAIN. That is not redundant with the delta comparison: the baseline is
 * always all-zero, so a counter restored to zero would otherwise compare
 * equal and the pilot would report itself healthy again after having already
 * seen a mutation. The latch makes recovery-by-restoration impossible.
 *
 * Returns true only when no violation is latched, the baseline is READY, both
 * authorities answered, the history is intact, every counter delta is zero
 * and the canonical tuning snapshot is exactly equal.
 */
bool nx_mutation_baseline_compare(NxMutationComparison *out);

/* Test-only. Clears the baseline AND both latches so a suite can exercise the
 * lifecycle. This is the only way any state here is ever cleared; a reboot is
 * the only production equivalent. */
void nx_mutation_baseline_reset_for_test(void);

_Static_assert(NX_BASELINE_READY_OK == 0,
               "a zeroed block code must mean ready only when evaluated");
_Static_assert(NX_BASELINE_BLOCK__COUNT == 16,
               "W6.1 readiness predicate changed — review tokens and tests");

#endif /* NX_MUTATION_BASELINE_H_ */
