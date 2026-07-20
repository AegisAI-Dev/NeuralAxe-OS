/**
 * NeuralAxe Stability Lab — stop conditions (Phase 2K).
 *
 * Deterministic, owner-visible stop conditions evaluated per telemetry sample.
 * These NEVER replace the firmware's own hard thermal protection — they are a
 * conservative operator guard that sits underneath it. Emergency override and
 * an invalid control sensor abort immediately; temperature / fan / error /
 * reject conditions use a documented debounce so a single transient spike does
 * not abort a run. Thresholds may be TIGHTENED by the owner but never raised
 * beyond the safe UI ceilings (see clamp helpers).
 *
 * The Lab never deliberately generates an emergency condition — it only reacts
 * to one the firmware reports.
 */

import { LabSample } from './stability-telemetry';
import { clampAsicStop, clampVrmStop } from './stability-preflight';

export const STOP_TUNABLES = {
  errorPct: { min: 1, max: 20, default: 5 },
  rejectPct: { min: 1, max: 25, default: 8 },
  fanSaturationPct: { min: 90, max: 100, default: 100 },
  debounceSamples: { min: 2, max: 6, default: 3 },
} as const;

/** Share-confidence gate, identical to fleet-intel: 100 total or 3 rejected. */
const REJECT_MIN_TOTAL = 100;
const REJECT_MIN_REJECTED = 3;

export interface StopThresholds {
  asicC: number;
  vrmC: number;
  errorPct: number;
  rejectPct: number;
  /** Opt-in: sustained fan saturation is only a stop when enabled. */
  fanSaturationStop: boolean;
  fanSaturationPct: number;
  debounceSamples: number;
}

export function defaultStopThresholds(): StopThresholds {
  return {
    asicC: clampAsicStop(undefined),
    vrmC: clampVrmStop(undefined),
    errorPct: STOP_TUNABLES.errorPct.default,
    rejectPct: STOP_TUNABLES.rejectPct.default,
    fanSaturationStop: false,
    fanSaturationPct: STOP_TUNABLES.fanSaturationPct.default,
    debounceSamples: STOP_TUNABLES.debounceSamples.default,
  };
}

function clampTunable(value: unknown, bounds: { min: number; max: number; default: number }): number {
  const n = typeof value === 'number' && isFinite(value) ? value : bounds.default;
  return Math.min(bounds.max, Math.max(bounds.min, n));
}

/** Clamp every threshold into its safe UI bounds (owner may tighten, not exceed). */
export function clampThresholds(input: Partial<StopThresholds>): StopThresholds {
  return {
    asicC: clampAsicStop(input.asicC),
    vrmC: clampVrmStop(input.vrmC),
    errorPct: clampTunable(input.errorPct, STOP_TUNABLES.errorPct),
    rejectPct: clampTunable(input.rejectPct, STOP_TUNABLES.rejectPct),
    fanSaturationStop: input.fanSaturationStop === true,
    fanSaturationPct: clampTunable(input.fanSaturationPct, STOP_TUNABLES.fanSaturationPct),
    debounceSamples: Math.round(clampTunable(input.debounceSamples, STOP_TUNABLES.debounceSamples)),
  };
}

// ---------------------------------------------------------------------------
// Conservative-default migration (Phase 2K.1)
// ---------------------------------------------------------------------------

/**
 * The Phase 2K default threshold set — the ONLY legacy shape that migrates. Its
 * VRM stop was 100 °C (a hair under the firmware hard limit), which Phase 2K.1
 * replaces with a conservative 70 °C board-601 operator default. Everything else
 * is unchanged, so migration is a targeted, transparent bump — never a silent
 * rewrite of an owner-customised threshold.
 */
export const LEGACY_DEFAULT_THRESHOLDS: Readonly<StopThresholds> = {
  asicC: 68,
  vrmC: 100,
  errorPct: 5,
  rejectPct: 8,
  fanSaturationStop: false,
  fanSaturationPct: 100,
  debounceSamples: 3,
} as const;

/**
 * True when a stored config is EXACTLY the legacy default set — i.e. the owner
 * never touched any threshold. Only such an untouched config is migrated; the
 * moment any field differs (including a deliberate VRM value), the whole config
 * is treated as owner-customised and preserved verbatim (still clamped to safe
 * bounds).
 */
export function isLegacyUntouched(stored: Partial<StopThresholds> | null | undefined): boolean {
  if (!stored) return false;
  const l = LEGACY_DEFAULT_THRESHOLDS;
  return stored.asicC === l.asicC
    && stored.vrmC === l.vrmC
    && stored.errorPct === l.errorPct
    && stored.rejectPct === l.rejectPct
    && stored.fanSaturationStop === l.fanSaturationStop
    && stored.fanSaturationPct === l.fanSaturationPct
    && stored.debounceSamples === l.debounceSamples;
}

/**
 * Migrate a stored (possibly legacy) threshold config to the current model:
 *  - nothing stored → the current conservative defaults;
 *  - an untouched legacy default set → the current conservative defaults
 *    (this is the ONLY case where a stored VRM 100 °C becomes 70 °C);
 *  - any owner-customised config → preserved verbatim, merely clamped to the
 *    safe UI bounds (a deliberate VRM 100 °C is kept, never silently lowered).
 */
export function migrateThresholds(stored: Partial<StopThresholds> | null | undefined): StopThresholds {
  if (!stored) return defaultStopThresholds();
  if (isLegacyUntouched(stored)) return defaultStopThresholds();
  return clampThresholds(stored);
}

export interface DebounceState {
  asic: number;
  vrm: number;
  fan: number;
  error: number;
  reject: number;
  mining: number;
}

export function zeroDebounce(): DebounceState {
  return { asic: 0, vrm: 0, fan: 0, error: 0, reject: 0, mining: 0 };
}

export interface StopReason {
  code: 'emergency' | 'sensor' | 'asic-temp' | 'vrm-temp' | 'fan-saturation' | 'error-rate' | 'reject-rate' | 'mining-stopped';
  message: string;
  immediate: boolean;
  /** The sample time (ms) that triggered the stop. */
  atMs: number;
}

export interface StopEvaluation {
  stop: StopReason | null;
  debounce: DebounceState;
}

/** Cumulative reject percentage of the sample; null before any share. */
function sampleRejectPct(sample: LabSample): number | null {
  const a = sample.sharesAccepted;
  const r = sample.sharesRejected;
  if (a === null || r === null) return null;
  const total = a + r;
  return total > 0 ? (r / total) * 100 : null;
}

function rejectConfident(sample: LabSample): boolean {
  const a = sample.sharesAccepted ?? 0;
  const r = sample.sharesRejected ?? 0;
  return a + r >= REJECT_MIN_TOTAL || r >= REJECT_MIN_REJECTED;
}

/**
 * Evaluate one sample against the thresholds, carrying debounce counters.
 *
 * - Emergency override and an invalid control sensor abort IMMEDIATELY.
 * - Temperature / fan / error / reject conditions must persist for
 *   `debounceSamples` consecutive qualifying samples before they abort.
 * - A gap sample (no telemetry) neither triggers nor resets debounce — it is
 *   simply skipped so a momentary dropout can't mask or fabricate a stop.
 *
 * Pure: given the same inputs it returns the same output; the caller threads
 * the returned debounce state into the next call.
 */
export function evaluateSample(sample: LabSample, thresholds: StopThresholds, debounce: DebounceState): StopEvaluation {
  // A gap sample carries NO telemetry, so it cannot be judged for ANY stop
  // condition — including the immediate ones. Treating "no reading" as
  // "sensor invalid" or "emergency" would fabricate a stop from a mere dropout;
  // telemetry loss is handled by the freshness/offline path instead. Skip it
  // entirely (debounce is left untouched, neither advanced nor reset).
  if (sample.gap) {
    return { stop: null, debounce };
  }

  // Immediate, non-debounced safety states first (real telemetry present).
  if (sample.emergencyOverride) {
    return { stop: { code: 'emergency', message: 'Emergency thermal override active — firmware forced 100 % fan', immediate: true, atMs: sample.tMs }, debounce };
  }
  if (!sample.sensorValid) {
    return { stop: { code: 'sensor', message: 'Control temperature sensor reported invalid data', immediate: true, atMs: sample.tMs }, debounce };
  }

  const next: DebounceState = { ...debounce };
  const need = thresholds.debounceSamples;

  const bump = (key: keyof DebounceState, violated: boolean): boolean => {
    next[key] = violated ? next[key] + 1 : 0;
    return next[key] >= need;
  };

  // ASIC temperature.
  const asicViolated = sample.asicTemp !== null && sample.asicTemp > thresholds.asicC;
  if (bump('asic', asicViolated)) {
    return { stop: { code: 'asic-temp', message: `ASIC ${Math.round(sample.asicTemp!)} °C stayed above the ${thresholds.asicC} °C stop limit`, immediate: false, atMs: sample.tMs }, debounce: next };
  }
  // VRM temperature (only when the board reports it).
  const vrmViolated = sample.vrmTemp !== null && sample.vrmTemp > thresholds.vrmC;
  if (bump('vrm', vrmViolated)) {
    return { stop: { code: 'vrm-temp', message: `VRM ${Math.round(sample.vrmTemp!)} °C stayed above the ${thresholds.vrmC} °C stop limit`, immediate: false, atMs: sample.tMs }, debounce: next };
  }
  // Mining stopped mid-run.
  const miningViolated = sample.miningPaused === true;
  if (bump('mining', miningViolated)) {
    return { stop: { code: 'mining-stopped', message: 'Mining did not stay active during the run', immediate: false, atMs: sample.tMs }, debounce: next };
  }
  // Sustained fan saturation (opt-in).
  const fanViolated = thresholds.fanSaturationStop && sample.appliedFan !== null && sample.appliedFan >= thresholds.fanSaturationPct;
  if (bump('fan', fanViolated)) {
    return { stop: { code: 'fan-saturation', message: `Fan stayed at/above ${thresholds.fanSaturationPct} % (saturated)`, immediate: false, atMs: sample.tMs }, debounce: next };
  }
  // Sustained ASIC error rate.
  const errViolated = sample.errorPercentage !== null && sample.errorPercentage > thresholds.errorPct;
  if (bump('error', errViolated)) {
    return { stop: { code: 'error-rate', message: `ASIC error rate stayed above ${thresholds.errorPct} %`, immediate: false, atMs: sample.tMs }, debounce: next };
  }
  // Sustained reject rate (after the share-confidence gate).
  const rej = sampleRejectPct(sample);
  const rejViolated = rej !== null && rej > thresholds.rejectPct && rejectConfident(sample);
  if (bump('reject', rejViolated)) {
    return { stop: { code: 'reject-rate', message: `Share reject rate stayed above ${thresholds.rejectPct} %`, immediate: false, atMs: sample.tMs }, debounce: next };
  }

  return { stop: null, debounce: next };
}
