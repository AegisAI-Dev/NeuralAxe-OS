import {
  bestDiffPctOfNetwork,
  compactNumber,
  currentVsAveragePct,
  currentVsExpectedPct,
  formatExpectedTime,
  formatProbabilityPct,
  recentVariabilityPct,
  rejectRatePct,
  sharesPerHour,
  modeAwareThermalStatus,
  soloOdds,
  thermalControlInsight,
  thermalHeadroom,
} from './deck-intel';

describe('deck-intel', () => {
  describe('sharesPerHour', () => {
    it('derives shares/hour from uptime', () => {
      expect(sharesPerHour(120, 3600)).toBe(120);
      expect(sharesPerHour(30, 1800)).toBe(60);
    });
    it('returns null for missing data or a warm-up period', () => {
      expect(sharesPerHour(5, 30)).toBeNull();      // < 60 s uptime
      expect(sharesPerHour(undefined, 3600)).toBeNull();
      expect(sharesPerHour(5, 0)).toBeNull();
      expect(sharesPerHour(-1, 3600)).toBeNull();
    });
  });

  describe('rejectRatePct', () => {
    it('computes the rejected percentage of all shares', () => {
      expect(rejectRatePct(90, 10)).toBe(10);
      expect(rejectRatePct(100, 0)).toBe(0);
    });
    it('is null before any share and for invalid input', () => {
      expect(rejectRatePct(0, 0)).toBeNull();
      expect(rejectRatePct(NaN, 5)).toBeNull();
    });
  });

  describe('hashrate quality', () => {
    it('compares current hashrate against expected and 1h average', () => {
      expect(currentVsExpectedPct(475, 500)).toBe(95);
      expect(currentVsAveragePct(510, 500)).toBe(102);
    });
    it('guards zero/invalid references', () => {
      expect(currentVsExpectedPct(475, 0)).toBeNull();
      expect(currentVsExpectedPct(undefined, 500)).toBeNull();
      expect(currentVsAveragePct(475, NaN)).toBeNull();
    });

    it('computes the coefficient of variation for a steady vs. noisy series', () => {
      const steady = recentVariabilityPct([500, 501, 499, 500, 500])!;
      const noisy = recentVariabilityPct([400, 600, 380, 620, 500])!;
      expect(steady).toBeLessThan(0.2);
      expect(noisy).toBeGreaterThan(steady);
    });
    it('requires at least five valid samples and a positive mean', () => {
      expect(recentVariabilityPct([500, 500, 500])).toBeNull();
      expect(recentVariabilityPct(null)).toBeNull();
      expect(recentVariabilityPct([0, 0, 0, 0, 0])).toBeNull();
      expect(recentVariabilityPct([500, NaN, 500, 500, 500])).toBeNull(); // NaN dropped → 4 samples
    });
  });

  describe('soloOdds', () => {
    // Reference case: difficulty 1e14, hashrate 1000 GH/s (1 TH/s).
    // expectedHashes = 1e14 × 2^32 ≈ 4.295e23
    // expectedSeconds = 4.295e23 / 1e12 ≈ 4.295e11 s
    it('applies the documented formulas exactly', () => {
      const odds = soloOdds(1e14, 1000)!;
      expect(odds.expectedHashesPerBlock).toBe(1e14 * 2 ** 32);
      expect(odds.expectedSecondsToBlock).toBeCloseTo((1e14 * 2 ** 32) / 1e12, 0);
      const expectedP = 1 - Math.exp(-86400 / odds.expectedSecondsToBlock);
      expect(odds.dailyBlockProbability).toBe(expectedP);
      expect(odds.oneInNDays).toBeCloseTo(odds.expectedSecondsToBlock / 86400, 6);
    });

    it('for very long expectations, daily probability ≈ 86400/expectedSeconds', () => {
      const odds = soloOdds(155.97e12, 1000)!; // realistic network difficulty, 1 TH/s
      const approx = 86400 / odds.expectedSecondsToBlock;
      expect(odds.dailyBlockProbability).toBeCloseTo(approx, 12);
      expect(odds.dailyBlockProbability).toBeLessThan(1e-5);
    });

    it('handles zero and invalid values safely', () => {
      expect(soloOdds(0, 1000)).toBeNull();
      expect(soloOdds(1e14, 0)).toBeNull();
      expect(soloOdds(NaN, 1000)).toBeNull();
      expect(soloOdds(1e14, undefined)).toBeNull();
      expect(soloOdds(-5, 1000)).toBeNull();
    });

    it('probability stays within (0, 1] for extreme hashrates', () => {
      const tiny = soloOdds(155.97e12, 0.001)!;
      const huge = soloOdds(1, 1e12)!;
      expect(tiny.dailyBlockProbability).toBeGreaterThan(0);
      expect(tiny.dailyBlockProbability).toBeLessThan(1);
      expect(huge.dailyBlockProbability).toBeGreaterThan(0.99);
      expect(huge.dailyBlockProbability).toBeLessThanOrEqual(1);
    });
  });

  describe('bestDiffPctOfNetwork', () => {
    it('reports best difficulty as a share of network difficulty', () => {
      expect(bestDiffPctOfNetwork(1.5597e12, 155.97e12)).toBeCloseTo(1, 6);
      expect(bestDiffPctOfNetwork(0, 155.97e12)).toBe(0);
    });
    it('is null without a valid network difficulty', () => {
      expect(bestDiffPctOfNetwork(100, 0)).toBeNull();
      expect(bestDiffPctOfNetwork(undefined, 155.97e12)).toBeNull();
    });
  });

  describe('formatting', () => {
    it('formats expected times in sensible units', () => {
      expect(formatExpectedTime(4.295e11)).toContain('years');   // ≈ 13.6k years
      expect(formatExpectedTime(86400 * 10)).toBe('~10 days');
      expect(formatExpectedTime(7200)).toBe('~2 hours');
      expect(formatExpectedTime(600)).toBe('~10 minutes');
      expect(formatExpectedTime(0)).toBe('—');
      expect(formatExpectedTime(null)).toBe('—');
    });

    it('formats tiny probabilities in scientific notation instead of a misleading 0.00 %', () => {
      expect(formatProbabilityPct(1.9e-6)).toBe('1.9e-4 %');
      expect(formatProbabilityPct(0.5)).toBe('50.00 %');
      expect(formatProbabilityPct(0.0001)).toBe('0.01 %');
      expect(formatProbabilityPct(0)).toBe('0 %');
      expect(formatProbabilityPct(null)).toBe('—');
      expect(formatProbabilityPct(NaN)).toBe('—');
    });

    it('compacts huge counts', () => {
      expect(compactNumber(13600)).toBe('14k');
      expect(compactNumber(2.5e6)).toBe('2.5M');
      expect(compactNumber(3.2e9)).toBe('3.2B');
      expect(compactNumber(999)).toBe('999');
    });
  });

  describe('thermalHeadroom', () => {
    it('classifies below / at / above target with a ±2 °C band', () => {
      expect(thermalHeadroom(50, 55, true, 40).state).toBe('below-target');
      expect(thermalHeadroom(55, 55, true, 40).state).toBe('at-target');
      expect(thermalHeadroom(56.5, 55, true, 40).state).toBe('at-target');
      expect(thermalHeadroom(58, 55, true, 40).state).toBe('above-target');
    });

    it('reports the fixed 70 °C line as overheat regardless of target', () => {
      expect(thermalHeadroom(71, 66, true, 100).state).toBe('overheat');
      expect(thermalHeadroom(71, undefined, true, 100).state).toBe('overheat');
    });

    it('flags automatic-fan saturation transparently', () => {
      expect(thermalHeadroom(58, 55, true, 96).fanSaturated).toBeTrue();
      expect(thermalHeadroom(58, 55, true, 96).label).toContain('saturation');
      expect(thermalHeadroom(58, 55, false, 100).fanSaturated).toBeFalse();
      expect(thermalHeadroom(55, 55, true, 40).label).toContain('stable');
    });

    it('handles missing readings without inventing state', () => {
      expect(thermalHeadroom(undefined, 55, true, 40).state).toBe('unknown');
      expect(thermalHeadroom(0, 55, true, 40).state).toBe('unknown');
      expect(thermalHeadroom(55, undefined, true, 40).state).toBe('unknown');
      expect(thermalHeadroom(55, 55, true, undefined).fanSaturated).toBeFalse();
    });

    it('reports the signed delta from target', () => {
      expect(thermalHeadroom(58, 55, true, 40).deltaC).toBe(3);
      expect(thermalHeadroom(50, 55, true, 40).deltaC).toBe(-5);
    });
  });

  describe('thermalControlInsight (Phase 2H fan-decision transparency)', () => {
    const curveInfo = {
      thermalControlMode: 'curve', thermalControlReason: 'curve active',
      emergencyOverrideActive: 0, controlSensorValid: 1, hysteresisHolding: 0,
      requestedFanPercent: 62, appliedFanPercent: 62,
    };

    it('reserves the error severity for the genuine emergency override', () => {
      const insight = thermalControlInsight({ ...curveInfo, emergencyOverrideActive: 1 });
      expect(insight.severity).toBe('error');
      expect(insight.label).toBe('Emergency thermal override');

      // Everything below emergency must never be red.
      expect(thermalControlInsight(curveInfo).severity).not.toBe('error');
      expect(thermalControlInsight({ ...curveInfo, fanCurveError: 'malformed' }).severity).not.toBe('error');
      expect(thermalControlInsight({ ...curveInfo, controlSensorValid: 0 }).severity).not.toBe('error');
    });

    it('warns on an invalid stored curve and names the safe fallback', () => {
      const insight = thermalControlInsight({ ...curveInfo, fanCurveError: 'temperatures not ascending' });
      expect(insight.severity).toBe('warn');
      expect(insight.label).toBe('Curve configuration invalid — safe fallback active');
      expect(insight.detail).toContain('temperatures not ascending');
      expect(insight.detail).toContain('target control');
    });

    it('reports waiting for valid sensor data before telemetry exists', () => {
      const insight = thermalControlInsight({ ...curveInfo, controlSensorValid: 0 });
      expect(insight.severity).toBe('info');
      expect(insight.label).toBe('Waiting for valid sensor data');
    });

    it('describes stable curve control including the hysteresis hold', () => {
      expect(thermalControlInsight(curveInfo).label).toBe('Curve control stable');
      expect(thermalControlInsight(curveInfo).detail).toContain('62 %');

      const holding = thermalControlInsight({
        ...curveInfo, hysteresisHolding: 1, requestedFanPercent: 55, appliedFanPercent: 70,
      });
      expect(holding.label).toBe('Curve control stable');
      expect(holding.detail).toContain('70 %');
      expect(holding.detail).toContain('55 %');
    });

    it('labels target and manual modes and keeps manual honest about protection', () => {
      expect(thermalControlInsight({ ...curveInfo, thermalControlMode: 'target' }).label).toBe('Target control active');
      const manual = thermalControlInsight({ ...curveInfo, thermalControlMode: 'manual' });
      expect(manual.label).toBe('Manual fan active');
      expect(manual.severity).toBe('info');
      expect(manual.detail.toLowerCase()).toContain('thermal protection still overrides');
    });

    it('degrades gracefully when the device reports no mode', () => {
      const insight = thermalControlInsight({ controlSensorValid: 1 });
      expect(insight.severity).toBe('info');
      expect(insight.label).toBe('Thermal control state unknown');
    });
  });

  describe('modeAwareThermalStatus (2H.1 pilot closure)', () => {
    const base = {
      controlSensorValid: 1, emergencyOverrideActive: 0, hysteresisHolding: 0,
    };

    it('target mode: above target is a warning derived from the configured target', () => {
      const status = modeAwareThermalStatus({
        ...base, thermalControlMode: 'target', temp: 63, temptarget: 60, fanspeed: 80, appliedFanPercent: 80,
      });
      expect(status.severity).toBe('warn');
      expect(status.label).toBe('Above target');
      expect(status.detail).toContain('+3.0 °C vs target');
    });

    it('target mode: at target reads as PID tracking', () => {
      const status = modeAwareThermalStatus({
        ...base, thermalControlMode: 'target', temp: 60, temptarget: 60, fanspeed: 55, appliedFanPercent: 55,
      });
      expect(status.severity).toBe('ok');
      expect(status.label).toBe('At target — PID tracking');
    });

    it('target mode: below target stays ok', () => {
      const status = modeAwareThermalStatus({
        ...base, thermalControlMode: 'target', temp: 50, temptarget: 60, fanspeed: 30, appliedFanPercent: 30,
      });
      expect(status.severity).toBe('ok');
      expect(status.label).toBe('Below target');
    });

    it('curve mode: normal operation never mentions the target temperature', () => {
      const status = modeAwareThermalStatus({
        ...base, thermalControlMode: 'curve', temp: 57, temptarget: 60,
        effectiveControlTemperature: 57, requestedFanPercent: 65, appliedFanPercent: 65,
        activeCurveSegment: 2,
      });
      expect(status.severity).toBe('ok');
      expect(status.label).toBe('Curve control stable');
      expect(status.detail).toContain('57 °C');
      expect(status.detail).toContain('P2 → P3');
      expect((status.label + ' ' + status.detail).toLowerCase()).not.toContain('target');
    });

    it('curve mode: the pilot-observed hysteresis hold reads as normal operation', () => {
      // Real Gamma observation: 57-58 °C, requested 68 %, applied 70 %, P2→P3.
      const status = modeAwareThermalStatus({
        ...base, thermalControlMode: 'curve', temp: 57, temptarget: 60,
        effectiveControlTemperature: 57.5, requestedFanPercent: 68, appliedFanPercent: 70,
        activeCurveSegment: 2, hysteresisHolding: 1,
      });
      expect(status.severity).toBe('ok');
      expect(status.label).toBe('Curve control active — hysteresis hold');
      expect(status.detail).toContain('70 %');
      expect(status.detail).toContain('68 %');
      expect(status.detail).toContain('P2 → P3');
    });

    it('curve mode: fan near saturation is amber only when genuinely near saturation', () => {
      const saturated = modeAwareThermalStatus({
        ...base, thermalControlMode: 'curve', temp: 63,
        effectiveControlTemperature: 63, requestedFanPercent: 96, appliedFanPercent: 96, activeCurveSegment: 3,
      });
      expect(saturated.severity).toBe('warn');
      expect(saturated.label).toBe('Fan near saturation');

      const notSaturated = modeAwareThermalStatus({
        ...base, thermalControlMode: 'curve', temp: 60,
        effectiveControlTemperature: 60, requestedFanPercent: 80, appliedFanPercent: 80, activeCurveSegment: 3,
      });
      expect(notSaturated.severity).toBe('ok');
    });

    it('curve mode: invalid stored curve reports the safe fallback as a warning', () => {
      const status = modeAwareThermalStatus({
        ...base, thermalControlMode: 'curve', temp: 55,
        fanCurveError: 'temperatures not ascending',
        effectiveControlTemperature: 55, appliedFanPercent: 50,
      });
      expect(status.severity).toBe('warn');
      expect(status.label).toBe('Curve invalid — safe fallback active');
    });

    it('manual mode: normal operation is informational and mentions active protection', () => {
      const status = modeAwareThermalStatus({
        ...base, thermalControlMode: 'manual', temp: 55, manualFanSpeed: 70, appliedFanPercent: 70,
      });
      expect(status.severity).toBe('info');
      expect(status.label).toBe('Manual fan active');
      expect(status.detail).toContain('70 %');
      expect(status.detail?.toLowerCase()).toContain('thermal protection remains active');
    });

    it('manual mode: high temperature warns before the overheat line', () => {
      const status = modeAwareThermalStatus({
        ...base, thermalControlMode: 'manual', temp: 66, manualFanSpeed: 40, appliedFanPercent: 40,
      });
      expect(status.severity).toBe('warn');
      expect(status.label).toBe('High temperature — manual fan');
    });

    it('emergency override is red in every mode', () => {
      for (const mode of ['target', 'curve', 'manual']) {
        const status = modeAwareThermalStatus({
          ...base, thermalControlMode: mode, emergencyOverrideActive: 1, temp: 76, appliedFanPercent: 100,
        });
        expect(status.severity).withContext(mode).toBe('error');
        expect(status.label).toBe('Emergency thermal override');
      }
    });

    it('the fixed overheat line stays red regardless of mode', () => {
      const status = modeAwareThermalStatus({
        ...base, thermalControlMode: 'curve', temp: 71,
        effectiveControlTemperature: 71, appliedFanPercent: 100, activeCurveSegment: 4,
      });
      expect(status.severity).toBe('error');
      expect(status.label).toBe('Above safe temperature');
    });

    it('degraded sensor state is amber, not red', () => {
      const status = modeAwareThermalStatus({
        ...base, thermalControlMode: 'curve', controlSensorValid: 0,
      });
      expect(status.severity).toBe('warn');
      expect(status.label).toBe('Waiting for valid sensor data');
    });
  });
});
