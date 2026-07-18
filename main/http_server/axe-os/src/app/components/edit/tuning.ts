import { AbstractControl, ValidationErrors, ValidatorFn } from '@angular/forms';

/**
 * Tuning workspace helpers (Stage 2G).
 *
 * Everything here is built strictly from values the firmware itself provides
 * or enforces:
 *  - frequency / core-voltage option lists and defaults come from
 *    GET /api/system/asic (board-601 values served by the device);
 *  - numeric bounds mirror the firmware's own NVS validation table
 *    (main/nvs_config.c) — the UI blocks exactly what the firmware would
 *    reject, nothing more, nothing less.
 *
 * Presets are combinations of EXISTING valid option values only; nothing is
 * invented, nothing auto-saves, nothing auto-restarts.
 */

/** Bounds from the firmware NVS table (main/nvs_config.c). */
export const TUNING_BOUNDS = {
  /** TYPE_FLOAT, min 1 — values outside the ASIC option list are expert territory. */
  frequency: { min: 1, max: 65535 },
  /** TYPE_U16 millivolts, min 1. */
  coreVoltage: { min: 1, max: 65535 },
  temptarget: { min: 35, max: 66 },
  minFanSpeed: { min: 0, max: 99 },
  manualFanSpeed: { min: 0, max: 100 },
} as const;

export type PresetId = 'eco' | 'balanced' | 'performance';

export interface TuningPreset {
  id: PresetId;
  label: string;
  /** Exact existing option values — always shown to the user before applying. */
  frequency: number;
  coreVoltage: number;
}

function validOptions(options: unknown): number[] {
  return Array.isArray(options)
    ? options.filter((v): v is number => typeof v === 'number' && isFinite(v) && v > 0)
    : [];
}

/**
 * Build presets from the device-provided option lists. Returns [] when the
 * lists are missing or too small to be meaningful — no invented values.
 * Eco = lowest option pair, Balanced = the board defaults, Performance =
 * highest option pair. Duplicate value pairs are collapsed (first wins).
 */
export function buildTuningPresets(
  frequencyOptions: number[] | undefined,
  voltageOptions: number[] | undefined,
  defaultFrequency: number | undefined,
  defaultVoltage: number | undefined,
): TuningPreset[] {
  const freqs = validOptions(frequencyOptions);
  const volts = validOptions(voltageOptions);
  if (freqs.length < 2 || volts.length < 2) {
    return [];
  }

  const candidates: TuningPreset[] = [
    { id: 'eco', label: 'Eco', frequency: Math.min(...freqs), coreVoltage: Math.min(...volts) },
  ];

  if (typeof defaultFrequency === 'number' && freqs.includes(defaultFrequency)
    && typeof defaultVoltage === 'number' && volts.includes(defaultVoltage)) {
    candidates.push({ id: 'balanced', label: 'Balanced', frequency: defaultFrequency, coreVoltage: defaultVoltage });
  }

  candidates.push({ id: 'performance', label: 'Performance', frequency: Math.max(...freqs), coreVoltage: Math.max(...volts) });

  const seen = new Set<string>();
  return candidates.filter(preset => {
    const key = `${preset.frequency}/${preset.coreVoltage}`;
    if (seen.has(key)) {
      return false;
    }
    seen.add(key);
    return true;
  });
}

/**
 * Which preset the pending values correspond to; 'custom' whenever the pair
 * diverges from every preset (required behavior: Custom is selected the
 * moment values diverge).
 */
export function activeTuningPreset(
  frequency: unknown,
  coreVoltage: unknown,
  presets: TuningPreset[],
): PresetId | 'custom' {
  const match = presets.find(p => p.frequency === frequency && p.coreVoltage === coreVoltage);
  return match ? match.id : 'custom';
}

/**
 * Validator mirroring the firmware's numeric NVS validation: the value must
 * be a finite number within [min, max]. Blocks NaN/null/undefined/Infinity
 * and out-of-range submissions at the form level.
 */
export function firmwareRangeValidator(min: number, max: number, integer: boolean = false): ValidatorFn {
  return (control: AbstractControl): ValidationErrors | null => {
    const value = control.value;
    if (value === null || value === undefined || value === '') {
      return { required: true };
    }
    const num = typeof value === 'number' ? value : Number(value);
    if (!isFinite(num)) {
      return { nonFinite: true };
    }
    if (integer && !Number.isInteger(num)) {
      return { integer: true };
    }
    if (num < min || num > max) {
      return { range: { min, max, actual: num } };
    }
    return null;
  };
}

// ---------------------------------------------------------------------------
// Phase 2H — thermal control (mode / fan curve / hysteresis)
// ---------------------------------------------------------------------------

export type ThermalControlMode = 'target' | 'curve' | 'manual';

export interface FanCurvePoint {
  tempC: number;
  fanPercent: number;
}

/**
 * Bounds mirror the firmware contract exactly (components/thermal_control):
 * 4 points, temperatures 20–70 °C (the final point stays ≥5 °C below the
 * 75 °C hard-throttle limit), fan 0–100 %, hysteresis 0–8 °C.
 */
export const FAN_CURVE_BOUNDS = {
  points: 4,
  tempC: { min: 20, max: 70 },
  fanPercent: { min: 0, max: 100 },
  hysteresis: { min: 0, max: 8 },
} as const;

const MODE_LABELS: { [mode: string]: string } = {
  target: 'Target Temperature',
  curve: 'Fan Curve',
  manual: 'Manual Fan',
};

export function thermalModeLabel(mode: unknown): string {
  return (typeof mode === 'string' && MODE_LABELS[mode]) || '—';
}

/**
 * Validate a pending curve exactly as the firmware will
 * (parse_fan_curve_json + thermal_curve_validate): exactly 4 points, integer
 * values, temps 20–70 strictly ascending, fan 0–100 non-decreasing.
 * Returns human-readable errors; [] means the firmware would accept it.
 */
export function validateFanCurve(points: ReadonlyArray<FanCurvePoint> | null | undefined): string[] {
  if (!Array.isArray(points) || points.length !== FAN_CURVE_BOUNDS.points) {
    return [`The curve needs exactly ${FAN_CURVE_BOUNDS.points} points.`];
  }
  const errors: string[] = [];
  points.forEach((point, i) => {
    const n = i + 1;
    const t = point?.tempC;
    const f = point?.fanPercent;
    if (typeof t !== 'number' || !isFinite(t) || !Number.isInteger(t)) {
      errors.push(`Point ${n}: temperature must be a whole number.`);
    } else if (t < FAN_CURVE_BOUNDS.tempC.min || t > FAN_CURVE_BOUNDS.tempC.max) {
      errors.push(`Point ${n}: temperature must be ${FAN_CURVE_BOUNDS.tempC.min}–${FAN_CURVE_BOUNDS.tempC.max} °C.`);
    }
    if (typeof f !== 'number' || !isFinite(f) || !Number.isInteger(f)) {
      errors.push(`Point ${n}: fan percent must be a whole number.`);
    } else if (f < FAN_CURVE_BOUNDS.fanPercent.min || f > FAN_CURVE_BOUNDS.fanPercent.max) {
      errors.push(`Point ${n}: fan percent must be ${FAN_CURVE_BOUNDS.fanPercent.min}–${FAN_CURVE_BOUNDS.fanPercent.max} %.`);
    }
  });
  if (errors.length) {
    return errors;
  }
  for (let i = 1; i < points.length; i++) {
    if (points[i].tempC <= points[i - 1].tempC) {
      errors.push(`Point ${i + 1}: temperature must be higher than point ${i}.`);
    }
    if (points[i].fanPercent < points[i - 1].fanPercent) {
      errors.push(`Point ${i + 1}: fan percent may not decrease.`);
    }
  }
  return errors;
}

/**
 * Firmware segment semantics: -1 none/not in curve mode, 0 below the first
 * point, 1..3 interpolating between points k and k+1, 4 at/above the final
 * point.
 */
export function curveSegmentLabel(segment: unknown): string {
  if (typeof segment !== 'number' || !Number.isInteger(segment) || segment < 0) {
    return '—';
  }
  if (segment === 0) {
    return 'Below curve';
  }
  if (segment >= FAN_CURVE_BOUNDS.points) {
    return 'Above final point';
  }
  return `P${segment} → P${segment + 1}`;
}

export type ThermalProfileId = 'quiet' | 'balanced' | 'aggressive';

export interface ThermalProfile {
  id: ThermalProfileId;
  label: string;
  description: string;
  points: FanCurvePoint[];
}

/**
 * Pending curve templates only — selecting one fills the pending editor and
 * nothing else (no save, no restart). Values are documented and inside the
 * firmware bounds; every profile still reaches 100 % fan at or before the
 * default curve's 64 °C final point, so none trades protection for silence.
 * "Balanced" is exactly the firmware's built-in default curve.
 */
export const THERMAL_PROFILES: ThermalProfile[] = [
  {
    id: 'quiet',
    label: 'Quiet',
    description: 'Ramps later (48–64 °C) for lower noise at low load; still reaches 100 % at 64 °C.',
    points: [
      { tempC: 48, fanPercent: 25 },
      { tempC: 55, fanPercent: 40 },
      { tempC: 60, fanPercent: 60 },
      { tempC: 64, fanPercent: 100 },
    ],
  },
  {
    id: 'balanced',
    label: 'Balanced',
    description: "The firmware's built-in default curve (45–64 °C).",
    points: [
      { tempC: 45, fanPercent: 25 },
      { tempC: 52, fanPercent: 45 },
      { tempC: 58, fanPercent: 70 },
      { tempC: 64, fanPercent: 100 },
    ],
  },
  {
    id: 'aggressive',
    label: 'Aggressive Cooling',
    description: 'Ramps early (40–60 °C) and reaches 100 % at 60 °C for maximum cooling headroom.',
    points: [
      { tempC: 40, fanPercent: 35 },
      { tempC: 48, fanPercent: 60 },
      { tempC: 54, fanPercent: 85 },
      { tempC: 60, fanPercent: 100 },
    ],
  },
];

/** Profile matching the pending points, or 'custom' the moment they diverge. */
export function activeThermalProfile(points: ReadonlyArray<FanCurvePoint>, profiles: ThermalProfile[] = THERMAL_PROFILES): ThermalProfileId | 'custom' {
  const match = profiles.find(profile =>
    profile.points.length === points.length
    && profile.points.every((p, i) => p.tempC === points[i]?.tempC && p.fanPercent === points[i]?.fanPercent));
  return match ? match.id : 'custom';
}

export interface CurvePreviewPoint {
  x: number;
  y: number;
  tempC: number;
  fanPercent: number;
}

export interface CurvePreviewModel {
  /** SVG polyline points, including the flat plateaus below/above the curve. */
  polyline: string;
  markers: CurvePreviewPoint[];
  /** x for a live temperature marker; null when out of the drawable range. */
  tempX: (tempC: number | null | undefined) => number | null;
  width: number;
  height: number;
}

/**
 * Pure viewport math for the SVG curve preview. Maps 20–70 °C to x and
 * 0–100 % to y (inverted), with the below-first / above-final plateaus the
 * firmware actually applies. Display only — never feeds values back.
 */
export function curvePreviewModel(points: ReadonlyArray<FanCurvePoint>, width: number = 280, height: number = 120, pad: number = 8): CurvePreviewModel {
  const { min: tMin, max: tMax } = FAN_CURVE_BOUNDS.tempC;
  const x = (t: number) => pad + ((t - tMin) / (tMax - tMin)) * (width - 2 * pad);
  const y = (f: number) => pad + ((100 - f) / 100) * (height - 2 * pad);

  const valid = validateFanCurve(points).length === 0;
  const markers: CurvePreviewPoint[] = valid
    ? points.map(p => ({ x: x(p.tempC), y: y(p.fanPercent), tempC: p.tempC, fanPercent: p.fanPercent }))
    : [];

  let polyline = '';
  if (valid) {
    const parts: string[] = [];
    parts.push(`${x(tMin)},${y(points[0].fanPercent)}`);
    points.forEach(p => parts.push(`${x(p.tempC)},${y(p.fanPercent)}`));
    parts.push(`${x(tMax)},${y(points[points.length - 1].fanPercent)}`);
    polyline = parts.join(' ');
  }

  return {
    polyline,
    markers,
    tempX: (tempC: number | null | undefined) => {
      if (typeof tempC !== 'number' || !isFinite(tempC) || tempC < tMin || tempC > tMax) {
        return null;
      }
      return x(tempC);
    },
    width,
    height,
  };
}

/** Compact one-line curve rendering for read-only telemetry views. */
export function fanCurveSummary(points: unknown): string {
  if (!Array.isArray(points) || points.length === 0) {
    return '—';
  }
  return points
    .map(p => `${p?.tempC ?? '?'}°→${p?.fanPercent ?? '?'}%`)
    .join(' · ');
}

/** The 8 flat curve form-control names, in order. */
export const CURVE_TEMP_CONTROLS = ['fanCurveTemp0', 'fanCurveTemp1', 'fanCurveTemp2', 'fanCurveTemp3'] as const;
export const CURVE_FAN_CONTROLS = ['fanCurveFan0', 'fanCurveFan1', 'fanCurveFan2', 'fanCurveFan3'] as const;

/** Read the pending curve out of a flat form-value object. */
export function curveFromFormValue(value: { [key: string]: unknown }): FanCurvePoint[] {
  return CURVE_TEMP_CONTROLS.map((tempKey, i) => ({
    tempC: Number(value[tempKey]),
    fanPercent: Number(value[CURVE_FAN_CONTROLS[i]]),
  }));
}

/**
 * Build the PATCH payload from the raw form value. The flat curve controls
 * are internal to the editor: the firmware only ever receives the structured
 * fanCurve array (when curve mode is pending) plus the mode string, and the
 * legacy autofanspeed flag stays in sync (target/curve → true,
 * manual → false) exactly as the firmware does on its side.
 */
export function buildSettingsPayload(raw: { [key: string]: any }): { [key: string]: any } {
  const payload: { [key: string]: any } = {};
  const internal = new Set<string>([...CURVE_TEMP_CONTROLS, ...CURVE_FAN_CONTROLS]);
  for (const key of Object.keys(raw)) {
    if (!internal.has(key)) {
      payload[key] = raw[key];
    }
  }
  const mode = raw['thermalControlMode'];
  if (mode === 'target' || mode === 'curve' || mode === 'manual') {
    payload['autofanspeed'] = mode !== 'manual';
    if (mode === 'curve') {
      const curve = curveFromFormValue(raw);
      if (validateFanCurve(curve).length === 0) {
        payload['fanCurve'] = curve;
      }
    }
  }
  return payload;
}

/** Fields the firmware applies live (no restart) — mirrors edit.component's noRestartFields. */
export interface PendingChange {
  field: string;
  label: string;
  current: string;
  pending: string;
  restartRequired: boolean;
}

const FIELD_LABELS: { [key: string]: { label: string; unit?: string } } = {
  frequency: { label: 'Frequency', unit: ' MHz' },
  coreVoltage: { label: 'Core Voltage', unit: ' mV' },
  autofanspeed: { label: 'Automatic Fan Control' },
  thermalControlMode: { label: 'Thermal Control Mode' },
  temptarget: { label: 'Target Temperature', unit: ' °C' },
  minfanspeed: { label: 'Minimum Fan Speed', unit: ' %' },
  manualFanSpeed: { label: 'Manual Fan Speed', unit: ' %' },
  fanCurveTemp0: { label: 'Curve Point 1 Temperature', unit: ' °C' },
  fanCurveTemp1: { label: 'Curve Point 2 Temperature', unit: ' °C' },
  fanCurveTemp2: { label: 'Curve Point 3 Temperature', unit: ' °C' },
  fanCurveTemp3: { label: 'Curve Point 4 Temperature', unit: ' °C' },
  fanCurveFan0: { label: 'Curve Point 1 Fan', unit: ' %' },
  fanCurveFan1: { label: 'Curve Point 2 Fan', unit: ' %' },
  fanCurveFan2: { label: 'Curve Point 3 Fan', unit: ' %' },
  fanCurveFan3: { label: 'Curve Point 4 Fan', unit: ' %' },
  fanCurveHysteresis: { label: 'Curve Hysteresis', unit: ' °C' },
  overheat_mode: { label: 'Overheat Mode' },
  display: { label: 'Display Type' },
  rotation: { label: 'Display Rotation', unit: '°' },
  invertscreen: { label: 'Invert Display' },
  displayTimeout: { label: 'Display Sleep' },
  statsFrequency: { label: 'Data Logging Interval', unit: ' s' },
};

function displayValue(field: string, value: unknown): string {
  if (value === null || value === undefined || value === '') {
    return '—';
  }
  if (typeof value === 'boolean') {
    return value ? 'On' : 'Off';
  }
  if (field === 'thermalControlMode') {
    return thermalModeLabel(value);
  }
  const unit = FIELD_LABELS[field]?.unit ?? '';
  return `${value}${unit}`;
}

/**
 * Diff of a pending (unsaved) form state against the loaded device baseline.
 * Pure data for the "Current → Pending" review list; display only.
 */
export function pendingChanges(
  baseline: { [key: string]: unknown },
  pending: { [key: string]: unknown },
  noRestartFields: string[],
): PendingChange[] {
  return Object.keys(baseline)
    .filter(field => {
      const a = baseline[field];
      const b = pending[field];
      return b !== undefined && a !== b;
    })
    .map(field => ({
      field,
      label: FIELD_LABELS[field]?.label ?? field,
      current: displayValue(field, baseline[field]),
      pending: displayValue(field, pending[field]),
      restartRequired: !noRestartFields.includes(field),
    }));
}
