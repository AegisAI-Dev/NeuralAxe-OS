import {
  MEMPOOL_PROVIDER,
  normalizeMempoolBlock,
  normalizeMempoolBlocks,
  normalizeMempoolDetail,
} from './mempool-provider';
import {
  BLOCK_STRONG_COINBASE,
  BLOCK_PROVIDER_REPORTED,
  BLOCK_PROBABLE_DOMAIN,
  BLOCK_ALIAS_MATCH,
  BLOCK_UNKNOWN_POOL,
  BLOCK_UNATTRIBUTED,
  BLOCK_CONFLICTING,
  BLOCK_MISSING_FIELDS,
  MEMPOOL_BLOCKS_MIXED,
  DUPLICATE_BLOCKS,
  OUT_OF_ORDER,
  MALFORMED_NOT_ARRAY,
  MALFORMED_GARBAGE,
  MALFORMED_NULLS,
  mkMempoolBlock,
} from './block-fixtures';
import { MAX_BLOCKS } from './block-intelligence.model';

const NOW = 1_733_000_000_000;
const ID = MEMPOOL_PROVIDER.id;

describe('mempool-provider: single block normalization', () => {
  it('derives PROVIDER-REPORTED attribution when the provider supplies a pool (never confirmed)', () => {
    const b = normalizeMempoolBlock(BLOCK_PROVIDER_REPORTED, ID, NOW)!;
    expect(b.attribution.poolName).toBe('Foundry USA');
    expect(b.attribution.confidence).toBe('provider-reported');
    expect(b.attribution.confidence).not.toBe('confirmed');
    expect(b.source).toBe(ID);
  });
  it('derives STRONG via a coinbase name token when the provider pool is Unknown', () => {
    const b = normalizeMempoolBlock(BLOCK_STRONG_COINBASE, ID, NOW)!;
    expect(b.attribution.confidence).toBe('strong');
    expect(b.attribution.poolName).toBe('AntPool');
  });
  it('derives PROBABLE via a coinbase domain/alias (no provider pool)', () => {
    const b = normalizeMempoolBlock(BLOCK_PROBABLE_DOMAIN, ID, NOW)!;
    expect(b.attribution.confidence).toBe('probable');
    expect(b.attribution.poolName).toBe('Foundry USA');
  });
  it('canonicalizes a provider alias label to provider-reported', () => {
    const b = normalizeMempoolBlock(BLOCK_ALIAS_MATCH, ID, NOW)!;
    expect(b.attribution.poolName).toBe('Braiins Pool');
    expect(b.attribution.confidence).toBe('provider-reported');
  });
  it('treats mempool "Unknown" pool as unknown/unattributed honestly', () => {
    expect(normalizeMempoolBlock(BLOCK_UNKNOWN_POOL, ID, NOW)!.attribution.confidence).toBe('unknown');
    expect(normalizeMempoolBlock(BLOCK_UNATTRIBUTED, ID, NOW)!.attribution.confidence).toBe('unattributed');
  });
  it('flags conflicting labels as probable', () => {
    const b = normalizeMempoolBlock(BLOCK_CONFLICTING, ID, NOW)!;
    expect(b.attribution.confidence).toBe('probable');
  });
  it('derives subsidy from height and reward from extras', () => {
    const b = normalizeMempoolBlock(BLOCK_PROVIDER_REPORTED, ID, NOW)!;
    expect(b.subsidy).toBe(312_500_000); // height ~870k → 3.125 BTC
    expect(b.reward).toBe(315_000_000);
  });
  it('tolerates missing optional fields', () => {
    const b = normalizeMempoolBlock(BLOCK_MISSING_FIELDS, ID, NOW)!;
    expect(b.txCount).toBeNull();
    expect(b.size).toBeNull();
    expect(b.weight).toBeNull();
    expect(b.attribution.confidence).toBe('unattributed');
  });
  it('returns null for a block without height or hash', () => {
    expect(normalizeMempoolBlock({ id: 'x' }, ID, NOW)).toBeNull();
    expect(normalizeMempoolBlock({ height: 1 }, ID, NOW)).toBeNull();
  });
});

describe('mempool-provider: array normalization', () => {
  it('normalizes, dedupes and orders the mixed fixture', () => {
    const out = normalizeMempoolBlocks(MEMPOOL_BLOCKS_MIXED, ID, NOW);
    expect(out.length).toBe(MEMPOOL_BLOCKS_MIXED.length);
    // newest first
    for (let i = 1; i < out.length; i++) {
      expect(out[i - 1].height).toBeGreaterThan(out[i].height);
    }
  });
  it('collapses duplicate blocks', () => {
    const out = normalizeMempoolBlocks(DUPLICATE_BLOCKS, ID, NOW);
    const hashes = out.map(b => b.hash);
    expect(new Set(hashes).size).toBe(hashes.length);
  });
  it('reorders out-of-order input newest-first', () => {
    const out = normalizeMempoolBlocks(OUT_OF_ORDER, ID, NOW);
    expect(out.map(b => b.height)).toEqual([870052, 870051, 870050]);
  });
  it('returns [] for malformed payloads', () => {
    expect(normalizeMempoolBlocks(MALFORMED_NOT_ARRAY, ID, NOW)).toEqual([]);
    expect(normalizeMempoolBlocks(MALFORMED_GARBAGE, ID, NOW)).toEqual([]);
  });
  it('skips null / garbage array entries', () => {
    expect(normalizeMempoolBlocks(MALFORMED_NULLS, ID, NOW)).toEqual([]);
  });
  it('bounds an oversized array to MAX_BLOCKS', () => {
    const many = Array.from({ length: MAX_BLOCKS + 15 }, (_, i) => mkMempoolBlock({ id: 'b' + i, height: 900000 + i }));
    expect(normalizeMempoolBlocks(many, ID, NOW).length).toBe(MAX_BLOCKS);
  });
});

describe('mempool-provider: detail', () => {
  it('adds detail fields on top of the summary', () => {
    const d = normalizeMempoolDetail(BLOCK_PROVIDER_REPORTED, ID, NOW)!;
    expect(d.merkleRoot).toBeTruthy();
    expect(d.previousBlockHash).toBeTruthy();
    expect(typeof d.difficulty).toBe('number');
    expect(d.attribution.poolName).toBe('Foundry USA');
  });
  it('returns null for an unusable block', () => {
    expect(normalizeMempoolDetail({}, ID, NOW)).toBeNull();
  });
});

describe('mempool-provider: descriptor', () => {
  it('is the attribution-capable primary provider', () => {
    expect(MEMPOOL_PROVIDER.attributionCapable).toBeTrue();
    expect(MEMPOOL_PROVIDER.directBrowserAccess).toBeTrue();
    expect(MEMPOOL_PROVIDER.kind).toBe('public');
  });
});
