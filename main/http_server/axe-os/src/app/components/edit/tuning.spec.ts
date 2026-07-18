import { FormControl } from '@angular/forms';
import {
  TUNING_BOUNDS,
  activeTuningPreset,
  buildTuningPresets,
  firmwareRangeValidator,
  pendingChanges,
  FAN_CURVE_BOUNDS,
  FanCurvePoint,
  THERMAL_PROFILES,
  activeThermalProfile,
  buildSettingsPayload,
  curveFromFormValue,
  curvePreviewModel,
  curveSegmentLabel,
  fanCurveSummary,
  thermalModeLabel,
  validateFanCurve,
} from './tuning';

// Board-601 (Gamma / BM1370) values as served by GET /api/system/asic.
const FREQS = [400, 425, 450, 475, 485, 500, 525, 550, 575];
const VOLTS = [1100, 1150, 1200, 1250, 1300];

describe('tuning', () => {
  describe('buildTuningPresets', () => {
    it('builds Eco/Balanced/Performance strictly from existing option values', () => {
      const presets = buildTuningPresets(FREQS, VOLTS, 485, 1150);
      expect(presets.map(p => p.id)).toEqual(['eco', 'balanced', 'performance']);
      expect(presets[0]).toEqual({ id: 'eco', label: 'Eco', frequency: 400, coreVoltage: 1100 });
      expect(presets[1]).toEqual({ id: 'balanced', label: 'Balanced', frequency: 485, coreVoltage: 1150 });
      expect(presets[2]).toEqual({ id: 'performance', label: 'Performance', frequency: 575, coreVoltage: 1300 });
      // Every preset value must exist in the firmware-provided option lists.
      for (const preset of presets) {
        expect(FREQS).toContain(preset.frequency);
        expect(VOLTS).toContain(preset.coreVoltage);
      }
    });

    it('supports the 1150 mV genuine board default and other valid defaults', () => {
      expect(buildTuningPresets(FREQS, VOLTS, 485, 1150).find(p => p.id === 'balanced')?.coreVoltage).toBe(1150);
      expect(buildTuningPresets(FREQS, VOLTS, 525, 1200).find(p => p.id === 'balanced')?.coreVoltage).toBe(1200);
    });

    it('omits Balanced when the defaults are not in the option lists (nothing invented)', () => {
      const presets = buildTuningPresets(FREQS, VOLTS, 999, 1150);
      expect(presets.map(p => p.id)).toEqual(['eco', 'performance']);
    });

    it('returns no presets when the option lists are missing or too small', () => {
      expect(buildTuningPresets([], VOLTS, 485, 1150)).toEqual([]);
      expect(buildTuningPresets(undefined, VOLTS, 485, 1150)).toEqual([]);
      expect(buildTuningPresets([485], VOLTS, 485, 1150)).toEqual([]);
      expect(buildTuningPresets(FREQS, [1150], 485, 1150)).toEqual([]);
    });

    it('collapses duplicate value pairs', () => {
      const presets = buildTuningPresets([400, 575], [1100, 1300], 400, 1100);
      // Balanced would equal Eco — only distinct pairs remain.
      expect(presets.map(p => p.id)).toEqual(['eco', 'performance']);
    });

    it('ignores invalid option values', () => {
      const presets = buildTuningPresets([NaN, 400, -5, 575] as number[], VOLTS, 485, 1150);
      expect(presets.find(p => p.id === 'eco')?.frequency).toBe(400);
      expect(presets.find(p => p.id === 'performance')?.frequency).toBe(575);
    });
  });

  describe('activeTuningPreset', () => {
    const presets = buildTuningPresets(FREQS, VOLTS, 485, 1150);

    it('identifies the matching preset', () => {
      expect(activeTuningPreset(400, 1100, presets)).toBe('eco');
      expect(activeTuningPreset(485, 1150, presets)).toBe('balanced');
      expect(activeTuningPreset(575, 1300, presets)).toBe('performance');
    });

    it('selects Custom whenever the pair diverges from every preset', () => {
      expect(activeTuningPreset(485, 1200, presets)).toBe('custom');
      expect(activeTuningPreset(625, 1150, presets)).toBe('custom');
      expect(activeTuningPreset(undefined, undefined, presets)).toBe('custom');
    });
  });

  describe('firmwareRangeValidator', () => {
    function errorsFor(value: unknown, min: number, max: number, integer = false) {
      return firmwareRangeValidator(min, max, integer)(new FormControl(value as any));
    }

    it('accepts valid firmware-range values, including 1150 mV and 625 MHz', () => {
      expect(errorsFor(1150, TUNING_BOUNDS.coreVoltage.min, TUNING_BOUNDS.coreVoltage.max, true)).toBeNull();
      expect(errorsFor(625, TUNING_BOUNDS.frequency.min, TUNING_BOUNDS.frequency.max)).toBeNull();
      expect(errorsFor(55, TUNING_BOUNDS.temptarget.min, TUNING_BOUNDS.temptarget.max, true)).toBeNull();
      expect(errorsFor(25, TUNING_BOUNDS.minFanSpeed.min, TUNING_BOUNDS.minFanSpeed.max, true)).toBeNull();
    });

    it('blocks NaN / null / undefined / non-finite submissions', () => {
      expect(errorsFor(null, 1, 100)).toEqual({ required: true });
      expect(errorsFor(undefined, 1, 100)).toEqual({ required: true });
      expect(errorsFor('', 1, 100)).toEqual({ required: true });
      expect(errorsFor(NaN, 1, 100)).toEqual({ nonFinite: true });
      expect(errorsFor(Infinity, 1, 100)).toEqual({ nonFinite: true });
      expect(errorsFor('abc', 1, 100)).toEqual({ nonFinite: true });
    });

    it('blocks out-of-range values exactly at the firmware bounds', () => {
      expect(errorsFor(0, TUNING_BOUNDS.coreVoltage.min, TUNING_BOUNDS.coreVoltage.max, true)).toEqual(
        { range: { min: 1, max: 65535, actual: 0 } });
      expect(errorsFor(34, TUNING_BOUNDS.temptarget.min, TUNING_BOUNDS.temptarget.max, true)).toEqual(
        { range: { min: 35, max: 66, actual: 34 } });
      expect(errorsFor(67, TUNING_BOUNDS.temptarget.min, TUNING_BOUNDS.temptarget.max, true)).toEqual(
        { range: { min: 35, max: 66, actual: 67 } });
      expect(errorsFor(100, TUNING_BOUNDS.minFanSpeed.min, TUNING_BOUNDS.minFanSpeed.max, true)?.['range']).toBeTruthy();
    });

    it('enforces integers where the firmware type is integral', () => {
      expect(errorsFor(1150.5, 1, 65535, true)).toEqual({ integer: true });
      expect(errorsFor(487.5, 1, 65535, false)).toBeNull(); // frequency is TYPE_FLOAT
    });
  });

  describe('pendingChanges', () => {
    const NO_RESTART = ['frequency', 'coreVoltage', 'autofanspeed', 'temptarget', 'manualFanSpeed', 'overheat_mode', 'statsFrequency', 'displayTimeout'];

    it('reports changed fields with current and pending values and units', () => {
      const changes = pendingChanges(
        { frequency: 625, coreVoltage: 1150, temptarget: 55 },
        { frequency: 485, coreVoltage: 1150, temptarget: 60 },
        NO_RESTART,
      );
      expect(changes.length).toBe(2);
      expect(changes[0]).toEqual({
        field: 'frequency', label: 'Frequency',
        current: '625 MHz', pending: '485 MHz', restartRequired: false,
      });
      expect(changes[1].current).toBe('55 °C');
      expect(changes[1].pending).toBe('60 °C');
    });

    it('marks fields outside the no-restart list as restart required', () => {
      const changes = pendingChanges(
        { display: 'NONE', minfanspeed: 25 },
        { display: 'SSD1306 (128x32)', minfanspeed: 30 },
        NO_RESTART,
      );
      expect(changes.find(c => c.field === 'display')?.restartRequired).toBeTrue();
      expect(changes.find(c => c.field === 'minfanspeed')?.restartRequired).toBeTrue();
    });

    it('formats booleans as On/Off and handles absent values', () => {
      const changes = pendingChanges(
        { autofanspeed: true, invertscreen: undefined },
        { autofanspeed: false, invertscreen: true },
        NO_RESTART,
      );
      expect(changes.find(c => c.field === 'autofanspeed')?.current).toBe('On');
      expect(changes.find(c => c.field === 'autofanspeed')?.pending).toBe('Off');
      expect(changes.find(c => c.field === 'invertscreen')?.current).toBe('—');
    });

    it('is empty when nothing changed', () => {
      expect(pendingChanges({ frequency: 625 }, { frequency: 625 }, NO_RESTART)).toEqual([]);
    });
  });
});

// ---------------------------------------------------------------------------
// Phase 2H — thermal control model (mirrors the firmware contract)
// ---------------------------------------------------------------------------

describe('tuning thermal control (Phase 2H)', () => {
  const validCurve: FanCurvePoint[] = [
    { tempC: 45, fanPercent: 25 },
    { tempC: 52, fanPercent: 45 },
    { tempC: 58, fanPercent: 70 },
    { tempC: 64, fanPercent: 100 },
  ];

  describe('validateFanCurve', () => {
    it('accepts the firmware default curve', () => {
      expect(validateFanCurve(validCurve)).toEqual([]);
    });

    it('rejects wrong point counts', () => {
      expect(validateFanCurve(validCurve.slice(0, 3)).length).toBe(1);
      expect(validateFanCurve([...validCurve, { tempC: 66, fanPercent: 100 }]).length).toBe(1);
      expect(validateFanCurve([]).length).toBe(1);
      expect(validateFanCurve(null).length).toBe(1);
      expect(validateFanCurve(undefined).length).toBe(1);
    });

    it('rejects duplicate and descending temperatures', () => {
      const dup = validCurve.map(p => ({ ...p }));
      dup[1].tempC = 45;
      expect(validateFanCurve(dup).some(e => e.includes('higher than'))).toBeTrue();

      const desc = validCurve.map(p => ({ ...p }));
      desc[2].tempC = 40;
      expect(validateFanCurve(desc).length).toBeGreaterThan(0);
    });

    it('rejects descending fan percentages', () => {
      const bad = validCurve.map(p => ({ ...p }));
      bad[3].fanPercent = 10;
      expect(validateFanCurve(bad).some(e => e.includes('may not decrease'))).toBeTrue();
    });

    it('rejects out-of-range and non-integer and non-finite values', () => {
      const hot = validCurve.map(p => ({ ...p }));
      hot[3].tempC = FAN_CURVE_BOUNDS.tempC.max + 1;
      expect(validateFanCurve(hot).length).toBeGreaterThan(0);

      const cold = validCurve.map(p => ({ ...p }));
      cold[0].tempC = FAN_CURVE_BOUNDS.tempC.min - 1;
      expect(validateFanCurve(cold).length).toBeGreaterThan(0);

      const overFan = validCurve.map(p => ({ ...p }));
      overFan[3].fanPercent = 101;
      expect(validateFanCurve(overFan).length).toBeGreaterThan(0);

      const fractional = validCurve.map(p => ({ ...p }));
      fractional[1].tempC = 52.5;
      expect(validateFanCurve(fractional).some(e => e.includes('whole number'))).toBeTrue();

      const nan = validCurve.map(p => ({ ...p }));
      nan[1].fanPercent = NaN;
      expect(validateFanCurve(nan).length).toBeGreaterThan(0);
    });
  });

  describe('thermal profiles (pending templates only)', () => {
    it('every profile is valid under the firmware contract', () => {
      for (const profile of THERMAL_PROFILES) {
        expect(validateFanCurve(profile.points))
          .withContext(`profile ${profile.id}`)
          .toEqual([]);
      }
    });

    it('every profile still reaches 100% fan at or before 64 °C (no silence-over-protection)', () => {
      for (const profile of THERMAL_PROFILES) {
        const final = profile.points[profile.points.length - 1];
        expect(final.fanPercent).withContext(`profile ${profile.id}`).toBe(100);
        expect(final.tempC).withContext(`profile ${profile.id}`).toBeLessThanOrEqual(64);
      }
    });

    it('Balanced matches the firmware default curve exactly', () => {
      expect(THERMAL_PROFILES.find(p => p.id === 'balanced')?.points).toEqual(validCurve);
    });

    it('activeThermalProfile identifies matches and falls back to custom on any divergence', () => {
      expect(activeThermalProfile(validCurve)).toBe('balanced');
      const custom = validCurve.map(p => ({ ...p }));
      custom[2].fanPercent = 71;
      expect(activeThermalProfile(custom)).toBe('custom');
    });
  });

  describe('curvePreviewModel', () => {
    it('maps curve points into the viewbox with the firmware plateaus', () => {
      const model = curvePreviewModel(validCurve, 280, 120, 8);
      expect(model.markers.length).toBe(4);
      // strictly increasing x, strictly decreasing y (fan rises)
      for (let i = 1; i < model.markers.length; i++) {
        expect(model.markers[i].x).toBeGreaterThan(model.markers[i - 1].x);
        expect(model.markers[i].y).toBeLessThan(model.markers[i - 1].y);
      }
      // polyline includes the plateau endpoints at 20 °C and 70 °C
      const xs = model.polyline.split(' ').map(pair => Number(pair.split(',')[0]));
      expect(xs[0]).toBe(8);
      expect(xs[xs.length - 1]).toBe(272);
    });

    it('renders nothing for an invalid curve instead of a misleading shape', () => {
      const bad = validCurve.map(p => ({ ...p }));
      bad[1].tempC = 45;
      const model = curvePreviewModel(bad);
      expect(model.polyline).toBe('');
      expect(model.markers).toEqual([]);
    });

    it('places the live-temperature marker only inside the drawable range', () => {
      const model = curvePreviewModel(validCurve);
      expect(model.tempX(45)).not.toBeNull();
      expect(model.tempX(10)).toBeNull();
      expect(model.tempX(80)).toBeNull();
      expect(model.tempX(null)).toBeNull();
      expect(model.tempX(NaN)).toBeNull();
    });
  });

  describe('buildSettingsPayload', () => {
    const rawCurveControls = {
      fanCurveTemp0: 45, fanCurveFan0: 25,
      fanCurveTemp1: 52, fanCurveFan1: 45,
      fanCurveTemp2: 58, fanCurveFan2: 70,
      fanCurveTemp3: 64, fanCurveFan3: 100,
    };

    it('sends a structured fanCurve array and syncs autofanspeed in curve mode', () => {
      const payload = buildSettingsPayload({
        thermalControlMode: 'curve', fanCurveHysteresis: 2, temptarget: 60, ...rawCurveControls,
      });
      expect(payload['fanCurve']).toEqual(validCurve);
      expect(payload['autofanspeed']).toBeTrue();
      expect(payload['thermalControlMode']).toBe('curve');
      expect(payload['fanCurveHysteresis']).toBe(2);
      // internal editor controls never leak into the API payload
      expect(Object.keys(payload).some(k => k.startsWith('fanCurveTemp') || k.startsWith('fanCurveFan'))).toBeFalse();
    });

    it('never sends an invalid curve', () => {
      const payload = buildSettingsPayload({
        thermalControlMode: 'curve', ...rawCurveControls, fanCurveTemp1: 45,
      });
      expect(payload['fanCurve']).toBeUndefined();
    });

    it('maps target and manual modes onto the legacy autofanspeed flag', () => {
      expect(buildSettingsPayload({ thermalControlMode: 'target', ...rawCurveControls })['autofanspeed']).toBeTrue();
      expect(buildSettingsPayload({ thermalControlMode: 'manual', ...rawCurveControls })['autofanspeed']).toBeFalse();
      // no fanCurve outside curve mode
      expect(buildSettingsPayload({ thermalControlMode: 'target', ...rawCurveControls })['fanCurve']).toBeUndefined();
    });

    it('leaves unrelated fields untouched and does not add a mode when none is set', () => {
      const payload = buildSettingsPayload({ frequency: 485, coreVoltage: 1150 });
      expect(payload).toEqual({ frequency: 485, coreVoltage: 1150 });
    });

    it('curveFromFormValue reads the flat controls in order', () => {
      expect(curveFromFormValue(rawCurveControls as any)).toEqual(validCurve);
    });
  });

  describe('labels', () => {
    it('thermalModeLabel names the three modes and dashes the unknown', () => {
      expect(thermalModeLabel('target')).toBe('Target Temperature');
      expect(thermalModeLabel('curve')).toBe('Fan Curve');
      expect(thermalModeLabel('manual')).toBe('Manual Fan');
      expect(thermalModeLabel('bogus')).toBe('—');
      expect(thermalModeLabel(undefined)).toBe('—');
    });

    it('curveSegmentLabel maps the firmware segment semantics', () => {
      expect(curveSegmentLabel(-1)).toBe('—');
      expect(curveSegmentLabel(0)).toBe('Below curve');
      expect(curveSegmentLabel(1)).toBe('P1 → P2');
      expect(curveSegmentLabel(3)).toBe('P3 → P4');
      expect(curveSegmentLabel(4)).toBe('Above final point');
      expect(curveSegmentLabel('x')).toBe('—');
      expect(curveSegmentLabel(2.5)).toBe('—');
    });

    it('fanCurveSummary renders a compact readable line', () => {
      expect(fanCurveSummary(validCurve)).toBe('45°→25% · 52°→45% · 58°→70% · 64°→100%');
      expect(fanCurveSummary([])).toBe('—');
      expect(fanCurveSummary(undefined)).toBe('—');
    });
  });
});
