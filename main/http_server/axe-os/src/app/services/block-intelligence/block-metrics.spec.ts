import {
  computeIntervals,
  median,
  computeBlockMetrics,
  sessionObservation,
} from './block-metrics';
import { BlockSummary, ConfiguredPoolMatch, AttributionConfidence } from './block-intelligence.model';

function block(height: number, timestampMs: number, match: ConfiguredPoolMatch = 'insufficient', conf: AttributionConfidence = 'unknown'): BlockSummary {
  return {
    height, hash: 'h' + height, timestampMs, sourceTimestampMs: 0, txCount: null, size: null, weight: null,
    totalFees: null, subsidy: null, reward: null,
    attribution: { poolName: null, providerLabel: null, slug: null, method: 'none', confidence: conf, source: 't', evidence: { coinbaseTagAscii: null, coinbaseTagId: null, providerMatchRate: null, reason: '', aliases: [] } },
    configuredMatch: match, source: 't',
  };
}

describe('block-metrics: intervals & median', () => {
  it('computes positive intervals ordered by time', () => {
    const blocks = [block(3, 3000), block(1, 1000), block(2, 1600)];
    expect(computeIntervals(blocks)).toEqual([600, 1400]);
  });
  it('ignores non-positive / invalid timestamps', () => {
    const blocks = [block(1, 1000), block(2, 1000), block(3, 0)];
    expect(computeIntervals(blocks)).toEqual([]); // 1000→1000 is 0, dropped
  });
  it('median of odd and even sets', () => {
    expect(median([3, 1, 2])).toBe(2);
    expect(median([1, 2, 3, 4])).toBe(2.5);
    expect(median([])).toBeNull();
  });
});

describe('block-metrics: computeBlockMetrics', () => {
  it('returns null interval stats for a single block', () => {
    const m = computeBlockMetrics([block(1, 1000)]);
    expect(m.averageIntervalMs).toBeNull();
    expect(m.spanMs).toBeNull();
    expect(m.blocksPerHour).toBeNull();
  });
  it('computes interval stats and blocks-per-hour over the span', () => {
    // three blocks, 10 min apart → span 20 min, 2 intervals over span → 2/(1200000ms)*3600000 = 6/h
    const blocks = [block(3, 2_400_000), block(2, 1_800_000), block(1, 1_200_000)];
    const m = computeBlockMetrics(blocks);
    expect(m.averageIntervalMs).toBe(600000);
    expect(m.medianIntervalMs).toBe(600000);
    expect(m.shortestIntervalMs).toBe(600000);
    expect(m.longestIntervalMs).toBe(600000);
    expect(m.spanMs).toBe(1200000);
    expect(m.blocksPerHour).toBeCloseTo(6, 5);
  });
  it('tallies configured matches and attribution confidence', () => {
    const blocks = [
      block(4, 4000, 'active', 'provider-reported'),
      block(3, 3000, 'fallback', 'strong'),
      block(2, 2000, 'both', 'provider-reported'),
      block(1, 1000, 'none', 'unknown'),
    ];
    const m = computeBlockMetrics(blocks);
    expect(m.activePoolBlocks).toBe(2);   // active + both
    expect(m.fallbackPoolBlocks).toBe(2); // fallback + both
    expect(m.unknownAttribution).toBe(1);
    expect(m.highConfidenceCoverage).toBeCloseTo(3 / 4, 5);
  });
});

describe('block-metrics: sessionObservation', () => {
  it('counts new network blocks since session start', () => {
    const o = sessionObservation(870000, 870005, 1000, 61000);
    expect(o.startHeight).toBe(870000);
    expect(o.networkBlocksObserved).toBe(5);
    expect(o.elapsedMs).toBe(60000);
  });
  it('never returns a negative count on a replaced/backwards tip', () => {
    expect(sessionObservation(870005, 870003, 0, 1000).networkBlocksObserved).toBe(0);
  });
  it('zero when start height is unknown', () => {
    expect(sessionObservation(null, 870005, 0, 1000).networkBlocksObserved).toBe(0);
  });
  it('elapsed never negative', () => {
    expect(sessionObservation(1, 1, 5000, 1000).elapsedMs).toBe(0);
  });
});
