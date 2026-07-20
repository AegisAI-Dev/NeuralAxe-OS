import {
  cadenceForVisibility,
  nextBackoffMs,
  deriveFreshness,
  checkResponseSize,
  serializeCache,
  deserializeCache,
  emptySnapshot,
  FOREGROUND_CADENCE_MS,
  BACKGROUND_CADENCE_MS,
  BACKOFF_BASE_MS,
  BACKOFF_MAX_MS,
  FRESHNESS_WINDOW_MS,
  FreshnessInputs,
} from './block-refresh';
import { BlockSummary, MAX_RESPONSE_BYTES } from './block-intelligence.model';

function fresh(overrides: Partial<FreshnessInputs> = {}): FreshnessInputs {
  return {
    hasData: true, inFlight: false, fromCache: false, lastSuccessMs: 1000, dataSourceMs: 1000,
    consecutiveFailures: 0, nextRetryMs: null, providerId: 'mempool.space', now: 1000, ...overrides,
  };
}

function block(height: number): BlockSummary {
  return {
    height, hash: 'h' + height, timestampMs: height, sourceTimestampMs: 0, txCount: null, size: null, weight: null,
    totalFees: null, subsidy: null, reward: null,
    attribution: { poolName: null, providerLabel: null, slug: null, method: 'none', confidence: 'unknown', source: 't', evidence: { coinbaseTagAscii: null, coinbaseTagId: null, providerMatchRate: null, reason: '', aliases: [] } },
    configuredMatch: 'insufficient', source: 't',
  };
}

describe('block-refresh: cadence', () => {
  it('is faster when visible', () => {
    expect(cadenceForVisibility('visible')).toBe(FOREGROUND_CADENCE_MS);
    expect(cadenceForVisibility('hidden')).toBe(BACKGROUND_CADENCE_MS);
    expect(FOREGROUND_CADENCE_MS).toBeLessThan(BACKGROUND_CADENCE_MS);
  });
  it('foreground cadence sits in the 30–60 s band', () => {
    expect(FOREGROUND_CADENCE_MS).toBeGreaterThanOrEqual(30000);
    expect(FOREGROUND_CADENCE_MS).toBeLessThanOrEqual(60000);
  });
});

describe('block-refresh: nextBackoffMs', () => {
  it('grows exponentially from the base', () => {
    expect(nextBackoffMs(1)).toBe(BACKOFF_BASE_MS);
    expect(nextBackoffMs(2)).toBe(BACKOFF_BASE_MS * 2);
    expect(nextBackoffMs(3)).toBe(BACKOFF_BASE_MS * 4);
  });
  it('caps at the maximum', () => {
    expect(nextBackoffMs(99)).toBe(BACKOFF_MAX_MS);
  });
  it('treats non-positive counts as the first failure', () => {
    expect(nextBackoffMs(0)).toBe(BACKOFF_BASE_MS);
  });
});

describe('block-refresh: deriveFreshness', () => {
  it('loading when no data and in flight', () => {
    expect(deriveFreshness(fresh({ hasData: false, inFlight: true, lastSuccessMs: null, dataSourceMs: null })).status).toBe('loading');
  });
  it('unavailable when no data and not in flight', () => {
    expect(deriveFreshness(fresh({ hasData: false, inFlight: false, lastSuccessMs: null, dataSourceMs: null })).status).toBe('unavailable');
  });
  it('retrying when a failure occurred but data is still shown', () => {
    const f = deriveFreshness(fresh({ consecutiveFailures: 2, nextRetryMs: 5000 }));
    expect(f.status).toBe('retrying');
    expect(f.nextRetryMs).toBe(5000);
  });
  it('cached when showing restored cache never validated this session', () => {
    expect(deriveFreshness(fresh({ fromCache: true, lastSuccessMs: null })).status).toBe('cached');
  });
  it('live within the freshness window', () => {
    expect(deriveFreshness(fresh({ now: 1000 + FRESHNESS_WINDOW_MS })).status).toBe('live');
  });
  it('stale beyond the freshness window', () => {
    expect(deriveFreshness(fresh({ now: 1000 + FRESHNESS_WINDOW_MS + 1 })).status).toBe('stale');
  });
});

describe('block-refresh: checkResponseSize', () => {
  it('accepts a normal payload', () => {
    expect(checkResponseSize([{ a: 1 }], 'p', 0)).toBeNull();
  });
  it('rejects an oversized payload', () => {
    const err = checkResponseSize('x'.repeat(MAX_RESPONSE_BYTES + 1), 'p', 0);
    expect(err?.kind).toBe('oversized');
  });
});

describe('block-refresh: cache serialize/deserialize', () => {
  it('round-trips a bounded snapshot', () => {
    const raw = serializeCache([block(2), block(1)], 'mempool.space', 5000);
    const parsed = deserializeCache(raw);
    expect(parsed?.blocks.length).toBe(2);
    expect(parsed?.providerId).toBe('mempool.space');
    expect(parsed?.savedAtMs).toBe(5000);
  });
  it('rejects malformed / wrong-version cache', () => {
    expect(deserializeCache('not json')).toBeNull();
    expect(deserializeCache(JSON.stringify({ version: 999, blocks: [] }))).toBeNull();
    expect(deserializeCache(null)).toBeNull();
    expect(deserializeCache('')).toBeNull();
  });
});

describe('block-refresh: emptySnapshot', () => {
  it('is honest and empty', () => {
    const s = emptySnapshot({ status: 'loading', now: 0 });
    expect(s.blocks).toEqual([]);
    expect(s.freshness.status).toBe('loading');
    expect(s.freshness.inFlight).toBeTrue();
    expect(s.lastError).toBeNull();
  });
});
