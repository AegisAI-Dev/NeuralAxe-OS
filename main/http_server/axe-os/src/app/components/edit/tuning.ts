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
  temptarget: { label: 'Target Temperature', unit: ' °C' },
  minfanspeed: { label: 'Minimum Fan Speed', unit: ' %' },
  manualFanSpeed: { label: 'Manual Fan Speed', unit: ' %' },
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
