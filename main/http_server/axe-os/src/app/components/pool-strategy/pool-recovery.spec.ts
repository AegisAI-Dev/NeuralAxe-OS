import { SwitchInterruptionRecord, deriveInterruptionRecovery, deviceMatches } from './pool-recovery';
import { captureConfig } from './pool-history';
import { systemInfo, SECRET_TOKENS } from './pool-fixtures';

const USER = 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1';

function record(overrides: Partial<SwitchInterruptionRecord> = {}): SwitchInterruptionRecord {
  return {
    sessionId: 's1',
    at: 1,
    targetProfileName: 'BCH Demo',
    targetChain: 'BCH',
    // Original = the device's own config (solo.ckpool.org) captured at start.
    original: captureConfig(systemInfo()),
    // Target = a different pool the switch was moving to.
    target: { host: 'bch.example-pool.test', port: 3334, user: 'bitcoincash:q.rig1' },
    passwordWasReplaced: false,
    ...overrides,
  };
}

describe('pool-recovery', () => {
  it('carries NO password VALUE in the record — only the non-secret passwordMode/flag (Blocker 2/4)', () => {
    const flat = JSON.stringify(record({ passwordWasReplaced: true }));
    SECRET_TOKENS.forEach(t => expect(flat).not.toContain(t));
    // The only "password" substrings allowed are the metadata keys, never a value.
    expect(flat).not.toMatch(/"(stratum)?[Pp]assword"\s*:/);
  });

  describe('deviceMatches', () => {
    it('matches host+port+user (host case-insensitive)', () => {
      expect(deviceMatches({ host: 'SOLO.CKPOOL.ORG', port: 3333, user: USER }, systemInfo())).toBeTrue();
      expect(deviceMatches({ host: 'other.test', port: 3333, user: USER }, systemInfo())).toBeFalse();
      expect(deviceMatches({ host: 'solo.ckpool.org', port: 3333, user: USER }, null)).toBeFalse();
    });
  });

  describe('deriveInterruptionRecovery — the 7 interruption cases', () => {
    it('1/2. refresh after PATCH but before restart → still on ORIGINAL', () => {
      // Device has not restarted; telemetry still shows the original pool.
      const r = deriveInterruptionRecovery(record(), systemInfo());
      expect(r.situation).toBe('on-original');
      expect(r.canRetryRollback).toBeTrue();
      expect(r.headline).toContain('ORIGINAL');
    });

    it('3. refresh during reconnect (no telemetry yet)', () => {
      const r = deriveInterruptionRecovery(record(), null);
      expect(r.situation).toBe('no-telemetry');
      expect(r.detail).toContain('No device telemetry');
    });

    it('4. interrupted with NO original password → not recoverable + masked guidance', () => {
      const r = deriveInterruptionRecovery(record({ passwordWasReplaced: true }), systemInfo({ stratumURL: 'bch.example-pool.test', stratumPort: 3334, stratumUser: 'bitcoincash:q.rig1' }));
      expect(r.originalPasswordRecoverable).toBeFalse();
      expect(r.passwordNote).toContain('keep the device\'s current password');
    });

    it('5. telemetry already matches the ORIGINAL', () => {
      const r = deriveInterruptionRecovery(record(), systemInfo());
      expect(r.situation).toBe('on-original');
      expect(r.originalPasswordRecoverable).toBeTrue();
      expect(r.passwordNote).toBeNull();
    });

    it('6. telemetry matches the TARGET', () => {
      const r = deriveInterruptionRecovery(record(), systemInfo({ stratumURL: 'bch.example-pool.test', stratumPort: 3334, stratumUser: 'bitcoincash:q.rig1' }));
      expect(r.situation).toBe('on-target');
      expect(r.headline).toContain('TARGET');
      expect(r.canRetryRollback).toBeTrue();
    });

    it('7. unknown current configuration (matches neither)', () => {
      const r = deriveInterruptionRecovery(record(), systemInfo({ stratumURL: 'somewhere-else.test' }));
      expect(r.situation).toBe('unknown');
      expect(r.detail).toContain('neither');
    });

    it('never claims Complete and always offers rollback', () => {
      (['on-original', 'on-target', 'unknown', 'no-telemetry'] as const).forEach(() => {
        const r = deriveInterruptionRecovery(record(), systemInfo({ stratumURL: 'x.test' }));
        expect(r.headline.toLowerCase()).not.toContain('complete');
      });
    });
  });
});
