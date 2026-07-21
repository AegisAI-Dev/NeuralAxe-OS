import {
  SwitchEvent, SwitchSnapshot, initialSwitchSnapshot, isSwitchActive, isSwitchTerminal,
  nextSwitchState, reduceSwitch, reduceSwitchAll, switchStateExplanation, switchStateLabel,
} from './pool-switch-machine';

const ev = (type: SwitchEvent['type'], extra: Partial<SwitchEvent> = {}): SwitchEvent => ({ type, at: 1, ...extra });

function run(events: SwitchEvent['type'][], seed?: SwitchSnapshot): SwitchSnapshot {
  return reduceSwitchAll(seed ?? initialSwitchSnapshot(), events.map(t => ev(t)));
}

const HAPPY: SwitchEvent['type'][] = [
  'START_PREFLIGHT', 'PREFLIGHT_OK', 'CONFIRM', 'CAPTURED', 'APPLY_RESTART',
  'RESTART_SENT', 'RECONNECTED', 'VERIFY_OK', 'COMPLETE',
];

describe('pool-switch-machine', () => {
  it('starts idle with a seeded timeline', () => {
    const s = initialSwitchSnapshot();
    expect(s.state).toBe('idle');
    expect(s.timeline.length).toBe(1);
  });

  it('runs the full happy path to complete and marks it verified', () => {
    const s = run(HAPPY);
    expect(s.state).toBe('complete');
    expect(s.switchVerified).toBeTrue();
    expect(s.timeline[s.timeline.length - 1].state).toBe('complete');
  });

  it('records the target profile on CONFIRM', () => {
    const s = reduceSwitchAll(initialSwitchSnapshot(), [
      ev('START_PREFLIGHT'), ev('PREFLIGHT_OK'),
      ev('CONFIRM', { profileId: 'p1', profileName: 'BCH Pool', chain: 'BCH' }),
    ]);
    expect(s.state).toBe('capturing');
    expect(s.targetProfileId).toBe('p1');
    expect(s.targetProfileName).toBe('BCH Pool');
    expect(s.targetChain).toBe('BCH');
  });

  it('applies live (no restart) straight to verifying', () => {
    const s = run(['START_PREFLIGHT', 'PREFLIGHT_OK', 'CONFIRM', 'CAPTURED', 'APPLY_APPLIED']);
    expect(s.state).toBe('verifying');
  });

  it('rolls back on a failed verification and ends failed', () => {
    let s = run(['START_PREFLIGHT', 'PREFLIGHT_OK', 'CONFIRM', 'CAPTURED', 'APPLY_RESTART', 'RESTART_SENT', 'RECONNECTED']);
    s = reduceSwitch(s, ev('VERIFY_FAIL', { reason: 'no mining' }));
    expect(s.state).toBe('rolling-back');
    s = reduceSwitch(s, ev('ROLLBACK_OK'));
    expect(s.state).toBe('failed');
    expect(s.rollbackResult).toBe('verified');
    expect(s.switchVerified).toBeFalse();
  });

  it('records a partial and a failed rollback', () => {
    const base = run(['START_PREFLIGHT', 'PREFLIGHT_OK', 'CONFIRM', 'CAPTURED', 'APPLY_FAIL']);
    expect(base.state).toBe('rolling-back');
    expect(reduceSwitch(base, ev('ROLLBACK_PARTIAL')).rollbackResult).toBe('partial');
    expect(reduceSwitch(base, ev('ROLLBACK_FAIL')).rollbackResult).toBe('failed');
  });

  it('handles reconnect timeout → rollback', () => {
    let s = run(['START_PREFLIGHT', 'PREFLIGHT_OK', 'CONFIRM', 'CAPTURED', 'APPLY_RESTART', 'RESTART_SENT']);
    s = reduceSwitch(s, ev('RECONNECT_TIMEOUT', { reason: 'timeout' }));
    expect(s.state).toBe('rolling-back');
    expect(s.reason).toBe('timeout');
  });

  it('fails cleanly when capture fails (no rollback needed)', () => {
    const s = run(['START_PREFLIGHT', 'PREFLIGHT_OK', 'CONFIRM', 'CAPTURE_FAIL']);
    expect(s.state).toBe('failed');
    expect(s.rollbackResult).toBeNull();
  });

  it('aborts before any change from capturing', () => {
    const s = run(['START_PREFLIGHT', 'PREFLIGHT_OK', 'CONFIRM', 'ABORT']);
    expect(s.state).toBe('aborted');
  });

  it('routes an abort during verifying through rollback', () => {
    let s = run(['START_PREFLIGHT', 'PREFLIGHT_OK', 'CONFIRM', 'CAPTURED', 'APPLY_APPLIED']);
    s = reduceSwitch(s, ev('ABORT'));
    expect(s.state).toBe('rolling-back');
  });

  it('supports manual restore from complete and from idle', () => {
    const complete = run(HAPPY);
    let s = reduceSwitch(complete, ev('RESTORE_START', { profileName: 'x' }));
    expect(s.state).toBe('restoring');
    expect(reduceSwitch(s, ev('RESTORE_OK')).state).toBe('complete');
    expect(reduceSwitch(s, ev('RESTORE_FAIL')).state).toBe('failed');

    const fromIdle = reduceSwitch(initialSwitchSnapshot(), ev('RESTORE_START'));
    expect(fromIdle.state).toBe('restoring');
  });

  it('records restore results', () => {
    const restoring = reduceSwitch(run(HAPPY), ev('RESTORE_START'));
    expect(reduceSwitch(restoring, ev('RESTORE_OK')).restoreResult).toBe('verified');
    expect(reduceSwitch(restoring, ev('RESTORE_PARTIAL')).restoreResult).toBe('partial');
  });

  it('allows retry from failed', () => {
    let s = run(['START_PREFLIGHT', 'PREFLIGHT_OK', 'CONFIRM', 'CAPTURED', 'APPLY_FAIL', 'ROLLBACK_FAIL']);
    expect(s.state).toBe('failed');
    expect(reduceSwitch(s, ev('RETRY_ROLLBACK')).state).toBe('rolling-back');
    expect(reduceSwitch(s, ev('RETRY_RESTORE')).state).toBe('restoring');
  });

  it('ignores invalid transitions as no-ops', () => {
    const idle = initialSwitchSnapshot();
    const after = reduceSwitch(idle, ev('APPLY_APPLIED'));
    expect(after).toBe(idle);
    expect(nextSwitchState('idle', 'APPLY_APPLIED')).toBeNull();
  });

  it('interrupts from any active state but not from terminal', () => {
    const active = run(['START_PREFLIGHT', 'PREFLIGHT_OK', 'CONFIRM', 'CAPTURED']);
    expect(reduceSwitch(active, ev('INTERRUPT')).state).toBe('interrupted');
    const complete = run(HAPPY);
    expect(reduceSwitch(complete, ev('INTERRUPT'))).toBe(complete);
  });

  it('resets from any terminal to idle', () => {
    expect(reduceSwitch(run(HAPPY), ev('RESET')).state).toBe('idle');
  });

  it('classifies active and terminal states', () => {
    expect(isSwitchActive('verifying')).toBeTrue();
    expect(isSwitchActive('idle')).toBeFalse();
    expect(isSwitchTerminal('complete')).toBeTrue();
    expect(isSwitchTerminal('reconnecting')).toBeFalse();
  });

  it('provides a label and explanation for every state', () => {
    (['idle', 'capturing', 'rolling-back', 'restoring', 'complete', 'failed', 'interrupted'] as const).forEach(st => {
      expect(switchStateLabel(st).length).toBeGreaterThan(0);
      expect(switchStateExplanation(st).length).toBeGreaterThan(0);
    });
  });

  it('never carries a credential in the snapshot', () => {
    const s = run(HAPPY);
    expect(JSON.stringify(s)).not.toMatch(/password|wallet|secret/i);
  });
});
