import { FormControl } from '@angular/forms';
import {
  TUNING_BOUNDS,
  activeTuningPreset,
  buildTuningPresets,
  firmwareRangeValidator,
  pendingChanges,
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
