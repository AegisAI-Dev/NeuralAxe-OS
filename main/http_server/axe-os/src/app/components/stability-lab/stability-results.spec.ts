import {
  MIN_VALID_SAMPLES,
  ProfileRun,
  computeProfileResult,
  applyComparativeBadges,
  completionStatement,
  isPromotable,
  ProfileResult,
} from './stability-results';
import { LabSample, SAMPLE_INTERVAL_MS } from './stability-telemetry';

let clock = 0;
function s(over: Partial<LabSample> = {}): LabSample {
  clock += SAMPLE_INTERVAL_MS;
  return {
    tMs: clock, phase: 'measure', profileIndex: 0,
    hashRate: 1290, expectedHashrate: 1275, power: 21, efficiency: 16.3,
    asicTemp: 60, vrmTemp: 45, requestedFan: 50, appliedFan: 55, rpm: 15230,
    errorPercentage: 0.5, sharesAccepted: 1000, sharesRejected: 5, poolLatency: 30,
    thermalControlMode: 'curve', controlTemp: 60, emergencyOverride: false,
    sensorValid: true, miningPaused: false, gap: false, ...over,
  };
}

function makeRun(over: Partial<ProfileRun> = {}): ProfileRun {
  clock = 0;
  const measure = [s({ hashRate: 1200 }), s({ hashRate: 1300 }), s({ hashRate: 1250 }), s({ hashRate: 1290 })];
  return {
    profileId: 'p1', profileName: 'Balanced', thermalControlMode: 'curve',
    warmupSamples: [s({ phase: 'warmup', hashRate: 800 })],
    measureSamples: measure,
    requestedMeasureMs: 4 * SAMPLE_INTERVAL_MS,
    measuredMeasureMs: 4 * SAMPLE_INTERVAL_MS,
    restartOccurred: false, countersReset: false,
    ranFullWindow: true, aborted: false, failed: false,
    ...over,
  };
}

describe('computeProfileResult (aggregates)', () => {
  it('computes averages, median, variability from measurement samples only', () => {
    const result = computeProfileResult(makeRun());
    expect(result.status).toBe('completed');
    expect(result.avgHashrate).toBeCloseTo((1200 + 1300 + 1250 + 1290) / 4, 3);
    expect(result.medianHashrate).toBeCloseTo((1290 + 1250) / 2, 3); // sorted median of 4
    expect(result.hashrateVariabilityPct).toBeGreaterThan(0);
    // warm-up (800) must not drag the average down.
    expect(result.avgHashrate!).toBeGreaterThan(1200);
  });

  it('derives efficiency from aggregate power and hashrate', () => {
    const result = computeProfileResult(makeRun({
      measureSamples: [s({ hashRate: 1000, power: 20 }), s({ hashRate: 1000, power: 20 }), s({ hashRate: 1000, power: 20 })],
    }));
    expect(result.avgEfficiency).toBeCloseTo(20 / 1.0, 3); // 20 W / 1 TH
  });

  it('reports peak and average temperatures', () => {
    const result = computeProfileResult(makeRun({
      measureSamples: [s({ asicTemp: 58 }), s({ asicTemp: 66 }), s({ asicTemp: 61 })],
    }));
    expect(result.peakAsicTemp).toBe(66);
    expect(result.avgAsicTemp).toBeCloseTo((58 + 66 + 61) / 3, 3);
  });

  it('counts valid samples, gaps and coverage', () => {
    const result = computeProfileResult(makeRun({
      measureSamples: [s(), s({ gap: true, hashRate: null, asicTemp: null }), s(), s()],
    }));
    expect(result.validSamples).toBe(3);
    expect(result.missingSamples).toBe(1);
    expect(result.coveragePct).not.toBeNull();
  });
});

describe('status resolution (partial vs completed vs aborted)', () => {
  it('marks a run that did not finish the window as Partial, never Completed', () => {
    const result = computeProfileResult(makeRun({ ranFullWindow: false }));
    expect(result.status).toBe('partial');
    expect(result.badges.some(b => b.kind === 'partial')).toBeTrue();
  });

  it('marks an aborted run and records the reason (never Completed)', () => {
    const result = computeProfileResult(makeRun({ aborted: true, abortReason: 'ASIC over 68 °C' }));
    expect(result.status).toBe('aborted');
    expect(result.badges[0].label).toContain('Aborted:');
    expect(result.abortReason).toContain('68');
  });

  it('marks a run with too few valid samples as Insufficient', () => {
    const few = [s(), s({ gap: true, hashRate: null, asicTemp: null })];
    expect(few.filter(x => !x.gap).length).toBeLessThan(MIN_VALID_SAMPLES);
    const result = computeProfileResult(makeRun({ measureSamples: few }));
    expect(result.status).toBe('insufficient');
  });

  it('flags a share-counter reset and a restart', () => {
    const result = computeProfileResult(makeRun({ restartOccurred: true, countersReset: true }));
    expect(result.badges.some(b => b.kind === 'counter-reset')).toBeTrue();
    expect(result.badges.some(b => b.kind === 'restarted')).toBeTrue();
  });
});

describe('reject-rate confidence in results', () => {
  it('withholds a reject rate on a small share sample', () => {
    const result = computeProfileResult(makeRun({
      measureSamples: [s({ sharesAccepted: 20, sharesRejected: 0 }), s({ sharesAccepted: 21, sharesRejected: 1 }), s({ sharesAccepted: 22, sharesRejected: 1 })],
    }));
    expect(result.rejectRatePct).toBeNull();
  });
});

describe('completionStatement', () => {
  it('qualifies a completed run and never claims permanent stability', () => {
    const statement = completionStatement(computeProfileResult(makeRun()));
    expect(statement).toContain('one session');
    expect(statement).toContain('No configured stop condition triggered');
    expect(statement.toLowerCase()).not.toContain('guaranteed stable');
  });
  it('states the abort reason for an aborted run', () => {
    const statement = completionStatement(computeProfileResult(makeRun({ aborted: true, abortReason: 'sensor invalid' })));
    expect(statement).toContain('sensor invalid');
  });
});

describe('applyComparativeBadges (transparent, no opaque score)', () => {
  it('awards named comparative badges across completed results', () => {
    const fast = computeProfileResult(makeRun({ profileId: 'a', profileName: 'Fast', measureSamples: [s({ hashRate: 1400, power: 25 }), s({ hashRate: 1400, power: 25 }), s({ hashRate: 1400, power: 25 })] }));
    const eff = computeProfileResult(makeRun({ profileId: 'b', profileName: 'Eco', measureSamples: [s({ hashRate: 1000, power: 15, asicTemp: 55 }), s({ hashRate: 1000, power: 15, asicTemp: 55 }), s({ hashRate: 1000, power: 15, asicTemp: 55 })] }));
    const results: ProfileResult[] = applyComparativeBadges([fast, eff]);
    expect(results[0].badges.some(b => b.kind === 'highest-hashrate')).toBeTrue();
    expect(results[1].badges.some(b => b.kind === 'lowest-efficiency')).toBeTrue();
    expect(results[1].badges.some(b => b.kind === 'lowest-temp')).toBeTrue();
  });

  it('does not award comparative badges to aborted results', () => {
    const good = computeProfileResult(makeRun({ profileId: 'a', profileName: 'Good' }));
    const bad = computeProfileResult(makeRun({ profileId: 'b', profileName: 'Bad', aborted: true, abortReason: 'x' }));
    applyComparativeBadges([good, bad]);
    expect(bad.badges.some(b => b.kind.startsWith('highest') || b.kind.startsWith('lowest'))).toBeFalse();
  });
});

describe('isPromotable', () => {
  it('only completed runs are promotable (aborted/partial/failed are not)', () => {
    expect(isPromotable(computeProfileResult(makeRun()))).toBeTrue();
    expect(isPromotable(computeProfileResult(makeRun({ aborted: true, abortReason: 'x' })))).toBeFalse();
    expect(isPromotable(computeProfileResult(makeRun({ ranFullWindow: false })))).toBeFalse();
  });
});
