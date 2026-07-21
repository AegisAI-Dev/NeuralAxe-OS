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
  PoolAttribution,
} from './block-intelligence.model';
import { identifyPoolFromAttribution, identifyPoolWithKind, PoolIdentity } from './pool-normalize';
import { sanitizeCoinbase, SanitizedCoinbase } from './coinbase-sanitize';

/** Normalized inputs an adapter extracts from a provider's raw block. */
export interface AttributionInput {
  /** Provider-supplied pool display name (raw label), or null. */
  providerPoolName: string | null;
  /** Provider-supplied slug/id, or null. */
  providerSlug: string | null;
  /** Provider's LOSSY coinbase ASCII (may contain binary/replacement noise), or null. */
  coinbaseAscii: string | null;
  /** Provider's raw coinbase scriptsig hex (source of truth for evidence), or null. */
  coinbaseHex: string | null;
  /** Provider's own match confidence in [0,1], or null. */
  providerMatchRate: number | null;
}

/**
 * Bound and sanitize a free-text coinbase tag to a readable ASCII string.
 * Delegates to {@link sanitizeCoinbase} so a single, tested rule governs how
 * non-printable / replacement-character noise is stripped. Returns null when no
 * readable content remains.
 */
export function sanitizeTag(value: string | null | undefined): string | null {
  const c = sanitizeCoinbase({ ascii: value ?? null, hex: null });
  return c.hasReadable ? c.readable : null;
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
  const providerMatchRate = finiteRate(input.providerMatchRate);

  // Sanitize the coinbase ONCE (prefers raw hex bytes over lossy ASCII). The
  // readable form is the ONLY thing matching ever sees — the escaped/hex display
  // views can never create a stronger match, and no raw binary is exposed.
  const coinbase = sanitizeCoinbase({ ascii: input.coinbaseAscii, hex: input.coinbaseHex });
  const readable = coinbase.hasReadable ? coinbase.readable : null;

  // How the coinbase tag matches a known pool (name = strong, domain = probable).
  const coinbaseMatch = readable ? identifyPoolWithKind({ coinbaseTag: readable }) : null;
  // Which known pool does the provider label point at (for canonicalization)?
  const labelIdentity = (providerLabel || slug)
    ? identifyPoolFromAttribution({ poolName: providerLabel, slug })
    : null;

  const hasAnyCoinbase = coinbase.status !== 'empty';

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
    evidence: makeEvidence(readable, coinbase, providerMatchRate, reason, aliases),
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
  coinbase: SanitizedCoinbase,
  providerMatchRate: number | null,
  reason: string,
  aliases: string[],
): AttributionEvidence {
  return {
    coinbaseTagAscii,
    coinbaseTagId: null,
    providerMatchRate,
    reason,
    aliases,
    coinbase: coinbase.status === 'empty' ? null : coinbase,
  };
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
