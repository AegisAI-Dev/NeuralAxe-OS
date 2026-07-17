import { SystemInfo as ISystemInfo } from 'src/app/generated/models';

/**
 * Safe pool switching (Stage 2G).
 *
 * The firmware already supports a user-chosen active pool via the existing
 * `useFallbackStratum` NVS setting (REST key on PATCH /api/system):
 *  - true:  the device boots onto the FALLBACK pool and never auto-returns
 *           to the primary (protocol_coordinator disables the heartbeat
 *           probe for a user choice);
 *  - false: normal behavior — primary preferred, automatic failover intact.
 *
 * Switching therefore flips WHICH configured pool is preferred instead of
 * rewriting the two configurations into each other. This is deliberate:
 * pool passwords are write-only through the API (never returned by
 * /api/system/info), so a literal value swap could not carry them over and
 * would corrupt credentials. Flipping the preference preserves every
 * configured field of both pools — host, port, worker, password, TLS,
 * certificates, protocol, SV2 keys, difficulty, extranonce, coinbase
 * decoding — because none of them are touched.
 *
 * The switch uses the existing save semantics (PATCH /api/system) and the
 * existing restart endpoint; automatic failover logic is not modified and
 * no new firmware endpoint is introduced.
 */

export type PoolRole = 'primary' | 'fallback';

/** Display-safe endpoint summary (no credentials). */
export interface PoolEndpointView {
  role: PoolRole;
  host: string;
  port: number | null;
  /** Raw worker string — must be rendered through the address mask in templates. */
  worker: string;
}

export interface PoolSwitchGate {
  allowed: boolean;
  /** User-facing explanation when the switch is not allowed. */
  reason: string | null;
}

export interface PoolSwitchPlan {
  /** Pool the device is actively mining on right now. */
  activeNow: PoolEndpointView;
  standbyNow: PoolEndpointView;
  /** Pool that will be preferred (and mined) after the restart. */
  activeAfter: PoolEndpointView;
  standbyAfter: PoolEndpointView;
  /** Exact PATCH body — only the preference flag, nothing else is written. */
  patch: { useFallbackStratum: boolean };
}

function str(value: unknown): string {
  return typeof value === 'string' ? value.trim() : '';
}

function portOf(value: unknown): number | null {
  return typeof value === 'number' && isFinite(value) && value >= 0 && value <= 65535
    ? Math.floor(value)
    : null;
}

export function endpointView(info: ISystemInfo, role: PoolRole): PoolEndpointView {
  const prefix = role === 'primary' ? 'stratum' : 'fallbackStratum';
  const record = info as unknown as { [key: string]: unknown };
  return {
    role,
    host: str(record[`${prefix}URL`]),
    port: portOf(record[`${prefix}Port`]),
    worker: str(record[`${prefix}User`]),
  };
}

/** A pool configuration usable as an active mining target. */
export function poolConfigValid(view: PoolEndpointView): boolean {
  return view.host !== '' && view.port !== null && view.worker !== '';
}

/** Primary and fallback point at the same endpoint with the same worker. */
export function poolsEquivalent(info: ISystemInfo): boolean {
  const primary = endpointView(info, 'primary');
  const fallback = endpointView(info, 'fallback');
  return primary.host.toLowerCase() === fallback.host.toLowerCase()
    && primary.port === fallback.port
    && primary.worker === fallback.worker;
}

/**
 * Whether switching is currently allowed, with a user-facing reason when not.
 * `formDirty` blocks switching while unsaved pool edits exist — the dialog
 * reflects the configuration stored on the device, not the unsaved form.
 */
export function poolSwitchGate(info: ISystemInfo, formDirty: boolean): PoolSwitchGate {
  const primary = endpointView(info, 'primary');
  const fallback = endpointView(info, 'fallback');

  if (!poolConfigValid(fallback)) {
    return { allowed: false, reason: 'No usable fallback pool is configured (host, port and worker are required).' };
  }
  if (!poolConfigValid(primary)) {
    return { allowed: false, reason: 'The primary pool configuration is incomplete (host, port and worker are required).' };
  }
  if (poolsEquivalent(info)) {
    return { allowed: false, reason: 'Primary and fallback pools are identical — switching would change nothing.' };
  }
  if (formDirty) {
    return { allowed: false, reason: 'Save or revert the unsaved pool edits below first — switching applies to the configuration stored on the device.' };
  }
  return { allowed: true, reason: null };
}

/**
 * Build the switch plan from the device's CURRENT state: whichever pool is
 * actively mining now becomes standby, and the other becomes the preferred
 * pool after restart. The patch writes only `useFallbackStratum`.
 */
export function poolSwitchPlan(info: ISystemInfo): PoolSwitchPlan {
  const primary = endpointView(info, 'primary');
  const fallback = endpointView(info, 'fallback');
  const fallbackActiveNow = !!info.isUsingFallbackStratum;

  const activeNow = fallbackActiveNow ? fallback : primary;
  const standbyNow = fallbackActiveNow ? primary : fallback;

  return {
    activeNow,
    standbyNow,
    activeAfter: standbyNow,
    standbyAfter: activeNow,
    patch: { useFallbackStratum: !fallbackActiveNow },
  };
}
