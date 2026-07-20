import { blockDeckGlance, freshnessLabel, freshnessSeverity } from './block-deck';
import {
  BlockIntelligenceSnapshot,
  BlockSummary,
  ConfiguredPoolMatch,
  AttributionConfidence,
  ProviderStatus,
} from 'src/app/services/block-intelligence/block-intelligence.model';

function block(height: number, timestampMs: number, match: ConfiguredPoolMatch, conf: AttributionConfidence, poolName: string | null): BlockSummary {
  return {
    height, hash: 'h' + height, timestampMs, sourceTimestampMs: timestampMs, txCount: 3000, size: 1_500_000, weight: 3_990_000,
    totalFees: 2_500_000, subsidy: 312_500_000, reward: 315_000_000,
    attribution: { poolName, providerLabel: poolName, slug: null, method: 'provider-pool', confidence: conf, source: 'mempool.space', evidence: { coinbaseTagAscii: null, coinbaseTagId: null, providerMatchRate: null, reason: '', aliases: [] } },
    configuredMatch: match, source: 'mempool.space',
  };
}

function snap(blocks: BlockSummary[], status: ProviderStatus = 'live'): BlockIntelligenceSnapshot {
  return {
    blocks,
    freshness: { status, provider: 'mempool.space', lastSuccessMs: 1000, ageMs: 0, nextRetryMs: null, inFlight: false, fromCache: false },
    lastError: null, tipReplaced: false,
  };
}

describe('block-deck: blockDeckGlance', () => {
  it('is honest and empty for a null / empty snapshot', () => {
    const g = blockDeckGlance(null, 1000);
    expect(g.hasData).toBeFalse();
    expect(g.height).toBeNull();
    expect(g.freshnessStatus).toBe('loading');
  });

  it('summarizes the latest block with pool + confidence', () => {
    const now = 1_733_000_000_000;
    const g = blockDeckGlance(snap([
      block(870010, now - 600000, 'active', 'provider-reported', 'Public Pool'),
      block(870009, now - 1200000, 'none', 'strong', 'Foundry USA'),
    ]), now);
    expect(g.hasData).toBeTrue();
    expect(g.height).toBe(870010);
    expect(g.poolLabel).toBe('Public Pool');
    expect(g.confidence).toBe('provider-reported');
    expect(g.matchLabel).toBe('Active pool match'); // notable match surfaced
    expect(g.avgIntervalMs).toBe(600000);
  });

  it('only surfaces a notable configured match', () => {
    const now = 1_000_000;
    const g = blockDeckGlance(snap([block(870010, now - 600000, 'none', 'strong', 'Foundry USA')]), now);
    expect(g.matchLabel).toBeNull();
    expect(g.matchKind).toBeNull();
  });

  it('renders Unknown / Unattributed honestly', () => {
    const now = 1_000_000;
    expect(blockDeckGlance(snap([block(1, now, 'insufficient', 'unknown', null)]), now).poolLabel).toBe('Unknown pool');
    expect(blockDeckGlance(snap([block(1, now, 'insufficient', 'unattributed', null)]), now).poolLabel).toBe('Unattributed');
  });
});

describe('block-deck: freshness display', () => {
  it('labels every status', () => {
    (['loading', 'live', 'stale', 'cached', 'retrying', 'unavailable'] as ProviderStatus[]).forEach(s => {
      expect(typeof freshnessLabel(s)).toBe('string');
    });
    expect(freshnessLabel('unavailable')).toBe('Provider unavailable');
  });
  it('never marks a provider outage as danger/red', () => {
    (['loading', 'live', 'stale', 'cached', 'retrying', 'unavailable'] as ProviderStatus[]).forEach(s => {
      expect(['ok', 'info', 'warn', 'neutral']).toContain(freshnessSeverity(s));
    });
    expect(freshnessSeverity('live')).toBe('ok');
    expect(freshnessSeverity('unavailable')).toBe('warn');
  });
});
