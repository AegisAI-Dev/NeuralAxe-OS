/**
 * Semantic operational status — FIXED meanings, independent of the user's
 * accent theme:
 *
 *   ok      (green) healthy / online / success
 *   info    (cyan)  neutral technical information
 *   warn    (amber) caution
 *   danger  (red)   actual error, dangerous value or destructive action
 *   neutral (gray)  inactive / unknown / no data
 *
 * These helpers classify live telemetry into one of those severities. They are
 * pure display logic: thresholds mirror the warnings that already exist in the
 * UI (e.g. the legacy dashboard's "Danger: High Temperature" at 70 °C) so the
 * bar color and the warning text can never disagree. A user picking a red
 * accent theme must never turn healthy telemetry red — accent styling and
 * semantic status are separate systems.
 */

export type SemanticSeverity = 'ok' | 'info' | 'warn' | 'danger' | 'neutral';

function invalid(value: unknown): boolean {
  return value === null || value === undefined || typeof value !== 'number' || !isFinite(value as number);
}

/** CSS class for a telemetry meter (PrimeNG progress bar) of that severity. */
export function meterClass(severity: SemanticSeverity): string {
  return `nx-meter-${severity}`;
}

/**
 * ASIC temperature. Warning starts 5 °C below the 70 °C danger line the UI
 * already communicates ("Danger: High Temperature" on the legacy dashboard).
 */
export function asicTempSeverity(temp: unknown): SemanticSeverity {
  if (invalid(temp) || (temp as number) <= 0) return 'neutral';
  const t = temp as number;
  if (t >= 70) return 'danger';
  if (t >= 65) return 'warn';
  return 'ok';
}

/**
 * Voltage-regulator temperature. Danger at 105 °C (existing dashboard
 * warning); caution from 85 °C (existing Command Deck gauge threshold).
 */
export function vrTempSeverity(vrTemp: unknown): SemanticSeverity {
  if (invalid(vrTemp) || (vrTemp as number) <= 0) return 'neutral';
  const t = vrTemp as number;
  if (t >= 105) return 'danger';
  if (t >= 85) return 'warn';
  return 'ok';
}

/** Power draw relative to the board's rated maximum. */
export function powerSeverity(power: unknown, maxPower: unknown): SemanticSeverity {
  if (invalid(power) || invalid(maxPower) || (maxPower as number) <= 0) return 'neutral';
  const ratio = (power as number) / (maxPower as number);
  if (ratio > 1) return 'danger';
  if (ratio >= 0.9) return 'warn';
  return 'ok';
}

/**
 * Input voltage vs. nominal. Mirrors the existing low-voltage danger rule
 * `voltage < (nominal + 0.5) * 0.87`; a small band above it is a caution.
 */
export function inputVoltageSeverity(voltage: unknown, nominalVoltage: unknown): SemanticSeverity {
  if (invalid(voltage) || invalid(nominalVoltage) || (nominalVoltage as number) <= 0) return 'neutral';
  const v = voltage as number;
  const ceiling = (nominalVoltage as number) + 0.5;
  if (v < ceiling * 0.87) return 'danger';
  if (v < ceiling * 0.9) return 'warn';
  return 'ok';
}

/** Fan duty and measured frequency/voltage are neutral technical info. */
export function infoSeverity(value: unknown): SemanticSeverity {
  return invalid(value) ? 'neutral' : 'info';
}

/**
 * Thermal headroom relative to the configured target temperature.
 * Below/at target is healthy; above target is a caution until the fixed
 * 70 °C danger line takes over.
 */
export function thermalDeltaSeverity(temp: unknown, target: unknown): SemanticSeverity {
  if (invalid(temp) || (temp as number) <= 0) return 'neutral';
  if ((temp as number) >= 70) return 'danger';
  if (invalid(target) || (target as number) <= 0) return 'neutral';
  return (temp as number) > (target as number) + 2 ? 'warn' : 'ok';
}

/** Fan saturation: automatic fan control pinned at (near) 100% duty. */
export function fanSaturationSeverity(autoFan: unknown, fanPercent: unknown): SemanticSeverity {
  if (invalid(fanPercent)) return 'neutral';
  if (!autoFan) return 'info';
  return (fanPercent as number) >= 95 ? 'warn' : 'ok';
}
