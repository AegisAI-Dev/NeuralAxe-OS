import { deriveVersionState } from './version-state';

describe('deriveVersionState', () => {
  const FW = 'v2.14.2-15-g723e61dc';
  const OLD = 'v2.14.2-13-g388287da';

  it('reports a matching pair when firmware and live web revision agree', () => {
    const state = deriveVersionState(FW, FW, FW);
    expect(state.status).toBe('match');
    expect(state.installedWeb).toBe(FW);
    expect(state.webSource).toBe('live');
    expect(state.restartPending).toBeFalse();
  });

  it('reports a genuine mismatch when the live web revision differs from firmware', () => {
    const state = deriveVersionState(FW, OLD, OLD);
    expect(state.status).toBe('mismatch');
    expect(state.installedWeb).toBe(OLD);
    expect(state.restartPending).toBeFalse();
  });

  it('flags restart pending for a stale boot snapshot (www OTA without restart)', () => {
    // The exact state observed on the real Gamma: firmware -15, live web -15,
    // but the firmware still reports the -13 it saw at boot.
    const state = deriveVersionState(FW, OLD, FW);
    expect(state.status).toBe('match');       // installed artifacts DO match
    expect(state.restartPending).toBeTrue();  // snapshot refresh needs a restart
    expect(state.installedWeb).toBe(FW);
    expect(state.bootWeb).toBe(OLD);
  });

  it('never fakes equality when the live web revision is unavailable', () => {
    const state = deriveVersionState(FW, FW, null);
    expect(state.status).toBe('unverified');
    expect(state.installedWeb).toBeNull();     // boot snapshot is NOT substituted
    expect(state.webSource).toBe('boot-snapshot');
    expect(state.restartPending).toBeFalse();
  });

  it('labels the boot snapshot as such when it is the only observation', () => {
    const state = deriveVersionState(FW, OLD, null);
    expect(state.status).toBe('unverified');
    expect(state.bootWeb).toBe(OLD);
    expect(state.webSource).toBe('boot-snapshot');
  });

  it('reconciles after a restart: snapshot catches up and the pair matches', () => {
    const before = deriveVersionState(FW, OLD, FW);
    expect(before.restartPending).toBeTrue();
    const after = deriveVersionState(FW, FW, FW);
    expect(after.restartPending).toBeFalse();
    expect(after.status).toBe('match');
  });

  it('returns unknown when nothing usable is reported', () => {
    expect(deriveVersionState(null, null, null).status).toBe('unknown');
    expect(deriveVersionState(undefined, '', null).status).toBe('unknown');
    expect(deriveVersionState('', FW, FW).status).toBe('unknown');
  });

  it('treats blank and whitespace-only values as absent', () => {
    const state = deriveVersionState(`  ${FW}  `, '   ', null);
    expect(state.firmware).toBe(FW);
    expect(state.bootWeb).toBeNull();
    expect(state.webSource).toBe('none');
    // No web observation at all — not even a boot snapshot — is 'unknown'.
    expect(state.status).toBe('unknown');
  });

  it('restart pending survives alongside a genuine mismatch', () => {
    // live web -13 (differs from firmware) AND boot snapshot older still.
    const state = deriveVersionState(FW, 'v2.14.2', OLD);
    expect(state.status).toBe('mismatch');
    expect(state.restartPending).toBeTrue();
  });
});
