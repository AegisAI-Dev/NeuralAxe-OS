/**
 * NeuralAxe Timed Pool Session — PURE operator timeline (Phase 2M.1B, Gate B9).
 *
 * The timeline is DESCRIPTIVE, never authoritative. Every stage state is
 * derived from ONE sanitized backend status token; nothing is ever marked
 * complete because time has passed, and an unrecognised state leaves the whole
 * timeline in its safe pending shape.
 */

import { TimedSessionStatus } from 'src/app/generated/models';
import { TimelineStage, TimelineStageId, TimelineStageState, TimelineView } from './timed-session.models';

const ORDER: ReadonlyArray<{ id: TimelineStageId; label: string }> = [
  { id: 'accepted', label: 'Request accepted' },
  { id: 'snapshot', label: 'Source snapshot persisted' },
  { id: 'apply', label: 'Target applied' },
  { id: 'verify', label: 'Target verified' },
  { id: 'mining', label: 'Target mining' },
  { id: 'restoring', label: 'Source restoration' },
  { id: 'restore-verify', label: 'Source verified' },
  { id: 'complete', label: 'Completed' },
  { id: 'acknowledge', label: 'Acknowledgement' },
];

/**
 * How far the DURABLE state proves the flow got. The index is the count of
 * stages that are provably done; `-1` means the state is unrecognised.
 */
function durableProgress(token: string): number {
  switch (token) {
    case 'IDLE': return 0;
    case 'PREPARING': return 1;                 // accepted
    case 'TARGET_SNAPSHOT_COMMITTED': return 2; // + snapshot
    case 'APPLYING_TARGET': return 2;
    case 'RESTARTING_FOR_TARGET': return 3;     // + apply
    case 'VERIFYING_TARGET': return 3;
    case 'TARGET_ACTIVE': return 4;             // + verify; mining is HAPPENING, not finished
    case 'RESTORE_DUE': return 5;
    case 'APPLYING_RESTORE': return 5;
    case 'RESTARTING_FOR_RESTORE': return 6;    // + restoring
    case 'VERIFYING_RESTORE': return 6;
    case 'COMPLETE': return 8;                  // + restore-verify + complete
    case 'CANCELLED': return 2;
    case 'TARGET_FAILED': return 3;
    case 'INTERRUPTED': return 2;
    case 'RESTORE_FAILED': return 6;
    case 'RECOVERY_REQUIRED': return 0;
    default: return -1;
  }
}

/** The stage the device is actively working on, or null. */
function activeStage(token: string): TimelineStageId | null {
  switch (token) {
    case 'PREPARING': return 'snapshot';
    case 'TARGET_SNAPSHOT_COMMITTED': return 'apply';
    case 'APPLYING_TARGET': return 'apply';
    case 'RESTARTING_FOR_TARGET': return 'verify';
    case 'VERIFYING_TARGET': return 'verify';
    case 'TARGET_ACTIVE': return 'mining';
    case 'RESTORE_DUE': return 'restoring';
    case 'APPLYING_RESTORE': return 'restoring';
    case 'RESTARTING_FOR_RESTORE': return 'restore-verify';
    case 'VERIFYING_RESTORE': return 'restore-verify';
    case 'COMPLETE': return 'acknowledge';
    default: return null;
  }
}

/** The stage a failure state marks as failed, or null. */
function failedStage(token: string): TimelineStageId | null {
  switch (token) {
    case 'TARGET_FAILED': return 'verify';
    case 'RESTORE_FAILED': return 'restore-verify';
    case 'INTERRUPTED': return 'apply';
    default: return null;
  }
}

/**
 * A session that ended before the target was ever applied never restores, so
 * the restoration stages are shown as skipped rather than pending forever.
 */
function skipsRestoration(token: string, restoreRequired: boolean): boolean {
  return token === 'CANCELLED' && !restoreRequired;
}

export function buildTimeline(status: TimedSessionStatus): TimelineView {
  const token = status.durableState ?? '';
  const progress = durableProgress(token);
  const active = activeStage(token);
  const failed = failedStage(token);
  const guard = status.operatorRecoveryRequired === true || token === 'RECOVERY_REQUIRED';
  const skipRestore = skipsRestoration(token, status.restoreRequired === true);

  const stages: TimelineStage[] = ORDER.map((entry, index) => {
    let stageState: TimelineStageState = 'pending';
    if (progress < 0) {
      stageState = 'pending'; // unrecognised state: claim nothing at all
    } else if (failed === entry.id) {
      stageState = 'failed';
    } else if (index < progress) {
      stageState = 'done';
    } else if (active === entry.id && !guard) {
      stageState = 'active';
    }
    if (skipRestore && (entry.id === 'restoring' || entry.id === 'restore-verify' || entry.id === 'mining' || entry.id === 'verify')) {
      stageState = 'skipped';
    }
    // Acknowledgement only lights up once the device says a result is retained.
    if (entry.id === 'acknowledge') {
      stageState = status.terminalResultPending === true
        ? (guard ? 'pending' : 'active')
        : (progress >= 8 ? 'done' : stageState === 'active' ? 'pending' : stageState);
    }
    return { id: entry.id, label: entry.label, state: stageState };
  });

  return {
    stages,
    interrupted: guard,
    interruptionDetail: guard
      ? 'The device failed closed and locked further changes. The stages below are history, not a forecast — ' +
        'an operator must resolve the recovery state before anything continues.'
      : '',
  };
}
