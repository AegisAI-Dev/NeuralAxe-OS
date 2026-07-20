/**
 * Pure refresh / cache / backoff / freshness helpers (Phase 2L, Stage 10 & 15).
 *
 * The RxJS wiring in block-intelligence.service.ts is intentionally thin; all of
 * the timing and freshness POLICY lives here as pure, unit-tested functions so
 * the provider-friendly contract (bounded cadence, exponential backoff, honest
 * freshness, bounded cache) can be verified deterministically.
 */

import {
  BlockIntelligenceSnapshot,
  BlockSummary,
  MAX_BLOCKS,
  MAX_RESPONSE_BYTES,
  ProviderError,
  ProviderFreshness,
  ProviderStatus,
} from './block-intelligence.model';

// ---- Cadence (visibility-aware) -------------------------------------------

/** Foreground refresh cadence (~45 s, within the recommended 30–60 s band). */
export const FOREGROUND_CADENCE_MS = 45_000;
/** Background cadence when the page is hidden — deliberately much slower. */
export const BACKGROUND_CADENCE_MS = 300_000;

export function cadenceForVisibility(state: string): number {
  return state === 'visible' ? FOREGROUND_CADENCE_MS : BACKGROUND_CADENCE_MS;
}

// ---- Timeout & backoff -----------------------------------------------------

/** Per-request timeout budget; a request exceeding this is aborted. */
export const REQUEST_TIMEOUT_MS = 12_000;

/** Backoff schedule on consecutive provider failures. */
export const BACKOFF_BASE_MS = 15_000;
export const BACKOFF_MAX_MS = 300_000;

/**
 * Exponential backoff for the Nth consecutive failure (N ≥ 1):
 * base·2^(N−1), capped at BACKOFF_MAX_MS. N ≤ 0 returns the base delay.
 */
export function nextBackoffMs(consecutiveFailures: number, base: number = BACKOFF_BASE_MS, max: number = BACKOFF_MAX_MS): number {
  const n = Math.max(1, Math.floor(consecutiveFailures));
  const raw = base * Math.pow(2, n - 1);
  return Math.min(max, raw);
}

// ---- Freshness -------------------------------------------------------------

/**
 * A snapshot is "live" while its data is younger than this; older successful
 * data is shown but labelled "stale" rather than pretending to be current.
 */
export const FRESHNESS_WINDOW_MS = 90_000;

export interface FreshnessInputs {
  hasData: boolean;
  inFlight: boolean;
  fromCache: boolean;
  /** ms epoch of last successful refresh; null if never this session. */
  lastSuccessMs: number | null;
  /** ms epoch this data was fetched by its provider; null when from cache only. */
  dataSourceMs: number | null;
  consecutiveFailures: number;
  /** ms epoch of the next scheduled retry after failure; null otherwise. */
  nextRetryMs: number | null;
  providerId: string | null;
  now: number;
}

/**
 * Derive the honest freshness view. Priorities:
 *   - no data at all + in flight → loading; no data + failed → unavailable;
 *   - a failure with data still shown → retrying;
 *   - data never validated live this session (restored cache) → cached;
 *   - fresh successful data within the window → live; older → stale.
 */
export function deriveFreshness(i: FreshnessInputs): ProviderFreshness {
  const ageMs = i.dataSourceMs !== null ? Math.max(0, i.now - i.dataSourceMs) : null;
  let status: ProviderStatus;

  if (!i.hasData) {
    status = i.inFlight ? 'loading' : 'unavailable';
  } else if (i.consecutiveFailures > 0) {
    status = 'retrying';
  } else if (i.fromCache && i.lastSuccessMs === null) {
    status = 'cached';
  } else if (ageMs !== null && ageMs <= FRESHNESS_WINDOW_MS) {
    status = 'live';
  } else {
    status = 'stale';
  }

  return {
    status,
    provider: i.providerId,
    lastSuccessMs: i.lastSuccessMs,
    ageMs,
    nextRetryMs: i.consecutiveFailures > 0 ? i.nextRetryMs : null,
    inFlight: i.inFlight,
    fromCache: i.fromCache,
  };
}

// ---- Response-size guard ---------------------------------------------------

/**
 * Reject a raw response whose serialized size exceeds the byte bound. Returns a
 * ProviderError (kind 'oversized') when too large, else null. Guards against a
 * provider returning an unbounded payload.
 */
export function checkResponseSize(raw: unknown, provider: string, now: number, max: number = MAX_RESPONSE_BYTES): ProviderError | null {
  let size = 0;
  try {
    size = typeof raw === 'string' ? raw.length : JSON.stringify(raw).length;
  } catch {
    return { provider, kind: 'parse', message: 'Response could not be serialized.', atMs: now };
  }
  if (size > max) {
    return { provider, kind: 'oversized', message: `Response exceeded ${Math.round(max / 1024)} KB and was rejected.`, atMs: now };
  }
  return null;
}

// ---- Bounded cache (localStorage) -----------------------------------------

export const CACHE_KEY = 'NX_BLOCK_INTEL_CACHE';
export const CACHE_VERSION = 1;

export interface CachedSnapshot {
  version: number;
  blocks: BlockSummary[];
  providerId: string | null;
  savedAtMs: number;
}

/** Serialize a bounded snapshot for storage; blocks are capped defensively. */
export function serializeCache(blocks: readonly BlockSummary[], providerId: string | null, now: number): string {
  const payload: CachedSnapshot = {
    version: CACHE_VERSION,
    blocks: blocks.slice(0, MAX_BLOCKS),
    providerId,
    savedAtMs: now,
  };
  return JSON.stringify(payload);
}

/** Parse a stored cache, validating version and bounding length; null if invalid. */
export function deserializeCache(raw: string | null | undefined): CachedSnapshot | null {
  if (typeof raw !== 'string' || raw === '') {
    return null;
  }
  let parsed: any;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return null;
  }
  if (!parsed || parsed.version !== CACHE_VERSION || !Array.isArray(parsed.blocks)) {
    return null;
  }
  return {
    version: CACHE_VERSION,
    blocks: parsed.blocks.slice(0, MAX_BLOCKS),
    providerId: typeof parsed.providerId === 'string' ? parsed.providerId : null,
    savedAtMs: typeof parsed.savedAtMs === 'number' && isFinite(parsed.savedAtMs) ? parsed.savedAtMs : 0,
  };
}

// ---- Snapshot assembly -----------------------------------------------------

export interface EmptySnapshotOptions {
  status: ProviderStatus;
  providerId?: string | null;
  now: number;
}

/** An empty, honest snapshot (loading / unavailable), never a fabricated one. */
export function emptySnapshot(opts: EmptySnapshotOptions): BlockIntelligenceSnapshot {
  return {
    blocks: [],
    freshness: {
      status: opts.status,
      provider: opts.providerId ?? null,
      lastSuccessMs: null,
      ageMs: null,
      nextRetryMs: null,
      inFlight: opts.status === 'loading',
      fromCache: false,
    },
    lastError: null,
    tipReplaced: false,
  };
}
