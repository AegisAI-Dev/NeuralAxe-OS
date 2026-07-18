import { deriveVersionState, derivePairStatus } from './version-state';

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

describe('derivePairStatus (Phase 2J.1 honest pair status)', () => {
  const FW = 'v2.14.2-29-gbbad2369';
  const OLD = 'v2.14.2-13-g388287da';

  it('live exact match -> live-match, ok, live-verified', () => {
    const p = derivePairStatus(FW, FW, FW);
    expect(p.state).toBe('live-match');
    expect(p.severity).toBe('ok');
    expect(p.liveVerified).toBeTrue();
    expect(p.primary).toContain('Live');
  });

  it('live mismatch -> live-mismatch, danger, live-verified', () => {
    const p = derivePairStatus(FW, FW, OLD);
    expect(p.state).toBe('live-mismatch');
    expect(p.severity).toBe('danger');
    expect(p.liveVerified).toBeTrue();
  });

  it('the owner-observed state (live missing, boot == firmware) -> boot-match, NOT alarming', () => {
    const p = derivePairStatus(FW, FW, null);
    expect(p.state).toBe('boot-match');
    expect(p.severity).toBe('ok');           // proven match, styled calmly
    expect(p.liveVerified).toBeFalse();       // but NOT claimed as live-verified
    expect(p.bootVerified).toBeTrue();
    expect(p.primary).toBe('Boot pair match');
    expect(p.secondary).toContain('Live verification unavailable');
  });

  it('live missing + boot mismatch -> boot-mismatch, warn, not hidden', () => {
    const p = derivePairStatus(FW, OLD, null);
    expect(p.state).toBe('boot-mismatch');
    expect(p.severity).toBe('warn');
    expect(p.liveVerified).toBeFalse();
    expect(p.bootVerified).toBeTrue();
  });

  it('firmware missing -> unknown', () => {
    const p = derivePairStatus(null, FW, FW);
    expect(p.state).toBe('unknown');
    expect(p.severity).toBe('info');
    expect(p.liveVerified).toBeFalse();
    expect(p.bootVerified).toBeFalse();
  });

  it('boot web missing (and live missing) -> unknown', () => {
    expect(derivePairStatus(FW, null, null).state).toBe('unknown');
  });

  it('all values missing -> unknown', () => {
    expect(derivePairStatus(null, null, null).state).toBe('unknown');
    expect(derivePairStatus(undefined, '', '   ').state).toBe('unknown');
  });

  it('normalizes whitespace but preserves the exact revision', () => {
    const p = derivePairStatus(`  ${FW}  `, `  ${FW} `, null);
    expect(p.state).toBe('boot-match');
    expect(p.bootVerified).toBeTrue();
  });

  it('treats -dirty as a significant, distinguishing suffix', () => {
    // clean firmware vs a dirty live web build = a genuine live mismatch
    const p = derivePairStatus(FW, FW, `${FW}-dirty`);
    expect(p.state).toBe('live-mismatch');
    expect(p.severity).toBe('danger');
  });

  it('similar-but-not-identical revisions are a mismatch, not a match', () => {
    const near = 'v2.14.2-29-gbbad2360'; // last hex digit differs
    expect(derivePairStatus(FW, FW, near).state).toBe('live-mismatch');
    expect(derivePairStatus(FW, near, null).state).toBe('boot-mismatch');
  });

  it('never represents a boot match as a live match', () => {
    const p = derivePairStatus(FW, FW, null);
    expect(p.state).not.toBe('live-match');
    expect(p.liveVerified).toBeFalse();
  });
});
