/**
 * NeuralAxe Stability Lab — Page Visibility contract (Phase 2K.1).
 *
 * The Lab explicitly tracks whether its page is foreground-visible, because a
 * hidden tab is the direct cause of throttled telemetry coverage. This is NOT a
 * cosmetic warning: the accumulated visibility evidence (interruption count,
 * total and longest hidden duration, and whether warm-up/measurement were
 * affected) is recorded on the result and export, and a low-coverage hidden run
 * can never be labelled Completed (that gate lives in the results module).
 *
 * Every duration is measured on the MONOTONIC clock (performance.now) so a
 * wall-clock change cannot corrupt a hidden-segment length. Becoming visible
 * again NEVER refreshes telemetry freshness and NEVER resets a timer — this
 * module only observes; the freshness/deadline logic is untouched.
 */

export type VisibilityPhase = 'warmup' | 'measure' | 'other';

export interface VisibilityAccumulator {
  /** Whether the page is currently hidden. */
  hidden: boolean;
  /** Monotonic start of the current hidden segment, or null when visible. */
  hiddenSinceMonoMs: number | null;
  /** Number of times the page went hidden during the session. */
  interruptions: number;
  /** Sum of COMPLETED hidden segments (ms). */
  totalHiddenMs: number;
  /** Longest COMPLETED hidden segment (ms). */
  longestHiddenMs: number;
  hiddenDuringWarmup: boolean;
  hiddenDuringMeasure: boolean;
}

export interface VisibilityStats {
  currentlyHidden: boolean;
  interruptions: number;
  /** Total hidden time INCLUDING any in-progress hidden segment. */
  totalHiddenMs: number;
  /** Longest hidden segment INCLUDING any in-progress hidden segment. */
  longestHiddenMs: number;
  hiddenDuringWarmup: boolean;
  hiddenDuringMeasure: boolean;
}

function blank(): VisibilityAccumulator {
  return {
    hidden: false,
    hiddenSinceMonoMs: null,
    interruptions: 0,
    totalHiddenMs: 0,
    longestHiddenMs: 0,
    hiddenDuringWarmup: false,
    hiddenDuringMeasure: false,
  };
}

/**
 * Start a fresh accumulator. If the page is already hidden when the session
 * begins, that is recorded honestly as an interruption in progress.
 */
export function initialVisibility(hidden: boolean, nowMonoMs: number): VisibilityAccumulator {
  const base = blank();
  return hidden ? applyVisibility(base, true, nowMonoMs, 'other') : base;
}

/**
 * Record the current visibility state at a monotonic instant, during a given
 * phase. Safe to call on every visibilitychange AND on every supervise tick with
 * the unchanged state: a same-state call never inflates the interruption count,
 * but it does keep the phase-affected flags honest while the page stays hidden.
 */
export function applyVisibility(
  acc: VisibilityAccumulator,
  hidden: boolean,
  nowMonoMs: number,
  phase: VisibilityPhase,
): VisibilityAccumulator {
  const next: VisibilityAccumulator = { ...acc };

  // A hidden page during warm-up or measurement marks that phase as affected,
  // whether this is the transition into hidden or a later tick still hidden.
  if (hidden) {
    if (phase === 'warmup') next.hiddenDuringWarmup = true;
    if (phase === 'measure') next.hiddenDuringMeasure = true;
  }

  if (hidden === acc.hidden) {
    return next; // no transition — phase flags above may have updated
  }

  if (hidden) {
    // visible → hidden: open a new hidden segment.
    next.hidden = true;
    next.hiddenSinceMonoMs = nowMonoMs;
    next.interruptions = acc.interruptions + 1;
  } else {
    // hidden → visible: close the segment and fold it into the totals.
    const seg = acc.hiddenSinceMonoMs !== null ? Math.max(0, nowMonoMs - acc.hiddenSinceMonoMs) : 0;
    next.hidden = false;
    next.hiddenSinceMonoMs = null;
    next.totalHiddenMs = acc.totalHiddenMs + seg;
    next.longestHiddenMs = Math.max(acc.longestHiddenMs, seg);
  }
  return next;
}

/**
 * Snapshot the visibility evidence at a monotonic instant, INCLUDING any
 * currently-open hidden segment (so a page that is still hidden reports its
 * running hidden duration, not a stale total).
 */
export function visibilityStats(acc: VisibilityAccumulator, nowMonoMs: number): VisibilityStats {
  const inProgress = acc.hidden && acc.hiddenSinceMonoMs !== null
    ? Math.max(0, nowMonoMs - acc.hiddenSinceMonoMs)
    : 0;
  return {
    currentlyHidden: acc.hidden,
    interruptions: acc.interruptions,
    totalHiddenMs: acc.totalHiddenMs + inProgress,
    longestHiddenMs: Math.max(acc.longestHiddenMs, inProgress),
    hiddenDuringWarmup: acc.hiddenDuringWarmup,
    hiddenDuringMeasure: acc.hiddenDuringMeasure,
  };
}

/** A concise operator-facing summary of the visibility evidence. */
export function visibilitySummary(stats: VisibilityStats): string {
  if (stats.interruptions === 0 && !stats.currentlyHidden) {
    return 'Page stayed visible for the whole session.';
  }
  const secs = Math.round(stats.totalHiddenMs / 1000);
  const longest = Math.round(stats.longestHiddenMs / 1000);
  return `Page hidden ${stats.interruptions} time(s), ${secs}s total (longest ${longest}s).`;
}
