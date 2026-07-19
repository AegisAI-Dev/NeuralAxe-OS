/**
 * NeuralAxe Stability Lab — supported-device gate and preflight (Phase 2K).
 *
 * Two deterministic, pure gates:
 *  1. supportedDevice() — is this a NeuralAxe-Managed Gamma / board 601 / BM1370?
 *     Only such a device may EXECUTE a session; anything else gets a read-only
 *     explanation. The gate uses the NeuralAxe DECLARED build target
 *     (targetBoard / targetDevice / targetAsic) — the authoritative statement of
 *     what this firmware build supports — not the runtime boardVersion string,
 *     which reports the physical PCB revision (e.g. "602" on a Gamma 601 board).
 *  2. preflight() — every safety precondition, each with an explicit reason.
 *     There is NO vague score: a run either clears every named check or it does
 *     not, and each failing check says exactly why.
 */

import { PairStatus } from '../../services/version-state';

export const SUPPORTED_TARGET = {
  device: 'Gamma',
  board: '601',
  asic: 'BM1370',
} as const;

/** SystemInfo subset the gates read (kept structural for easy testing). */
export interface DeviceIdentity {
  productName?: string;
  vendor?: string;
  targetDevice?: string;
  targetBoard?: string;
  targetAsic?: string;
  ASICModel?: string;
  boardVersion?: string;
}

export interface DeviceTelemetry extends DeviceIdentity {
  temp?: number;
  vrTemp?: number;
  effectiveControlTemperature?: number;
  controlSensorValid?: number;
  emergencyOverrideActive?: number;
  miningPaused?: boolean;
  overheat_mode?: number;
  hardware_fault?: string;
  power_fault?: string;
}

export type SupportedKind =
  | 'neuralaxe-supported'
  | 'neuralaxe-unsupported'
  | 'stock-axeos'
  | 'unknown';

export interface SupportedDeviceCheck {
  /** True only for a NeuralAxe-Managed Gamma / 601 / BM1370. */
  supported: boolean;
  /** True when the device reports the NeuralAxe product identity. */
  managed: boolean;
  kind: SupportedKind;
  label: string;
  /** Owner-facing explanation (always populated). */
  detail: string;
  /** Reasons the device is not executable; empty when supported. */
  reasons: string[];
}

const str = (v: unknown): string => (typeof v === 'string' ? v.trim() : '');
const num = (v: unknown): number | null => (typeof v === 'number' && isFinite(v) ? v : null);

/**
 * Classify a device for Stability Lab execution. Managed identity comes from the
 * additive NeuralAxe product fields; the supported target comes from the
 * declared build target. Nothing here talks to the network.
 */
export function supportedDevice(info: DeviceIdentity | null | undefined): SupportedDeviceCheck {
  if (!info) {
    return {
      supported: false, managed: false, kind: 'unknown',
      label: 'Awaiting device',
      detail: 'No device telemetry yet — the Stability Lab is read-only until the device reports in.',
      reasons: ['No device telemetry received yet.'],
    };
  }

  const productName = str(info.productName);
  const managed = productName !== '';

  if (!managed) {
    return {
      supported: false, managed: false, kind: 'stock-axeos',
      label: 'Stock AxeOS device',
      detail: 'This device is not running NeuralAxe OS. The Stability Lab only executes on NeuralAxe-Managed hardware; here it is read-only.',
      reasons: ['Device does not report the NeuralAxe product identity (stock AxeOS / ESP-Miner).'],
    };
  }

  const device = str(info.targetDevice);
  const board = str(info.targetBoard);
  const asic = str(info.targetAsic);
  const runtimeAsic = str(info.ASICModel);
  const reasons: string[] = [];

  if (device && device !== SUPPORTED_TARGET.device) {
    reasons.push(`Device family ${device} is not supported (only ${SUPPORTED_TARGET.device}).`);
  }
  if (board && board !== SUPPORTED_TARGET.board) {
    reasons.push(`Board ${board} is not a Stability Lab target (only board ${SUPPORTED_TARGET.board}).`);
  }
  if (asic && asic !== SUPPORTED_TARGET.asic) {
    reasons.push(`ASIC ${asic} is not supported (only ${SUPPORTED_TARGET.asic}).`);
  }
  if (runtimeAsic && runtimeAsic !== SUPPORTED_TARGET.asic) {
    reasons.push(`Reported ASIC ${runtimeAsic} is not ${SUPPORTED_TARGET.asic}.`);
  }
  // A managed device must still DECLARE its target; missing declaration = cannot confirm support.
  if (!board || !asic) {
    reasons.push('NeuralAxe build target is not fully declared — cannot confirm supported hardware.');
  }

  if (reasons.length) {
    return {
      supported: false, managed: true, kind: 'neuralaxe-unsupported',
      label: `Unsupported target${board ? ` (board ${board})` : ''}`,
      detail: `${productName} on an unsupported target. The Stability Lab is read-only here — it executes only on ${SUPPORTED_TARGET.device} / board ${SUPPORTED_TARGET.board} / ${SUPPORTED_TARGET.asic}.`,
      reasons,
    };
  }

  return {
    supported: true, managed: true, kind: 'neuralaxe-supported',
    label: `${SUPPORTED_TARGET.device} · board ${SUPPORTED_TARGET.board} · ${SUPPORTED_TARGET.asic}`,
    detail: `${productName} on a supported ${SUPPORTED_TARGET.device} (board ${SUPPORTED_TARGET.board}, ${SUPPORTED_TARGET.asic}).`,
    reasons: [],
  };
}

// ---------------------------------------------------------------------------
// Preflight
// ---------------------------------------------------------------------------

/**
 * Owner-visible ASIC/VRM stop thresholds for the session. Conservative defaults;
 * the owner may TIGHTEN them but never raise them beyond the safe UI ceilings —
 * these mirror the fixed semantic danger lines (ASIC 70 °C, VRM 105 °C) and stay
 * a margin under the firmware's own hard protection, which is never replaced.
 */
export const STOP_THRESHOLD_BOUNDS = {
  asicC: { min: 55, max: 70, default: 68 },
  vrmC: { min: 70, max: 105, default: 100 },
} as const;

export function clampAsicStop(value: unknown): number {
  const n = num(value);
  if (n === null) return STOP_THRESHOLD_BOUNDS.asicC.default;
  return Math.min(STOP_THRESHOLD_BOUNDS.asicC.max, Math.max(STOP_THRESHOLD_BOUNDS.asicC.min, n));
}

export function clampVrmStop(value: unknown): number {
  const n = num(value);
  if (n === null) return STOP_THRESHOLD_BOUNDS.vrmC.default;
  return Math.min(STOP_THRESHOLD_BOUNDS.vrmC.max, Math.max(STOP_THRESHOLD_BOUNDS.vrmC.min, n));
}

export interface PreflightInput {
  info: DeviceTelemetry | null;
  /** Telemetry freshness — the real online signal, not the WebSocket socket. */
  online: boolean;
  /** Optional freshness explanation (age / staleness) shown on the online check. */
  onlineDetail?: string;
  pairStatus: PairStatus | null;
  profilesQueued: number;
  baselineCaptured: boolean;
  /** Another Stability Lab session already running (this or another tab). */
  otherSessionActive: boolean;
  stopAsicC: number;
  stopVrmC: number;
}

export interface PreflightCheck {
  id: string;
  label: string;
  ok: boolean;
  /** Explanation shown whether the check passes or fails. */
  detail: string;
  /** Device checks are moot when the device is unsupported. */
  applicable: boolean;
}

export interface PreflightResult {
  canStart: boolean;
  supported: SupportedDeviceCheck;
  checks: PreflightCheck[];
  /** Just the failing, applicable checks — the blocking reasons. */
  blockers: PreflightCheck[];
}

/**
 * Deterministic preflight. A session may start only when EVERY applicable check
 * passes. Each check carries its own explanation — there is no opaque score.
 */
export function preflight(input: PreflightInput): PreflightResult {
  const supported = supportedDevice(input.info);
  const info = input.info;
  const checks: PreflightCheck[] = [];

  const add = (id: string, label: string, ok: boolean, detail: string, applicable = true) =>
    checks.push({ id, label, ok, detail, applicable });

  add('supported', 'Supported device',
    supported.supported,
    supported.supported ? supported.detail : supported.reasons.join(' '),
    true);

  // Device-dependent checks only make sense on a supported device.
  const dev = supported.supported;

  add('online', 'Device online (fresh telemetry)',
    dev && input.online,
    input.onlineDetail
      ? input.onlineDetail
      : (input.online ? 'Device is streaming fresh telemetry.' : 'No fresh telemetry — reconnect before running a session.'),
    dev);

  const pair = input.pairStatus;
  const pairOk = !!pair && (pair.state === 'live-match' || pair.state === 'boot-match');
  add('pair', 'Firmware / web pair',
    dev && pairOk,
    pair
      ? (pairOk ? `${pair.primary}. ${pair.secondary}.` : `${pair.primary} — ${pair.secondary}.`)
      : 'Pair status unknown — cannot verify a matching firmware/web release.',
    dev);

  const emergency = num(info?.emergencyOverrideActive) === 1;
  add('emergency', 'No emergency override',
    dev && !emergency,
    emergency ? 'Emergency thermal override is active — hard protection is forcing 100 % fan.' : 'No emergency thermal override is active.',
    dev);

  const overheat = num(info?.overheat_mode) === 1;
  add('overheat', 'No overheat lockout',
    dev && !overheat,
    overheat ? 'Overheat protection is engaged.' : 'Overheat protection is not engaged.',
    dev);

  const sensorValid = num(info?.controlSensorValid) === 1;
  add('sensor', 'Valid control sensor',
    dev && sensorValid,
    sensorValid ? 'The control temperature sensor is reporting valid data.' : 'The control temperature sensor is not reporting valid data.',
    dev);

  const asic = num(info?.temp);
  const vrm = num(info?.vrTemp);
  const asicOk = asic !== null && asic > 0 && asic < input.stopAsicC;
  const vrmOk = vrm === null || vrm <= 0 ? true : vrm < input.stopVrmC; // VRM optional on some boards
  const tempsOk = asicOk && vrmOk;
  let tempDetail: string;
  if (asic === null || asic <= 0) {
    tempDetail = 'ASIC temperature is unavailable — cannot start without a valid reading.';
  } else if (!asicOk) {
    tempDetail = `ASIC ${Math.round(asic)} °C is already at/above the ${input.stopAsicC} °C session stop limit.`;
  } else if (!vrmOk) {
    tempDetail = `VRM ${Math.round(vrm!)} °C is already at/above the ${input.stopVrmC} °C session stop limit.`;
  } else {
    tempDetail = `ASIC ${Math.round(asic)} °C${vrm && vrm > 0 ? `, VRM ${Math.round(vrm)} °C` : ''} — below the session stop limits.`;
  }
  add('temps', 'Temperatures below stop limits', dev && tempsOk, tempDetail, dev);

  const mining = info?.miningPaused !== true;
  add('mining', 'Mining active',
    dev && mining,
    mining ? 'Mining is active.' : 'Mining is paused — resume mining before running a session.',
    dev);

  const hasProfiles = num(input.profilesQueued) !== null && input.profilesQueued >= 1;
  add('profiles', 'At least one profile queued',
    hasProfiles,
    hasProfiles ? `${input.profilesQueued} profile(s) queued.` : 'Add at least one profile to the queue.',
    true);

  add('baseline', 'Original configuration captured',
    dev && input.baselineCaptured,
    input.baselineCaptured ? 'The current configuration is captured for restore-original.' : 'The current configuration has not been captured yet.',
    dev);

  add('no-session', 'No other session running',
    !input.otherSessionActive,
    input.otherSessionActive ? 'Another Stability Lab session is already running.' : 'No other Stability Lab session is running.',
    true);

  const applicable = checks.filter(c => c.applicable);
  const canStart = applicable.every(c => c.ok);
  const blockers = applicable.filter(c => !c.ok);

  return { canStart, supported, checks, blockers };
}
