/**
 * NeuralAxe Stability Lab — session state machine (Phase 2K).
 *
 * A pure, explicit finite-state machine. Only valid transitions are applied;
 * every transition is timestamped and carries a human explanation. There is no
 * silent skipping — a phase that is not run (e.g. a zero-length cooldown, or a
 * profile that applies live with no restart) still records a visible timeline
 * entry saying so.
 *
 * The engine (the component) owns real timers, telemetry and HTTP; this module
 * owns only the state logic and is fully unit-tested.
 */

export type LabState =
  | 'idle'
  | 'preflight'
  | 'awaiting-confirmation'
  | 'applying'
  | 'restarting'
  | 'reconnecting'
  | 'warmup'
  | 'measuring'
  | 'cooldown'
  | 'restoring'
  | 'complete'
  | 'aborted'
  | 'failed'
  | 'interrupted';

export const TERMINAL_STATES: ReadonlyArray<LabState> = ['complete', 'aborted', 'failed', 'interrupted'];

/** Non-terminal states in which real work is happening on the device. */
export const ACTIVE_STATES: ReadonlyArray<LabState> = [
  'applying', 'restarting', 'reconnecting', 'warmup', 'measuring', 'cooldown', 'restoring',
];

export type LabEventType =
  | 'START_PREFLIGHT'
  | 'PREFLIGHT_OK'
  | 'PREFLIGHT_FAIL'
  | 'CONFIRM'
  | 'CANCEL'
  | 'APPLY_APPLIED'     // profile applied live (no restart needed)
  | 'APPLY_RESTART'     // profile applied, a restart is required
  | 'APPLY_FAIL'
  | 'RESTART_SENT'
  | 'RECONNECTED'
  | 'RECONNECT_TIMEOUT'
  | 'WARMUP_DONE'
  | 'MEASURE_DONE'
  | 'COOLDOWN_DONE'
  | 'STOP'              // automatic stop condition
  | 'ABORT'             // owner-initiated abort
  | 'RESTORE_OK'
  | 'RESTORE_FAIL'
  | 'INTERRUPT'         // browser/session interruption detected on recovery
  | 'RESET';

export interface LabEvent {
  type: LabEventType;
  at: number;
  reason?: string;
}

export interface TimelineEntry {
  state: LabState;
  at: number;
  note?: string;
  profileIndex?: number;
}

export type RestoreOutcome = 'complete' | 'aborted' | 'failed';
export type RestoreResult = 'ok' | 'failed' | 'partial';

export interface LabSnapshot {
  state: LabState;
  /** Zero-based index of the profile currently being run. */
  profileIndex: number;
  profileCount: number;
  /** Why we entered restoring — decides the terminal state after restore. */
  restoreOutcome: RestoreOutcome | null;
  /** Whether the restore itself succeeded (recorded on RESTORE_OK/FAIL). */
  restoreResult: RestoreResult | null;
  /** Explanation for aborted/failed/interrupted (or the last stop reason). */
  reason: string | null;
  timeline: TimelineEntry[];
}

export function initialSnapshot(profileCount: number): LabSnapshot {
  return {
    state: 'idle',
    profileIndex: 0,
    profileCount: Math.max(0, Math.floor(profileCount) || 0),
    restoreOutcome: null,
    restoreResult: null,
    reason: null,
    timeline: [{ state: 'idle', at: 0, note: 'Session not started' }],
  };
}

const STATE_LABELS: { [k in LabState]: string } = {
  idle: 'Idle',
  preflight: 'Preflight',
  'awaiting-confirmation': 'Awaiting confirmation',
  applying: 'Applying profile',
  restarting: 'Restarting',
  reconnecting: 'Reconnecting',
  warmup: 'Warm-up',
  measuring: 'Measuring',
  cooldown: 'Cooldown',
  restoring: 'Restoring original',
  complete: 'Complete',
  aborted: 'Aborted',
  failed: 'Failed',
  interrupted: 'Operator session interrupted',
};

const STATE_EXPLANATIONS: { [k in LabState]: string } = {
  idle: 'No session is running. Configure profiles and run preflight to begin.',
  preflight: 'Checking every safety precondition before anything changes.',
  'awaiting-confirmation': 'Preflight passed. Review the plan and confirm to start.',
  applying: 'Writing the profile settings to the device.',
  restarting: 'The device is restarting to apply the settings.',
  reconnecting: 'Waiting for the device to come back and resume mining.',
  warmup: 'Letting the device stabilize. Warm-up samples are excluded from the profile averages.',
  measuring: 'Collecting the measurement window used to score this profile.',
  cooldown: 'Optional settle time before the next profile.',
  restoring: 'Restoring your original configuration.',
  complete: 'The session finished and your original configuration was restored.',
  aborted: 'The session was stopped and your original configuration was restored.',
  failed: 'The session could not complete. A restore of the original configuration was attempted.',
  interrupted: 'The browser session was interrupted while a run was in progress. On-device settings may not reflect a clean restore — verify the device.',
};

export function stateLabel(state: LabState): string {
  return STATE_LABELS[state] ?? state;
}

export function stateExplanation(state: LabState): string {
  return STATE_EXPLANATIONS[state] ?? '';
}

/**
 * Pure transition function. Returns the next state for a valid (state,event)
 * pair, or null when the transition is not allowed. Data-dependent branches
 * (advance vs finish, restore outcome) are resolved by the reducer, which has
 * the full snapshot; this function answers structural validity.
 */
export function nextState(state: LabState, event: LabEventType, snapshot?: LabSnapshot): LabState | null {
  // A fresh reset is allowed from any terminal state.
  if (event === 'RESET') {
    return TERMINAL_STATES.includes(state) || state === 'idle' ? 'idle' : null;
  }
  // Interruption can be recorded from any non-terminal state.
  if (event === 'INTERRUPT') {
    return TERMINAL_STATES.includes(state) ? null : 'interrupted';
  }

  switch (state) {
    case 'idle':
      return event === 'START_PREFLIGHT' ? 'preflight' : null;
    case 'preflight':
      if (event === 'PREFLIGHT_OK') return 'awaiting-confirmation';
      if (event === 'PREFLIGHT_FAIL') return 'idle';
      return null;
    case 'awaiting-confirmation':
      if (event === 'CONFIRM') return 'applying';
      if (event === 'CANCEL') return 'idle';
      return null;
    case 'applying':
      if (event === 'APPLY_APPLIED') return 'warmup';
      if (event === 'APPLY_RESTART') return 'restarting';
      if (event === 'APPLY_FAIL') return 'restoring';
      if (event === 'STOP' || event === 'ABORT') return 'restoring';
      return null;
    case 'restarting':
      if (event === 'RESTART_SENT') return 'reconnecting';
      if (event === 'STOP' || event === 'ABORT') return 'restoring';
      return null;
    case 'reconnecting':
      if (event === 'RECONNECTED') return 'warmup';
      if (event === 'RECONNECT_TIMEOUT') return 'restoring';
      if (event === 'STOP' || event === 'ABORT') return 'restoring';
      return null;
    case 'warmup':
      if (event === 'WARMUP_DONE') return 'measuring';
      if (event === 'STOP' || event === 'ABORT') return 'restoring';
      return null;
    case 'measuring':
      if (event === 'MEASURE_DONE') return 'cooldown';
      if (event === 'STOP' || event === 'ABORT') return 'restoring';
      return null;
    case 'cooldown':
      if (event === 'COOLDOWN_DONE') {
        const idx = snapshot?.profileIndex ?? 0;
        const count = snapshot?.profileCount ?? 1;
        return idx < count - 1 ? 'applying' : 'restoring';
      }
      if (event === 'STOP' || event === 'ABORT') return 'restoring';
      return null;
    case 'restoring':
      if (event === 'RESTORE_OK') {
        switch (snapshot?.restoreOutcome) {
          case 'aborted': return 'aborted';
          case 'failed': return 'failed';
          default: return 'complete';
        }
      }
      if (event === 'RESTORE_FAIL') return 'failed';
      return null;
    default:
      return null; // terminal states accept only RESET/INTERRUPT handled above
  }
}

/**
 * Apply an event to a snapshot. Invalid transitions are a no-op (the snapshot is
 * returned unchanged) so the engine can never silently corrupt state; callers
 * that must know use nextState(). Valid transitions append a timestamped
 * timeline entry and update the derived fields.
 */
export function reduce(snapshot: LabSnapshot, event: LabEvent): LabSnapshot {
  const target = nextState(snapshot.state, event.type, snapshot);
  if (target === null) {
    return snapshot;
  }

  let profileIndex = snapshot.profileIndex;
  let restoreOutcome = snapshot.restoreOutcome;
  let restoreResult = snapshot.restoreResult;
  let reason = snapshot.reason;
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
      profileIndex = 0;
      note = 'Owner confirmed — starting profile 1';
      break;
    case 'CANCEL':
      note = 'Cancelled before start';
      break;
    case 'APPLY_APPLIED':
      note = 'Profile applied live (no restart required)';
      break;
    case 'APPLY_RESTART':
      note = 'Profile applied — restart required';
      break;
    case 'APPLY_FAIL':
      restoreOutcome = 'failed';
      reason = event.reason ?? 'Applying the profile failed';
      note = reason;
      break;
    case 'RESTART_SENT':
      note = 'Restart command sent';
      break;
    case 'RECONNECTED':
      note = 'Device reconnected and mining resumed';
      break;
    case 'RECONNECT_TIMEOUT':
      restoreOutcome = 'failed';
      reason = event.reason ?? 'Device did not reconnect in time';
      note = reason;
      break;
    case 'WARMUP_DONE':
      note = 'Warm-up complete';
      break;
    case 'MEASURE_DONE':
      note = 'Measurement window complete';
      break;
    case 'COOLDOWN_DONE':
      if (target === 'applying') {
        profileIndex = snapshot.profileIndex + 1;
        note = `Advancing to profile ${profileIndex + 1}`;
      } else {
        restoreOutcome = restoreOutcome ?? 'complete';
        note = 'All profiles run — restoring original';
      }
      break;
    case 'STOP':
      restoreOutcome = 'aborted';
      reason = event.reason ?? 'Stop condition met';
      note = `Stopped: ${reason}`;
      break;
    case 'ABORT':
      restoreOutcome = 'aborted';
      reason = event.reason ?? 'Owner aborted';
      note = `Aborted: ${reason}`;
      break;
    case 'RESTORE_OK':
      restoreResult = 'ok';
      note = 'Original configuration restored';
      break;
    case 'RESTORE_FAIL':
      restoreResult = 'failed';
      reason = event.reason ?? 'Restore failed';
      note = reason;
      break;
    case 'INTERRUPT':
      reason = event.reason ?? 'Browser session interrupted';
      note = reason;
      break;
    case 'RESET':
      return initialSnapshot(snapshot.profileCount);
  }

  const entry: TimelineEntry = { state: target, at: event.at, note, profileIndex };
  return {
    state: target,
    profileIndex,
    profileCount: snapshot.profileCount,
    restoreOutcome,
    restoreResult,
    reason,
    timeline: [...snapshot.timeline, entry],
  };
}

/** Convenience: apply a list of events in order (for tests and recovery). */
export function reduceAll(snapshot: LabSnapshot, events: LabEvent[]): LabSnapshot {
  return events.reduce((snap, event) => reduce(snap, event), snapshot);
}

export function isTerminal(state: LabState): boolean {
  return TERMINAL_STATES.includes(state);
}

export function isActive(state: LabState): boolean {
  return ACTIVE_STATES.includes(state);
}
