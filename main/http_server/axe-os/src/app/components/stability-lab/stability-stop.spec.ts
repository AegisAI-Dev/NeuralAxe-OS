import {
  STOP_TUNABLES,
  StopThresholds,
  defaultStopThresholds,
  clampThresholds,
  zeroDebounce,
  evaluateSample,
  DebounceState,
} from './stability-stop';
import { LabSample } from './stability-telemetry';

const sample = (over: Partial<LabSample> = {}): LabSample => ({
  tMs: 1000,
  phase: 'measure',
  profileIndex: 0,
  hashRate: 1290,
  expectedHashrate: 1275,
  power: 21,
  efficiency: 16.3,
  asicTemp: 60,
  vrmTemp: 45,
  requestedFan: 50,
  appliedFan: 55,
  rpm: 15230,
  errorPercentage: 0.5,
  sharesAccepted: 1000,
  sharesRejected: 5,
  poolLatency: 30,
  thermalControlMode: 'curve',
  controlTemp: 60,
  emergencyOverride: false,
  sensorValid: true,
  miningPaused: false,
  gap: false,
  ...over,
});

/** Feed a sequence of identical violating samples and return the first stop. */
function feed(samples: LabSample[], thresholds: StopThresholds) {
  let debounce: DebounceState = zeroDebounce();
  for (const s of samples) {
    const evaluation = evaluateSample(s, thresholds, debounce);
    debounce = evaluation.debounce;
    if (evaluation.stop) {
      return evaluation.stop;
    }
  }
  return null;
}

describe('threshold clamping (tighten-only, never beyond safe bounds)', () => {
  it('provides conservative defaults', () => {
    const t = defaultStopThresholds();
    expect(t.asicC).toBeLessThanOrEqual(70);
    expect(t.vrmC).toBeLessThanOrEqual(105);
    expect(t.fanSaturationStop).toBeFalse();
    expect(t.debounceSamples).toBe(STOP_TUNABLES.debounceSamples.default);
  });

  it('clamps an attempt to raise thresholds beyond the safe ceilings', () => {
    const t = clampThresholds({ asicC: 95, vrmC: 130, errorPct: 99, rejectPct: 99, debounceSamples: 100 });
    expect(t.asicC).toBe(70);   // ASIC ceiling
    expect(t.vrmC).toBe(105);   // VRM ceiling
    expect(t.errorPct).toBe(STOP_TUNABLES.errorPct.max);
    expect(t.rejectPct).toBe(STOP_TUNABLES.rejectPct.max);
    expect(t.debounceSamples).toBe(STOP_TUNABLES.debounceSamples.max);
  });

  it('allows the owner to tighten thresholds', () => {
    const t = clampThresholds({ asicC: 60, errorPct: 2 });
    expect(t.asicC).toBe(60);
    expect(t.errorPct).toBe(2);
  });
});

describe('immediate stops', () => {
  const t = defaultStopThresholds();
  it('aborts on emergency override with the first sample', () => {
    const stop = evaluateSample(sample({ emergencyOverride: true }), t, zeroDebounce()).stop;
    expect(stop?.code).toBe('emergency');
    expect(stop?.immediate).toBeTrue();
  });
  it('aborts on an invalid control sensor immediately', () => {
    const stop = evaluateSample(sample({ sensorValid: false }), t, zeroDebounce()).stop;
    expect(stop?.code).toBe('sensor');
    expect(stop?.immediate).toBeTrue();
  });
  it('records the triggering sample time', () => {
    const stop = evaluateSample(sample({ emergencyOverride: true, tMs: 42000 }), t, zeroDebounce()).stop;
    expect(stop?.atMs).toBe(42000);
  });

  it('never fabricates an immediate stop from a telemetry-loss gap sample', () => {
    // A gap has no readings, so sensorValid/emergency default falsey — but a
    // dropout must NOT be treated as "sensor invalid" or "emergency".
    const gap = sample({ gap: true, hashRate: null, asicTemp: null, sensorValid: false, emergencyOverride: false });
    const evaluation = evaluateSample(gap, t, zeroDebounce());
    expect(evaluation.stop).toBeNull();
  });
});

describe('debounced stops', () => {
  const t = defaultStopThresholds();

  it('does not abort on a single transient temperature spike', () => {
    const hot = sample({ asicTemp: t.asicC + 5 });
    let debounce = zeroDebounce();
    const first = evaluateSample(hot, t, debounce);
    expect(first.stop).toBeNull();          // debounce not yet reached
    // A cool sample resets the counter.
    const cool = evaluateSample(sample({ asicTemp: 60 }), t, first.debounce);
    expect(cool.debounce.asic).toBe(0);
  });

  it('aborts once the temperature stays over the limit for the debounce window', () => {
    const hot = sample({ asicTemp: t.asicC + 5 });
    const stop = feed([hot, hot, hot], t); // default debounce = 3
    expect(stop?.code).toBe('asic-temp');
    expect(stop?.immediate).toBeFalse();
  });

  it('aborts on sustained VRM over-temperature', () => {
    const hot = sample({ vrmTemp: t.vrmC + 3 });
    expect(feed([hot, hot, hot], t)?.code).toBe('vrm-temp');
  });

  it('aborts on sustained mining pause', () => {
    const paused = sample({ miningPaused: true });
    expect(feed([paused, paused, paused], t)?.code).toBe('mining-stopped');
  });

  it('only stops on fan saturation when it is configured as a stop', () => {
    const saturated = sample({ appliedFan: 100 });
    expect(feed([saturated, saturated, saturated, saturated], t)).toBeNull(); // default off
    const withFan = clampThresholds({ ...t, fanSaturationStop: true });
    expect(feed([saturated, saturated, saturated], withFan)?.code).toBe('fan-saturation');
  });

  it('applies the share-confidence gate before a reject-rate stop', () => {
    // High reject rate but a tiny sample → not confident → never stops.
    const tiny = sample({ sharesAccepted: 20, sharesRejected: 2 }); // 9% but only 2 rejects, 22 total
    expect(feed([tiny, tiny, tiny, tiny], t)).toBeNull();
    // Confident sample above the threshold → stops after debounce.
    const confident = sample({ sharesAccepted: 900, sharesRejected: 100 }); // 10% of 1000
    expect(feed([confident, confident, confident], t)?.code).toBe('reject-rate');
  });

  it('aborts on sustained excessive ASIC error rate', () => {
    const noisy = sample({ errorPercentage: t.errorPct + 3 });
    expect(feed([noisy, noisy, noisy], t)?.code).toBe('error-rate');
  });

  it('skips gap samples without triggering or resetting debounce', () => {
    const hot = sample({ asicTemp: t.asicC + 5 });
    const gap = sample({ gap: true, asicTemp: null, hashRate: null });
    let debounce = zeroDebounce();
    debounce = evaluateSample(hot, t, debounce).debounce;   // asic=1
    debounce = evaluateSample(gap, t, debounce).debounce;   // skipped, still 1
    expect(debounce.asic).toBe(1);
    debounce = evaluateSample(hot, t, debounce).debounce;   // asic=2
    const final = evaluateSample(hot, t, debounce);         // asic=3 → stop
    expect(final.stop?.code).toBe('asic-temp');
  });
});
