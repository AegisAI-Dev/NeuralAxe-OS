/**
 * NeuralAxe Timed Pool Session — PURE polling policy (Phase 2M.1B, Gate B9).
 *
 * Bounded cadence, stepped backoff and the availability transition table, kept
 * out of the RxJS wiring so the timing contract can be verified deterministically.
 *
 * NOTHING here reads a clock or performs IO. A missed poll is never treated as
 * a session failure: it only changes how soon the next read happens.
 */

import { TimedSessionFailure } from 'src/app/services/timed-session.service';
import { TimedSessionAvailability } from './timed-session.models';

/** Cadence while a session is running or a command is queued. */
export const ACTIVE_CADENCE_MS = 2000;
/** Cadence while the device is idle and nothing is queued. */
export const IDLE_CADENCE_MS = 10000;
/** Slow cadence while the browser tab is hidden. */
export const HIDDEN_CADENCE_MS = 30000;
/** First backoff step after a device/network error. */
export const BACKOFF_BASE_MS = 4000;
/** Hard ceiling for the backoff. */
export const BACKOFF_MAX_MS = 60000;

export interface CadenceInputs {
  /** The device reports a session record or a queued command. */
  readonly busy: boolean;
  /** `document.visibilityState === 'visible'`. */
  readonly visible: boolean;
}

/**
 * Ordinary (non-error) cadence. A hidden tab always polls slowly, because a
 * dashboard nobody is looking at should not keep a miner busy.
 */
export function cadenceFor(inputs: CadenceInputs): number {
  if (!inputs.visible) {
    return HIDDEN_CADENCE_MS;
  }
  return inputs.busy ? ACTIVE_CADENCE_MS : IDLE_CADENCE_MS;
}

/**
 * Stepped backoff for the Nth consecutive failure (N ≥ 1): base·2^(N−1),
 * capped at BACKOFF_MAX_MS. N ≤ 0 returns the base delay. Bounded and
 * monotonic — it never grows without limit and never resets itself.
 */
export function nextBackoffMs(consecutiveFailures: number, base: number = BACKOFF_BASE_MS, max: number = BACKOFF_MAX_MS): number {
  const n = Math.max(1, Math.floor(consecutiveFailures));
  const raw = base * Math.pow(2, n - 1);
  return Math.min(max, raw);
}

/**
 * Map ONE failed status read onto the bounded availability model.
 *
 * A 404 is the capability answer: this firmware was built without the
 * timed-session API, so there is nothing to keep asking for.
 */
export function availabilityForFailure(failure: TimedSessionFailure | null | undefined): TimedSessionAvailability {
  switch (failure?.kind) {
    case 'not-present': return 'API_DISABLED_OR_NOT_PRESENT';
    case 'origin-denied': return 'ORIGIN_DENIED';
    case 'offline': return 'DEVICE_OFFLINE';
    case 'unavailable': return 'TEMPORARILY_UNAVAILABLE';
    case 'validation':
    case 'conflict':
      // A status GET cannot legitimately produce these; treat as unknown.
      return 'UNKNOWN_ERROR';
    default: return 'UNKNOWN_ERROR';
  }
}

/**
 * True when automatic polling must STOP until the operator explicitly asks
 * again. Only the two answers that will not change by themselves qualify.
 */
export function stopsPolling(availability: TimedSessionAvailability): boolean {
  return availability === 'API_DISABLED_OR_NOT_PRESENT' || availability === 'ORIGIN_DENIED';
}
