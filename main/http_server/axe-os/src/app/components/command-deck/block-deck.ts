/**
 * Command Deck compact Block Intelligence glance (Phase 2L, Stage 7).
 *
 * Pure derivation of the small deck card from a shared snapshot. It never
 * duplicates the full block table and never implies the local device found a
 * block — it summarizes the latest NETWORK block and, only when notable, whether
 * it aligned with a configured pool.
 */

import {
  AttributionConfidence,
  BlockIntelligenceSnapshot,
  ConfiguredPoolMatch,
  ProviderStatus,
} from 'src/app/services/block-intelligence/block-intelligence.model';
import { tipOf, blockAgeMs } from 'src/app/services/block-intelligence/block-normalize';
import { computeBlockMetrics } from 'src/app/services/block-intelligence/block-metrics';
import { confidenceLabel, configuredMatchLabel } from 'src/app/services/block-intelligence/attribution';

export interface BlockDeckGlance {
  hasData: boolean;
  height: number | null;
  ageMs: number | null;
  /** Pool name, or an honest 'Unknown pool' / 'Unattributed'. */
  poolLabel: string;
  confidence: AttributionConfidence | null;
  confidenceText: string;
  /** Only set when the block matched a configured pool (active/fallback/both). */
  matchLabel: string | null;
  matchKind: ConfiguredPoolMatch | null;
  avgIntervalMs: number | null;
  freshnessStatus: ProviderStatus;
  freshnessText: string;
  provider: string | null;
}

/** Fixed, honest label for a provider freshness status. */
export function freshnessLabel(status: ProviderStatus): string {
  switch (status) {
    case 'loading': return 'Loading…';
    case 'live': return 'Live';
    case 'stale': return 'Stale';
    case 'cached': return 'Cached';
    case 'retrying': return 'Retrying';
    case 'unavailable': return 'Provider unavailable';
  }
}

/**
 * Semantic severity for a freshness status — accent-independent. A provider
 * being unavailable is NOT a device error, so it is never 'danger'/red; the
 * strongest it gets is 'warn'.
 */
export function freshnessSeverity(status: ProviderStatus): 'ok' | 'info' | 'warn' | 'neutral' {
  switch (status) {
    case 'live': return 'ok';
    case 'cached': return 'info';
    case 'loading': return 'neutral';
    case 'stale':
    case 'retrying':
    case 'unavailable': return 'warn';
  }
}

/** Human pool label from a block's attribution, honest about Unknown states. */
function poolLabelFor(confidence: AttributionConfidence, poolName: string | null): string {
  if (poolName) {
    return poolName;
  }
  return confidence === 'unattributed' ? 'Unattributed' : 'Unknown pool';
}

export function blockDeckGlance(snapshot: BlockIntelligenceSnapshot | null, now: number): BlockDeckGlance {
  const status = snapshot?.freshness.status ?? 'loading';
  const base: BlockDeckGlance = {
    hasData: false,
    height: null,
    ageMs: null,
    poolLabel: '—',
    confidence: null,
    confidenceText: '',
    matchLabel: null,
    matchKind: null,
    avgIntervalMs: null,
    freshnessStatus: status,
    freshnessText: freshnessLabel(status),
    provider: snapshot?.freshness.provider ?? null,
  };
  if (!snapshot || snapshot.blocks.length === 0) {
    return base;
  }
  const tip = tipOf(snapshot.blocks);
  if (!tip) {
    return base;
  }
  const metrics = computeBlockMetrics(snapshot.blocks);
  const match = tip.configuredMatch;
  const notableMatch = match === 'active' || match === 'fallback' || match === 'both';
  return {
    ...base,
    hasData: true,
    height: tip.height,
    ageMs: blockAgeMs(tip.timestampMs, now),
    poolLabel: poolLabelFor(tip.attribution.confidence, tip.attribution.poolName),
    confidence: tip.attribution.confidence,
    confidenceText: confidenceLabel(tip.attribution.confidence),
    matchLabel: notableMatch ? configuredMatchLabel(match) : null,
    matchKind: notableMatch ? match : null,
    avgIntervalMs: metrics.averageIntervalMs,
    provider: snapshot.freshness.provider,
  };
}
