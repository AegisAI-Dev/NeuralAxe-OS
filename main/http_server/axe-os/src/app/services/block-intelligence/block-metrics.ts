/**
 * Network block metrics (Phase 2L, Stage 8) and device-session context
 * (Stage 9). Everything here is computed transparently from the currently
 * loaded block window — these are RECENT-WINDOW OBSERVATIONS, never predictions,
 * and they are never extrapolated into a guaranteed future rate.
 */

import { BlockSummary } from './block-intelligence.model';

const MS_PER_HOUR = 3_600_000;

export interface NetworkBlockMetrics {
  /** Number of blocks in the window. */
  blockCount: number;
  /** Interval statistics in ms; null when fewer than two blocks. */
  averageIntervalMs: number | null;
  medianIntervalMs: number | null;
  shortestIntervalMs: number | null;
  longestIntervalMs: number | null;
  /** Blocks per hour observed across the sample's span; null when indeterminate. */
  blocksPerHour: number | null;
  /** Span from the oldest to newest block timestamp in ms; null when < 2 blocks. */
  spanMs: number | null;
  /** Attribution / configured-pool tallies over the window. */
  activePoolBlocks: number;
  fallbackPoolBlocks: number;
  unknownAttribution: number;
  /** confirmed + strong as a fraction of the window in [0,1]; null when empty. */
  highConfidenceCoverage: number | null;
}

/**
 * Consecutive-block intervals in ms, ordered oldest→newest. Only strictly
 * positive intervals between valid timestamps are returned, so out-of-order or
 * duplicate timestamps cannot produce a negative or zero "interval".
 */
export function computeIntervals(blocks: readonly BlockSummary[]): number[] {
  const times = blocks
    .map(b => b.timestampMs)
    .filter((t): t is number => typeof t === 'number' && isFinite(t) && t > 0)
    .sort((a, b) => a - b);
  const intervals: number[] = [];
  for (let i = 1; i < times.length; i++) {
    const d = times[i] - times[i - 1];
    if (d > 0) {
      intervals.push(d);
    }
  }
  return intervals;
}

export function median(values: readonly number[]): number | null {
  if (values.length === 0) {
    return null;
  }
  const sorted = [...values].sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0 ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid];
}

/**
 * Compute the window metrics. Interval stats need ≥ 2 blocks; blocks-per-hour is
 * derived from the actual observed span (count−1 intervals over the span), not
 * assumed. Tallies count configured matches and attribution confidence honestly.
 */
export function computeBlockMetrics(blocks: readonly BlockSummary[]): NetworkBlockMetrics {
  const intervals = computeIntervals(blocks);
  const times = blocks.map(b => b.timestampMs).filter(t => typeof t === 'number' && isFinite(t) && t > 0);
  const spanMs = times.length >= 2 ? Math.max(...times) - Math.min(...times) : null;

  let activePoolBlocks = 0;
  let fallbackPoolBlocks = 0;
  let unknownAttribution = 0;
  let highConfidence = 0;
  for (const b of blocks) {
    if (b.configuredMatch === 'active' || b.configuredMatch === 'both') {
      activePoolBlocks++;
    }
    if (b.configuredMatch === 'fallback' || b.configuredMatch === 'both') {
      fallbackPoolBlocks++;
    }
    const c = b.attribution.confidence;
    if (c === 'unknown' || c === 'unattributed') {
      unknownAttribution++;
    }
    // High-confidence = provider-reported or a strong coinbase match (and the
    // reserved 'confirmed', though public providers never emit it).
    if (c === 'confirmed' || c === 'provider-reported' || c === 'strong') {
      highConfidence++;
    }
  }

  const averageIntervalMs = intervals.length > 0
    ? intervals.reduce((a, b) => a + b, 0) / intervals.length
    : null;

  // Blocks per hour across the observed span: (count − 1) blocks arrived over the
  // span. Falls back to the average interval when a positive span exists.
  let blocksPerHour: number | null = null;
  if (spanMs !== null && spanMs > 0) {
    blocksPerHour = ((blocks.length - 1) / spanMs) * MS_PER_HOUR;
  }

  return {
    blockCount: blocks.length,
    averageIntervalMs,
    medianIntervalMs: median(intervals),
    shortestIntervalMs: intervals.length > 0 ? Math.min(...intervals) : null,
    longestIntervalMs: intervals.length > 0 ? Math.max(...intervals) : null,
    blocksPerHour,
    spanMs,
    activePoolBlocks,
    fallbackPoolBlocks,
    unknownAttribution,
    highConfidenceCoverage: blocks.length > 0 ? highConfidence / blocks.length : null,
  };
}

// ---------------------------------------------------------------------------
// Device-session context (Stage 9)
// ---------------------------------------------------------------------------

export interface SessionObservation {
  /** Tip height captured when this page-session started; null if unknown. */
  startHeight: number | null;
  /** Newly observed NETWORK blocks since the session started (never negative). */
  networkBlocksObserved: number;
  /** Elapsed page-session duration in ms (never negative). */
  elapsedMs: number;
}

/**
 * Compute the honest session observation. This counts NETWORK blocks observed
 * while the page was open — it never claims this device participated in any of
 * them. A tip that went backwards (replaced tip) yields 0 new blocks rather than
 * a negative count.
 */
export function sessionObservation(
  startHeight: number | null,
  currentTipHeight: number | null,
  startMs: number,
  nowMs: number,
): SessionObservation {
  const elapsedMs = Math.max(0, nowMs - startMs);
  let networkBlocksObserved = 0;
  if (typeof startHeight === 'number' && typeof currentTipHeight === 'number'
    && isFinite(startHeight) && isFinite(currentTipHeight)) {
    networkBlocksObserved = Math.max(0, currentTipHeight - startHeight);
  }
  return { startHeight, networkBlocksObserved, elapsedMs };
}
