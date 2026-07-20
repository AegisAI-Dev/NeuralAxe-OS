import {
  normalizePoolHost,
  registrableDomain,
  identifyPoolFromHost,
  identifyPoolFromAttribution,
  matchAttributionToConfig,
  applyConfiguredMatches,
} from './pool-normalize';
import { BlockSummary, PoolAttribution } from './block-intelligence.model';

function attr(overrides: Partial<PoolAttribution> = {}): PoolAttribution {
  return {
    poolName: null,
    providerLabel: null,
    slug: null,
    method: 'none',
    confidence: 'unknown',
    source: 'test',
    evidence: { coinbaseTagAscii: null, coinbaseTagId: null, providerMatchRate: null, reason: '', aliases: [] },
    ...overrides,
  };
}

describe('pool-normalize: normalizePoolHost', () => {
  it('lowercases and trims', () => {
    expect(normalizePoolHost('  Public-Pool.IO  ')).toBe('public-pool.io');
  });
  it('strips stratum+tcp protocol and port', () => {
    expect(normalizePoolHost('stratum+tcp://public-pool.io:21496')).toBe('public-pool.io');
  });
  it('strips https and path/query', () => {
    expect(normalizePoolHost('https://solo.ckpool.org/users/abc?x=1')).toBe('solo.ckpool.org');
  });
  it('drops a credentials segment defensively', () => {
    expect(normalizePoolHost('stratum+tcp://user:pass@pool.example:3333')).toBe('pool.example');
  });
  it('keeps IPv6 host without its port', () => {
    expect(normalizePoolHost('[2001:db8::1]:3333')).toBe('2001:db8::1');
  });
  it('returns empty string for non-strings', () => {
    expect(normalizePoolHost(null)).toBe('');
    expect(normalizePoolHost(undefined)).toBe('');
    expect(normalizePoolHost(42 as any)).toBe('');
  });
});

describe('pool-normalize: registrableDomain', () => {
  it('reduces a multi-label host to the registrable domain', () => {
    expect(registrableDomain('eu.stratum.slushpool.com')).toBe('slushpool.com');
  });
  it('handles two-level public suffixes', () => {
    expect(registrableDomain('foo.bar.co.uk')).toBe('bar.co.uk');
  });
  it('leaves IPs unchanged', () => {
    expect(registrableDomain('192.168.1.1')).toBe('192.168.1.1');
  });
  it('leaves single-label hosts unchanged', () => {
    expect(registrableDomain('localhost')).toBe('localhost');
  });
});

describe('pool-normalize: identify from host / attribution', () => {
  it('identifies a configured host by domain', () => {
    expect(identifyPoolFromHost('public-pool.io')?.key).toBe('publicpool');
    expect(identifyPoolFromHost('solo.ckpool.org')?.key).toBe('ckpool-solo');
    expect(identifyPoolFromHost('eu.stratum.slushpool.com')?.key).toBe('braiins');
  });
  it('returns null for an unknown host', () => {
    expect(identifyPoolFromHost('unknown-pool.example')).toBeNull();
  });
  it('identifies a pool from a provider label', () => {
    expect(identifyPoolFromAttribution({ poolName: 'Foundry USA' })?.key).toBe('foundryusa');
    expect(identifyPoolFromAttribution({ poolName: 'SlushPool' })?.key).toBe('braiins');
  });
  it('identifies a pool from a coinbase tag token', () => {
    expect(identifyPoolFromAttribution({ coinbaseTag: '/Foundry USA Pool/' })?.key).toBe('foundryusa');
  });
  it('does not match a short alias inside an unrelated word', () => {
    // "ocean" alias must not match inside "someoceanic"
    expect(identifyPoolFromAttribution({ poolName: 'someoceanicthing' })).toBeNull();
  });
  it('returns null when nothing is provided', () => {
    expect(identifyPoolFromAttribution({})).toBeNull();
  });
});

describe('pool-normalize: matchAttributionToConfig', () => {
  const config = { activeHost: 'public-pool.io', fallbackHost: 'solo.ckpool.org' };

  it('returns insufficient for unknown/unattributed attribution', () => {
    expect(matchAttributionToConfig(attr({ confidence: 'unknown' }), config).match).toBe('insufficient');
    expect(matchAttributionToConfig(attr({ confidence: 'unattributed' }), config).match).toBe('insufficient');
  });
  it('matches the active pool', () => {
    const a = attr({ poolName: 'Public Pool', confidence: 'provider-reported' });
    const r = matchAttributionToConfig(a, config);
    expect(r.match).toBe('active');
    expect(r.reason).toContain('active');
  });
  it('matches the fallback pool', () => {
    const a = attr({ poolName: 'Solo CKPool', slug: 'solock', confidence: 'strong' });
    expect(matchAttributionToConfig(a, config).match).toBe('fallback');
  });
  it('reports both when active and fallback resolve to the same identity', () => {
    const a = attr({ poolName: 'Solo CKPool', slug: 'solock', confidence: 'strong' });
    const r = matchAttributionToConfig(a, { activeHost: 'solo.ckpool.org', fallbackHost: 'eusolo.ckpool.org' });
    expect(r.match).toBe('both');
  });
  it('reports none for an identified pool that matches neither', () => {
    const a = attr({ poolName: 'Foundry USA', confidence: 'provider-reported' });
    expect(matchAttributionToConfig(a, config).match).toBe('none');
  });
  it('never uses stratum usernames — only host params are accepted', () => {
    // Type-level guarantee reinforced here: matching takes hosts, and a wallet in
    // the pool NAME (nonsensical) still just fails to identify, never leaks.
    const a = attr({ poolName: 'bc1qwalletxxxx.worker', confidence: 'probable' });
    const r = matchAttributionToConfig(a, config);
    expect(r.match).toBe('insufficient'); // unidentifiable, not a false match
    expect(JSON.stringify(r)).not.toContain('bc1qwalletxxxx');
  });
});

describe('pool-normalize: applyConfiguredMatches', () => {
  it('sets configuredMatch on each block immutably', () => {
    const block: BlockSummary = {
      height: 1, hash: 'h', timestampMs: 1, sourceTimestampMs: 1, txCount: null, size: null, weight: null,
      totalFees: null, subsidy: null, reward: null,
      attribution: attr({ poolName: 'Public Pool', confidence: 'provider-reported' }),
      configuredMatch: 'insufficient', source: 'test',
    };
    const out = applyConfiguredMatches([block], { activeHost: 'public-pool.io', fallbackHost: null });
    expect(out[0].configuredMatch).toBe('active');
    expect(block.configuredMatch).toBe('insufficient'); // original untouched
  });
});
