/**
 * Pure display formatting for Block Intelligence (Phase 2L).
 *
 * Kept out of the templates so every format rule is unit-tested and never
 * fabricates a value: unknown inputs render as an em dash, never as 0 or a
 * guess. Timestamps are always presented in UTC and clearly marked as such.
 */

import { SATS_PER_BTC } from './block-intelligence.model';

const DASH = '—';

function finite(value: unknown): number | null {
  return typeof value === 'number' && isFinite(value) ? value : null;
}

/** First 8 and last 8 hex characters of a hash, for compact display. */
export function shortHash(hash: string | null | undefined): string {
  if (typeof hash !== 'string' || hash.length < 20) {
    return typeof hash === 'string' && hash.length > 0 ? hash : DASH;
  }
  return `${hash.slice(0, 8)}…${hash.slice(-8)}`;
}

/** Compact relative age, e.g. "just now", "3m", "1h 5m", "2d 3h". */
export function formatAgeShort(ms: number | null | undefined): string {
  const v = finite(ms);
  if (v === null || v < 0) {
    return DASH;
  }
  const totalSec = Math.floor(v / 1000);
  if (totalSec < 30) {
    return 'just now';
  }
  const d = Math.floor(totalSec / 86400);
  const h = Math.floor((totalSec % 86400) / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const s = totalSec % 60;
  if (d > 0) return `${d}d${h > 0 ? ' ' + h + 'h' : ''}`;
  if (h > 0) return `${h}h${m > 0 ? ' ' + m + 'm' : ''}`;
  if (m > 0) return `${m}m${s > 0 ? ' ' + s + 's' : ''}`;
  return `${s}s`;
}

/** Interval duration, e.g. "8m 40s", "1h 2m". */
export function formatInterval(ms: number | null | undefined): string {
  const v = finite(ms);
  if (v === null || v < 0) {
    return DASH;
  }
  const totalSec = Math.round(v / 1000);
  const h = Math.floor(totalSec / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const s = totalSec % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

/** Satoshis → BTC string with 3 decimals by default; em dash when unknown. */
export function formatBtc(sats: number | null | undefined, decimals: number = 3): string {
  const v = finite(sats);
  if (v === null) {
    return DASH;
  }
  return `${(v / SATS_PER_BTC).toFixed(decimals)} BTC`;
}

/** Satoshis rendered as a grouped integer; em dash when unknown. */
export function formatSats(sats: number | null | undefined): string {
  const v = finite(sats);
  if (v === null) {
    return DASH;
  }
  return `${Math.round(v).toLocaleString('en-US')} sats`;
}

/** Byte count in KB/MB with one decimal; em dash when unknown. */
export function formatBytes(bytes: number | null | undefined): string {
  const v = finite(bytes);
  if (v === null || v < 0) {
    return DASH;
  }
  if (v >= 1_000_000) return `${(v / 1_000_000).toFixed(2)} MB`;
  if (v >= 1_000) return `${(v / 1_000).toFixed(1)} KB`;
  return `${Math.round(v)} B`;
}

/** Weight units in millions (MWU) with two decimals; em dash when unknown. */
export function formatWeight(weight: number | null | undefined): string {
  const v = finite(weight);
  if (v === null || v < 0) {
    return DASH;
  }
  return `${(v / 1_000_000).toFixed(2)} MWU`;
}

/** Grouped integer (tx count etc.); em dash when unknown. */
export function formatCount(value: number | null | undefined): string {
  const v = finite(value);
  if (v === null || v < 0) {
    return DASH;
  }
  return Math.round(v).toLocaleString('en-US');
}

/** Absolute UTC timestamp "YYYY-MM-DD HH:MM:SS UTC"; em dash when unknown. */
export function formatUtc(ms: number | null | undefined): string {
  const v = finite(ms);
  if (v === null || v <= 0) {
    return DASH;
  }
  const d = new Date(v);
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())} `
    + `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}:${pad(d.getUTCSeconds())} UTC`;
}

/** Blocks-per-hour with one decimal; em dash when unknown. */
export function formatPerHour(value: number | null | undefined): string {
  const v = finite(value);
  if (v === null || v < 0) {
    return DASH;
  }
  return `${v.toFixed(1)}/h`;
}
