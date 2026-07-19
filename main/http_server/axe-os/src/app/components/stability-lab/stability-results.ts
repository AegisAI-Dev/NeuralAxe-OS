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
import { accumulateShareDeltas, countGaps, expectedSampleCount } from './stability-telemetry';

/** Minimum valid measurement samples for a run to be scoreable. */
export const MIN_VALID_SAMPLES = 3;
/** Below this coverage a full-window run is reported as Partial, not Completed. */
export const PARTIAL_COVERAGE = 0.6;
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
  thermalControlMode: string;
  requestedMeasureMs: number;
  measuredMeasureMs: number;
  validSamples: number;
  missingSamples: number;
  expectedSamples: number;
  coveragePct: number | null;
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
  const validSamples = samples.filter(s => !s.gap).length;
  const missingSamples = countGaps(samples);
  const expected = expectedSampleCount(run.requestedMeasureMs);
  const coveragePct = expected > 0 ? Math.min(100, (validSamples / expected) * 100) : null;

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

  const status = resolveStatus(run, validSamples, coveragePct);

  const result: ProfileResult = {
    profileId: run.profileId,
    profileName: run.profileName,
    status,
    thermalControlMode: run.thermalControlMode,
    requestedMeasureMs: run.requestedMeasureMs,
    measuredMeasureMs: run.measuredMeasureMs,
    validSamples,
    missingSamples,
    expectedSamples: expected,
    coveragePct,
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

function resolveStatus(run: ProfileRun, validSamples: number, coveragePct: number | null): RunStatus {
  if (run.failed) return 'failed';
  if (run.aborted) return 'aborted';
  if (validSamples < MIN_VALID_SAMPLES) return 'insufficient';
  if (!run.ranFullWindow) return 'partial';
  if (coveragePct !== null && coveragePct < PARTIAL_COVERAGE * 100) return 'partial';
  return 'completed';
}

function statusBadges(result: ProfileResult, run: ProfileRun): ResultBadge[] {
  const badges: ResultBadge[] = [];
  switch (result.status) {
    case 'completed':
      badges.push({ kind: 'completed', label: 'Completed', severity: 'ok' });
      break;
    case 'partial':
      badges.push({ kind: 'partial', label: 'Partial', severity: 'warn' });
      break;
    case 'insufficient':
      badges.push({ kind: 'insufficient', label: 'Insufficient samples', severity: 'warn' });
      break;
    case 'aborted':
      badges.push({ kind: 'aborted', label: `Aborted: ${run.abortReason ?? 'stop condition'}`, severity: 'danger' });
      break;
    case 'failed':
      badges.push({ kind: 'failed', label: `Failed: ${run.abortReason ?? 'run failed'}`, severity: 'danger' });
      break;
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
 * A qualified, honest statement about a completed run. Never claims permanent
 * stability from one window.
 */
export function completionStatement(result: ProfileResult): string {
  if (result.status === 'aborted') {
    return `Aborted before the window finished: ${result.abortReason ?? 'a stop condition triggered'}.`;
  }
  if (result.status === 'failed') {
    return `The run failed: ${result.abortReason ?? 'see the timeline'}.`;
  }
  if (result.status === 'insufficient') {
    return 'Too few valid samples to score this run.';
  }
  const mins = Math.max(1, Math.round(result.measuredMeasureMs / 60000));
  const base = result.status === 'partial'
    ? `Completed a partial ${mins}-minute measurement window.`
    : `Completed the configured ${mins}-minute measurement window.`;
  return `${base} No configured stop condition triggered during this session. This is evidence from one session, not a lifetime stability guarantee.`;
}

// ---------------------------------------------------------------------------
// Comparative badges across the set (transparent, no opaque score)
// ---------------------------------------------------------------------------

const EPS = 1e-9;

/**
 * Award comparative badges among the scoreable results. Each badge names the
 * exact winning metric — highest hashrate, lowest efficiency (J/TH), lowest
 * peak temperature, lowest hashrate variability. Ties share the badge. Only
 * completed/partial results with the relevant metric participate.
 */
export function applyComparativeBadges(results: ProfileResult[]): ProfileResult[] {
  const scoreable = results.filter(r => r.status === 'completed' || r.status === 'partial');

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
