import { BehaviorSubject } from 'rxjs';
import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { LocalStorageService } from '../local-storage.service';
import { PoolStrategyService } from './pool-strategy.service';
import { ActivePoolRecord } from '../components/pool-strategy/pool-chain';
import { buildRestoreSnapshot, captureConfig, PoolSwitchRecord } from '../components/pool-strategy/pool-history';
import { btcProfile, systemInfo } from '../components/pool-strategy/pool-fixtures';

const KEYS = ['NX_POOL_PROFILES', 'NX_POOL_ACTIVE', 'NX_POOL_RESTORE_SNAPSHOT', 'NX_POOL_SWITCH_HISTORY', 'NX_POOL_SWITCH_ACTIVE'];

function makeService(info$: BehaviorSubject<ISystemInfo>): PoolStrategyService {
  const liveData = { info$ } as any;
  return new PoolStrategyService(liveData, new LocalStorageService());
}

function btcRecord(): ActivePoolRecord {
  return {
    profileId: 'p1', profileName: 'BTC Solo CKPool', chain: 'BTC',
    primaryHost: 'solo.ckpool.org', primaryPort: 3333,
    primaryUser: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1', appliedAt: 1,
  };
}

describe('PoolStrategyService', () => {
  let info$: BehaviorSubject<ISystemInfo>;
  let service: PoolStrategyService;

  beforeEach(() => {
    KEYS.forEach(k => window.localStorage.removeItem(k));
    info$ = new BehaviorSubject<ISystemInfo>(systemInfo());
    service = makeService(info$);
  });

  afterEach(() => KEYS.forEach(k => window.localStorage.removeItem(k)));

  it('creates and adds / updates / removes profiles', () => {
    const p = service.addProfile({ name: 'A', chain: 'BTC', primary: btcProfile().primary, fallback: null });
    expect(service.profiles.length).toBe(1);
    expect(p.id).toBeTruthy();

    service.updateProfile({ ...p, name: 'A2' });
    expect(service.profiles[0].name).toBe('A2');

    service.removeProfile(p.id);
    expect(service.profiles.length).toBe(0);
  });

  it('persists profiles across a new instance', () => {
    service.addProfile({ name: 'Persisted', chain: 'BCH', primary: btcProfile().primary, fallback: null });
    const reborn = makeService(info$);
    expect(reborn.profiles.map(p => p.name)).toContain('Persisted');
  });

  it('derives a verified chain context from the active record + live info', (done) => {
    service.setActiveRecord(btcRecord());
    service.chainContext$.subscribe(ctx => {
      if (ctx.labelled) {
        expect(ctx.chain).toBe('BTC');
        expect(ctx.verified).toBeTrue();
        done();
      }
    });
  });

  it('re-derives to Unknown when the device drifts from the applied profile', () => {
    service.setActiveRecord(btcRecord());
    expect(service.chainContext().chain).toBe('BTC');
    info$.next(systemInfo({ stratumURL: 'changed.test' }));
    expect(service.chainContext().chain).toBe('unknown');
  });

  it('clears the active record → Unknown', () => {
    service.setActiveRecord(btcRecord());
    service.clearActiveRecord();
    expect(service.chainContext().labelled).toBeFalse();
  });

  it('stores, reads and expires the restore snapshot', () => {
    const snap = buildRestoreSnapshot({ config: captureConfig(systemInfo()), now: Date.now(), fromProfileName: 'x', passwordWasReplaced: false });
    service.setRestoreSnapshot(snap);
    expect(service.getRestoreSnapshot()).toBeTruthy();
    // An already-expired snapshot is not returned and is cleared.
    const expired = buildRestoreSnapshot({ config: captureConfig(systemInfo()), now: Date.now() - 10, fromProfileName: 'x', passwordWasReplaced: false, ttlMs: 1 });
    service.setRestoreSnapshot(expired);
    expect(service.getRestoreSnapshot()).toBeNull();
    expect(window.localStorage.getItem('NX_POOL_RESTORE_SNAPSHOT')).toBe('null');
  });

  it('saves, lists and removes sanitized history', () => {
    const record = { id: 'r1', startedAt: 1, finishedAt: 2, sourceProfileName: null, sourceChain: 'BTC', targetProfileName: 'BCH', targetChain: 'BCH', sourceProfileId: null, targetProfileId: null, changes: [], credentialsReplaced: { primary: false, fallback: false }, finalState: 'complete', switchVerified: true, rollbackResult: null, restoreResult: null, reason: null, timeline: [] } as PoolSwitchRecord;
    service.saveHistory(record);
    expect(service.listHistory().length).toBe(1);
    service.removeHistory('r1');
    expect(service.listHistory().length).toBe(0);
  });

  it('stores and clears a NON-SECRET interruption record', () => {
    expect(service.getInterruptionRecord()).toBeNull();
    const record = {
      sessionId: 's1', at: 1, targetProfileName: 'BCH', targetChain: 'BCH' as const,
      original: captureConfig(systemInfo()),
      target: { host: 'bch.test', port: 3334, user: 'x' },
      passwordWasReplaced: true,
    };
    service.setInterruptionRecord(record);
    expect(service.wasSwitchInterrupted()).toBeTrue();
    expect(service.getInterruptionRecord()?.targetProfileName).toBe('BCH');
    // No password anywhere in the persisted record.
    expect((window.localStorage.getItem('NX_POOL_SWITCH_ACTIVE') || '').toLowerCase()).not.toContain('password"');
    service.clearSwitchActive();
    expect(service.getInterruptionRecord()).toBeNull();
    expect(service.wasSwitchInterrupted()).toBeFalse();
  });
});
