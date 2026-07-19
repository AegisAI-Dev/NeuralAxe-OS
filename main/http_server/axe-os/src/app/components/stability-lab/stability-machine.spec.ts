import {
  LabEvent,
  LabState,
  initialSnapshot,
  nextState,
  reduce,
  reduceAll,
  isTerminal,
  isActive,
  stateLabel,
  stateExplanation,
} from './stability-machine';

const ev = (type: LabEvent['type'], at = 0, reason?: string): LabEvent => ({ type, at, reason });

/** Drive one profile through a live-apply (no restart) happy path. */
function runOneProfile(startAt: number) {
  return [
    ev('APPLY_APPLIED', startAt),
    ev('WARMUP_DONE', startAt + 1),
    ev('MEASURE_DONE', startAt + 2),
    ev('COOLDOWN_DONE', startAt + 3),
  ];
}

describe('nextState (structural validity)', () => {
  it('allows only the defined transitions out of idle', () => {
    expect(nextState('idle', 'START_PREFLIGHT')).toBe('preflight');
    expect(nextState('idle', 'CONFIRM')).toBeNull();
    expect(nextState('idle', 'WARMUP_DONE')).toBeNull();
  });

  it('routes applying by restart requirement', () => {
    expect(nextState('applying', 'APPLY_APPLIED')).toBe('warmup');
    expect(nextState('applying', 'APPLY_RESTART')).toBe('restarting');
  });

  it('routes cooldown to the next profile or to restore based on the queue', () => {
    const mid = { ...initialSnapshot(3), state: 'cooldown' as LabState, profileIndex: 0 };
    expect(nextState('cooldown', 'COOLDOWN_DONE', mid)).toBe('applying');
    const last = { ...initialSnapshot(3), state: 'cooldown' as LabState, profileIndex: 2 };
    expect(nextState('cooldown', 'COOLDOWN_DONE', last)).toBe('restoring');
  });

  it('routes restore to the terminal that matches why restoring began', () => {
    const complete = { ...initialSnapshot(1), state: 'restoring' as LabState, restoreOutcome: 'complete' as const };
    const aborted = { ...initialSnapshot(1), state: 'restoring' as LabState, restoreOutcome: 'aborted' as const };
    const failed = { ...initialSnapshot(1), state: 'restoring' as LabState, restoreOutcome: 'failed' as const };
    expect(nextState('restoring', 'RESTORE_OK', complete)).toBe('complete');
    expect(nextState('restoring', 'RESTORE_OK', aborted)).toBe('aborted');
    expect(nextState('restoring', 'RESTORE_OK', failed)).toBe('failed');
    expect(nextState('restoring', 'RESTORE_FAIL', complete)).toBe('failed');
  });

  it('never transitions out of a terminal state except via RESET', () => {
    for (const t of ['complete', 'aborted', 'failed', 'interrupted'] as LabState[]) {
      expect(nextState(t, 'CONFIRM')).toBeNull();
      expect(nextState(t, 'WARMUP_DONE')).toBeNull();
      expect(nextState(t, 'RESET')).toBe('idle');
      expect(nextState(t, 'INTERRUPT')).toBeNull();
    }
  });

  it('allows INTERRUPT from any active state', () => {
    for (const s of ['warmup', 'measuring', 'applying', 'reconnecting', 'restoring'] as LabState[]) {
      expect(nextState(s, 'INTERRUPT')).toBe('interrupted');
    }
  });
});

describe('reduce (full happy path)', () => {
  it('runs a single-profile session to complete and restores the original', () => {
    let snap = initialSnapshot(1);
    const events: LabEvent[] = [
      ev('START_PREFLIGHT', 0),
      ev('PREFLIGHT_OK', 1),
      ev('CONFIRM', 2),
      ...runOneProfile(3),
      ev('RESTORE_OK', 8),
    ];
    snap = reduceAll(snap, events);
    expect(snap.state).toBe('complete');
    expect(snap.restoreOutcome).toBe('complete');
    expect(snap.restoreResult).toBe('ok');
    // Timeline is timestamped and never skips a state silently.
    const states = snap.timeline.map(t => t.state);
    expect(states).toContain('warmup');
    expect(states).toContain('measuring');
    expect(states).toContain('restoring');
    expect(snap.timeline.every(t => typeof t.at === 'number')).toBeTrue();
  });

  it('runs multiple profiles in order, incrementing the profile index', () => {
    let snap = initialSnapshot(2);
    snap = reduceAll(snap, [ev('START_PREFLIGHT'), ev('PREFLIGHT_OK'), ev('CONFIRM')]);
    expect(snap.profileIndex).toBe(0);
    snap = reduceAll(snap, runOneProfile(10)); // profile 1 done → advance
    expect(snap.state).toBe('applying');
    expect(snap.profileIndex).toBe(1);
    snap = reduceAll(snap, runOneProfile(20)); // profile 2 done → restore
    expect(snap.state).toBe('restoring');
    snap = reduce(snap, ev('RESTORE_OK', 30));
    expect(snap.state).toBe('complete');
  });

  it('handles a restart-required profile through restarting/reconnecting', () => {
    let snap = initialSnapshot(1);
    snap = reduceAll(snap, [ev('START_PREFLIGHT'), ev('PREFLIGHT_OK'), ev('CONFIRM')]);
    snap = reduceAll(snap, [ev('APPLY_RESTART', 1), ev('RESTART_SENT', 2), ev('RECONNECTED', 3)]);
    expect(snap.state).toBe('warmup');
    const states = snap.timeline.map(t => t.state);
    expect(states).toContain('restarting');
    expect(states).toContain('reconnecting');
  });
});

describe('reduce (abort / stop / failure)', () => {
  it('an automatic stop during measuring restores and ends aborted with a reason', () => {
    let snap = initialSnapshot(1);
    snap = reduceAll(snap, [ev('START_PREFLIGHT'), ev('PREFLIGHT_OK'), ev('CONFIRM'), ev('APPLY_APPLIED'), ev('WARMUP_DONE')]);
    expect(snap.state).toBe('measuring');
    snap = reduce(snap, ev('STOP', 100, 'ASIC 70 °C exceeded 68 °C stop limit'));
    expect(snap.state).toBe('restoring');
    expect(snap.restoreOutcome).toBe('aborted');
    expect(snap.reason).toContain('68 °C');
    snap = reduce(snap, ev('RESTORE_OK', 101));
    expect(snap.state).toBe('aborted');
  });

  it('an owner abort during warm-up restores and ends aborted', () => {
    let snap = initialSnapshot(1);
    snap = reduceAll(snap, [ev('START_PREFLIGHT'), ev('PREFLIGHT_OK'), ev('CONFIRM'), ev('APPLY_APPLIED')]);
    snap = reduce(snap, ev('ABORT', 50, 'Owner pressed Abort'));
    expect(snap.state).toBe('restoring');
    expect(snap.restoreOutcome).toBe('aborted');
  });

  it('an apply failure attempts restore then ends failed', () => {
    let snap = initialSnapshot(1);
    snap = reduceAll(snap, [ev('START_PREFLIGHT'), ev('PREFLIGHT_OK'), ev('CONFIRM')]);
    snap = reduce(snap, ev('APPLY_FAIL', 5, 'PATCH /api/system failed'));
    expect(snap.state).toBe('restoring');
    expect(snap.restoreOutcome).toBe('failed');
    snap = reduce(snap, ev('RESTORE_OK', 6));
    expect(snap.state).toBe('failed');
    expect(snap.restoreResult).toBe('ok'); // restore itself succeeded even though the session failed
  });

  it('a reconnect timeout ends failed after restore', () => {
    let snap = initialSnapshot(1);
    snap = reduceAll(snap, [ev('START_PREFLIGHT'), ev('PREFLIGHT_OK'), ev('CONFIRM'), ev('APPLY_RESTART'), ev('RESTART_SENT')]);
    snap = reduce(snap, ev('RECONNECT_TIMEOUT', 60, 'No telemetry after 120 s'));
    expect(snap.state).toBe('restoring');
    expect(snap.restoreOutcome).toBe('failed');
  });

  it('a failed restore ends failed and records the restore result', () => {
    let snap = initialSnapshot(1);
    snap = reduceAll(snap, [ev('START_PREFLIGHT'), ev('PREFLIGHT_OK'), ev('CONFIRM'), ...runOneProfile(1)]);
    expect(snap.state).toBe('restoring');
    snap = reduce(snap, ev('RESTORE_FAIL', 20, 'Device unreachable during restore'));
    expect(snap.state).toBe('failed');
    expect(snap.restoreResult).toBe('failed');
  });
});

describe('reduce (invalid transitions and interruption)', () => {
  it('is a no-op on an invalid transition (no silent corruption)', () => {
    const snap = initialSnapshot(1);
    const after = reduce(snap, ev('WARMUP_DONE', 1)); // not valid from idle
    expect(after).toBe(snap); // unchanged reference
  });

  it('an interrupted run becomes "operator session interrupted", never complete', () => {
    let snap = initialSnapshot(1);
    snap = reduceAll(snap, [ev('START_PREFLIGHT'), ev('PREFLIGHT_OK'), ev('CONFIRM'), ev('APPLY_APPLIED'), ev('WARMUP_DONE')]);
    snap = reduce(snap, ev('INTERRUPT', 999));
    expect(snap.state).toBe('interrupted');
    expect(snap.state).not.toBe('complete');
    expect(stateLabel('interrupted')).toContain('interrupted');
  });

  it('RESET returns a clean idle snapshot', () => {
    let snap = initialSnapshot(2);
    snap = reduceAll(snap, [ev('START_PREFLIGHT'), ev('PREFLIGHT_OK'), ev('CONFIRM'), ...runOneProfile(1), ...runOneProfile(10), ev('RESTORE_OK', 20)]);
    expect(snap.state).toBe('complete');
    snap = reduce(snap, ev('RESET', 21));
    expect(snap.state).toBe('idle');
    expect(snap.profileIndex).toBe(0);
    expect(snap.reason).toBeNull();
  });
});

describe('helpers', () => {
  it('classifies terminal and active states', () => {
    expect(isTerminal('complete')).toBeTrue();
    expect(isTerminal('warmup')).toBeFalse();
    expect(isActive('measuring')).toBeTrue();
    expect(isActive('idle')).toBeFalse();
  });
  it('provides a label and explanation for every state', () => {
    for (const s of ['idle', 'warmup', 'measuring', 'complete', 'aborted', 'failed', 'interrupted'] as LabState[]) {
      expect(stateLabel(s).length).toBeGreaterThan(0);
      expect(stateExplanation(s).length).toBeGreaterThan(0);
    }
  });
});
