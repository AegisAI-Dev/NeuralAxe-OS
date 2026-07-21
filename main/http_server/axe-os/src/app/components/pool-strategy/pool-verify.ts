/**
 * NeuralAxe Pool Strategy Center — apply / reconnect verification (Phase 2M,
 * Stage 7 & 8).
 *
 * A switch is NEVER declared successful just because the PATCH returned 200. This
 * module derives, from genuinely-available telemetry only, the layered evidence:
 *
 *   settings applied → pool connected → mining resumed → target host verified
 *                    → share activity observed (optional)
 *
 * Basic connection success = connected AND mining resumed AND the active pool
 * host matches the target primary OR fallback — it does NOT require an accepted
 * share (solo-pool share cadence varies). The same evaluation verifies a rollback
 * or restore by pointing it at the original configuration's hosts.
 *
 * Host comparison reuses the shared host-only normalizer — usernames, workers and
 * passwords are never involved.
 */

import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { normalizePoolHost } from 'src/app/services/block-intelligence/pool-normalize';
import { FRESHNESS_LIMIT_MS, SESSION_RECONNECT_GRACE_MS } from '../stability-lab/stability-freshness';

/**
 * Conservative reconnect / verification timeouts, derived from the freshness
 * contract. A pool switch requires a device restart, so the reconnect budget is
 * larger than a live tuning change: freshness limit + reconnect grace, doubled to
 * cover the reboot itself.
 */
export const RECONNECT_TIMEOUT_MS = (FRESHNESS_LIMIT_MS + SESSION_RECONNECT_GRACE_MS) * 2; // 90 s
/** After reconnect, how long to keep verifying mining + host before giving up. */
export const VERIFY_TIMEOUT_MS = FRESHNESS_LIMIT_MS + SESSION_RECONNECT_GRACE_MS;          // 45 s

const num = (v: unknown): number | null => (typeof v === 'number' && isFinite(v) ? v : null);

/** Target hosts a switch/rollback should land on (normalized bare hosts). */
export interface VerifyTarget {
  primaryHost: string;
  fallbackHost: string;
}

export type ActiveHostRole = 'primary' | 'fallback' | 'none' | 'unknown';

export interface PoolVerification {
  /** Caller pass-through: the PATCH request succeeded. */
  settingsApplied: boolean;
  /** Fresh telemetry is arriving (the device reconnected). */
  poolConnected: boolean;
  /** Mining is active (not paused, positive hashrate). */
  miningResumed: boolean;
  /** The device's active pool host matches the target primary or fallback. */
  targetHostVerified: boolean;
  activeHostRole: ActiveHostRole;
  /** New accepted/rejected share activity observed since the baseline. */
  shareActivityObserved: boolean;
  latencyMs: number | null;
  faulted: boolean;
  faultReason: string | null;
  /** Basic-connection success — connected + mining + target host, no fault. Not share-gated. */
  connectedAndMining: boolean;
  detail: string;
}

export interface PoolVerifyInput {
  info: ISystemInfo | null;
  /** Fresh telemetry (the real reconnect signal), from the freshness contract. */
  online: boolean;
  target: VerifyTarget;
  baselineAccepted: number | null;
  baselineRejected: number | null;
}

/** Which configured pool the device is actively using, by normalized host. */
function activeHost(info: ISystemInfo): string {
  const usingFallback = !!num(info.isUsingFallbackStratum);
  return normalizePoolHost(usingFallback ? (info as any).fallbackStratumURL : (info as any).stratumURL);
}

export function evaluatePoolVerification(input: PoolVerifyInput): PoolVerification {
  const info = input.info;
  if (!info) {
    return {
      settingsApplied: false, poolConnected: false, miningResumed: false, targetHostVerified: false,
      activeHostRole: 'unknown', shareActivityObserved: false, latencyMs: null,
      faulted: false, faultReason: null, connectedAndMining: false,
      detail: 'No telemetry yet — verification pending.',
    };
  }

  const poolConnected = input.online;
  const paused = info.miningPaused === true;
  const hashRate = num(info.hashRate);
  const miningResumed = poolConnected && !paused && hashRate !== null && hashRate > 0;

  const active = activeHost(info);
  const primary = normalizePoolHost(input.target.primaryHost);
  const fallback = normalizePoolHost(input.target.fallbackHost);
  let activeHostRole: ActiveHostRole = 'none';
  let targetHostVerified = false;
  if (active === '') {
    activeHostRole = 'unknown';
  } else if (primary !== '' && active === primary) {
    activeHostRole = 'primary';
    targetHostVerified = true;
  } else if (fallback !== '' && active === fallback) {
    activeHostRole = 'fallback';
    targetHostVerified = true;
  }
  // Only assert host verification once telemetry is actually fresh.
  if (!poolConnected) {
    targetHostVerified = false;
  }

  const acc = num(info.sharesAccepted);
  const rej = num(info.sharesRejected);
  const baseA = input.baselineAccepted;
  const baseR = input.baselineRejected;
  const shareActivityObserved =
    (acc !== null && baseA !== null && acc > baseA) ||
    (rej !== null && baseR !== null && rej > baseR) ||
    // A counter reset after a restart is itself evidence of a fresh session.
    (acc !== null && baseA !== null && acc < baseA);

  const emergency = num(info.emergencyOverrideActive) === 1;
  const overheat = num(info.overheat_mode) === 1;
  const faulted = emergency || overheat;
  const faultReason = emergency
    ? 'Emergency thermal override is active.'
    : overheat ? 'Overheat protection is engaged.' : null;

  const latencyMs = num(info.responseTime);

  const connectedAndMining = poolConnected && miningResumed && targetHostVerified && !faulted;

  let detail: string;
  if (!poolConnected) {
    detail = 'Waiting for the device to reconnect (fresh telemetry).';
  } else if (faulted) {
    detail = faultReason ?? 'A device fault is active.';
  } else if (!miningResumed) {
    detail = 'Reconnected — waiting for mining to resume.';
  } else if (!targetHostVerified) {
    detail = 'Mining resumed, but the active pool host does not yet match the target.';
  } else {
    detail = `Verified: connected and mining on the target ${activeHostRole} pool.`;
  }

  return {
    settingsApplied: false,
    poolConnected,
    miningResumed,
    targetHostVerified,
    activeHostRole,
    shareActivityObserved,
    latencyMs,
    faulted,
    faultReason,
    connectedAndMining,
    detail,
  };
}

export interface VerifyLevel {
  id: 'applied' | 'connected' | 'mining' | 'host' | 'shares';
  label: string;
  ok: boolean;
  /** Whether this level is required for basic success (shares are optional). */
  required: boolean;
  detail: string;
}

/**
 * Display rows for the evidence levels (Stage 7 / Blocker 5). The wording is
 * deliberately precise: we observe that the device SUBMITTED settings, RESTARTED,
 * RECONNECTED, is on the TARGET HOST, and RESUMED mining. A successful pool
 * connection lets us INFER that the pool accepted the worker/password — we never
 * claim the password itself was read back or verified.
 */
export function verificationLevels(v: PoolVerification): VerifyLevel[] {
  const roleText = v.activeHostRole === 'primary' ? 'primary'
    : v.activeHostRole === 'fallback' ? 'fallback'
    : v.activeHostRole === 'none' ? 'a different pool' : 'unknown';
  return [
    { id: 'applied', label: 'Target settings submitted', ok: v.settingsApplied, required: true,
      detail: v.settingsApplied ? 'The target pool settings were submitted to the device.' : 'The target pool settings have not been submitted yet.' },
    { id: 'connected', label: 'Device restarted & telemetry reconnected', ok: v.poolConnected, required: true,
      detail: v.poolConnected ? 'The device restarted and is streaming fresh telemetry again.' : 'Waiting for the device to restart and reconnect.' },
    { id: 'mining', label: 'Mining resumed', ok: v.miningResumed, required: true,
      detail: v.miningResumed ? 'Mining resumed with a positive hashrate.' : 'Mining has not resumed yet.' },
    { id: 'host', label: 'Target host observed (pool accepted the connection)', ok: v.targetHostVerified, required: true,
      detail: v.targetHostVerified
        ? `Active pool matches the target ${roleText} pool. A successful pool connection implies the pool accepted the worker credentials — the password itself is never read back from the device.`
        : `Active pool is ${roleText}.` },
    { id: 'shares', label: 'Share activity observed', ok: v.shareActivityObserved, required: false,
      detail: v.shareActivityObserved ? 'New share activity was observed on the target pool.' : 'No new share yet (solo cadence varies — not required for success).' },
  ];
}
