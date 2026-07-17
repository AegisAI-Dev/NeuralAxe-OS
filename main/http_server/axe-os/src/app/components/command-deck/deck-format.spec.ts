import { fmtNum, fmtInt, fmtLatency, fmtPct, fmtBytes, fmtTemp, fmtVolts, INVALID } from './deck-format';

describe('deck-format (real-device formatting battery)', () => {

  describe('fmtLatency', () => {
    it('rounds real-device fractional latency to integer ms', () => {
      expect(fmtLatency(106.8960037)).toBe('107 ms'); // observed on the pilot Gamma
      expect(fmtLatency(10)).toBe('10 ms');
      expect(fmtLatency(1.4)).toBe('1 ms');
    });

    it('allows one decimal below 1 ms', () => {
      expect(fmtLatency(0.5)).toBe('0.5 ms');
      expect(fmtLatency(0.04)).toBe('0.0 ms');
    });

    it('falls back neutrally for invalid values', () => {
      expect(fmtLatency(null)).toBe(INVALID);
      expect(fmtLatency(undefined)).toBe(INVALID);
      expect(fmtLatency(-5)).toBe(INVALID);
      expect(fmtLatency(NaN)).toBe(INVALID);
      expect(fmtLatency(Infinity)).toBe(INVALID);
      expect(fmtLatency('10' as any)).toBe(INVALID);
    });

    it('handles zero as a valid reading', () => {
      expect(fmtLatency(0)).toBe('0 ms');
    });
  });

  describe('fmtNum / fmtInt / fmtPct', () => {
    it('applies controlled precision', () => {
      expect(fmtNum(11.670000076293945, 1, ' W')).toBe('11.7 W');
      expect(fmtNum(24.5678, 1, ' J/TH')).toBe('24.6 J/TH');
      expect(fmtPct(0.19999, 2)).toBe('0.20 %');
    });

    it('groups large integers', () => {
      expect(fmtInt(842763)).toBe('842,763');
      expect(fmtInt(18760)).toBe('18,760');
      expect(fmtInt(7190, ' RPM')).toBe('7,190 RPM');
    });

    it('never renders NaN, Infinity, null or undefined', () => {
      for (const bad of [NaN, Infinity, -Infinity, null, undefined, 'x' as any, {} as any]) {
        expect(fmtNum(bad)).toBe(INVALID);
        expect(fmtInt(bad)).toBe(INVALID);
        expect(fmtPct(bad)).toBe(INVALID);
        expect(fmtTemp(bad)).toBe(INVALID);
        expect(fmtBytes(bad)).toBe(INVALID);
      }
    });
  });

  describe('fmtBytes / fmtTemp', () => {
    it('formats memory clearly with units', () => {
      expect(fmtBytes(200504)).toContain('kB');
      expect(fmtBytes(-1)).toBe(INVALID);
    });

    it('rounds temperatures to whole degrees', () => {
      expect(fmtTemp(59.4)).toBe('59°C');
      expect(fmtTemp(53.51)).toBe('54°C');
    });
  });

  describe('fmtVolts (measured telemetry, mV in → V out)', () => {
    it('renders the pilot-device measured voltage correctly', () => {
      expect(fmtVolts(1140)).toBe('1.14 V');   // observed ~1.14 V on the Gamma 601
      expect(fmtVolts(1150)).toBe('1.15 V');
      expect(fmtVolts(1100)).toBe('1.10 V');
    });

    it('never renders invalid telemetry', () => {
      expect(fmtVolts(NaN)).toBe(INVALID);
      expect(fmtVolts(undefined)).toBe(INVALID);
      expect(fmtVolts(null)).toBe(INVALID);
      expect(fmtVolts(Infinity)).toBe(INVALID);
      expect(fmtVolts(-5)).toBe(INVALID);
    });
  });
});
