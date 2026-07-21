import {
  ActivePoolRecord, CHAIN_DISCLAIMER, deriveChainContext, isBchContext, isBtcContext, recordMatchesDevice,
} from './pool-chain';
import { systemInfo } from './pool-fixtures';

function record(overrides: Partial<ActivePoolRecord> = {}): ActivePoolRecord {
  return {
    profileId: 'p1',
    profileName: 'BTC Solo',
    chain: 'BTC',
    primaryHost: 'solo.ckpool.org',
    primaryPort: 3333,
    primaryUser: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1',
    appliedAt: 1_700_000_000_000,
    ...overrides,
  };
}

describe('pool-chain', () => {
  it('exposes the required disclaimer wording', () => {
    expect(CHAIN_DISCLAIMER).toContain('Chain label describes the selected pool profile');
    expect(CHAIN_DISCLAIMER).toContain('does not');
    expect(CHAIN_DISCLAIMER).toContain('cryptographically');
  });

  describe('recordMatchesDevice', () => {
    it('matches an unchanged device (case-insensitive host)', () => {
      expect(recordMatchesDevice(record({ primaryHost: 'SOLO.CKPOOL.ORG' }), systemInfo())).toBeTrue();
    });
    it('does not match a changed host / port / user', () => {
      expect(recordMatchesDevice(record(), systemInfo({ stratumURL: 'else.test' }))).toBeFalse();
      expect(recordMatchesDevice(record(), systemInfo({ stratumPort: 3334 }))).toBeFalse();
      expect(recordMatchesDevice(record(), systemInfo({ stratumUser: 'other.rig' }))).toBeFalse();
      expect(recordMatchesDevice(record(), null)).toBeFalse();
    });
  });

  describe('deriveChainContext', () => {
    it('is Unknown with no record', () => {
      const ctx = deriveChainContext(null, systemInfo());
      expect(ctx.chain).toBe('unknown');
      expect(ctx.labelled).toBeFalse();
      expect(ctx.detail).toContain('No labelled pool profile');
    });
    it('is the labelled chain when verified', () => {
      const ctx = deriveChainContext(record(), systemInfo());
      expect(ctx.chain).toBe('BTC');
      expect(ctx.labelled).toBeTrue();
      expect(ctx.verified).toBeTrue();
      expect(ctx.profileName).toBe('BTC Solo');
      expect(ctx.detail).toContain(CHAIN_DISCLAIMER);
    });
    it('is Unknown when the device drifted from the applied profile', () => {
      const ctx = deriveChainContext(record({ chain: 'BCH', profileName: 'BCH Pool' }), systemInfo({ stratumURL: 'changed.test' }));
      expect(ctx.chain).toBe('unknown');
      expect(ctx.labelled).toBeFalse();
      expect(ctx.detail).toContain('no longer matches');
      expect(ctx.detail).toContain('BCH Pool');
    });
  });

  describe('context helpers', () => {
    it('recognizes BTC and BCH verified contexts', () => {
      const btc = deriveChainContext(record(), systemInfo());
      const bch = deriveChainContext(record({ chain: 'BCH', primaryHost: 'bch.test', primaryUser: 'u', primaryPort: 1 }), systemInfo({ stratumURL: 'bch.test', stratumUser: 'u', stratumPort: 1 }));
      expect(isBtcContext(btc)).toBeTrue();
      expect(isBchContext(btc)).toBeFalse();
      expect(isBchContext(bch)).toBeTrue();
    });
    it('is neither when unknown', () => {
      const ctx = deriveChainContext(null, systemInfo());
      expect(isBtcContext(ctx)).toBeFalse();
      expect(isBchContext(ctx)).toBeFalse();
    });
  });
});
