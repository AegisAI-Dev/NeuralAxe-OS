/**
 * NeuralAxe Stability Lab — sanitized session history and export (Phase 2K).
 *
 * History lives in localStorage (existing frontend convention) and is bounded.
 * It NEVER stores IP, SSID, pool URL/password, wallet, worker or credentials,
 * and never stores raw API responses — only tuning/thermal configuration, stop
 * thresholds, aggregate results, the state timeline and sanitized notes. The
 * device hostname is the only identifier retained, and privacy mode removes it
 * from every export. No cloud sync — everything stays in the browser.
 */

import { TuningConfig } from './stability-profile';
import { ProfileResult } from './stability-results';
import { TimelineEntry, LabState, RestoreResult } from './stability-machine';
import { StopThresholds } from './stability-stop';

export const HISTORY_KEY = 'NX_STABILITY_SESSIONS';
export const MAX_SESSIONS = 20;

export const REPORT_DISCLAIMER =
  'This report describes one controlled NeuralAxe Stability Lab session. It does not guarantee '
  + 'permanent hardware stability, component lifetime or safe operation under different ambient conditions.';

export interface SessionDevice {
  productName?: string;
  productVersion?: string;
  targetDevice?: string;
  targetBoard?: string;
  targetAsic?: string;
  firmware?: string;
}

export interface StoredProfile {
  name: string;
  config: TuningConfig;
  warmupSec: number;
  measureSec: number;
  cooldownSec: number;
  notes?: string;
}

export interface StabilitySessionRecord {
  id: string;
  startedAt: number;
  finishedAt: number;
  device: SessionDevice;
  /** Device hostname — removed from exports under privacy mode; never an IP. */
  hostname?: string;
  original: TuningConfig;
  profiles: StoredProfile[];
  thresholds: StopThresholds;
  results: ProfileResult[];
  timeline: TimelineEntry[];
  finalState: LabState;
  reason: string | null;
  restoreResult: RestoreResult | null;
  /** Configured sample cadence (ms) for the session (Phase 2K.1). */
  sampleCadenceMs?: number;
  /** Bounded maximum session-DURATION contract (ms) — a monotonic runtime cap. */
  maxSessionDurationMs?: number;
  /** Whether restore-original was verified to have succeeded. */
  restoreVerified?: boolean;
}

/**
 * Fill in the Phase 2K.1 evidence fields a result/record may lack, so a session
 * stored by Phase 2K still loads and renders safely (defensive migration — never
 * fabricates evidence, just supplies neutral defaults for absent fields).
 */
export function normalizeResult(r: ProfileResult): ProfileResult {
  return {
    ...r,
    statusReason: typeof (r as any).statusReason === 'string' ? r.statusReason : legacyStatusReason(r),
    cadenceMs: typeof (r as any).cadenceMs === 'number' ? r.cadenceMs : 5000,
    medianIntervalMs: typeof (r as any).medianIntervalMs === 'number' ? r.medianIntervalMs : null,
    maxGapMs: typeof (r as any).maxGapMs === 'number' ? r.maxGapMs : 0,
    totalGapMs: typeof (r as any).totalGapMs === 'number' ? r.totalGapMs : 0,
    visInterruptions: typeof (r as any).visInterruptions === 'number' ? r.visInterruptions : 0,
    totalHiddenMs: typeof (r as any).totalHiddenMs === 'number' ? r.totalHiddenMs : 0,
    longestHiddenMs: typeof (r as any).longestHiddenMs === 'number' ? r.longestHiddenMs : 0,
    hiddenDuringWarmup: (r as any).hiddenDuringWarmup === true,
    hiddenDuringMeasure: (r as any).hiddenDuringMeasure === true,
  };
}

function legacyStatusReason(r: ProfileResult): string {
  const cov = typeof r.coveragePct === 'number' ? `${Math.round(r.coveragePct)}%` : '—';
  switch (r.status) {
    case 'completed': return `Completed (coverage ${cov}).`;
    case 'partial': return `Partial (coverage ${cov}).`;
    case 'aborted': return `Aborted — ${r.abortReason ?? 'stop condition'}.`;
    case 'failed': return `Failed — ${r.abortReason ?? 'run failed'}.`;
    default: return `Insufficient — ${r.validSamples} valid samples.`;
  }
}

/** Defensively normalise a stored session so legacy records load safely. */
export function normalizeRecord(record: StabilitySessionRecord): StabilitySessionRecord {
  return {
    ...record,
    results: Array.isArray(record.results) ? record.results.map(normalizeResult) : [],
    sampleCadenceMs: typeof record.sampleCadenceMs === 'number' ? record.sampleCadenceMs : 5000,
    restoreVerified: typeof record.restoreVerified === 'boolean' ? record.restoreVerified : record.restoreResult === 'ok',
  };
}

/** Keys that must NEVER appear anywhere in a stored session. */
const FORBIDDEN_KEY_RE = /(ip(v4|v6)?|ssid|wifi|pass(word)?|wallet|worker|stratum|user|cert|pubkey|mac)/i;

/**
 * Deep-scan an object for any forbidden identifier key. Used as a test/guard
 * to prove the sanitizer never lets sensitive data into history.
 */
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

function trimNotes(notes: unknown): string | undefined {
  if (typeof notes !== 'string') return undefined;
  const t = notes.trim().slice(0, 280);
  return t || undefined;
}

/** Strip a tuning config to only supported tuning/thermal fields. */
function sanitizeConfig(config: TuningConfig): TuningConfig {
  const out: TuningConfig = {
    frequency: config.frequency,
    coreVoltage: config.coreVoltage,
    thermalControlMode: config.thermalControlMode,
  };
  if (typeof config.temptarget === 'number') out.temptarget = config.temptarget;
  if (typeof config.minFanSpeed === 'number') out.minFanSpeed = config.minFanSpeed;
  if (typeof config.manualFanSpeed === 'number') out.manualFanSpeed = config.manualFanSpeed;
  if (typeof config.fanCurveHysteresis === 'number') out.fanCurveHysteresis = config.fanCurveHysteresis;
  if (Array.isArray(config.fanCurve)) out.fanCurve = config.fanCurve.map(p => ({ tempC: p.tempC, fanPercent: p.fanPercent }));
  return out;
}

/**
 * Build a stored session record from raw session inputs, keeping only allowed
 * fields. The device object drops anything that is not a build-target label; the
 * hostname is retained (the only identifier) but never an IP/SSID.
 */
export function buildSessionRecord(input: {
  id: string;
  startedAt: number;
  finishedAt: number;
  device: SessionDevice;
  hostname?: string;
  original: TuningConfig;
  profiles: StoredProfile[];
  thresholds: StopThresholds;
  results: ProfileResult[];
  timeline: TimelineEntry[];
  finalState: LabState;
  reason: string | null;
  restoreResult: RestoreResult | null;
  sampleCadenceMs?: number;
  maxSessionDurationMs?: number;
  restoreVerified?: boolean;
}): StabilitySessionRecord {
  return {
    id: input.id,
    startedAt: input.startedAt,
    finishedAt: input.finishedAt,
    device: {
      productName: input.device.productName,
      productVersion: input.device.productVersion,
      targetDevice: input.device.targetDevice,
      targetBoard: input.device.targetBoard,
      targetAsic: input.device.targetAsic,
      firmware: input.device.firmware,
    },
    hostname: typeof input.hostname === 'string' ? input.hostname : undefined,
    original: sanitizeConfig(input.original),
    profiles: input.profiles.map(p => ({
      name: p.name,
      config: sanitizeConfig(p.config),
      warmupSec: p.warmupSec,
      measureSec: p.measureSec,
      cooldownSec: p.cooldownSec,
      notes: trimNotes(p.notes),
    })),
    thresholds: { ...input.thresholds },
    results: input.results,
    timeline: input.timeline,
    finalState: input.finalState,
    reason: input.reason,
    restoreResult: input.restoreResult,
    sampleCadenceMs: typeof input.sampleCadenceMs === 'number' ? input.sampleCadenceMs : 5000,
    maxSessionDurationMs: typeof input.maxSessionDurationMs === 'number' ? input.maxSessionDurationMs : undefined,
    restoreVerified: typeof input.restoreVerified === 'boolean' ? input.restoreVerified : input.restoreResult === 'ok',
  };
}

/** Prepend a record and keep only the newest MAX_SESSIONS. */
export function addSession(list: StabilitySessionRecord[], record: StabilitySessionRecord, max: number = MAX_SESSIONS): StabilitySessionRecord[] {
  const next = [record, ...(Array.isArray(list) ? list.filter(r => r.id !== record.id) : [])];
  return next.slice(0, Math.max(1, max));
}

export function removeSession(list: StabilitySessionRecord[], id: string): StabilitySessionRecord[] {
  return (Array.isArray(list) ? list : []).filter(r => r.id !== id);
}

/** Return a privacy-safe copy: the hostname is removed under privacy mode. */
export function sanitizeForExport(record: StabilitySessionRecord, privacy: boolean): StabilitySessionRecord {
  if (!privacy) return record;
  const { hostname, ...rest } = record;
  return { ...rest, hostname: undefined };
}

// ---------------------------------------------------------------------------
// Export formats
// ---------------------------------------------------------------------------

export function exportJson(record: StabilitySessionRecord, privacy: boolean): string {
  return JSON.stringify({ ...sanitizeForExport(record, privacy), disclaimer: REPORT_DISCLAIMER }, null, 2);
}

function csvCell(value: unknown): string {
  const s = value === null || value === undefined ? '' : String(value);
  return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
}

const round = (v: number | null, d = 1): string => (v === null || !isFinite(v) ? '' : v.toFixed(d));

const secOrBlank = (ms: number | null | undefined): string =>
  typeof ms === 'number' && isFinite(ms) ? String(Math.round(ms / 1000)) : '';

/** One summary row per profile result, including Phase 2K.1 coverage evidence. */
export function exportCsv(record: StabilitySessionRecord): string {
  const rec = normalizeRecord(record);
  const header = [
    'profile', 'status', 'statusReason', 'avgHashrate_GHs', 'medianHashrate_GHs', 'variability_pct',
    'avgPower_W', 'avgEfficiency_JTH', 'peakAsic_C', 'avgAsic_C', 'peakVrm_C',
    'avgAppliedFan_pct', 'avgErrorRate_pct', 'acceptedDelta', 'rejectedDelta', 'rejectRate_pct',
    'poolLatencyAvg_ms', 'validSamples', 'expectedSamples', 'coverage_pct', 'cadence_s',
    'medianInterval_s', 'maxGap_s', 'totalGap_s', 'visInterruptions', 'totalHidden_s', 'longestHidden_s',
    'hiddenDuringMeasure', 'measuredSeconds', 'abortReason',
  ];
  const rows = rec.results.map(r => [
    csvCell(r.profileName), csvCell(r.status), csvCell(r.statusReason),
    round(r.avgHashrate), round(r.medianHashrate), round(r.hashrateVariabilityPct, 2),
    round(r.avgPower), round(r.avgEfficiency), round(r.peakAsicTemp), round(r.avgAsicTemp), round(r.peakVrmTemp),
    round(r.avgAppliedFan), round(r.avgErrorRate, 2),
    csvCell(r.acceptedShareDelta ?? ''), csvCell(r.rejectedShareDelta ?? ''), round(r.rejectRatePct, 2),
    round(r.poolLatencyAvg), csvCell(r.validSamples), csvCell(r.expectedSamples), round(r.coveragePct),
    secOrBlank(r.cadenceMs), secOrBlank(r.medianIntervalMs), secOrBlank(r.maxGapMs), secOrBlank(r.totalGapMs),
    csvCell(r.visInterruptions), secOrBlank(r.totalHiddenMs), secOrBlank(r.longestHiddenMs),
    csvCell(r.hiddenDuringMeasure ? 'yes' : 'no'),
    csvCell(Math.round(r.measuredMeasureMs / 1000)), csvCell(r.abortReason ?? ''),
  ].join(','));
  return [header.join(','), ...rows].join('\n');
}

const MODE_LABEL: { [k: string]: string } = { target: 'Target Temperature', curve: 'Fan Curve', manual: 'Manual Fan' };

function configLine(config: TuningConfig): string {
  const parts = [`${config.frequency} MHz`, `${config.coreVoltage} mV`, MODE_LABEL[config.thermalControlMode] ?? config.thermalControlMode];
  if (config.thermalControlMode === 'target' && typeof config.temptarget === 'number') parts.push(`target ${config.temptarget} °C`);
  if (config.thermalControlMode === 'manual' && typeof config.manualFanSpeed === 'number') parts.push(`fan ${config.manualFanSpeed} %`);
  if (config.thermalControlMode === 'curve' && Array.isArray(config.fanCurve)) parts.push('curve ' + config.fanCurve.map(p => `${p.tempC}°→${p.fanPercent}%`).join(' '));
  return parts.join(', ');
}

const msToMin = (ms: number | null | undefined): string =>
  typeof ms === 'number' && isFinite(ms) ? `${Math.round(ms / 60000)} min` : '—';
const msToSec = (ms: number | null | undefined): string =>
  typeof ms === 'number' && isFinite(ms) ? `${Math.round(ms / 1000)} s` : '—';

/** Human-readable Markdown report. */
export function exportMarkdown(record: StabilitySessionRecord, privacy: boolean): string {
  const rec = sanitizeForExport(normalizeRecord(record), privacy);
  const d = rec.device;
  const lines: string[] = [];
  lines.push('# NeuralAxe Stability Lab session report', '');
  lines.push(`- **Session:** ${rec.id}`);
  lines.push(`- **Started:** ${new Date(rec.startedAt).toISOString()}`);
  lines.push(`- **Finished:** ${new Date(rec.finishedAt).toISOString()}`);
  lines.push(`- **Device:** ${d.productName ?? '—'} ${d.productVersion ?? ''} · ${d.targetDevice ?? '—'} / board ${d.targetBoard ?? '—'} / ${d.targetAsic ?? '—'}`);
  lines.push(`- **Firmware:** ${d.firmware ?? '—'}`);
  if (!privacy && rec.hostname) lines.push(`- **Hostname:** ${rec.hostname}`);
  lines.push(`- **Final state:** ${rec.finalState}${rec.reason ? ` (${rec.reason})` : ''}`);
  lines.push(`- **Restore result:** ${rec.restoreResult ?? '—'} (verified: ${rec.restoreVerified ? 'yes' : 'no'})`);
  lines.push(`- **Sample cadence:** ${msToSec(rec.sampleCadenceMs)} · **Max session-duration contract:** ${msToMin(rec.maxSessionDurationMs)} (monotonic)`, '');

  lines.push('## Original configuration', '', `- ${configLine(rec.original)}`, '');

  lines.push('## Session plan', '');
  rec.profiles.forEach((p, i) => {
    lines.push(`${i + 1}. **${p.name}** — ${configLine(p.config)}`);
    lines.push(`   - warm-up ${p.warmupSec}s · measurement ${p.measureSec}s · cooldown ${p.cooldownSec}s${p.notes ? ` · notes: ${p.notes}` : ''}`);
  });
  lines.push('');

  lines.push('## Stop thresholds', '');
  lines.push(`- ASIC ≥ ${rec.thresholds.asicC} °C · VRM ≥ ${rec.thresholds.vrmC} °C · error > ${rec.thresholds.errorPct} % · reject > ${rec.thresholds.rejectPct} %`);
  lines.push(`- Debounce ${rec.thresholds.debounceSamples} samples · fan-saturation stop ${rec.thresholds.fanSaturationStop ? 'on' : 'off'}`, '');

  lines.push('## Results', '');
  lines.push('| Profile | Status | Avg HR (GH/s) | Var % | Avg W | J/TH | Peak ASIC °C | Coverage | Valid samples |');
  lines.push('| --- | --- | --- | --- | --- | --- | --- | --- | --- |');
  rec.results.forEach(r => {
    lines.push(`| ${r.profileName} | ${r.status} | ${round(r.avgHashrate)} | ${round(r.hashrateVariabilityPct, 1)} | ${round(r.avgPower)} | ${round(r.avgEfficiency)} | ${round(r.peakAsicTemp)} | ${round(r.coveragePct)}% | ${r.validSamples} valid · target ${r.expectedSamples} |`);
  });
  lines.push('');

  lines.push('## Coverage & visibility evidence', '');
  rec.results.forEach(r => {
    lines.push(`- **${r.profileName}** — ${r.statusReason}`);
    lines.push(`  - Coverage ${round(r.coveragePct)}% · ${r.validSamples} valid samples · target ${r.expectedSamples} · cadence ${msToSec(r.cadenceMs)}`);
    lines.push(`  - Median sample interval ${msToSec(r.medianIntervalMs)} · max gap ${msToSec(r.maxGapMs)} · total gap ${msToSec(r.totalGapMs)}`);
    lines.push(`  - Page hidden ${r.visInterruptions} time(s) · ${msToSec(r.totalHiddenMs)} total (longest ${msToSec(r.longestHiddenMs)}) · during measurement: ${r.hiddenDuringMeasure ? 'yes' : 'no'}`);
  });
  lines.push('');

  const aborts = rec.results.filter(r => r.status === 'aborted' || r.status === 'failed');
  if (aborts.length) {
    lines.push('## Aborted / failed runs', '');
    aborts.forEach(r => lines.push(`- **${r.profileName}**: ${r.abortReason ?? r.status}`));
    lines.push('');
  }

  lines.push('## Limitations', '');
  lines.push('- Measurement excludes warm-up samples; warm-up establishes thermal stabilization only.');
  lines.push('- Results are per-session evidence, not a lifetime stability guarantee.');
  lines.push('- Firmware hard thermal protection remained active throughout and was never replaced.');
  lines.push('', `> ${REPORT_DISCLAIMER}`, '');
  return lines.join('\n');
}

// ---------------------------------------------------------------------------
// Store wrapper (thin; uses the existing LocalStorageService contract)
// ---------------------------------------------------------------------------

export interface ObjectStore {
  getObject(key: string): any | null;
  setObject(key: string, value: object): void;
}

export class StabilityHistoryStore {
  constructor(private storage: ObjectStore) {}

  list(): StabilitySessionRecord[] {
    const raw = this.storage.getObject(HISTORY_KEY);
    // Defensively migrate legacy (Phase 2K) records so they render safely.
    return Array.isArray(raw) ? raw.map(normalizeRecord) : [];
  }

  save(record: StabilitySessionRecord): StabilitySessionRecord[] {
    const next = addSession(this.list(), record);
    this.storage.setObject(HISTORY_KEY, next);
    return next;
  }

  remove(id: string): StabilitySessionRecord[] {
    const next = removeSession(this.list(), id);
    this.storage.setObject(HISTORY_KEY, next);
    return next;
  }

  clear(): void {
    this.storage.setObject(HISTORY_KEY, []);
  }
}
