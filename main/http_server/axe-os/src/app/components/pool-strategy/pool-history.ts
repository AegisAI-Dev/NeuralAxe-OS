/**
 * NeuralAxe Pool Strategy Center — sanitized history, restore snapshot & export
 * (Phase 2M, Stages 8/9/13).
 *
 * History lives in localStorage (existing convention) and is bounded. It stores
 * ONLY: session id, timestamps, source/target profile id + name, chain labels,
 * MASKED host/port changes, the state timeline, verification result, rollback
 * result and failure reason. It NEVER stores a password, full wallet/account,
 * worker, SSID, Wi-Fi password, miner IP or a raw API response — proven by a
 * forbidden-key guard.
 *
 * The restore snapshot (the captured original configuration) is held under its
 * own bounded, expiring key. It carries the fields needed to reapply the original
 * pools — but never a password (write-only on the device): a restore keeps the
 * device's current password, which the dialog states honestly.
 */

import { SystemInfo as ISystemInfo, Settings } from 'src/app/generated/models';
import { PoolChain, PoolEndpoint, chainShort, endpointFromInfo, maskHost, normalizePort, realSecret } from './pool-profile';
import { RecoveryResult, SwitchState, SwitchTimelineEntry } from './pool-switch-machine';

export const HISTORY_KEY = 'NX_POOL_SWITCH_HISTORY';
export const RESTORE_KEY = 'NX_POOL_RESTORE_SNAPSHOT';
export const MAX_RECORDS = 20;
/** Retained restore snapshot lifetime — a documented safe expiry (24 h). */
export const RESTORE_TTL_MS = 24 * 60 * 60 * 1000;

export const REPORT_DISCLAIMER =
  'This report describes NeuralAxe Pool Strategy Center switch sessions on this browser. '
  + 'Chain labels describe the selected pool profiles; NeuralAxe does not cryptographically '
  + 'determine which chain a pool mines. No wallet, worker or password values are included.';

// ---------------------------------------------------------------------------
// Captured original configuration + restore snapshot
// ---------------------------------------------------------------------------

/** The device pool configuration captured before a switch, for rollback / restore. */
export interface CapturedConfig {
  primary: PoolEndpoint;
  fallback: PoolEndpoint;
  /** Original preferred-pool flag, restored verbatim. */
  useFallbackStratum: number;
}

export interface RestoreSnapshot {
  config: CapturedConfig;
  capturedAt: number;
  expiresAt: number;
  /** The profile that had been switched TO (what is being undone). */
  fromProfileName: string | null;
  /** True when the switch set a new password (so restore cannot recover the old one). */
  passwordWasReplaced: boolean;
}

/** Capture the device's current pool configuration (passwords are never read). */
export function captureConfig(info: ISystemInfo): CapturedConfig {
  return {
    primary: endpointFromInfo(info, 'primary'),
    fallback: endpointFromInfo(info, 'fallback'),
    useFallbackStratum: typeof info.isUsingFallbackStratum === 'number' ? info.isUsingFallbackStratum : 0,
  };
}

/**
 * Session-only plan for reinstating original passwords during an in-session
 * rollback. `originalPassword` is held in component memory for the active
 * operation only and is never persisted. Manual restore after success passes an
 * empty plan (the original secret is gone), so the device keeps its current
 * password and the UI says so.
 */
export interface RestoreSecretPlan {
  originalPassword?: string;
  /** Reapply the original password to the primary pool (it was replaced). */
  restorePrimary?: boolean;
  /** Reapply the original password to the fallback pool (it was replaced). */
  restoreFallback?: boolean;
}

/**
 * Build the PATCH body that restores a captured configuration. By default
 * passwords are OMITTED (the device keeps its current password). A password is
 * written back ONLY when the caller supplies a real session `originalPassword`
 * AND flags the pool(s) whose password the switch had replaced — this is how an
 * in-session rollback fully recovers the original credentials. An empty/masked
 * value is never sent. The original preferred-pool flag is restored verbatim.
 */
export function captureToSettings(config: CapturedConfig, plan: RestoreSecretPlan = {}): Settings {
  const p = config.primary;
  const f = config.fallback;
  const body = {
    stratumURL: p.host,
    stratumPort: normalizePort(p.port) ?? 0,
    stratumUser: p.user,
    stratumProtocol: p.protocol,
    stratumTLS: p.tls,
    fallbackStratumURL: f.host,
    fallbackStratumPort: normalizePort(f.port) ?? 0,
    fallbackStratumUser: f.user,
    fallbackStratumProtocol: f.protocol,
    fallbackStratumTLS: f.tls,
    useFallbackStratum: config.useFallbackStratum,
  } as Settings;
  const secret = realSecret(plan.originalPassword);
  if (secret !== null && plan.restorePrimary) {
    (body as any).stratumPassword = secret;
  }
  if (secret !== null && plan.restoreFallback) {
    (body as any).fallbackStratumPassword = secret;
  }
  return body;
}

export function buildRestoreSnapshot(input: {
  config: CapturedConfig;
  now: number;
  fromProfileName: string | null;
  passwordWasReplaced: boolean;
  ttlMs?: number;
}): RestoreSnapshot {
  const ttl = typeof input.ttlMs === 'number' && input.ttlMs > 0 ? input.ttlMs : RESTORE_TTL_MS;
  return {
    config: input.config,
    capturedAt: input.now,
    expiresAt: input.now + ttl,
    fromProfileName: input.fromProfileName,
    passwordWasReplaced: input.passwordWasReplaced,
  };
}

export function isRestoreValid(snapshot: RestoreSnapshot | null | undefined, now: number): boolean {
  return !!snapshot && !!snapshot.config && typeof snapshot.expiresAt === 'number' && snapshot.expiresAt > now;
}

// ---------------------------------------------------------------------------
// Switch history record (sanitized)
// ---------------------------------------------------------------------------

export interface MaskedHostChange {
  role: 'primary' | 'fallback';
  hostFromMasked: string;
  hostToMasked: string;
  portFrom: number | null;
  portTo: number | null;
  changed: boolean;
}

export interface SwitchRecordDevice {
  productName?: string;
  productVersion?: string;
  targetDevice?: string;
  targetBoard?: string;
  targetAsic?: string;
  firmware?: string;
}

export interface PoolSwitchRecord {
  id: string;
  startedAt: number;
  finishedAt: number;
  sourceProfileId: string | null;
  sourceProfileName: string | null;
  sourceChain: string;   // short label: BTC / BCH / Custom / Unknown
  targetProfileId: string | null;
  targetProfileName: string | null;
  targetChain: string;
  changes: MaskedHostChange[];
  /** No values — only whether a credential replacement was requested. */
  credentialsReplaced: { primary: boolean; fallback: boolean };
  finalState: SwitchState;
  switchVerified: boolean;
  rollbackResult: RecoveryResult | null;
  restoreResult: RecoveryResult | null;
  reason: string | null;
  timeline: SwitchTimelineEntry[];
  device?: SwitchRecordDevice;
}

/** A masked host/port change row from two endpoints. */
export function maskedHostChange(role: 'primary' | 'fallback', from: PoolEndpoint, to: PoolEndpoint): MaskedHostChange {
  const portFrom = normalizePort(from.port);
  const portTo = normalizePort(to.port);
  const changed = (from.host ?? '').trim().toLowerCase() !== (to.host ?? '').trim().toLowerCase() || portFrom !== portTo;
  return {
    role,
    hostFromMasked: maskHost(from.host),
    hostToMasked: maskHost(to.host),
    portFrom,
    portTo,
    changed,
  };
}

/**
 * Keys that must NEVER appear anywhere in a stored / exported record. Note it
 * deliberately does NOT forbid `host` — only MASKED pool hosts are ever stored.
 */
const FORBIDDEN_KEY_RE = /(ip(v4|v6)?|ssid|wifi|pass(word)?|wallet|worker|user|stratum|account|secret|cert|pubkey|mac)/i;

/** Deep-scan for any forbidden identifier key (test/guard). */
export function containsForbiddenKeys(value: unknown): boolean {
  if (Array.isArray(value)) {
    return value.some(containsForbiddenKeys);
  }
  if (value && typeof value === 'object') {
    for (const [key, child] of Object.entries(value)) {
      if (FORBIDDEN_KEY_RE.test(key)) return true;
      if (containsForbiddenKeys(child)) return true;
    }
  }
  return false;
}

export function buildSwitchRecord(input: {
  id: string;
  startedAt: number;
  finishedAt: number;
  source: { profileId: string | null; profileName: string | null; chain: PoolChain | 'unknown' };
  target: { profileId: string | null; profileName: string | null; chain: PoolChain };
  changes: MaskedHostChange[];
  credentialsReplaced: { primary: boolean; fallback: boolean };
  finalState: SwitchState;
  switchVerified: boolean;
  rollbackResult: RecoveryResult | null;
  restoreResult: RecoveryResult | null;
  reason: string | null;
  timeline: SwitchTimelineEntry[];
  device?: SwitchRecordDevice;
}): PoolSwitchRecord {
  return {
    id: input.id,
    startedAt: input.startedAt,
    finishedAt: input.finishedAt,
    sourceProfileId: input.source.profileId,
    sourceProfileName: input.source.profileName,
    sourceChain: input.source.chain === 'unknown' ? 'Unknown' : chainShort(input.source.chain),
    targetProfileId: input.target.profileId,
    targetProfileName: input.target.profileName,
    targetChain: chainShort(input.target.chain),
    changes: input.changes.map(c => ({ ...c })),
    credentialsReplaced: { ...input.credentialsReplaced },
    finalState: input.finalState,
    switchVerified: input.switchVerified,
    rollbackResult: input.rollbackResult,
    restoreResult: input.restoreResult,
    reason: input.reason,
    timeline: input.timeline.map(t => ({ ...t })),
    device: input.device ? { ...input.device } : undefined,
  };
}

export function addRecord(list: PoolSwitchRecord[], record: PoolSwitchRecord, max: number = MAX_RECORDS): PoolSwitchRecord[] {
  const next = [record, ...(Array.isArray(list) ? list.filter(r => r.id !== record.id) : [])];
  return next.slice(0, Math.max(1, max));
}

export function removeRecord(list: PoolSwitchRecord[], id: string): PoolSwitchRecord[] {
  return (Array.isArray(list) ? list : []).filter(r => r.id !== id);
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

export function exportJson(records: PoolSwitchRecord[]): string {
  return JSON.stringify({ records, disclaimer: REPORT_DISCLAIMER }, null, 2);
}

function stateResult(record: PoolSwitchRecord): string {
  switch (record.finalState) {
    case 'complete': return record.restoreResult ? 'restored' : 'switched (verified)';
    case 'failed': return `failed${record.rollbackResult ? ` — rollback ${record.rollbackResult}` : ''}`;
    case 'aborted': return 'aborted (no change)';
    case 'interrupted': return 'interrupted';
    default: return record.finalState;
  }
}

export function exportMarkdown(records: PoolSwitchRecord[]): string {
  const lines: string[] = [];
  lines.push('# NeuralAxe Pool Strategy Center — switch history', '');
  if (records.length === 0) {
    lines.push('_No switch sessions recorded._', '');
  }
  records.forEach(r => {
    lines.push(`## ${new Date(r.startedAt).toISOString()} — ${r.sourceChain} → ${r.targetChain}`);
    lines.push(`- **Session:** ${r.id}`);
    lines.push(`- **From:** ${r.sourceProfileName ?? '—'} (${r.sourceChain})`);
    lines.push(`- **To:** ${r.targetProfileName ?? '—'} (${r.targetChain})`);
    lines.push(`- **Outcome:** ${stateResult(r)}`);
    lines.push(`- **Verified:** ${r.switchVerified ? 'yes' : 'no'}`);
    if (r.reason) lines.push(`- **Reason:** ${r.reason}`);
    r.changes.filter(c => c.changed).forEach(c => {
      lines.push(`- **${c.role} pool:** ${c.hostFromMasked}:${c.portFrom ?? '—'} → ${c.hostToMasked}:${c.portTo ?? '—'}`);
    });
    if (r.device) {
      lines.push(`- **Device:** ${r.device.productName ?? '—'} · ${r.device.targetDevice ?? '—'} / board ${r.device.targetBoard ?? '—'} / ${r.device.targetAsic ?? '—'} · fw ${r.device.firmware ?? '—'}`);
    }
    lines.push('');
  });
  lines.push(`> ${REPORT_DISCLAIMER}`, '');
  return lines.join('\n');
}

// ---------------------------------------------------------------------------
// Store wrappers (thin; use the existing LocalStorageService contract)
// ---------------------------------------------------------------------------

export interface ObjectStore {
  getObject(key: string): any | null;
  setObject(key: string, value: object): void;
}

export class PoolHistoryStore {
  constructor(private storage: ObjectStore) {}

  list(): PoolSwitchRecord[] {
    const raw = this.storage.getObject(HISTORY_KEY);
    return Array.isArray(raw) ? raw : [];
  }

  save(record: PoolSwitchRecord): PoolSwitchRecord[] {
    const next = addRecord(this.list(), record);
    this.storage.setObject(HISTORY_KEY, next);
    return next;
  }

  remove(id: string): PoolSwitchRecord[] {
    const next = removeRecord(this.list(), id);
    this.storage.setObject(HISTORY_KEY, next);
    return next;
  }

  clear(): void {
    this.storage.setObject(HISTORY_KEY, []);
  }
}
