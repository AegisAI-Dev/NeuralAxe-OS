import {
  UNLABELLED_CONFIG, buildSwitchProvenance, buildRestoreProvenance,
  resolveRestoreActivation, restoreOutcome, transitionProfileLabel,
} from './pool-provenance';
import { deriveChainContext, ActivePoolRecord } from './pool-chain';
import { systemInfo, btcProfile, bchProfile, customProfile } from './pool-fixtures';
import { PoolProfile } from './pool-profile';

const BTC_USER = 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1';

function btcRecord(): ActivePoolRecord {
  return { profileId: 'p-btc', profileName: 'BTC Solo CKPool', chain: 'BTC', primaryHost: 'solo.ckpool.org', primaryPort: 3333, primaryUser: BTC_USER, appliedAt: 1 };
}
/** Device sitting on the BTC profile (systemInfo default). */
function deviceBtc() { return systemInfo(); }
/** A BTC profile whose identity matches the default device. */
function btcProfileMatching(): PoolProfile {
  const p = btcProfile();
  p.id = 'p-btc';
  p.primary = { ...p.primary, host: 'solo.ckpool.org', port: 3333, user: BTC_USER };
  return p;
}
function sourceHosts() {
  return { primaryHost: 'solo.ckpool.org', primaryPort: 3333, fallbackHost: 'backup.example-pool.test', fallbackPort: 3334 };
}

describe('pool-provenance', () => {
  describe('buildSwitchProvenance — immutable source', () => {
    it('records an UNLABELLED source as chain unknown / no profile (the first-switch case)', () => {
      const ctx = deriveChainContext(null, systemInfo());       // Custom/Unknown
      const prov = buildSwitchProvenance({ operationId: 'op1', at: 1, sourceContext: ctx, sourceActive: null, sourceHosts: sourceHosts(), targetProfile: bchProfile(), passwordReplaced: false });
      expect(prov.sourceChain).toBe('unknown');
      expect(prov.sourceProfileId).toBeNull();
      expect(prov.sourceProfileName).toBeNull();
      expect(prov.targetChain).toBe('BCH');
      expect(prov.hadPreviousLabelledProfile).toBeFalse();
    });
    it('records a LABELLED BTC source with its profile id/name/chain', () => {
      const ctx = deriveChainContext(btcRecord(), deviceBtc());  // BTC verified
      const prov = buildSwitchProvenance({ operationId: 'op2', at: 1, sourceContext: ctx, sourceActive: btcRecord(), sourceHosts: sourceHosts(), targetProfile: bchProfile(), passwordReplaced: true });
      expect(prov.sourceChain).toBe('BTC');
      expect(prov.sourceProfileId).toBe('p-btc');
      expect(prov.sourceProfileName).toBe('BTC Solo CKPool');
      expect(prov.targetChain).toBe('BCH');
      expect(prov.passwordReplaced).toBeTrue();
      expect(prov.hadPreviousLabelledProfile).toBeTrue();
    });
    it('masks source hosts and carries no full account / password', () => {
      const ctx = deriveChainContext(btcRecord(), deviceBtc());
      const prov = buildSwitchProvenance({ operationId: 'op', at: 1, sourceContext: ctx, sourceActive: btcRecord(), sourceHosts: sourceHosts(), targetProfile: bchProfile(), passwordReplaced: false });
      const flat = JSON.stringify(prov);
      // The only "password" is the non-secret `passwordReplaced` boolean flag — never a value.
      expect(flat).not.toMatch(/"[a-zA-Z]*[Pp]assword"\s*:\s*"/);
      expect(flat).not.toContain(BTC_USER);
    });
  });

  describe('history transition combinations (Stage 3)', () => {
    const pairs: Array<[any, any, PoolProfile]> = [
      [null, systemInfo(), bchProfile()],                                  // Custom/Unknown → BCH
      [btcRecord(), deviceBtc(), bchProfile()],                            // BTC → BCH
    ];
    it('Custom/Unknown → BCH (never rewritten to BCH → BCH)', () => {
      const prov = buildSwitchProvenance({ operationId: 'o', at: 1, sourceContext: deriveChainContext(null, systemInfo()), sourceActive: null, sourceHosts: sourceHosts(), targetProfile: bchProfile(), passwordReplaced: false });
      expect(prov.sourceChain).toBe('unknown');
      expect(prov.targetChain).toBe('BCH');
      expect(prov.sourceChain === prov.targetChain).toBeFalse();
    });
    it('BTC → BTC same-chain pool change remains valid', () => {
      const prov = buildSwitchProvenance({ operationId: 'o', at: 1, sourceContext: deriveChainContext(btcRecord(), deviceBtc()), sourceActive: btcRecord(), sourceHosts: sourceHosts(), targetProfile: btcProfile(), passwordReplaced: false });
      expect(prov.sourceChain).toBe('BTC');
      expect(prov.targetChain).toBe('BTC');
    });
    it('BCH → BCH same-chain pool change remains valid', () => {
      const bchRec: ActivePoolRecord = { ...btcRecord(), chain: 'BCH', profileName: 'BCH A' };
      const info = systemInfo();
      const prov = buildSwitchProvenance({ operationId: 'o', at: 1, sourceContext: deriveChainContext(bchRec, info), sourceActive: bchRec, sourceHosts: sourceHosts(), targetProfile: bchProfile(), passwordReplaced: false });
      expect(prov.sourceChain).toBe('BCH');
      expect(prov.targetChain).toBe('BCH');
    });
    it('BTC → Custom/Unknown (custom target)', () => {
      const prov = buildSwitchProvenance({ operationId: 'o', at: 1, sourceContext: deriveChainContext(btcRecord(), deviceBtc()), sourceActive: btcRecord(), sourceHosts: sourceHosts(), targetProfile: customProfile(), passwordReplaced: false });
      expect(prov.sourceChain).toBe('BTC');
      expect(prov.targetChain).toBe('custom');
    });
  });

  describe('buildRestoreProvenance — source is current active, target is previous', () => {
    it('BCH active → restore to previous BTC profile', () => {
      const bchRec: ActivePoolRecord = { ...btcRecord(), chain: 'BCH', profileName: 'BCH Pool', profileId: 'p-bch' };
      const prov = buildRestoreProvenance({ operationId: 'r', at: 1, sourceContext: deriveChainContext(bchRec, systemInfo()), sourceActive: bchRec, sourceHosts: sourceHosts(), previousProfileId: 'p-btc', previousProfileName: 'BTC Solo CKPool', previousChain: 'BTC', passwordReplaced: false });
      expect(prov.kind).toBe('restore');
      expect(prov.sourceChain).toBe('BCH');
      expect(prov.targetProfileId).toBe('p-btc');
      expect(prov.targetChain).toBe('BTC');
    });
    it('BCH active → restore to unlabelled original (target unknown)', () => {
      const bchRec: ActivePoolRecord = { ...btcRecord(), chain: 'BCH', profileName: 'BCH Pool', profileId: 'p-bch' };
      const prov = buildRestoreProvenance({ operationId: 'r', at: 1, sourceContext: deriveChainContext(bchRec, systemInfo()), sourceActive: bchRec, sourceHosts: sourceHosts(), previousProfileId: null, previousProfileName: null, previousChain: 'unknown', passwordReplaced: false });
      expect(prov.targetProfileId).toBeNull();
      expect(prov.targetChain).toBe('unknown');
      expect(prov.hadPreviousLabelledProfile).toBeFalse();
    });
  });

  describe('resolveRestoreActivation (Stage 4/5)', () => {
    const restoredIdentity = { host: 'solo.ckpool.org', port: 3333, user: BTC_USER };
    it('A. reactivates the exact previous labelled profile', () => {
      const r = resolveRestoreActivation({ previousProfileId: 'p-btc', previousProfileName: 'BTC Solo CKPool', previousChain: 'BTC', restoredIdentity, liveInfo: deviceBtc(), profiles: [btcProfileMatching()], at: 5 });
      expect(r.status).toBe('exact-profile');
      expect(r.activate?.profileId).toBe('p-btc');
      expect(r.chain).toBe('BTC');
      expect(r.historyTargetChain).toBe('BTC');
    });
    it('B. unlabelled original → Custom/Unknown, nothing activated', () => {
      const r = resolveRestoreActivation({ previousProfileId: null, previousProfileName: null, previousChain: 'unknown', restoredIdentity, liveInfo: deviceBtc(), profiles: [], at: 5 });
      expect(r.status).toBe('unlabelled');
      expect(r.activate).toBeNull();
      expect(r.chain).toBe('unknown');
    });
    it('C. live config mismatch → mismatch, nothing activated', () => {
      const r = resolveRestoreActivation({ previousProfileId: 'p-btc', previousProfileName: 'BTC', previousChain: 'BTC', restoredIdentity, liveInfo: systemInfo({ stratumURL: 'somewhere-else.test' }), profiles: [btcProfileMatching()], at: 5 });
      expect(r.status).toBe('mismatch');
      expect(r.activate).toBeNull();
      expect(r.chain).toBe('unknown');
    });
    it('D. ambiguous stored profiles (previous id unusable) → ambiguous, none chosen', () => {
      const a = btcProfileMatching(); a.id = 'dup-a'; a.name = 'Dup A';
      const b = btcProfileMatching(); b.id = 'dup-b'; b.name = 'Dup B';
      const r = resolveRestoreActivation({ previousProfileId: 'gone', previousProfileName: 'Old', previousChain: 'BTC', restoredIdentity, liveInfo: deviceBtc(), profiles: [a, b], at: 5 });
      expect(r.status).toBe('ambiguous');
      expect(r.activate).toBeNull();
    });
    it('D-pref. exact previous id wins even when other profiles also match', () => {
      const exact = btcProfileMatching();  // id p-btc
      const other = btcProfileMatching(); other.id = 'other'; other.name = 'Other';
      const r = resolveRestoreActivation({ previousProfileId: 'p-btc', previousProfileName: 'BTC', previousChain: 'BTC', restoredIdentity, liveInfo: deviceBtc(), profiles: [exact, other], at: 5 });
      expect(r.status).toBe('exact-profile');
      expect(r.activate?.profileId).toBe('p-btc');
    });
    it('profile-missing → Custom/Unknown when the labelled profile is gone and no identity match', () => {
      const r = resolveRestoreActivation({ previousProfileId: 'gone', previousProfileName: 'Old BTC', previousChain: 'BTC', restoredIdentity, liveInfo: deviceBtc(), profiles: [], at: 5 });
      expect(r.status).toBe('profile-missing');
      expect(r.chain).toBe('unknown');
    });
    it('no telemetry → mismatch', () => {
      const r = resolveRestoreActivation({ previousProfileId: 'p-btc', previousProfileName: 'BTC', previousChain: 'BTC', restoredIdentity, liveInfo: null, profiles: [btcProfileMatching()], at: 5 });
      expect(r.status).toBe('mismatch');
    });
  });

  describe('restoreOutcome wording (Stage 6)', () => {
    it('verified + password not replaced → exact', () => {
      const v = restoreOutcome('verified', false, false);
      expect(v.category).toBe('exact');
      expect(v.severity).toBe('ok');
    });
    it('verified + replaced + secret unavailable → operational (honest, not alarming)', () => {
      const v = restoreOutcome('verified', true, false);
      expect(v.category).toBe('operational');
      expect(v.severity).toBe('ok');
      expect(v.detail).toContain('write-only');
      expect(v.detail).toContain('cannot prove');
    });
    it('verified + replaced + secret available (in-session rollback) → exact', () => {
      expect(restoreOutcome('verified', true, true).category).toBe('exact');
    });
    it('partial → warn, failed → err', () => {
      expect(restoreOutcome('partial', false, false).category).toBe('partial');
      expect(restoreOutcome('partial', false, false).severity).toBe('warn');
      expect(restoreOutcome('failed', false, false).category).toBe('failed');
      expect(restoreOutcome('failed', false, false).severity).toBe('err');
    });
    it('never claims byte-for-byte password restoration in the operational case', () => {
      expect(restoreOutcome('verified', true, false).detail.toLowerCase()).not.toContain('byte');
    });
  });

  describe('transitionProfileLabel', () => {
    it('labels null / empty as "Unlabelled configuration"', () => {
      expect(transitionProfileLabel(null)).toBe(UNLABELLED_CONFIG);
      expect(transitionProfileLabel('')).toBe(UNLABELLED_CONFIG);
      expect(transitionProfileLabel('BTC Solo')).toBe('BTC Solo');
    });
  });
});
