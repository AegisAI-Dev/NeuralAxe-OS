/**
 * NeuralAxe Pool Strategy Center — profile diff & review (Phase 2M, Stage 4).
 *
 * Builds the exact, MASKED changes a switch would apply, comparing the device's
 * current pool configuration to a target profile. It NEVER exposes a full
 * wallet, worker, account or password value — hosts and accounts are masked and
 * the password row only reports whether credentials will be replaced or kept.
 */

import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import {
  PoolChain, PoolEndpoint, PoolProfile, PoolRole,
  chainLabel, endpointFromInfo, fallbackProvided, maskAccount, maskHost, normalizePort,
} from './pool-profile';
import { ChainContext } from './pool-chain';

export type PoolDiffField = 'host' | 'port' | 'user' | 'password' | 'protocol' | 'tls';

export interface PoolDiffRow {
  field: PoolDiffField;
  label: string;
  role: PoolRole;
  /** Masked (host/user/password) or plain (port/protocol/tls) current value. */
  fromDisplay: string;
  toDisplay: string;
  changed: boolean;
  /** True for a credential row (password) so the UI can flag it. */
  sensitive: boolean;
}

export interface SwitchReview {
  currentChainLabel: string;
  targetChainLabel: string;
  chainChanges: boolean;
  primary: PoolDiffRow[];
  fallback: PoolDiffRow[];
  /** True when the profile has no explicit fallback (primary is mirrored). */
  fallbackMirrored: boolean;
  credentialsReplaced: { primary: boolean; fallback: boolean };
  anyChange: boolean;
  /** Pool settings always need a restart to take effect on this firmware. */
  restartExpected: boolean;
  /** Owner-facing cautions (cross-chain, kept-password, mirror). */
  warnings: string[];
}

function s(value: unknown): string {
  return typeof value === 'string' ? value.trim() : '';
}

export function tlsLabel(tls: number | null | undefined): string {
  switch (tls) {
    case 1: return 'TLS (system cert)';
    case 2: return 'TLS (custom CA)';
    default: return 'No TLS';
  }
}

function portDisplay(port: number | null): string {
  return port === null ? '—' : String(port);
}

/** Password row: current is always hidden (write-only); target reports intent. */
function passwordToDisplay(target: PoolEndpoint): string {
  return target.passwordMode === 'set' ? 'replace at switch (entered securely, never stored)' : 'keep current (unchanged)';
}

function endpointDiff(current: PoolEndpoint, target: PoolEndpoint, role: PoolRole): PoolDiffRow[] {
  const hostChanged = s(current.host).toLowerCase() !== s(target.host).toLowerCase();
  const portChanged = normalizePort(current.port) !== normalizePort(target.port);
  const userChanged = s(current.user) !== s(target.user);
  const protocolChanged = current.protocol !== target.protocol;
  const tlsChanged = (current.tls ?? 0) !== (target.tls ?? 0);
  const passwordChanged = target.passwordMode === 'set';

  return [
    { field: 'host', label: 'Host', role, fromDisplay: maskHost(current.host), toDisplay: maskHost(target.host), changed: hostChanged, sensitive: false },
    { field: 'port', label: 'Port', role, fromDisplay: portDisplay(normalizePort(current.port)), toDisplay: portDisplay(normalizePort(target.port)), changed: portChanged, sensitive: false },
    { field: 'user', label: 'User / account', role, fromDisplay: maskAccount(current.user), toDisplay: maskAccount(target.user), changed: userChanged, sensitive: true },
    { field: 'password', label: 'Password', role, fromDisplay: 'current (hidden)', toDisplay: passwordToDisplay(target), changed: passwordChanged, sensitive: true },
    { field: 'protocol', label: 'Protocol', role, fromDisplay: current.protocol, toDisplay: target.protocol, changed: protocolChanged, sensitive: false },
    { field: 'tls', label: 'TLS', role, fromDisplay: tlsLabel(current.tls), toDisplay: tlsLabel(target.tls), changed: tlsChanged, sensitive: false },
  ];
}

/**
 * Build the masked review of switching from the current device configuration to
 * `profile`. `currentContext` supplies the honest current chain label.
 */
export function buildSwitchReview(
  info: ISystemInfo | null,
  currentContext: ChainContext,
  profile: PoolProfile,
): SwitchReview {
  const currentPrimary = info ? endpointFromInfo(info, 'primary') : blankCurrent();
  const currentFallback = info ? endpointFromInfo(info, 'fallback') : blankCurrent();

  const targetPrimary = profile.primary;
  const mirrored = !fallbackProvided(profile.fallback);
  const targetFallback = mirrored ? profile.primary : (profile.fallback as PoolEndpoint);

  const primary = endpointDiff(currentPrimary, targetPrimary, 'primary');
  const fallback = endpointDiff(currentFallback, targetFallback, 'fallback');

  const credentialsReplaced = {
    primary: targetPrimary.passwordMode === 'set',
    fallback: targetFallback.passwordMode === 'set',
  };

  const primaryChanged = primary.some(r => r.changed);
  const fallbackChanged = fallback.some(r => r.changed);
  const anyChange = primaryChanged || fallbackChanged;

  const currentChain: PoolChain | 'unknown' = currentContext.chain;
  const chainChanges = currentContext.labelled && currentChain !== profile.chain;

  const warnings: string[] = [];
  if (mirrored) {
    warnings.push('This profile has no separate fallback pool — its primary will be mirrored as the fallback so an automatic failover stays on the same chain.');
  }
  if (chainChanges) {
    warnings.push(`This switch changes the labelled chain from ${chainLabel(currentChain as PoolChain)} to ${chainLabel(profile.chain)}. BTC and BCH pools require different payout / account details — confirm this profile's account is correct for its chain.`);
  }
  if (credentialsReplaced.primary || credentialsReplaced.fallback) {
    warnings.push('This profile replaces a pool password. You will enter the new password — and the current password, so rollback can restore it — before the switch applies. Neither password is stored anywhere.');
  } else {
    warnings.push('The pool password will be kept as-is on the device (it cannot be read back). If the target pool needs a different password, edit the profile to replace it.');
  }

  return {
    currentChainLabel: currentContext.label,
    targetChainLabel: chainLabel(profile.chain),
    chainChanges,
    primary,
    fallback,
    fallbackMirrored: mirrored,
    credentialsReplaced,
    anyChange,
    restartExpected: true,
    warnings,
  };
}

function blankCurrent(): PoolEndpoint {
  return { host: '', port: null, user: '', passwordMode: 'keep', tls: 0, protocol: 'SV1' };
}

/** The fixed confirmation statements the SWITCH POOL PROFILE dialog must show. */
export const SWITCH_CONFIRM_STATEMENTS: readonly string[] = [
  'Mining will be interrupted briefly while the device applies the new pool settings.',
  'The primary and fallback pool settings on the device will change to this profile.',
  'The miner will reconnect and may restart to apply the change.',
  'The switch can fail — the target pool may be unreachable or reject the worker.',
  'Keep this dashboard open and connected until the switch AND its verification finish. Automatic rollback runs only while this session is open — a browser close, refresh, sleep or network drop interrupts it and it will NOT auto-complete.',
  'If reconnect or mining cannot be verified (while the dashboard stays open), NeuralAxe automatically rolls back to your captured original configuration.',
  'This changes which pool the miner connects to. It does not convert, move or transfer any funds.',
  'BTC and BCH profiles may require different payout / account details — make sure this profile is correct for its chain.',
];
