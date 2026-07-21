import {
  bchProfile, btcProfile, customProfile, invalidHostProfile, invalidPortProfile, missingCredProfile,
  primaryFallbackProfile, primaryOnlyProfile, secretProfile, SECRET_TOKENS, systemInfo,
} from './pool-fixtures';
import { validateProfile } from './pool-profile';

function serialized(p: unknown): string { return JSON.stringify(p); }

describe('pool-fixtures', () => {
  it('produces valid BTC / BCH / Custom / primary-only / primary+fallback profiles', () => {
    expect(validateProfile(btcProfile())).toEqual([]);
    expect(validateProfile(bchProfile())).toEqual([]);
    expect(validateProfile(customProfile())).toEqual([]);
    expect(validateProfile(primaryOnlyProfile())).toEqual([]);
    expect(validateProfile(primaryFallbackProfile())).toEqual([]);
    expect(validateProfile(secretProfile())).toEqual([]);
  });

  it('produces the invalid fixtures for the negative scenarios', () => {
    expect(validateProfile(invalidHostProfile()).length).toBeGreaterThan(0);
    expect(validateProfile(invalidPortProfile()).length).toBeGreaterThan(0);
    expect(validateProfile(missingCredProfile()).length).toBeGreaterThan(0);
  });

  it('labels chains distinctly', () => {
    expect(btcProfile().chain).toBe('BTC');
    expect(bchProfile().chain).toBe('BCH');
    expect(customProfile().chain).toBe('custom');
  });

  it('gives each profile a unique id', () => {
    const ids = [btcProfile().id, bchProfile().id, customProfile().id];
    expect(new Set(ids).size).toBe(ids.length);
  });

  it('secret fixture is set-mode but stores NO raw password on the profile', () => {
    const sp = secretProfile();
    expect(sp.primary.passwordMode).toBe('set');
    expect(sp.fallback?.passwordMode).toBe('set');
    // The session password sentinels must never appear in the persisted profile.
    SECRET_TOKENS.forEach(t => expect(serialized(sp)).not.toContain(t));
  });

  it('produces a supported NeuralAxe system-info by default', () => {
    const info = systemInfo();
    expect(info.targetBoard).toBe('601');
    expect(info.targetAsic).toBe('BM1370');
    expect(info.productName).toBe('NeuralAxe OS');
  });
});
