import {
  PROFILE_LIMITS, blankEndpoint, buildStarterProfiles, canAddProfile, chainLabel, chainShort,
  endpointFromInfo, endpointReplacesPassword, endpointUsable, fallbackProvided, isDuplicateProfile, isPoolChain,
  maskAccount, maskHost, maskPassword, normalizePort, profileToSettings, realSecret, validateEndpoint, validateProfile,
} from './pool-profile';
import { btcProfile, bchProfile, endpoint, primaryFallbackProfile, profile, secretProfile, SECRET_PASSWORDS, systemInfo } from './pool-fixtures';

describe('pool-profile', () => {
  describe('chain labels', () => {
    it('labels each chain and defaults unknown to Custom', () => {
      expect(chainLabel('BTC')).toContain('BTC');
      expect(chainLabel('BCH')).toContain('BCH');
      expect(chainLabel('custom')).toContain('Custom');
      expect(chainLabel(null)).toContain('Custom');
      expect(chainShort('BTC')).toBe('BTC');
      expect(chainShort(undefined)).toBe('Custom');
    });
    it('validates chain membership', () => {
      expect(isPoolChain('BTC')).toBeTrue();
      expect(isPoolChain('BCH')).toBeTrue();
      expect(isPoolChain('custom')).toBeTrue();
      expect(isPoolChain('LTC')).toBeFalse();
      expect(isPoolChain(null)).toBeFalse();
    });
  });

  describe('normalizePort', () => {
    it('accepts valid ports and rejects out-of-range', () => {
      expect(normalizePort(3333)).toBe(3333);
      expect(normalizePort('3333')).toBe(3333);
      expect(normalizePort(0)).toBe(0);
      expect(normalizePort(65535)).toBe(65535);
      expect(normalizePort(65536)).toBeNull();
      expect(normalizePort(-1)).toBeNull();
      expect(normalizePort('abc')).toBeNull();
      expect(normalizePort(null)).toBeNull();
    });
  });

  describe('masking', () => {
    it('masks a host without revealing it whole', () => {
      const masked = maskHost('solo.ckpool.org');
      expect(masked).not.toBe('');
      expect(maskHost('')).toBe('—');
    });
    it('keeps the worker suffix but masks the account', () => {
      const masked = maskAccount('bc1qverylongwalletaddressxxxxxxxxxxxx.rig1');
      expect(masked).toContain('.rig1');
      expect(masked).not.toContain('verylongwalletaddress');
    });
    it('masks a bare account with no worker', () => {
      expect(maskAccount('someaccount')).not.toBe('someaccount');
      expect(maskAccount('')).toBe('—');
    });
    it('reports password intent only, never a value', () => {
      expect(maskPassword(endpoint({ passwordMode: 'keep' }))).toContain('unchanged');
      expect(maskPassword(endpoint({ passwordMode: 'set' }))).toContain('switch');
      expect(maskPassword(endpoint({ passwordMode: 'set' }))).not.toMatch(/secret|password value/i);
    });
  });

  describe('realSecret (empty / masked rejection)', () => {
    it('accepts a real password (spaces preserved, not trimmed)', () => {
      expect(realSecret('hunter2')).toBe('hunter2');
      expect(realSecret(' p w ')).toBe(' p w ');
    });
    it('rejects empty and masked placeholders', () => {
      expect(realSecret('')).toBeNull();
      expect(realSecret('*****')).toBeNull();
      expect(realSecret('••••••')).toBeNull();
      expect(realSecret('current (hidden)')).toBeNull();
      expect(realSecret(undefined)).toBeNull();
      expect(realSecret(123)).toBeNull();
    });
  });

  describe('endpointReplacesPassword', () => {
    it('is true only for set mode', () => {
      expect(endpointReplacesPassword(endpoint({ passwordMode: 'set' }))).toBeTrue();
      expect(endpointReplacesPassword(endpoint({ passwordMode: 'keep' }))).toBeFalse();
      expect(endpointReplacesPassword(null)).toBeFalse();
    });
  });

  describe('endpoint usability + fallback presence', () => {
    it('detects a usable endpoint', () => {
      expect(endpointUsable(endpoint())).toBeTrue();
      expect(endpointUsable(endpoint({ host: '' }))).toBeFalse();
      expect(endpointUsable(endpoint({ port: null }))).toBeFalse();
      expect(endpointUsable(endpoint({ user: '' }))).toBeFalse();
      expect(endpointUsable(null)).toBeFalse();
    });
    it('detects whether a fallback was provided', () => {
      expect(fallbackProvided(null)).toBeFalse();
      expect(fallbackProvided(blankEndpoint())).toBeFalse();
      expect(fallbackProvided(endpoint())).toBeTrue();
    });
  });

  describe('validation', () => {
    it('accepts a valid endpoint', () => {
      expect(validateEndpoint(endpoint(), 'primary')).toEqual([]);
    });
    it('rejects a protocol prefix and inline port', () => {
      expect(validateEndpoint(endpoint({ host: 'stratum+tcp://x' }), 'primary').some(e => /prefix/.test(e))).toBeTrue();
      expect(validateEndpoint(endpoint({ host: 'pool.test:3333' }), 'primary').some(e => /inline :port/.test(e))).toBeTrue();
    });
    it('does not validate a password on the profile (entered at switch time)', () => {
      // A 'set' endpoint is structurally valid with no stored password.
      expect(validateEndpoint(endpoint({ passwordMode: 'set' }), 'primary')).toEqual([]);
    });
    it('validates a whole profile including fallback when present', () => {
      expect(validateProfile(btcProfile())).toEqual([]);
      expect(validateProfile(profile({ name: '' })).some(e => /name/i.test(e))).toBeTrue();
      const badFallback = primaryFallbackProfile();
      badFallback.fallback!.host = 'stratum+tcp://bad';
      expect(validateProfile(badFallback).some(e => /Fallback/.test(e))).toBeTrue();
    });
    it('does not validate an absent fallback', () => {
      expect(validateProfile(profile({ fallback: null }))).toEqual([]);
    });
  });

  describe('duplicate detection', () => {
    it('flags a duplicate name (case-insensitive)', () => {
      const a = btcProfile();
      const b = profile({ name: a.name.toUpperCase(), primary: endpoint({ host: 'other.test' }) });
      expect(isDuplicateProfile(b, [a])).toBeTrue();
    });
    it('flags a duplicate chain + primary identity', () => {
      const a = btcProfile();
      const b = { ...btcProfile(), name: 'Different Name' };
      expect(isDuplicateProfile(b, [a])).toBeTrue();
    });
    it('does not flag a distinct profile', () => {
      expect(isDuplicateProfile(bchProfile(), [btcProfile()])).toBeFalse();
    });
    it('ignores itself by id', () => {
      const a = btcProfile();
      expect(isDuplicateProfile(a, [a])).toBeFalse();
    });
    it('enforces the profile cap', () => {
      expect(canAddProfile(PROFILE_LIMITS.maxProfiles - 1)).toBeTrue();
      expect(canAddProfile(PROFILE_LIMITS.maxProfiles)).toBeFalse();
    });
  });

  describe('endpointFromInfo', () => {
    it('reads primary and fallback with keep password mode', () => {
      const info = systemInfo();
      const p = endpointFromInfo(info, 'primary');
      expect(p.host).toBe('solo.ckpool.org');
      expect(p.port).toBe(3333);
      expect(p.passwordMode).toBe('keep');
      const f = endpointFromInfo(info, 'fallback');
      expect(f.host).toBe('backup.example-pool.test');
    });
  });

  describe('profileToSettings', () => {
    it('always prefers the primary and omits a kept password', () => {
      const body: any = profileToSettings(btcProfile());
      expect(body.useFallbackStratum).toBe(0);
      expect(body.stratumURL).toBe('solo.ckpool.org');
      expect('stratumPassword' in body).toBeFalse();
      expect('fallbackStratumPassword' in body).toBeFalse();
    });
    it('mirrors the primary into the fallback when none configured', () => {
      const body: any = profileToSettings(profile({ fallback: null, primary: endpoint({ host: 'only.test', port: 3333 }) }));
      expect(body.fallbackStratumURL).toBe('only.test');
      expect(body.fallbackStratumPort).toBe(3333);
    });
    it('injects session passwords only for set endpoints, never from the profile', () => {
      const body: any = profileToSettings(secretProfile(), {
        primaryPassword: SECRET_PASSWORDS.targetPrimary,
        fallbackPassword: SECRET_PASSWORDS.targetFallback,
      });
      expect(body.stratumPassword).toBe(SECRET_PASSWORDS.targetPrimary);
      expect(body.fallbackStratumPassword).toBe(SECRET_PASSWORDS.targetFallback);
    });
    it('omits set passwords when no secret is supplied (never empty/masked)', () => {
      const body: any = profileToSettings(secretProfile(), {});
      expect('stratumPassword' in body).toBeFalse();
      expect('fallbackStratumPassword' in body).toBeFalse();
    });
    it('never writes an empty or masked placeholder password', () => {
      const body: any = profileToSettings(secretProfile(), { primaryPassword: '', fallbackPassword: '*****' });
      expect('stratumPassword' in body).toBeFalse();
      expect('fallbackStratumPassword' in body).toBeFalse();
    });
    it('shares the primary secret with a mirrored set-mode fallback', () => {
      const body: any = profileToSettings(profile({ fallback: null, primary: endpoint({ passwordMode: 'set' }) }), { primaryPassword: SECRET_PASSWORDS.targetPrimary });
      expect(body.stratumPassword).toBe(SECRET_PASSWORDS.targetPrimary);
      expect(body.fallbackStratumPassword).toBe(SECRET_PASSWORDS.targetPrimary);
    });
    it('uses the configured fallback when provided', () => {
      const body: any = profileToSettings(primaryFallbackProfile());
      expect(body.fallbackStratumURL).toBe('fallback.example-pool.test');
    });
  });

  describe('starters', () => {
    it('builds starters from a full configuration', () => {
      const built = buildStarterProfiles(systemInfo());
      const keys = built.specs.map(s => s.key);
      expect(keys).toContain('primary-only');
      expect(keys).toContain('primary-fallback');
      expect(keys).toContain('current-config');
      expect(built.notes.some(n => /password/i.test(n))).toBeTrue();
    });
    it('omits primary+fallback with a note when no fallback is configured', () => {
      const built = buildStarterProfiles(systemInfo({ fallbackStratumURL: '', fallbackStratumUser: '', fallbackStratumPort: 0 as any }));
      expect(built.specs.map(s => s.key)).not.toContain('primary-fallback');
      expect(built.notes.some(n => /fallback/i.test(n))).toBeTrue();
    });
    it('returns no starters when there is no device or an incomplete primary', () => {
      expect(buildStarterProfiles(null).specs).toEqual([]);
      const built = buildStarterProfiles(systemInfo({ stratumURL: '' }));
      expect(built.specs).toEqual([]);
      expect(built.notes.some(n => /incomplete/i.test(n))).toBeTrue();
    });
    it('never labels a chain automatically (draft is custom)', () => {
      const built = buildStarterProfiles(systemInfo());
      built.specs.forEach(s => expect(s.draft.chain).toBe('custom'));
    });
  });
});
