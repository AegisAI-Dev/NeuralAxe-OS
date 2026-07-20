import {
  considerArrival, initialIntake, hasCoreTelemetry, MIN_SAMPLE_INTERVAL_MS,
} from './stability-intake';
import { shareDelta } from './stability-telemetry';

const reading = (over: { [k: string]: any } = {}) => ({
  hashRate: 1290, power: 21, temp: 55, vrTemp: 45, fanrpm: 4200, fanspeed: 55,
  sharesAccepted: 1000, sharesRejected: 5, errorPercentage: 0.5, responseTime: 40, uptimeSeconds: 100,
  ...over,
});

/**
 * Drive a sequence of arrivals {info, arrivalId, t} through the intake and return
 * how many were accepted, with the per-step reasons.
 */
function drive(seq: Array<{ info: any; id: number; t: number }>) {
  let state = initialIntake();
  const reasons: string[] = [];
  for (const { info, id, t } of seq) {
    const d = considerArrival(state, info, id, t);
    state = d.state;
    reasons.push(d.reason);
  }
  return { accepted: state.acceptedCount, reasons, state };
}

describe('stability-intake — arrival-identity evidence acceptance', () => {
  // A. Two genuine arrivals with IDENTICAL payloads → two samples.
  it('A. two identical-payload genuine arrivals five seconds apart → two samples', () => {
    const identical = reading({ hashRate: 1290, temp: 55, power: 21 });
    const { accepted } = drive([
      { info: { ...identical }, id: 1, t: 0 },
      { info: { ...identical }, id: 2, t: 5000 }, // byte-identical values, NEW arrival id
    ]);
    expect(accepted).toBe(2); // content is identical — identity is not
  });

  // B. One arrival replayed three times → one sample.
  it('B. one arrival replayed three times → one sample', () => {
    const r = reading();
    const { accepted, reasons } = drive([
      { info: r, id: 7, t: 0 },
      { info: r, id: 7, t: 6000 },   // same arrival id replayed later
      { info: { ...r }, id: 7, t: 12000 },
    ]);
    expect(accepted).toBe(1);
    expect(reasons.slice(1)).toEqual(['duplicate-arrival', 'duplicate-arrival']);
  });

  // C. Unsubscribe/resubscribe shareReplay → no new sample (same id replayed).
  it('C. a re-subscription replay of the last arrival adds no sample', () => {
    let state = initialIntake();
    state = considerArrival(state, reading(), 4, 0).state;
    const resub = considerArrival(state, reading({ hashRate: 1291 }), 4, 8000); // replayed id 4
    expect(resub.accept).toBeFalse();
    expect(resub.reason).toBe('duplicate-arrival');
    expect(resub.state.acceptedCount).toBe(1);
  });

  // D. Reconnect replay → no duplicate.
  it('D. a reconnect replay of the last known arrival does not duplicate evidence', () => {
    let state = initialIntake();
    const last = reading({ hashRate: 1290 });
    state = considerArrival(state, last, 10, 0).state;
    const replay = considerArrival(state, { ...last }, 10, 40000); // reconnect replays id 10
    expect(replay.accept).toBeFalse();
    expect(replay.reason).toBe('duplicate-arrival');
    // The first genuinely new arrival after reconnect IS accepted.
    const fresh = considerArrival(replay.state, reading({ hashRate: 1293 }), 11, 41000);
    expect(fresh.accept).toBeTrue();
    expect(fresh.state.acceptedCount).toBe(2);
  });

  // E. Genuine identical HTTP fallback emissions → separate samples (same contract).
  it('E. genuine identical HTTP-fallback arrivals → separate samples', () => {
    const identical = reading({ hashRate: 1290, temp: 55 });
    const { accepted } = drive([
      { info: { ...identical }, id: 1, t: 0 },
      { info: { ...identical }, id: 2, t: 60000 },  // 60 s hidden-poll cadence, identical values
      { info: { ...identical }, id: 3, t: 120000 },
    ]);
    expect(accepted).toBe(3);
  });

  // F. Genuine STATIC telemetry over 600 s → sufficient coverage (the core fix).
  it('F. genuine static telemetry for 600 s yields ~full coverage (identical values still count)', () => {
    const seq = [];
    const staticReading = reading({ hashRate: 1290, temp: 55, power: 21, sharesAccepted: 1000, sharesRejected: 5 });
    for (let i = 0; i <= 120; i++) seq.push({ info: { ...staticReading }, id: i + 1, t: i * 5000 });
    const { accepted } = drive(seq);
    // 121 arrivals at 5 s cadence over 600 s — all byte-identical, all counted.
    expect(accepted).toBe(121);
  });

  it('holds the cadence — arrivals faster than the cadence yield ~one sample per slot', () => {
    const seq = [];
    for (let i = 0; i <= 20; i++) seq.push({ info: reading({ hashRate: 1290 + i }), id: i + 1, t: i * 1000 });
    const { accepted } = drive(seq); // 1 s arrivals → accept at t=0,5,10,15,20 → 5
    expect(accepted).toBe(5);
  });

  it('accepts a slightly-early arrival within the jitter tolerance', () => {
    let state = initialIntake();
    state = considerArrival(state, reading(), 1, 0).state;
    const early = considerArrival(state, reading({ hashRate: 1295 }), 2, 4600); // 4.6 s
    expect(early.accept).toBeTrue();
  });

  it('a gap arrival (no hashrate and no valid temp) is not a valid sample', () => {
    const gap = { version: 'v', uptimeSeconds: 100, temp: 0 };
    expect(hasCoreTelemetry(gap)).toBeFalse();
    const d = considerArrival(initialIntake(), gap, 1, 0);
    expect(d.accept).toBeFalse();
    expect(d.reason).toBe('no-core-telemetry');
    expect(d.state.acceptedCount).toBe(0);
  });

  it('accepts a temperature-only arrival (hashrate briefly absent)', () => {
    const tempOnly = { temp: 52, uptimeSeconds: 100 };
    expect(hasCoreTelemetry(tempOnly)).toBeTrue();
    expect(considerArrival(initialIntake(), tempOnly, 1, 0).accept).toBeTrue();
  });

  it('remains reset-aware — a post-reboot counter reset is a new arrival and the delta flags the reset', () => {
    let state = initialIntake();
    const before = reading({ sharesAccepted: 5000, sharesRejected: 20 });
    const afterReboot = reading({ sharesAccepted: 3, sharesRejected: 0 });
    state = considerArrival(state, before, 1, 0).state;
    const post = considerArrival(state, afterReboot, 2, 6000);
    expect(post.accept).toBeTrue();
    const d = shareDelta(5000, 20, 3, 0);
    expect(d.reset).toBeTrue();
    expect(d.accepted).toBe(3);
  });

  it('an out-of-order (older) arrival id is treated as a duplicate, never over-sampling', () => {
    let state = initialIntake();
    state = considerArrival(state, reading(), 5, 0).state;
    const older = considerArrival(state, reading({ hashRate: 1295 }), 4, 9000); // stale id
    expect(older.accept).toBeFalse();
    expect(older.reason).toBe('duplicate-arrival');
  });
});
