/**
 * NeuralAxe Stability Lab — telemetry sampling model (Phase 2K).
 *
 * Samples are snapshots of the EXISTING shared telemetry stream
 * (LiveDataService.info$) taken on a fixed cadence — the Lab never opens a
 * second polling loop and never over-polls the miner. Every field is guarded:
 * an unavailable reading is null, never NaN/Infinity, and a tick that arrives
 * without core telemetry is marked as a gap rather than invented.
 *
 * Share counters are cumulative on the device and reset on reboot; this module
 * derives per-interval deltas carefully and flags resets instead of treating a
 * cumulative counter as per-sample production.
 */

/** Least-aggressive cadence consistent with the existing 5 s poll fallback. */
export const SAMPLE_INTERVAL_MS = 5000;

export type SamplePhase = 'warmup' | 'measure' | 'cooldown';

export interface RawTelemetry {
  hashRate?: number;
  expectedHashrate?: number;
  power?: number;
  temp?: number;                       // ASIC temperature
  vrTemp?: number;                     // VRM temperature
  requestedFanPercent?: number;
  appliedFanPercent?: number;
  fanspeed?: number;
  fanrpm?: number;
  errorPercentage?: number;
  sharesAccepted?: number;
  sharesRejected?: number;
  responseTime?: number;               // pool latency ms
  thermalControlMode?: string;
  effectiveControlTemperature?: number;
  emergencyOverrideActive?: number;
  controlSensorValid?: number;
  miningPaused?: boolean;
}

export interface LabSample {
  /** Monotonic milliseconds since session start (never negative). */
  tMs: number;
  phase: SamplePhase;
  profileIndex: number;
  hashRate: number | null;
  expectedHashrate: number | null;
  power: number | null;
  /** Derived J/TH; null when hashrate is zero/unavailable. */
  efficiency: number | null;
  asicTemp: number | null;
  vrmTemp: number | null;
  requestedFan: number | null;
  appliedFan: number | null;
  rpm: number | null;
  errorPercentage: number | null;
  /** Cumulative counters exactly as reported (deltas are derived elsewhere). */
  sharesAccepted: number | null;
  sharesRejected: number | null;
  poolLatency: number | null;
  thermalControlMode: string | null;
  controlTemp: number | null;
  emergencyOverride: boolean;
  sensorValid: boolean;
  miningPaused: boolean;
  /** True when core telemetry (hashrate AND ASIC temp) was missing this tick. */
  gap: boolean;
}

export function finiteOrNull(value: unknown): number | null {
  return typeof value === 'number' && isFinite(value) ? value : null;
}

/** A non-negative finite counter, or null. */
function counter(value: unknown): number | null {
  const n = finiteOrNull(value);
  return n !== null && n >= 0 ? n : null;
}

export interface SampleContext {
  sessionStartMs: number;
  now: number;
  phase: SamplePhase;
  profileIndex: number;
}

/**
 * Build one guarded sample from a raw telemetry object. Time is clamped to be
 * monotonic and non-negative; every numeric field passes through finiteOrNull
 * so NaN/Infinity can never enter the record.
 */
export function buildSample(info: RawTelemetry | null | undefined, ctx: SampleContext): LabSample {
  const tMs = Math.max(0, Math.round(ctx.now - ctx.sessionStartMs));
  const hashRate = finiteOrNull(info?.hashRate);
  const power = finiteOrNull(info?.power);
  const asicTemp = positiveOrNull(info?.temp);
  const vrmTemp = positiveOrNull(info?.vrTemp);

  let efficiency: number | null = null;
  if (hashRate !== null && hashRate > 0 && power !== null && power >= 0) {
    efficiency = power / (hashRate / 1000);
  }

  const appliedFan = finiteOrNull(info?.appliedFanPercent) ?? finiteOrNull(info?.fanspeed);

  return {
    tMs,
    phase: ctx.phase,
    profileIndex: ctx.profileIndex,
    hashRate,
    expectedHashrate: finiteOrNull(info?.expectedHashrate),
    power,
    efficiency,
    asicTemp,
    vrmTemp,
    requestedFan: finiteOrNull(info?.requestedFanPercent),
    appliedFan,
    rpm: finiteOrNull(info?.fanrpm),
    errorPercentage: finiteOrNull(info?.errorPercentage),
    sharesAccepted: counter(info?.sharesAccepted),
    sharesRejected: counter(info?.sharesRejected),
    poolLatency: nonNegativeOrNull(info?.responseTime),
    thermalControlMode: typeof info?.thermalControlMode === 'string' ? info.thermalControlMode : null,
    controlTemp: positiveOrNull(info?.effectiveControlTemperature),
    emergencyOverride: finiteOrNull(info?.emergencyOverrideActive) === 1,
    sensorValid: finiteOrNull(info?.controlSensorValid) === 1,
    miningPaused: info?.miningPaused === true,
    gap: hashRate === null && asicTemp === null,
  };
}

function positiveOrNull(value: unknown): number | null {
  const n = finiteOrNull(value);
  return n !== null && n > 0 ? n : null;
}

function nonNegativeOrNull(value: unknown): number | null {
  const n = finiteOrNull(value);
  return n !== null && n >= 0 ? n : null;
}

// ---------------------------------------------------------------------------
// Share-delta derivation (cumulative counters, reset-aware)
// ---------------------------------------------------------------------------

export interface ShareDelta {
  accepted: number;
  rejected: number;
  /** True when a counter went backwards (a device reboot reset it). */
  reset: boolean;
}

/**
 * Delta between two cumulative counter readings. A backwards step means the
 * device reset its counters (reboot); the post-reset reading is then the shares
 * since the reset, and the reset flag is raised so callers never present a
 * cumulative counter as per-interval production.
 */
export function shareDelta(
  prevAccepted: number | null,
  prevRejected: number | null,
  currAccepted: number | null,
  currRejected: number | null,
): ShareDelta {
  const pa = counter(prevAccepted);
  const pr = counter(prevRejected);
  const ca = counter(currAccepted);
  const cr = counter(currRejected);
  if (ca === null || cr === null) {
    return { accepted: 0, rejected: 0, reset: false };
  }
  if (pa === null || pr === null) {
    // First reading of the window: no delta yet.
    return { accepted: 0, rejected: 0, reset: false };
  }
  const reset = ca < pa || cr < pr;
  if (reset) {
    // Counters restarted from zero; attribute the post-reset totals.
    return { accepted: ca, rejected: cr, reset: true };
  }
  return { accepted: ca - pa, rejected: cr - pr, reset: false };
}

export interface AccumulatedShares {
  accepted: number;
  rejected: number;
  /** Any counter reset seen across the window. */
  hadReset: boolean;
  /** Whether at least two readings with valid counters existed. */
  hasData: boolean;
}

/**
 * Sum share deltas across an ordered series of samples (measurement window).
 * Only consecutive pairs that both carry valid counters contribute; a reset is
 * summed as the post-reset totals and flagged.
 */
export function accumulateShareDeltas(samples: ReadonlyArray<Pick<LabSample, 'sharesAccepted' | 'sharesRejected'>>): AccumulatedShares {
  let accepted = 0;
  let rejected = 0;
  let hadReset = false;
  let pairs = 0;
  for (let i = 1; i < samples.length; i++) {
    const d = shareDelta(
      samples[i - 1].sharesAccepted, samples[i - 1].sharesRejected,
      samples[i].sharesAccepted, samples[i].sharesRejected,
    );
    if (samples[i].sharesAccepted !== null && samples[i - 1].sharesAccepted !== null) {
      pairs++;
    }
    accepted += d.accepted;
    rejected += d.rejected;
    hadReset = hadReset || d.reset;
  }
  return { accepted, rejected, hadReset, hasData: pairs > 0 };
}

// ---------------------------------------------------------------------------
// Coverage
// ---------------------------------------------------------------------------

/** Expected number of samples for a window of a given duration at the cadence. */
export function expectedSampleCount(durationMs: number, cadenceMs: number = SAMPLE_INTERVAL_MS): number {
  const d = finiteOrNull(durationMs);
  const c = finiteOrNull(cadenceMs);
  if (d === null || c === null || c <= 0 || d <= 0) {
    return 0;
  }
  return Math.max(1, Math.round(d / c));
}

/** Count samples missing core telemetry (gaps) in a window. */
export function countGaps(samples: ReadonlyArray<Pick<LabSample, 'gap'>>): number {
  return samples.reduce((n, s) => n + (s.gap ? 1 : 0), 0);
}
