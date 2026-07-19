import {
  SAMPLE_INTERVAL_MS,
  LabSample,
  buildSample,
  finiteOrNull,
  shareDelta,
  accumulateShareDeltas,
  expectedSampleCount,
  countGaps,
} from './stability-telemetry';

const ctx = (over: Partial<{ sessionStartMs: number; now: number; phase: any; profileIndex: number }> = {}) => ({
  sessionStartMs: 1000,
  now: 6000,
  phase: 'measure' as const,
  profileIndex: 0,
  ...over,
});

describe('buildSample', () => {
  it('builds a guarded sample with monotonic session-relative time', () => {
    const sample = buildSample({
      hashRate: 1290, power: 21, temp: 60, vrTemp: 45, appliedFanPercent: 55, requestedFanPercent: 50,
      fanrpm: 15230, errorPercentage: 0.8, sharesAccepted: 100, sharesRejected: 2, responseTime: 30,
      thermalControlMode: 'curve', effectiveControlTemperature: 60, emergencyOverrideActive: 0,
      controlSensorValid: 1, miningPaused: false,
    }, ctx());
    expect(sample.tMs).toBe(5000);
    expect(sample.hashRate).toBe(1290);
    expect(sample.efficiency).toBeCloseTo(21 / 1.29, 3);
    expect(sample.sensorValid).toBeTrue();
    expect(sample.emergencyOverride).toBeFalse();
    expect(sample.gap).toBeFalse();
  });

  it('never records NaN or Infinity — invalid readings become null', () => {
    const sample = buildSample({
      hashRate: NaN, power: Infinity, temp: -1, vrTemp: 0, errorPercentage: NaN,
      sharesAccepted: -5, responseTime: -3,
    } as any, ctx());
    expect(sample.hashRate).toBeNull();
    expect(sample.power).toBeNull();
    expect(sample.asicTemp).toBeNull();   // -1 is "unavailable"
    expect(sample.vrmTemp).toBeNull();    // 0 is "unavailable"
    expect(sample.efficiency).toBeNull();
    expect(sample.sharesAccepted).toBeNull(); // negative counter rejected
    expect(sample.poolLatency).toBeNull();
    expect(finiteOrNull(NaN)).toBeNull();
  });

  it('clamps time to be non-negative even if the clock jitters', () => {
    const sample = buildSample({ hashRate: 1 }, ctx({ now: 500, sessionStartMs: 1000 }));
    expect(sample.tMs).toBe(0);
  });

  it('marks a gap when core telemetry is entirely missing', () => {
    const sample = buildSample({ power: 20 } as any, ctx());
    expect(sample.gap).toBeTrue();
  });
});

describe('shareDelta (cumulative, reset-aware)', () => {
  it('returns the increment between two readings', () => {
    expect(shareDelta(100, 2, 110, 3)).toEqual({ accepted: 10, rejected: 1, reset: false });
  });

  it('returns zero for the first reading of a window', () => {
    expect(shareDelta(null, null, 100, 2)).toEqual({ accepted: 0, rejected: 0, reset: false });
  });

  it('detects a counter reset (reboot) and attributes the post-reset totals', () => {
    const d = shareDelta(5000, 40, 12, 0);
    expect(d.reset).toBeTrue();
    expect(d.accepted).toBe(12);
    expect(d.rejected).toBe(0);
  });

  it('never treats a cumulative counter as per-sample production', () => {
    // Two large but increasing counters yield a small delta, not the total.
    expect(shareDelta(18760, 153, 18765, 153).accepted).toBe(5);
  });
});

describe('accumulateShareDeltas', () => {
  const s = (a: number | null, r: number | null): Pick<LabSample, 'sharesAccepted' | 'sharesRejected'> => ({ sharesAccepted: a, sharesRejected: r });

  it('sums per-interval deltas across a window', () => {
    const result = accumulateShareDeltas([s(100, 1), s(110, 1), s(125, 2)]);
    expect(result.accepted).toBe(25);
    expect(result.rejected).toBe(1);
    expect(result.hadReset).toBeFalse();
    expect(result.hasData).toBeTrue();
  });

  it('flags a reset within the window and still returns a sane total', () => {
    const result = accumulateShareDeltas([s(5000, 40), s(10, 0), s(30, 1)]);
    expect(result.hadReset).toBeTrue();
    expect(result.accepted).toBe(10 + 20); // post-reset attribution + subsequent delta
  });

  it('reports no data when counters are unavailable', () => {
    const result = accumulateShareDeltas([s(null, null), s(null, null)]);
    expect(result.hasData).toBeFalse();
  });
});

describe('coverage helpers', () => {
  it('computes the expected sample count at the fixed cadence', () => {
    expect(expectedSampleCount(60000)).toBe(60000 / SAMPLE_INTERVAL_MS);
    expect(expectedSampleCount(0)).toBe(0);
    expect(expectedSampleCount(NaN)).toBe(0);
  });
  it('counts gap samples', () => {
    expect(countGaps([{ gap: true }, { gap: false }, { gap: true }])).toBe(2);
  });
});
