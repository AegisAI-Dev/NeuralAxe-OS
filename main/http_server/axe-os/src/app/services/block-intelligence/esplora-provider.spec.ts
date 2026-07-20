import {
  ESPLORA_PROVIDER,
  normalizeEsploraBlock,
  normalizeEsploraBlocks,
  normalizeEsploraDetail,
} from './esplora-provider';
import { ESPLORA_BLOCKS, mkEsploraBlock, MALFORMED_NOT_ARRAY } from './block-fixtures';

const NOW = 1_733_000_000_000;
const ID = ESPLORA_PROVIDER.id;

describe('esplora-provider', () => {
  it('normalizes blocks with honest Unattributed attribution (no pool data)', () => {
    const out = normalizeEsploraBlocks(ESPLORA_BLOCKS, ID, NOW);
    expect(out.length).toBe(3);
    out.forEach(b => {
      expect(b.attribution.confidence).toBe('unattributed');
      expect(b.attribution.poolName).toBeNull();
      expect(b.totalFees).toBeNull(); // esplora block list has no fee total
    });
  });
  it('derives subsidy and a subsidy-only reward from height', () => {
    const b = normalizeEsploraBlock(mkEsploraBlock({ height: 840000 }), ID, NOW)!;
    expect(b.subsidy).toBe(312_500_000);
    expect(b.reward).toBe(312_500_000);
  });
  it('orders newest-first and dedupes', () => {
    const out = normalizeEsploraBlocks([
      mkEsploraBlock({ id: 'a', height: 5 }),
      mkEsploraBlock({ id: 'b', height: 7 }),
      mkEsploraBlock({ id: 'a', height: 5 }),
    ], ID, NOW);
    expect(out.map(b => b.height)).toEqual([7, 5]);
  });
  it('returns [] for malformed payloads', () => {
    expect(normalizeEsploraBlocks(MALFORMED_NOT_ARRAY, ID, NOW)).toEqual([]);
  });
  it('builds detail fields', () => {
    const d = normalizeEsploraDetail(mkEsploraBlock({ height: 800000 }), ID, NOW)!;
    expect(typeof d.difficulty).toBe('number');
    expect(d.previousBlockHash).toBeTruthy();
  });
  it('descriptor is the fallback and not attribution-capable, but LAN-reusable shape', () => {
    expect(ESPLORA_PROVIDER.attributionCapable).toBeFalse();
    expect(ESPLORA_PROVIDER.directBrowserAccess).toBeTrue();
  });
});
