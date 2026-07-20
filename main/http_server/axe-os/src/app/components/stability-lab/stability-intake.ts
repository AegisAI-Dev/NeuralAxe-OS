/**
 * NeuralAxe Stability Lab — event-driven telemetry intake (Phase 2K.1).
 *
 * The reliability fix at the heart of this phase: evidence is accepted when a
 * GENUINELY NEW telemetry arrival is delivered on the shared stream, NOT when a
 * dedicated 5 s timer fires. A backgrounded tab throttles `setInterval`/
 * `setTimeout` to ~once per minute, which is exactly why a 600 s window could
 * finish with only ~10 timer-driven samples while telemetry itself kept flowing.
 *
 * DEDUP IS ON ARRIVAL IDENTITY, NOT PAYLOAD CONTENT. Each genuine emission is
 * stamped with a monotonic `arrivalId` at the single shared boundary
 * (TelemetryArrivalService). Using content difference would be unsafe — a real
 * new reading may be byte-identical to the previous one (stable temps/power/fan/
 * counters) and must still count. The arrival identity distinguishes:
 *   1. two genuine arrivals with identical payloads   → two NEW ids → two samples;
 *   2. a shareReplay replay / re-subscription         → the SAME id  → one sample;
 *   3. a reconnect replay of the last arrival         → the SAME id  → no dup;
 *   4. an Angular re-render                            → no emission  → no sample;
 *   5. a timer callback without a new arrival          → not called   → no sample.
 *
 * A monotonic min-cadence gate then holds the ~5 s cadence so a stream that
 * arrives faster than the cadence is not over-sampled. Gap payloads (no core
 * telemetry) are never evidence. This module owns no timers and no subscriptions.
 */

import { SAMPLE_INTERVAL_MS } from './stability-telemetry';

/** Minimum monotonic spacing between two accepted samples (the configured cadence). */
export const MIN_SAMPLE_INTERVAL_MS = SAMPLE_INTERVAL_MS;

/**
 * A sample arriving slightly early (jitter under the cadence) is still accepted,
 * so a stream that emits at ~4.9 s does not get halved to a 10 s effective rate.
 */
export const CADENCE_TOLERANCE_MS = 500;

export type IntakeReason = 'accepted' | 'no-core-telemetry' | 'duplicate-arrival' | 'too-soon';

export interface IntakeState {
  /** Monotonic receipt time (performance.now) of the last accepted sample. */
  lastAcceptedMonoMs: number | null;
  /** Arrival identity of the last accepted sample (the dedup key). */
  lastAcceptedArrivalId: number | null;
  /** Number of samples accepted since the state was created/reset. */
  acceptedCount: number;
}

export interface IntakeDecision {
  accept: boolean;
  reason: IntakeReason;
  /** The next intake state (unchanged when the sample is rejected). */
  state: IntakeState;
}

export function initialIntake(): IntakeState {
  return { lastAcceptedMonoMs: null, lastAcceptedArrivalId: null, acceptedCount: 0 };
}

const isFiniteNum = (v: unknown): v is number => typeof v === 'number' && isFinite(v);

/**
 * True when a payload carries real core telemetry — a finite hashrate OR a
 * positive ASIC temperature. A payload with neither is a gap (no evidence) and
 * must never be counted or re-stamp freshness. Kept identical to the sample
 * builder's gap rule so the two agree.
 */
export function hasCoreTelemetry(info: { [k: string]: any } | null | undefined): boolean {
  if (!info) return false;
  const hr = info['hashRate'];
  const temp = info['temp'];
  return isFiniteNum(hr) || (isFiniteNum(temp) && temp > 0);
}

/**
 * Decide whether a genuine telemetry ARRIVAL becomes a measurement sample. Pure:
 * given the same state and inputs it returns the same decision; the caller threads
 * `decision.state` into the next call.
 *
 * @param arrivalId  the monotonic identity minted at the shared boundary — the
 *                   ONLY proof of a new arrival (never payload content).
 * @param nowMonoMs  a monotonic clock (performance.now) so a wall-clock change can
 *                   never inflate or starve the evidence rate.
 */
export function considerArrival(
  state: IntakeState,
  info: { [k: string]: any } | null | undefined,
  arrivalId: number,
  nowMonoMs: number,
  cadenceMs: number = MIN_SAMPLE_INTERVAL_MS,
  toleranceMs: number = CADENCE_TOLERANCE_MS,
): IntakeDecision {
  // 1. No real telemetry — a gap is never evidence.
  if (!hasCoreTelemetry(info)) {
    return { accept: false, reason: 'no-core-telemetry', state };
  }

  // 2. Not a new arrival. A replay / re-subscription / reconnect replay carries an
  //    id we have already accepted (ids are monotonic, so anything <= the last
  //    accepted id is old). A genuinely new arrival — even with a byte-identical
  //    payload — has a strictly greater id and passes this gate.
  if (state.lastAcceptedArrivalId !== null && arrivalId <= state.lastAcceptedArrivalId) {
    return { accept: false, reason: 'duplicate-arrival', state };
  }

  // 3. Genuinely new, but too soon after the last accepted sample — hold the
  //    cadence so a faster-than-cadence stream is not over-sampled. The first
  //    sample of a window (no prior accepted sample) is always taken.
  if (state.lastAcceptedMonoMs !== null) {
    const elapsed = Math.max(0, nowMonoMs - state.lastAcceptedMonoMs);
    if (elapsed < cadenceMs - toleranceMs) {
      return { accept: false, reason: 'too-soon', state };
    }
  }

  return {
    accept: true,
    reason: 'accepted',
    state: {
      lastAcceptedMonoMs: nowMonoMs,
      lastAcceptedArrivalId: arrivalId,
      acceptedCount: state.acceptedCount + 1,
    },
  };
}
