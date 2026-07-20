/**
 * Shared, pure block-normalization utilities (Phase 2L, Stage 2 & 15).
 *
 * Adapters use these to turn raw provider fields into guarded internal values,
 * to bound the retained window in memory, and to deduplicate / order blocks.
 * Nothing here performs I/O.
 */

import { BlockSummary, MAX_BLOCKS, SATS_PER_BTC } from './block-intelligence.model';

export function finiteOrNull(value: unknown): number | null {
  return typeof value === 'number' && isFinite(value) ? value : null;
}

/** A finite, non-negative number or null. */
export function nonNegOrNull(value: unknown): number | null {
  const n = finiteOrNull(value);
  return n !== null && n >= 0 ? n : null;
}

/** A finite integer height ≥ 0, or null. */
export function heightOrNull(value: unknown): number | null {
  const n = finiteOrNull(value);
  return n !== null && n >= 0 && Number.isInteger(n) ? n : null;
}

/** Convert a Unix timestamp in seconds to ms; null when invalid. */
export function unixSecondsToMs(value: unknown): number | null {
  const n = finiteOrNull(value);
  if (n === null || n <= 0) {
    return null;
  }
  // Guard against a provider already sending ms (very large values).
  return n > 1e12 ? Math.round(n) : Math.round(n * 1000);
}

/**
 * Block subsidy in satoshis for a given height, from the halving schedule
 * (50 BTC halving every 210_000 blocks). Returns 0 once the subsidy rounds to
 * zero (after 33 halvings) and null for an invalid height. This is derivable
 * purely from height, so it is always available even when a provider omits it.
 */
export function blockSubsidySats(height: unknown): number | null {
  const h = heightOrNull(height);
  if (h === null) {
    return null;
  }
  const halvings = Math.floor(h / 210_000);
  if (halvings >= 64) {
    return 0;
  }
  // Integer-safe: start at 50 BTC in sats and right-shift per halving.
  const initial = 50 * SATS_PER_BTC;
  const subsidy = Math.floor(initial / Math.pow(2, halvings));
  return subsidy;
}

/** reward = subsidy + fees, when both are known; else whichever is derivable. */
export function deriveReward(subsidy: number | null, totalFees: number | null): number | null {
  if (subsidy === null && totalFees === null) {
    return null;
  }
  return (subsidy ?? 0) + (totalFees ?? 0);
}

/**
 * Deduplicate by block hash (first occurrence wins), then order newest-first by
 * height, then cap to {@link MAX_BLOCKS}. Blocks without a usable hash+height are
 * dropped. This is what keeps the retained window bounded and free of duplicate
 * or out-of-order records regardless of provider quirks.
 */
export function dedupeAndBound(blocks: readonly BlockSummary[], max: number = MAX_BLOCKS): BlockSummary[] {
  const byHash = new Map<string, BlockSummary>();
  for (const b of blocks) {
    if (!b || typeof b.hash !== 'string' || b.hash === '' || heightOrNull(b.height) === null) {
      continue;
    }
    if (!byHash.has(b.hash)) {
      byHash.set(b.hash, b);
    }
  }
  return Array.from(byHash.values())
    .sort((a, b) => b.height - a.height)
    .slice(0, Math.max(1, max));
}

/** The tip (highest block), or null for an empty list. */
export function tipOf(blocks: readonly BlockSummary[]): BlockSummary | null {
  let tip: BlockSummary | null = null;
  for (const b of blocks) {
    if (tip === null || b.height > tip.height) {
      tip = b;
    }
  }
  return tip;
}

export interface TipChange {
  /** A brand-new higher tip appeared (normal forward progress). */
  advanced: boolean;
  /**
   * The tip's hash changed at the SAME-or-lower height — a replaced/reorged
   * tip, handled honestly (Stage 15) rather than silently merged.
   */
  replaced: boolean;
  /** The new tip height, or null. */
  newHeight: number | null;
  /** The previous tip height, or null. */
  prevHeight: number | null;
}

/**
 * Compare the previous and next tips. `advanced` is ordinary forward progress;
 * `replaced` flags a tip whose hash changed without the height increasing (or
 * whose height went backwards) — a refreshed/replaced recent tip that must be
 * surfaced, never silently merged.
 */
export function detectTipChange(
  prev: readonly BlockSummary[] | null,
  next: readonly BlockSummary[],
): TipChange {
  const prevTip = prev ? tipOf(prev) : null;
  const nextTip = tipOf(next);
  if (!nextTip) {
    return { advanced: false, replaced: false, newHeight: null, prevHeight: prevTip?.height ?? null };
  }
  if (!prevTip) {
    return { advanced: false, replaced: false, newHeight: nextTip.height, prevHeight: null };
  }
  if (nextTip.height > prevTip.height) {
    return { advanced: true, replaced: false, newHeight: nextTip.height, prevHeight: prevTip.height };
  }
  // Same or lower height but a different hash → a replaced tip.
  if (nextTip.height <= prevTip.height && nextTip.hash !== prevTip.hash) {
    return { advanced: false, replaced: true, newHeight: nextTip.height, prevHeight: prevTip.height };
  }
  return { advanced: false, replaced: false, newHeight: nextTip.height, prevHeight: prevTip.height };
}

/** Age of a block in ms relative to `now`, floored at 0; null when invalid. */
export function blockAgeMs(timestampMs: number | null | undefined, now: number): number | null {
  const t = finiteOrNull(timestampMs);
  if (t === null) {
    return null;
  }
  return Math.max(0, now - t);
}
