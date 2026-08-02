/**
 * NeuralAxe Timed Pool Session — PURE command-correlation contract (Gate B9).
 *
 * THE PROBLEM
 * -----------
 * The page submits a command and gets HTTP 202, which means "queued" and
 * nothing more. To say anything further it must read the device's own status.
 * But a status snapshot carries the result of whatever the device processed
 * LAST — which may be a command from a previous page instance, or from another
 * client on the same unauthenticated LAN. Reading such a snapshot naively
 * produces a completion claim for a command the device has not looked at.
 *
 * A `client_request_id` echo narrows this, but cannot close it: the id is
 * client-chosen, unauthenticated, only 32 bits, and RAM-only, so a reloaded
 * page or a second client can legitimately present the same number.
 *
 * THE CONTRACT
 * ------------
 * Before submitting, the page captures a BASELINE of what the device was
 * reporting. A later status may resolve that command only when ALL of:
 *
 *   1. the echoed `client_request_id` matches the one sent;
 *   2. the echoed command KIND matches the one sent;
 *   3. the status sequence is strictly NEWER than the baseline; and
 *   4. the command-specific authoritative state transition has actually
 *      happened — a durable session now exists, restoration has genuinely
 *      progressed, the retained terminal result has genuinely cleared.
 *
 * Check 4 is the one that carries the weight. `lastCommandResult` alone is
 * never sufficient for a success verdict, because it is exactly the field a
 * stale or foreign command can populate.
 *
 * WHAT THE RESULT MEANS
 * ---------------------
 * Even with all four checks passing, this is CORRELATION, not proof of
 * ownership. The honest claim is about the DEVICE'S POSTURE — "the device now
 * reports the requested posture" — never "your request completed". The
 * `attribution` field carries that distinction explicitly so no caller can
 * accidentally upgrade it.
 *
 * NOTHING here reads a clock, performs IO, or touches storage.
 */

import { TimedSessionStatus } from 'src/app/generated/models';
import { CommandKind } from './timed-session.models';

/** Backend `lastCommand` / `pendingCommand` token for each UI command kind. */
export const COMMAND_TOKEN: Readonly<Record<CommandKind, string>> = {
  create: 'CREATE_SESSION',
  restore: 'RESTORE_NOW',
  acknowledge: 'ACKNOWLEDGE_TERMINAL',
};

/**
 * Durable states in which source restoration is under way or already past.
 * `RESTORE_FAILED` counts: the device genuinely attempted the return and is
 * reporting the outcome, which is a real transition, not a silent no-op.
 */
const RESTORE_OR_BEYOND: ReadonlySet<string> = new Set([
  'RESTORE_DUE', 'APPLYING_RESTORE', 'RESTARTING_FOR_RESTORE',
  'VERIFYING_RESTORE', 'COMPLETE', 'RESTORE_FAILED',
]);

/**
 * What the device was reporting immediately BEFORE a command was submitted.
 * Bounded, structural and identity-free — it is a snapshot of tokens and
 * booleans, never of pool identity.
 */
export interface CommandBaseline {
  readonly statusSequence: number;
  readonly durableState: string;
  readonly runtimeState: string;
  readonly executionState: string;
  readonly sessionPresent: boolean;
  readonly terminalResultPending: boolean;
  readonly restoreRequired: boolean;
  readonly lastCommand: string;
  readonly lastCommandResult: string;
  readonly lastClientRequestId: number;
}

/** Capture the pre-submit baseline. Returns null when nothing is known yet. */
export function captureBaseline(status: TimedSessionStatus | null | undefined): CommandBaseline | null {
  if (!status) {
    return null;
  }
  return {
    statusSequence: status.statusSequence ?? 0,
    durableState: status.durableState ?? '',
    runtimeState: status.runtimeState ?? '',
    executionState: status.executionState ?? '',
    sessionPresent: status.sessionPresent === true,
    terminalResultPending: status.terminalResultPending === true,
    restoreRequired: status.restoreRequired === true,
    lastCommand: status.lastCommand ?? 'NONE',
    lastCommandResult: status.lastCommandResult ?? '',
    lastClientRequestId: status.lastClientRequestId ?? 0,
  };
}

/** Outcome of one correlation attempt. */
export type CommandOutcome = 'queued' | 'processing' | 'posture-reached' | 'refused';

export interface CommandResolution {
  readonly outcome: CommandOutcome;
  /**
   * `device-posture` — the device's own status shows the requested posture.
   * `none` — nothing attributable has been observed yet.
   *
   * There is deliberately no `owned` value. This dashboard cannot prove that
   * the device acted on THIS page's request, and no field here may imply it.
   */
  readonly attribution: 'device-posture' | 'none';
  /** Which of the four checks failed first, for tests and diagnostics. */
  readonly blockedBy: 'none' | 'pending' | 'id' | 'kind' | 'sequence' | 'evidence';
}

export interface CorrelationInput {
  readonly kind: CommandKind;
  readonly clientRequestId: number;
  readonly baseline: CommandBaseline | null;
  readonly status: TimedSessionStatus;
}

/**
 * Has the authoritative, command-specific transition actually happened?
 *
 * Each answer is a statement about DEVICE STATE, never about a result code.
 */
export function commandEvidence(
  kind: CommandKind,
  baseline: CommandBaseline,
  status: TimedSessionStatus,
): boolean {
  const durable = status.durableState ?? '';
  switch (kind) {
    case 'create': {
      // A durable session must now exist, and the device must have moved off
      // the free posture it was in when the request was sent.
      const advanced = durable !== baseline.durableState
        || (status.runtimeState ?? '') !== baseline.runtimeState
        || (status.executionState ?? '') !== baseline.executionState;
      return status.sessionPresent === true && durable !== 'IDLE' && durable !== '' && advanced;
    }
    case 'restore': {
      // Restoration must be under way or past — and must have MOVED. A device
      // that was already sitting in the same restore state proves nothing.
      return RESTORE_OR_BEYOND.has(durable) && durable !== baseline.durableState;
    }
    case 'acknowledge': {
      // The retained result must genuinely be gone. Nothing is cleared here
      // optimistically; the device has to say so itself.
      return baseline.terminalResultPending && status.terminalResultPending !== true;
    }
    default:
      return false;
  }
}

/**
 * Apply the full contract to ONE status snapshot.
 *
 * Anything short of all four checks leaves the command queued. Staying queued
 * is always safe: it says the device has not reported on this request yet,
 * which is exactly true.
 */
export function resolveCommand(input: CorrelationInput): CommandResolution {
  const { kind, clientRequestId, baseline, status } = input;

  // The device is holding a command right now.
  if (status.commandPending === true) {
    return { outcome: 'processing', attribution: 'none', blockedBy: 'pending' };
  }
  if (!baseline) {
    return { outcome: 'queued', attribution: 'none', blockedBy: 'sequence' };
  }

  // 1. id echo
  const echoedId = status.lastClientRequestId ?? 0;
  if (clientRequestId === 0 || echoedId !== clientRequestId) {
    return { outcome: 'queued', attribution: 'none', blockedBy: 'id' };
  }
  // 2. command kind echo
  if ((status.lastCommand ?? 'NONE') !== COMMAND_TOKEN[kind]) {
    return { outcome: 'queued', attribution: 'none', blockedBy: 'kind' };
  }
  // 3. the snapshot must be strictly newer than the pre-submit baseline
  if ((status.statusSequence ?? 0) <= baseline.statusSequence) {
    return { outcome: 'queued', attribution: 'none', blockedBy: 'sequence' };
  }

  // A refusal is safe to report on the first three checks: it claims nothing
  // happened, which cannot mislead an operator into thinking it did.
  const result = status.lastCommandResult ?? '';
  if (result !== '' && result !== 'ACCEPTED') {
    return { outcome: 'refused', attribution: 'none', blockedBy: 'none' };
  }

  // 4. the authoritative, command-specific transition
  if (!commandEvidence(kind, baseline, status)) {
    return { outcome: 'queued', attribution: 'none', blockedBy: 'evidence' };
  }
  return { outcome: 'posture-reached', attribution: 'device-posture', blockedBy: 'none' };
}
