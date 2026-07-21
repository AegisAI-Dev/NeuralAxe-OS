/**
 * NeuralAxe Pool Strategy Center — preflight contract (Phase 2M, Stage 6).
 *
 * A switch may start only when EVERY applicable check passes. Each check carries
 * its own explanation — there is NO opaque score. Device checks are moot (and
 * marked not-applicable) when the device is unsupported. The supported-device
 * gate is the shared one used by the Stability Lab, so "supported" means exactly
 * the same thing across the product: a NeuralAxe-Managed Gamma / 601 / BM1370
 * by DECLARED build target.
 */

import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { PairStatus } from 'src/app/services/version-state';
import { SupportedDeviceCheck, supportedDevice } from '../stability-lab/stability-preflight';
import { PoolProfile, endpointUsable, fallbackProvided } from './pool-profile';

const num = (v: unknown): number | null => (typeof v === 'number' && isFinite(v) ? v : null);

export interface PoolPreflightInput {
  info: ISystemInfo | null;
  /** Telemetry freshness — the real online signal, not the WebSocket socket. */
  online: boolean;
  onlineDetail?: string;
  pairStatus: PairStatus | null;
  profile: PoolProfile | null;
  /** validateProfile(profile) result. */
  profileErrors: string[];
  /** The review reports a meaningful change vs the current configuration. */
  reviewHasChange: boolean;
  /** The current device configuration has been captured for restore. */
  originalCaptured: boolean;
  /** A Stability Lab session is running (this or another tab). */
  stabilityActive: boolean;
  /** Another pool switch is already in progress. */
  otherSwitchActive: boolean;
}

export interface PoolPreflightCheck {
  id: string;
  label: string;
  ok: boolean;
  detail: string;
  applicable: boolean;
}

export interface PoolPreflightResult {
  canStart: boolean;
  supported: SupportedDeviceCheck;
  checks: PoolPreflightCheck[];
  blockers: PoolPreflightCheck[];
}

/**
 * Deterministic preflight. Pure — the caller supplies freshness, pair status and
 * the profile/review state.
 */
export function poolPreflight(input: PoolPreflightInput): PoolPreflightResult {
  const supported = supportedDevice(input.info);
  const info = input.info;
  const dev = supported.supported;
  const checks: PoolPreflightCheck[] = [];
  const add = (id: string, label: string, ok: boolean, detail: string, applicable = true) =>
    checks.push({ id, label, ok, detail, applicable });

  add('supported', 'Supported NeuralAxe device',
    supported.supported,
    supported.supported ? supported.detail : supported.reasons.join(' '),
    true);

  add('online', 'Device online (fresh telemetry)',
    dev && input.online,
    input.onlineDetail
      ? input.onlineDetail
      : (input.online ? 'Device is streaming fresh telemetry.' : 'No fresh telemetry — reconnect before switching pools.'),
    dev);

  const pair = input.pairStatus;
  const pairOk = !!pair && (pair.state === 'live-match' || pair.state === 'boot-match');
  add('pair', 'Firmware / web pair',
    dev && pairOk,
    pair
      ? (pairOk ? `${pair.primary}. ${pair.secondary}.` : `${pair.primary} — ${pair.secondary}.`)
      : 'Pair status unknown — cannot verify a matching firmware/web release.',
    dev);

  const miningKnown = !!info && (typeof info.miningPaused === 'boolean' || num(info.hashRate) !== null);
  add('mining-known', 'Mining state known',
    dev && miningKnown,
    miningKnown
      ? (info?.miningPaused === true ? 'Mining is currently paused (a switch will still apply and reconnect).' : 'Mining state is reported by the device.')
      : 'The device has not reported a mining state yet.',
    dev);

  const emergency = num(info?.emergencyOverrideActive) === 1;
  add('emergency', 'No emergency override',
    dev && !emergency,
    emergency ? 'Emergency thermal override is active — resolve it before switching pools.' : 'No emergency thermal override is active.',
    dev);

  add('no-stability', 'No Stability Lab session running',
    !input.stabilityActive,
    input.stabilityActive ? 'A Stability Lab session is active — finish it before switching pools.' : 'No Stability Lab session is running.',
    true);

  add('no-other-switch', 'No other pool switch running',
    !input.otherSwitchActive,
    input.otherSwitchActive ? 'A pool switch is already in progress.' : 'No other pool switch is in progress.',
    true);

  add('captured', 'Current configuration captured',
    dev && input.originalCaptured,
    input.originalCaptured ? 'The current pool configuration is captured for rollback / restore.' : 'The current pool configuration has not been captured yet.',
    dev);

  const hasProfile = !!input.profile;
  const profileValid = hasProfile && input.profileErrors.length === 0;
  add('profile-valid', 'Target profile is valid',
    profileValid,
    !hasProfile
      ? 'Select a pool profile to switch to.'
      : (profileValid ? `Profile "${input.profile!.name}" validates.` : input.profileErrors.join(' ')),
    true);

  add('differs', 'Target differs from current configuration',
    dev && input.reviewHasChange,
    input.reviewHasChange ? 'The target profile changes the current pool configuration.' : 'The target profile matches the current configuration — nothing would change.',
    dev);

  const credsOk = !!input.profile
    && endpointUsable(input.profile.primary)
    && (!fallbackProvided(input.profile.fallback) || endpointUsable(input.profile.fallback));
  add('credentials', 'Required credentials available',
    credsOk,
    !input.profile
      ? 'No profile selected.'
      : (credsOk ? 'Host, port and account are present for the pools this profile will write.' : 'The profile is missing a required host, port or account value.'),
    true);

  const applicable = checks.filter(c => c.applicable);
  const canStart = applicable.every(c => c.ok);
  const blockers = applicable.filter(c => !c.ok);
  return { canStart, supported, checks, blockers };
}
