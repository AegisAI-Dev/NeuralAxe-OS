import { poolDeckGlance } from './pool-deck';
import { deriveChainContext, ActivePoolRecord, ChainContext } from './pool-chain';
import { PoolSwitchRecord } from './pool-history';
import { systemInfo } from './pool-fixtures';

function bchCtx(): ChainContext {
  const record: ActivePoolRecord = {
    profileId: 'p', profileName: 'BCH Pool', chain: 'BCH',
    primaryHost: 'bch.test', primaryPort: 1, primaryUser: 'u', appliedAt: 1,
  };
  return deriveChainContext(record, systemInfo({ stratumURL: 'bch.test', stratumPort: 1, stratumUser: 'u' }));
}

const lastRecord: PoolSwitchRecord = {
  id: 's1', startedAt: 1, finishedAt: 2,
  sourceProfileId: null, sourceProfileName: 'BTC', sourceChain: 'BTC',
  targetProfileId: 'p', targetProfileName: 'BCH Pool', targetChain: 'BCH',
  changes: [], credentialsReplaced: { primary: false, fallback: false },
  finalState: 'complete', switchVerified: true, rollbackResult: null, restoreResult: null, reason: null, timeline: [],
};

describe('pool-deck', () => {
  it('summarizes an unknown context (no profile)', () => {
    const glance = poolDeckGlance({ context: deriveChainContext(null, systemInfo()), history: [], restoreAvailable: false, switchActive: false });
    expect(glance.chainShort).toBe('Custom / Unknown');
    expect(glance.chainSeverity).toBe('neutral');
    expect(glance.suppressBitcoinMatch).toBeFalse();
    expect(glance.lastSwitch).toBeNull();
  });

  it('summarizes a verified BCH context and suppresses bitcoin match', () => {
    const glance = poolDeckGlance({ context: bchCtx(), history: [lastRecord], restoreAvailable: true, switchActive: false });
    expect(glance.chainShort).toBe('BCH');
    expect(glance.chainSeverity).toBe('info');
    expect(glance.verified).toBeTrue();
    expect(glance.suppressBitcoinMatch).toBeTrue();
    expect(glance.restoreAvailable).toBeTrue();
    expect(glance.lastSwitch?.targetChain).toBe('BCH');
  });

  it('marks a BTC context ok and keeps matching', () => {
    const record: ActivePoolRecord = { profileId: 'p', profileName: 'BTC', chain: 'BTC', primaryHost: 'solo.ckpool.org', primaryPort: 3333, primaryUser: 'bc1qsyntheticbtcaddrexampleonly000000000000.rig1', appliedAt: 1 };
    const glance = poolDeckGlance({ context: deriveChainContext(record, systemInfo()), history: [], restoreAvailable: false, switchActive: false });
    expect(glance.chainSeverity).toBe('ok');
    expect(glance.suppressBitcoinMatch).toBeFalse();
  });

  it('reflects an active switch', () => {
    const glance = poolDeckGlance({ context: bchCtx(), history: [], restoreAvailable: false, switchActive: true });
    expect(glance.switching).toBeTrue();
  });
});
