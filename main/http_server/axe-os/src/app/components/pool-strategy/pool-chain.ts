/**
 * NeuralAxe Pool Strategy Center — explicit chain context (Phase 2M, Stage 3).
 *
 * The active mining-chain context is derived ONLY from the owner-selected
 * profile that was actually applied through the Center, verified against the
 * device's live pool configuration. It is NEVER inferred from a hostname.
 *
 * The context is honest in three ways:
 *   - if a labelled profile was applied AND the device still holds that config
 *     → the context is that profile's chain, VERIFIED;
 *   - if a labelled profile was applied but the device config has since changed
 *     (edited on the Pools page, a manual save, etc.) → the context is Unknown
 *     with an explanation (never a stale chain claim);
 *   - if no profile was ever applied through the Center → Unknown.
 */

import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { PoolChain, chainLabel, chainShort, chainShortLabel, normalizePort } from './pool-profile';

/** The exact wording required whenever a chain label is shown. */
export const CHAIN_DISCLAIMER =
  'Chain label describes the selected pool profile. NeuralAxe does not '
  + 'cryptographically determine which chain the pool is mining.';

/**
 * Persisted record of the profile last applied through the Center. Stores a
 * snapshot of the applied PRIMARY identity so verification survives the profile
 * later being edited or deleted. No password is ever stored here.
 */
export interface ActivePoolRecord {
  profileId: string;
  profileName: string;
  chain: PoolChain;
  /** Snapshot of the applied primary identity (masked in every display). */
  primaryHost: string;
  primaryPort: number | null;
  primaryUser: string;
  appliedAt: number;
}

export type ChainContextChain = PoolChain | 'unknown';

export interface ChainContext {
  /** Effective context chain, or 'unknown' when it cannot be asserted. */
  chain: ChainContextChain;
  /** True only when a labelled profile is verified as currently applied. */
  labelled: boolean;
  /** True when the device config still matches the applied profile. */
  verified: boolean;
  label: string;
  short: string;
  /** The labelled profile name when verified; else null. */
  profileName: string | null;
  /** Owner-facing explanation (always populated). */
  detail: string;
}

function s(value: unknown): string {
  return typeof value === 'string' ? value.trim() : '';
}

/**
 * Whether the device's current PRIMARY pool identity still matches the record's
 * applied snapshot (host + port + user). Host compared case-insensitively.
 */
export function recordMatchesDevice(record: ActivePoolRecord, info: ISystemInfo | null | undefined): boolean {
  if (!info) return false;
  const host = s((info as any).stratumURL).toLowerCase();
  const port = normalizePort((info as any).stratumPort);
  const user = s((info as any).stratumUser);
  return host === s(record.primaryHost).toLowerCase()
    && port === normalizePort(record.primaryPort)
    && user === s(record.primaryUser);
}

function unknownContext(detail: string): ChainContext {
  return {
    chain: 'unknown',
    labelled: false,
    verified: false,
    label: 'Custom / Unknown',
    short: chainShortLabel('unknown'),
    profileName: null,
    detail,
  };
}

/**
 * Derive the current chain context. Pure — the caller supplies the stored active
 * record and the live device info.
 */
export function deriveChainContext(
  record: ActivePoolRecord | null | undefined,
  info: ISystemInfo | null | undefined,
): ChainContext {
  if (!record) {
    return unknownContext('No labelled pool profile has been applied through the Pool Strategy Center yet.');
  }
  if (!recordMatchesDevice(record, info)) {
    return unknownContext(
      `The device pool configuration no longer matches "${record.profileName}" (${chainShort(record.chain)}), `
      + 'so the chain context cannot be asserted — it was changed outside the Pool Strategy Center.',
    );
  }
  return {
    chain: record.chain,
    labelled: true,
    verified: true,
    label: chainLabel(record.chain),
    short: chainShort(record.chain),
    profileName: record.profileName,
    detail: `Active profile "${record.profileName}" is labelled ${chainLabel(record.chain)}. ${CHAIN_DISCLAIMER}`,
  };
}

/** True when the context asserts a BTC chain (Block Intelligence is in-context). */
export function isBtcContext(context: ChainContext): boolean {
  return context.labelled && context.chain === 'BTC';
}

/** True when the context asserts a BCH chain (Block Intelligence is out-of-context). */
export function isBchContext(context: ChainContext): boolean {
  return context.labelled && context.chain === 'BCH';
}
