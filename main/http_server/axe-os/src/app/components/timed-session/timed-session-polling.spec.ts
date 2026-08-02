/**
 * Gate B9 — PURE polling-policy tests: bounded cadence, bounded backoff and
 * the availability transition table.
 */

import { TimedSessionFailure } from 'src/app/services/timed-session.service';
import {
  ACTIVE_CADENCE_MS, BACKOFF_BASE_MS, BACKOFF_MAX_MS, HIDDEN_CADENCE_MS, IDLE_CADENCE_MS,
  availabilityForFailure, cadenceFor, nextBackoffMs, stopsPolling,
} from './timed-session-polling';

describe('polling: cadence', () => {
  it('polls quickly while busy and slowly while idle', () => {
    expect(cadenceFor({ busy: true, visible: true })).toBe(ACTIVE_CADENCE_MS);
    expect(cadenceFor({ busy: false, visible: true })).toBe(IDLE_CADENCE_MS);
    expect(ACTIVE_CADENCE_MS).toBeLessThan(IDLE_CADENCE_MS);
  });

  it('a hidden tab always polls slowly, busy or not', () => {
    expect(cadenceFor({ busy: true, visible: false })).toBe(HIDDEN_CADENCE_MS);
    expect(cadenceFor({ busy: false, visible: false })).toBe(HIDDEN_CADENCE_MS);
    expect(HIDDEN_CADENCE_MS).toBeGreaterThan(IDLE_CADENCE_MS);
  });
});

describe('polling: backoff', () => {
  it('steps exponentially from the base and is capped', () => {
    expect(nextBackoffMs(1)).toBe(BACKOFF_BASE_MS);
    expect(nextBackoffMs(2)).toBe(BACKOFF_BASE_MS * 2);
    expect(nextBackoffMs(3)).toBe(BACKOFF_BASE_MS * 4);
    expect(nextBackoffMs(50)).toBe(BACKOFF_MAX_MS);
    expect(BACKOFF_MAX_MS).toBe(60000);
  });

  it('is monotonic and never unbounded', () => {
    let prev = 0;
    for (let n = 1; n <= 20; n++) {
      const d = nextBackoffMs(n);
      expect(d).toBeGreaterThanOrEqual(prev);
      expect(d).toBeLessThanOrEqual(BACKOFF_MAX_MS);
      prev = d;
    }
  });

  it('treats a zero or negative failure count as the base delay', () => {
    expect(nextBackoffMs(0)).toBe(BACKOFF_BASE_MS);
    expect(nextBackoffMs(-3)).toBe(BACKOFF_BASE_MS);
  });
});

describe('polling: availability transitions', () => {
  function failure(kind: TimedSessionFailure['kind']): TimedSessionFailure {
    return { kind };
  }

  it('maps every failure kind', () => {
    expect(availabilityForFailure(failure('not-present'))).toBe('API_DISABLED_OR_NOT_PRESENT');
    expect(availabilityForFailure(failure('origin-denied'))).toBe('ORIGIN_DENIED');
    expect(availabilityForFailure(failure('offline'))).toBe('DEVICE_OFFLINE');
    expect(availabilityForFailure(failure('unavailable'))).toBe('TEMPORARILY_UNAVAILABLE');
    expect(availabilityForFailure(failure('unknown'))).toBe('UNKNOWN_ERROR');
  });

  it('treats a validation or conflict answer to a status GET as unknown', () => {
    expect(availabilityForFailure(failure('validation'))).toBe('UNKNOWN_ERROR');
    expect(availabilityForFailure(failure('conflict'))).toBe('UNKNOWN_ERROR');
  });

  it('a missing failure object still maps safely', () => {
    expect(availabilityForFailure(null)).toBe('UNKNOWN_ERROR');
    expect(availabilityForFailure(undefined)).toBe('UNKNOWN_ERROR');
  });

  it('stops polling ONLY for the two answers that cannot change by themselves', () => {
    expect(stopsPolling('API_DISABLED_OR_NOT_PRESENT')).toBeTrue();
    expect(stopsPolling('ORIGIN_DENIED')).toBeTrue();
    (['CHECKING', 'AVAILABLE', 'DEVICE_OFFLINE', 'TEMPORARILY_UNAVAILABLE', 'UNKNOWN_ERROR'] as const)
      .forEach((a) => expect(stopsPolling(a)).withContext(a).toBeFalse());
  });
});
