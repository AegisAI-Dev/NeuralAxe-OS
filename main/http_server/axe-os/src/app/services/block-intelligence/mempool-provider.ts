/**
 * mempool.space provider adapter (Phase 2L — primary provider).
 *
 * Pure normalization of mempool.space's public REST responses into the internal
 * model. No I/O here: the service performs the bounded HTTP GET and hands the
 * parsed JSON to these functions. mempool.space is the primary provider because
 * it supplies pool attribution (`extras.pool`) and coinbase-tag evidence
 * (`extras.coinbaseSignatureAscii`), validated to be CORS-accessible from the
 * browser.
 *
 * Only the fields the UI needs are read; the raw payload is never stored.
 */

import {
  BlockDetail,
  BlockProviderDescriptor,
  BlockSummary,
} from './block-intelligence.model';
import { deriveAttribution, sanitizeTag } from './attribution';
import {
  blockAgeMs,
  blockSubsidySats,
  dedupeAndBound,
  deriveReward,
  finiteOrNull,
  heightOrNull,
  nonNegOrNull,
  unixSecondsToMs,
} from './block-normalize';

export const MEMPOOL_PROVIDER: BlockProviderDescriptor = {
  id: 'mempool.space',
  label: 'mempool.space',
  kind: 'public',
  attributionCapable: true,
  baseUrl: 'https://mempool.space',
  recentBlocksPath: '/api/v1/blocks',
  blockDetailPath: '/api/v1/block/:hash',
  directBrowserAccess: true,
};

/** A raw mempool "pool" object is only usable when it names a real pool. */
function providerPool(raw: any): { name: string | null; slug: string | null } {
  const pool = raw?.extras?.pool;
  const name = typeof pool?.name === 'string' ? pool.name.trim() : '';
  const slug = typeof pool?.slug === 'string' ? pool.slug.trim() : '';
  // mempool returns name "Unknown" / slug "unknown" when it cannot identify;
  // that is not a label — surface it as no provider pool so our own coinbase
  // matching (and the honest Unknown state) can take over.
  const unknown = name.toLowerCase() === 'unknown' || slug.toLowerCase() === 'unknown';
  return {
    name: name !== '' && !unknown ? name : null,
    slug: slug !== '' && !unknown ? slug : null,
  };
}

/** Normalize a single raw mempool block into a BlockSummary; null when unusable. */
export function normalizeMempoolBlock(raw: any, source: string, fetchedAtMs: number): BlockSummary | null {
  const height = heightOrNull(raw?.height);
  const hash = typeof raw?.id === 'string' && raw.id !== '' ? raw.id : null;
  if (height === null || hash === null) {
    return null;
  }
  const timestampMs = unixSecondsToMs(raw?.timestamp) ?? 0;
  const totalFees = nonNegOrNull(raw?.extras?.totalFees);
  const subsidy = blockSubsidySats(height);
  const reward = nonNegOrNull(raw?.extras?.reward) ?? deriveReward(subsidy, totalFees);

  const { name, slug } = providerPool(raw);
  const coinbaseTagAscii = sanitizeTag(raw?.extras?.coinbaseSignatureAscii);

  const attribution = deriveAttribution({
    providerPoolName: name,
    providerSlug: slug,
    coinbaseTagAscii,
    // The provider pool id is an internal identifier, NOT coinbase evidence, so it
    // must never make "has coinbase evidence" true. Coinbase evidence comes solely
    // from the decoded coinbase signature (coinbaseTagAscii).
    coinbaseTagId: null,
    providerMatchRate: null, // mempool "matchRate" is a template metric, not pool confidence
  }, source);

  return {
    height,
    hash,
    timestampMs,
    sourceTimestampMs: fetchedAtMs,
    txCount: nonNegOrNull(raw?.tx_count),
    size: nonNegOrNull(raw?.size),
    weight: nonNegOrNull(raw?.weight),
    totalFees,
    subsidy,
    reward,
    attribution,
    configuredMatch: 'insufficient',
    source,
  };
}

/** Normalize the `/api/v1/blocks` array into a bounded, ordered BlockSummary[]. */
export function normalizeMempoolBlocks(raw: unknown, source: string, fetchedAtMs: number): BlockSummary[] {
  if (!Array.isArray(raw)) {
    return [];
  }
  const summaries: BlockSummary[] = [];
  for (const item of raw) {
    const s = normalizeMempoolBlock(item, source, fetchedAtMs);
    if (s) {
      summaries.push(s);
    }
  }
  return dedupeAndBound(summaries);
}

/** Normalize a single `/api/v1/block/:hash` response into a BlockDetail. */
export function normalizeMempoolDetail(raw: any, source: string, fetchedAtMs: number): BlockDetail | null {
  const summary = normalizeMempoolBlock(raw, source, fetchedAtMs);
  if (!summary) {
    return null;
  }
  return {
    ...summary,
    version: finiteOrNull(raw?.version),
    merkleRoot: typeof raw?.merkle_root === 'string' ? raw.merkle_root : null,
    previousBlockHash: typeof raw?.previousblockhash === 'string' ? raw.previousblockhash : null,
    medianTimeMs: unixSecondsToMs(raw?.mediantime),
    bits: finiteOrNull(raw?.bits),
    nonce: finiteOrNull(raw?.nonce),
    difficulty: finiteOrNull(raw?.difficulty),
  };
}

/** Convenience for callers that want block age at read time. */
export function mempoolBlockAgeMs(block: BlockSummary, now: number): number | null {
  return blockAgeMs(block.timestampMs, now);
}
