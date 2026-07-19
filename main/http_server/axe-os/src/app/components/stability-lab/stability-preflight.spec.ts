import {
  SUPPORTED_TARGET,
  STOP_THRESHOLD_BOUNDS,
  supportedDevice,
  preflight,
  clampAsicStop,
  clampVrmStop,
  PreflightInput,
  DeviceTelemetry,
} from './stability-preflight';
import { PairStatus, derivePairStatus } from '../../services/version-state';

const supportedIdentity = {
  productName: 'NeuralAxe OS',
  vendor: 'NeuralShield',
  targetDevice: 'Gamma',
  targetBoard: '601',
  targetAsic: 'BM1370',
  ASICModel: 'BM1370',
};

const okPair: PairStatus = derivePairStatus('v2.14.2-31-g1c411d52', 'v2.14.2-31-g1c411d52', 'v2.14.2-31-g1c411d52');

const healthyTelemetry = (over: Partial<DeviceTelemetry> = {}): DeviceTelemetry => ({
  ...supportedIdentity,
  temp: 60,
  vrTemp: 45,
  effectiveControlTemperature: 60,
  controlSensorValid: 1,
  emergencyOverrideActive: 0,
  overheat_mode: 0,
  miningPaused: false,
  ...over,
});

const baseInput = (over: Partial<PreflightInput> = {}): PreflightInput => ({
  info: healthyTelemetry(),
  online: true,
  pairStatus: okPair,
  profilesQueued: 2,
  baselineCaptured: true,
  otherSessionActive: false,
  stopAsicC: STOP_THRESHOLD_BOUNDS.asicC.default,
  stopVrmC: STOP_THRESHOLD_BOUNDS.vrmC.default,
  ...over,
});

describe('supportedDevice', () => {
  it('accepts a NeuralAxe-Managed Gamma / 601 / BM1370', () => {
    const check = supportedDevice(supportedIdentity);
    expect(check.supported).toBeTrue();
    expect(check.managed).toBeTrue();
    expect(check.kind).toBe('neuralaxe-supported');
    expect(check.reasons).toEqual([]);
    expect(SUPPORTED_TARGET.board).toBe('601');
  });

  it('classifies a stock AxeOS device as read-only, not supported', () => {
    const check = supportedDevice({ ASICModel: 'BM1370', boardVersion: '601' }); // no productName
    expect(check.supported).toBeFalse();
    expect(check.managed).toBeFalse();
    expect(check.kind).toBe('stock-axeos');
    expect(check.reasons[0]).toContain('stock AxeOS');
  });

  it('blocks a managed but unsupported board 702', () => {
    const check = supportedDevice({ ...supportedIdentity, targetBoard: '702' });
    expect(check.supported).toBeFalse();
    expect(check.kind).toBe('neuralaxe-unsupported');
    expect(check.reasons.some(r => r.includes('702'))).toBeTrue();
    expect(check.label).toContain('702');
  });

  it('blocks an unsupported ASIC (BM1368)', () => {
    const check = supportedDevice({ ...supportedIdentity, targetAsic: 'BM1368', ASICModel: 'BM1368' });
    expect(check.supported).toBeFalse();
    expect(check.reasons.some(r => r.includes('BM1368'))).toBeTrue();
  });

  it('uses the declared build target, not the runtime PCB revision (602 board still supported)', () => {
    // Real Gamma 601 target reports physical boardVersion "602" — must NOT block.
    const check = supportedDevice({ ...supportedIdentity, boardVersion: '602' });
    expect(check.supported).toBeTrue();
  });

  it('does not confirm support when the build target is undeclared', () => {
    const check = supportedDevice({ productName: 'NeuralAxe OS', ASICModel: 'BM1370' });
    expect(check.supported).toBeFalse();
    expect(check.reasons.some(r => r.toLowerCase().includes('not fully declared'))).toBeTrue();
  });

  it('returns unknown for no telemetry', () => {
    expect(supportedDevice(null).kind).toBe('unknown');
  });
});

describe('clamp stop thresholds', () => {
  it('clamps ASIC threshold into safe bounds and never above 70', () => {
    expect(clampAsicStop(90)).toBe(STOP_THRESHOLD_BOUNDS.asicC.max);
    expect(clampAsicStop(70)).toBe(70);
    expect(clampAsicStop(40)).toBe(STOP_THRESHOLD_BOUNDS.asicC.min);
    expect(clampAsicStop('x')).toBe(STOP_THRESHOLD_BOUNDS.asicC.default);
  });
  it('clamps VRM threshold into safe bounds and never above 105', () => {
    expect(clampVrmStop(130)).toBe(STOP_THRESHOLD_BOUNDS.vrmC.max);
    expect(clampVrmStop(60)).toBe(STOP_THRESHOLD_BOUNDS.vrmC.min);
  });
});

describe('preflight', () => {
  it('clears every check for a healthy supported device with a queued profile', () => {
    const result = preflight(baseInput());
    expect(result.canStart).toBeTrue();
    expect(result.blockers).toEqual([]);
    expect(result.checks.find(c => c.id === 'supported')!.ok).toBeTrue();
  });

  it('blocks and explains an emergency override immediately', () => {
    const result = preflight(baseInput({ info: healthyTelemetry({ emergencyOverrideActive: 1 }) }));
    expect(result.canStart).toBeFalse();
    const blocker = result.blockers.find(b => b.id === 'emergency');
    expect(blocker).toBeTruthy();
    expect(blocker!.detail).toContain('Emergency');
  });

  it('blocks an invalid control sensor', () => {
    const result = preflight(baseInput({ info: healthyTelemetry({ controlSensorValid: 0 }) }));
    expect(result.blockers.some(b => b.id === 'sensor')).toBeTrue();
  });

  it('blocks when the ASIC temperature is already above the stop limit', () => {
    const result = preflight(baseInput({ info: healthyTelemetry({ temp: 69 }), stopAsicC: 68 }));
    const blocker = result.blockers.find(b => b.id === 'temps');
    expect(blocker).toBeTruthy();
    expect(blocker!.detail).toContain('stop limit');
  });

  it('blocks when the ASIC temperature is unavailable', () => {
    const result = preflight(baseInput({ info: healthyTelemetry({ temp: -1 }) }));
    expect(result.blockers.some(b => b.id === 'temps')).toBeTrue();
  });

  it('tolerates a board that does not report VRM temperature', () => {
    const result = preflight(baseInput({ info: healthyTelemetry({ vrTemp: -1 }) }));
    expect(result.checks.find(c => c.id === 'temps')!.ok).toBeTrue();
  });

  it('blocks paused mining', () => {
    const result = preflight(baseInput({ info: healthyTelemetry({ miningPaused: true }) }));
    expect(result.blockers.some(b => b.id === 'mining')).toBeTrue();
  });

  it('blocks an offline device', () => {
    const result = preflight(baseInput({ online: false }));
    expect(result.blockers.some(b => b.id === 'online')).toBeTrue();
  });

  it('blocks a live-verified pair mismatch', () => {
    const mismatch = derivePairStatus('v2.14.2-31', 'v2.14.2-31', 'v2.14.2-30');
    const result = preflight(baseInput({ pairStatus: mismatch }));
    expect(result.blockers.some(b => b.id === 'pair')).toBeTrue();
  });

  it('accepts a boot-verified pair match when live verification is unavailable', () => {
    const bootMatch = derivePairStatus('v2.14.2-31', 'v2.14.2-31', null);
    const result = preflight(baseInput({ pairStatus: bootMatch }));
    expect(result.checks.find(c => c.id === 'pair')!.ok).toBeTrue();
  });

  it('blocks without a queued profile or captured baseline', () => {
    expect(preflight(baseInput({ profilesQueued: 0 })).blockers.some(b => b.id === 'profiles')).toBeTrue();
    expect(preflight(baseInput({ baselineCaptured: false })).blockers.some(b => b.id === 'baseline')).toBeTrue();
  });

  it('blocks when another session is running', () => {
    expect(preflight(baseInput({ otherSessionActive: true })).blockers.some(b => b.id === 'no-session')).toBeTrue();
  });

  it('marks device checks inapplicable (not failing) on an unsupported device', () => {
    const result = preflight(baseInput({ info: { productName: 'NeuralAxe OS', targetDevice: 'Gamma', targetBoard: '702', targetAsic: 'BM1370', ASICModel: 'BM1370' } as any }));
    expect(result.canStart).toBeFalse();
    // The 'online'/'temps' checks are marked inapplicable, so the only real
    // blocker surfaced is the supported-device gate.
    expect(result.blockers.map(b => b.id)).toContain('supported');
    expect(result.checks.find(c => c.id === 'temps')!.applicable).toBeFalse();
  });
});
