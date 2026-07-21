/**
 * NeuralAxe Pool Strategy Center — interrupted-switch recovery (Phase 2M,
 * Stage 5 / Blocker 4).
 *
 * A frontend state machine cannot guarantee rollback when the browser closes,
 * crashes, the PC sleeps or the network drops mid-switch. When a switch starts,
 * a NON-SECRET record of what was being switched (from → to, and whether a
 * password was to be replaced) is persisted so that, after a reload, the owner
 * gets an honest recovery state derived from live telemetry — never a false
 * "Complete" and never a claim that the previous pool was restored unless
 * telemetry actually shows it.
 *
 * NO PASSWORD is ever part of this record. Because the session secret is gone
 * after a reload, the original password can never be recovered here; when the
 * switch had replaced a password the recovery guidance says so and stays masked.
 */

import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { normalizePoolHost } from 'src/app/services/block-intelligence/pool-normalize';
import { PoolChain, normalizePort } from './pool-profile';
import { CapturedConfig } from './pool-history';

/** Non-secret identity of one pool (for comparison only). */
export interface PoolIdentitySnapshot {
  host: string;
  port: number | null;
  user: string;
}

/**
 * Persisted the instant a switch starts; cleared when it finishes cleanly.
 * `original` is the FULL non-secret captured configuration (host/port/user/tls/
 * protocol/preference — never a password) so a post-reload rollback can reapply
 * it. `target` is only the primary identity, for comparison.
 */
export interface SwitchInterruptionRecord {
  sessionId: string;
  at: number;
  targetProfileName: string | null;
  targetChain: PoolChain;
  original: CapturedConfig;
  target: PoolIdentitySnapshot;
  /** Whether the interrupted switch intended to replace a pool password. */
  passwordWasReplaced: boolean;
}

export type RecoverySituation = 'on-original' | 'on-target' | 'unknown' | 'no-telemetry';

export interface InterruptionRecovery {
  situation: RecoverySituation;
  headline: string;
  detail: string;
  /** Offer to reapply the original configuration. */
  canRetryRollback: boolean;
  /** True only when the original password is still on the device (never replaced). */
  originalPasswordRecoverable: boolean;
  /** Masked guidance shown when the original password cannot be recovered. */
  passwordNote: string | null;
}

function s(value: unknown): string {
  return typeof value === 'string' ? value.trim() : '';
}

/** Whether the device's ACTIVE primary pool matches a non-secret identity. */
export function deviceMatches(identity: PoolIdentitySnapshot, info: ISystemInfo | null | undefined): boolean {
  if (!info) return false;
  const host = normalizePoolHost((info as any).stratumURL);
  const port = normalizePort((info as any).stratumPort);
  const user = s((info as any).stratumUser);
  return host !== '' && host === normalizePoolHost(identity.host)
    && port === normalizePort(identity.port)
    && user === s(identity.user);
}

/**
 * Derive an honest recovery state from the interruption record and live
 * telemetry. Pure. Never claims a restore that telemetry does not show.
 */
export function deriveInterruptionRecovery(
  record: SwitchInterruptionRecord,
  info: ISystemInfo | null | undefined,
): InterruptionRecovery {
  const recoverable = !record.passwordWasReplaced;
  const passwordNote = recoverable
    ? null
    : 'The interrupted switch was replacing a pool password. That password was only held for the live operation and is now gone — a rollback can restore the original host and account but will keep the device\'s current password. Re-enter the original password on the Pools page if the original pool needs it.';

  const base = { canRetryRollback: true, originalPasswordRecoverable: recoverable, passwordNote };

  if (!info) {
    return {
      ...base,
      situation: 'no-telemetry',
      headline: 'Previous pool switch was interrupted',
      detail: 'A switch was in progress when the dashboard closed or refreshed. No device telemetry yet — reconnect, then verify the pool configuration before acting.',
    };
  }
  if (deviceMatches(record.original.primary, info)) {
    return {
      ...base,
      situation: 'on-original',
      headline: 'Interrupted — device appears to be on the ORIGINAL pool',
      detail: 'Live telemetry matches the pool from before the switch. The switch likely did not take effect (or was already rolled back). Verify, and re-run the switch if you still want it.',
    };
  }
  if (deviceMatches(record.target, info)) {
    return {
      ...base,
      situation: 'on-target',
      headline: `Interrupted — device appears to be on the TARGET pool${record.targetProfileName ? ` (${record.targetProfileName})` : ''}`,
      detail: 'Live telemetry matches the target pool, so the switch may have applied — but it was never verified in this session. Keep it if intended, or use Retry rollback to return to the original configuration.',
    };
  }
  return {
    ...base,
    situation: 'unknown',
    headline: 'Interrupted — current pool configuration is unknown',
    detail: 'Live telemetry matches neither the original nor the target pool. Verify the configuration on the Pools page before acting.',
  };
}
