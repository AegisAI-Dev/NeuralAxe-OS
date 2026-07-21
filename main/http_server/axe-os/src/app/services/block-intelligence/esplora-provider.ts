/**
 * Esplora / Blockstream provider adapter (Phase 2L — fallback provider).
 *
 * Pure normalization of the Esplora REST schema (blockstream.info and any
 * self-hosted Esplora / electrs / mempool instance share it). Esplora does NOT
 * attribute pools, so every block normalizes to an honest Unattributed state —
 * never a guessed pool. This is the fallback when mempool.space is unreachable,
 * and — crucially — it is the SAME schema a future LAN/local NeuralAxe Node
 * provider would speak (Stage 12), so adding a trusted local provider later
 * reuses this adapter unchanged.
 */

import {
  BlockDetail,
  BlockProviderDescriptor,
  BlockSummary,
} from './block-intelligence.model';
import { deriveAttribution } from './attribution';
import {
  blockSubsidySats,
  dedupeAndBound,
  deriveReward,
  finiteOrNull,
  heightOrNull,
  nonNegOrNull,
  unixSecondsToMs,
} from './block-normalize';

export const ESPLORA_PROVIDER: BlockProviderDescriptor = {
  id: 'blockstream.info',
  label: 'Blockstream (Esplora)',
  kind: 'public',
  attributionCapable: false,
  baseUrl: 'https://blockstream.info',
  recentBlocksPath: '/api/blocks',
  blockDetailPath: '/api/block/:hash',
  directBrowserAccess: true,
};

/** Normalize a single raw Esplora block; null when unusable. */
export function normalizeEsploraBlock(raw: any, source: string, fetchedAtMs: number): BlockSummary | null {
  const height = heightOrNull(raw?.height);
  const hash = typeof raw?.id === 'string' && raw.id !== '' ? raw.id : null;
  if (height === null || hash === null) {
    return null;
  }
  const timestampMs = unixSecondsToMs(raw?.timestamp) ?? 0;
  const subsidy = blockSubsidySats(height);

  // Esplora carries no pool / coinbase attribution → honest Unattributed.
  const attribution = deriveAttribution({
    providerPoolName: null,
    providerSlug: null,
    coinbaseAscii: null,
    coinbaseHex: null,
    providerMatchRate: null,
  }, source);

  return {
    height,
    hash,
    timestampMs,
    sourceTimestampMs: fetchedAtMs,
    txCount: nonNegOrNull(raw?.tx_count),
    size: nonNegOrNull(raw?.size),
    weight: nonNegOrNull(raw?.weight),
    totalFees: null,             // Esplora block list carries no fee total
    subsidy,
    reward: deriveReward(subsidy, null),
    attribution,
    configuredMatch: 'insufficient',
    source,
  };
}

/** Normalize the `/api/blocks` array into a bounded, ordered BlockSummary[]. */
export function normalizeEsploraBlocks(raw: unknown, source: string, fetchedAtMs: number): BlockSummary[] {
  if (!Array.isArray(raw)) {
    return [];
  }
  const summaries: BlockSummary[] = [];
  for (const item of raw) {
    const s = normalizeEsploraBlock(item, source, fetchedAtMs);
    if (s) {
      summaries.push(s);
    }
  }
  return dedupeAndBound(summaries);
}

/** Normalize a single `/api/block/:hash` response into a BlockDetail. */
export function normalizeEsploraDetail(raw: any, source: string, fetchedAtMs: number): BlockDetail | null {
  const summary = normalizeEsploraBlock(raw, source, fetchedAtMs);
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
