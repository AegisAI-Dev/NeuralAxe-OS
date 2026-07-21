import { poolPreflight, PoolPreflightInput } from './pool-preflight';
import { derivePairStatus } from 'src/app/services/version-state';
import { btcProfile, deviceEmergency, deviceStockAxeos, deviceUnsupportedBoard, systemInfo } from './pool-fixtures';
import { validateProfile } from './pool-profile';

const okPair = derivePairStatus('v2.14.2-39-gb3a16002', 'v2.14.2-39-gb3a16002', 'v2.14.2-39-gb3a16002');

function baseInput(overrides: Partial<PoolPreflightInput> = {}): PoolPreflightInput {
  const profile = btcProfile();
  return {
    info: systemInfo(),
    online: true,
    pairStatus: okPair,
    profile,
    profileErrors: validateProfile(profile),
    reviewHasChange: true,
    originalCaptured: true,
    stabilityActive: false,
    otherSwitchActive: false,
    ...overrides,
  };
}

function blockerIds(input: PoolPreflightInput): string[] {
  return poolPreflight(input).blockers.map(b => b.id);
}

describe('pool-preflight', () => {
  it('passes when every applicable check passes', () => {
    const result = poolPreflight(baseInput());
    expect(result.canStart).toBeTrue();
    expect(result.blockers).toEqual([]);
    expect(result.supported.supported).toBeTrue();
  });

  it('blocks a stock AxeOS device (unsupported)', () => {
    const result = poolPreflight(baseInput({ info: deviceStockAxeos() }));
    expect(result.canStart).toBeFalse();
    expect(result.supported.supported).toBeFalse();
  });

  it('blocks an unsupported board', () => {
    expect(blockerIds(baseInput({ info: deviceUnsupportedBoard() }))).toContain('supported');
  });

  it('blocks when offline', () => {
    expect(blockerIds(baseInput({ online: false }))).toContain('online');
  });

  it('blocks on an emergency override', () => {
    expect(blockerIds(baseInput({ info: deviceEmergency() }))).toContain('emergency');
  });

  it('blocks while a Stability Lab session is active', () => {
    expect(blockerIds(baseInput({ stabilityActive: true }))).toContain('no-stability');
  });

  it('blocks while another switch is active', () => {
    expect(blockerIds(baseInput({ otherSwitchActive: true }))).toContain('no-other-switch');
  });

  it('blocks when the original configuration is not captured', () => {
    expect(blockerIds(baseInput({ originalCaptured: false }))).toContain('captured');
  });

  it('blocks when the target profile is invalid', () => {
    const bad = btcProfile();
    bad.name = '';
    expect(blockerIds(baseInput({ profile: bad, profileErrors: validateProfile(bad) }))).toContain('profile-valid');
  });

  it('blocks when there is no profile selected', () => {
    const ids = blockerIds(baseInput({ profile: null, profileErrors: [] }));
    expect(ids).toContain('profile-valid');
    expect(ids).toContain('credentials');
  });

  it('blocks when the target does not differ from the current configuration', () => {
    expect(blockerIds(baseInput({ reviewHasChange: false }))).toContain('differs');
  });

  it('blocks with an unknown firmware/web pair', () => {
    expect(blockerIds(baseInput({ pairStatus: null }))).toContain('pair');
  });

  it('marks device checks not applicable on an unsupported device', () => {
    const result = poolPreflight(baseInput({ info: deviceStockAxeos() }));
    const online = result.checks.find(c => c.id === 'online');
    expect(online?.applicable).toBeFalse();
  });

  it('every check has an explanation and there is no opaque score', () => {
    const result = poolPreflight(baseInput({ online: false }));
    result.checks.forEach(c => expect(c.detail.length).toBeGreaterThan(0));
    expect((result as any).score).toBeUndefined();
  });
});
