/**
 * NeuralAxe Timed Pool Session — PURE status/conflict mapper (Gate B9).
 *
 * Turns ONE sanitized Gate B8 payload into bounded operator labels. It is
 * total: every unrecognised machine token falls back to a single generic safe
 * label, and no raw backend string is ever rendered.
 *
 * NOTHING here reads a clock, touches storage, logs, or performs IO. Elapsed
 * browser time is never used to infer a device state — `remainingSeconds` is
 * shown only when the backend itself marked it valid.
 */

import { OperationConflict, TimedSessionStatus } from 'src/app/generated/models';
import {
  AvailabilityView,
  CommandView,
  ConflictView,
  DeadlineView,
  HeartbeatView,
  LabelledState,
  Severity,
  TimedSessionAvailability,
  TimedSessionView,
  TrustedTimeView,
  UNKNOWN_DETAIL,
  UNKNOWN_LABEL,
} from './timed-session.models';

function state(label: string, detail: string, severity: Severity = 'neutral'): LabelledState {
  return { label, detail, severity };
}

const UNKNOWN_STATE: LabelledState = state(UNKNOWN_LABEL, UNKNOWN_DETAIL, 'warn');

// ---------------------------------------------------------------------------
// Availability
// ---------------------------------------------------------------------------

export function availabilityView(a: TimedSessionAvailability): AvailabilityView {
  switch (a) {
    case 'CHECKING':
      return {
        availability: a, label: 'Checking device support', severity: 'info',
        detail: 'Asking the device whether the timed-session control API is present in this firmware build.',
        operable: false, retryable: false, pollingStopped: false,
      };
    case 'AVAILABLE':
      return {
        availability: a, label: 'Available', severity: 'ok',
        detail: 'The device exposes the timed-session control API. The device remains authoritative for every decision.',
        operable: true, retryable: false, pollingStopped: false,
      };
    case 'API_DISABLED_OR_NOT_PRESENT':
      return {
        availability: a, label: 'Not available in this firmware build', severity: 'neutral',
        detail:
          'This firmware was built without the timed-session control API, so the device has no such route. ' +
          'Nothing is wrong with the miner. Polling has been stopped; use “Check again” after flashing a build that enables it.',
        operable: false, retryable: true, pollingStopped: true,
      };
    case 'ORIGIN_DENIED':
      return {
        availability: a, label: 'Request refused by the device', severity: 'warn',
        detail:
          'The device refused this browser’s origin. Open the dashboard directly from the miner’s address on the same private network.',
        operable: false, retryable: true, pollingStopped: true,
      };
    case 'DEVICE_OFFLINE':
      return {
        availability: a, label: 'Device unreachable', severity: 'warn',
        detail: 'The dashboard cannot reach the miner. Retries are slowing down automatically; nothing is being resent.',
        operable: false, retryable: true, pollingStopped: false,
      };
    case 'TEMPORARILY_UNAVAILABLE':
      return {
        availability: a, label: 'Temporarily unavailable', severity: 'warn',
        detail: 'The device accepted the request but its control-plane runtime is not ready. This usually clears shortly after boot.',
        operable: false, retryable: true, pollingStopped: false,
      };
    case 'UNKNOWN_ERROR':
    default:
      return {
        availability: 'UNKNOWN_ERROR', label: UNKNOWN_LABEL, severity: 'warn',
        detail: UNKNOWN_DETAIL, operable: false, retryable: true, pollingStopped: false,
      };
  }
}

// ---------------------------------------------------------------------------
// Durable session state (committed B1 tokens)
// ---------------------------------------------------------------------------

const DURABLE: Readonly<Record<string, LabelledState>> = {
  IDLE: state('No session', 'No timed session exists on the device.', 'neutral'),
  PREPARING: state('Preparing', 'The device is preparing the session record. Nothing has been applied yet.', 'info'),
  TARGET_SNAPSHOT_COMMITTED: state(
    'Source snapshot persisted',
    'The device captured and durably stored its own current pool configuration. The target pool has not been applied.',
    'info'),
  APPLYING_TARGET: state(
    'Applying target',
    'The device is writing the target pool configuration. Source restoration is now mandatory.', 'info'),
  RESTARTING_FOR_TARGET: state('Reconnecting for target', 'The device is restarting its pool connection for the target.', 'info'),
  VERIFYING_TARGET: state(
    'Verifying target',
    'The device is proving the live target connection and hardware evidence. Target mining is NOT authorised yet.', 'info'),
  TARGET_ACTIVE: state('Target mining', 'The device verified the target pool and is mining it under the session deadline.', 'ok'),
  RESTORE_DUE: state('Restoration due', 'The session ended or was interrupted. The device owes a return to the original pool.', 'warn'),
  APPLYING_RESTORE: state('Restoring source', 'The device is writing the original pool configuration back.', 'warn'),
  RESTARTING_FOR_RESTORE: state('Reconnecting for source', 'The device is restarting its pool connection for the original pool.', 'warn'),
  VERIFYING_RESTORE: state('Verifying source', 'The device is proving the original pool reconnected and is mining again.', 'warn'),
  COMPLETE: state(
    'Completed',
    'The original pool was restored and verified mining again. The result is retained until you acknowledge it.', 'ok'),
  TARGET_FAILED: state('Target failed', 'The target pool could not be verified. The device owes a restoration.', 'danger'),
  RESTORE_FAILED: state(
    'Restoration failed',
    'The device could not prove the original pool was restored. It will not clear this by itself — operator action is required.', 'danger'),
  INTERRUPTED: state('Interrupted', 'The session was cut off mid-flight. The device recovers conservatively toward the original pool.', 'warn'),
  RECOVERY_REQUIRED: state(
    'Recovery required',
    'The device cannot trust its stored session state. It has locked mutation until an operator resolves it.', 'danger'),
  CANCELLED: state(
    'Cancelled before any change',
    'The session was cancelled before the target pool was touched. Nothing was applied and nothing is owed.', 'ok'),
};

export function durableStateView(token: string | undefined): LabelledState {
  return (token && DURABLE[token]) || UNKNOWN_STATE;
}

// ---------------------------------------------------------------------------
// Runtime / execution / ownership tokens
// ---------------------------------------------------------------------------

const RUNTIME: Readonly<Record<string, LabelledState>> = {
  runtime_uninitialized: state('Not started', 'The session runtime has not initialised.', 'neutral'),
  runtime_bootstrapping: state('Starting', 'The session runtime is starting up.', 'info'),
  runtime_free: state('Idle', 'No session facts exist; normal pool operation continues.', 'ok'),
  runtime_terminal_pending: state('Result retained', 'A finished session result is held until acknowledged.', 'info'),
  runtime_persistence_pending: state('Saving', 'A record must be durably stored before anything advances.', 'info'),
  runtime_waiting_for_trusted_time: state('Waiting for trusted time', 'Pool startup is held until the device establishes trusted time.', 'warn'),
  runtime_verify_target_pending: state('Target verification pending', 'Eligibility only — this is not a mining authorisation.', 'info'),
  runtime_restore_source_pending: state('Restoration pending', 'The device owes a return to the original pool.', 'warn'),
  runtime_operator_recovery: state('Operator recovery', 'An operator recovery operation holds the device.', 'danger'),
  runtime_recovery_guard: state('Recovery guard', 'Fail-closed: normal mutation is locked until an operator resolves it.', 'danger'),
  runtime_stopped: state('Stopped', 'The session runtime task is stopped.', 'neutral'),
  runtime_error: state('Runtime error', 'The session runtime failed closed.', 'danger'),
  runtime_invalid: UNKNOWN_STATE,
};

export function runtimeStateView(token: string | undefined): LabelledState {
  return (token && RUNTIME[token]) || UNKNOWN_STATE;
}

const EXECUTION: Readonly<Record<string, LabelledState>> = {
  EXEC_STATE_DISABLED: state('Disabled', 'The controlled-execution layer is not enabled in this firmware build.', 'neutral'),
  EXEC_STATE_IDLE: state('Idle', 'Bound and waiting; no session action is in progress.', 'neutral'),
  EXEC_STATE_ENTRY_PENDING: state('Starting', 'An action posture was seen and is being validated.', 'info'),
  EXEC_STATE_TARGET_READBACK: state('Checking target configuration', 'Comparing the stored configuration against the target.', 'info'),
  EXEC_STATE_TARGET_APPLYING: state('Applying target', 'Writing the target pool configuration.', 'info'),
  EXEC_STATE_TARGET_CONFIG_VERIFIED: state('Target configuration verified', 'The written configuration read back exactly. Reconnect pending.', 'info'),
  EXEC_STATE_TARGET_CONNECTING: state('Connecting to target', 'Verification-only connection. No mining is authorised.', 'info'),
  EXEC_STATE_TARGET_PROTOCOL_VERIFIED: state('Target protocol verified', 'Evidence complete; the mining grant is pending.', 'info'),
  EXEC_STATE_TARGET_MINING: state('Target mining', 'The grant is active and health is being watched.', 'ok'),
  EXEC_STATE_RESTORE_PENDING: state('Preparing restoration', 'Mining is inhibited and the device is quiescing before restoring.', 'warn'),
  EXEC_STATE_SOURCE_APPLYING: state('Restoring source', 'Writing the original pool configuration back.', 'warn'),
  EXEC_STATE_SOURCE_CONFIG_VERIFIED: state('Source configuration verified', 'The original configuration read back exactly. Reconnect pending.', 'warn'),
  EXEC_STATE_SOURCE_CONNECTING: state('Reconnecting to source', 'Verification-only connection to the original pool.', 'warn'),
  EXEC_STATE_SOURCE_PROTOCOL_VERIFIED: state('Source protocol verified', 'The original pool connection and identity are proven.', 'warn'),
  EXEC_STATE_SOURCE_MINING_VERIFYING: state('Verifying source mining', 'Proving the original pool is mining again before completing.', 'warn'),
  EXEC_STATE_COMPLETE_HANDOFF: state('Handing back', 'Completion is durable; normal pool operation is resuming.', 'ok'),
  EXEC_STATE_DONE: state('Finished', 'Execution finished; the record is retained.', 'ok'),
  EXEC_STATE_HANDOFF_FAILED: state('Hand-back failed', 'Completion is durable but normal operation did not resume cleanly.', 'warn'),
  EXEC_STATE_RESTORE_FAILED_HELD: state('Restoration failed — held', 'The obligation is retained and pool startup stays held.', 'danger'),
  EXEC_STATE_RECOVERY_GUARD: state('Recovery guard', 'Fail-closed; an operator is required.', 'danger'),
  EXEC_STATE_ERROR: state('Execution error', 'The execution layer failed closed.', 'danger'),
};

export function executionStateView(token: string | undefined): LabelledState {
  return (token && EXECUTION[token]) || UNKNOWN_STATE;
}

const OWNER: Readonly<Record<string, LabelledState>> = {
  OWNER_NONE: state('None', 'No operation owns the device right now.', 'neutral'),
  OWNER_TIMED_SESSION: state('Timed session', 'A timed pool session owns the pool configuration.', 'info'),
  OWNER_BOOT_RECOVERY: state('Boot recovery', 'Boot recovery owns the device after a restart.', 'warn'),
  OWNER_SOURCE_RESTORE: state('Source restoration', 'A restoration back to the original pool owns the device.', 'warn'),
  OWNER_MANUAL_POOL_PATCH: state('Manual pool change', 'A manual pool configuration change owns the device.', 'info'),
  OWNER_OTA_UPDATE: state('Firmware update', 'An OTA update owns the device.', 'info'),
  OWNER_SESSION_ACKNOWLEDGE: state('Acknowledgement', 'A short-lived acknowledgement operation owns the device.', 'info'),
  OWNER_OPERATOR_RECOVERY: state('Operator recovery', 'An operator recovery operation owns the device.', 'danger'),
  OWNER_RECOVERY_GUARD: state('Recovery guard', 'The fail-closed guard owns the device.', 'danger'),
};

export function ownerView(token: string | undefined): LabelledState {
  return (token && OWNER[token]) || UNKNOWN_STATE;
}

const PHASE: Readonly<Record<string, LabelledState>> = {
  PHASE_UNBOOTSTRAPPED: state('Not established', 'Ownership is unknown, so every change is refused.', 'warn'),
  PHASE_FREE: state('Free', 'No operation holds the device.', 'ok'),
  PHASE_RESERVED_PENDING_PERSISTENCE: state('Reserved', 'Reserved while a record is being stored. Nothing has been applied.', 'info'),
  PHASE_ACTIVE: state('Active', 'The operation is established and may act.', 'info'),
  PHASE_WAITING_FOR_TRUSTED_TIME: state('Waiting for trusted time', 'Pool startup is held until time can be trusted.', 'warn'),
  PHASE_VERIFYING_TARGET: state('Verifying target', 'Live verification is in progress. No mining is authorised by this phase.', 'info'),
  PHASE_RESTORING_SOURCE: state('Restoring source', 'The same operation is returning the device to its original pool.', 'warn'),
  PHASE_TERMINAL_ACK_PENDING: state('Awaiting acknowledgement', 'A durable final result is waiting to be acknowledged.', 'info'),
  PHASE_RELEASING: state('Releasing', 'Ownership is being released.', 'info'),
  PHASE_RECOVERY_GUARD: state('Recovery guard', 'Fail-closed; only operator recovery is admitted.', 'danger'),
};

export function phaseView(token: string | undefined): LabelledState {
  return (token && PHASE[token]) || UNKNOWN_STATE;
}

const GATE: Readonly<Record<string, LabelledState>> = {
  GATE_DEFAULT_OPEN: state('Normal', 'No session epoch exists; ordinary mining is unaffected.', 'neutral'),
  GATE_INHIBITED: state('Inhibited', 'Work delivery to the ASIC is held by the session.', 'warn'),
  GATE_OPEN_TARGET: state('Open for target', 'Work delivery is open under a verified target grant.', 'ok'),
  GATE_OPEN_SOURCE_RESTORED: state('Open for source', 'Work delivery is open again on the restored original pool.', 'ok'),
};

export function asicGateView(token: string | undefined): LabelledState {
  return (token && GATE[token]) || UNKNOWN_STATE;
}

// ---------------------------------------------------------------------------
// Trusted time, deadline, heartbeat
// ---------------------------------------------------------------------------

/** Format backend-provided seconds. Never derived from browser time. */
export function formatRemaining(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) {
    return '';
  }
  const total = Math.floor(seconds);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  if (h > 0) {
    return `${h} h ${String(m).padStart(2, '0')} m`;
  }
  if (m > 0) {
    return `${m} m ${String(s).padStart(2, '0')} s`;
  }
  return `${s} s`;
}

export function trustedTimeView(status: TimedSessionStatus): TrustedTimeView {
  const required = status.trustedTimeRequired === true;
  const available = status.trustedTimeAvailable === true;
  if (!required && available) {
    return {
      required, available, warn: false, severity: 'ok',
      label: 'Trusted time available',
      detail: 'The device has an accepted trusted-time reference.',
    };
  }
  if (!required) {
    return {
      required, available, warn: false, severity: 'neutral',
      label: 'Trusted time not required',
      detail: 'Nothing on the device currently depends on a trusted-time reference.',
    };
  }
  if (available) {
    return {
      required, available, warn: false, severity: 'ok',
      label: 'Trusted time available',
      detail: 'The device has the trusted-time reference its current recovery plan requires.',
    };
  }
  return {
    required, available, warn: true, severity: 'warn',
    label: 'Trusted time unavailable',
    detail:
      'The device needs a trusted-time reference and does not have one. Any remaining duration is therefore not exact, ' +
      'and this dashboard will not estimate one from your browser clock.',
  };
}

export function deadlineView(status: TimedSessionStatus): DeadlineView {
  const known = status.remainingSecondsValid === true;
  const seconds = known ? (status.remainingSeconds ?? 0) : 0;
  const text = known ? formatRemaining(seconds) : '';
  switch (status.deadlineStatus) {
    case 'ACTIVE':
      return {
        label: 'Running', severity: 'ok', remainingKnown: known, remainingSeconds: seconds, remainingText: text,
        detail: known
          ? 'The device reports the remaining session time shown here.'
          : 'The session is running, but the device cannot currently prove how much time remains.',
      };
    case 'EXPIRED':
      return {
        label: 'Expired', severity: 'warn', remainingKnown: false, remainingSeconds: 0, remainingText: '',
        detail: 'The session deadline has passed. The device restores the original pool by itself.',
      };
    case 'RESTORE_PENDING':
      return {
        label: 'Restoration pending', severity: 'warn', remainingKnown: false, remainingSeconds: 0, remainingText: '',
        detail: 'The deadline is no longer the operative fact — a return to the original pool is owed.',
      };
    case 'UNKNOWN':
      return {
        label: 'Not applicable', severity: 'neutral', remainingKnown: false, remainingSeconds: 0, remainingText: '',
        detail: 'No session deadline applies right now.',
      };
    default:
      return {
        label: UNKNOWN_LABEL, severity: 'warn', remainingKnown: false, remainingSeconds: 0, remainingText: '',
        detail: UNKNOWN_DETAIL,
      };
  }
}

const HEARTBEAT: Readonly<Record<string, LabelledState>> = {
  NOT_APPLICABLE: state('Not applicable', 'No session is mining a target pool, so no liveness heartbeat applies.', 'neutral'),
  WAITING: state('Waiting', 'The next heartbeat window has not opened yet.', 'info'),
  DUE: state('Due', 'A heartbeat is due and will be stored on the next device tick.', 'info'),
  COMMITTED: state('Committed', 'The device durably stored its latest accepted trusted-time floor.', 'ok'),
  TIME_UNTRUSTED: state('Trusted time unavailable', 'No trusted-time reference is available, so nothing is written.', 'warn'),
  PERSIST_FAILED: state(
    'Storage failed',
    'The device could not durably store a heartbeat. It does not claim liveness, and it stops the session safely if this persists.',
    'danger'),
  RECOVERY_GUARD: state('Recovery guard', 'The outcome was unknowable, so the device failed closed.', 'danger'),
};

export function heartbeatView(status: TimedSessionStatus): HeartbeatView {
  const base = HEARTBEAT[status.heartbeatStatus] ?? UNKNOWN_STATE;
  return { ...base, commits: status.heartbeatCommits ?? 0 };
}

/** The one sentence that must always accompany the heartbeat. */
export const HEARTBEAT_NOTE =
  'A heartbeat only confirms a durable trusted-time floor. It never extends the original deadline, and it is not a ' +
  'substitute for ASIC, protocol or thermal health.';

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

const COMMAND_NAME: Readonly<Record<string, string>> = {
  NONE: 'None',
  CREATE_SESSION: 'Start session',
  RESTORE_NOW: 'Restore now',
  ACKNOWLEDGE_TERMINAL: 'Acknowledge result',
};

export function commandNameView(token: string | undefined): LabelledState {
  const name = (token && COMMAND_NAME[token]) || '';
  if (!name) {
    return UNKNOWN_STATE;
  }
  return state(name, token === 'NONE' ? 'No command is queued.' : 'This command is queued for the device.',
    token === 'NONE' ? 'neutral' : 'info');
}

const COMMAND_RESULT: Readonly<Record<string, LabelledState>> = {
  NONE: state('None', 'No command has been processed since the device booted.', 'neutral'),
  PENDING: state('Queued', 'The device has the command but has not processed it yet.', 'info'),
  ACCEPTED: state('Completed', 'The device processed the command successfully.', 'ok'),
  REJECTED_STATE: state('Refused — device state', 'The device refused the command because of its current state.', 'warn'),
  REJECTED_CONFLICT: state('Refused — busy', 'Another operation owns the device.', 'warn'),
  REJECTED_HARDWARE: state('Refused — hardware', 'This board or ASIC is not supported for timed sessions.', 'danger'),
  REJECTED_SOURCE_UNSUPPORTED: state(
    'Refused — current pool not restorable',
    'The device could not guarantee an exact return to its current pool configuration, so it refused before changing anything.',
    'warn'),
  REJECTED_TARGET_UNSUPPORTED: state('Refused — target rejected', 'The device rejected the target pool details.', 'warn'),
  REJECTED_VALIDATION: state('Refused — invalid request', 'The device rejected the request as invalid.', 'warn'),
  FAILED_PERSIST: state('Failed — storage', 'The device could not durably store the change. Nothing was applied.', 'danger'),
  FAILED_READBACK: state('Failed — verification', 'The device could not verify what it stored.', 'danger'),
  FAILED_OWNERSHIP: state('Failed — ownership', 'The device could not confirm ownership of the operation.', 'danger'),
  RECOVERY_GUARD: state('Failed — recovery guard', 'The outcome was unknowable, so the device locked itself fail-closed.', 'danger'),
  ADOPTION_FAILED: state(
    'Not started — cancelled safely',
    'The device created the session but its execution layer refused to run it, so it cancelled before touching any pool. ' +
    'Nothing was applied and no restoration is owed.',
    'warn'),
};

export function commandResultView(token: string | undefined): LabelledState {
  return (token && COMMAND_RESULT[token]) || UNKNOWN_STATE;
}

const VALIDATION: Readonly<Record<string, string>> = {
  BODY_TOO_LARGE: 'The request was too large.',
  BODY_MALFORMED: 'The request was malformed.',
  BODY_NOT_OBJECT: 'The request had the wrong shape.',
  UNKNOWN_FIELD: 'The request contained a field the device does not accept.',
  DUPLICATE_FIELD: 'The request repeated a field.',
  MISSING_FIELD: 'A required value was missing.',
  NULL_FIELD: 'A required value was empty.',
  TYPE_MISMATCH: 'A value had the wrong type.',
  TOO_MANY_FIELDS: 'The request contained too many fields.',
  PASSWORD_FIELD_REJECTED: 'The device refuses any password-like field. Timed sessions always keep the existing password.',
  SOURCE_FIELD_REJECTED: 'The device captures its own current pool configuration and refuses one supplied by a client.',
  SESSION_ID_REJECTED: 'The device refuses a client-supplied session identifier.',
  INTERNAL_FIELD_REJECTED: 'The request contained an internal field a client may not set.',
  DURATION_OUT_OF_RANGE: 'The duration must be between 15 minutes and 24 hours.',
  HOST_EMPTY: 'A target pool host is required.',
  HOST_TOO_LONG: 'The target pool host is too long.',
  PORT_INVALID: 'The target pool port must be between 1 and 65535.',
  ACCOUNT_EMPTY: 'A target account or worker is required.',
  ACCOUNT_TOO_LONG: 'The target account is too long.',
  PROTOCOL_UNSUPPORTED: 'That Stratum protocol is not supported.',
  TLS_MODE_UNSUPPORTED: 'That TLS mode is not supported.',
  TLS_CUSTOM_REJECTED: 'Custom certificates are not supported for timed sessions, because the device could not restore them exactly.',
  CHAIN_UNSUPPORTED: 'That chain label is not supported.',
  REQUEST_ID_INVALID: 'The diagnostics identifier was not valid.',
};

export function validationLabel(code: string | undefined): string {
  return (code && VALIDATION[code]) || 'The device rejected the request. No change was made.';
}

// ---------------------------------------------------------------------------
// Conflicts (committed Gate B5 machine codes)
// ---------------------------------------------------------------------------

interface ConflictCopy {
  readonly title: string;
  readonly detail: string;
  readonly severity: Severity;
}

const CONFLICT: Readonly<Record<string, ConflictCopy>> = {
  OPERATION_ALLOWED: {
    title: 'No conflict', severity: 'ok',
    detail: 'The device reported no ownership conflict.',
  },
  // The device's own "I could not classify this" answer. It is a real,
  // documented code, so it gets real wording rather than being pushed into
  // the fallback for codes this dashboard has never heard of.
  OPERATION_CODE_UNKNOWN: {
    title: 'The device refused this operation without classifying it', severity: 'warn',
    detail:
      'The device blocked the operation but could not say which owner or condition caused it. Nothing was '
      + 'changed. Read the status below before trying anything else.',
  },
  OPERATION_BUSY_TIMED_SESSION: {
    title: 'A timed session already owns the device', severity: 'warn',
    detail: 'Only one timed session can exist. Wait for it to finish, or restore it now.',
  },
  OPERATION_BUSY_RESTORE: {
    title: 'A restoration is in progress', severity: 'warn',
    detail: 'The device is returning to its original pool. Wait until that completes.',
  },
  OPERATION_BUSY_MANUAL_POOL_CHANGE: {
    title: 'A manual pool change is in progress', severity: 'warn',
    detail: 'Finish or cancel the manual pool change first.',
  },
  OPERATION_BUSY_OTA: {
    title: 'A firmware update is in progress', severity: 'warn',
    detail: 'Wait for the update to finish before starting a session.',
  },
  OPERATION_RECOVERY_LOCKED: {
    title: 'Locked for recovery', severity: 'danger',
    detail: 'The device could not trust its stored state and locked changes. An operator must resolve this; retrying will not help.',
  },
  OPERATION_BOOTSTRAP_REQUIRED: {
    title: 'Device still starting', severity: 'info',
    detail: 'The device has not finished establishing ownership yet. This normally clears within moments of boot.',
  },
  OPERATION_TERMINAL_ACK_REQUIRED: {
    title: 'A previous result is waiting', severity: 'info',
    detail: 'Acknowledge the retained result before starting a new session.',
  },
  OPERATION_PERSISTENCE_UNCERTAIN: {
    title: 'Storage outcome unknown', severity: 'danger',
    detail: 'The device could not determine whether a write landed and failed closed. An operator must resolve this.',
  },
  OPERATION_NOT_OWNER: {
    title: 'Not the owner of this operation', severity: 'warn',
    detail: 'The device is owned by a different operation, so it refused this one.',
  },
  OPERATION_STALE_LEASE: {
    title: 'Out of date', severity: 'warn',
    detail: 'The device moved on since this request was prepared. Refresh the status and try again.',
  },
  OPERATION_NO_ACTIVE_SESSION: {
    title: 'No active session', severity: 'info',
    detail: 'There is no running session for this action to control.',
  },
  OPERATION_UNSAFE_RELEASE: {
    title: 'Not safe to clear', severity: 'warn',
    detail: 'The device still owes work for this session, so it refused to clear it.',
  },
  OPERATION_INVALID_REQUEST: {
    title: 'Request refused', severity: 'warn',
    detail: 'The device refused the request as invalid.',
  },
};

const CONFLICT_FALLBACK: ConflictCopy = {
  title: 'The device refused this operation', severity: 'warn',
  detail: 'The device reported an ownership conflict this dashboard does not recognise. The device remains authoritative.',
};

export function conflictView(body: Partial<OperationConflict> | null | undefined): ConflictView {
  const copy = (body?.code && CONFLICT[body.code]) || CONFLICT_FALLBACK;
  const owner = body?.activeOwner ? ownerView(body.activeOwner) : null;
  return {
    title: copy.title,
    detail: copy.detail,
    severity: copy.severity,
    retryable: body?.retryable === true,
    ownerLabel: owner && owner !== UNKNOWN_STATE ? owner.label : '',
    restoreRequired: body?.restoreRequired === true,
    terminalAckRequired: body?.terminalAckRequired === true,
  };
}

// ---------------------------------------------------------------------------
// The whole status view
// ---------------------------------------------------------------------------

/** Durable states in which the backend may accept a Restore Now. */
const RESTORE_OFFERABLE = new Set<string>([
  'PREPARING', 'TARGET_SNAPSHOT_COMMITTED', 'APPLYING_TARGET', 'RESTARTING_FOR_TARGET',
  'VERIFYING_TARGET', 'TARGET_ACTIVE', 'RESTORE_DUE', 'APPLYING_RESTORE',
  'RESTARTING_FOR_RESTORE', 'VERIFYING_RESTORE', 'INTERRUPTED', 'TARGET_FAILED',
]);

export function buildView(status: TimedSessionStatus): TimedSessionView {
  const durable = durableStateView(status.durableState);
  const grantActive = status.targetMiningGrantActive === true;
  const recovery = status.operatorRecoveryRequired === true;
  const terminalAck = status.terminalResultPending === true;
  // Answering the status route is not the same as being able to run a
  // session. All three must be true, and each is read strictly — an absent
  // flag counts as "not capable", never as "probably fine".
  const capable = status.apiEnabled === true
    && status.executionEnabled === true
    && status.runtimeInitialized === true;

  return {
    sessionPresent: status.sessionPresent === true,
    durable,
    runtime: runtimeStateView(status.runtimeState),
    execution: executionStateView(status.executionState),
    owner: ownerView(status.leaseOwner),
    phase: phaseView(status.leasePhase),
    protocol: status.protocolStartPermitted
      ? state('Permitted', 'Ordinary pool startup may proceed.', 'ok')
      : state('Held', 'The device is holding pool startup until its session posture resolves.', 'warn'),
    asicGate: asicGateView(status.asicGate),
    grant: grantActive
      ? state('Active', 'The device verified the target and authorised mining it.', 'ok')
      : state('Not authorised', 'No target-mining authorisation exists. Verification alone is never mining.', 'neutral'),
    obligation: status.restoreRequired
      ? state('Owed', 'The device must return to its original pool before this session can be considered finished.', 'warn')
      : state('None', 'No return to the original pool is owed.', 'ok'),
    trustedTime: trustedTimeView(status),
    deadline: deadlineView(status),
    heartbeat: heartbeatView(status),
    pendingCommand: commandNameView(status.pendingCommand),
    lastCommandResult: commandResultView(status.lastCommandResult),
    terminalAckRequired: terminalAck,
    operatorRecoveryRequired: recovery,
    restoreOffered: status.sessionPresent === true && RESTORE_OFFERABLE.has(status.durableState),
    acknowledgeOffered: terminalAck && !recovery && !status.restoreRequired,
    createOffered: capable && !status.sessionPresent && !terminalAck && !recovery,
    statusSequence: status.statusSequence ?? 0,
    executionEnabled: status.executionEnabled === true,
    runtimeInitialized: status.runtimeInitialized === true,
    apiEnabled: status.apiEnabled === true,
    capable,
  };
}

/** Bounded, identity-free banner for one submitted command. */
export function commandView(
  kind: CommandView['kind'],
  phase: CommandView['phase'],
  opts: {
    conflict?: ConflictView | null;
    validationCode?: string;
    requestSequence?: number;
    clientRequestId?: number;
  } = {},
): CommandView {
  const conflict = opts.conflict ?? null;
  const base = {
    kind, phase, conflict,
    validationLabel: phase === 'validation-failed' ? validationLabel(opts.validationCode) : '',
    requestSequence: opts.requestSequence ?? 0,
    clientRequestId: opts.clientRequestId ?? 0,
  };
  switch (phase) {
    case 'idle':
      return { ...base, title: '', detail: '', severity: 'neutral' };
    case 'submitting':
      return { ...base, title: 'Sending request…', detail: 'The request is being sent to the device.', severity: 'info' };
    case 'accepted':
      return {
        ...base, title: 'Request accepted for processing', severity: 'info',
        detail:
          'The device queued the request. Accepted is NOT started — the device status below is the only proof of what ' +
          'actually happens next.',
      };
    case 'processing':
      return {
        ...base, title: 'Device is processing the request', severity: 'info',
        detail: 'The device has the command and is working through it. Watch the status below.',
      };
    case 'posture-reached':
      return {
        ...base, title: 'The device now reports the requested posture', severity: 'ok',
        detail:
          'The device’s own status now shows the state this request asked for, and it changed after the request '
          + 'was sent. Request numbers are best-effort correlation on a network with no authentication, so this is '
          + 'the device reporting its state — not a receipt proving this page’s request was the one carried out.',
      };
    case 'request-ids-exhausted':
      return {
        ...base, title: 'This page can no longer number requests safely', severity: 'warn',
        detail:
          'Every request number available to this page has been used. Reusing one could make a result the device '
          + 'kept from an earlier command look like the answer to a new one, so nothing was sent. Reload the page '
          + 'to start a fresh sequence. Nothing on the device was changed.',
      };
    case 'validation-failed':
      return {
        ...base, title: 'The device rejected the request', severity: 'warn',
        detail: 'Nothing was changed on the device.',
      };
    case 'conflict':
      return {
        ...base, title: conflict?.title ?? CONFLICT_FALLBACK.title, severity: conflict?.severity ?? 'warn',
        detail: conflict?.detail ?? CONFLICT_FALLBACK.detail,
      };
    case 'temporarily-unavailable':
      return {
        ...base, title: 'The device could not accept the request right now', severity: 'warn',
        detail: 'Its control-plane runtime or command queue was unavailable. Nothing was changed and nothing was resent.',
      };
    case 'origin-denied':
      return {
        ...base, title: 'The device refused this browser', severity: 'warn',
        detail: 'Open the dashboard from the miner’s address on the same private network.',
      };
    case 'failed':
      return {
        ...base, title: 'The request did not reach the device', severity: 'warn',
        detail: 'Nothing was sent twice. Check the status below, then retry manually if you still want to.',
      };
    case 'device-refused':
      return {
        ...base, title: 'The device did not accept the request', severity: 'warn',
        detail:
          'The device processed a request bearing this page’s request number and reported that it was not '
          + 'accepted. Nothing was changed by it. The status below is authoritative for what the device is '
          + 'actually doing.',
      };
    case 'unknown':
    default:
      return { ...base, title: UNKNOWN_LABEL, detail: UNKNOWN_DETAIL, severity: 'warn' };
  }
}
