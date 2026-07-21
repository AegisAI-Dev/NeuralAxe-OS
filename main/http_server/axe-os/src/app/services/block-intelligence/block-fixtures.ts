/**
 * Deterministic, sanitized Block Intelligence fixtures (Phase 2L, Stage 14).
 *
 * These drive the unit tests and the deterministic screenshot harness. They
 * contain NO real user identifiers. The privacy fixture deliberately embeds
 * wallet / worker / SSID-shaped values to PROVE they never leave the local
 * matching layer — tests assert those strings never appear in any normalized
 * output or outbound request representation.
 *
 * Raw shapes mirror the real provider schemas (mempool.space v1 / Esplora) so
 * the adapters are exercised against realistic payloads without any network.
 */

// A fixed reference "now" so age-based assertions are deterministic.
export const FIXTURE_NOW_MS = 1_733_000_000_000; // 2024-11-30T20:53:20Z

const BASE_HEIGHT = 870_000; // post-4th-halving (subsidy 3.125 BTC)
const TEN_MIN = 600_000;

/** Build a raw mempool.space `/api/v1/blocks` entry with sane defaults. */
export function mkMempoolBlock(overrides: any = {}): any {
  const height = overrides.height ?? BASE_HEIGHT;
  const extras = {
    reward: 315_000_000,
    totalFees: 2_500_000,
    coinbaseSignatureAscii: null as string | null,
    pool: { id: 999, name: 'Unknown', slug: 'unknown', minerNames: null },
    matchRate: 100,
    ...(overrides.extras ?? {}),
  };
  return {
    id: overrides.id ?? `0000000000000000000hash${height}`,
    height,
    version: 0x20000000,
    // Height-staggered ~10-minute spacing so interval metrics and ages are realistic.
    timestamp: overrides.timestamp ?? Math.floor(FIXTURE_NOW_MS / 1000) - (BASE_HEIGHT + 60 - height) * 600,
    bits: 386_000_000,
    nonce: 123_456_789,
    difficulty: 90_000_000_000_000,
    merkle_root: 'aa'.repeat(32),
    tx_count: overrides.tx_count ?? 3200,
    size: overrides.size ?? 1_500_000,
    weight: overrides.weight ?? 3_990_000,
    previousblockhash: `0000000000000000000hash${height - 1}`,
    mediantime: Math.floor((FIXTURE_NOW_MS - TEN_MIN * 2) / 1000),
    stale: false,
    ...overrides,
    extras,
  };
}

/** Build a raw Esplora `/api/blocks` entry. */
export function mkEsploraBlock(overrides: any = {}): any {
  const height = overrides.height ?? BASE_HEIGHT;
  return {
    id: overrides.id ?? `0000000000000000000esplora${height}`,
    height,
    version: 0x20000000,
    timestamp: overrides.timestamp ?? Math.floor(FIXTURE_NOW_MS / 1000) - (BASE_HEIGHT + 60 - height) * 600,
    tx_count: overrides.tx_count ?? 2800,
    size: overrides.size ?? 1_400_000,
    weight: overrides.weight ?? 3_950_000,
    merkle_root: 'bb'.repeat(32),
    previousblockhash: `0000000000000000000esplora${height - 1}`,
    mediantime: Math.floor((FIXTURE_NOW_MS - TEN_MIN * 2) / 1000),
    nonce: 987_654_321,
    bits: 386_000_000,
    difficulty: 90_000_000_000_000,
    ...overrides,
  };
}

// ---------------------------------------------------------------------------
// (1) known pool with strong coinbase evidence — no provider pool, coinbase names it
// ---------------------------------------------------------------------------
/**
 * Build a realistic coinbase scriptsig hex: a binary prefix (BIP34 height /
 * extranonce stand-in) + the printable pool tag + a binary suffix. Lets the
 * sanitized-evidence drawer be exercised against genuine binary bytes.
 */
export function coinbaseHexFor(tagAscii: string, prefix: number[] = [0x03, 0x87, 0x9a, 0x0e, 0x00], suffix: number[] = [0x00, 0xff]): string {
  const body = Array.from(tagAscii).map(c => c.charCodeAt(0));
  return [...prefix, ...body, ...suffix].map(b => b.toString(16).padStart(2, '0')).join('');
}

export const BLOCK_STRONG_COINBASE = mkMempoolBlock({
  id: 'strong-coinbase',
  height: BASE_HEIGHT + 10,
  extras: { coinbaseSignatureAscii: 'AntPool/mined by xyz', coinbaseRaw: coinbaseHexFor('AntPool/mined by xyz'), pool: { id: 0, name: 'Unknown', slug: 'unknown' } },
});

// ---------------------------------------------------------------------------
// (2) provider-reported attribution — provider pool (corroborated by coinbase).
//     Public evidence → provider-reported, NEVER "confirmed".
// ---------------------------------------------------------------------------
export const BLOCK_PROVIDER_REPORTED = mkMempoolBlock({
  id: 'provider-reported',
  height: BASE_HEIGHT + 9,
  extras: { coinbaseSignatureAscii: '/Foundry USA Pool/', coinbaseRaw: coinbaseHexFor('/Foundry USA Pool/'), pool: { id: 111, name: 'Foundry USA', slug: 'foundryusa' } },
});

// ---------------------------------------------------------------------------
// (2b) probable via coinbase DOMAIN/alias — no provider pool, coinbase carries a
//      pool domain (not a name token) → probable, not strong.
// ---------------------------------------------------------------------------
export const BLOCK_PROBABLE_DOMAIN = mkMempoolBlock({
  id: 'probable-domain',
  height: BASE_HEIGHT + 12,
  extras: { coinbaseSignatureAscii: 'foundrydigital.com', pool: { id: 0, name: 'Unknown', slug: 'unknown' } },
});

// ---------------------------------------------------------------------------
// (3) provider label needing canonicalization (SlushPool → Braiins Pool).
//     Provider supplied a pool → provider-reported, with a canonical name.
// ---------------------------------------------------------------------------
export const BLOCK_ALIAS_MATCH = mkMempoolBlock({
  id: 'alias-match',
  height: BASE_HEIGHT + 8,
  extras: { coinbaseSignatureAscii: null, pool: { id: 5, name: 'SlushPool', slug: 'slushpool' } },
});

// ---------------------------------------------------------------------------
// (4) unknown pool — coinbase present but not a known pool
// ---------------------------------------------------------------------------
export const BLOCK_UNKNOWN_POOL = mkMempoolBlock({
  id: 'unknown-pool',
  height: BASE_HEIGHT + 7,
  extras: { coinbaseSignatureAscii: 'mined-by-anon-9931', pool: { id: 0, name: 'Unknown', slug: 'unknown' } },
});

// ---------------------------------------------------------------------------
// (5) empty / missing coinbase evidence — unattributed
// ---------------------------------------------------------------------------
export const BLOCK_UNATTRIBUTED = mkMempoolBlock({
  id: 'unattributed',
  height: BASE_HEIGHT + 6,
  extras: { coinbaseSignatureAscii: null, pool: { id: 0, name: 'Unknown', slug: 'unknown' } },
});

// ---------------------------------------------------------------------------
// (6) conflicting attribution labels — provider says AntPool, coinbase says Foundry
// ---------------------------------------------------------------------------
export const BLOCK_CONFLICTING = mkMempoolBlock({
  id: 'conflicting',
  height: BASE_HEIGHT + 5,
  extras: { coinbaseSignatureAscii: '/Foundry USA Pool/', pool: { id: 222, name: 'AntPool', slug: 'antpool' } },
});

// ---------------------------------------------------------------------------
// (7) active-pool match — Public Pool (configured active = public-pool.io)
// ---------------------------------------------------------------------------
export const BLOCK_ACTIVE_MATCH = mkMempoolBlock({
  id: 'active-match',
  height: BASE_HEIGHT + 4,
  extras: { coinbaseSignatureAscii: 'public-pool.io', pool: { id: 300, name: 'Public Pool', slug: 'public-pool' } },
});

// ---------------------------------------------------------------------------
// (8) fallback-pool match — Solo CKPool (configured fallback = solo.ckpool.org)
// ---------------------------------------------------------------------------
export const BLOCK_FALLBACK_MATCH = mkMempoolBlock({
  id: 'fallback-match',
  height: BASE_HEIGHT + 3,
  extras: { coinbaseSignatureAscii: 'ckpool.eu/solo.ckpool.org', pool: { id: 400, name: 'Solo CKPool', slug: 'solock' } },
});

// ---------------------------------------------------------------------------
// (14) missing optional block fields — only height/id/timestamp present
// ---------------------------------------------------------------------------
export const BLOCK_MISSING_FIELDS: any = {
  id: 'missing-fields',
  height: BASE_HEIGHT + 2,
  timestamp: Math.floor((FIXTURE_NOW_MS - TEN_MIN) / 1000),
  // no tx_count / size / weight / extras
};

/** A full, ordered mempool blocks response mixing attribution scenarios. */
export const MEMPOOL_BLOCKS_MIXED: any[] = [
  BLOCK_STRONG_COINBASE,
  BLOCK_PROVIDER_REPORTED,
  BLOCK_ALIAS_MATCH,
  BLOCK_UNKNOWN_POOL,
  BLOCK_UNATTRIBUTED,
  BLOCK_CONFLICTING,
  BLOCK_ACTIVE_MATCH,
  BLOCK_FALLBACK_MATCH,
  BLOCK_MISSING_FIELDS,
];

/** Esplora response (no attribution → all unattributed). */
export const ESPLORA_BLOCKS: any[] = [
  mkEsploraBlock({ id: 'esplora-tip', height: BASE_HEIGHT + 10 }),
  mkEsploraBlock({ id: 'esplora-2', height: BASE_HEIGHT + 9 }),
  mkEsploraBlock({ id: 'esplora-3', height: BASE_HEIGHT + 8 }),
];

// ---------------------------------------------------------------------------
// (9) ambiguous active/fallback alias — both configured pools resolve to ckpool-solo
// ---------------------------------------------------------------------------
export const BLOCK_AMBIGUOUS = mkMempoolBlock({
  id: 'ambiguous',
  height: BASE_HEIGHT + 1,
  extras: { coinbaseSignatureAscii: 'solo.ckpool.org', pool: { id: 400, name: 'Solo CKPool', slug: 'solock' } },
});

// ---------------------------------------------------------------------------
// (12) new block tip — before/after (height advances by 1)
// ---------------------------------------------------------------------------
export const NEW_TIP_BEFORE: any[] = [
  mkMempoolBlock({ id: 'tip-A', height: BASE_HEIGHT + 20 }),
  mkMempoolBlock({ id: 'tip-A-1', height: BASE_HEIGHT + 19 }),
];
export const NEW_TIP_AFTER: any[] = [
  mkMempoolBlock({ id: 'tip-B', height: BASE_HEIGHT + 21 }),
  mkMempoolBlock({ id: 'tip-A', height: BASE_HEIGHT + 20 }),
  mkMempoolBlock({ id: 'tip-A-1', height: BASE_HEIGHT + 19 }),
];

// ---------------------------------------------------------------------------
// (13) reorg / replaced tip — same height, different hash
// ---------------------------------------------------------------------------
export const REORG_BEFORE: any[] = [
  mkMempoolBlock({ id: 'reorg-original', height: BASE_HEIGHT + 30 }),
  mkMempoolBlock({ id: 'reorg-parent', height: BASE_HEIGHT + 29 }),
];
export const REORG_AFTER: any[] = [
  mkMempoolBlock({ id: 'reorg-replacement', height: BASE_HEIGHT + 30 }),
  mkMempoolBlock({ id: 'reorg-parent', height: BASE_HEIGHT + 29 }),
];

// ---------------------------------------------------------------------------
// (17) duplicate blocks — same hash appears twice
// ---------------------------------------------------------------------------
export const DUPLICATE_BLOCKS: any[] = [
  mkMempoolBlock({ id: 'dup', height: BASE_HEIGHT + 40 }),
  mkMempoolBlock({ id: 'dup', height: BASE_HEIGHT + 40 }),
  mkMempoolBlock({ id: 'dup-parent', height: BASE_HEIGHT + 39 }),
];

// ---------------------------------------------------------------------------
// (18) out-of-order blocks — heights not descending
// ---------------------------------------------------------------------------
export const OUT_OF_ORDER: any[] = [
  mkMempoolBlock({ id: 'ooo-low', height: BASE_HEIGHT + 50, timestamp: Math.floor((FIXTURE_NOW_MS - TEN_MIN * 3) / 1000) }),
  mkMempoolBlock({ id: 'ooo-high', height: BASE_HEIGHT + 52, timestamp: Math.floor((FIXTURE_NOW_MS - TEN_MIN) / 1000) }),
  mkMempoolBlock({ id: 'ooo-mid', height: BASE_HEIGHT + 51, timestamp: Math.floor((FIXTURE_NOW_MS - TEN_MIN * 2) / 1000) }),
];

// ---------------------------------------------------------------------------
// (15) malformed provider payload
// ---------------------------------------------------------------------------
export const MALFORMED_NOT_ARRAY: any = { error: 'rate limited' };
export const MALFORMED_GARBAGE: any = 'not json at all';
export const MALFORMED_NULLS: any[] = [null, undefined, { height: 'oops' }, { id: 123 }];

// ---------------------------------------------------------------------------
// (16) oversized provider response — exceeds MAX_RESPONSE_BYTES
// ---------------------------------------------------------------------------
export function oversizedPayload(bytes: number): string {
  return 'x'.repeat(bytes);
}

// ---------------------------------------------------------------------------
// (20) PRIVACY fixture — sensitive values that must NEVER leave local matching.
// Only the hosts are ever used for matching; usernames/SSID/passwords/IP are
// present precisely so tests can assert they never appear downstream.
// ---------------------------------------------------------------------------
export const PRIVACY_SENSITIVE = {
  stratumUser: 'bc1qexamplewalletdonotusexxxxxxxxxxxxxxxx.worker-01',
  fallbackStratumUser: 'bc1qexamplewalletdonotusexxxxxxxxxxxxxxxx.worker-02',
  ssid: 'MY_SECRET_HOME_WIFI',
  wifiPass: 'hunter2-not-real',
  hostname: 'neuralaxe-livingroom',
  ip: '192.168.7.42',
  // The only fields matching is ever allowed to touch:
  activeHost: 'public-pool.io',
  fallbackHost: 'solo.ckpool.org',
  activeUrl: 'stratum+tcp://public-pool.io:21496',
  fallbackUrl: 'solo.ckpool.org:3333',
};

/** Every sensitive string that must never appear in normalized output. */
export const PRIVACY_FORBIDDEN_STRINGS: string[] = [
  PRIVACY_SENSITIVE.stratumUser,
  PRIVACY_SENSITIVE.fallbackStratumUser,
  PRIVACY_SENSITIVE.ssid,
  PRIVACY_SENSITIVE.wifiPass,
  PRIVACY_SENSITIVE.hostname,
  PRIVACY_SENSITIVE.ip,
];

/** Configured pools for the ambiguous case — both resolve to ckpool-solo. */
export const AMBIGUOUS_CONFIG = { activeHost: 'solo.ckpool.org', fallbackHost: 'eusolo.ckpool.org' };

/** Standard configured pools: active Public Pool, fallback Solo CKPool. */
export const STANDARD_CONFIG = { activeHost: 'public-pool.io', fallbackHost: 'solo.ckpool.org' };
