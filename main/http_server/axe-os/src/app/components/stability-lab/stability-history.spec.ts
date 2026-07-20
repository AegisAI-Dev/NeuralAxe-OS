import {
  HISTORY_KEY,
  MAX_SESSIONS,
  REPORT_DISCLAIMER,
  StabilitySessionRecord,
  StoredProfile,
  buildSessionRecord,
  addSession,
  removeSession,
  sanitizeForExport,
  containsForbiddenKeys,
  exportJson,
  exportCsv,
  exportMarkdown,
  StabilityHistoryStore,
  ObjectStore,
  normalizeRecord,
  normalizeResult,
} from './stability-history';
import { TuningConfig } from './stability-profile';
import { computeProfileResult, ProfileRun } from './stability-results';
import { LabSample, SAMPLE_INTERVAL_MS } from './stability-telemetry';
import { initialSnapshot } from './stability-machine';
import { defaultStopThresholds } from './stability-stop';

const original: TuningConfig = { frequency: 485, coreVoltage: 1200, thermalControlMode: 'target', temptarget: 60, minFanSpeed: 25 };

function sampleResult() {
  let clock = 0;
  const s = (hr: number): LabSample => {
    clock += SAMPLE_INTERVAL_MS;
    return { tMs: clock, phase: 'measure', profileIndex: 0, hashRate: hr, expectedHashrate: 1275, power: 21, efficiency: 16, asicTemp: 60, vrmTemp: 45, requestedFan: 50, appliedFan: 55, rpm: 15000, errorPercentage: 0.5, sharesAccepted: 1000, sharesRejected: 5, poolLatency: 30, thermalControlMode: 'target', controlTemp: 60, emergencyOverride: false, sensorValid: true, miningPaused: false, gap: false };
  };
  const run: ProfileRun = {
    profileId: 'p1', profileName: 'Balanced', thermalControlMode: 'target',
    warmupSamples: [], measureSamples: [s(1200), s(1300), s(1250)],
    requestedMeasureMs: 3 * SAMPLE_INTERVAL_MS, measuredMeasureMs: 3 * SAMPLE_INTERVAL_MS,
    restartOccurred: false, countersReset: false, ranFullWindow: true, aborted: false, failed: false,
  };
  return computeProfileResult(run);
}

const storedProfile: StoredProfile = {
  name: 'Balanced', config: original, warmupSec: 180, measureSec: 1200, cooldownSec: 0, notes: 'baseline reference',
};

function makeRecord(id = 'sess-1'): StabilitySessionRecord {
  return buildSessionRecord({
    id, startedAt: 1_700_000_000_000, finishedAt: 1_700_000_600_000,
    device: {
      productName: 'NeuralAxe OS', productVersion: '0.1.0-dev', targetDevice: 'Gamma', targetBoard: '601', targetAsic: 'BM1370', firmware: 'v2.14.2-31-g1c411d52',
      // Attempted leaks that MUST be dropped by buildSessionRecord's whitelist:
      ...( { ipv4: '10.0.0.5', ssid: 'HomeNet', stratumUser: 'wallet.worker' } as any ),
    },
    hostname: 'gamma-lab-01',
    original,
    profiles: [storedProfile],
    thresholds: defaultStopThresholds(),
    results: [sampleResult()],
    timeline: initialSnapshot(1).timeline,
    finalState: 'complete',
    reason: null,
    restoreResult: 'ok',
  });
}

describe('buildSessionRecord (sanitization)', () => {
  it('keeps only build-target device fields and never IP/SSID/pool/worker', () => {
    const record = makeRecord();
    expect(record.device.targetBoard).toBe('601');
    expect((record.device as any).ipv4).toBeUndefined();
    expect((record.device as any).ssid).toBeUndefined();
    expect((record.device as any).stratumUser).toBeUndefined();
    expect(containsForbiddenKeys(record)).toBeFalse();
  });

  it('retains the hostname as the only identifier (never an IP)', () => {
    expect(makeRecord().hostname).toBe('gamma-lab-01');
  });

  it('strips a tuning config to supported fields only', () => {
    const record = buildSessionRecord({
      ...JSON.parse(JSON.stringify(makeRecord())),
      original: { ...original, ...( { stratumURL: 'pool.example', wifiPass: 'secret' } as any ) },
    } as any);
    expect(containsForbiddenKeys(record.original)).toBeFalse();
    expect(JSON.stringify(record)).not.toContain('pool.example');
    expect(JSON.stringify(record)).not.toContain('secret');
  });
});

describe('history bounding', () => {
  it('prepends newest and caps at MAX_SESSIONS', () => {
    let list: StabilitySessionRecord[] = [];
    for (let i = 0; i < MAX_SESSIONS + 5; i++) {
      list = addSession(list, makeRecord(`sess-${i}`));
    }
    expect(list.length).toBe(MAX_SESSIONS);
    expect(list[0].id).toBe(`sess-${MAX_SESSIONS + 4}`); // newest first
  });

  it('replaces a record with the same id rather than duplicating', () => {
    let list = addSession([], makeRecord('dup'));
    list = addSession(list, makeRecord('dup'));
    expect(list.length).toBe(1);
  });

  it('removes by id', () => {
    const list = addSession(addSession([], makeRecord('a')), makeRecord('b'));
    expect(removeSession(list, 'a').map(r => r.id)).toEqual(['b']);
  });
});

describe('privacy mode', () => {
  it('removes the hostname from exports under privacy mode', () => {
    const record = makeRecord();
    expect(sanitizeForExport(record, true).hostname).toBeUndefined();
    expect(sanitizeForExport(record, false).hostname).toBe('gamma-lab-01');
    expect(exportMarkdown(record, true)).not.toContain('gamma-lab-01');
    expect(exportJson(record, true)).not.toContain('gamma-lab-01');
  });
});

describe('export formats', () => {
  it('JSON export carries the disclaimer and no forbidden keys', () => {
    const json = exportJson(makeRecord(), false);
    expect(json).toContain(REPORT_DISCLAIMER);
    expect(json).not.toMatch(/ipv4|ssid|wallet|stratum/i);
  });

  it('CSV export has one row per profile with a header', () => {
    const csv = exportCsv(makeRecord());
    const lines = csv.split('\n');
    expect(lines[0]).toContain('profile');
    expect(lines[0]).toContain('avgHashrate_GHs');
    expect(lines.length).toBe(2); // header + 1 profile
  });

  it('Markdown export includes plan, thresholds, results and the required disclaimer', () => {
    const md = exportMarkdown(makeRecord(), false);
    expect(md).toContain('# NeuralAxe Stability Lab session report');
    expect(md).toContain('## Session plan');
    expect(md).toContain('## Stop thresholds');
    expect(md).toContain('## Results');
    expect(md).toContain('## Limitations');
    expect(md).toContain(REPORT_DISCLAIMER);
    expect(md).toContain('board 601');
  });
});

describe('Phase 2K.1 evidence + legacy migration', () => {
  it('CSV export includes the coverage/visibility evidence columns', () => {
    const header = exportCsv(makeRecord()).split('\n')[0];
    for (const col of ['statusReason', 'expectedSamples', 'coverage_pct', 'cadence_s', 'medianInterval_s', 'maxGap_s', 'totalGap_s', 'visInterruptions', 'totalHidden_s', 'hiddenDuringMeasure']) {
      expect(header).toContain(col);
    }
  });

  it('Markdown export includes a coverage & visibility evidence section and the wall-clock contract', () => {
    const md = exportMarkdown(makeRecord(), false);
    expect(md).toContain('## Coverage & visibility evidence');
    expect(md).toContain('Max session-duration contract');
    expect(md).toContain('valid samples · target');
  });

  it('records the sample cadence, max wall-clock and restore-verified fields', () => {
    const rec = buildSessionRecord({
      id: 's', startedAt: 1, finishedAt: 2, device: {}, original,
      profiles: [storedProfile], thresholds: defaultStopThresholds(), results: [sampleResult()],
      timeline: initialSnapshot(1).timeline, finalState: 'complete', reason: null, restoreResult: 'ok',
      sampleCadenceMs: 5000, maxSessionDurationMs: 1_800_000, restoreVerified: true,
    });
    expect(rec.sampleCadenceMs).toBe(5000);
    expect(rec.maxSessionDurationMs).toBe(1_800_000);
    expect(rec.restoreVerified).toBeTrue();
  });

  it('normalizeResult fills Phase 2K.1 fields absent from a legacy (Phase 2K) result', () => {
    const legacy: any = {
      profileId: 'p', profileName: 'Legacy', status: 'completed', thermalControlMode: 'target',
      requestedMeasureMs: 600000, measuredMeasureMs: 600000, validSamples: 100, missingSamples: 20,
      expectedSamples: 120, coveragePct: 83, restartOccurred: false, countersReset: false, abortReason: null,
      avgHashrate: 1273, medianHashrate: 1273, hashrateVariabilityPct: 1, avgPower: 21, avgEfficiency: 17,
      peakAsicTemp: 55, avgAsicTemp: 54, peakVrmTemp: 48, avgVrmTemp: 47, avgRequestedFan: 60, avgAppliedFan: 62,
      fanSaturationMs: 0, avgErrorRate: 0.4, acceptedShareDelta: 10, rejectedShareDelta: 0, rejectRatePct: null,
      poolLatencyAvg: 40, poolLatencyPeak: 45, badges: [],
    };
    const n = normalizeResult(legacy);
    expect(typeof n.statusReason).toBe('string');
    expect(n.cadenceMs).toBe(5000);
    expect(n.medianIntervalMs).toBeNull();
    expect(n.maxGapMs).toBe(0);
    expect(n.visInterruptions).toBe(0);
    expect(n.hiddenDuringMeasure).toBeFalse();
  });

  it('a legacy stored session loads safely through the store (defensive migration)', () => {
    const legacyRecord: any = {
      id: 'legacy', startedAt: 1, finishedAt: 2, device: { targetBoard: '601' }, original,
      profiles: [storedProfile], thresholds: defaultStopThresholds(),
      results: [{ profileId: 'p', profileName: 'Old', status: 'completed', validSamples: 100, expectedSamples: 120, coveragePct: 83, badges: [], abortReason: null }],
      timeline: [], finalState: 'complete', reason: null, restoreResult: 'ok',
      // no sampleCadenceMs / restoreVerified — legacy record.
    };
    const backing: ObjectStore & { data: any } = (() => {
      const data: any = { [HISTORY_KEY]: [legacyRecord] };
      return { data, getObject: (k: string) => data[k] ?? null, setObject: (k: string, v: any) => { data[k] = v; } };
    })();
    const store = new StabilityHistoryStore(backing);
    const list = store.list();
    expect(list.length).toBe(1);
    expect(list[0].restoreVerified).toBeTrue();      // derived from restoreResult 'ok'
    expect(list[0].sampleCadenceMs).toBe(5000);
    expect(typeof list[0].results[0].statusReason).toBe('string');
    expect(list[0].results[0].cadenceMs).toBe(5000);
    // Exports of a migrated legacy record do not throw.
    expect(() => exportMarkdown(list[0], false)).not.toThrow();
    expect(() => exportCsv(list[0])).not.toThrow();
  });
});

describe('StabilityHistoryStore', () => {
  function fakeStore(): ObjectStore & { data: { [k: string]: any } } {
    const data: { [k: string]: any } = {};
    return { data, getObject: (k) => data[k] ?? null, setObject: (k, v) => { data[k] = v; } };
  }

  it('saves, lists, removes and clears through the object store', () => {
    const backing = fakeStore();
    const store = new StabilityHistoryStore(backing);
    expect(store.list()).toEqual([]);
    store.save(makeRecord('a'));
    store.save(makeRecord('b'));
    expect(store.list().map(r => r.id)).toEqual(['b', 'a']);
    expect(backing.data[HISTORY_KEY].length).toBe(2);
    store.remove('a');
    expect(store.list().map(r => r.id)).toEqual(['b']);
    store.clear();
    expect(store.list()).toEqual([]);
  });
});
