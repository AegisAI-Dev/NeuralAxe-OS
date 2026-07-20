import {
  finiteOrNull,
  nonNegOrNull,
  heightOrNull,
  unixSecondsToMs,
  blockSubsidySats,
  deriveReward,
  dedupeAndBound,
  tipOf,
  detectTipChange,
  blockAgeMs,
} from './block-normalize';
import { BlockSummary, MAX_BLOCKS, SATS_PER_BTC } from './block-intelligence.model';

function block(height: number, hash: string, timestampMs = height * 1000): BlockSummary {
  return {
    height, hash, timestampMs, sourceTimestampMs: 0, txCount: null, size: null, weight: null,
    totalFees: null, subsidy: null, reward: null,
    attribution: { poolName: null, providerLabel: null, slug: null, method: 'none', confidence: 'unknown', source: 't', evidence: { coinbaseTagAscii: null, coinbaseTagId: null, providerMatchRate: null, reason: '', aliases: [] } },
    configuredMatch: 'insufficient', source: 't',
  };
}

describe('block-normalize: guards', () => {
  it('finiteOrNull', () => {
    expect(finiteOrNull(5)).toBe(5);
    expect(finiteOrNull(NaN)).toBeNull();
    expect(finiteOrNull(Infinity)).toBeNull();
    expect(finiteOrNull('5' as any)).toBeNull();
  });
  it('nonNegOrNull', () => {
    expect(nonNegOrNull(0)).toBe(0);
    expect(nonNegOrNull(-1)).toBeNull();
  });
  it('heightOrNull rejects non-integers and negatives', () => {
    expect(heightOrNull(870000)).toBe(870000);
    expect(heightOrNull(1.5)).toBeNull();
    expect(heightOrNull(-1)).toBeNull();
  });
  it('unixSecondsToMs converts seconds and passes through ms', () => {
    expect(unixSecondsToMs(1700000000)).toBe(1700000000000);
    expect(unixSecondsToMs(1700000000000)).toBe(1700000000000);
    expect(unixSecondsToMs(0)).toBeNull();
    expect(unixSecondsToMs(-1)).toBeNull();
  });
});

describe('block-normalize: blockSubsidySats', () => {
  it('genesis-era subsidy is 50 BTC', () => {
    expect(blockSubsidySats(0)).toBe(50 * SATS_PER_BTC);
    expect(blockSubsidySats(209_999)).toBe(50 * SATS_PER_BTC);
  });
  it('halves at each halving', () => {
    expect(blockSubsidySats(210_000)).toBe(25 * SATS_PER_BTC);
    expect(blockSubsidySats(420_000)).toBe(1_250_000_000); // 12.5 BTC
    expect(blockSubsidySats(630_000)).toBe(625_000_000);   // 6.25 BTC
    expect(blockSubsidySats(840_000)).toBe(312_500_000);   // 3.125 BTC
  });
  it('returns 0 after subsidy exhausts', () => {
    expect(blockSubsidySats(64 * 210_000)).toBe(0);
  });
  it('null for invalid heights', () => {
    expect(blockSubsidySats(-1)).toBeNull();
    expect(blockSubsidySats(1.5)).toBeNull();
  });
});

describe('block-normalize: deriveReward', () => {
  it('sums subsidy and fees', () => {
    expect(deriveReward(312_500_000, 2_500_000)).toBe(315_000_000);
  });
  it('handles missing fees', () => {
    expect(deriveReward(312_500_000, null)).toBe(312_500_000);
  });
  it('null when both missing', () => {
    expect(deriveReward(null, null)).toBeNull();
  });
});

describe('block-normalize: dedupeAndBound', () => {
  it('removes duplicate hashes and orders newest-first', () => {
    const out = dedupeAndBound([block(1, 'a'), block(3, 'c'), block(1, 'a'), block(2, 'b')]);
    expect(out.map(b => b.height)).toEqual([3, 2, 1]);
  });
  it('drops blocks without a usable hash/height', () => {
    const bad = { ...block(1, ''), height: NaN } as any;
    const out = dedupeAndBound([bad, block(2, 'b')]);
    expect(out.map(b => b.hash)).toEqual(['b']);
  });
  it('caps to MAX_BLOCKS', () => {
    const many = Array.from({ length: MAX_BLOCKS + 10 }, (_, i) => block(i, 'h' + i));
    expect(dedupeAndBound(many).length).toBe(MAX_BLOCKS);
  });
});

describe('block-normalize: tip / detectTipChange', () => {
  it('tipOf returns the highest block', () => {
    expect(tipOf([block(1, 'a'), block(5, 'e'), block(3, 'c')])?.height).toBe(5);
    expect(tipOf([])).toBeNull();
  });
  it('detects normal forward advance', () => {
    const change = detectTipChange([block(10, 'x')], [block(11, 'y'), block(10, 'x')]);
    expect(change.advanced).toBeTrue();
    expect(change.replaced).toBeFalse();
    expect(change.newHeight).toBe(11);
  });
  it('detects a replaced tip (same height, different hash)', () => {
    const change = detectTipChange([block(10, 'orig')], [block(10, 'replacement')]);
    expect(change.advanced).toBeFalse();
    expect(change.replaced).toBeTrue();
  });
  it('no change when the tip is identical', () => {
    const change = detectTipChange([block(10, 'x')], [block(10, 'x')]);
    expect(change.advanced).toBeFalse();
    expect(change.replaced).toBeFalse();
  });
  it('handles no previous data', () => {
    const change = detectTipChange(null, [block(10, 'x')]);
    expect(change.advanced).toBeFalse();
    expect(change.replaced).toBeFalse();
    expect(change.newHeight).toBe(10);
  });
});

describe('block-normalize: blockAgeMs', () => {
  it('computes a floored age', () => {
    expect(blockAgeMs(1000, 5000)).toBe(4000);
    expect(blockAgeMs(6000, 5000)).toBe(0); // never negative
    expect(blockAgeMs(null, 5000)).toBeNull();
  });
});
