/**
 * Gate B9 — PURE status/conflict mapper tests.
 *
 * Every mapping is total, every unknown token falls back to ONE generic safe
 * label, and no raw backend prose is ever rendered. No network, no storage,
 * no clock.
 */

import { OperationConflict, TimedSessionStatus } from 'src/app/generated/models';
import {
  HEARTBEAT_NOTE, asicGateView, availabilityView, buildView, commandNameView, commandResultView,
  commandView, conflictView, deadlineView, durableStateView, executionStateView, formatRemaining,
  heartbeatView, ownerView, phaseView, runtimeStateView, trustedTimeView, validationLabel,
} from './timed-session-mapper';
import { UNKNOWN_LABEL } from './timed-session.models';

function status(overrides: Partial<TimedSessionStatus> = {}): TimedSessionStatus {
  return {
    apiEnabled: true, runtimeInitialized: true, executionEnabled: true,
    sessionPresent: false, terminalResultPending: false,
    durableState: 'IDLE', runtimeState: 'runtime_free', executionState: 'EXEC_STATE_IDLE',
    leaseOwner: 'OWNER_NONE', leasePhase: 'PHASE_FREE',
    protocolStartPermitted: true, asicGate: 'GATE_DEFAULT_OPEN',
    targetMiningGrantActive: false, restoreRequired: false, operatorRecoveryRequired: false,
    deadlineStatus: 'UNKNOWN', heartbeatStatus: 'NOT_APPLICABLE', statusSequence: 7,
    ...overrides,
  };
}

// ---------------------------------------------------------------------------

describe('mapper: availability', () => {
  it('AVAILABLE is the only operable state', () => {
    expect(availabilityView('AVAILABLE').operable).toBeTrue();
    (['CHECKING', 'API_DISABLED_OR_NOT_PRESENT', 'ORIGIN_DENIED', 'DEVICE_OFFLINE',
      'TEMPORARILY_UNAVAILABLE', 'UNKNOWN_ERROR'] as const).forEach((a) => {
      expect(availabilityView(a).operable).withContext(a).toBeFalse();
    });
  });

  it('a missing API stops polling and offers a manual re-check', () => {
    const v = availabilityView('API_DISABLED_OR_NOT_PRESENT');
    expect(v.pollingStopped).toBeTrue();
    expect(v.retryable).toBeTrue();
    expect(v.label).toContain('Not available in this firmware build');
    // It must not read as a fault.
    expect(v.detail).toContain('Nothing is wrong with the miner');
  });

  it('origin denial stops polling; offline and unavailable keep bounded retries', () => {
    expect(availabilityView('ORIGIN_DENIED').pollingStopped).toBeTrue();
    expect(availabilityView('DEVICE_OFFLINE').pollingStopped).toBeFalse();
    expect(availabilityView('TEMPORARILY_UNAVAILABLE').pollingStopped).toBeFalse();
  });

  it('an unrecognised availability falls back safely', () => {
    const v = availabilityView('NOT_A_STATE' as never);
    expect(v.availability).toBe('UNKNOWN_ERROR');
    expect(v.label).toBe(UNKNOWN_LABEL);
    expect(v.operable).toBeFalse();
  });
});

// ---------------------------------------------------------------------------

describe('mapper: durable / runtime / execution tokens', () => {
  const DURABLE = ['IDLE', 'PREPARING', 'TARGET_SNAPSHOT_COMMITTED', 'APPLYING_TARGET',
    'RESTARTING_FOR_TARGET', 'VERIFYING_TARGET', 'TARGET_ACTIVE', 'RESTORE_DUE',
    'APPLYING_RESTORE', 'RESTARTING_FOR_RESTORE', 'VERIFYING_RESTORE', 'COMPLETE',
    'TARGET_FAILED', 'RESTORE_FAILED', 'INTERRUPTED', 'RECOVERY_REQUIRED', 'CANCELLED'];

  it('maps every committed durable token to a distinct human label', () => {
    const labels = new Set<string>();
    DURABLE.forEach((t) => {
      const v = durableStateView(t);
      expect(v.label).withContext(t).not.toBe(UNKNOWN_LABEL);
      expect(v.detail.length).toBeGreaterThan(10);
      labels.add(v.label);
    });
    expect(labels.size).toBe(DURABLE.length);
  });

  it('never describes target VERIFICATION as mining', () => {
    const v = durableStateView('VERIFYING_TARGET');
    expect(v.label.toLowerCase()).not.toContain('mining');
    expect(v.detail).toContain('NOT authorised');
  });

  it('unknown durable/runtime/execution/owner/phase/gate tokens all fall back once', () => {
    [durableStateView('WAT'), runtimeStateView('WAT'), executionStateView('WAT'),
      ownerView('WAT'), phaseView('WAT'), asicGateView('WAT'),
      durableStateView(undefined), runtimeStateView(undefined)].forEach((v) => {
      expect(v.label).toBe(UNKNOWN_LABEL);
      expect(v.severity).toBe('warn');
    });
  });

  it('maps every committed runtime and execution token', () => {
    ['runtime_uninitialized', 'runtime_bootstrapping', 'runtime_free', 'runtime_terminal_pending',
      'runtime_persistence_pending', 'runtime_waiting_for_trusted_time', 'runtime_verify_target_pending',
      'runtime_restore_source_pending', 'runtime_operator_recovery', 'runtime_recovery_guard',
      'runtime_stopped', 'runtime_error'].forEach((t) => {
      expect(runtimeStateView(t).label).withContext(t).not.toBe(UNKNOWN_LABEL);
    });
    ['EXEC_STATE_DISABLED', 'EXEC_STATE_IDLE', 'EXEC_STATE_TARGET_APPLYING', 'EXEC_STATE_TARGET_MINING',
      'EXEC_STATE_SOURCE_APPLYING', 'EXEC_STATE_RESTORE_FAILED_HELD', 'EXEC_STATE_RECOVERY_GUARD',
      'EXEC_STATE_ERROR', 'EXEC_STATE_DONE'].forEach((t) => {
      expect(executionStateView(t).label).withContext(t).not.toBe(UNKNOWN_LABEL);
    });
  });

  it('maps every committed owner class and phase', () => {
    ['OWNER_NONE', 'OWNER_TIMED_SESSION', 'OWNER_BOOT_RECOVERY', 'OWNER_SOURCE_RESTORE',
      'OWNER_MANUAL_POOL_PATCH', 'OWNER_OTA_UPDATE', 'OWNER_SESSION_ACKNOWLEDGE',
      'OWNER_OPERATOR_RECOVERY', 'OWNER_RECOVERY_GUARD'].forEach((t) => {
      expect(ownerView(t).label).withContext(t).not.toBe(UNKNOWN_LABEL);
    });
    ['PHASE_UNBOOTSTRAPPED', 'PHASE_FREE', 'PHASE_RESERVED_PENDING_PERSISTENCE', 'PHASE_ACTIVE',
      'PHASE_WAITING_FOR_TRUSTED_TIME', 'PHASE_VERIFYING_TARGET', 'PHASE_RESTORING_SOURCE',
      'PHASE_TERMINAL_ACK_PENDING', 'PHASE_RELEASING', 'PHASE_RECOVERY_GUARD'].forEach((t) => {
      expect(phaseView(t).label).withContext(t).not.toBe(UNKNOWN_LABEL);
    });
  });
});

// ---------------------------------------------------------------------------

describe('mapper: deadline and remaining time', () => {
  it('formats backend seconds only', () => {
    expect(formatRemaining(3600)).toBe('1 h 00 m');
    expect(formatRemaining(3542)).toBe('59 m 02 s');
    expect(formatRemaining(45)).toBe('45 s');
    expect(formatRemaining(-1)).toBe('');
    expect(formatRemaining(Number.NaN)).toBe('');
  });

  it('shows remaining time only when the backend marked it valid', () => {
    const known = deadlineView(status({ deadlineStatus: 'ACTIVE', remainingSecondsValid: true, remainingSeconds: 120 }));
    expect(known.remainingKnown).toBeTrue();
    expect(known.remainingText).toBe('2 m 00 s');

    const unknown = deadlineView(status({ deadlineStatus: 'ACTIVE', remainingSecondsValid: false, remainingSeconds: 999 }));
    expect(unknown.remainingKnown).toBeFalse();
    expect(unknown.remainingText).toBe('');
    expect(unknown.detail).toContain('cannot currently prove');
  });

  it('maps every deadline status and falls back on an unknown one', () => {
    expect(deadlineView(status({ deadlineStatus: 'EXPIRED' })).label).toBe('Expired');
    expect(deadlineView(status({ deadlineStatus: 'RESTORE_PENDING' })).label).toBe('Restoration pending');
    expect(deadlineView(status({ deadlineStatus: 'UNKNOWN' })).label).toBe('Not applicable');
    expect(deadlineView(status({ deadlineStatus: 'NOPE' as never })).label).toBe(UNKNOWN_LABEL);
  });
});

// ---------------------------------------------------------------------------

describe('mapper: trusted time and heartbeat', () => {
  it('warns loudly and refuses to estimate when trusted time is required but absent', () => {
    const v = trustedTimeView(status({ trustedTimeRequired: true, trustedTimeAvailable: false }));
    expect(v.warn).toBeTrue();
    expect(v.severity).toBe('warn');
    expect(v.detail).toContain('not exact');
    expect(v.detail).toContain('browser clock');
  });

  it('does not warn when trusted time is available or not required', () => {
    expect(trustedTimeView(status({ trustedTimeRequired: true, trustedTimeAvailable: true })).warn).toBeFalse();
    expect(trustedTimeView(status({ trustedTimeRequired: false, trustedTimeAvailable: false })).warn).toBeFalse();
  });

  it('maps every heartbeat state and carries the commit count', () => {
    (['NOT_APPLICABLE', 'WAITING', 'DUE', 'COMMITTED', 'TIME_UNTRUSTED', 'PERSIST_FAILED',
      'RECOVERY_GUARD'] as const).forEach((h) => {
      const v = heartbeatView(status({ heartbeatStatus: h, heartbeatCommits: 4 }));
      expect(v.label).withContext(h).not.toBe(UNKNOWN_LABEL);
      expect(v.commits).toBe(4);
    });
    expect(heartbeatView(status({ heartbeatStatus: 'NOPE' as never })).label).toBe(UNKNOWN_LABEL);
  });

  it('the heartbeat note states it never extends the deadline', () => {
    expect(HEARTBEAT_NOTE).toContain('never extends the original deadline');
    expect(HEARTBEAT_NOTE).toContain('not a');
    expect(HEARTBEAT_NOTE).toContain('thermal health');
  });

  it('a failed heartbeat never claims liveness', () => {
    const v = heartbeatView(status({ heartbeatStatus: 'PERSIST_FAILED' }));
    expect(v.severity).toBe('danger');
    expect(v.detail).toContain('does not claim liveness');
  });
});

// ---------------------------------------------------------------------------

describe('mapper: conflicts', () => {
  const CODES: OperationConflict['code'][] = [
    'OPERATION_ALLOWED', 'OPERATION_BUSY_TIMED_SESSION', 'OPERATION_BUSY_RESTORE',
    'OPERATION_BUSY_MANUAL_POOL_CHANGE', 'OPERATION_BUSY_OTA', 'OPERATION_RECOVERY_LOCKED',
    'OPERATION_BOOTSTRAP_REQUIRED', 'OPERATION_TERMINAL_ACK_REQUIRED',
    'OPERATION_PERSISTENCE_UNCERTAIN', 'OPERATION_NOT_OWNER', 'OPERATION_STALE_LEASE',
    'OPERATION_NO_ACTIVE_SESSION', 'OPERATION_UNSAFE_RELEASE', 'OPERATION_INVALID_REQUEST',
  ];

  it('maps every committed Gate B5 code to a distinct bounded title', () => {
    const titles = new Set<string>();
    CODES.forEach((code) => {
      const v = conflictView({ code, activeOwner: 'OWNER_NONE', restoreRequired: false, retryable: false, terminalAckRequired: false });
      expect(v.title.length).withContext(code).toBeGreaterThan(3);
      expect(v.detail.length).withContext(code).toBeGreaterThan(10);
      titles.add(v.title);
    });
    expect(titles.size).toBe(CODES.length);
  });

  it('carries the retryable flag, the owner CLASS and the two safe booleans', () => {
    const v = conflictView({
      code: 'OPERATION_BUSY_OTA', activeOwner: 'OWNER_OTA_UPDATE',
      retryable: true, restoreRequired: true, terminalAckRequired: true,
    });
    expect(v.retryable).toBeTrue();
    expect(v.ownerLabel).toBe('Firmware update');
    expect(v.restoreRequired).toBeTrue();
    expect(v.terminalAckRequired).toBeTrue();
  });

  it('an unknown code and a missing body both use one generic fallback', () => {
    const unknown = conflictView({ code: 'SOMETHING_NEW' as never });
    const missing = conflictView(null);
    expect(unknown.title).toBe(missing.title);
    expect(unknown.title).toBe('The device refused this operation');
    expect(unknown.retryable).toBeFalse();
    expect(unknown.ownerLabel).toBe('');
  });

  it('never renders a raw backend string', () => {
    const v = conflictView({ code: 'OPERATION_NOT_OWNER' as never, activeOwner: 'RAW_BACKEND_PROSE' as never });
    expect(v.title).not.toContain('RAW_BACKEND_PROSE');
    expect(v.detail).not.toContain('RAW_BACKEND_PROSE');
    expect(v.ownerLabel).toBe(''); // unknown owner class is dropped, not echoed
  });
});

// ---------------------------------------------------------------------------

describe('mapper: commands', () => {
  it('accepted is queued, never started', () => {
    const v = commandView('create', 'accepted', { requestSequence: 3 });
    expect(v.title).toBe('Request accepted for processing');
    expect(v.detail).toContain('NOT started');
    expect(v.severity).toBe('info');
    expect(v.title.toLowerCase()).not.toContain('session started');
    expect(v.requestSequence).toBe(3);
  });

  it('the strongest positive phase describes the DEVICE, never ownership', () => {
    const v = commandView('create', 'posture-reached');
    expect(v.severity).toBe('ok');
    expect(v.title).toBe('The device now reports the requested posture');
    // No wording may suggest this page's request is proven to be the one the
    // device carried out — request numbers are not authentication.
    const text = `${v.title} ${v.detail}`.toLowerCase();
    ['your request completed', 'your request was', 'you started', 'confirmed your',
      'we started', 'successfully started'].forEach((claim) => {
      expect(text).withContext(claim).not.toContain(claim);
    });
    expect(text).toContain('not a receipt');
  });

  it('a device refusal is worded differently from a lost request', () => {
    // Two different situations needing two different operator actions: one
    // means the device said no, the other means nobody knows what the device
    // saw. They must never share copy.
    const refused = commandView('create', 'device-refused');
    const lost = commandView('create', 'failed');
    expect(refused.title).toBe('The device did not accept the request');
    expect(refused.detail).toContain('reported that it was not accepted');
    expect(refused.detail).toContain('Nothing was changed');
    expect(lost.title).toBe('The request did not reach the device');
    expect(refused.title).not.toBe(lost.title);
    expect(refused.detail).not.toBe(lost.detail);
  });

  it('exhaustion is stated plainly and promises nothing was sent', () => {
    const v = commandView('restore', 'request-ids-exhausted');
    expect(v.severity).toBe('warn');
    expect(v.detail).toContain('nothing was sent');
    expect(v.detail).toContain('Reload the page');
    expect(v.detail).toContain('Nothing on the device was changed');
  });

  it('every command phase produces a bounded view', () => {
    (['idle', 'submitting', 'accepted', 'processing', 'posture-reached', 'validation-failed',
      'conflict', 'temporarily-unavailable', 'origin-denied', 'failed', 'device-refused',
      'request-ids-exhausted', 'unknown'] as const)
      .forEach((phase) => {
        const v = commandView('create', phase);
        expect(v.phase).withContext(phase).toBe(phase);
        if (phase !== 'idle') {
          expect(v.title.length).withContext(phase).toBeGreaterThan(3);
        }
      });
  });

  it('maps validation codes and falls back safely', () => {
    expect(validationLabel('DURATION_OUT_OF_RANGE')).toContain('15 minutes and 24 hours');
    expect(validationLabel('PASSWORD_FIELD_REJECTED')).toContain('password');
    expect(validationLabel('TLS_CUSTOM_REJECTED')).toContain('Custom certificates');
    expect(validationLabel('BRAND_NEW_CODE')).toBe('The device rejected the request. No change was made.');
    expect(validationLabel(undefined)).toBe('The device rejected the request. No change was made.');
  });

  it('maps every command name and result token', () => {
    ['NONE', 'CREATE_SESSION', 'RESTORE_NOW', 'ACKNOWLEDGE_TERMINAL'].forEach((t) => {
      expect(commandNameView(t).label).withContext(t).not.toBe(UNKNOWN_LABEL);
    });
    ['NONE', 'PENDING', 'ACCEPTED', 'REJECTED_STATE', 'REJECTED_CONFLICT', 'REJECTED_HARDWARE',
      'REJECTED_SOURCE_UNSUPPORTED', 'REJECTED_TARGET_UNSUPPORTED', 'REJECTED_VALIDATION',
      'FAILED_PERSIST', 'FAILED_READBACK', 'FAILED_OWNERSHIP', 'RECOVERY_GUARD',
      'ADOPTION_FAILED'].forEach((t) => {
      expect(commandResultView(t).label).withContext(t).not.toBe(UNKNOWN_LABEL);
    });
    expect(commandNameView('WAT').label).toBe(UNKNOWN_LABEL);
    expect(commandResultView('WAT').label).toBe(UNKNOWN_LABEL);
  });

  it('ADOPTION_FAILED is never presented as a started session', () => {
    const v = commandResultView('ADOPTION_FAILED');
    expect(v.label).toContain('Not started');
    expect(v.detail).toContain('no restoration is owed');
  });
});

// ---------------------------------------------------------------------------

describe('mapper: whole status view', () => {
  it('a FREE device offers create and nothing else', () => {
    const v = buildView(status());
    expect(v.createOffered).toBeTrue();
    expect(v.restoreOffered).toBeFalse();
    expect(v.acknowledgeOffered).toBeFalse();
    expect(v.grant.label).toBe('Not authorised');
  });

  it('target mining is shown active ONLY when the backend reports a grant', () => {
    const verifying = buildView(status({
      sessionPresent: true, durableState: 'VERIFYING_TARGET', targetMiningGrantActive: false,
      asicGate: 'GATE_INHIBITED',
    }));
    expect(verifying.grant.label).toBe('Not authorised');
    expect(verifying.grant.detail).toContain('Verification alone is never mining');

    const mining = buildView(status({
      sessionPresent: true, durableState: 'TARGET_ACTIVE', targetMiningGrantActive: true,
      asicGate: 'GATE_OPEN_TARGET',
    }));
    expect(mining.grant.label).toBe('Active');
    expect(mining.grant.severity).toBe('ok');
  });

  it('offers Restore Now only for states the backend may accept', () => {
    ['APPLYING_TARGET', 'VERIFYING_TARGET', 'TARGET_ACTIVE', 'RESTORE_DUE', 'INTERRUPTED', 'TARGET_FAILED']
      .forEach((s) => expect(buildView(status({ sessionPresent: true, durableState: s })).restoreOffered)
        .withContext(s).toBeTrue());
    ['IDLE', 'COMPLETE', 'CANCELLED', 'RESTORE_FAILED', 'RECOVERY_REQUIRED']
      .forEach((s) => expect(buildView(status({ sessionPresent: true, durableState: s })).restoreOffered)
        .withContext(s).toBeFalse());
  });

  it('offers acknowledgement only for a safe retained terminal', () => {
    const ok = buildView(status({ sessionPresent: true, durableState: 'COMPLETE', terminalResultPending: true }));
    expect(ok.acknowledgeOffered).toBeTrue();

    const obligated = buildView(status({ sessionPresent: true, durableState: 'RESTORE_FAILED', terminalResultPending: true, restoreRequired: true }));
    expect(obligated.acknowledgeOffered).toBeFalse();

    const guarded = buildView(status({ sessionPresent: true, durableState: 'COMPLETE', terminalResultPending: true, operatorRecoveryRequired: true }));
    expect(guarded.acknowledgeOffered).toBeFalse();
  });

  it('blocks create while a terminal result or recovery is outstanding', () => {
    expect(buildView(status({ terminalResultPending: true })).createOffered).toBeFalse();
    expect(buildView(status({ operatorRecoveryRequired: true })).createOffered).toBeFalse();
    expect(buildView(status({ sessionPresent: true, durableState: 'TARGET_ACTIVE' })).createOffered).toBeFalse();
  });

  it('carries protocol hold and restoration obligation honestly', () => {
    const held = buildView(status({ protocolStartPermitted: false, restoreRequired: true }));
    expect(held.protocol.label).toBe('Held');
    expect(held.obligation.label).toBe('Owed');
    const free = buildView(status());
    expect(free.protocol.label).toBe('Permitted');
    expect(free.obligation.label).toBe('None');
  });

  it('an entirely unknown status still produces a safe view', () => {
    const v = buildView(status({
      durableState: 'WAT', runtimeState: 'WAT', executionState: 'WAT',
      leaseOwner: 'WAT', leasePhase: 'WAT', asicGate: 'WAT',
    }));
    expect(v.durable.label).toBe(UNKNOWN_LABEL);
    expect(v.grant.label).toBe('Not authorised');
    expect(v.restoreOffered).toBeFalse();
  });
});

// ---------------------------------------------------------------------------

describe('mapper: build and runtime capability', () => {
  it('all three flags true is the only capable answer', () => {
    expect(buildView(status()).capable).toBeTrue();
  });

  ([
    ['apiEnabled', 'the control API is off'],
    ['executionEnabled', 'the execution layer is off'],
    ['runtimeInitialized', 'the runtime has not come up'],
  ] as const).forEach(([flag, why]) => {
    it(`is NOT capable when ${why}`, () => {
      const v = buildView(status({ [flag]: false }));
      expect(v.capable).withContext(flag).toBeFalse();
      expect(v.createOffered).withContext('create withheld').toBeFalse();
    });

    it(`treats a MISSING ${flag} as not capable, never as probably fine`, () => {
      const s = status();
      delete (s as unknown as Record<string, unknown>)[flag];
      expect(buildView(s).capable).toBeFalse();
    });
  });

  it('capability never affects what the device is reported to be doing', () => {
    // An incapable device that somehow reports an active session is still
    // described honestly; capability gates only what the OPERATOR is offered.
    const v = buildView(status({
      executionEnabled: false, sessionPresent: true,
      durableState: 'TARGET_ACTIVE', restoreRequired: true, targetMiningGrantActive: true,
    }));
    expect(v.capable).toBeFalse();
    expect(v.createOffered).toBeFalse();
    expect(v.sessionPresent).toBeTrue();
    expect(v.restoreOffered).withContext('restoring is still offered').toBeTrue();
  });
});

describe('mapper: every conflict code the firmware can emit has wording', () => {
  // Mirrors the string table the firmware serialises in pool_session_api.c.
  // `OPERATION_CODE_UNKNOWN` comes from the firmware's defensive `default:`
  // branch in pool_operation_http_code_str(). Gate B9 added it to the
  // published enum, so it is now spellable as the GENERATED type — that is
  // itself the contract assertion.
  const EMITTED: OperationConflict['code'][] = [
    'OPERATION_ALLOWED', 'OPERATION_BOOTSTRAP_REQUIRED', 'OPERATION_BUSY_MANUAL_POOL_CHANGE',
    'OPERATION_BUSY_OTA', 'OPERATION_BUSY_RESTORE', 'OPERATION_BUSY_TIMED_SESSION',
    'OPERATION_CODE_UNKNOWN', 'OPERATION_INVALID_REQUEST', 'OPERATION_NOT_OWNER',
    'OPERATION_NO_ACTIVE_SESSION', 'OPERATION_PERSISTENCE_UNCERTAIN', 'OPERATION_RECOVERY_LOCKED',
    'OPERATION_STALE_LEASE', 'OPERATION_TERMINAL_ACK_REQUIRED', 'OPERATION_UNSAFE_RELEASE',
  ];

  it('covers all fifteen without falling back', () => {
    const unseen = 'SOMETHING_THE_UI_HAS_NEVER_SEEN' as OperationConflict['code'];
    const fallback = conflictView({ code: unseen }).title;
    EMITTED.forEach((code) => {
      const v = conflictView({ code });
      expect(v.title).withContext(code).not.toBe(fallback);
      expect(v.detail.length).withContext(code).toBeGreaterThan(20);
    });
  });

  it('still fails safe for a code it has never seen', () => {
    const v = conflictView({ code: 'OPERATION_FROM_THE_FUTURE' as OperationConflict['code'] });
    expect(v.title).toBe('The device refused this operation');
    expect(v.detail).toContain('remains authoritative');
    expect(v.retryable).withContext('never guesses that a retry would work').toBeFalse();
  });
});

// ---------------------------------------------------------------------------

describe('conflict contract: OPERATION_CODE_UNKNOWN is now in the schema', () => {
  it('the GENERATED model accepts it — no cast required', () => {
    // If the openapi enum ever loses the value again this line stops
    // compiling, which is exactly the guard that was missing before.
    const code: OperationConflict['code'] = 'OPERATION_CODE_UNKNOWN';
    const body: OperationConflict = {
      code, retryable: false, activeOwner: 'OWNER_NONE',
      restoreRequired: false, terminalAckRequired: false,
    };
    expect(body.code).toBe('OPERATION_CODE_UNKNOWN');
  });

  it('maps to bounded wording that names the situation honestly', () => {
    const v = conflictView({ code: 'OPERATION_CODE_UNKNOWN', retryable: false });
    expect(v.title).toBe('The device refused this operation without classifying it');
    expect(v.detail).toContain('Nothing was changed');
    expect(v.severity).toBe('warn');
  });

  it('exposes no identity and no raw backend text', () => {
    const v = conflictView({
      code: 'OPERATION_CODE_UNKNOWN', retryable: false, activeOwner: 'OWNER_NONE',
      restoreRequired: false, terminalAckRequired: false,
    });
    const rendered = `${v.title} ${v.detail} ${v.ownerLabel}`;
    expect(rendered).not.toContain('OPERATION_CODE_UNKNOWN');
    expect(rendered).not.toMatch(/host|account|worker|wallet|pass|lease|token|session id/i);
  });

  it('a genuinely unrecognised future string still uses the final safe fallback', () => {
    const v = conflictView({ code: 'OPERATION_FROM_THE_FUTURE' as OperationConflict['code'] });
    expect(v.title).toBe('The device refused this operation');
    expect(v.detail).toContain('remains authoritative');
    expect(v.retryable).toBeFalse();
  });

  it('a malformed or absent code also falls back safely', () => {
    [undefined, null, '' as OperationConflict['code']].forEach((code) => {
      const v = conflictView({ code: code as OperationConflict['code'] });
      expect(v.title).toBe('The device refused this operation');
    });
    expect(conflictView(null).title).toBe('The device refused this operation');
  });
});
