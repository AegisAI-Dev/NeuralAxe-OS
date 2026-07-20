import {
  computeCoverage, coverageTarget, sampleTargetText, coveragePctText,
} from './stability-coverage';

/** Evenly spaced sample times over a window, inclusive of both boundaries. */
function evenTimes(count: number, windowMs: number): number[] {
  if (count <= 1) return [0];
  const step = windowMs / (count - 1);
  return Array.from({ length: count }, (_, i) => Math.round(i * step));
}

describe('stability-coverage — sample-target semantics', () => {
  it('derives the target from window and cadence (600s / 5s = 120)', () => {
    expect(coverageTarget(600_000, 5000)).toBe(120);
    expect(coverageTarget(1_200_000, 5000)).toBe(240);
  });

  it('Fixture A — 10 valid samples against target 120 is 8.3% coverage', () => {
    const c = computeCoverage({ sampleTimesMs: evenTimes(10, 600_000), windowMs: 600_000, cadenceMs: 5000 });
    expect(c.validSamples).toBe(10);
    expect(c.expectedTarget).toBe(120);
    expect(Math.round(c.coveragePct * 10) / 10).toBe(8.3);
    expect(coveragePctText(c.coveragePct)).toBe('8.3%');
    expect(c.missingSamples).toBe(110);
    expect(c.totalGapMs).toBe(550_000); // 600000 − 10*5000
  });

  it('Fixture B — 121 valid samples against target 120 caps at 100% and keeps all samples', () => {
    const times = evenTimes(121, 600_000);
    const c = computeCoverage({ sampleTimesMs: times, windowMs: 600_000, cadenceMs: 5000 });
    expect(c.validSamples).toBe(121); // never trimmed to 120
    expect(c.expectedTarget).toBe(120);
    expect(c.coveragePct).toBe(100);   // capped, not 100.83
    expect(coveragePctText(c.coveragePct)).toBe('100%');
    expect(c.missingSamples).toBe(0);
    expect(c.totalGapMs).toBe(0);
    expect(sampleTargetText(c.validSamples, c.expectedTarget)).toBe('121 valid samples · target 120');
  });

  it('119 / 120 / 121 boundary counts are all reported honestly', () => {
    for (const n of [119, 120, 121]) {
      const c = computeCoverage({ sampleTimesMs: evenTimes(n, 600_000), windowMs: 600_000, cadenceMs: 5000 });
      expect(c.validSamples).toBe(n);
      expect(c.expectedTarget).toBe(120);
      expect(c.coveragePct).toBeLessThanOrEqual(100);
    }
    expect(computeCoverage({ sampleTimesMs: evenTimes(119, 600_000), windowMs: 600_000 }).coveragePct)
      .toBeCloseTo((119 / 120) * 100, 5);
  });

  it('a sample exactly at measurement start and one exactly at the end both count', () => {
    const c = computeCoverage({ sampleTimesMs: [0, 300_000, 600_000], windowMs: 600_000, cadenceMs: 5000 });
    expect(c.validSamples).toBe(3);
    expect(c.maxGapMs).toBe(300_000);
  });

  it('tolerates jitter around the 5s cadence without dropping samples', () => {
    const jittered = [0, 4800, 10200, 14900, 20100];
    const c = computeCoverage({ sampleTimesMs: jittered, windowMs: 25_000, cadenceMs: 5000 });
    expect(c.validSamples).toBe(5);
    expect(c.medianIntervalMs).not.toBeNull();
    expect(c.medianIntervalMs!).toBeGreaterThan(4000);
    expect(c.medianIntervalMs!).toBeLessThan(6000);
  });

  it('deduplicated / identical timestamps still count each provided sample', () => {
    const c = computeCoverage({ sampleTimesMs: [1000, 1000, 2000], windowMs: 60_000, cadenceMs: 5000 });
    expect(c.validSamples).toBe(3);
    // intervals [0, 1000] → median 500 (each provided sample is retained).
    expect(c.medianIntervalMs).toBe(500);
  });

  it('handles out-of-order emissions by sorting before measuring intervals', () => {
    const c = computeCoverage({ sampleTimesMs: [10_000, 0, 5000], windowMs: 60_000, cadenceMs: 5000 });
    expect(c.validSamples).toBe(3);
    expect(c.medianIntervalMs).toBe(5000);
  });

  it('reports a long gap as the max gap', () => {
    const c = computeCoverage({ sampleTimesMs: [0, 5000, 400_000], windowMs: 600_000, cadenceMs: 5000 });
    expect(c.maxGapMs).toBe(395_000);
  });

  it('never divides by zero or emits NaN/Infinity for a zero window', () => {
    const c = computeCoverage({ sampleTimesMs: [], windowMs: 0, cadenceMs: 5000 });
    expect(c.expectedTarget).toBe(0);
    expect(c.coveragePct).toBe(0);
    expect(c.totalGapMs).toBe(0);
    expect(Number.isFinite(c.coveragePct)).toBeTrue();
  });

  it('an empty window of valid samples is entirely one gap', () => {
    const c = computeCoverage({ sampleTimesMs: [], windowMs: 600_000, cadenceMs: 5000 });
    expect(c.validSamples).toBe(0);
    expect(c.coveragePct).toBe(0);
    expect(c.maxGapMs).toBe(600_000);
    expect(c.medianIntervalMs).toBeNull();
  });

  it('coveragePctText caps and formats', () => {
    expect(coveragePctText(8.333)).toBe('8.3%');
    expect(coveragePctText(100)).toBe('100%');
    expect(coveragePctText(150)).toBe('100%');
    expect(coveragePctText(null)).toBe('—');
  });
});
