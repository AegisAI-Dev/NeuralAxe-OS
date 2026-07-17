import {
  asicTempSeverity,
  fanSaturationSeverity,
  infoSeverity,
  inputVoltageSeverity,
  meterClass,
  powerSeverity,
  thermalDeltaSeverity,
  vrTempSeverity,
} from './semantic-status';

describe('semantic-status', () => {
  describe('meterClass', () => {
    it('maps severities to nx-meter classes', () => {
      expect(meterClass('ok')).toBe('nx-meter-ok');
      expect(meterClass('info')).toBe('nx-meter-info');
      expect(meterClass('warn')).toBe('nx-meter-warn');
      expect(meterClass('danger')).toBe('nx-meter-danger');
      expect(meterClass('neutral')).toBe('nx-meter-neutral');
    });
  });

  describe('asicTempSeverity', () => {
    it('is ok in the healthy range', () => {
      expect(asicTempSeverity(55)).toBe('ok');
      expect(asicTempSeverity(64.9)).toBe('ok');
    });
    it('warns from 65 °C and turns danger at the existing 70 °C line', () => {
      expect(asicTempSeverity(65)).toBe('warn');
      expect(asicTempSeverity(69.9)).toBe('warn');
      expect(asicTempSeverity(70)).toBe('danger');
      expect(asicTempSeverity(90)).toBe('danger');
    });
    it('is neutral for invalid or unread values', () => {
      expect(asicTempSeverity(undefined)).toBe('neutral');
      expect(asicTempSeverity(null)).toBe('neutral');
      expect(asicTempSeverity(NaN)).toBe('neutral');
      expect(asicTempSeverity(0)).toBe('neutral');
      expect(asicTempSeverity(-1)).toBe('neutral');
    });
  });

  describe('vrTempSeverity', () => {
    it('matches the existing 85 caution / 105 danger thresholds', () => {
      expect(vrTempSeverity(45)).toBe('ok');
      expect(vrTempSeverity(84.9)).toBe('ok');
      expect(vrTempSeverity(85)).toBe('warn');
      expect(vrTempSeverity(104.9)).toBe('warn');
      expect(vrTempSeverity(105)).toBe('danger');
    });
    it('is neutral for missing sensors', () => {
      expect(vrTempSeverity(0)).toBe('neutral');
      expect(vrTempSeverity(undefined)).toBe('neutral');
    });
  });

  describe('powerSeverity', () => {
    it('is ok below 90% of the rated maximum', () => {
      expect(powerSeverity(11.7, 25)).toBe('ok');
      expect(powerSeverity(22.4, 25)).toBe('ok');
    });
    it('warns from 90% and is danger above the rated maximum', () => {
      expect(powerSeverity(22.5, 25)).toBe('warn');
      expect(powerSeverity(25, 25)).toBe('warn');
      expect(powerSeverity(25.1, 25)).toBe('danger');
    });
    it('is neutral without a valid rating', () => {
      expect(powerSeverity(10, 0)).toBe('neutral');
      expect(powerSeverity(10, undefined)).toBe('neutral');
      expect(powerSeverity(NaN, 25)).toBe('neutral');
    });
  });

  describe('inputVoltageSeverity', () => {
    // Mirrors the legacy dashboard rule: danger when v < (nominal + 0.5) * 0.87.
    it('is ok at nominal supply', () => {
      expect(inputVoltageSeverity(5.2, 5)).toBe('ok');
    });
    it('warns just above the danger line and is danger below it', () => {
      expect(inputVoltageSeverity(5.5 * 0.88, 5)).toBe('warn');
      expect(inputVoltageSeverity(5.5 * 0.86, 5)).toBe('danger');
    });
    it('is neutral for invalid readings', () => {
      expect(inputVoltageSeverity(undefined, 5)).toBe('neutral');
      expect(inputVoltageSeverity(5.2, 0)).toBe('neutral');
    });
  });

  describe('infoSeverity', () => {
    it('classifies presence as neutral technical info', () => {
      expect(infoSeverity(485)).toBe('info');
      expect(infoSeverity(0)).toBe('info');
      expect(infoSeverity(undefined)).toBe('neutral');
      expect(infoSeverity(NaN)).toBe('neutral');
    });
  });

  describe('thermalDeltaSeverity', () => {
    it('is ok at or below the target (with 2 °C tolerance)', () => {
      expect(thermalDeltaSeverity(53, 55)).toBe('ok');
      expect(thermalDeltaSeverity(55, 55)).toBe('ok');
      expect(thermalDeltaSeverity(57, 55)).toBe('ok');
    });
    it('warns above target and turns danger at the fixed 70 °C line', () => {
      expect(thermalDeltaSeverity(58, 55)).toBe('warn');
      expect(thermalDeltaSeverity(70, 55)).toBe('danger');
      expect(thermalDeltaSeverity(70, undefined)).toBe('danger');
    });
    it('is neutral without valid readings', () => {
      expect(thermalDeltaSeverity(undefined, 55)).toBe('neutral');
      expect(thermalDeltaSeverity(55, undefined)).toBe('neutral');
      expect(thermalDeltaSeverity(0, 55)).toBe('neutral');
    });
  });

  describe('fanSaturationSeverity', () => {
    it('is ok while automatic fan control has headroom', () => {
      expect(fanSaturationSeverity(true, 40)).toBe('ok');
      expect(fanSaturationSeverity(true, 94)).toBe('ok');
    });
    it('warns when the automatic fan is pinned near 100%', () => {
      expect(fanSaturationSeverity(true, 95)).toBe('warn');
      expect(fanSaturationSeverity(true, 100)).toBe('warn');
    });
    it('is informational under manual fan control and neutral without data', () => {
      expect(fanSaturationSeverity(false, 100)).toBe('info');
      expect(fanSaturationSeverity(true, undefined)).toBe('neutral');
    });
  });
});
