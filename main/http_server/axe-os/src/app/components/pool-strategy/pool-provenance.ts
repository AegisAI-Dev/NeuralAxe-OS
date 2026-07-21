/**
 * NeuralAxe Pool Strategy Center — immutable operation provenance (Phase 2M.0).
 *
 * A pool switch (or restore) mutates active state: applying a profile makes the
 * TARGET the active configuration, which recomputes the live chain context. If
 * the history record read that live context at completion it would record
 * `target → target` (the real-hardware `BCH → BCH` bug). This module captures an
 * IMMUTABLE, non-secret snapshot of the SOURCE context BEFORE any mutation, so
 * the history record, restore reactivation and chain-context reporting are
 * faithful to what actually happened.
 *
 * PRIVACY: provenance holds only non-secret data — no password, no session
 * secret, no full wallet/worker/account, no raw API response. Hosts are masked.
 */

import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { PoolChain, PoolProfile, maskHost, normalizePort } from './pool-profile';
import { ChainContext, ChainContextChain, ActivePoolRecord } from './pool-chain';
import { RecoveryResult } from './pool-switch-machine';
import { deviceMatches } from './pool-recovery';

export const UNLABELLED_CONFIG = 'Unlabelled configuration';

export type ProvenanceKind = 'switch' | 'restore';

/** Immutable, non-secret snapshot captured before a pool mutation. */
export interface SwitchProvenance {
  operationId: string;
  at: number;
  kind: ProvenanceKind;
  // --- SOURCE (state before the mutation) ---
  sourceProfileId: string | null;
  sourceProfileName: string | null;      // null ⇒ UNLABELLED_CONFIG
  sourceChain: ChainContextChain;        // BTC | BCH | custom | 'unknown'
  sourcePrimaryHostMasked: string;
  sourcePrimaryPort: number | null;
  sourceFallbackHostMasked: string;
  sourceFallbackPort: number | null;
  // --- TARGET (what the operation moves to) ---
  targetProfileId: string | null;
  targetProfileName: string | null;      // null ⇒ UNLABELLED_CONFIG
  targetChain: ChainContextChain;
  // --- flags ---
  passwordReplaced: boolean;
  hadPreviousLabelledProfile: boolean;
}

function s(v: unknown): string { return typeof v === 'string' ? v.trim() : ''; }

/** Source host/port summary from the device configuration captured pre-switch. */
export interface SourceHosts {
  primaryHost: string; primaryPort: number | null;
  fallbackHost: string; fallbackPort: number | null;
}

/**
 * Build the provenance for a SWITCH. The source comes from the honest, verified
 * chain context that was live before the switch (never recomputed afterwards)
 * and the current active record; the target is the profile being applied.
 */
export function buildSwitchProvenance(input: {
  operationId: string;
  at: number;
  sourceContext: ChainContext;
  sourceActive: ActivePoolRecord | null;
  sourceHosts: SourceHosts;
  targetProfile: PoolProfile;
  passwordReplaced: boolean;
}): SwitchProvenance {
  const labelled = input.sourceContext.labelled;
  return {
    operationId: input.operationId,
    at: input.at,
    kind: 'switch',
    sourceProfileId: labelled ? (input.sourceActive?.profileId ?? null) : null,
    sourceProfileName: labelled ? (input.sourceContext.profileName ?? null) : null,
    sourceChain: input.sourceContext.chain,
    sourcePrimaryHostMasked: maskHost(input.sourceHosts.primaryHost),
    sourcePrimaryPort: normalizePort(input.sourceHosts.primaryPort),
    sourceFallbackHostMasked: maskHost(input.sourceHosts.fallbackHost),
    sourceFallbackPort: normalizePort(input.sourceHosts.fallbackPort),
    targetProfileId: input.targetProfile.id,
    targetProfileName: input.targetProfile.name,
    targetChain: input.targetProfile.chain,
    passwordReplaced: input.passwordReplaced,
    hadPreviousLabelledProfile: labelled,
  };
}

/**
 * Build the provenance for a RESTORE. The source is the profile currently active
 * (what we are undoing); the target is the PREVIOUS profile/chain recorded when
 * the original switch was captured.
 */
export function buildRestoreProvenance(input: {
  operationId: string;
  at: number;
  sourceContext: ChainContext;
  sourceActive: ActivePoolRecord | null;
  sourceHosts: SourceHosts;
  previousProfileId: string | null;
  previousProfileName: string | null;
  previousChain: ChainContextChain;
  passwordReplaced: boolean;
}): SwitchProvenance {
  const labelled = input.sourceContext.labelled;
  const prevLabelled = input.previousProfileId !== null && input.previousChain !== 'unknown';
  return {
    operationId: input.operationId,
    at: input.at,
    kind: 'restore',
    sourceProfileId: labelled ? (input.sourceActive?.profileId ?? null) : null,
    sourceProfileName: labelled ? (input.sourceContext.profileName ?? null) : null,
    sourceChain: input.sourceContext.chain,
    sourcePrimaryHostMasked: maskHost(input.sourceHosts.primaryHost),
    sourcePrimaryPort: normalizePort(input.sourceHosts.primaryPort),
    sourceFallbackHostMasked: maskHost(input.sourceHosts.fallbackHost),
    sourceFallbackPort: normalizePort(input.sourceHosts.fallbackPort),
    targetProfileId: prevLabelled ? input.previousProfileId : null,
    targetProfileName: prevLabelled ? input.previousProfileName : null,
    targetChain: prevLabelled ? input.previousChain : 'unknown',
    passwordReplaced: input.passwordReplaced,
    hadPreviousLabelledProfile: prevLabelled,
  };
}

/** Profile name for a transition end, or the "Unlabelled configuration" label. */
export function transitionProfileLabel(profileName: string | null): string {
  return profileName && profileName.trim() !== '' ? profileName : UNLABELLED_CONFIG;
}

// ---------------------------------------------------------------------------
// Restore reactivation — deterministic post-restore active state (Stage 4/5)
// ---------------------------------------------------------------------------

export type RestoreMatchStatus =
  | 'exact-profile'   // a labelled profile matches live and is reactivated
  | 'unlabelled'      // restored to the unlabelled original → Custom/Unknown
  | 'mismatch'        // live config does not match the captured original
  | 'ambiguous'       // several stored profiles match; none chosen
  | 'profile-missing';// the previous labelled profile is gone / no match

export interface RestoreActivation {
  status: RestoreMatchStatus;
  /** The active record to store, or null when nothing should be marked active. */
  activate: ActivePoolRecord | null;
  /** Resulting chain context. */
  chain: ChainContextChain;
  /** Target label for the restore history record. */
  historyTargetProfileName: string | null;
  historyTargetChain: ChainContextChain;
  note: string;
}

/** Non-secret primary identity of a captured configuration. */
export interface RestoredIdentity {
  host: string; port: number | null; user: string;
}

function profileMatchesDevice(profile: PoolProfile, info: ISystemInfo | null): boolean {
  return deviceMatches(
    { host: profile.primary.host, port: normalizePort(profile.primary.port), user: profile.primary.user },
    info,
  );
}

function activeRecordFor(profile: PoolProfile, at: number): ActivePoolRecord {
  return {
    profileId: profile.id,
    profileName: profile.name,
    chain: profile.chain,
    primaryHost: profile.primary.host,
    primaryPort: normalizePort(profile.primary.port),
    primaryUser: profile.primary.user,
    appliedAt: at,
  };
}

/**
 * Decide the post-restore active state and history target. Matching verifies the
 * NON-SECRET pool identity (host/port/account) only — passwords are write-only
 * and never compared. Prefers the exact previous profile ID from the snapshot;
 * reports ambiguity rather than choosing arbitrarily.
 */
export function resolveRestoreActivation(input: {
  previousProfileId: string | null;
  previousProfileName: string | null;
  previousChain: ChainContextChain;
  restoredIdentity: RestoredIdentity;
  liveInfo: ISystemInfo | null;
  profiles: PoolProfile[];
  at: number;
}): RestoreActivation {
  const { liveInfo, profiles, previousProfileId, previousChain, previousProfileName, at } = input;

  // The device must actually be on the captured original, verified by identity.
  const liveOnRestored = deviceMatches(input.restoredIdentity, liveInfo);
  if (!liveInfo || !liveOnRestored) {
    return {
      status: 'mismatch', activate: null, chain: 'unknown',
      historyTargetProfileName: null, historyTargetChain: 'unknown',
      note: 'The live pool configuration does not match the captured original — the restore is not confirmed active.',
    };
  }

  // Prefer the exact previous active profile ID.
  if (previousProfileId !== null) {
    const exact = profiles.find(p => p.id === previousProfileId);
    if (exact && profileMatchesDevice(exact, liveInfo)) {
      return {
        status: 'exact-profile', activate: activeRecordFor(exact, at), chain: exact.chain,
        historyTargetProfileName: exact.name, historyTargetChain: exact.chain,
        note: `Reactivated the previous profile "${exact.name}" (${exact.chain}).`,
      };
    }
  }

  // No usable previous ID: an unlabelled original restores to Custom/Unknown.
  if (previousProfileId === null && previousChain === 'unknown') {
    return {
      status: 'unlabelled', activate: null, chain: 'unknown',
      historyTargetProfileName: null, historyTargetChain: 'unknown',
      note: 'Restored to the previous unlabelled configuration; chain context is Custom / Unknown.',
    };
  }

  // Previous was labelled but the exact profile is unusable: match by identity.
  const matches = profiles.filter(p => profileMatchesDevice(p, liveInfo));
  if (matches.length === 1) {
    const only = matches[0];
    return {
      status: 'exact-profile', activate: activeRecordFor(only, at), chain: only.chain,
      historyTargetProfileName: only.name, historyTargetChain: only.chain,
      note: `Reactivated the matching profile "${only.name}" (${only.chain}).`,
    };
  }
  if (matches.length > 1) {
    return {
      status: 'ambiguous', activate: null, chain: 'unknown',
      historyTargetProfileName: previousProfileName, historyTargetChain: previousChain,
      note: 'Multiple stored profiles match this pool identity — none was marked active. Open the profile to re-label it.',
    };
  }
  return {
    status: 'profile-missing', activate: null, chain: 'unknown',
    historyTargetProfileName: previousProfileName, historyTargetChain: 'unknown',
    note: 'The previously active profile no longer exists; the restored configuration is now Unlabelled.',
  };
}

// ---------------------------------------------------------------------------
// Restore outcome wording (Stage 6)
// ---------------------------------------------------------------------------

export type RestoreCategory = 'exact' | 'operational' | 'partial' | 'failed';

export interface RestoreOutcomeView {
  category: RestoreCategory;
  severity: 'ok' | 'warn' | 'err';
  headline: string;
  detail: string;
}

/**
 * Technically-exact restore wording. A write-only password can only be proven
 * restored when it was never replaced, or when the original secret was available
 * in the supervised active operation (an in-session rollback). Otherwise a
 * verified restore is OPERATIONAL — identity restored, connection and mining
 * verified — but byte-for-byte password restoration cannot be proven, and we
 * say exactly that without an alarming generic warning.
 */
export function restoreOutcome(
  result: RecoveryResult,
  passwordWasReplaced: boolean,
  originalSecretAvailable: boolean,
): RestoreOutcomeView {
  if (result === 'verified') {
    if (!passwordWasReplaced || originalSecretAvailable) {
      return {
        category: 'exact', severity: 'ok',
        headline: 'Original configuration restored and verified',
        detail: 'The original pool host, port and account were restored, the device reconnected and mining resumed. The password was not replaced (or the original secret was reinstated in this operation).',
      };
    }
    return {
      category: 'operational', severity: 'ok',
      headline: 'Operational restore verified',
      detail: 'Original pool identity restored and mining resumed. The password is write-only; NeuralAxe verified the connection operationally but cannot prove that the original password value was restored. Re-enter the original password on the Pools page if that pool requires it.',
    };
  }
  if (result === 'partial') {
    return {
      category: 'partial', severity: 'warn',
      headline: 'Partial restore',
      detail: 'The original settings were reapplied, but reconnect / mining could not be fully verified — the previous pool is NOT confirmed restored. Verify the device.',
    };
  }
  return {
    category: 'failed', severity: 'err',
    headline: 'Restore failed',
    detail: 'The original configuration could not be re-established. Verify the device pool configuration on the Pools page.',
  };
}
