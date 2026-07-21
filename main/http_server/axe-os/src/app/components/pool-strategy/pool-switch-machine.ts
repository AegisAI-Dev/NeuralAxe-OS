/**
 * NeuralAxe Pool Strategy Center — safe switch state machine (Phase 2M, Stage 5).
 *
 * A pure, explicit finite-state machine covering the full switch lifecycle and
 * both recovery paths (automatic rollback on failure, manual restore after
 * success). Only valid transitions apply; every transition is timestamped and
 * carries a human explanation. There is no silent skipping and no state ever
 * carries a credential — only profile IDs, names and the chain label.
 *
 * The engine (the component) owns real HTTP, telemetry and timeouts; this module
 * owns only the state logic and is fully unit-tested.
 */

import { PoolChain } from './pool-profile';

export type SwitchState =
  | 'idle'
  | 'preflight'
  | 'awaiting-confirmation'
  | 'capturing'
  | 'applying'
  | 'restarting'
  | 'reconnecting'
  | 'verifying'
  | 'mining-resumed'
  | 'rolling-back'
  | 'restoring'
  | 'complete'
  | 'aborted'
  | 'failed'
  | 'interrupted';

export const TERMINAL_STATES: ReadonlyArray<SwitchState> = ['complete', 'aborted', 'failed', 'interrupted'];

/** Non-terminal states in which real work is happening on the device. */
export const ACTIVE_STATES: ReadonlyArray<SwitchState> = [
  'capturing', 'applying', 'restarting', 'reconnecting', 'verifying', 'mining-resumed', 'rolling-back', 'restoring',
];

export type SwitchEventType =
  | 'START_PREFLIGHT'
  | 'PREFLIGHT_OK'
  | 'PREFLIGHT_FAIL'
  | 'CONFIRM'
  | 'CANCEL'
  | 'CAPTURED'
  | 'CAPTURE_FAIL'
  | 'APPLY_APPLIED'     // applied with no restart needed
  | 'APPLY_RESTART'     // applied, a restart is required
  | 'APPLY_FAIL'
  | 'RESTART_SENT'
  | 'RESTART_FAIL'
  | 'RECONNECTED'
  | 'RECONNECT_TIMEOUT'
  | 'VERIFY_OK'
  | 'VERIFY_FAIL'
  | 'COMPLETE'
  | 'ROLLBACK_OK'
  | 'ROLLBACK_PARTIAL'
  | 'ROLLBACK_FAIL'
  | 'RETRY_ROLLBACK'
  | 'RESTORE_START'
  | 'RESTORE_OK'
  | 'RESTORE_PARTIAL'
  | 'RESTORE_FAIL'
  | 'RETRY_RESTORE'
  | 'ABORT'
  | 'INTERRUPT'
  | 'RESET';

export interface SwitchEvent {
  type: SwitchEventType;
  at: number;
  reason?: string;
  /** Target profile identity (CONFIRM / RESTORE_START only). Never a credential. */
  profileId?: string;
  profileName?: string;
  chain?: PoolChain;
}

export interface SwitchTimelineEntry {
  state: SwitchState;
  at: number;
  note?: string;
}

/** Verified / partially verified / failed — used for both rollback and restore. */
export type RecoveryResult = 'verified' | 'partial' | 'failed';

export interface SwitchSnapshot {
  state: SwitchState;
  targetProfileId: string | null;
  targetProfileName: string | null;
  targetChain: PoolChain | null;
  /** Failure / abort explanation, or the last stop reason. */
  reason: string | null;
  /** Outcome of an automatic rollback (failure path). */
  rollbackResult: RecoveryResult | null;
  /** Outcome of a manual restore (Stage 9). */
  restoreResult: RecoveryResult | null;
  /** Whether the target profile was verified as applied before any failure. */
  switchVerified: boolean;
  startedAt: number | null;
  timeline: SwitchTimelineEntry[];
}

export function initialSwitchSnapshot(): SwitchSnapshot {
  return {
    state: 'idle',
    targetProfileId: null,
    targetProfileName: null,
    targetChain: null,
    reason: null,
    rollbackResult: null,
    restoreResult: null,
    switchVerified: false,
    startedAt: null,
    timeline: [{ state: 'idle', at: 0, note: 'No switch in progress' }],
  };
}

const STATE_LABELS: { [k in SwitchState]: string } = {
  idle: 'Idle',
  preflight: 'Preflight',
  'awaiting-confirmation': 'Awaiting confirmation',
  capturing: 'Capturing original',
  applying: 'Applying profile',
  restarting: 'Restarting',
  reconnecting: 'Reconnecting',
  verifying: 'Verifying pool',
  'mining-resumed': 'Mining resumed',
  'rolling-back': 'Rolling back',
  restoring: 'Restoring original',
  complete: 'Complete',
  aborted: 'Aborted',
  failed: 'Failed',
  interrupted: 'Interrupted',
};

const STATE_EXPLANATIONS: { [k in SwitchState]: string } = {
  idle: 'No switch is in progress. Select a profile and review the changes to begin.',
  preflight: 'Checking every safety precondition before anything changes.',
  'awaiting-confirmation': 'Preflight passed. Review the masked changes and confirm to switch.',
  capturing: 'Capturing the current pool configuration so it can be restored.',
  applying: 'Writing the target pool settings to the device.',
  restarting: 'Restarting the device so the new pool settings take effect.',
  reconnecting: 'Waiting for the device to reconnect and resume mining.',
  verifying: 'Verifying the device reconnected, mining resumed and the active pool matches the target.',
  'mining-resumed': 'Mining resumed on the target pool — finishing up.',
  'rolling-back': 'The switch could not be verified — restoring your captured original configuration.',
  restoring: 'Restoring your previous pool configuration.',
  complete: 'The switch completed and was verified.',
  aborted: 'The switch was stopped before the device configuration changed.',
  failed: 'The switch could not complete. A rollback to the original configuration was attempted.',
  interrupted: 'The browser session was interrupted mid-switch — verify the device pool configuration.',
};

export function switchStateLabel(state: SwitchState): string {
  return STATE_LABELS[state] ?? state;
}

export function switchStateExplanation(state: SwitchState): string {
  return STATE_EXPLANATIONS[state] ?? '';
}

/**
 * Pure transition function. Returns the next state for a valid (state, event)
 * pair, or null when the transition is not allowed.
 */
export function nextSwitchState(state: SwitchState, event: SwitchEventType): SwitchState | null {
  if (event === 'RESET') {
    return TERMINAL_STATES.includes(state) || state === 'idle' ? 'idle' : null;
  }
  if (event === 'INTERRUPT') {
    return TERMINAL_STATES.includes(state) ? null : 'interrupted';
  }

  switch (state) {
    case 'idle':
      if (event === 'START_PREFLIGHT') return 'preflight';
      // Manual restore of a persisted snapshot is available from idle too
      // (e.g. after a reload following a successful switch).
      if (event === 'RESTORE_START') return 'restoring';
      return null;
    case 'preflight':
      if (event === 'PREFLIGHT_OK') return 'awaiting-confirmation';
      if (event === 'PREFLIGHT_FAIL') return 'idle';
      return null;
    case 'awaiting-confirmation':
      if (event === 'CONFIRM') return 'capturing';
      if (event === 'CANCEL') return 'idle';
      return null;
    case 'capturing':
      if (event === 'CAPTURED') return 'applying';
      if (event === 'CAPTURE_FAIL') return 'failed';
      if (event === 'ABORT') return 'aborted';
      return null;
    case 'applying':
      if (event === 'APPLY_APPLIED') return 'verifying';
      if (event === 'APPLY_RESTART') return 'restarting';
      if (event === 'APPLY_FAIL') return 'rolling-back';
      if (event === 'ABORT') return 'rolling-back';
      return null;
    case 'restarting':
      if (event === 'RESTART_SENT') return 'reconnecting';
      if (event === 'RESTART_FAIL') return 'rolling-back';
      if (event === 'ABORT') return 'rolling-back';
      return null;
    case 'reconnecting':
      if (event === 'RECONNECTED') return 'verifying';
      if (event === 'RECONNECT_TIMEOUT') return 'rolling-back';
      if (event === 'ABORT') return 'rolling-back';
      return null;
    case 'verifying':
      if (event === 'VERIFY_OK') return 'mining-resumed';
      if (event === 'VERIFY_FAIL') return 'rolling-back';
      if (event === 'ABORT') return 'rolling-back';
      return null;
    case 'mining-resumed':
      if (event === 'COMPLETE') return 'complete';
      return null;
    case 'rolling-back':
      if (event === 'ROLLBACK_OK' || event === 'ROLLBACK_PARTIAL' || event === 'ROLLBACK_FAIL') return 'failed';
      return null;
    case 'restoring':
      if (event === 'RESTORE_OK') return 'complete';
      if (event === 'RESTORE_PARTIAL' || event === 'RESTORE_FAIL') return 'failed';
      return null;
    case 'complete':
      if (event === 'RESTORE_START') return 'restoring';
      return null;
    case 'failed':
      if (event === 'RETRY_ROLLBACK') return 'rolling-back';
      if (event === 'RETRY_RESTORE') return 'restoring';
      return null;
    default:
      return null; // aborted / interrupted accept only RESET (handled above)
  }
}

/**
 * Apply an event to a snapshot. Invalid transitions are a no-op so the engine can
 * never silently corrupt state. Valid transitions append a timestamped timeline
 * entry and update the derived fields.
 */
export function reduceSwitch(snapshot: SwitchSnapshot, event: SwitchEvent): SwitchSnapshot {
  const target = nextSwitchState(snapshot.state, event.type);
  if (target === null) {
    return snapshot;
  }

  let { targetProfileId, targetProfileName, targetChain, reason, rollbackResult, restoreResult, switchVerified, startedAt } = snapshot;
  let note: string | undefined;

  switch (event.type) {
    case 'START_PREFLIGHT':
      note = 'Preflight started';
      break;
    case 'PREFLIGHT_OK':
      note = 'Preflight passed';
      break;
    case 'PREFLIGHT_FAIL':
      reason = event.reason ?? 'Preflight failed';
      note = reason;
      break;
    case 'CONFIRM':
      targetProfileId = event.profileId ?? targetProfileId;
      targetProfileName = event.profileName ?? targetProfileName;
      targetChain = event.chain ?? targetChain;
      startedAt = event.at;
      reason = null;
      rollbackResult = null;
      restoreResult = null;
      switchVerified = false;
      note = `Owner confirmed — switching to "${targetProfileName}"`;
      break;
    case 'CANCEL':
      note = 'Cancelled before any change';
      break;
    case 'CAPTURED':
      note = 'Original configuration captured';
      break;
    case 'CAPTURE_FAIL':
      reason = event.reason ?? 'Could not capture the original configuration — no change was made';
      note = reason;
      break;
    case 'APPLY_APPLIED':
      note = 'Profile applied (no restart required)';
      break;
    case 'APPLY_RESTART':
      note = 'Profile applied — restart required';
      break;
    case 'APPLY_FAIL':
      reason = event.reason ?? 'Applying the profile failed';
      note = reason;
      break;
    case 'RESTART_SENT':
      note = 'Restart command sent';
      break;
    case 'RESTART_FAIL':
      reason = event.reason ?? 'Restart command failed';
      note = reason;
      break;
    case 'RECONNECTED':
      note = 'Device reconnected';
      break;
    case 'RECONNECT_TIMEOUT':
      reason = event.reason ?? 'Device did not reconnect in time';
      note = reason;
      break;
    case 'VERIFY_OK':
      switchVerified = true;
      note = 'Reconnect, mining and target pool verified';
      break;
    case 'VERIFY_FAIL':
      reason = event.reason ?? 'Could not verify the switch';
      note = reason;
      break;
    case 'COMPLETE':
      note = 'Switch complete and verified';
      break;
    case 'ROLLBACK_OK':
      rollbackResult = 'verified';
      reason = reason ?? 'Switch failed — rolled back and verified the original configuration';
      note = 'Rollback verified';
      break;
    case 'ROLLBACK_PARTIAL':
      rollbackResult = 'partial';
      note = 'Rollback applied but only partially verified';
      break;
    case 'ROLLBACK_FAIL':
      rollbackResult = 'failed';
      note = event.reason ?? 'Rollback could not be verified — check the device';
      break;
    case 'RETRY_ROLLBACK':
      note = 'Retrying rollback';
      break;
    case 'RESTORE_START':
      targetProfileName = event.profileName ?? targetProfileName;
      restoreResult = null;
      reason = null;
      note = 'Restoring the previous pool configuration';
      break;
    case 'RESTORE_OK':
      restoreResult = 'verified';
      note = 'Previous configuration restored and verified';
      break;
    case 'RESTORE_PARTIAL':
      restoreResult = 'partial';
      reason = event.reason ?? 'Restore applied but only partially verified';
      note = reason;
      break;
    case 'RESTORE_FAIL':
      restoreResult = 'failed';
      reason = event.reason ?? 'Restore could not be verified';
      note = reason;
      break;
    case 'RETRY_RESTORE':
      note = 'Retrying restore';
      break;
    case 'ABORT':
      reason = event.reason ?? 'Owner aborted';
      note = `Aborted: ${reason}`;
      break;
    case 'INTERRUPT':
      reason = event.reason ?? 'Browser session interrupted';
      note = reason;
      break;
    case 'RESET':
      return initialSwitchSnapshot();
  }

  return {
    state: target,
    targetProfileId,
    targetProfileName,
    targetChain,
    reason,
    rollbackResult,
    restoreResult,
    switchVerified,
    startedAt,
    timeline: [...snapshot.timeline, { state: target, at: event.at, note }],
  };
}

export function reduceSwitchAll(snapshot: SwitchSnapshot, events: SwitchEvent[]): SwitchSnapshot {
  return events.reduce((snap, event) => reduceSwitch(snap, event), snapshot);
}

export function isSwitchTerminal(state: SwitchState): boolean {
  return TERMINAL_STATES.includes(state);
}

export function isSwitchActive(state: SwitchState): boolean {
  return ACTIVE_STATES.includes(state);
}
