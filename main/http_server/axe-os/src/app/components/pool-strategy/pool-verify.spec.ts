import {
  RECONNECT_TIMEOUT_MS, VERIFY_TIMEOUT_MS, evaluatePoolVerification, verificationLevels,
} from './pool-verify';
import { deviceFallbackActive, deviceHostMismatch, deviceMiningPaused, deviceEmergency, systemInfo } from './pool-fixtures';

const TARGET = { primaryHost: 'solo.ckpool.org', fallbackHost: 'backup.example-pool.test' };

function evalWith(info: any, online = true, baseA: number | null = 100, baseR: number | null = 2) {
  return evaluatePoolVerification({ info, online, target: TARGET, baselineAccepted: baseA, baselineRejected: baseR });
}

describe('pool-verify', () => {
  it('has conservative timeouts (reconnect longer than verify)', () => {
    expect(RECONNECT_TIMEOUT_MS).toBeGreaterThan(VERIFY_TIMEOUT_MS);
    expect(VERIFY_TIMEOUT_MS).toBeGreaterThan(0);
  });

  it('is pending with no telemetry', () => {
    const v = evalWith(null);
    expect(v.poolConnected).toBeFalse();
    expect(v.connectedAndMining).toBeFalse();
    expect(v.detail).toContain('pending');
  });

  it('verifies connected + mining + primary host', () => {
    const v = evalWith(systemInfo(), true, 100, 2);
    expect(v.poolConnected).toBeTrue();
    expect(v.miningResumed).toBeTrue();
    expect(v.targetHostVerified).toBeTrue();
    expect(v.activeHostRole).toBe('primary');
    expect(v.connectedAndMining).toBeTrue();
  });

  it('accepts the fallback host as a valid target', () => {
    const v = evalWith(deviceFallbackActive());
    expect(v.activeHostRole).toBe('fallback');
    expect(v.targetHostVerified).toBeTrue();
    expect(v.connectedAndMining).toBeTrue();
  });

  it('fails host verification on a mismatch', () => {
    const v = evalWith(deviceHostMismatch());
    expect(v.activeHostRole).toBe('none');
    expect(v.targetHostVerified).toBeFalse();
    expect(v.connectedAndMining).toBeFalse();
  });

  it('does not claim mining when paused', () => {
    const v = evalWith(deviceMiningPaused());
    expect(v.miningResumed).toBeFalse();
    expect(v.connectedAndMining).toBeFalse();
  });

  it('does not verify host while offline', () => {
    const v = evalWith(systemInfo(), false);
    expect(v.poolConnected).toBeFalse();
    expect(v.targetHostVerified).toBeFalse();
  });

  it('flags a device fault (emergency)', () => {
    const v = evalWith(deviceEmergency());
    expect(v.faulted).toBeTrue();
    expect(v.connectedAndMining).toBeFalse();
  });

  it('observes new share activity and a counter reset', () => {
    expect(evalWith(systemInfo({ sharesAccepted: 150 }), true, 100, 2).shareActivityObserved).toBeTrue();
    expect(evalWith(systemInfo({ sharesAccepted: 3 }), true, 100, 2).shareActivityObserved).toBeTrue(); // reset
    expect(evalWith(systemInfo({ sharesAccepted: 100 }), true, 100, 2).shareActivityObserved).toBeFalse();
  });

  it('never requires a share for basic success', () => {
    const v = evalWith(systemInfo({ sharesAccepted: 100 }), true, 100, 2);
    expect(v.shareActivityObserved).toBeFalse();
    expect(v.connectedAndMining).toBeTrue();
  });

  it('exposes the five evidence levels with shares optional', () => {
    const levels = verificationLevels(evalWith(systemInfo()));
    const shares = levels.find(l => l.id === 'shares');
    expect(shares?.required).toBeFalse();
    expect(levels.filter(l => l.required).map(l => l.id)).toEqual(['applied', 'connected', 'mining', 'host']);
  });
});
