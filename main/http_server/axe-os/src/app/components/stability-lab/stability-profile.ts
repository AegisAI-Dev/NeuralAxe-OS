/**
 * NeuralAxe Stability Lab — profile model (Phase 2K).
 *
 * A Stability Profile is an OWNER-SELECTED tuning configuration to be
 * benchmarked. It contains ONLY supported tuning/thermal fields — never pool,
 * Wi-Fi, worker or credential data. Every numeric bound reuses existing
 * repository truth:
 *  - frequency / core-voltage option lists come from GET /api/system/asic;
 *  - numeric bounds mirror the firmware NVS table via ../edit/tuning
 *    (TUNING_BOUNDS, FAN_CURVE_BOUNDS) — nothing is invented here;
 *  - the fan curve is validated exactly as the firmware will (validateFanCurve).
 *
 * There is NO autonomous search and NO invented aggressive preset: starter
 * profiles are built only from the device-served option lists and the captured
 * baseline. profileToSettings() emits the firmware's own REST field names.
 */

import {
  TUNING_BOUNDS,
  FAN_CURVE_BOUNDS,
  FanCurvePoint,
  ThermalControlMode,
  TuningPreset,
  buildTuningPresets,
  validateFanCurve,
} from '../edit/tuning';

export { FanCurvePoint, ThermalControlMode } from '../edit/tuning';

/**
 * Warm-up / measurement / cooldown duration bounds (seconds). Conservative
 * minimums keep a run meaningful; generous maximums avoid forcing a marathon.
 * A measurement window shorter than a minute cannot be called a benchmark.
 */
export const DURATION_BOUNDS = {
  warmupSec: { min: 30, max: 900 },      // 30 s – 15 min
  measureSec: { min: 60, max: 3600 },    // 1 min – 60 min
  cooldownSec: { min: 0, max: 600 },     // 0 – 10 min
} as const;

export const DEFAULT_DURATIONS = {
  warmupSec: 180,    // 3 min
  measureSec: 1200,  // 20 min
  cooldownSec: 0,
} as const;

/** Owner/session profile limits. */
export const PROFILE_LIMITS = {
  minProfiles: 1,
  maxProfiles: 5,
  nameMaxLength: 40,
  notesMaxLength: 280,
} as const;

/**
 * A supported tuning configuration snapshot — the shared shape for the captured
 * baseline and every profile's applied settings. Only tuning/thermal fields.
 */
export interface TuningConfig {
  frequency: number;
  coreVoltage: number;
  thermalControlMode: ThermalControlMode;
  /** target mode only */
  temptarget?: number;
  /** target / curve modes (min-fan floor) */
  minFanSpeed?: number;
  /** manual mode only */
  manualFanSpeed?: number;
  /** curve mode only */
  fanCurve?: FanCurvePoint[];
  /** curve mode only */
  fanCurveHysteresis?: number;
}

export interface StabilityProfile extends TuningConfig {
  /** Stable local identifier (never leaves the browser). */
  id: string;
  name: string;
  warmupSec: number;
  measureSec: number;
  cooldownSec: number;
  notes?: string;
}

/** A single "current → profile" field change for the diff view. */
export interface ProfileDiffRow {
  field: string;
  label: string;
  current: string;
  next: string;
}

const num = (value: unknown): number | null =>
  typeof value === 'number' && isFinite(value) ? value : null;

/** A finite integer inside [min, max], else null. */
function boundedInt(value: unknown, min: number, max: number): number | null {
  const n = num(value);
  if (n === null || !Number.isInteger(n) || n < min || n > max) {
    return null;
  }
  return n;
}

// ---------------------------------------------------------------------------
// Baseline capture
// ---------------------------------------------------------------------------

/**
 * Capture the supported tuning configuration from a live SystemInfo-like object.
 * Reads only tuning/thermal fields — pool/Wi-Fi/worker/credentials are never
 * touched. Missing fields stay undefined rather than being invented.
 */
export function captureBaseline(info: { [key: string]: any } | null | undefined): TuningConfig | null {
  if (!info) {
    return null;
  }
  const frequency = num(info['frequency']);
  const coreVoltage = num(info['coreVoltage']);
  const mode = info['thermalControlMode'];
  if (frequency === null || coreVoltage === null
    || (mode !== 'target' && mode !== 'curve' && mode !== 'manual')) {
    return null;
  }
  const config: TuningConfig = {
    frequency,
    coreVoltage,
    thermalControlMode: mode,
  };
  const target = num(info['temptarget']);
  if (target !== null) config.temptarget = target;
  const minFan = num(info['minFanSpeed']);
  if (minFan !== null) config.minFanSpeed = minFan;
  const manualFan = num(info['manualFanSpeed']);
  if (manualFan !== null) config.manualFanSpeed = manualFan;
  const hyst = num(info['fanCurveHysteresis']);
  if (hyst !== null) config.fanCurveHysteresis = hyst;
  const curve = info['fanCurve'];
  if (Array.isArray(curve) && curve.length === FAN_CURVE_BOUNDS.points) {
    config.fanCurve = curve.map((p: any) => ({ tempC: p.tempC, fanPercent: p.fanPercent }));
  }
  return config;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

/**
 * Validate a profile exactly as the firmware would, plus the Lab's own
 * duration/naming rules. Returns human-readable errors; [] means valid.
 * `frequencyOptions` / `voltageOptions` are advisory: a value outside them is
 * allowed (expert territory) but flagged separately via outsideOptionList().
 */
export function validateProfile(profile: Partial<StabilityProfile> | null | undefined): string[] {
  const errors: string[] = [];
  if (!profile) {
    return ['Profile is missing.'];
  }

  const name = typeof profile.name === 'string' ? profile.name.trim() : '';
  if (!name) {
    errors.push('Give the profile a name.');
  } else if (name.length > PROFILE_LIMITS.nameMaxLength) {
    errors.push(`Name must be ${PROFILE_LIMITS.nameMaxLength} characters or fewer.`);
  }

  if (boundedInt(profile.frequency, TUNING_BOUNDS.frequency.min, TUNING_BOUNDS.frequency.max) === null
    && num(profile.frequency) === null) {
    errors.push('Frequency must be a number.');
  } else if (num(profile.frequency) !== null
    && (num(profile.frequency)! < TUNING_BOUNDS.frequency.min || num(profile.frequency)! > TUNING_BOUNDS.frequency.max)) {
    errors.push(`Frequency must be ${TUNING_BOUNDS.frequency.min}–${TUNING_BOUNDS.frequency.max} MHz.`);
  }

  if (boundedInt(profile.coreVoltage, TUNING_BOUNDS.coreVoltage.min, TUNING_BOUNDS.coreVoltage.max) === null) {
    errors.push(`Core voltage must be a whole number ${TUNING_BOUNDS.coreVoltage.min}–${TUNING_BOUNDS.coreVoltage.max} mV.`);
  }

  const mode = profile.thermalControlMode;
  if (mode !== 'target' && mode !== 'curve' && mode !== 'manual') {
    errors.push('Choose a thermal control mode.');
  } else if (mode === 'target') {
    if (boundedInt(profile.temptarget, TUNING_BOUNDS.temptarget.min, TUNING_BOUNDS.temptarget.max) === null) {
      errors.push(`Target temperature must be ${TUNING_BOUNDS.temptarget.min}–${TUNING_BOUNDS.temptarget.max} °C.`);
    }
    if (boundedInt(profile.minFanSpeed, TUNING_BOUNDS.minFanSpeed.min, TUNING_BOUNDS.minFanSpeed.max) === null) {
      errors.push(`Minimum fan must be ${TUNING_BOUNDS.minFanSpeed.min}–${TUNING_BOUNDS.minFanSpeed.max} %.`);
    }
  } else if (mode === 'manual') {
    if (boundedInt(profile.manualFanSpeed, TUNING_BOUNDS.manualFanSpeed.min, TUNING_BOUNDS.manualFanSpeed.max) === null) {
      errors.push(`Manual fan must be ${TUNING_BOUNDS.manualFanSpeed.min}–${TUNING_BOUNDS.manualFanSpeed.max} %.`);
    }
  } else if (mode === 'curve') {
    errors.push(...validateFanCurve(profile.fanCurve));
    if (boundedInt(profile.fanCurveHysteresis, FAN_CURVE_BOUNDS.hysteresis.min, FAN_CURVE_BOUNDS.hysteresis.max) === null) {
      errors.push(`Curve hysteresis must be ${FAN_CURVE_BOUNDS.hysteresis.min}–${FAN_CURVE_BOUNDS.hysteresis.max} °C.`);
    }
    if (boundedInt(profile.minFanSpeed, TUNING_BOUNDS.minFanSpeed.min, TUNING_BOUNDS.minFanSpeed.max) === null) {
      errors.push(`Minimum fan must be ${TUNING_BOUNDS.minFanSpeed.min}–${TUNING_BOUNDS.minFanSpeed.max} %.`);
    }
  }

  if (boundedInt(profile.warmupSec, DURATION_BOUNDS.warmupSec.min, DURATION_BOUNDS.warmupSec.max) === null) {
    errors.push(`Warm-up must be ${DURATION_BOUNDS.warmupSec.min}–${DURATION_BOUNDS.warmupSec.max} seconds.`);
  }
  if (boundedInt(profile.measureSec, DURATION_BOUNDS.measureSec.min, DURATION_BOUNDS.measureSec.max) === null) {
    errors.push(`Measurement must be ${DURATION_BOUNDS.measureSec.min}–${DURATION_BOUNDS.measureSec.max} seconds.`);
  }
  if (boundedInt(profile.cooldownSec, DURATION_BOUNDS.cooldownSec.min, DURATION_BOUNDS.cooldownSec.max) === null) {
    errors.push(`Cooldown must be ${DURATION_BOUNDS.cooldownSec.min}–${DURATION_BOUNDS.cooldownSec.max} seconds.`);
  }

  if (typeof profile.notes === 'string' && profile.notes.length > PROFILE_LIMITS.notesMaxLength) {
    errors.push(`Notes must be ${PROFILE_LIMITS.notesMaxLength} characters or fewer.`);
  }

  return errors;
}

/**
 * True when the profile's frequency/voltage pair is not one of the device's
 * served option values — expert territory, allowed but surfaced to the owner.
 */
export function outsideOptionList(
  profile: Pick<StabilityProfile, 'frequency' | 'coreVoltage'>,
  frequencyOptions: number[] | undefined,
  voltageOptions: number[] | undefined,
): boolean {
  const freqs = Array.isArray(frequencyOptions) ? frequencyOptions : [];
  const volts = Array.isArray(voltageOptions) ? voltageOptions : [];
  if (!freqs.length || !volts.length) {
    return false; // no option truth to compare against
  }
  return !freqs.includes(profile.frequency) || !volts.includes(profile.coreVoltage);
}

// ---------------------------------------------------------------------------
// Duplicate detection
// ---------------------------------------------------------------------------

/**
 * Normalized signature of the EFFECTIVE settings a profile applies (name and
 * durations excluded). Two profiles with the same signature are duplicates.
 */
export function profileSignature(config: TuningConfig): string {
  const mode = config.thermalControlMode;
  const parts: (string | number)[] = ['f', config.frequency, 'v', config.coreVoltage, 'm', mode];
  if (mode === 'target') {
    parts.push('t', config.temptarget ?? '', 'mf', config.minFanSpeed ?? '');
  } else if (mode === 'manual') {
    parts.push('man', config.manualFanSpeed ?? '');
  } else if (mode === 'curve') {
    parts.push('mf', config.minFanSpeed ?? '', 'h', config.fanCurveHysteresis ?? '');
    (config.fanCurve ?? []).forEach(p => parts.push(p.tempC, p.fanPercent));
  }
  return parts.join(':');
}

/** Whether adding `candidate` would duplicate an existing profile's settings. */
export function isDuplicateProfile(candidate: TuningConfig, existing: TuningConfig[]): boolean {
  const sig = profileSignature(candidate);
  return existing.some(p => profileSignature(p) === sig);
}

// ---------------------------------------------------------------------------
// Diff against the captured original
// ---------------------------------------------------------------------------

function fanCurveText(points: FanCurvePoint[] | undefined): string {
  if (!Array.isArray(points) || points.length === 0) {
    return '—';
  }
  return points.map(p => `${p.tempC}°→${p.fanPercent}%`).join(' · ');
}

const MODE_LABEL: { [k in ThermalControlMode]: string } = {
  target: 'Target Temperature',
  curve: 'Fan Curve',
  manual: 'Manual Fan',
};

/**
 * The exact fields a profile changes versus the captured original — the
 * "Current → Profile" review. Only fields that genuinely differ are listed;
 * a field the profile leaves at the baseline is never shown as a change.
 */
export function profileDiff(original: TuningConfig, profile: TuningConfig): ProfileDiffRow[] {
  const rows: ProfileDiffRow[] = [];
  const push = (field: string, label: string, cur: unknown, next: unknown, fmt: (v: any) => string) => {
    const c = fmt(cur);
    const n = fmt(next);
    if (c !== n) {
      rows.push({ field, label, current: c, next: n });
    }
  };
  const mhz = (v: any) => (num(v) === null ? '—' : `${v} MHz`);
  const mv = (v: any) => (num(v) === null ? '—' : `${v} mV`);
  const degc = (v: any) => (num(v) === null ? '—' : `${v} °C`);
  const pct = (v: any) => (num(v) === null ? '—' : `${v} %`);

  push('frequency', 'Frequency', original.frequency, profile.frequency, mhz);
  push('coreVoltage', 'Core Voltage', original.coreVoltage, profile.coreVoltage, mv);
  push('thermalControlMode', 'Thermal Mode', original.thermalControlMode, profile.thermalControlMode, v => MODE_LABEL[v as ThermalControlMode] ?? '—');

  // Mode-relevant fields for the PROFILE's target mode (never invent omitted).
  if (profile.thermalControlMode === 'target') {
    push('temptarget', 'Target Temperature', original.temptarget, profile.temptarget, degc);
    push('minFanSpeed', 'Minimum Fan', original.minFanSpeed, profile.minFanSpeed, pct);
  } else if (profile.thermalControlMode === 'manual') {
    push('manualFanSpeed', 'Manual Fan', original.manualFanSpeed, profile.manualFanSpeed, pct);
  } else if (profile.thermalControlMode === 'curve') {
    push('minFanSpeed', 'Minimum Fan', original.minFanSpeed, profile.minFanSpeed, pct);
    push('fanCurveHysteresis', 'Curve Hysteresis', original.fanCurveHysteresis, profile.fanCurveHysteresis, degc);
    push('fanCurve', 'Fan Curve', original.fanCurve, profile.fanCurve, fanCurveText);
  }
  return rows;
}

// ---------------------------------------------------------------------------
// Firmware settings payload
// ---------------------------------------------------------------------------

/**
 * Build the PATCH /api/system payload for a tuning config, using the firmware's
 * own REST field names (nvs_config.c rest_name). The legacy autofanspeed flag
 * is kept in sync with the mode (target/curve → 1, manual → 0) exactly as the
 * firmware and the existing edit component do. Only tuning/thermal keys are ever
 * emitted — no pool/Wi-Fi/worker/credential field can appear here.
 */
export function profileToSettings(config: TuningConfig): { [key: string]: any } {
  const payload: { [key: string]: any } = {
    frequency: config.frequency,
    coreVoltage: config.coreVoltage,
    thermalControlMode: config.thermalControlMode,
    autofanspeed: config.thermalControlMode !== 'manual' ? 1 : 0,
  };
  if (config.thermalControlMode === 'target') {
    if (num(config.temptarget) !== null) payload['temptarget'] = config.temptarget;
    if (num(config.minFanSpeed) !== null) payload['minFanSpeed'] = config.minFanSpeed;
  } else if (config.thermalControlMode === 'manual') {
    if (num(config.manualFanSpeed) !== null) payload['manualFanSpeed'] = config.manualFanSpeed;
  } else if (config.thermalControlMode === 'curve') {
    if (num(config.minFanSpeed) !== null) payload['minFanSpeed'] = config.minFanSpeed;
    if (num(config.fanCurveHysteresis) !== null) payload['fanCurveHysteresis'] = config.fanCurveHysteresis;
    if (validateFanCurve(config.fanCurve).length === 0) payload['fanCurve'] = config.fanCurve;
  }
  return payload;
}

// ---------------------------------------------------------------------------
// Restart audit — which emitted fields apply live vs require a restart
// ---------------------------------------------------------------------------

/**
 * Every REST field profileToSettings() can emit, classified live-vs-restart
 * against proven repository truth (keys lower-cased for the firmware's
 * case-insensitive cJSON lookup):
 *
 *  - frequency, coreVoltage, autofanspeed, thermalControlMode, manualFanSpeed,
 *    temptarget, minFanSpeed  → in edit.component.ts `noRestartFields`
 *    (applied live; the fan controller re-reads within one second).
 *  - fanCurve, fanCurveHysteresis → the fan_controller_task re-reads "mode,
 *    curve, hysteresis, min fan within one second of a save — no restart"
 *    (tasks/fan_controller_task.c); edit.component lists the flat curve
 *    controls in noRestartFields for the same reason.
 *
 * Result: on this firmware EVERY field a Stability Profile writes is applied
 * live. This table is the single source of truth for the restart calculation,
 * so a future restart-only field added here (or to profileToSettings) is counted
 * honestly instead of being silently assumed live.
 */
export const LIVE_APPLIED_FIELDS: ReadonlySet<string> = new Set([
  'frequency', 'corevoltage', 'autofanspeed', 'thermalcontrolmode',
  'manualfanspeed', 'temptarget', 'minfanspeed', 'fancurve', 'fancurvehysteresis',
]);

export interface RestartAuditRow {
  field: string;
  live: boolean;
  source: string;
}

/** Whether a single emitted REST field needs a restart to take effect. */
export function fieldRequiresRestart(field: string): boolean {
  return !LIVE_APPLIED_FIELDS.has(String(field).toLowerCase());
}

/** Per-field live/restart classification for every field a profile can emit. */
export function restartAudit(): RestartAuditRow[] {
  const NO_RESTART = 'edit.component noRestartFields';
  const FAN_TASK = 'fan_controller_task re-read (≤1 s)';
  const rows: Array<[string, string]> = [
    ['frequency', NO_RESTART], ['coreVoltage', NO_RESTART], ['thermalControlMode', NO_RESTART],
    ['autofanspeed', NO_RESTART], ['temptarget', NO_RESTART], ['minFanSpeed', NO_RESTART],
    ['manualFanSpeed', NO_RESTART], ['fanCurve', FAN_TASK], ['fanCurveHysteresis', FAN_TASK],
  ];
  return rows.map(([field, source]) => ({ field, live: !fieldRequiresRestart(field), source }));
}

function payloadValue(v: unknown): string {
  return JSON.stringify(v ?? null);
}

/**
 * Whether applying `to` over a device currently at `from` requires a restart:
 * true only when a field whose VALUE changes is classified restart-only. Values
 * that are identical need no write, and every changed field here is live, so
 * this returns false for all supported profiles — but it is computed from the
 * real payload diff, never assumed.
 */
export function profileRestartRequired(from: TuningConfig, to: TuningConfig): boolean {
  const fromP = profileToSettings(from);
  const toP = profileToSettings(to);
  const keys = new Set([...Object.keys(fromP), ...Object.keys(toP)]);
  for (const key of keys) {
    if (payloadValue(fromP[key]) !== payloadValue(toP[key]) && fieldRequiresRestart(key)) {
      return true;
    }
  }
  return false;
}

/**
 * Real planned-restart count for a session: each profile is applied over the
 * device's current state (baseline → profile 1 → profile 2 → …), plus the
 * final restore back to baseline. Counts only transitions that genuinely need a
 * restart — derived from the diffs, not asserted.
 */
export function sessionRestartCount(baseline: TuningConfig, profiles: TuningConfig[]): number {
  let count = 0;
  let current = baseline;
  for (const profile of profiles) {
    if (profileRestartRequired(current, profile)) count++;
    current = profile;
  }
  if (profiles.length && profileRestartRequired(current, baseline)) count++; // restore
  return count;
}

// ---------------------------------------------------------------------------
// Starter profiles (honest — no invented tuning)
// ---------------------------------------------------------------------------

let profileSeq = 0;
/** Monotonic local id; unique within a browser session. */
export function nextProfileId(): string {
  profileSeq += 1;
  return `p${Date.now().toString(36)}-${profileSeq}`;
}

export interface StarterProfileSpec {
  key: 'current' | 'conservative' | 'balanced' | 'performance';
  label: string;
  description: string;
  config: TuningConfig;
}

/** Starter build output: usable specs plus honest notes for omitted candidates. */
export interface StarterBuildResult {
  specs: StarterProfileSpec[];
  /** Explanations for meaningful starters that were deliberately omitted. */
  notes: string[];
}

const PRESET_TO_STARTER: { [id in TuningPreset['id']]: { key: StarterProfileSpec['key']; label: string } } = {
  eco: { key: 'conservative', label: 'Conservative' },
  balanced: { key: 'balanced', label: 'Balanced' },
  performance: { key: 'performance', label: 'Performance' },
};

/**
 * A short, honest description of what a starter actually changes versus the
 * baseline — the real freq/voltage diff, never a marketing label. A same-setup
 * candidate (no change) is described as such rather than pretending otherwise.
 */
export function describeStarterDiff(baseline: TuningConfig, config: TuningConfig): string {
  const parts: string[] = [];
  const fd = config.frequency - baseline.frequency;
  const vd = config.coreVoltage - baseline.coreVoltage;
  if (fd > 0) parts.push(`raises frequency ${baseline.frequency}→${config.frequency} MHz`);
  else if (fd < 0) parts.push(`lowers frequency ${baseline.frequency}→${config.frequency} MHz`);
  if (vd > 0) parts.push(`raises voltage ${baseline.coreVoltage}→${config.coreVoltage} mV`);
  else if (vd < 0) parts.push(`lowers voltage ${baseline.coreVoltage}→${config.coreVoltage} mV`);
  if (!parts.length) return 'No change from the current configuration.';
  const sentence = `${parts.join(', ')} — same thermal setup.`;
  return sentence.charAt(0).toUpperCase() + sentence.slice(1);
}

/**
 * Build starter profile specs HONESTLY. "Current Configuration" is the exact
 * captured baseline. Conservative/Balanced/Performance vary ONLY frequency and
 * voltage, derived from the device-served option lists (buildTuningPresets), and
 * keep the current thermal setup. On top of that honest-derivation base, three
 * hard rules apply so no misleading starter is ever offered:
 *
 *  1. A candidate whose ONLY meaningful change is a higher voltage at the SAME
 *     frequency is never offered — that adds heat and power with no extra
 *     hashrate and is not a real "performance" gain. (This is exactly the
 *     625 MHz / 1150 mV → 625 MHz / 1250 mV case the real pilot exposed.)
 *  2. A Performance starter is offered only when it genuinely RAISES the
 *     frequency; when the baseline is already at the highest served frequency,
 *     Performance is omitted with an explanation rather than silently dropped.
 *  3. Duplicates (of Current or of each other) are collapsed.
 *
 * No voltage/frequency pair is invented; a raised voltage only ever appears
 * bundled with a genuinely higher frequency it supports (repository-served
 * option truth). Returns the usable specs plus notes explaining any omission.
 */
export function buildStarterProfiles(
  baseline: TuningConfig | null | undefined,
  frequencyOptions: number[] | undefined,
  voltageOptions: number[] | undefined,
  defaultFrequency: number | undefined,
  defaultVoltage: number | undefined,
): StarterBuildResult {
  if (!baseline) {
    return { specs: [], notes: [] };
  }
  const currentConfig: TuningConfig = { ...baseline, fanCurve: baseline.fanCurve ? [...baseline.fanCurve] : undefined };
  const specs: StarterProfileSpec[] = [{
    key: 'current',
    label: 'Current Configuration',
    description: 'Exactly what the device runs right now — the control reference.',
    config: currentConfig,
  }];
  const notes: string[] = [];
  const seen = new Set<string>([profileSignature(currentConfig)]);

  const presets = buildTuningPresets(frequencyOptions, voltageOptions, defaultFrequency, defaultVoltage);
  for (const preset of presets) {
    const meta = PRESET_TO_STARTER[preset.id];
    const config: TuningConfig = {
      ...baseline,
      frequency: preset.frequency,
      coreVoltage: preset.coreVoltage,
      fanCurve: baseline.fanCurve ? [...baseline.fanCurve] : undefined,
    };
    const sig = profileSignature(config);
    const fd = preset.frequency - baseline.frequency;
    const vd = preset.coreVoltage - baseline.coreVoltage;

    // Rule 3: drop duplicates (of Current or an earlier starter) silently.
    if (seen.has(sig)) {
      continue;
    }
    // Rule 1: never offer same-frequency / higher-voltage as any starter.
    if (fd === 0 && vd > 0) {
      notes.push(`${meta.label} starter omitted — it would only raise voltage ${baseline.coreVoltage}→${preset.coreVoltage} mV at the same ${baseline.frequency} MHz (more heat and power, no extra hashrate).`);
      continue;
    }
    // Rule 2: Performance must genuinely raise the frequency.
    if (meta.key === 'performance' && fd <= 0) {
      notes.push(`Performance starter omitted — ${baseline.frequency} MHz is already at or above the highest served frequency, so there is no honest higher-frequency option.`);
      continue;
    }

    seen.add(sig);
    specs.push({ key: meta.key, label: meta.label, description: describeStarterDiff(baseline, config), config });
  }

  return { specs, notes };
}

/** Total estimated session seconds for an ordered profile queue. */
export function totalSessionSeconds(profiles: Array<Pick<StabilityProfile, 'warmupSec' | 'measureSec' | 'cooldownSec'>>): number {
  return profiles.reduce((sum, p) => sum + (num(p.warmupSec) ?? 0) + (num(p.measureSec) ?? 0) + (num(p.cooldownSec) ?? 0), 0);
}

// ---------------------------------------------------------------------------
// Maximum session-duration contract (Phase 2K.1)
// ---------------------------------------------------------------------------

/**
 * Slack multiplier over the planned duration to absorb restarts, reconnect
 * grace and modest browser throttling before the hard cap trips.
 */
export const MAX_SESSION_SLACK = 2;
/** Fixed head-room added on top of the slack multiple (ms). */
export const MAX_SESSION_MARGIN_MS = 10 * 60 * 1000; // 10 min
/** Absolute ceiling — a session may never run longer than this, whatever the plan. */
export const MAX_SESSION_ABSOLUTE_MS = 6 * 60 * 60 * 1000; // 6 h

/**
 * The bounded maximum session DURATION (a monotonic runtime cap — NOT a
 * wall-clock timestamp). A running session whose MONOTONIC elapsed time exceeds
 * this (e.g. because the browser was throttled/hung so phase deadlines arrived
 * far late) is aborted and the original configuration restored — the tested
 * profile is never left active indefinitely. Because the cap is compared against
 * the monotonic clock, a system wall-clock change (forward or backward) can never
 * trip it early or extend it. Derived from the planned duration (× slack +
 * margin), then clamped to an absolute ceiling.
 */
export function maxSessionDurationMs(
  profiles: Array<Pick<StabilityProfile, 'warmupSec' | 'measureSec' | 'cooldownSec'>>,
): number {
  const plannedMs = totalSessionSeconds(profiles) * 1000;
  const bounded = plannedMs * MAX_SESSION_SLACK + MAX_SESSION_MARGIN_MS;
  return Math.min(MAX_SESSION_ABSOLUTE_MS, Math.max(MAX_SESSION_MARGIN_MS, bounded));
}
