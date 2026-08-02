/**
 * Gate B9 — the command-correlation contract.
 *
 * These tests encode the rule that a queued command may only be resolved when
 * the echoed id, the echoed command kind, a strictly newer status sequence AND
 * the command-specific authoritative state transition all hold — and that even
 * then the claim is about the DEVICE'S posture, never about ownership of this
 * page's request.
 */

import { TimedSessionStatus } from 'src/app/generated/models';
import { mockStatus } from 'src/app/services/timed-session.service';
import {
  COMMAND_TOKEN, CommandBaseline, captureBaseline, commandEvidence, resolveCommand,
} from './timed-session-correlation';

function status(over: Partial<TimedSessionStatus> = {}): TimedSessionStatus {
  return { ...mockStatus('free', 10), ...over } as TimedSessionStatus;
}

/** A free device at sequence 10, as it would look just before a create. */
function freeBaseline(over: Partial<TimedSessionStatus> = {}): CommandBaseline {
  return captureBaseline(status(over))!;
}

const ID = 77;

// ---------------------------------------------------------------------------

describe('correlation: baseline capture', () => {
  it('captures the whole bounded posture', () => {
    const b = captureBaseline(status({
      statusSequence: 42, durableState: 'TARGET_ACTIVE', runtimeState: 'runtime_target_active',
      executionState: 'EXEC_STATE_TARGET_ACTIVE', sessionPresent: true, terminalResultPending: false,
      restoreRequired: true, lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED',
      lastClientRequestId: 5,
    }))!;
    expect(b.statusSequence).toBe(42);
    expect(b.durableState).toBe('TARGET_ACTIVE');
    expect(b.runtimeState).toBe('runtime_target_active');
    expect(b.executionState).toBe('EXEC_STATE_TARGET_ACTIVE');
    expect(b.sessionPresent).toBeTrue();
    expect(b.restoreRequired).toBeTrue();
    expect(b.lastCommand).toBe('CREATE_SESSION');
    expect(b.lastCommandResult).toBe('ACCEPTED');
    expect(b.lastClientRequestId).toBe(5);
  });

  it('carries no identity field', () => {
    const b = captureBaseline(status())!;
    Object.keys(b).forEach((k) => {
      expect(k).withContext(k).not.toMatch(/host|account|worker|wallet|pass|lease|token/i);
    });
  });

  it('is null when nothing has been read yet', () => {
    expect(captureBaseline(null)).toBeNull();
    expect(captureBaseline(undefined)).toBeNull();
  });
});

// ---------------------------------------------------------------------------

describe('correlation: the four checks', () => {
  it('a device still holding a command reports processing, not completion', () => {
    const r = resolveCommand({
      kind: 'create', clientRequestId: ID, baseline: freeBaseline(),
      status: status({ statusSequence: 11, commandPending: true }),
    });
    expect(r.outcome).toBe('processing');
    expect(r.attribution).toBe('none');
  });

  it('CHECK 1 — a different echoed id never resolves', () => {
    const r = resolveCommand({
      kind: 'create', clientRequestId: ID, baseline: freeBaseline(),
      status: status({
        statusSequence: 11, commandPending: false, lastClientRequestId: 4242,
        lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED', sessionPresent: true,
        durableState: 'TARGET_ACTIVE',
      }),
    });
    expect(r.outcome).toBe('queued');
    expect(r.blockedBy).toBe('id');
  });

  it('CHECK 1 — an unsent id (0) never resolves', () => {
    const r = resolveCommand({
      kind: 'create', clientRequestId: 0, baseline: freeBaseline(),
      status: status({ statusSequence: 11, lastClientRequestId: 0, lastCommand: 'CREATE_SESSION' }),
    });
    expect(r.blockedBy).toBe('id');
  });

  it('CHECK 2 — a matching id with the WRONG command kind never resolves', () => {
    // The device processed somebody's RESTORE_NOW that happened to carry the
    // same number. It says nothing about the create this page sent.
    const r = resolveCommand({
      kind: 'create', clientRequestId: ID, baseline: freeBaseline(),
      status: status({
        statusSequence: 11, commandPending: false, lastClientRequestId: ID,
        lastCommand: 'RESTORE_NOW', lastCommandResult: 'ACCEPTED',
        sessionPresent: true, durableState: 'TARGET_ACTIVE',
      }),
    });
    expect(r.outcome).toBe('queued');
    expect(r.blockedBy).toBe('kind');
  });

  it('CHECK 3 — a STALE retained result with the same id does not resolve', () => {
    // The canonical reload case: a previous page instance used id 77, the
    // device still reports it, and this page's counter has reached 77 again.
    // The status sequence has not advanced past the baseline, so it cannot be
    // an answer to anything sent after the baseline was taken.
    const r = resolveCommand({
      kind: 'create', clientRequestId: ID, baseline: freeBaseline({ statusSequence: 30 }),
      status: status({
        statusSequence: 30, commandPending: false, lastClientRequestId: ID,
        lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED',
        sessionPresent: true, durableState: 'TARGET_ACTIVE',
      }),
    });
    expect(r.outcome).toBe('queued');
    expect(r.blockedBy).toBe('sequence');
  });

  it('CHECK 3 — an OLDER sequence than the baseline never resolves', () => {
    const r = resolveCommand({
      kind: 'create', clientRequestId: ID, baseline: freeBaseline({ statusSequence: 30 }),
      status: status({
        statusSequence: 29, lastClientRequestId: ID, lastCommand: 'CREATE_SESSION',
        lastCommandResult: 'ACCEPTED', sessionPresent: true, durableState: 'TARGET_ACTIVE',
      }),
    });
    expect(r.blockedBy).toBe('sequence');
  });

  it('CHECK 4 — id, kind and sequence all match but the state never moved', () => {
    // lastCommandResult alone is exactly what a stale or foreign command can
    // set, so it is never sufficient on its own.
    const r = resolveCommand({
      kind: 'create', clientRequestId: ID, baseline: freeBaseline(),
      status: status({
        statusSequence: 11, commandPending: false, lastClientRequestId: ID,
        lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED',
        sessionPresent: false, durableState: 'IDLE',
      }),
    });
    expect(r.outcome).toBe('queued');
    expect(r.blockedBy).toBe('evidence');
  });

  it('no baseline means nothing can ever be attributed', () => {
    const r = resolveCommand({
      kind: 'create', clientRequestId: ID, baseline: null,
      status: status({ statusSequence: 99, lastClientRequestId: ID, lastCommand: 'CREATE_SESSION' }),
    });
    expect(r.outcome).toBe('queued');
  });
});

// ---------------------------------------------------------------------------

describe('correlation: command-specific authoritative evidence', () => {
  it('CREATE resolves only after a durable session and a real advance', () => {
    const baseline = freeBaseline();
    const advanced = status({
      statusSequence: 11, commandPending: false, lastClientRequestId: ID,
      lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED',
      sessionPresent: true, durableState: 'TARGET_SNAPSHOT_COMMITTED',
      runtimeState: 'runtime_persistence_pending',
    });
    expect(commandEvidence('create', baseline, advanced)).toBeTrue();
    const r = resolveCommand({ kind: 'create', clientRequestId: ID, baseline, status: advanced });
    expect(r.outcome).toBe('posture-reached');
    expect(r.attribution).toBe('device-posture');
  });

  it('CREATE does NOT resolve on a session that is present but still IDLE', () => {
    expect(commandEvidence('create', freeBaseline(),
      status({ sessionPresent: true, durableState: 'IDLE' }))).toBeFalse();
  });

  it('RESTORE resolves only once restoration has genuinely progressed', () => {
    const baseline = captureBaseline(status({
      statusSequence: 20, sessionPresent: true, durableState: 'TARGET_ACTIVE', restoreRequired: true,
    }))!;
    const moved = status({
      statusSequence: 21, commandPending: false, lastClientRequestId: ID,
      lastCommand: 'RESTORE_NOW', lastCommandResult: 'ACCEPTED',
      sessionPresent: true, durableState: 'APPLYING_RESTORE', restoreRequired: true,
    });
    expect(commandEvidence('restore', baseline, moved)).toBeTrue();
    expect(resolveCommand({ kind: 'restore', clientRequestId: ID, baseline, status: moved }).outcome)
      .toBe('posture-reached');
  });

  it('RESTORE does NOT resolve merely because the id matched', () => {
    const baseline = captureBaseline(status({
      statusSequence: 20, sessionPresent: true, durableState: 'TARGET_ACTIVE', restoreRequired: true,
    }))!;
    const unmoved = status({
      statusSequence: 21, commandPending: false, lastClientRequestId: ID,
      lastCommand: 'RESTORE_NOW', lastCommandResult: 'ACCEPTED',
      sessionPresent: true, durableState: 'TARGET_ACTIVE', restoreRequired: true,
    });
    expect(commandEvidence('restore', baseline, unmoved)).toBeFalse();
    expect(resolveCommand({ kind: 'restore', clientRequestId: ID, baseline, status: unmoved }).blockedBy)
      .toBe('evidence');
  });

  it('RESTORE does not resolve from a restore posture that never changed', () => {
    const baseline = captureBaseline(status({
      statusSequence: 20, sessionPresent: true, durableState: 'RESTORE_DUE', restoreRequired: true,
    }))!;
    const same = status({
      statusSequence: 21, lastClientRequestId: ID, lastCommand: 'RESTORE_NOW',
      sessionPresent: true, durableState: 'RESTORE_DUE', restoreRequired: true,
    });
    expect(commandEvidence('restore', baseline, same)).toBeFalse();
  });

  it('ACKNOWLEDGE resolves only once the retained result is actually gone', () => {
    const baseline = captureBaseline(status({
      statusSequence: 30, terminalResultPending: true, durableState: 'COMPLETE',
    }))!;
    const cleared = status({
      statusSequence: 31, commandPending: false, lastClientRequestId: ID,
      lastCommand: 'ACKNOWLEDGE_TERMINAL', lastCommandResult: 'ACCEPTED',
      terminalResultPending: false, durableState: 'IDLE', sessionPresent: false,
    });
    expect(commandEvidence('acknowledge', baseline, cleared)).toBeTrue();
    expect(resolveCommand({ kind: 'acknowledge', clientRequestId: ID, baseline, status: cleared }).outcome)
      .toBe('posture-reached');
  });

  it('ACKNOWLEDGE never clears optimistically while the result is still retained', () => {
    const baseline = captureBaseline(status({
      statusSequence: 30, terminalResultPending: true, durableState: 'COMPLETE',
    }))!;
    const still = status({
      statusSequence: 31, commandPending: false, lastClientRequestId: ID,
      lastCommand: 'ACKNOWLEDGE_TERMINAL', lastCommandResult: 'ACCEPTED',
      terminalResultPending: true, durableState: 'COMPLETE',
    });
    expect(commandEvidence('acknowledge', baseline, still)).toBeFalse();
    expect(resolveCommand({ kind: 'acknowledge', clientRequestId: ID, baseline, status: still }).blockedBy)
      .toBe('evidence');
  });

  it('a refusal is reported once id, kind and sequence agree', () => {
    // Claiming "nothing happened" cannot mislead an operator into believing
    // something did, so it is safe on the first three checks alone.
    const r = resolveCommand({
      kind: 'create', clientRequestId: ID, baseline: freeBaseline(),
      status: status({
        statusSequence: 11, commandPending: false, lastClientRequestId: ID,
        lastCommand: 'CREATE_SESSION', lastCommandResult: 'REJECTED',
      }),
    });
    expect(r.outcome).toBe('refused');
    expect(r.attribution).toBe('none');
  });

  it('maps each UI command to its committed backend token', () => {
    expect(COMMAND_TOKEN).toEqual({
      create: 'CREATE_SESSION', restore: 'RESTORE_NOW', acknowledge: 'ACKNOWLEDGE_TERMINAL',
    });
  });
});

// ---------------------------------------------------------------------------

describe('correlation: attribution is never ownership', () => {
  it('the strongest possible outcome is still only device posture', () => {
    const baseline = freeBaseline();
    const r = resolveCommand({
      kind: 'create', clientRequestId: ID, baseline,
      status: status({
        statusSequence: 11, commandPending: false, lastClientRequestId: ID,
        lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED',
        sessionPresent: true, durableState: 'APPLYING_TARGET',
      }),
    });
    expect(r.attribution).toBe('device-posture');
    // There is deliberately no value that means "this page owns this result".
    expect(['device-posture', 'none']).toContain(r.attribution);
  });

  it('a same-id status from another client is indistinguishable, so it is never upgraded', () => {
    // This is the honest position: on an unauthenticated LAN a second client
    // can present the same number, so the best available answer is a statement
    // about the device — which is exactly what `device-posture` encodes.
    const baseline = freeBaseline();
    const foreign = status({
      statusSequence: 11, commandPending: false, lastClientRequestId: ID,
      lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED',
      sessionPresent: true, durableState: 'APPLYING_TARGET',
    });
    const r = resolveCommand({ kind: 'create', clientRequestId: ID, baseline, status: foreign });
    expect(r.outcome).toBe('posture-reached');
    expect(r.attribution).not.toBe('owned' as unknown as 'device-posture');
  });
});
