import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import {
  endpointView,
  poolConfigValid,
  poolSwitchGate,
  poolSwitchPlan,
  poolsEquivalent,
} from './pool-switch';

function infoWith(overrides: Partial<ISystemInfo> = {}): ISystemInfo {
  return {
    stratumURL: 'public-pool.io',
    stratumPort: 21496,
    stratumUser: 'bc1qprimaryworker.axe',
    fallbackStratumURL: 'solo.ckpool.org',
    fallbackStratumPort: 3333,
    fallbackStratumUser: 'bc1qfallbackworker.axe',
    isUsingFallbackStratum: 0,
    ...overrides,
  } as ISystemInfo;
}

describe('pool-switch', () => {
  describe('endpointView / poolConfigValid', () => {
    it('summarizes both roles without exposing credentials', () => {
      const info = infoWith();
      const primary = endpointView(info, 'primary');
      const fallback = endpointView(info, 'fallback');
      expect(primary).toEqual({ role: 'primary', host: 'public-pool.io', port: 21496, worker: 'bc1qprimaryworker.axe' });
      expect(fallback.host).toBe('solo.ckpool.org');
      expect(Object.keys(primary)).not.toContain('password');
    });

    it('rejects incomplete configurations', () => {
      expect(poolConfigValid(endpointView(infoWith({ fallbackStratumURL: '' }), 'fallback'))).toBeFalse();
      expect(poolConfigValid(endpointView(infoWith({ fallbackStratumPort: undefined as any }), 'fallback'))).toBeFalse();
      expect(poolConfigValid(endpointView(infoWith({ fallbackStratumPort: 70000 as any }), 'fallback'))).toBeFalse();
      expect(poolConfigValid(endpointView(infoWith({ fallbackStratumUser: '   ' }), 'fallback'))).toBeFalse();
      expect(poolConfigValid(endpointView(infoWith(), 'fallback'))).toBeTrue();
    });
  });

  describe('poolsEquivalent', () => {
    it('is false for distinct pools and true for identical endpoint + worker', () => {
      expect(poolsEquivalent(infoWith())).toBeFalse();
      expect(poolsEquivalent(infoWith({
        fallbackStratumURL: 'Public-Pool.io',
        fallbackStratumPort: 21496,
        fallbackStratumUser: 'bc1qprimaryworker.axe',
      }))).toBeTrue();
    });

    it('treats same host with different worker as non-equivalent', () => {
      expect(poolsEquivalent(infoWith({
        fallbackStratumURL: 'public-pool.io',
        fallbackStratumPort: 21496,
      }))).toBeFalse();
    });
  });

  describe('poolSwitchGate', () => {
    it('allows switching for two valid distinct pools with no unsaved edits', () => {
      expect(poolSwitchGate(infoWith(), false)).toEqual({ allowed: true, reason: null });
    });

    it('blocks when the fallback configuration is invalid', () => {
      const gate = poolSwitchGate(infoWith({ fallbackStratumURL: '' }), false);
      expect(gate.allowed).toBeFalse();
      expect(gate.reason).toContain('fallback');
    });

    it('blocks when the primary configuration is invalid', () => {
      const gate = poolSwitchGate(infoWith({ stratumUser: '' }), false);
      expect(gate.allowed).toBeFalse();
      expect(gate.reason).toContain('primary');
    });

    it('blocks when both configurations are equivalent', () => {
      const gate = poolSwitchGate(infoWith({
        fallbackStratumURL: 'public-pool.io',
        fallbackStratumPort: 21496,
        fallbackStratumUser: 'bc1qprimaryworker.axe',
      }), false);
      expect(gate.allowed).toBeFalse();
      expect(gate.reason).toContain('identical');
    });

    it('blocks while the pool form has unsaved edits', () => {
      const gate = poolSwitchGate(infoWith(), true);
      expect(gate.allowed).toBeFalse();
      expect(gate.reason).toContain('unsaved');
    });
  });

  describe('poolSwitchPlan', () => {
    it('plans primary → fallback when the primary is active', () => {
      const plan = poolSwitchPlan(infoWith({ isUsingFallbackStratum: 0 }));
      expect(plan.activeNow.role).toBe('primary');
      expect(plan.activeAfter.role).toBe('fallback');
      expect(plan.standbyAfter.role).toBe('primary');
      expect(plan.patch).toEqual({ useFallbackStratum: true });
    });

    it('plans fallback → primary when the fallback is active', () => {
      const plan = poolSwitchPlan(infoWith({ isUsingFallbackStratum: 1 }));
      expect(plan.activeNow.role).toBe('fallback');
      expect(plan.activeAfter.role).toBe('primary');
      expect(plan.patch).toEqual({ useFallbackStratum: false });
    });

    it('writes ONLY the preference flag — every pool field is preserved by construction', () => {
      const plan = poolSwitchPlan(infoWith());
      expect(Object.keys(plan.patch)).toEqual(['useFallbackStratum']);
    });

    it('never includes credentials in the plan', () => {
      const plan = poolSwitchPlan(infoWith());
      const flat = JSON.stringify(plan);
      expect(flat).not.toContain('assword');
      expect(flat).not.toContain('*****');
    });
  });
});
