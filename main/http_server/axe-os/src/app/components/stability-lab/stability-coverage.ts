/**
 * NeuralAxe Stability Lab — sample-target and coverage semantics (Phase 2K.1).
 *
 * Resolves the ambiguous "121/120" presentation. The valid-sample count is
 * whatever genuinely arrived; it is NEVER trimmed or invalidated to force an
 * exact ratio against the target. The target is derived consistently from the
 * measurement duration and the configured cadence, and coverage is capped at
 * 100 %. Gaps in evidence are measured (max gap, total gap, median interval),
 * not hidden.
 *
 * All arithmetic is guarded: a zero/negative window or cadence yields zeros
 * rather than NaN/Infinity, and no division by zero can occur.
 */

import { SAMPLE_INTERVAL_MS, expectedSampleCount } from './stability-telemetry';

export const DEFAULT_CADENCE_MS = SAMPLE_INTERVAL_MS;

export interface CoverageInput {
  /** Monotonic times (ms) of the VALID samples, relative to measurement start. */
  sampleTimesMs: ReadonlyArray<number>;
  /** Configured measurement window (ms). */
  windowMs: number;
  /** Configured cadence between samples (ms). */
  cadenceMs?: number;
}

export interface CoverageStats {
  validSamples: number;
  expectedTarget: number;
  cadenceMs: number;
  /** 0–100, capped. Round for display; keep full precision here. */
  coveragePct: number;
  /** max(0, expectedTarget − validSamples). */
  missingSamples: number;
  /** Median spacing between consecutive valid samples; null when < 2 samples. */
  medianIntervalMs: number | null;
  /** Largest gap in evidence within the window (incl. leading/trailing). */
  maxGapMs: number;
  /** Window time not covered by valid evidence at the cadence. */
  totalGapMs: number;
}

const isFiniteNum = (v: unknown): v is number => typeof v === 'number' && isFinite(v);

function median(xs: number[]): number | null {
  if (!xs.length) return null;
  const sorted = [...xs].sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}

/** The target sample count for a window at a cadence (derived, not asserted). */
export function coverageTarget(windowMs: number, cadenceMs: number = DEFAULT_CADENCE_MS): number {
  return expectedSampleCount(windowMs, cadenceMs);
}

/**
 * Compute coverage statistics for a measurement window. Every valid sample
 * counts; the coverage ratio is capped at 100 % so 121 valid samples against a
 * target of 120 read as full coverage rather than an error.
 */
export function computeCoverage(input: CoverageInput): CoverageStats {
  const cadenceMs = isFiniteNum(input.cadenceMs) && input.cadenceMs! > 0 ? input.cadenceMs! : DEFAULT_CADENCE_MS;
  const windowMs = isFiniteNum(input.windowMs) && input.windowMs > 0 ? input.windowMs : 0;
  const expectedTarget = coverageTarget(windowMs, cadenceMs);

  // Keep only finite times, clamp into the window, and order them.
  const times = (input.sampleTimesMs ?? [])
    .filter(isFiniteNum)
    .map(t => Math.min(windowMs, Math.max(0, t)))
    .sort((a, b) => a - b);
  const validSamples = times.length;

  const coveragePct = expectedTarget > 0
    ? Math.min(100, (validSamples / expectedTarget) * 100)
    : 0;
  const missingSamples = Math.max(0, expectedTarget - validSamples);

  // Inter-sample intervals for the median cadence.
  const intervals: number[] = [];
  for (let i = 1; i < times.length; i++) intervals.push(times[i] - times[i - 1]);
  const medianIntervalMs = median(intervals);

  // Largest gap: leading (window start → first sample), each inter-sample
  // interval, and trailing (last sample → window end). With no samples the whole
  // window is one gap.
  let maxGapMs = 0;
  if (windowMs > 0) {
    if (validSamples === 0) {
      maxGapMs = windowMs;
    } else {
      maxGapMs = Math.max(times[0], windowMs - times[times.length - 1], ...intervals, 0);
    }
  }

  // Time not covered by valid evidence at the configured cadence.
  const coveredMs = Math.min(windowMs, validSamples * cadenceMs);
  const totalGapMs = Math.max(0, windowMs - coveredMs);

  return {
    validSamples,
    expectedTarget,
    cadenceMs,
    coveragePct,
    missingSamples,
    medianIntervalMs,
    maxGapMs,
    totalGapMs,
  };
}

/** Honest headline text — "121 valid samples · target 120" (never "121/120"). */
export function sampleTargetText(validSamples: number, expectedTarget: number): string {
  const v = isFiniteNum(validSamples) ? validSamples : 0;
  const t = isFiniteNum(expectedTarget) ? expectedTarget : 0;
  return `${v} valid sample${v === 1 ? '' : 's'} · target ${t}`;
}

/** Coverage as a display string, one decimal, capped: "8.3%" / "100%". */
export function coveragePctText(coveragePct: number | null): string {
  if (!isFiniteNum(coveragePct)) return '—';
  const capped = Math.min(100, Math.max(0, coveragePct));
  // Whole numbers show without a trailing ".0"; otherwise one decimal.
  return (Number.isInteger(capped) ? capped.toFixed(0) : capped.toFixed(1)) + '%';
}
