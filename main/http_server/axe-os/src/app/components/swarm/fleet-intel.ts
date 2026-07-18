/**
 * Fleet Command Center intelligence (Phase 2I).
 *
 * Pure, tested derivations over the device objects the Fleet page already
 * holds (the merged /api/system/info + /api/system/asic payload per IP, plus
 * the reachability bookkeeping the component records at refresh time).
 * Nothing here talks to the network, and no value is ever invented: a metric
 * a device does not report renders as unavailable.
 *
 * Semantic severities are fixed and independent of the user accent:
 * green/cyan = normal, amber = attention/degraded, red = critical/offline.
 */

export interface FleetDevice {
  IP: string;
  /** Reachability bookkeeping written by the component's refresh handlers. */
  nxReachable?: boolean;
  nxLastSeenMs?: number;
  // Fields the fleet reads when a device reports them (never invented).
  hostname?: string;
  deviceModel?: string;
  ASICModel?: string;
  boardVersion?: string;
  asicCount?: number;
  swarmColor?: string;
  productName?: string;
  productVersion?: string;
  version?: string | number;
  axeOSVersion?: string;
  hashRate?: number;
  expectedHashrate?: number;
  power?: number;
  temp?: number;
  temp2?: number;
  vrTemp?: number;
  fanspeed?: number;
  fanrpm?: number;
  errorPercentage?: number;
  sharesAccepted?: number;
  sharesRejected?: number;
  uptimeSeconds?: number;
  bestDiff?: number;
  stratumURL?: string;
  fallbackStratumURL?: string;
  isUsingFallbackStratum?: number | null;
  responseTime?: number;
  wifiRSSI?: number;
  frequency?: number;
  coreVoltage?: number;
  coreVoltageActual?: number;
  temptarget?: number;
  minFanSpeed?: number;
  thermalControlMode?: string;
  requestedFanPercent?: number;
  appliedFanPercent?: number;
  activeCurveSegment?: number;
  hysteresisHolding?: number;
  controlSensor?: string;
  controlSensorValid?: number;
  emergencyOverrideActive?: number;
  fanCurveError?: string;
  overheat_mode?: number | null;
  miningPaused?: boolean;
  power_fault?: string | null;
  runningPartition?: string;
  resetReason?: string;
  [key: string]: any;
}

function num(value: unknown): number | null {
  return typeof value === 'number' && isFinite(value) ? value : null;
}

function positive(value: unknown): number | null {
  const v = num(value);
  return v !== null && v > 0 ? v : null;
}

/** A non-negative finite share counter, or null when unavailable/invalid. */
function counter(value: unknown): number | null {
  return typeof value === 'number' && isFinite(value) && value >= 0 ? value : null;
}

// ---------------------------------------------------------------------------
// Reachability
// ---------------------------------------------------------------------------

/**
 * A device is online when its last refresh attempt succeeded. Devices from
 * an old stored list that have not been refreshed yet (no bookkeeping) are
 * neither online nor offline — their state is unknown until the first
 * refresh completes.
 */
export function deviceOnline(device: FleetDevice): boolean | null {
  if (typeof device.nxReachable === 'boolean') {
    return device.nxReachable;
  }
  return null;
}

/** "Last seen 41 s ago" text, or null when no timestamp exists. */
export function lastSeenText(device: FleetDevice, nowMs: number = Date.now()): string | null {
  const seen = positive(device.nxLastSeenMs);
  if (seen === null) {
    return null;
  }
  const ageS = Math.max(0, Math.round((nowMs - seen) / 1000));
  if (ageS < 90) {
    return `${ageS} s ago`;
  }
  const ageMin = Math.round(ageS / 60);
  if (ageMin < 90) {
    return `${ageMin} min ago`;
  }
  return `${Math.round(ageMin / 60)} h ago`;
}

// ---------------------------------------------------------------------------
// Classification (Stage 6)
// ---------------------------------------------------------------------------

export type FleetClass = 'neuralaxe' | 'compatible' | 'unsupported' | 'unknown';

export interface FleetClassification {
  kind: FleetClass;
  label: string;
  tooltip: string;
}

/**
 * Transparent classification:
 * - neuralaxe:    supported board reporting the NeuralAxe identity fields;
 * - compatible:   reachable AxeOS/ESP-Miner device not running NeuralAxe;
 * - unsupported:  reports a board outside the release target (e.g. 702) —
 *                 monitorable, but NeuralAxe publishes no release for it;
 * - unknown:      insufficient data to classify.
 */
export function classifyDevice(device: FleetDevice): FleetClassification {
  const board = String(device?.boardVersion ?? '');
  const hasIdentity = !!device?.ASICModel || !!device?.version || !!board;

  if (!hasIdentity) {
    return {
      kind: 'unknown',
      label: 'Unknown',
      tooltip: 'Not enough data to classify this device yet — waiting for a successful refresh.',
    };
  }
  if (board && !board.startsWith('601')) {
    return {
      kind: 'unsupported',
      label: `Board ${board}`,
      tooltip: `Board ${board} is not a NeuralAxe release target (Gamma 601 only). It can be monitored and controlled as an AxeOS miner, but NeuralAxe firmware must not be installed on it.`,
    };
  }
  if (device?.productName) {
    return {
      kind: 'neuralaxe',
      label: 'NeuralAxe',
      tooltip: `${device.productName} ${device.productVersion ?? ''}`.trim() + ' — NeuralAxe-managed device',
    };
  }
  return {
    kind: 'compatible',
    label: 'AxeOS',
    tooltip: 'Compatible ESP-Miner/AxeOS device (not running NeuralAxe OS)',
  };
}

// ---------------------------------------------------------------------------
// Health (Stage 3)
// ---------------------------------------------------------------------------

export type FleetHealthState = 'healthy' | 'attention' | 'critical' | 'offline' | 'unknown';

export interface FleetHealth {
  state: FleetHealthState;
  /** Every rule that fired, most severe first — shown as the explanation. */
  reasons: string[];
}

/** Fixed thresholds shared with the Command Deck semantics. */
const ASIC_CRITICAL_C = 70;
const ASIC_ATTENTION_C = 65;
const VR_CRITICAL_C = 105;
const VR_ATTENTION_C = 85;
const FAN_SATURATION_PCT = 95;
const ERROR_RATE_ATTENTION_PCT = 2;
const REJECT_RATE_ATTENTION_PCT = 2;
/**
 * Sample-confidence contract (2I.1): the reject-rate rule only becomes
 * actionable once enough evidence exists — at least 100 total shares OR at
 * least 3 rejected shares. A single reject in a small startup sample (the
 * real pilot showed 1/26 ≈ 3.85 % at two minutes uptime) says nothing about
 * fleet health and must not raise Attention on its own. Persistent or
 * repeated rejection (≥3 rejects) is actionable at any sample size.
 */
const REJECT_MIN_TOTAL_SHARES = 100;
const REJECT_MIN_REJECTED = 3;

/**
 * Deterministic device health. Absence of telemetry is never interpreted as
 * healthy: an unreachable device is offline, and a reachable device without
 * core telemetry is unknown. Unsupported boards are NOT downgraded for being
 * unsupported — classification and health are independent.
 */
export function deviceHealth(device: FleetDevice): FleetHealth {
  const online = deviceOnline(device);
  if (online === false) {
    const seen = lastSeenText(device);
    return { state: 'offline', reasons: [seen ? `Unreachable — last seen ${seen}` : 'Unreachable'] };
  }

  const temp = positive(device.temp);
  const hashRate = num(device.hashRate);
  if (online === null || (temp === null && hashRate === null)) {
    return { state: 'unknown', reasons: ['No telemetry received yet — health cannot be assessed'] };
  }

  const critical: string[] = [];
  const attention: string[] = [];

  // Critical: genuine safety / fault states.
  if (device.overheat_mode === 1) critical.push('Overheat protection engaged');
  if (device.emergencyOverrideActive === 1) critical.push('Emergency thermal override active');
  if (device.power_fault) critical.push(`Power fault: ${device.power_fault}`);
  if (temp !== null && temp >= ASIC_CRITICAL_C) critical.push(`ASIC ${Math.round(temp)} °C — at/above the 70 °C safety line`);
  const vr = positive(device.vrTemp);
  if (vr !== null && vr >= VR_CRITICAL_C) critical.push(`VRM ${Math.round(vr)} °C — at/above the 105 °C limit`);

  // Attention: degraded but not dangerous.
  if (temp !== null && temp >= ASIC_ATTENTION_C && temp < ASIC_CRITICAL_C) attention.push(`ASIC ${Math.round(temp)} °C — within 5 °C of the safety line`);
  if (vr !== null && vr >= VR_ATTENTION_C && vr < VR_CRITICAL_C) attention.push(`VRM ${Math.round(vr)} °C elevated`);
  const fan = num(device.fanspeed);
  if (fan !== null && fan >= FAN_SATURATION_PCT) attention.push(`Fan at ${Math.round(fan)} % — near saturation`);
  const errPct = num(device.errorPercentage);
  if (errPct !== null && errPct > ERROR_RATE_ATTENTION_PCT) attention.push(`ASIC error rate ${errPct.toFixed(1)} %`);
  const rejectPct = rejectRatePct(device);
  if (rejectPct !== null && rejectPct > REJECT_RATE_ATTENTION_PCT && rejectSampleConfident(device)) {
    attention.push(`Share reject rate ${rejectPct.toFixed(1)} %`);
  }
  if (device.isUsingFallbackStratum === 1) attention.push('Mining on the fallback pool');
  if (device.miningPaused) attention.push('Mining paused');
  if (typeof device.fanCurveError === 'string' && device.fanCurveError) attention.push('Fan curve invalid — safe fallback active');
  if (device.controlSensorValid === 0) attention.push('Thermal sensor reporting invalid data');
  if (pairMismatch(device)) attention.push('Firmware and web revisions differ (may be a boot snapshot — a restart refreshes it)');

  if (critical.length) {
    return { state: 'critical', reasons: [...critical, ...attention] };
  }
  if (attention.length) {
    return { state: 'attention', reasons: attention };
  }
  return { state: 'healthy', reasons: ['All reported values within normal ranges'] };
}

/**
 * Whether the share sample is large enough for the reject-rate rule to be
 * actionable: total >= 100 shares, or >= 3 rejects at any sample size.
 */
export function rejectSampleConfident(device: FleetDevice): boolean {
  const accepted = num(device.sharesAccepted) ?? 0;
  const rejected = num(device.sharesRejected) ?? 0;
  return accepted + rejected >= REJECT_MIN_TOTAL_SHARES || rejected >= REJECT_MIN_REJECTED;
}

/**
 * Neutral note for the warming-up window: rejects exist and the naive rate
 * is above the threshold, but the sample is too small to act on. Display
 * only — never a health state.
 */
export function shareSampleNote(device: FleetDevice): string | null {
  const rejectPct = rejectRatePct(device);
  if (rejectPct === null || rejectPct <= REJECT_RATE_ATTENTION_PCT || rejectSampleConfident(device)) {
    return null;
  }
  const accepted = num(device.sharesAccepted) ?? 0;
  const rejected = num(device.sharesRejected) ?? 0;
  return `Share sample still warming up (${rejected} rejected of ${accepted + rejected} — too few shares to judge)`;
}

/** Reject percentage of all submitted shares; null before any share. */
export function rejectRatePct(device: FleetDevice): number | null {
  const accepted = num(device.sharesAccepted);
  const rejected = num(device.sharesRejected);
  if (accepted === null || rejected === null || accepted < 0 || rejected < 0) {
    return null;
  }
  const total = accepted + rejected;
  return total > 0 ? (rejected / total) * 100 : null;
}

/** Firmware vs installed-web revision comparison (only when both reported). */
export function pairMismatch(device: FleetDevice): boolean {
  const fw = device.version;
  const web = device.axeOSVersion;
  return typeof fw === 'string' && typeof web === 'string' && fw !== '' && web !== '' && fw !== web;
}

/** J/TH when the device reports both power and hashrate; null otherwise. */
export function deviceEfficiency(device: FleetDevice): number | null {
  const power = positive(device.power);
  const gh = positive(device.hashRate);
  if (power === null || gh === null) {
    return null;
  }
  return power / (gh / 1000);
}

/** Bare pool host for grouping/display; null when not reported. */
export function activePoolHost(device: FleetDevice): string | null {
  const url = device.isUsingFallbackStratum === 1 ? device.fallbackStratumURL : device.stratumURL;
  return typeof url === 'string' && url.trim() !== '' ? url.trim() : null;
}

// ---------------------------------------------------------------------------
// Fleet summary (Stage 2)
// ---------------------------------------------------------------------------

export interface FleetSummary {
  total: number;
  online: number;
  offline: number;
  pendingFirstContact: number;
  neuralaxe: number;
  compatible: number;
  unsupported: number;
  unknownClass: number;
  /** GH/s across online devices reporting hashrate. */
  totalHashRate: number;
  /** Watts across online devices reporting power. */
  totalPower: number;
  /** Devices contributing to each total (honest partial-coverage labeling). */
  hashRateDevices: number;
  powerDevices: number;
  /** J/TH from the devices reporting BOTH power and hashrate; null if none. */
  efficiency: number | null;
  attention: number;
  critical: number;
  /** Pool host -> device count, from devices that report a pool. */
  pools: Array<{ host: string; devices: number }>;
}

export function fleetSummary(devices: FleetDevice[]): FleetSummary {
  const summary: FleetSummary = {
    total: devices.length,
    online: 0, offline: 0, pendingFirstContact: 0,
    neuralaxe: 0, compatible: 0, unsupported: 0, unknownClass: 0,
    totalHashRate: 0, totalPower: 0, hashRateDevices: 0, powerDevices: 0,
    efficiency: null, attention: 0, critical: 0, pools: [],
  };

  let pairedPower = 0;
  let pairedHashGh = 0;
  const poolCounts = new Map<string, number>();

  for (const device of devices) {
    const online = deviceOnline(device);
    if (online === true) summary.online++;
    else if (online === false) summary.offline++;
    else summary.pendingFirstContact++;

    switch (classifyDevice(device).kind) {
      case 'neuralaxe': summary.neuralaxe++; break;
      case 'compatible': summary.compatible++; break;
      case 'unsupported': summary.unsupported++; break;
      default: summary.unknownClass++; break;
    }

    const health = deviceHealth(device).state;
    if (health === 'attention') summary.attention++;
    if (health === 'critical') summary.critical++;

    if (online === true) {
      const gh = positive(device.hashRate);
      const power = positive(device.power);
      if (gh !== null) { summary.totalHashRate += gh; summary.hashRateDevices++; }
      if (power !== null) { summary.totalPower += power; summary.powerDevices++; }
      if (gh !== null && power !== null) { pairedHashGh += gh; pairedPower += power; }

      const pool = activePoolHost(device);
      if (pool) poolCounts.set(pool, (poolCounts.get(pool) ?? 0) + 1);
    }
  }

  if (pairedHashGh > 0) {
    summary.efficiency = pairedPower / (pairedHashGh / 1000);
  }
  summary.pools = Array.from(poolCounts.entries())
    .map(([host, count]) => ({ host, devices: count }))
    .sort((a, b) => b.devices - a.devices || a.host.localeCompare(b.host));

  return summary;
}

// ---------------------------------------------------------------------------
// Fleet share aggregation (Phase 2I.2 Stage 2)
// ---------------------------------------------------------------------------

/**
 * Aggregated share counters across the reporting fleet.
 *
 * `accepted`/`rejected` are the sum of each device's CURRENT counters — not
 * lifetime totals. A miner's counters reset on reboot, firmware update or
 * device reset, so this is a live snapshot, not cumulative history.
 *
 * Only online devices that report BOTH counters as valid, non-negative numbers
 * contribute. A missing or invalid counter is never coerced to zero, and an
 * offline device (whose telemetry the refresh path zeroes) is never presented
 * as a live contributor.
 */
export interface FleetShares {
  /** Sum of current accepted counters across reporting devices. */
  accepted: number;
  /** Sum of current rejected counters across reporting devices. */
  rejected: number;
  /** accepted + rejected. */
  total: number;
  /** Reject rate over reported shares; null when no shares are reported yet. */
  rejectRatePct: number | null;
  /** Devices whose current counters were counted (the N in "N of M"). */
  reportingDevices: number;
  /** Whole-fleet device count for coverage labeling (the M in "N of M"). */
  totalDevices: number;
  /** True once at least one device has contributed valid counters. */
  hasData: boolean;
}

export function fleetShares(devices: FleetDevice[]): FleetShares {
  let accepted = 0;
  let rejected = 0;
  let reportingDevices = 0;

  for (const device of devices) {
    // Only online devices are live contributors; offline/stale counters
    // (which the refresh error path zeroes) are never summed as live shares.
    if (deviceOnline(device) !== true) {
      continue;
    }
    const acc = counter(device.sharesAccepted);
    const rej = counter(device.sharesRejected);
    // A device must report BOTH counters as valid non-negative numbers; a
    // missing/invalid counter excludes the device rather than being zeroed.
    if (acc === null || rej === null) {
      continue;
    }
    accepted += acc;
    rejected += rej;
    reportingDevices++;
  }

  const total = accepted + rejected;
  return {
    accepted,
    rejected,
    total,
    rejectRatePct: total > 0 ? (rejected / total) * 100 : null,
    reportingDevices,
    totalDevices: devices.length,
    hasData: reportingDevices > 0,
  };
}

export type FleetShareSeverity = 'ok' | 'attention';

/** Whether the aggregate share sample is large enough to act on its reject
 *  rate — the same contract as per-device health (>=100 total or >=3 rejects). */
function fleetShareSampleConfident(shares: FleetShares): boolean {
  return shares.total >= REJECT_MIN_TOTAL_SHARES || shares.rejected >= REJECT_MIN_REJECTED;
}

/**
 * Fleet-share tile severity. Follows the health/sample-confidence contract:
 * an elevated reject rate on a small startup sample is NOT actionable, and a
 * share reject rate is at most an Attention signal — never Critical. A
 * low-confidence startup sample therefore never makes the tile look critical.
 */
export function fleetShareSeverity(shares: FleetShares): FleetShareSeverity {
  if (shares.rejectRatePct === null || shares.rejectRatePct <= REJECT_RATE_ATTENTION_PCT) {
    return 'ok';
  }
  return fleetShareSampleConfident(shares) ? 'attention' : 'ok';
}

/** Neutral warming-up note when the aggregate reject rate is elevated but the
 *  fleet sample is still too small to judge. Display only — never a severity. */
export function fleetShareSampleNote(shares: FleetShares): string | null {
  if (shares.rejectRatePct === null || shares.rejectRatePct <= REJECT_RATE_ATTENTION_PCT || fleetShareSampleConfident(shares)) {
    return null;
  }
  return `Reject rate is over ${REJECT_RATE_ATTENTION_PCT}% but the fleet sample is still small `
    + `(${shares.rejected} of ${shares.total}) — too few shares to act on`;
}

/**
 * Compact count for tile display (12345 -> "12.3K", 2_400_000 -> "2.4M").
 * The exact integer stays available via formatExactCount for the tooltip/label.
 */
export function formatCompactCount(value: number): string {
  if (typeof value !== 'number' || !isFinite(value)) {
    return '—';
  }
  const abs = Math.abs(value);
  if (abs < 1000) {
    return String(Math.round(value));
  }
  const units: Array<{ limit: number; suffix: string }> = [
    { limit: 1e12, suffix: 'T' },
    { limit: 1e9, suffix: 'B' },
    { limit: 1e6, suffix: 'M' },
    { limit: 1e3, suffix: 'K' },
  ];
  for (const { limit, suffix } of units) {
    if (abs >= limit) {
      const scaled = value / limit;
      const text = Math.abs(scaled) >= 100 ? Math.round(scaled).toString() : scaled.toFixed(1).replace(/\.0$/, '');
      return `${text}${suffix}`;
    }
  }
  return String(Math.round(value));
}

/** Exact grouped integer for the tooltip / accessible label. */
export function formatExactCount(value: number): string {
  return typeof value === 'number' && isFinite(value) ? Math.round(value).toLocaleString('en-US') : '—';
}

// ---------------------------------------------------------------------------
// Filters and sorting (Stage 7)
// ---------------------------------------------------------------------------

export interface FleetFilters {
  text: string;
  health: FleetHealthState | 'all';
  classification: FleetClass | 'all';
  online: 'all' | 'online' | 'offline';
  pool: string | 'all';
}

export const DEFAULT_FLEET_FILTERS: FleetFilters = {
  text: '', health: 'all', classification: 'all', online: 'all', pool: 'all',
};

export function filterDevices(devices: FleetDevice[], filters: FleetFilters): FleetDevice[] {
  const text = (filters.text || '').toLowerCase();
  return devices.filter(device => {
    if (text) {
      const haystack = [device.hostname, device.deviceModel, device.ASICModel, device.IP, device.boardVersion]
        .filter((part): part is string => typeof part === 'string')
        .join(' ')
        .toLowerCase();
      if (!haystack.includes(text)) return false;
    }
    if (filters.health !== 'all' && deviceHealth(device).state !== filters.health) return false;
    if (filters.classification !== 'all' && classifyDevice(device).kind !== filters.classification) return false;
    if (filters.online === 'online' && deviceOnline(device) !== true) return false;
    if (filters.online === 'offline' && deviceOnline(device) !== false) return false;
    if (filters.pool !== 'all' && activePoolHost(device) !== filters.pool) return false;
    return true;
  });
}

/** Sortable fields; derived metrics sort by their derived value. */
export type FleetSortField =
  | 'hostname' | 'IP' | 'hashRate' | 'efficiency' | 'temp' | 'errorPercentage'
  | 'power' | 'uptimeSeconds' | 'sharesAccepted' | 'version' | 'health';

const HEALTH_ORDER: { [key in FleetHealthState]: number } = {
  critical: 0, attention: 1, offline: 2, unknown: 3, healthy: 4,
};

export function compareDevices(a: FleetDevice, b: FleetDevice, field: FleetSortField, direction: 'asc' | 'desc'): number {
  let comparison = 0;
  if (field === 'IP') {
    const aOct = String(a.IP ?? '').split('.').map(Number);
    const bOct = String(b.IP ?? '').split('.').map(Number);
    for (let i = 0; i < 4 && comparison === 0; i++) {
      comparison = (aOct[i] ?? 0) - (bOct[i] ?? 0);
    }
  } else if (field === 'efficiency') {
    // null efficiency sorts to the end in both directions
    const ae = deviceEfficiency(a);
    const be = deviceEfficiency(b);
    if (ae === null && be === null) comparison = 0;
    else if (ae === null) return 1;
    else if (be === null) return -1;
    else comparison = ae - be;
  } else if (field === 'health') {
    comparison = HEALTH_ORDER[deviceHealth(a).state] - HEALTH_ORDER[deviceHealth(b).state];
  } else {
    const av = a[field];
    const bv = b[field];
    if (typeof av === 'number' && typeof bv === 'number') comparison = av - bv;
    else comparison = String(av ?? '').localeCompare(String(bv ?? ''), undefined, { numeric: true });
  }
  return direction === 'asc' ? comparison : -comparison;
}
