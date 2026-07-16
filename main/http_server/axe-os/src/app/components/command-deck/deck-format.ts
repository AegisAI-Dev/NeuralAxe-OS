import { ByteSuffixPipe } from 'src/app/pipes/byte-suffix.pipe';

/**
 * Defensive display formatting for live-device telemetry.
 *
 * Real hardware occasionally reports null/NaN/Infinity or long fractional
 * values (e.g. pool latency 106.8960037 ms). These helpers guarantee the
 * Command Deck never renders raw invalid states or uncontrolled precision.
 * They format display strings only — underlying data is never altered.
 */

export const INVALID = '—'; // em dash fallback

function invalid(value: unknown): boolean {
  return value === null || value === undefined || typeof value !== 'number' || !isFinite(value as number);
}

/** Fixed-decimal number with optional suffix; em dash for invalid input. */
export function fmtNum(value: unknown, decimals: number = 1, suffix: string = ''): string {
  if (invalid(value)) {
    return INVALID;
  }
  return (value as number).toFixed(decimals) + suffix;
}

/** Integer with thousands grouping; em dash for invalid input. */
export function fmtInt(value: unknown, suffix: string = ''): string {
  if (invalid(value)) {
    return INVALID;
  }
  return Math.round(value as number).toLocaleString('en-US') + suffix;
}

/**
 * Pool latency: integer milliseconds by default (107 ms), one decimal below
 * 1 ms, em dash for invalid or negative values.
 */
export function fmtLatency(ms: unknown): string {
  if (invalid(ms) || (ms as number) < 0) {
    return INVALID;
  }
  const v = ms as number;
  if (v > 0 && v < 1) {
    return v.toFixed(1) + ' ms';
  }
  return Math.round(v) + ' ms';
}

/** Percentage with fixed decimals; em dash for invalid input. */
export function fmtPct(value: unknown, decimals: number = 2): string {
  if (invalid(value)) {
    return INVALID;
  }
  return (value as number).toFixed(decimals) + ' %';
}

/** Bytes via the upstream ByteSuffix pipe; em dash for invalid input. */
export function fmtBytes(value: unknown): string {
  if (invalid(value) || (value as number) < 0) {
    return INVALID;
  }
  return ByteSuffixPipe.transform(value as number);
}

/** Temperature in whole °C; em dash for invalid input. */
export function fmtTemp(value: unknown): string {
  if (invalid(value)) {
    return INVALID;
  }
  return Math.round(value as number) + '°C';
}

export const DeckFmt = {
  num: fmtNum,
  int: fmtInt,
  latency: fmtLatency,
  pct: fmtPct,
  bytes: fmtBytes,
  temp: fmtTemp,
  INVALID,
};
