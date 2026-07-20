/**
 * Pure pool normalization and configured-pool matching (Phase 2L, Stage 4).
 *
 * PRIVACY: every function here takes host / URL / label strings ONLY. Stratum
 * usernames (which carry wallet.worker) are never a parameter and must never be
 * passed in. Matching is host-only and happens locally in the browser after
 * public block data is retrieved.
 *
 * Matching is explainable: each result carries a human-readable reason and the
 * alias identities considered, and NEVER implies that a domain match proves the
 * local worker produced the block.
 */

import { BlockSummary, ConfiguredPoolMatch, PoolAttribution } from './block-intelligence.model';

// ---------------------------------------------------------------------------
// Host / URL normalization
// ---------------------------------------------------------------------------

/** Protocol prefixes that appear on configured stratum URLs. */
const PROTOCOL_RE = /^(stratum\+tcp|stratum\+ssl|stratum|tcp|ssl|tls|http|https|ws|wss):\/\//i;

/**
 * Normalize a stratum URL / hostname to a bare lowercase host:
 *   - lowercased and trimmed;
 *   - protocol removed (stratum+tcp://, tcp://, https://, …);
 *   - any user@ credentials segment removed (defensive — never expected here);
 *   - port removed;
 *   - path / query / fragment removed.
 *
 * The original value is the caller's responsibility to preserve separately;
 * this returns the normalized host only (or '' when nothing usable remains).
 */
export function normalizePoolHost(raw: unknown): string {
  if (typeof raw !== 'string') {
    return '';
  }
  let host = raw.trim().toLowerCase();
  if (host === '') {
    return '';
  }
  host = host.replace(PROTOCOL_RE, '');
  // Defensive: drop any credentials segment (user:pass@host) if present.
  const at = host.lastIndexOf('@');
  if (at !== -1) {
    host = host.slice(at + 1);
  }
  // Drop path / query / fragment.
  host = host.split('/')[0].split('?')[0].split('#')[0];
  // Drop port. IPv6 in brackets keeps its colons; strip a trailing :port only.
  if (host.startsWith('[')) {
    const close = host.indexOf(']');
    if (close !== -1) {
      host = host.slice(1, close);
    }
  } else {
    const colon = host.lastIndexOf(':');
    if (colon !== -1 && /^\d+$/.test(host.slice(colon + 1))) {
      host = host.slice(0, colon);
    }
  }
  return host.trim();
}

/** Multi-label public suffixes we must not collapse past (small, curated). */
const TWO_LEVEL_SUFFIXES = new Set([
  'co.uk', 'org.uk', 'ac.uk', 'gov.uk',
  'com.au', 'net.au', 'org.au',
  'co.jp', 'ne.jp', 'or.jp',
  'com.cn', 'net.cn', 'org.cn',
  'com.br',
]);

/**
 * Best-effort registrable domain (e.g. `eu.stratum.slushpool.com` →
 * `slushpool.com`). Not a full public-suffix implementation — it handles the
 * common two-level suffixes above and otherwise takes the last two labels.
 * IPs and single-label hosts are returned unchanged.
 */
export function registrableDomain(host: string): string {
  const h = normalizePoolHost(host);
  if (h === '' || /^\d{1,3}(\.\d{1,3}){3}$/.test(h) || h.indexOf('.') === -1) {
    return h;
  }
  const parts = h.split('.');
  const lastTwo = parts.slice(-2).join('.');
  if (TWO_LEVEL_SUFFIXES.has(lastTwo) && parts.length >= 3) {
    return parts.slice(-3).join('.');
  }
  return lastTwo;
}

// ---------------------------------------------------------------------------
// Pool identity registry (heuristic, curated, bounded)
// ---------------------------------------------------------------------------

export interface PoolIdentity {
  /** Stable canonical key. */
  key: string;
  /** Display name. */
  name: string;
  /** Registrable domains associated with this pool (for host matching). */
  domains: string[];
  /** Lowercased label / coinbase-tag tokens associated with this pool. */
  labels: string[];
}

/**
 * A small, transparent registry of pool identities. Used to map BOTH a
 * configured stratum host AND a provider's attribution label/coinbase tag to
 * the same canonical identity, which is what makes a match explainable. It is
 * intentionally conservative — an unlisted pool simply yields no match rather
 * than a guessed one.
 */
export const POOL_REGISTRY: readonly PoolIdentity[] = [
  { key: 'foundryusa', name: 'Foundry USA', domains: ['foundrydigital.com'], labels: ['foundry usa', 'foundryusa', 'foundry'] },
  { key: 'antpool', name: 'AntPool', domains: ['antpool.com'], labels: ['antpool'] },
  { key: 'f2pool', name: 'F2Pool', domains: ['f2pool.com'], labels: ['f2pool', '?f2pool'] },
  { key: 'viabtc', name: 'ViaBTC', domains: ['viabtc.com'], labels: ['viabtc'] },
  { key: 'binancepool', name: 'Binance Pool', domains: ['binance.com'], labels: ['binance', 'binance pool'] },
  { key: 'braiins', name: 'Braiins Pool', domains: ['braiins.com', 'slushpool.com'], labels: ['braiins', 'slush', 'slushpool', 'braiins pool'] },
  { key: 'luxor', name: 'Luxor', domains: ['luxor.tech'], labels: ['luxor'] },
  { key: 'sbicrypto', name: 'SBI Crypto', domains: ['sbicrypto.com'], labels: ['sbi crypto', 'sbicrypto'] },
  { key: 'marapool', name: 'MARA Pool', domains: ['marapool.com', 'mara.com'], labels: ['mara', 'mara pool', 'marapool'] },
  { key: 'spiderpool', name: 'SpiderPool', domains: ['spiderpool.com'], labels: ['spiderpool'] },
  { key: 'ocean', name: 'OCEAN', domains: ['ocean.xyz'], labels: ['ocean', 'ocean.xyz'] },
  { key: 'secpool', name: 'SECPOOL', domains: ['secpool.com'], labels: ['secpool'] },
  { key: 'carbonnegative', name: 'Carbon Negative', domains: [], labels: ['carbon negative', 'bitfufu'] },
  { key: 'publicpool', name: 'Public Pool', domains: ['public-pool.io'], labels: ['public pool', 'public-pool', 'publicpool'] },
  { key: 'ckpool-solo', name: 'Solo CKPool', domains: ['ckpool.org', 'solo.ckpool.org', 'eusolo.ckpool.org'], labels: ['solo ckpool', 'solock', 'ckpool.eu/solo.ckpool.org', 'solo.ckpool.org'] },
  { key: 'ckpool', name: 'CKPool', domains: ['ckpool.org'], labels: ['ckpool'] },
];

function normalizeLabel(value: unknown): string {
  return typeof value === 'string' ? value.trim().toLowerCase() : '';
}

/** Identify a pool from a configured stratum host (domain suffix match). */
export function identifyPoolFromHost(host: unknown): PoolIdentity | null {
  const domain = registrableDomain(typeof host === 'string' ? host : '');
  const full = normalizePoolHost(host);
  if (domain === '') {
    return null;
  }
  for (const pool of POOL_REGISTRY) {
    for (const d of pool.domains) {
      const dd = d.toLowerCase();
      if (domain === dd || full === dd || full.endsWith('.' + dd)) {
        return pool;
      }
    }
  }
  return null;
}

/** How an attribution matched a pool: by a name/label token or a domain/alias. */
export interface PoolMatch {
  pool: PoolIdentity;
  /** 'name' = a clear name/label/coinbase-tag token (strong); 'domain' = a
   *  weaker domain/alias hint (probable). */
  via: 'name' | 'domain';
}

/**
 * Identify a pool from a provider label / slug / coinbase tag, and report HOW it
 * matched. Name/label token matches (pass 1) are the stronger signal; domain/
 * alias matches (pass 2) are weaker. Matches are boundary-delimited so short
 * aliases never hit inside unrelated words (e.g. "ckpool" in "someckpoolish").
 */
export function identifyPoolWithKind(input: {
  poolName?: unknown;
  slug?: unknown;
  coinbaseTag?: unknown;
}): PoolMatch | null {
  const candidates = [normalizeLabel(input.poolName), normalizeLabel(input.slug), normalizeLabel(input.coinbaseTag)]
    .filter(c => c.length > 0);
  if (candidates.length === 0) {
    return null;
  }
  // Pass 1 — name/label token (strong evidence).
  for (const pool of POOL_REGISTRY) {
    for (const label of pool.labels) {
      const l = label.toLowerCase();
      for (const cand of candidates) {
        if (cand === l || containsToken(cand, l)) {
          return { pool, via: 'name' };
        }
      }
    }
    const slug = normalizeLabel(input.slug);
    if (slug !== '' && (slug === pool.key || slug.replace(/-/g, '') === pool.key)) {
      return { pool, via: 'name' };
    }
  }
  // Pass 2 — domain/alias token (weaker evidence).
  for (const pool of POOL_REGISTRY) {
    for (const d of pool.domains) {
      const dd = d.toLowerCase();
      for (const cand of candidates) {
        if (cand === dd || containsToken(cand, dd)) {
          return { pool, via: 'domain' };
        }
      }
    }
  }
  return null;
}

/** Identify a pool (identity only), regardless of how it matched. */
export function identifyPoolFromAttribution(input: {
  poolName?: unknown;
  slug?: unknown;
  coinbaseTag?: unknown;
}): PoolIdentity | null {
  return identifyPoolWithKind(input)?.pool ?? null;
}

/** True when `needle` appears in `haystack` as a boundary-delimited token. */
function containsToken(haystack: string, needle: string): boolean {
  if (needle.length < 3) {
    return false; // too short to be a safe token match
  }
  const idx = haystack.indexOf(needle);
  if (idx === -1) {
    return false;
  }
  const before = idx === 0 ? '' : haystack[idx - 1];
  const after = idx + needle.length >= haystack.length ? '' : haystack[idx + needle.length];
  const boundary = (ch: string) => ch === '' || !/[a-z0-9]/.test(ch);
  return boundary(before) && boundary(after);
}

// ---------------------------------------------------------------------------
// Configured-pool matching
// ---------------------------------------------------------------------------

/** The device's configured pools, host-only (never usernames). */
export interface ConfiguredPools {
  activeHost: string | null;
  fallbackHost: string | null;
}

export interface ConfiguredMatchResult {
  match: ConfiguredPoolMatch;
  /** Explainable, non-sensitive reason. */
  reason: string;
  /** Alias identities considered (canonical names), bounded. */
  aliases: string[];
}

/**
 * Match a block's attribution to the device's configured pools. Host-only,
 * heuristic and explicit:
 *   - Unknown / Unattributed → 'insufficient' (never forced into a match);
 *   - identity equals active AND fallback → 'both';
 *   - identity equals active → 'active'; equals fallback → 'fallback';
 *   - identified but no configured match → 'none'.
 *
 * A match means the attributed pool identity aligns with a configured pool's
 * domain/alias. It does NOT mean this device's worker produced the block.
 */
export function matchAttributionToConfig(
  attribution: Pick<PoolAttribution, 'poolName' | 'slug' | 'confidence' | 'evidence'>,
  config: ConfiguredPools,
): ConfiguredMatchResult {
  const blockPool = identifyPoolFromAttribution({
    poolName: attribution.poolName,
    slug: attribution.slug,
    coinbaseTag: attribution.evidence?.coinbaseTagAscii,
  });

  if (attribution.confidence === 'unknown' || attribution.confidence === 'unattributed' || !blockPool) {
    return {
      match: 'insufficient',
      reason: 'Block pool could not be identified with enough confidence to compare with configured pools.',
      aliases: [],
    };
  }

  const activePool = identifyPoolFromHost(config.activeHost);
  const fallbackPool = identifyPoolFromHost(config.fallbackHost);
  const aliases = uniq([blockPool.name, activePool?.name, fallbackPool?.name].filter((x): x is string => !!x));

  const matchesActive = !!activePool && activePool.key === blockPool.key;
  const matchesFallback = !!fallbackPool && fallbackPool.key === blockPool.key;

  if (matchesActive && matchesFallback) {
    return { match: 'both', reason: `${blockPool.name} matches both the configured active and fallback pool identities.`, aliases };
  }
  if (matchesActive) {
    return { match: 'active', reason: `${blockPool.name} matches the configured active pool domain/alias.`, aliases };
  }
  if (matchesFallback) {
    return { match: 'fallback', reason: `${blockPool.name} matches the configured fallback pool domain/alias.`, aliases };
  }
  return { match: 'none', reason: `${blockPool.name} does not match either configured pool.`, aliases };
}

/** Apply configured-pool matching across a block list, returning new objects. */
export function applyConfiguredMatches(blocks: readonly BlockSummary[], config: ConfiguredPools): BlockSummary[] {
  return blocks.map(block => ({
    ...block,
    configuredMatch: matchAttributionToConfig(block.attribution, config).match,
  }));
}

function uniq<T>(arr: T[]): T[] {
  return Array.from(new Set(arr));
}
