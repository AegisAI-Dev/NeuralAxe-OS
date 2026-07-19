import {
  DURATION_BOUNDS,
  PROFILE_LIMITS,
  TuningConfig,
  StabilityProfile,
  captureBaseline,
  validateProfile,
  outsideOptionList,
  profileSignature,
  isDuplicateProfile,
  profileDiff,
  profileToSettings,
  buildStarterProfiles,
  totalSessionSeconds,
  nextProfileId,
  restartAudit,
  fieldRequiresRestart,
  profileRestartRequired,
  sessionRestartCount,
} from './stability-profile';

const targetBaseline: TuningConfig = {
  frequency: 485,
  coreVoltage: 1200,
  thermalControlMode: 'target',
  temptarget: 60,
  minFanSpeed: 25,
};

const validTargetProfile = (over: Partial<StabilityProfile> = {}): StabilityProfile => ({
  id: 'p1',
  name: 'Test',
  frequency: 485,
  coreVoltage: 1200,
  thermalControlMode: 'target',
  temptarget: 60,
  minFanSpeed: 25,
  warmupSec: 180,
  measureSec: 1200,
  cooldownSec: 0,
  ...over,
});

const curveProfile = (over: Partial<StabilityProfile> = {}): StabilityProfile => ({
  id: 'pc',
  name: 'Curve',
  frequency: 500,
  coreVoltage: 1250,
  thermalControlMode: 'curve',
  minFanSpeed: 25,
  fanCurveHysteresis: 2,
  fanCurve: [
    { tempC: 45, fanPercent: 25 },
    { tempC: 52, fanPercent: 45 },
    { tempC: 58, fanPercent: 70 },
    { tempC: 64, fanPercent: 100 },
  ],
  warmupSec: 180,
  measureSec: 1200,
  cooldownSec: 0,
  ...over,
});

describe('captureBaseline', () => {
  it('captures only tuning/thermal fields from a live info object', () => {
    const config = captureBaseline({
      frequency: 625, coreVoltage: 1150, thermalControlMode: 'curve',
      temptarget: 60, minFanSpeed: 25, fanCurveHysteresis: 2,
      fanCurve: [{ tempC: 45, fanPercent: 25 }, { tempC: 52, fanPercent: 45 }, { tempC: 58, fanPercent: 70 }, { tempC: 64, fanPercent: 100 }],
      // things that MUST NOT be captured:
      stratumURL: 'pool.example', stratumUser: 'wallet.worker', ssid: 'HomeNet', wifiPass: 'secret', ipv4: '10.0.0.5',
    });
    expect(config).not.toBeNull();
    expect(config!.frequency).toBe(625);
    expect(config!.thermalControlMode).toBe('curve');
    expect(config!.fanCurve?.length).toBe(4);
    // No pool/wifi/credentials leak into the config type at all.
    expect(JSON.stringify(config)).not.toContain('pool.example');
    expect(JSON.stringify(config)).not.toContain('HomeNet');
    expect(JSON.stringify(config)).not.toContain('secret');
  });

  it('returns null when core fields are missing or invalid', () => {
    expect(captureBaseline(null)).toBeNull();
    expect(captureBaseline({ coreVoltage: 1200, thermalControlMode: 'target' })).toBeNull();
    expect(captureBaseline({ frequency: 485, coreVoltage: 1200, thermalControlMode: 'bogus' as any })).toBeNull();
    expect(captureBaseline({ frequency: NaN, coreVoltage: 1200, thermalControlMode: 'target' })).toBeNull();
  });

  it('does not fabricate an incomplete fan curve', () => {
    const config = captureBaseline({ frequency: 485, coreVoltage: 1200, thermalControlMode: 'curve', fanCurve: [{ tempC: 45, fanPercent: 25 }] });
    expect(config!.fanCurve).toBeUndefined();
  });
});

describe('validateProfile', () => {
  it('accepts a valid target profile', () => {
    expect(validateProfile(validTargetProfile())).toEqual([]);
  });

  it('accepts a valid curve profile', () => {
    expect(validateProfile(curveProfile())).toEqual([]);
  });

  it('rejects an out-of-range core voltage', () => {
    expect(validateProfile(validTargetProfile({ coreVoltage: 0 }))).toContain('Core voltage must be a whole number 1–65535 mV.');
  });

  it('rejects a target temperature outside firmware bounds', () => {
    const errors = validateProfile(validTargetProfile({ temptarget: 80 }));
    expect(errors.some(e => e.includes('Target temperature'))).toBeTrue();
  });

  it('validates the curve exactly as the firmware would (ascending temps)', () => {
    const bad = curveProfile({ fanCurve: [
      { tempC: 45, fanPercent: 25 }, { tempC: 45, fanPercent: 45 },
      { tempC: 58, fanPercent: 70 }, { tempC: 64, fanPercent: 100 },
    ]});
    expect(validateProfile(bad).some(e => e.toLowerCase().includes('higher than point'))).toBeTrue();
  });

  it('enforces duration bounds', () => {
    expect(validateProfile(validTargetProfile({ warmupSec: 5 })).some(e => e.includes('Warm-up'))).toBeTrue();
    expect(validateProfile(validTargetProfile({ measureSec: 30 })).some(e => e.includes('Measurement'))).toBeTrue();
    expect(validateProfile(validTargetProfile({ cooldownSec: 9999 })).some(e => e.includes('Cooldown'))).toBeTrue();
    // measurement floor is a real minute, not seconds
    expect(DURATION_BOUNDS.measureSec.min).toBe(60);
  });

  it('requires a non-empty, bounded name', () => {
    expect(validateProfile(validTargetProfile({ name: '' })).some(e => e.includes('name'))).toBeTrue();
    expect(validateProfile(validTargetProfile({ name: 'x'.repeat(PROFILE_LIMITS.nameMaxLength + 1) })).length).toBeGreaterThan(0);
  });

  it('requires manual fan only in manual mode', () => {
    const manual = validTargetProfile({ thermalControlMode: 'manual', manualFanSpeed: undefined });
    expect(validateProfile(manual).some(e => e.includes('Manual fan'))).toBeTrue();
    expect(validateProfile(validTargetProfile({ thermalControlMode: 'manual', manualFanSpeed: 60 }))).toEqual([]);
  });
});

describe('outsideOptionList', () => {
  it('flags a pair outside the served options', () => {
    expect(outsideOptionList({ frequency: 999, coreVoltage: 1200 }, [485, 500], [1150, 1200])).toBeTrue();
    expect(outsideOptionList({ frequency: 485, coreVoltage: 1200 }, [485, 500], [1150, 1200])).toBeFalse();
  });
  it('never flags when there is no option truth to compare against', () => {
    expect(outsideOptionList({ frequency: 999, coreVoltage: 9999 }, undefined, undefined)).toBeFalse();
  });
});

describe('duplicate detection', () => {
  it('treats identical effective settings as duplicates regardless of name/durations', () => {
    const a = validTargetProfile({ name: 'A', warmupSec: 60 });
    const b = validTargetProfile({ name: 'B', warmupSec: 900 });
    expect(profileSignature(a)).toBe(profileSignature(b));
    expect(isDuplicateProfile(b, [a])).toBeTrue();
  });

  it('distinguishes profiles that differ on any applied field', () => {
    const a = validTargetProfile();
    const b = validTargetProfile({ coreVoltage: 1250 });
    expect(isDuplicateProfile(b, [a])).toBeFalse();
  });

  it('distinguishes curve profiles with different curves', () => {
    const a = curveProfile();
    const b = curveProfile({ fanCurve: [
      { tempC: 40, fanPercent: 35 }, { tempC: 48, fanPercent: 60 },
      { tempC: 54, fanPercent: 85 }, { tempC: 60, fanPercent: 100 },
    ]});
    expect(isDuplicateProfile(b, [a])).toBeFalse();
  });
});

describe('profileDiff', () => {
  it('lists only genuinely changed fields', () => {
    const rows = profileDiff(targetBaseline, { ...targetBaseline, coreVoltage: 1250 });
    expect(rows.length).toBe(1);
    expect(rows[0].field).toBe('coreVoltage');
    expect(rows[0].current).toBe('1200 mV');
    expect(rows[0].next).toBe('1250 mV');
  });

  it('is empty when nothing changes', () => {
    expect(profileDiff(targetBaseline, { ...targetBaseline })).toEqual([]);
  });

  it('surfaces a mode switch and the mode-relevant fields', () => {
    const rows = profileDiff(targetBaseline, curveProfile());
    const fields = rows.map(r => r.field);
    expect(fields).toContain('thermalControlMode');
    expect(fields).toContain('fanCurve');
    expect(fields).toContain('frequency');
  });
});

describe('profileToSettings', () => {
  it('emits firmware rest-names and derives autofanspeed for target mode', () => {
    const payload = profileToSettings(targetBaseline);
    expect(payload['frequency']).toBe(485);
    expect(payload['coreVoltage']).toBe(1200);
    expect(payload['thermalControlMode']).toBe('target');
    expect(payload['autofanspeed']).toBe(1);
    expect(payload['temptarget']).toBe(60);
    expect(payload['minFanSpeed']).toBe(25);
  });

  it('sets autofanspeed 0 for manual mode and includes manualFanSpeed', () => {
    const payload = profileToSettings({ frequency: 485, coreVoltage: 1200, thermalControlMode: 'manual', manualFanSpeed: 70 });
    expect(payload['autofanspeed']).toBe(0);
    expect(payload['manualFanSpeed']).toBe(70);
    expect(payload['temptarget']).toBeUndefined();
  });

  it('includes a valid curve array in curve mode and never a free-form string', () => {
    const payload = profileToSettings(curveProfile());
    expect(Array.isArray(payload['fanCurve'])).toBeTrue();
    expect(payload['fanCurve'].length).toBe(4);
    expect(typeof payload['fanCurve']).not.toBe('string');
  });

  it('never emits pool/wifi/worker/credential keys', () => {
    const payload = profileToSettings({ ...targetBaseline, ...( { stratumURL: 'x', wifiPass: 'y' } as any) });
    expect(Object.keys(payload).join(',')).not.toMatch(/stratum|wifi|ssid|pass|user|worker|wallet/i);
  });
});

describe('buildStarterProfiles', () => {
  it('always includes Current and derives the rest only from served options', () => {
    const specs = buildStarterProfiles(targetBaseline, [400, 485, 575], [1100, 1200, 1300], 485, 1200);
    expect(specs[0].key).toBe('current');
    const keys = specs.map(s => s.key);
    expect(keys).toContain('conservative');
    expect(keys).toContain('performance');
    // Conservative keeps the SAME thermal setup, only freq/voltage change.
    const conservative = specs.find(s => s.key === 'conservative')!;
    expect(conservative.config.thermalControlMode).toBe('target');
    expect(conservative.config.temptarget).toBe(60);
    expect(conservative.config.frequency).toBe(400);
    expect(conservative.config.coreVoltage).toBe(1100);
  });

  it('returns only Current when option lists are unavailable (no invented tuning)', () => {
    const specs = buildStarterProfiles(targetBaseline, undefined, undefined, undefined, undefined);
    expect(specs.length).toBe(1);
    expect(specs[0].key).toBe('current');
  });

  it('returns nothing without a baseline', () => {
    expect(buildStarterProfiles(null, [400, 500], [1100, 1200], 485, 1200)).toEqual([]);
  });
});

describe('restart audit', () => {
  const target = (over: Partial<TuningConfig> = {}): TuningConfig => ({
    frequency: 485, coreVoltage: 1200, thermalControlMode: 'target', temptarget: 60, minFanSpeed: 25, ...over,
  });
  const curve = (over: Partial<TuningConfig> = {}): TuningConfig => ({
    frequency: 500, coreVoltage: 1250, thermalControlMode: 'curve', minFanSpeed: 25, fanCurveHysteresis: 2,
    fanCurve: [{ tempC: 45, fanPercent: 25 }, { tempC: 52, fanPercent: 45 }, { tempC: 58, fanPercent: 70 }, { tempC: 64, fanPercent: 100 }],
    ...over,
  });

  it('classifies every field a profile emits as live-applied on this firmware', () => {
    const audit = restartAudit();
    // Every emitted field is present and live (frequency..fanCurveHysteresis).
    expect(audit.map(r => r.field).sort()).toEqual([
      'autofanspeed', 'coreVoltage', 'fanCurve', 'fanCurveHysteresis', 'frequency',
      'manualFanSpeed', 'minFanSpeed', 'temptarget', 'thermalControlMode',
    ].sort());
    expect(audit.every(r => r.live)).toBeTrue();
    expect(audit.every(r => !!r.source)).toBeTrue();
  });

  it('fieldRequiresRestart is case-insensitive and flags an unknown/restart-only field', () => {
    expect(fieldRequiresRestart('minFanSpeed')).toBeFalse();
    expect(fieldRequiresRestart('minfanspeed')).toBeFalse(); // firmware cJSON is case-insensitive
    expect(fieldRequiresRestart('fanCurve')).toBeFalse();
    expect(fieldRequiresRestart('ssid')).toBeTrue();         // not a live-applied tuning field
  });

  it('requires no restart for any single supported field change', () => {
    expect(profileRestartRequired(target(), target({ frequency: 575 }))).toBeFalse();
    expect(profileRestartRequired(target(), target({ coreVoltage: 1300 }))).toBeFalse();
    expect(profileRestartRequired(target(), target({ temptarget: 55 }))).toBeFalse();
    expect(profileRestartRequired(target(), target({ minFanSpeed: 30 }))).toBeFalse();
    expect(profileRestartRequired(target(), { frequency: 500, coreVoltage: 1200, thermalControlMode: 'manual', manualFanSpeed: 80 })).toBeFalse();
  });

  it('requires no restart for a mode switch into curve mode, points or hysteresis changes', () => {
    expect(profileRestartRequired(target(), curve())).toBeFalse();
    expect(profileRestartRequired(curve(), curve({ fanCurveHysteresis: 5 }))).toBeFalse();
    const shifted = curve({ fanCurve: [{ tempC: 40, fanPercent: 35 }, { tempC: 48, fanPercent: 60 }, { tempC: 54, fanPercent: 85 }, { tempC: 60, fanPercent: 100 }] });
    expect(profileRestartRequired(curve(), shifted)).toBeFalse();
  });

  it('computes the real session restart count from the actual diffs (0 for supported profiles)', () => {
    const baseline = target();
    const profiles = [target({ frequency: 575, coreVoltage: 1300 }), curve(), target({ frequency: 400 })];
    expect(sessionRestartCount(baseline, profiles)).toBe(0);
    // no profiles → no restarts (no restore either)
    expect(sessionRestartCount(baseline, [])).toBe(0);
  });
});

describe('totalSessionSeconds & nextProfileId', () => {
  it('sums warm-up + measurement + cooldown across the queue', () => {
    expect(totalSessionSeconds([
      { warmupSec: 60, measureSec: 120, cooldownSec: 30 },
      { warmupSec: 60, measureSec: 120, cooldownSec: 0 },
    ])).toBe(390);
  });
  it('produces unique ids', () => {
    expect(nextProfileId()).not.toBe(nextProfileId());
  });
});
