import {
  FRESHNESS_LIMIT_MS,
  SESSION_RECONNECT_GRACE_MS,
  evaluateFreshness,
  shouldAbortForStaleness,
  reconnectGraceRemainingMs,
} from './stability-freshness';

describe('evaluateFreshness (monotonic receipt time)', () => {
  it('reports none before any sample is received', () => {
    const f = evaluateFreshness(null, 10_000);
    expect(f.state).toBe('none');
    expect(f.online).toBeFalse();
    expect(f.ageMs).toBeNull();
  });

  it('treats a just-received sample as fresh (WebSocket or poll — source-agnostic)', () => {
    const f = evaluateFreshness(100_000, 102_000); // 2 s old
    expect(f.state).toBe('fresh');
    expect(f.online).toBeTrue();
    expect(f.ageMs).toBe(2000);
  });

  it('keeps fresh right up to the limit and flips stale just past it', () => {
    expect(evaluateFreshness(0, FRESHNESS_LIMIT_MS).state).toBe('fresh');       // exactly at limit
    expect(evaluateFreshness(0, FRESHNESS_LIMIT_MS + 1).state).toBe('stale');   // one ms past
  });

  it('marks retained-but-old telemetry stale and offline', () => {
    const f = evaluateFreshness(0, 20_000); // 20 s, limit 15 s
    expect(f.state).toBe('stale');
    expect(f.online).toBeFalse();
    expect(f.reason).toContain('reconnect');
  });

  it('never produces a negative or false-fresh age when the monotonic clock jitters', () => {
    // now < last (should be impossible with a monotonic clock, but be defensive)
    const f = evaluateFreshness(500_000, 400_000);
    expect(f.ageMs).toBe(0);
    expect(f.state).toBe('fresh'); // clamped to 0, not negative, not stale
  });

  it('a genuinely new sample restores freshness', () => {
    const stale = evaluateFreshness(0, 30_000);
    expect(stale.online).toBeFalse();
    const restored = evaluateFreshness(30_000, 31_000); // new receipt at 30 s, checked at 31 s
    expect(restored.online).toBeTrue();
  });
});

describe('shouldAbortForStaleness (session reconnect grace)', () => {
  it('does not abort while within the freshness limit', () => {
    expect(shouldAbortForStaleness(FRESHNESS_LIMIT_MS - 1)).toBeFalse();
  });

  it('does not abort while stale but still inside the reconnect grace', () => {
    expect(shouldAbortForStaleness(FRESHNESS_LIMIT_MS + SESSION_RECONNECT_GRACE_MS - 1)).toBeFalse();
  });

  it('aborts once staleness exceeds the limit plus the grace', () => {
    expect(shouldAbortForStaleness(FRESHNESS_LIMIT_MS + SESSION_RECONNECT_GRACE_MS + 1)).toBeTrue();
  });

  it('never aborts on a null age', () => {
    expect(shouldAbortForStaleness(null)).toBeFalse();
  });

  it('reports the remaining reconnect grace, hitting zero when spent', () => {
    expect(reconnectGraceRemainingMs(FRESHNESS_LIMIT_MS)).toBe(SESSION_RECONNECT_GRACE_MS);
    expect(reconnectGraceRemainingMs(FRESHNESS_LIMIT_MS + SESSION_RECONNECT_GRACE_MS + 5000)).toBe(0);
  });
});
