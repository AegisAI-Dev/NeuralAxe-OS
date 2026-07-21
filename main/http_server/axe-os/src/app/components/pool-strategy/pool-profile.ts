/**
 * NeuralAxe Pool Strategy Center — bounded pool-profile model (Phase 2M).
 *
 * A PoolProfile is a small, explicit, owner-authored description of ONE pool
 * configuration (primary, optional fallback) plus a deliberate chain LABEL. It
 * is orchestration data only — nothing here mines, predicts profitability or
 * infers a chain from a hostname.
 *
 * CREDENTIAL PRIVACY (the crux of this model):
 *   - The device NEVER returns pool passwords (`/api/system/info` has no
 *     password field — they are write-only). A profile therefore cannot read a
 *     password back, and a RAW PASSWORD IS NEVER PERSISTED anywhere (not in a
 *     profile, restore snapshot, history, export, timeline or log). Each
 *     endpoint carries only a `passwordMode`:
 *       - 'keep' — applying it OMITS the password field so the device keeps
 *         whatever it already holds (no secret is involved at all);
 *       - 'set'  — the owner must type the replacement AT SWITCH TIME; that
 *         value is session-only (see `SwitchSecrets`), passed to
 *         `profileToSettings` for the active operation and then discarded. The
 *         profile stores only the fact that a replacement is required.
 *   - Starters created from the current device configuration always use 'keep'
 *     (the password is genuinely unknown to the browser).
 *
 * ON-CHAIN FAILOVER SAFETY:
 *   - Applying a profile always prefers the profile's PRIMARY
 *     (`useFallbackStratum: 0`).
 *   - A profile with no explicit fallback MIRRORS its primary into the fallback
 *     slot on apply, so an automatic failover can never land on a stale pool
 *     from a different chain. This never reuses another chain's credentials.
 */

import { SystemInfo as ISystemInfo, Settings } from 'src/app/generated/models';
import { AddressPipe } from 'src/app/pipes/address.pipe';

/** Explicit chain LABEL. Never inferred from a hostname. */
export type PoolChain = 'BTC' | 'BCH' | 'custom';

/** How the profile treats the (write-only) pool password on apply. */
export type PasswordMode = 'keep' | 'set';

export type StratumProtocol = 'SV1' | 'SV2';

/** One pool endpoint — primary or fallback. No raw API responses; bounded. */
export interface PoolEndpoint {
  host: string;
  port: number | null;
  /** wallet.worker / account value — masked in every display. */
  user: string;
  /**
   * 'keep' omits the password on apply (device keeps its own); 'set' means the
   * owner will enter a replacement AT SWITCH TIME. The raw password is NEVER
   * stored on the endpoint — only this mode is persisted.
   */
  passwordMode: PasswordMode;
  /** TLS mode (0 none, 1 system cert, 2 custom CA) — repository truth. */
  tls: number;
  protocol: StratumProtocol;
}

/**
 * Session-only pool passwords for ONE active switch/restore operation. Held in
 * component memory only, passed to the PATCH builders, and discarded when the
 * operation ends. It is NEVER serialized, persisted, logged or exported.
 */
export interface SwitchSecrets {
  /** Target primary password when the primary is passwordMode 'set'. */
  primaryPassword?: string;
  /** Target fallback password when an explicit fallback is passwordMode 'set'. */
  fallbackPassword?: string;
}

export interface PoolProfile {
  id: string;
  name: string;
  chain: PoolChain;
  primary: PoolEndpoint;
  /** Optional. When absent, apply mirrors the primary for on-chain failover. */
  fallback: PoolEndpoint | null;
  notes?: string;
  createdAt: number;
  updatedAt: number;
}

export type PoolRole = 'primary' | 'fallback';

export const PROFILE_LIMITS = {
  maxProfiles: 10,
  maxNameLength: 40,
  maxNotesLength: 280,
} as const;

export const CHAIN_LABELS: { [k in PoolChain]: string } = {
  BTC: 'Bitcoin (BTC)',
  BCH: 'Bitcoin Cash (BCH)',
  custom: 'Custom / Unknown',
};

export const CHAIN_SHORT: { [k in PoolChain]: string } = {
  BTC: 'BTC',
  BCH: 'BCH',
  custom: 'Custom',
};

export function chainLabel(chain: PoolChain | null | undefined): string {
  return chain ? CHAIN_LABELS[chain] ?? CHAIN_LABELS.custom : CHAIN_LABELS.custom;
}

export function chainShort(chain: PoolChain | null | undefined): string {
  return chain ? CHAIN_SHORT[chain] ?? CHAIN_SHORT.custom : CHAIN_SHORT.custom;
}

export function isPoolChain(value: unknown): value is PoolChain {
  return value === 'BTC' || value === 'BCH' || value === 'custom';
}

// ---------------------------------------------------------------------------
// Small pure helpers
// ---------------------------------------------------------------------------

function str(value: unknown): string {
  return typeof value === 'string' ? value.trim() : '';
}

/** A port in the valid Stratum range, or null. */
export function normalizePort(value: unknown): number | null {
  const n = typeof value === 'string' && value.trim() !== '' ? Number(value) : value;
  return typeof n === 'number' && isFinite(n) && n >= 0 && n <= 65535 ? Math.floor(n) : null;
}

/** True when an endpoint has every field needed to mine on it. */
export function endpointUsable(ep: PoolEndpoint | null | undefined): boolean {
  return !!ep && str(ep.host) !== '' && normalizePort(ep.port) !== null && str(ep.user) !== '';
}

/** True when the owner supplied ANY fallback endpoint field (so it must be validated). */
export function fallbackProvided(ep: PoolEndpoint | null | undefined): boolean {
  if (!ep) return false;
  return str(ep.host) !== '' || ep.port !== null || str(ep.user) !== '';
}

/** Whether applying this endpoint will replace the device's password. */
export function endpointReplacesPassword(ep: PoolEndpoint | null | undefined): boolean {
  return !!ep && ep.passwordMode === 'set';
}

// ---------------------------------------------------------------------------
// Masking (never reveals a full wallet / worker / password anywhere)
// ---------------------------------------------------------------------------

/** Host masking reuses the shared address mask (grouped, middle-ellipsized). */
export function maskHost(host: string | null | undefined): string {
  const h = str(host);
  return h === '' ? '—' : AddressPipe.transform(h);
}

/**
 * Mask a wallet.worker / account value: keep the worker suffix (after the last
 * '.') visible for identification and mask the account with the shared address
 * mask. A value with no '.' is masked whole. Never returns the full account.
 */
export function maskAccount(user: string | null | undefined): string {
  const u = str(user);
  if (u === '') return '—';
  const dot = u.lastIndexOf('.');
  if (dot > 0 && dot < u.length - 1) {
    const account = u.slice(0, dot);
    const worker = u.slice(dot + 1);
    return `${AddressPipe.transform(account)}.${worker}`;
  }
  return AddressPipe.transform(u);
}

/** Password is never shown or stored — only the mode is reported. */
export function maskPassword(ep: PoolEndpoint | null | undefined): string {
  if (!ep) return '—';
  return ep.passwordMode === 'set' ? 'replaced at switch (entered securely)' : 'unchanged (device keeps current)';
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

/** Host must not carry a protocol prefix or an inline :port (repository rule). */
const HOST_HAS_PROTOCOL = /stratum\+(tcp|tls|ssl):\/\//i;
const HOST_HAS_PORT = /:[1-9]\d{0,4}$/;

export function validateEndpoint(ep: PoolEndpoint, role: PoolRole): string[] {
  const errors: string[] = [];
  const roleName = role === 'primary' ? 'Primary' : 'Fallback';
  const host = str(ep.host);
  if (host === '') {
    errors.push(`${roleName} host is required.`);
  } else {
    if (HOST_HAS_PROTOCOL.test(host)) errors.push(`${roleName} host must not include a stratum+tcp:// prefix.`);
    if (HOST_HAS_PORT.test(host)) errors.push(`${roleName} host must not include an inline :port — use the port field.`);
  }
  if (normalizePort(ep.port) === null) {
    errors.push(`${roleName} port must be a whole number between 0 and 65535.`);
  }
  if (str(ep.user) === '') {
    errors.push(`${roleName} user / account is required.`);
  }
  // No password check: a 'set' endpoint takes its password at switch time
  // (session-only), so nothing password-related is validated on the stored profile.
  if (ep.protocol !== 'SV1' && ep.protocol !== 'SV2') {
    errors.push(`${roleName} protocol must be SV1 or SV2.`);
  }
  return errors;
}

/** Full profile validation — bounded, explicit, no hostname→chain inference. */
export function validateProfile(profile: PoolProfile): string[] {
  const errors: string[] = [];
  const name = str(profile.name);
  if (name === '') {
    errors.push('Profile name is required.');
  } else if (name.length > PROFILE_LIMITS.maxNameLength) {
    errors.push(`Profile name must be ${PROFILE_LIMITS.maxNameLength} characters or fewer.`);
  }
  if (!isPoolChain(profile.chain)) {
    errors.push('Chain label must be BTC, BCH or Custom/Unknown.');
  }
  errors.push(...validateEndpoint(profile.primary, 'primary'));
  if (fallbackProvided(profile.fallback)) {
    errors.push(...validateEndpoint(profile.fallback as PoolEndpoint, 'fallback'));
  }
  if (typeof profile.notes === 'string' && profile.notes.length > PROFILE_LIMITS.maxNotesLength) {
    errors.push(`Notes must be ${PROFILE_LIMITS.maxNotesLength} characters or fewer.`);
  }
  return errors;
}

function sameEndpointIdentity(a: PoolEndpoint, b: PoolEndpoint): boolean {
  return str(a.host).toLowerCase() === str(b.host).toLowerCase()
    && normalizePort(a.port) === normalizePort(b.port)
    && str(a.user) === str(b.user);
}

/**
 * Duplicate detection: a profile duplicates another when the NAME collides
 * (case-insensitive) OR when the chain + primary identity (host+port+user) are
 * identical. Credentials are not compared (passwords are unknown for 'keep').
 */
export function isDuplicateProfile(profile: PoolProfile, others: PoolProfile[]): boolean {
  const name = str(profile.name).toLowerCase();
  return others.some(o => {
    if (o.id === profile.id) return false;
    if (str(o.name).toLowerCase() === name && name !== '') return true;
    return o.chain === profile.chain && sameEndpointIdentity(o.primary, profile.primary);
  });
}

/** Whether the current profile list can accept another profile. */
export function canAddProfile(count: number): boolean {
  return count < PROFILE_LIMITS.maxProfiles;
}

let profileSeq = 0;
export function nextProfileId(): string {
  profileSeq += 1;
  return `nx-pool-${Date.now().toString(36)}-${profileSeq.toString(36)}`;
}

// ---------------------------------------------------------------------------
// Device config ⇄ profile
// ---------------------------------------------------------------------------

/** Read one endpoint from live SystemInfo. Password is always unknown → 'keep'. */
export function endpointFromInfo(info: ISystemInfo, role: PoolRole): PoolEndpoint {
  const prefix = role === 'primary' ? 'stratum' : 'fallbackStratum';
  const rec = info as unknown as { [k: string]: unknown };
  const protocol = rec[`${prefix}Protocol`];
  return {
    host: str(rec[`${prefix}URL`]),
    port: normalizePort(rec[`${prefix}Port`]),
    user: str(rec[`${prefix}User`]),
    passwordMode: 'keep',
    tls: normalizePort(rec[`${prefix}TLS`]) ?? 0,
    protocol: protocol === 'SV2' ? 'SV2' : 'SV1',
  };
}

/** A non-empty string, or null (used to reject empty/masked password values). */
export function realSecret(value: unknown): string | null {
  if (typeof value !== 'string') return null;
  const v = value; // NOT trimmed — a password may legitimately contain spaces
  if (v === '') return null;
  // Defensive: never let a masked placeholder be written to the device.
  if (/^\*+$/.test(v) || /^•+/.test(v) || /\(hidden\)/i.test(v)) return null;
  return v;
}

/**
 * Build the exact PATCH body for a profile, injecting session-only passwords.
 * Applying a profile always prefers the PRIMARY (useFallbackStratum: 0). A
 * password field is written ONLY for a 'set' endpoint AND only when the caller
 * supplies a real (non-empty, non-masked) secret for it; a 'keep' endpoint (or a
 * missing secret) OMITS the field entirely so the device keeps its own password.
 * An empty string or a masked placeholder is never sent. When no explicit
 * fallback exists, the primary is mirrored (and shares the primary secret) so
 * failover stays on the same chain.
 */
export function profileToSettings(profile: PoolProfile, secrets: SwitchSecrets = {}): Settings {
  const mirrored = !fallbackProvided(profile.fallback);
  const fb = mirrored ? profile.primary : (profile.fallback as PoolEndpoint);
  const body: Settings = {
    stratumURL: str(profile.primary.host),
    stratumPort: normalizePort(profile.primary.port) ?? 0,
    stratumUser: str(profile.primary.user),
    stratumProtocol: profile.primary.protocol,
    stratumTLS: profile.primary.tls,
    fallbackStratumURL: str(fb.host),
    fallbackStratumPort: normalizePort(fb.port) ?? 0,
    fallbackStratumUser: str(fb.user),
    fallbackStratumProtocol: fb.protocol,
    fallbackStratumTLS: fb.tls,
    useFallbackStratum: 0,
  } as Settings;
  const primarySecret = realSecret(secrets.primaryPassword);
  if (profile.primary.passwordMode === 'set' && primarySecret !== null) {
    (body as any).stratumPassword = primarySecret;
  }
  // A mirrored fallback shares the primary's secret; an explicit fallback uses its own.
  const fbSecret = realSecret(mirrored ? secrets.primaryPassword : secrets.fallbackPassword);
  if (fb.passwordMode === 'set' && fbSecret !== null) {
    (body as any).fallbackStratumPassword = fbSecret;
  }
  return body;
}

/** A blank endpoint for the editor. */
export function blankEndpoint(): PoolEndpoint {
  return { host: '', port: null, user: '', passwordMode: 'keep', tls: 0, protocol: 'SV1' };
}

// ---------------------------------------------------------------------------
// Starters from the current device configuration
// ---------------------------------------------------------------------------

export interface StarterSpec {
  key: 'current-config' | 'primary-only' | 'primary-fallback';
  label: string;
  description: string;
  /** Draft profile (no id / timestamps yet) — the owner names + labels it. */
  draft: Omit<PoolProfile, 'id' | 'createdAt' | 'updatedAt'>;
}

export interface StarterBuild {
  specs: StarterSpec[];
  /** Honest reasons a starter was omitted (e.g. no fallback configured). */
  notes: string[];
}

/**
 * Build starter drafts from the current configuration. Never invents public
 * pool endpoints or credentials, and never labels a chain — the draft chain
 * defaults to 'custom' and the owner sets the real label. Passwords are always
 * 'keep' (the browser cannot read them from the device).
 */
export function buildStarterProfiles(info: ISystemInfo | null): StarterBuild {
  if (!info) {
    return { specs: [], notes: ['No device telemetry yet — starters appear once the device reports in.'] };
  }
  const primary = endpointFromInfo(info, 'primary');
  const fallback = endpointFromInfo(info, 'fallback');
  const notes: string[] = [];
  const specs: StarterSpec[] = [];

  if (!endpointUsable(primary)) {
    notes.push('The current primary pool configuration is incomplete — no starter can be built from it.');
    return { specs, notes };
  }

  const draftBase = { chain: 'custom' as PoolChain, notes: undefined };

  specs.push({
    key: 'primary-only',
    label: 'Current Primary Only',
    description: 'Capture the current primary pool. No fallback (primary is mirrored on apply).',
    draft: { ...draftBase, name: '', primary: { ...primary }, fallback: null },
  });

  const hasFallback = endpointUsable(fallback);
  if (hasFallback) {
    specs.push({
      key: 'primary-fallback',
      label: 'Current Primary + Fallback',
      description: 'Capture both the current primary and fallback pools.',
      draft: { ...draftBase, name: '', primary: { ...primary }, fallback: { ...fallback } },
    });
    specs.push({
      key: 'current-config',
      label: 'Current Configuration',
      description: 'Capture the full current pool configuration (primary + fallback).',
      draft: { ...draftBase, name: '', primary: { ...primary }, fallback: { ...fallback } },
    });
  } else {
    notes.push('No fallback pool is configured, so "Current Primary + Fallback" is unavailable.');
    specs.push({
      key: 'current-config',
      label: 'Current Configuration',
      description: 'Capture the full current pool configuration (primary only).',
      draft: { ...draftBase, name: '', primary: { ...primary }, fallback: null },
    });
  }

  notes.push('Passwords cannot be read from the device — starters keep the current password on apply unless you enter a replacement.');
  return { specs, notes };
}

/** Deep clone (structuredClone-free, for old runtimes) — profiles are small/flat. */
export function cloneProfile(profile: PoolProfile): PoolProfile {
  return {
    ...profile,
    primary: { ...profile.primary },
    fallback: profile.fallback ? { ...profile.fallback } : null,
  };
}
