/**
 * NeuralAxe Stability Lab — telemetry freshness contract (Phase 2K).
 *
 * "Online" is NOT the mere presence of a retained telemetry object, and it is
 * NOT the WebSocket socket state (the 5 s HTTP poll keeps telemetry flowing even
 * when the live socket is down). Online means a GENUINELY NEW sample arrived
 * recently, measured against a MONOTONIC receipt clock (performance.now()), so a
 * system-clock change can never fabricate a fresh — or negative — age.
 *
 * Only non-gap samples (carrying real telemetry) refresh the receipt time; a
 * gap sample and a bare component re-render never do.
 */

/** Retained telemetry older than this is Stale (blocks a session start). */
export const FRESHNESS_LIMIT_MS = 15000;

/**
 * During a running session, once telemetry goes stale the run enters the
 * reconnect/offline path; if it stays stale this long past the freshness limit
 * the session aborts. A genuinely new sample within the grace restores it.
 */
export const SESSION_RECONNECT_GRACE_MS = 30000;

/** How often the component re-evaluates freshness / drives the session. */
export const HEARTBEAT_MS = 5000;

export type FreshnessState = 'fresh' | 'stale' | 'none';

export interface Freshness {
  state: FreshnessState;
  /** Monotonic age of the last genuinely-new sample, ms; null when none yet. */
  ageMs: number | null;
  /** True only when state === 'fresh'. */
  online: boolean;
  /** Owner-facing explanation (age / limit / reconnect). */
  reason: string;
}

/**
 * Pure freshness evaluation. Both times are MONOTONIC (performance.now()); age
 * is clamped to be non-negative so a backwards clock reads as age 0 (never
 * negative, never "extra fresh").
 */
export function evaluateFreshness(
  lastFreshMonoMs: number | null,
  nowMonoMs: number,
  limitMs: number = FRESHNESS_LIMIT_MS,
): Freshness {
  if (lastFreshMonoMs === null || !isFinite(lastFreshMonoMs)) {
    return { state: 'none', ageMs: null, online: false, reason: 'No telemetry received yet.' };
  }
  const ageMs = Math.max(0, nowMonoMs - lastFreshMonoMs);
  const limitS = Math.round(limitMs / 1000);
  if (ageMs <= limitMs) {
    return { state: 'fresh', ageMs, online: true, reason: `Live telemetry ${(ageMs / 1000).toFixed(0)} s old.` };
  }
  return {
    state: 'stale', ageMs, online: false,
    reason: `Telemetry is ${(ageMs / 1000).toFixed(0)} s old (over the ${limitS} s freshness limit) — reconnect before running.`,
  };
}

/**
 * Whether a running session must abort because telemetry has been stale beyond
 * the reconnect grace. Measured from the last genuinely-new sample: abort once
 * the age exceeds the freshness limit PLUS the grace. A fresh sample resets the
 * age and thus cancels the pending abort.
 */
export function shouldAbortForStaleness(
  ageMs: number | null,
  limitMs: number = FRESHNESS_LIMIT_MS,
  graceMs: number = SESSION_RECONNECT_GRACE_MS,
): boolean {
  return ageMs !== null && isFinite(ageMs) && ageMs > limitMs + graceMs;
}

/** Seconds of reconnect grace left before a stale session aborts; 0 when spent. */
export function reconnectGraceRemainingMs(
  ageMs: number | null,
  limitMs: number = FRESHNESS_LIMIT_MS,
  graceMs: number = SESSION_RECONNECT_GRACE_MS,
): number | null {
  if (ageMs === null || !isFinite(ageMs)) return null;
  return Math.max(0, (limitMs + graceMs) - ageMs);
}
