/**
 * Gate B9 — PURE timeline tests.
 *
 * The timeline describes what the device reported; it never predicts, and no
 * stage is ever completed because time passed.
 */

import { TimedSessionStatus } from 'src/app/generated/models';
import { buildTimeline } from './timed-session-timeline';
import { TimelineStageId, TimelineStageState } from './timed-session.models';

function status(overrides: Partial<TimedSessionStatus> = {}): TimedSessionStatus {
  return {
    apiEnabled: true, runtimeInitialized: true, executionEnabled: true,
    sessionPresent: true, terminalResultPending: false,
    durableState: 'IDLE', runtimeState: 'runtime_free', executionState: 'EXEC_STATE_IDLE',
    leaseOwner: 'OWNER_NONE', leasePhase: 'PHASE_FREE',
    protocolStartPermitted: true, asicGate: 'GATE_DEFAULT_OPEN',
    targetMiningGrantActive: false, restoreRequired: false, operatorRecoveryRequired: false,
    deadlineStatus: 'UNKNOWN', heartbeatStatus: 'NOT_APPLICABLE', statusSequence: 1,
    ...overrides,
  };
}

function stateOf(s: TimedSessionStatus, id: TimelineStageId): TimelineStageState {
  return buildTimeline(s).stages.find((x) => x.id === id)!.state;
}

describe('timeline: shape', () => {
  it('always renders the nine documented stages in order', () => {
    const ids = buildTimeline(status()).stages.map((s) => s.id);
    expect(ids).toEqual([
      'accepted', 'snapshot', 'apply', 'verify', 'mining',
      'restoring', 'restore-verify', 'complete', 'acknowledge',
    ]);
  });

  it('an idle device claims nothing', () => {
    const tl = buildTimeline(status({ durableState: 'IDLE', sessionPresent: false }));
    expect(tl.stages.every((s) => s.state === 'pending')).toBeTrue();
    expect(tl.interrupted).toBeFalse();
  });
});

describe('timeline: normal progression', () => {
  it('marks the snapshot done and the apply active once the snapshot is durable', () => {
    const s = status({ durableState: 'TARGET_SNAPSHOT_COMMITTED' });
    expect(stateOf(s, 'accepted')).toBe('done');
    expect(stateOf(s, 'snapshot')).toBe('done');
    expect(stateOf(s, 'apply')).toBe('active');
    expect(stateOf(s, 'mining')).toBe('pending');
    expect(stateOf(s, 'complete')).toBe('pending');
  });

  it('verification is active but mining is NOT complete', () => {
    const s = status({ durableState: 'VERIFYING_TARGET', restoreRequired: true });
    expect(stateOf(s, 'apply')).toBe('done');
    expect(stateOf(s, 'verify')).toBe('active');
    expect(stateOf(s, 'mining')).toBe('pending');
  });

  it('target mining lights up only at TARGET_ACTIVE', () => {
    const s = status({ durableState: 'TARGET_ACTIVE', restoreRequired: true });
    expect(stateOf(s, 'verify')).toBe('done');
    expect(stateOf(s, 'mining')).toBe('active');
    expect(stateOf(s, 'restoring')).toBe('pending');
  });

  it('completion marks every earlier stage done and the acknowledgement active', () => {
    const s = status({ durableState: 'COMPLETE', terminalResultPending: true });
    ['accepted', 'snapshot', 'apply', 'verify', 'mining', 'restoring', 'restore-verify', 'complete']
      .forEach((id) => expect(stateOf(s, id as TimelineStageId)).withContext(id).toBe('done'));
    expect(stateOf(s, 'acknowledge')).toBe('active');
  });

  it('an acknowledged completion shows the acknowledgement done', () => {
    const s = status({ durableState: 'COMPLETE', terminalResultPending: false });
    expect(stateOf(s, 'acknowledge')).toBe('done');
  });
});

describe('timeline: early restore and failures', () => {
  it('restoration may begin early without ever having mined', () => {
    const s = status({ durableState: 'APPLYING_RESTORE', restoreRequired: true });
    expect(stateOf(s, 'restoring')).toBe('active');
    expect(stateOf(s, 'complete')).toBe('pending');
    expect(stateOf(s, 'acknowledge')).toBe('pending');
  });

  it('a target failure marks verification failed, not done', () => {
    const s = status({ durableState: 'TARGET_FAILED', restoreRequired: true });
    expect(stateOf(s, 'verify')).toBe('failed');
    expect(stateOf(s, 'mining')).toBe('pending');
  });

  it('a restore failure marks source verification failed', () => {
    const s = status({ durableState: 'RESTORE_FAILED', restoreRequired: true });
    expect(stateOf(s, 'restore-verify')).toBe('failed');
    expect(stateOf(s, 'complete')).toBe('pending');
  });

  it('an interruption marks the apply stage failed', () => {
    const s = status({ durableState: 'INTERRUPTED', restoreRequired: true });
    expect(stateOf(s, 'apply')).toBe('failed');
  });

  it('a pre-mutation cancellation skips the target and restoration stages', () => {
    const s = status({ durableState: 'CANCELLED', restoreRequired: false, terminalResultPending: true });
    expect(stateOf(s, 'snapshot')).toBe('done');
    expect(stateOf(s, 'verify')).toBe('skipped');
    expect(stateOf(s, 'mining')).toBe('skipped');
    expect(stateOf(s, 'restoring')).toBe('skipped');
    expect(stateOf(s, 'restore-verify')).toBe('skipped');
    expect(stateOf(s, 'acknowledge')).toBe('active');
  });
});

describe('timeline: recovery guard and unknown states', () => {
  it('a recovery guard interrupts the timeline visibly and stops every active stage', () => {
    const tl = buildTimeline(status({ durableState: 'RECOVERY_REQUIRED', operatorRecoveryRequired: true }));
    expect(tl.interrupted).toBeTrue();
    expect(tl.interruptionDetail).toContain('history, not a forecast');
    expect(tl.stages.some((s) => s.state === 'active')).toBeFalse();
  });

  it('a guard raised during target mining still suppresses the active marker', () => {
    const tl = buildTimeline(status({ durableState: 'TARGET_ACTIVE', operatorRecoveryRequired: true, restoreRequired: true }));
    expect(tl.interrupted).toBeTrue();
    expect(tl.stages.find((s) => s.id === 'mining')!.state).not.toBe('active');
  });

  it('an unrecognised durable state claims nothing at all', () => {
    const tl = buildTimeline(status({ durableState: 'SOMETHING_NEW' }));
    expect(tl.stages.every((s) => s.state === 'pending')).toBeTrue();
    expect(tl.interrupted).toBeFalse();
  });

  it('is a pure function of the status — repeated calls never advance', () => {
    const s = status({ durableState: 'VERIFYING_TARGET', restoreRequired: true });
    const a = buildTimeline(s);
    const b = buildTimeline(s);
    const c = buildTimeline(s);
    expect(JSON.stringify(a)).toBe(JSON.stringify(b));
    expect(JSON.stringify(b)).toBe(JSON.stringify(c));
  });
});
