/**
 * Pool attribution confidence contract (Phase 2L, Stage 3).
 *
 * Pure derivation of a {@link PoolAttribution} from normalized provider signals.
 * Attribution is heuristic public evidence and is ALWAYS accompanied by its
 * method, confidence, source and reason. Unknown and Unattributed are honest,
 * first-class outcomes — the derivation never invents a pool.
 *
 * The five confidence states:
 *   confirmed     provider pool + corroborating coinbase / high provider match
 *   strong        strong coinbase-tag match names a known pool
 *   probable      provider/alias label only, no corroborating coinbase evidence
 *   unknown       coinbase present but no pool identified
 *   unattributed  no coinbase / evidence available at all
 */

import {
  AttributionConfidence,
  AttributionEvidence,
  AttributionMethod,
  MAX_COINBASE_TAG_CHARS,
  PoolAttribution,
} from './block-intelligence.model';
import { identifyPoolFromAttribution, identifyPoolWithKind, PoolIdentity } from './pool-normalize';

/** Normalized inputs an adapter extracts from a provider's raw block. */
export interface AttributionInput {
  /** Provider-supplied pool display name (raw label), or null. */
  providerPoolName: string | null;
  /** Provider-supplied slug/id, or null. */
  providerSlug: string | null;
  /** Decoded coinbase ASCII tag, or null. */
  coinbaseTagAscii: string | null;
  /** Coinbase tag identifier (bounded hex prefix), or null. */
  coinbaseTagId: string | null;
  /** Provider's own match confidence in [0,1], or null. */
  providerMatchRate: number | null;
}

/** Bound and sanitize a free-text tag for storage/display. */
export function sanitizeTag(value: string | null | undefined): string | null {
  if (typeof value !== 'string') {
    return null;
  }
  // Strip control chars, collapse whitespace, bound length.
  const cleaned = value.replace(/[\x00-\x1F\x7F]/g, ' ').replace(/\s+/g, ' ').trim();
  if (cleaned === '') {
    return null;
  }
  return cleaned.length > MAX_COINBASE_TAG_CHARS ? cleaned.slice(0, MAX_COINBASE_TAG_CHARS) + '…' : cleaned;
}

function finiteRate(value: unknown): number | null {
  if (typeof value !== 'number' || !isFinite(value)) {
    return null;
  }
  if (value < 0) return 0;
  if (value > 1) return 1;
  return value;
}

/**
 * Derive a complete, self-describing attribution. `source` is the provider id.
 */
export function deriveAttribution(input: AttributionInput, source: string): PoolAttribution {
  const providerLabel = nonEmpty(input.providerPoolName);
  const slug = nonEmpty(input.providerSlug);
  const coinbaseTagAscii = sanitizeTag(input.coinbaseTagAscii);
  const coinbaseTagId = nonEmpty(input.coinbaseTagId);
  const providerMatchRate = finiteRate(input.providerMatchRate);

  // How the coinbase tag matches a known pool (name = strong, domain = probable).
  const coinbaseMatch = coinbaseTagAscii
    ? identifyPoolWithKind({ coinbaseTag: coinbaseTagAscii })
    : null;
  // Which known pool does the provider label point at (for canonicalization)?
  const labelIdentity = (providerLabel || slug)
    ? identifyPoolFromAttribution({ poolName: providerLabel, slug })
    : null;

  const hasAnyCoinbase = coinbaseTagAscii !== null || coinbaseTagId !== null;

  const base = (
    confidence: AttributionConfidence,
    method: AttributionMethod,
    poolName: string | null,
    reason: string,
    aliases: string[] = [],
  ): PoolAttribution => ({
    poolName,
    providerLabel,
    slug,
    method,
    confidence,
    source,
    evidence: makeEvidence(coinbaseTagAscii, coinbaseTagId, providerMatchRate, reason, aliases),
  });

  // ---- Provider supplied a pool → PROVIDER-REPORTED ----------------------
  // The provider's own attribution is public evidence, not proof. The strongest
  // state a public provider ever yields is `provider-reported` — never
  // `confirmed` (which is reserved for a future authoritative provenance source).
  if (providerLabel) {
    const canonicalName = labelIdentity?.name ?? providerLabel;

    // Conflict: coinbase tag names a DIFFERENT known pool → downgrade to probable.
    if (coinbaseMatch && labelIdentity && coinbaseMatch.pool.key !== labelIdentity.key) {
      return base(
        'probable',
        'provider-pool',
        canonicalName,
        `Provider attributes this block to ${canonicalName}, but the coinbase tag resembles ${coinbaseMatch.pool.name} — treated as probable due to conflicting evidence.`,
        aliasNames(labelIdentity, coinbaseMatch.pool),
      );
    }

    const corroborated = !!coinbaseMatch && !!labelIdentity && coinbaseMatch.pool.key === labelIdentity.key;
    return base(
      'provider-reported',
      'provider-pool',
      canonicalName,
      corroborated
        ? `Provider attributes this block to ${canonicalName} (corroborated by the coinbase tag). Public evidence, not proof.`
        : `Provider attributes this block to ${canonicalName}. Public evidence, not proof.`,
      aliasNames(labelIdentity),
    );
  }

  // ---- No provider pool, but the coinbase tag identifies a known pool ----
  if (coinbaseMatch) {
    if (coinbaseMatch.via === 'name') {
      return base(
        'strong',
        'coinbase-tag',
        coinbaseMatch.pool.name,
        `Strong coinbase-tag match: ${coinbaseMatch.pool.name}. No provider pool label was supplied.`,
        aliasNames(coinbaseMatch.pool),
      );
    }
    return base(
      'probable',
      'alias-domain',
      coinbaseMatch.pool.name,
      `Likely ${coinbaseMatch.pool.name} based on coinbase alias/domain evidence.`,
      aliasNames(coinbaseMatch.pool),
    );
  }

  // ---- Coinbase present but nothing identifiable → Unknown --------------
  if (hasAnyCoinbase) {
    return base(
      'unknown',
      'none',
      null,
      'Coinbase evidence is present but does not match any known pool.',
    );
  }

  // ---- Nothing to go on → Unattributed ---------------------------------
  return base(
    'unattributed',
    'none',
    null,
    'No coinbase or pool evidence was available from this provider for this block.',
  );
}

function makeEvidence(
  coinbaseTagAscii: string | null,
  coinbaseTagId: string | null,
  providerMatchRate: number | null,
  reason: string,
  aliases: string[],
): AttributionEvidence {
  return { coinbaseTagAscii, coinbaseTagId, providerMatchRate, reason, aliases };
}

function nonEmpty(value: string | null | undefined): string | null {
  if (typeof value !== 'string') {
    return null;
  }
  const t = value.trim();
  return t === '' ? null : t;
}

function aliasNames(...ids: (PoolIdentity | null | undefined)[]): string[] {
  return Array.from(new Set(ids.filter((x): x is PoolIdentity => !!x).map(i => i.name)));
}

// ---------------------------------------------------------------------------
// Display helpers (accent-independent; label + severity, never color-only)
// ---------------------------------------------------------------------------

/** Fixed, human-readable label for a confidence level. */
export function confidenceLabel(confidence: AttributionConfidence): string {
  switch (confidence) {
    case 'confirmed': return 'Confirmed';               // reserved (future authoritative source)
    case 'provider-reported': return 'Provider-reported';
    case 'strong': return 'Strong coinbase match';
    case 'probable': return 'Probable';
    case 'unknown': return 'Unknown pool';
    case 'unattributed': return 'Unattributed';
  }
}

/**
 * Semantic severity for a confidence level — accent-independent. Attribution
 * strength is neutral technical information, never an alarm: provider-reported /
 * strong evidence is 'ok', weaker evidence 'info', and the absence of evidence
 * 'neutral'. Nothing here is ever 'danger' — an unattributed block is not an error.
 */
export function confidenceSeverity(confidence: AttributionConfidence): 'ok' | 'info' | 'neutral' {
  switch (confidence) {
    case 'confirmed':
    case 'provider-reported':
    case 'strong':
      return 'ok';
    case 'probable':
      return 'info';
    case 'unknown':
    case 'unattributed':
      return 'neutral';
  }
}

/** A short, honest phrase for how a block relates to configured pools. */
export function configuredMatchLabel(match: string): string {
  switch (match) {
    case 'active': return 'Active pool match';
    case 'fallback': return 'Fallback pool match';
    case 'both': return 'Active & fallback match';
    case 'none': return 'No configured-pool match';
    default: return 'Insufficient evidence';
  }
}
