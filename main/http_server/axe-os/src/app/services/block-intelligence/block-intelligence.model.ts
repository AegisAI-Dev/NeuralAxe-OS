/**
 * NeuralAxe OS — Bitcoin Block Intelligence, provider-neutral model (Phase 2L).
 *
 * The UI and every derivation depend ONLY on these internal models — never on a
 * single provider's raw JSON schema. Each provider ships an adapter that
 * normalizes its response into these shapes, so a future local Bitcoin node
 * (Esplora / electrs / Bitcoin Core RPC / NeuralAxe Node) can be added without
 * touching the workspace.
 *
 * Honesty contract, enforced throughout:
 *   - Pool attribution is heuristic public evidence. It NEVER proves that this
 *     individual NeuralAxe device produced a block. The only "found a block"
 *     evidence in this product is the device's own Stratum signal (SystemInfo
 *     blockFound), which lives entirely outside this subsystem.
 *   - Unknown / Unattributed are first-class, honest states — never guessed away.
 *   - Configured-pool matching is host-only and local; wallet/worker are never
 *     used and never leave the browser.
 */

import { SanitizedCoinbase } from './coinbase-sanitize';

// ---------------------------------------------------------------------------
// Attribution
// ---------------------------------------------------------------------------

/**
 * Confidence that a block belongs to the attributed pool. Ordered strongest →
 * weakest. Public providers give EVIDENCE, not proof, so the strongest state the
 * currently-implemented public providers ever produce is `provider-reported`.
 *
 * `confirmed` is RESERVED for a future source with direct authoritative evidence
 * (an explicit pool / Stratum / node provenance contract). The public-provider
 * adapters (mempool.space, Esplora) MUST NEVER emit `confirmed`.
 */
export type AttributionConfidence =
  | 'confirmed'         // RESERVED — future authoritative source only; never emitted by public providers
  | 'provider-reported' // the block-data provider attributes this block to a pool (public evidence, not proof)
  | 'strong'            // strong coinbase-tag match: the coinbase tag clearly names a known pool
  | 'probable'          // probable alias / domain match (weaker), or downgraded conflicting evidence
  | 'unknown'           // coinbase present but pool not identified
  | 'unattributed';     // no coinbase / evidence available at all

/** How the attribution was reached. */
export type AttributionMethod =
  | 'provider-pool'   // provider returned a pool object
  | 'coinbase-tag'    // matched a coinbase ASCII/hex tag locally
  | 'alias-domain'    // matched via a known alias/domain
  | 'none';           // nothing to go on

/** Bounded, sanitized evidence behind an attribution. No unbounded raw payload. */
export interface AttributionEvidence {
  /** Human-readable coinbase tag (ASCII), bounded length; null when absent. */
  coinbaseTagAscii: string | null;
  /** Coinbase tag identifier (e.g. bounded hex prefix); null when absent. */
  coinbaseTagId: string | null;
  /** Provider's own confidence/match rate in [0,1]; null when not supplied. */
  providerMatchRate: number | null;
  /** Short explanation of why this confidence was chosen. */
  reason: string;
  /** Alias identities considered while matching (bounded, deduplicated). */
  aliases: string[];
  /**
   * Bounded, sanitized coinbase evidence for safe display (readable / escaped /
   * optional hex). Optional so lightweight test constructions can omit it; the
   * adapters always populate it. See {@link SanitizedCoinbase}.
   */
  coinbase?: SanitizedCoinbase | null;
}

export interface PoolAttribution {
  /** Normalized pool name; null for Unknown / Unattributed. */
  poolName: string | null;
  /** The provider's original, untouched label; null when none. */
  providerLabel: string | null;
  /** Provider slug/id when available; null otherwise. */
  slug: string | null;
  method: AttributionMethod;
  confidence: AttributionConfidence;
  /** Source provider id, e.g. 'mempool.space'. */
  source: string;
  evidence: AttributionEvidence;
}

/** Whether an attributed block matches the device's configured pool(s). */
export type ConfiguredPoolMatch =
  | 'active'         // matches the configured active pool
  | 'fallback'       // matches the configured fallback pool
  | 'both'           // ambiguous — matched active AND fallback aliases
  | 'none'           // identified pool, but no configured-pool match
  | 'insufficient';  // not enough evidence (Unknown/Unattributed) to decide

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

export interface BlockSummary {
  height: number;
  hash: string;
  /** Block time (UTC), milliseconds since epoch. */
  timestampMs: number;
  /** When the snapshot carrying this block was fetched (ms since epoch). */
  sourceTimestampMs: number;
  txCount: number | null;
  /** Serialized size in bytes. */
  size: number | null;
  /** Weight units. */
  weight: number | null;
  /** Total fees in satoshis. */
  totalFees: number | null;
  /** Block subsidy in satoshis (derived from height when the provider omits it). */
  subsidy: number | null;
  /** Total reward (subsidy + fees) in satoshis when derivable. */
  reward: number | null;
  attribution: PoolAttribution;
  /** Filled by the local configured-pool matcher; 'insufficient' until matched. */
  configuredMatch: ConfiguredPoolMatch;
  /** Source provider id. */
  source: string;
}

/** A block's full inspectable detail — a superset of the summary. */
export interface BlockDetail extends BlockSummary {
  version: number | null;
  merkleRoot: string | null;
  previousBlockHash: string | null;
  medianTimeMs: number | null;
  bits: number | null;
  nonce: number | null;
  difficulty: number | null;
}

// ---------------------------------------------------------------------------
// Provider freshness / errors / snapshot
// ---------------------------------------------------------------------------

export type ProviderStatus =
  | 'loading'      // first fetch in flight, no data yet
  | 'live'         // fresh successful fetch within the freshness window
  | 'stale'        // last success is older than the freshness window
  | 'cached'       // showing a cache restored from storage (never re-validated this session)
  | 'retrying'     // a fetch failed; a retry is scheduled (data may still be shown)
  | 'unavailable'; // no usable data (fetch failed and no cache)

export interface ProviderFreshness {
  status: ProviderStatus;
  /** Provider id currently in effect (primary/fallback/cache), or null. */
  provider: string | null;
  /** ms epoch of the last successful refresh; null if never succeeded. */
  lastSuccessMs: number | null;
  /** Age of the shown snapshot in ms; null when unknown. */
  ageMs: number | null;
  /** ms epoch of the next scheduled retry after a failure; null otherwise. */
  nextRetryMs: number | null;
  /** True while a request is in flight. */
  inFlight: boolean;
  /** True when the shown data came from a persisted cache, not a live fetch. */
  fromCache: boolean;
}

export type ProviderErrorKind =
  | 'network'   // connection failed / offline / CORS rejection
  | 'http'      // non-2xx response
  | 'timeout'   // request exceeded the timeout budget
  | 'parse'     // response was not the expected shape
  | 'empty'     // response parsed but contained no blocks
  | 'oversized';// response exceeded the size bound

export interface ProviderError {
  provider: string;
  kind: ProviderErrorKind;
  /** Short, non-sensitive message safe to display. */
  message: string;
  atMs: number;
}

export interface BlockIntelligenceSnapshot {
  /** Recent blocks, newest first, bounded in length. */
  blocks: BlockSummary[];
  freshness: ProviderFreshness;
  /** The most recent provider error, or null. */
  lastError: ProviderError | null;
  /**
   * True when the tip (latest height/hash) changed vs the previous snapshot in
   * a way that replaced records — a refreshed/replaced tip (see Stage 15).
   */
  tipReplaced: boolean;
}

// ---------------------------------------------------------------------------
// Provider descriptor / adapter
// ---------------------------------------------------------------------------

export type ProviderKind = 'public' | 'lan' | 'local' | 'cache';

/**
 * Static descriptor of a provider. The concrete HTTP fetching lives in the
 * service; adapters supply pure `normalize*` functions plus this descriptor so
 * providers are declarative and testable without any network.
 */
export interface BlockProviderDescriptor {
  readonly id: string;               // 'mempool.space'
  readonly label: string;            // 'mempool.space'
  readonly kind: ProviderKind;
  readonly attributionCapable: boolean;
  /** Absolute base URL for public providers; relative for a future LAN/local one. */
  readonly baseUrl: string;
  /** Endpoint (relative to baseUrl) returning recent blocks. */
  readonly recentBlocksPath: string;
  /** Endpoint template for a single block's detail; `:hash` is substituted. */
  readonly blockDetailPath: string;
  /** Whether frontend-direct HTTPS is known to work (CORS validated). */
  readonly directBrowserAccess: boolean;
}

/**
 * Future provider priority (Stage 12). Represented as data now; only the public
 * tier is implemented in Phase 2L. A local/LAN provider added later takes
 * precedence without any UI change.
 */
export const PROVIDER_PRIORITY: readonly ProviderKind[] = [
  'local', // local configured provider (Bitcoin Core RPC / NeuralAxe Node) — future
  'lan',   // trusted LAN provider (self-hosted Esplora/electrs/mempool) — future
  'public',// public provider fallback — implemented now
  'cache', // cached snapshot
];

// ---------------------------------------------------------------------------
// Bounds (memory-safety; never store unbounded provider payloads)
// ---------------------------------------------------------------------------

/** Maximum recent blocks retained in a snapshot (Stage 5: ~10–20). */
export const MAX_BLOCKS = 20;

/** Maximum characters retained for any coinbase tag string. */
export const MAX_COINBASE_TAG_CHARS = 80;

/** Maximum raw provider response bytes accepted before rejecting as oversized. */
export const MAX_RESPONSE_BYTES = 512 * 1024;

/** Satoshis per whole bitcoin. */
export const SATS_PER_BTC = 100_000_000;
