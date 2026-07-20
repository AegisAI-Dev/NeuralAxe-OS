/**
 * Privacy contract tests (Phase 2L, Stage 11 & 14 #20).
 *
 * Proves that sensitive device values (wallet, worker, SSID, Wi-Fi password,
 * hostname, IP) never enter any normalized block output or matching result.
 * Only configured stratum HOSTS are ever used, and matching happens locally.
 */

import { normalizeMempoolBlocks } from './mempool-provider';
import { applyConfiguredMatches, normalizePoolHost, matchAttributionToConfig } from './pool-normalize';
import { MEMPOOL_PROVIDER } from './mempool-provider';
import {
  MEMPOOL_BLOCKS_MIXED,
  BLOCK_ACTIVE_MATCH,
  BLOCK_FALLBACK_MATCH,
  PRIVACY_SENSITIVE,
  PRIVACY_FORBIDDEN_STRINGS,
} from './block-fixtures';

const NOW = 1_733_000_000_000;

describe('block-intelligence privacy contract', () => {
  it('normalized blocks never contain any sensitive identifier', () => {
    const blocks = normalizeMempoolBlocks(MEMPOOL_BLOCKS_MIXED, MEMPOOL_PROVIDER.id, NOW);
    const config = {
      activeHost: normalizePoolHost(PRIVACY_SENSITIVE.activeUrl),
      fallbackHost: normalizePoolHost(PRIVACY_SENSITIVE.fallbackUrl),
    };
    const matched = applyConfiguredMatches(blocks, config);
    const serialized = JSON.stringify(matched);
    for (const secret of PRIVACY_FORBIDDEN_STRINGS) {
      expect(serialized).not.toContain(secret);
    }
  });

  it('matching uses only the host derived from a stratum URL (never the username)', () => {
    // The username is NEVER a parameter — deriving the host drops everything after '@'
    expect(normalizePoolHost(PRIVACY_SENSITIVE.activeUrl)).toBe('public-pool.io');
    expect(normalizePoolHost(PRIVACY_SENSITIVE.fallbackUrl)).toBe('solo.ckpool.org');
  });

  it('still matches configured pools correctly from hosts alone', () => {
    const config = {
      activeHost: normalizePoolHost(PRIVACY_SENSITIVE.activeUrl),
      fallbackHost: normalizePoolHost(PRIVACY_SENSITIVE.fallbackUrl),
    };
    const active = normalizeMempoolBlocks([BLOCK_ACTIVE_MATCH], MEMPOOL_PROVIDER.id, NOW)[0];
    const fallback = normalizeMempoolBlocks([BLOCK_FALLBACK_MATCH], MEMPOOL_PROVIDER.id, NOW)[0];
    expect(matchAttributionToConfig(active.attribution, config).match).toBe('active');
    expect(matchAttributionToConfig(fallback.attribution, config).match).toBe('fallback');
  });

  it('a match result never serializes a sensitive value', () => {
    const config = {
      activeHost: normalizePoolHost(PRIVACY_SENSITIVE.activeUrl),
      fallbackHost: normalizePoolHost(PRIVACY_SENSITIVE.fallbackUrl),
    };
    const active = normalizeMempoolBlocks([BLOCK_ACTIVE_MATCH], MEMPOOL_PROVIDER.id, NOW)[0];
    const result = matchAttributionToConfig(active.attribution, config);
    const serialized = JSON.stringify(result);
    for (const secret of PRIVACY_FORBIDDEN_STRINGS) {
      expect(serialized).not.toContain(secret);
    }
  });
});
