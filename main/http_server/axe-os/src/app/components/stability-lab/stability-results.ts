/**
 * NeuralAxe Stability Lab — result model (Phase 2K).
 *
 * Pure aggregation over a profile's MEASUREMENT samples (warm-up samples are
 * deliberately excluded). Every metric is transparent and independently
 * testable — there is NO single opaque "stability score" in this phase. A run
 * is described by named aggregates plus honest badges (Completed / Partial /
 * Aborted: <reason> / Insufficient samples), and a short qualified statement
 * ("No configured stop condition triggered during this session") — never an
 * unqualified "Stable".
 */

import { LabSample } from './stability-telemetry';
import { accumulateShareDeltas } from './stability-telemetry';
import { computeCoverage, DEFAULT_CADENCE_MS, coveragePctText } from './stability-coverage';
import { VisibilityStats } from './stability-visibility';

/** Minimum valid measurement samples for a run to be scoreable. */
export const MIN_VALID_SAMPLES = 3;
/**
 * Minimum telemetry coverage for a full-window run to be Completed rather than
 * Partial. Phase 2K.1 raises this to a conservative 90 % (Phase 2K used 60 %):
 * a run that only captured 8.3 % of its intended evidence must be reported as
 * Partial with an explicit reason, never silently Completed.
 */
export const MIN_COVERAGE = 0.9;
/** Fan duty at/above which a sample counts toward saturation duration. */
const FAN_SATURATION_PCT = 95;
const REJECT_MIN_TOTAL = 100;
const REJECT_MIN_REJECTED = 3;

export type RunStatus = 'completed' | 'partial' | 'aborted' | 'failed' | 'insufficient';

export interface ProfileRun {
  profileId: string;
  profileName: string;
  thermalControlMode: string;
  warmupSamples: LabSample[];
  measureSamples: LabSample[];
  requestedMeasureMs: number;
  measuredMeasureMs: number;
  /** Configured sample cadence (ms); defaults to the standard cadence. */
  cadenceMs?: number;
  /** Session-relative time (ms) at which the measurement window began. */
  measureStartTMs?: number;
  /** Visibility evidence captured over the measurement window. */
  visibility?: VisibilityStats;
  restartOccurred: boolean;
  /** True when a share counter reset (reboot) was seen during measurement. */
  countersReset: boolean;
  /** Terminal disposition from the engine: did the window run to the end? */
  ranFullWindow: boolean;
  aborted: boolean;
  failed: boolean;
  abortReason?: string;
}

export type BadgeSeverity = 'ok' | 'info' | 'warn' | 'danger';

export interface ResultBadge {
  kind: string;
  label: string;
  severity: BadgeSeverity;
}

export interface ProfileResult {
  profileId: string;
  profileName: string;
  status: RunStatus;
  /** Exact human reason for the status (never a bare "Partial"). */
  statusReason: string;
  thermalControlMode: string;
  requestedMeasureMs: number;
  measuredMeasureMs: number;
  validSamples: number;
  missingSamples: number;
  expectedSamples: number;
  coveragePct: number | null;
  /** Configured cadence (ms) and observed spacing/gap evidence. */
  cadenceMs: number;
  medianIntervalMs: number | null;
  maxGapMs: number;
  totalGapMs: number;
  /** Visibility evidence over the measurement window. */
  visInterruptions: number;
  totalHiddenMs: number;
  longestHiddenMs: number;
  hiddenDuringWarmup: boolean;
  hiddenDuringMeasure: boolean;
  restartOccurred: boolean;
  countersReset: boolean;
  abortReason: string | null;

  avgHashrate: number | null;
  medianHashrate: number | null;
  hashrateVariabilityPct: number | null;
  avgPower: number | null;
  avgEfficiency: number | null;
  peakAsicTemp: number | null;
  avgAsicTemp: number | null;
  peakVrmTemp: number | null;
  avgVrmTemp: number | null;
  avgRequestedFan: number | null;
  avgAppliedFan: number | null;
  fanSaturationMs: number;
  avgErrorRate: number | null;
  acceptedShareDelta: number | null;
  rejectedShareDelta: number | null;
  rejectRatePct: number | null;
  poolLatencyAvg: number | null;
  poolLatencyPeak: number | null;

  badges: ResultBadge[];
}

// ---- statistics helpers (pure) ----

function pick(samples: ReadonlyArray<LabSample>, selector: (s: LabSample) => number | null): number[] {
  const out: number[] = [];
  for (const s of samples) {
    const v = selector(s);
    if (v !== null && isFinite(v)) out.push(v);
  }
  return out;
}

function mean(xs: number[]): number | null {
  return xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : null;
}

function median(xs: number[]): number | null {
  if (!xs.length) return null;
  const sorted = [...xs].sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}

function stddev(xs: number[]): number | null {
  if (xs.length < 2) return null;
  const m = mean(xs)!;
  const variance = xs.reduce((acc, v) => acc + (v - m) * (v - m), 0) / (xs.length - 1);
  return Math.sqrt(variance);
}

function cvPct(xs: number[]): number | null {
  const m = mean(xs);
  const sd = stddev(xs);
  if (m === null || sd === null || m <= 0) return null;
  return (sd / m) * 100;
}

function maxOf(xs: number[]): number | null {
  return xs.length ? Math.max(...xs) : null;
}

/**
 * Compute a profile's result from its run. All metrics come from measurement
 * samples only; warm-up establishes stabilization and is never scored.
 */
export function computeProfileResult(run: ProfileRun): ProfileResult {
  const samples = run.measureSamples;
  const cadenceMs = typeof run.cadenceMs === 'number' && run.cadenceMs > 0 ? run.cadenceMs : DEFAULT_CADENCE_MS;
  const validMeasure = samples.filter(s => !s.gap);
  const measureStartTMs = typeof run.measureStartTMs === 'number' ? run.measureStartTMs : 0;

  // Coverage is measured over window-relative sample times — no valid sample is
  // ever discarded to force an exact ratio.
  const cov = computeCoverage({
    sampleTimesMs: validMeasure.map(s => s.tMs - measureStartTMs),
    windowMs: run.requestedMeasureMs,
    cadenceMs,
  });
  const validSamples = cov.validSamples;
  const missingSamples = cov.missingSamples;
  const expected = cov.expectedTarget;
  const coveragePct = expected > 0 ? cov.coveragePct : null;
  const vis: VisibilityStats = run.visibility ?? {
    currentlyHidden: false, interruptions: 0, totalHiddenMs: 0, longestHiddenMs: 0,
    hiddenDuringWarmup: false, hiddenDuringMeasure: false,
  };

  const hashrates = pick(samples, s => s.hashRate);
  const powers = pick(samples, s => s.power);
  const asicTemps = pick(samples, s => s.asicTemp);
  const vrmTemps = pick(samples, s => s.vrmTemp);
  const reqFans = pick(samples, s => s.requestedFan);
  const appFans = pick(samples, s => s.appliedFan);
  const errors = pick(samples, s => s.errorPercentage);
  const latencies = pick(samples, s => s.poolLatency);

  const avgHashrate = mean(hashrates);
  const avgPower = mean(powers);
  const avgEfficiency = (avgPower !== null && avgHashrate !== null && avgHashrate > 0)
    ? avgPower / (avgHashrate / 1000)
    : null;

  const shares = accumulateShareDeltas(samples);
  const totalShares = shares.accepted + shares.rejected;
  const rejectConfident = totalShares >= REJECT_MIN_TOTAL || shares.rejected >= REJECT_MIN_REJECTED;
  const rejectRatePct = shares.hasData && totalShares > 0 && rejectConfident
    ? (shares.rejected / totalShares) * 100
    : null;

  // Fan-saturation duration: sum the inter-sample interval preceding each
  // saturated sample (documented approximation over the measured samples).
  let fanSaturationMs = 0;
  for (let i = 1; i < samples.length; i++) {
    const s = samples[i];
    if (!s.gap && s.appliedFan !== null && s.appliedFan >= FAN_SATURATION_PCT) {
      fanSaturationMs += Math.max(0, s.tMs - samples[i - 1].tMs);
    }
  }

  const { status, reason } = resolveStatus(run, validSamples, coveragePct, expected, vis);

  const result: ProfileResult = {
    profileId: run.profileId,
    profileName: run.profileName,
    status,
    statusReason: reason,
    thermalControlMode: run.thermalControlMode,
    requestedMeasureMs: run.requestedMeasureMs,
    measuredMeasureMs: run.measuredMeasureMs,
    validSamples,
    missingSamples,
    expectedSamples: expected,
    coveragePct,
    cadenceMs,
    medianIntervalMs: cov.medianIntervalMs,
    maxGapMs: cov.maxGapMs,
    totalGapMs: cov.totalGapMs,
    visInterruptions: vis.interruptions,
    totalHiddenMs: vis.totalHiddenMs,
    longestHiddenMs: vis.longestHiddenMs,
    hiddenDuringWarmup: vis.hiddenDuringWarmup,
    hiddenDuringMeasure: vis.hiddenDuringMeasure,
    restartOccurred: run.restartOccurred,
    countersReset: run.countersReset || shares.hadReset,
    abortReason: run.abortReason ?? null,
    avgHashrate,
    medianHashrate: median(hashrates),
    hashrateVariabilityPct: cvPct(hashrates),
    avgPower,
    avgEfficiency,
    peakAsicTemp: maxOf(asicTemps),
    avgAsicTemp: mean(asicTemps),
    peakVrmTemp: maxOf(vrmTemps),
    avgVrmTemp: mean(vrmTemps),
    avgRequestedFan: mean(reqFans),
    avgAppliedFan: mean(appFans),
    fanSaturationMs,
    avgErrorRate: mean(errors),
    acceptedShareDelta: shares.hasData ? shares.accepted : null,
    rejectedShareDelta: shares.hasData ? shares.rejected : null,
    rejectRatePct,
    poolLatencyAvg: mean(latencies),
    poolLatencyPeak: maxOf(latencies),
    badges: [],
  };

  result.badges = statusBadges(result, run);
  return result;
}

/** Minutes, rounded, for a human duration phrase (≥ 1). */
function mins(ms: number): number {
  return Math.max(1, Math.round(ms / 60000));
}

/**
 * Resolve the run status AND the exact reason for it. A Partial is never a bare
 * badge — it names the dominant cause (coverage, hidden page, or gap), so the
 * owner always knows WHY a completed wall-clock window did not yield a completed
 * profile result.
 */
function resolveStatus(
  run: ProfileRun,
  validSamples: number,
  coveragePct: number | null,
  expectedTarget: number,
  vis: VisibilityStats,
): { status: RunStatus; reason: string } {
  if (run.failed) {
    return { status: 'failed', reason: `Failed — ${run.abortReason ?? 'the run could not complete'}.` };
  }
  if (run.aborted) {
    return { status: 'aborted', reason: `Aborted — ${run.abortReason ?? 'a stop condition triggered'}.` };
  }
  if (validSamples < MIN_VALID_SAMPLES) {
    return { status: 'insufficient', reason: `Insufficient — only ${validSamples} valid sample(s) (need ≥ ${MIN_VALID_SAMPLES}).` };
  }
  if (!run.ranFullWindow) {
    return { status: 'partial', reason: `Partial — the measurement window did not run to completion (${validSamples} valid samples, target ${expectedTarget}).` };
  }
  const pct = coveragePctText(coveragePct);
  if (coveragePct !== null && coveragePct < MIN_COVERAGE * 100) {
    // The window ran fully but too little evidence was captured. Name the most
    // likely driver: a hidden page is the usual cause of throttled coverage.
    let reason = `Partial — telemetry coverage ${pct} (${validSamples} valid samples, target ${expectedTarget}).`;
    if (vis.hiddenDuringMeasure && vis.totalHiddenMs > 0) {
      reason += ` Page hidden ~${mins(vis.totalHiddenMs)} min during measurement — keep the Lab page visible for full coverage.`;
    }
    return { status: 'partial', reason };
  }
  let reason = `Completed the configured measurement window with sufficient telemetry coverage (${pct}, ${validSamples} valid samples, target ${expectedTarget}).`;
  if (vis.hiddenDuringMeasure && vis.totalHiddenMs > 0) {
    reason += ` Note: the page was hidden ~${mins(vis.totalHiddenMs)} min but telemetry coverage stayed sufficient.`;
  }
  return { status: 'completed', reason };
}

function statusBadges(result: ProfileResult, run: ProfileRun): ResultBadge[] {
  const badges: ResultBadge[] = [];
  const pct = coveragePctText(result.coveragePct);
  switch (result.status) {
    case 'completed':
      badges.push({ kind: 'completed', label: 'Completed', severity: 'ok' });
      break;
    case 'partial':
      // Never a bare "Partial" — the badge carries the coverage so the reason is
      // unmistakable even at a glance.
      badges.push({ kind: 'partial', label: `Partial — coverage ${pct}`, severity: 'warn' });
      break;
    case 'insufficient':
      badges.push({ kind: 'insufficient', label: `Insufficient — ${result.validSamples} valid samples`, severity: 'warn' });
      break;
    case 'aborted':
      badges.push({ kind: 'aborted', label: `Aborted: ${run.abortReason ?? 'stop condition'}`, severity: 'danger' });
      break;
    case 'failed':
      badges.push({ kind: 'failed', label: `Failed: ${run.abortReason ?? 'run failed'}`, severity: 'danger' });
      break;
  }
  if (result.hiddenDuringMeasure) {
    badges.push({ kind: 'page-hidden', label: 'Page was hidden', severity: 'info' });
  }
  if (result.countersReset) {
    badges.push({ kind: 'counter-reset', label: 'Share counters reset', severity: 'info' });
  }
  if (result.restartOccurred) {
    badges.push({ kind: 'restarted', label: 'Device restarted', severity: 'info' });
  }
  return badges;
}

/**
 * A qualified, honest statement about a run. Never claims permanent stability
 * from one window, and always leads with the exact status reason.
 */
export function completionStatement(result: ProfileResult): string {
  if (result.status === 'aborted' || result.status === 'failed' || result.status === 'insufficient') {
    return result.statusReason;
  }
  const tail = ' This is evidence from one session, not a lifetime stability guarantee.';
  if (result.status === 'partial') {
    return `${result.statusReason} No configured stop condition triggered.${tail}`;
  }
  return `${result.statusReason} No configured stop condition triggered during this session.${tail}`;
}

// ---------------------------------------------------------------------------
// Comparative badges across the set (transparent, no opaque score)
// ---------------------------------------------------------------------------

const EPS = 1e-9;

/**
 * Award comparative badges among the results. Each badge names the exact winning
 * metric — highest hashrate, lowest efficiency (J/TH), lowest peak temperature,
 * lowest hashrate variability. Ties share the badge.
 *
 * Phase 2K.1: ONLY Completed results (which by definition have sufficient
 * telemetry coverage) may win a comparison badge. A Partial or Aborted result is
 * never crowned "best" on thin or interrupted evidence — one honest completed
 * session is evidence, not a permanent guarantee.
 */
export function applyComparativeBadges(results: ProfileResult[]): ProfileResult[] {
  const scoreable = results.filter(r => r.status === 'completed');

  const awardMin = (selector: (r: ProfileResult) => number | null, badge: ResultBadge) => {
    const vals = scoreable.map(selector).filter((v): v is number => v !== null);
    if (!vals.length) return;
    const best = Math.min(...vals);
    for (const r of scoreable) {
      const v = selector(r);
      if (v !== null && Math.abs(v - best) < EPS) r.badges.push(badge);
    }
  };
  const awardMax = (selector: (r: ProfileResult) => number | null, badge: ResultBadge) => {
    const vals = scoreable.map(selector).filter((v): v is number => v !== null);
    if (!vals.length) return;
    const best = Math.max(...vals);
    for (const r of scoreable) {
      const v = selector(r);
      if (v !== null && Math.abs(v - best) < EPS) r.badges.push(badge);
    }
  };

  if (scoreable.length >= 2) {
    awardMax(r => r.avgHashrate, { kind: 'highest-hashrate', label: 'Highest hashrate', severity: 'ok' });
    awardMin(r => r.avgEfficiency, { kind: 'lowest-efficiency', label: 'Lowest efficiency (J/TH)', severity: 'ok' });
    awardMin(r => r.peakAsicTemp, { kind: 'lowest-temp', label: 'Lowest temperature', severity: 'ok' });
    awardMin(r => r.hashrateVariabilityPct, { kind: 'lowest-variability', label: 'Lowest variability', severity: 'ok' });
  }
  return results;
}

/** Whether an aborted/failed/insufficient result may be promoted (never in v0.1). */
export function isPromotable(result: ProfileResult): boolean {
  return result.status === 'completed';
}
