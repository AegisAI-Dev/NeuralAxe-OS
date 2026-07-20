import {
  initialVisibility, applyVisibility, visibilityStats, visibilitySummary,
} from './stability-visibility';

describe('stability-visibility — Page Visibility contract', () => {
  it('a visible-only session reports zero interruptions and zero hidden time', () => {
    let acc = initialVisibility(false, 0);
    acc = applyVisibility(acc, false, 5000, 'measure'); // still visible
    const s = visibilityStats(acc, 10000);
    expect(s.interruptions).toBe(0);
    expect(s.totalHiddenMs).toBe(0);
    expect(s.currentlyHidden).toBeFalse();
    expect(s.hiddenDuringMeasure).toBeFalse();
    expect(visibilitySummary(s)).toContain('stayed visible');
  });

  it('records a hide→show as one interruption with the hidden duration', () => {
    let acc = initialVisibility(false, 0);
    acc = applyVisibility(acc, true, 10000, 'measure');  // hidden at t=10s
    acc = applyVisibility(acc, false, 25000, 'measure'); // shown at t=25s
    const s = visibilityStats(acc, 30000);
    expect(s.interruptions).toBe(1);
    expect(s.totalHiddenMs).toBe(15000);
    expect(s.longestHiddenMs).toBe(15000);
    expect(s.currentlyHidden).toBeFalse();
    expect(s.hiddenDuringMeasure).toBeTrue();
  });

  it('includes an in-progress hidden segment in the running stats', () => {
    let acc = initialVisibility(false, 0);
    acc = applyVisibility(acc, true, 10000, 'measure'); // hidden, not yet shown
    const s = visibilityStats(acc, 40000);              // 30 s later, still hidden
    expect(s.currentlyHidden).toBeTrue();
    expect(s.totalHiddenMs).toBe(30000);
    expect(s.longestHiddenMs).toBe(30000);
  });

  it('tracks the longest hidden segment across multiple interruptions', () => {
    let acc = initialVisibility(false, 0);
    acc = applyVisibility(acc, true, 1000, 'measure');
    acc = applyVisibility(acc, false, 4000, 'measure');   // 3 s
    acc = applyVisibility(acc, true, 10000, 'measure');
    acc = applyVisibility(acc, false, 30000, 'measure');  // 20 s
    acc = applyVisibility(acc, true, 40000, 'measure');
    acc = applyVisibility(acc, false, 45000, 'measure');  // 5 s
    const s = visibilityStats(acc, 46000);
    expect(s.interruptions).toBe(3);
    expect(s.totalHiddenMs).toBe(28000);
    expect(s.longestHiddenMs).toBe(20000);
  });

  it('does not inflate the interruption count on repeated same-state calls', () => {
    let acc = initialVisibility(false, 0);
    acc = applyVisibility(acc, true, 5000, 'measure');
    acc = applyVisibility(acc, true, 6000, 'measure'); // still hidden — supervise tick
    acc = applyVisibility(acc, true, 7000, 'measure'); // still hidden — supervise tick
    const s = visibilityStats(acc, 8000);
    expect(s.interruptions).toBe(1);
  });

  it('marks warm-up separately from measurement when each is affected', () => {
    let warm = initialVisibility(false, 0);
    warm = applyVisibility(warm, true, 1000, 'warmup');
    warm = applyVisibility(warm, false, 2000, 'warmup');
    const ws = visibilityStats(warm, 3000);
    expect(ws.hiddenDuringWarmup).toBeTrue();
    expect(ws.hiddenDuringMeasure).toBeFalse();
  });

  it('counts a session that begins hidden as an interruption in progress', () => {
    const acc = initialVisibility(true, 0);
    const s = visibilityStats(acc, 5000);
    expect(s.currentlyHidden).toBeTrue();
    expect(s.interruptions).toBe(1);
    expect(s.totalHiddenMs).toBe(5000);
  });

  it('never produces a negative hidden duration on a backwards clock', () => {
    let acc = initialVisibility(false, 100000);
    acc = applyVisibility(acc, true, 100000, 'measure');
    acc = applyVisibility(acc, false, 99000, 'measure'); // clock went backwards
    const s = visibilityStats(acc, 99000);
    expect(s.totalHiddenMs).toBe(0);
    expect(s.longestHiddenMs).toBe(0);
  });
});
