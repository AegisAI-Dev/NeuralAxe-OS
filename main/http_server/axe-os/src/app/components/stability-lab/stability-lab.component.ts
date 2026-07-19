import { Component, HostListener, OnDestroy, OnInit } from '@angular/core';
import { HttpErrorResponse } from '@angular/common/http';
import { Observable, Subject, Subscription, first, interval, takeUntil } from 'rxjs';
import { ToastrService } from 'ngx-toastr';

import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { LiveDataService } from 'src/app/services/live-data.service';
import { SystemApiService } from 'src/app/services/system.service';
import { WebVersionService } from 'src/app/services/web-version.service';
import { LocalStorageService } from 'src/app/local-storage.service';
import { SensitiveData } from 'src/app/services/sensitive-data.service';
import { AddressPipe } from 'src/app/pipes/address.pipe';
import { derivePairStatus, PairStatus } from 'src/app/services/version-state';
import { THERMAL_PROFILES, ThermalProfile, FanCurvePoint, ThermalControlMode, fanCurveSummary } from '../edit/tuning';

import {
  TuningConfig, StabilityProfile, StarterProfileSpec, DURATION_BOUNDS, DEFAULT_DURATIONS, PROFILE_LIMITS,
  captureBaseline, validateProfile, isDuplicateProfile, profileDiff, profileToSettings,
  buildStarterProfiles, totalSessionSeconds, nextProfileId, outsideOptionList,
  RestartAuditRow, restartAudit, profileRestartRequired, sessionRestartCount,
} from './stability-profile';
import {
  Freshness, FRESHNESS_LIMIT_MS, SESSION_RECONNECT_GRACE_MS, HEARTBEAT_MS,
  evaluateFreshness, shouldAbortForStaleness, reconnectGraceRemainingMs,
} from './stability-freshness';
import {
  SupportedDeviceCheck, PreflightResult, STOP_THRESHOLD_BOUNDS,
  supportedDevice, preflight, clampAsicStop, clampVrmStop,
} from './stability-preflight';
import {
  LabSnapshot, LabEventType, TimelineEntry, initialSnapshot, reduce, stateLabel, stateExplanation, isActive, isTerminal,
} from './stability-machine';
import {
  LabSample, buildSample, shareDelta,
} from './stability-telemetry';
import {
  StopThresholds, DebounceState, STOP_TUNABLES, defaultStopThresholds, clampThresholds, zeroDebounce, evaluateSample, StopReason,
} from './stability-stop';
import {
  ProfileRun, ProfileResult, computeProfileResult, applyComparativeBadges, completionStatement, isPromotable,
} from './stability-results';
import {
  StabilitySessionRecord, StabilityHistoryStore, buildSessionRecord, StoredProfile, exportJson, exportCsv, exportMarkdown,
} from './stability-history';

/** localStorage flag proving a live run was in progress (interruption detection). */
const ACTIVE_FLAG = 'NX_STABILITY_ACTIVE';

type EditorModel = {
  name: string;
  frequency: number | null;
  coreVoltage: number | null;
  thermalControlMode: ThermalControlMode;
  temptarget: number;
  minFanSpeed: number;
  manualFanSpeed: number;
  fanCurve: FanCurvePoint[];
  fanCurveHysteresis: number;
  warmupSec: number;
  measureSec: number;
  cooldownSec: number;
  notes: string;
};

@Component({
  selector: 'app-stability-lab',
  templateUrl: './stability-lab.component.html',
})
export class StabilityLabComponent implements OnInit, OnDestroy {
  private destroy$ = new Subject<void>();
  private heartbeatSub: Subscription | null = null;

  public readonly bounds = { duration: DURATION_BOUNDS, stop: STOP_THRESHOLD_BOUNDS, tunable: STOP_TUNABLES, profileLimits: PROFILE_LIMITS };
  public readonly restartAuditRows: RestartAuditRow[] = restartAudit();
  public readonly thermalProfiles: ThermalProfile[] = THERMAL_PROFILES;
  public readonly fanCurveSummary = fanCurveSummary;
  public readonly stateLabel = stateLabel;
  public readonly stateExplanation = stateExplanation;
  public readonly completionStatement = completionStatement;
  public readonly isPromotable = isPromotable;

  // ---- live device ----
  public info$: Observable<ISystemInfo>;
  public latestInfo: ISystemInfo | null = null;
  /** Monotonic receipt time (performance.now) of the last genuinely-new sample. */
  private lastFreshMonoMs: number | null = null;
  /** Derived freshness — the real "online" signal (fresh telemetry, not the socket). */
  public freshness: Freshness = evaluateFreshness(null, 0);
  /** True only when telemetry is fresh within the freshness limit. */
  public get online(): boolean { return this.freshness.online; }
  /** Live WebSocket link state — a supplementary display signal only. */
  public connected = false;
  public installedWebVersion: string | null = null;
  public pairStatus: PairStatus | null = null;
  public supported: SupportedDeviceCheck = supportedDevice(null);
  public privacyHidden$: Observable<boolean>;

  // ---- device tuning truth (from /api/system/asic) ----
  public frequencyOptions: number[] = [];
  public voltageOptions: number[] = [];
  public defaultFrequency = 0;
  public defaultVoltage = 0;

  // ---- session config ----
  public baseline: TuningConfig | null = null;
  public profiles: StabilityProfile[] = [];
  public starters: StarterProfileSpec[] = [];
  public thresholds: StopThresholds = defaultStopThresholds();
  public editor: EditorModel = this.blankEditor();

  // ---- preflight / plan ----
  public preflightResult: PreflightResult | null = null;
  public showConfirm = false;
  public acks = { settings: false, restart: false, interrupt: false, protection: false, restore: false };

  // ---- session runtime ----
  public snapshot: LabSnapshot = initialSnapshot(0);
  private sessionStartMs = 0;
  private phaseDeadline = 0;
  private measureStartMs = 0;
  private reconnectStartMono = 0;
  private staleAtRestartMono: number | null = null;
  private debounce: DebounceState = zeroDebounce();
  private startCounters: { a: number | null; r: number | null } = { a: null, r: null };
  private restartOccurredThisProfile = false;
  private countersResetThisProfile = false;
  /** The tuning config the device currently holds (baseline → each applied profile). */
  private currentDeviceConfig: TuningConfig | null = null;
  private warmupSamples: LabSample[] = [];
  private measureSamples: LabSample[] = [];
  private runs: ProfileRun[] = [];
  public liveSamples: LabSample[] = [];   // rolling view for the live chart (bounded)
  public results: ProfileResult[] = [];
  public activeStop: StopReason | null = null;
  public interruptedNotice = false;
  private sessionId = '';

  // ---- abort / promote dialogs ----
  public showAbort = false;
  public showPromote = false;
  public promoteTarget: ProfileResult | null = null;

  // ---- history ----
  private historyStore: StabilityHistoryStore;
  public history: StabilitySessionRecord[] = [];
  public selectedHistory: StabilitySessionRecord | null = null;

  private readonly LIVE_SAMPLE_CAP = 240;

  constructor(
    private liveDataService: LiveDataService,
    private systemService: SystemApiService,
    private webVersionService: WebVersionService,
    private toastr: ToastrService,
    private localStorage: LocalStorageService,
    private sensitiveData: SensitiveData,
  ) {
    this.info$ = this.liveDataService.info$;
    this.privacyHidden$ = this.sensitiveData.hidden;
    this.historyStore = new StabilityHistoryStore(this.localStorage);
  }

  ngOnInit(): void {
    // Interruption detection: a stale active flag means a previous run was cut
    // off by a browser close/refresh. We never claim it "continued".
    if (this.localStorage.getObject(ACTIVE_FLAG)) {
      this.interruptedNotice = true;
      this.localStorage.setObject(ACTIVE_FLAG, null as any);
      localStorage.removeItem(ACTIVE_FLAG);
    }

    this.history = this.historyStore.list();

    this.systemService.getAsicSettings().pipe(first(), takeUntil(this.destroy$)).subscribe(asic => {
      this.frequencyOptions = Array.isArray(asic.frequencyOptions) ? asic.frequencyOptions : [];
      this.voltageOptions = Array.isArray(asic.voltageOptions) ? asic.voltageOptions : [];
      this.defaultFrequency = asic.defaultFrequency;
      this.defaultVoltage = asic.defaultVoltage;
      this.rebuildStarters();
      this.resetEditorDefaults();
    });

    // The WebSocket link is a supplementary signal only. "Online" means the
    // device is streaming telemetry — the 5 s poll fallback keeps that true even
    // when the live socket is down — so we never base it on the socket alone.
    this.liveDataService.connected$.pipe(takeUntil(this.destroy$)).subscribe(c => { this.connected = c; this.recomputePreflight(); });

    this.webVersionService.installedWebVersion$.pipe(takeUntil(this.destroy$)).subscribe(v => {
      this.installedWebVersion = v;
      if (this.latestInfo) this.pairStatus = derivePairStatus(this.latestInfo.version, this.latestInfo.axeOSVersion, v);
      this.recomputePreflight();
    });

    this.info$.pipe(takeUntil(this.destroy$)).subscribe(info => {
      this.latestInfo = info;
      // Only a genuinely-new sample carrying real telemetry refreshes freshness.
      // A gap payload (no hashrate AND no valid temperature) never does, and a
      // bare component re-render never calls this subscription at all.
      if (this.hasCoreTelemetry(info)) {
        this.lastFreshMonoMs = this.nowMono();
      }
      this.supported = supportedDevice(info);
      this.pairStatus = derivePairStatus(info.version, info.axeOSVersion, this.installedWebVersion);
      // Capture the baseline once, when idle, from a supported device.
      if (this.baseline === null && this.snapshot.state === 'idle' && this.supported.supported) {
        this.baseline = captureBaseline(info);
        this.rebuildStarters();
        this.resetEditorDefaults();
      }
      this.refreshFreshness();
      this.recomputePreflight();
    });

    // Always-on heartbeat: re-evaluates freshness against the monotonic clock
    // (so staleness is detected even when NO new sample arrives) and drives the
    // running session. It reads the shared telemetry — it never polls the miner.
    this.heartbeatSub = interval(HEARTBEAT_MS).pipe(takeUntil(this.destroy$)).subscribe(() => this.onHeartbeat());
  }

  ngOnDestroy(): void {
    this.heartbeatSub?.unsubscribe();
    this.destroy$.next();
    this.destroy$.complete();
  }

  /** Overridable monotonic clock (performance.now) — injected in tests. */
  private nowMono(): number {
    return typeof performance !== 'undefined' && performance.now ? performance.now() : Date.now();
  }

  private hasCoreTelemetry(info: ISystemInfo | null | undefined): boolean {
    if (!info) return false;
    const hr = info.hashRate, temp = info.temp;
    return (typeof hr === 'number' && isFinite(hr)) || (typeof temp === 'number' && isFinite(temp) && temp > 0);
  }

  private refreshFreshness(): void {
    this.freshness = evaluateFreshness(this.lastFreshMonoMs, this.nowMono(), FRESHNESS_LIMIT_MS);
  }

  /** Single always-on tick: freshness + idle preflight + running-session drive. */
  private onHeartbeat(): void {
    this.refreshFreshness();
    const state = this.snapshot.state;
    if (state === 'idle') {
      this.recomputePreflight();
    } else if (state === 'warmup' || state === 'measuring' || state === 'cooldown') {
      this.onSessionTick();
    } else if (state === 'reconnecting') {
      this.onReconnectTick();
    }
  }

  /** Warn on browser refresh/close while a run is active (never silent). */
  @HostListener('window:beforeunload', ['$event'])
  onBeforeUnload(event: BeforeUnloadEvent): void {
    if (isActive(this.snapshot.state)) {
      event.preventDefault();
      event.returnValue = 'A Stability Lab session is running. Leaving now interrupts it and may leave settings unrestored.';
    }
  }

  /** CanDeactivate contract — route navigation during a run needs confirmation. */
  canDeactivate(): boolean {
    if (!isActive(this.snapshot.state)) return true;
    return window.confirm('A Stability Lab session is running. Leave and interrupt it?');
  }

  // =========================================================================
  // Editor + profile queue
  // =========================================================================

  private blankEditor(): EditorModel {
    return {
      name: '', frequency: null, coreVoltage: null, thermalControlMode: 'target',
      temptarget: 60, minFanSpeed: 25, manualFanSpeed: 70,
      fanCurve: THERMAL_PROFILES.find(p => p.id === 'balanced')!.points.map(p => ({ ...p })),
      fanCurveHysteresis: 2, warmupSec: DEFAULT_DURATIONS.warmupSec, measureSec: DEFAULT_DURATIONS.measureSec,
      cooldownSec: DEFAULT_DURATIONS.cooldownSec, notes: '',
    };
  }

  private resetEditorDefaults(): void {
    const b = this.baseline;
    this.editor = this.blankEditor();
    if (b) {
      this.editor.frequency = b.frequency;
      this.editor.coreVoltage = b.coreVoltage;
      this.editor.thermalControlMode = b.thermalControlMode;
      if (typeof b.temptarget === 'number') this.editor.temptarget = b.temptarget;
      if (typeof b.minFanSpeed === 'number') this.editor.minFanSpeed = b.minFanSpeed;
      if (typeof b.manualFanSpeed === 'number') this.editor.manualFanSpeed = b.manualFanSpeed;
      if (typeof b.fanCurveHysteresis === 'number') this.editor.fanCurveHysteresis = b.fanCurveHysteresis;
      if (Array.isArray(b.fanCurve)) this.editor.fanCurve = b.fanCurve.map(p => ({ ...p }));
    } else {
      this.editor.frequency = this.defaultFrequency || null;
      this.editor.coreVoltage = this.defaultVoltage || null;
    }
  }

  private rebuildStarters(): void {
    this.starters = buildStarterProfiles(this.baseline, this.frequencyOptions, this.voltageOptions, this.defaultFrequency, this.defaultVoltage);
  }

  public editorConfig(): TuningConfig {
    const e = this.editor;
    const config: TuningConfig = {
      frequency: Number(e.frequency),
      coreVoltage: Number(e.coreVoltage),
      thermalControlMode: e.thermalControlMode,
    };
    if (e.thermalControlMode === 'target') { config.temptarget = e.temptarget; config.minFanSpeed = e.minFanSpeed; }
    else if (e.thermalControlMode === 'manual') { config.manualFanSpeed = e.manualFanSpeed; }
    else if (e.thermalControlMode === 'curve') { config.minFanSpeed = e.minFanSpeed; config.fanCurve = e.fanCurve.map(p => ({ ...p })); config.fanCurveHysteresis = e.fanCurveHysteresis; }
    return config;
  }

  private editorProfile(): StabilityProfile {
    const e = this.editor;
    return { id: nextProfileId(), name: e.name.trim(), ...this.editorConfig(), warmupSec: e.warmupSec, measureSec: e.measureSec, cooldownSec: e.cooldownSec, notes: e.notes.trim() || undefined };
  }

  public editorErrors(): string[] {
    return validateProfile(this.editorProfile());
  }

  public editorDuplicate(): boolean {
    return isDuplicateProfile(this.editorConfig(), this.profiles);
  }

  public editorOutsideOptions(): boolean {
    const c = this.editorConfig();
    return outsideOptionList(c, this.frequencyOptions, this.voltageOptions);
  }

  public editorDiff() {
    return this.baseline ? profileDiff(this.baseline, this.editorConfig()) : [];
  }

  public canAddProfile(): boolean {
    return this.snapshot.state === 'idle'
      && this.profiles.length < PROFILE_LIMITS.maxProfiles
      && this.editorErrors().length === 0
      && !this.editorDuplicate();
  }

  public addEditorProfile(): void {
    if (!this.canAddProfile()) return;
    this.profiles = [...this.profiles, this.editorProfile()];
    this.toastr.success(`Added "${this.profiles[this.profiles.length - 1].name}" to the queue`);
    this.recomputePreflight();
  }

  public addStarter(spec: StarterProfileSpec): void {
    if (this.snapshot.state !== 'idle' || this.profiles.length >= PROFILE_LIMITS.maxProfiles) return;
    if (isDuplicateProfile(spec.config, this.profiles)) {
      this.toastr.info(`"${spec.label}" duplicates a queued profile`);
      return;
    }
    const profile: StabilityProfile = {
      id: nextProfileId(), name: spec.label, ...spec.config,
      warmupSec: DEFAULT_DURATIONS.warmupSec, measureSec: DEFAULT_DURATIONS.measureSec, cooldownSec: DEFAULT_DURATIONS.cooldownSec,
    };
    this.profiles = [...this.profiles, profile];
    this.recomputePreflight();
  }

  public removeProfile(id: string): void {
    if (this.snapshot.state !== 'idle') return;
    this.profiles = this.profiles.filter(p => p.id !== id);
    this.recomputePreflight();
  }

  public moveProfile(id: string, dir: -1 | 1): void {
    if (this.snapshot.state !== 'idle') return;
    const i = this.profiles.findIndex(p => p.id === id);
    const j = i + dir;
    if (i < 0 || j < 0 || j >= this.profiles.length) return;
    const next = [...this.profiles];
    [next[i], next[j]] = [next[j], next[i]];
    this.profiles = next;
  }

  public applyCurveTemplate(profile: ThermalProfile): void {
    this.editor.fanCurve = profile.points.map(p => ({ ...p }));
  }

  public profileDiffFor(profile: StabilityProfile) {
    return this.baseline ? profileDiff(this.baseline, profile) : [];
  }

  public totalPlanSeconds(): number {
    return totalSessionSeconds(this.profiles);
  }

  // ---- stop threshold editing (clamped) ----
  public setAsicStop(value: number): void { this.thresholds = { ...this.thresholds, asicC: clampAsicStop(value) }; this.recomputePreflight(); }
  public setVrmStop(value: number): void { this.thresholds = { ...this.thresholds, vrmC: clampVrmStop(value) }; this.recomputePreflight(); }
  public setThresholds(partial: Partial<StopThresholds>): void { this.thresholds = clampThresholds({ ...this.thresholds, ...partial }); this.recomputePreflight(); }

  // =========================================================================
  // Preflight + confirmation
  // =========================================================================

  public recomputePreflight(): void {
    this.preflightResult = preflight({
      info: this.latestInfo as any,
      online: this.online,
      onlineDetail: this.freshness.reason,
      pairStatus: this.pairStatus,
      profilesQueued: this.profiles.length,
      baselineCaptured: this.baseline !== null,
      otherSessionActive: false,
      stopAsicC: this.thresholds.asicC,
      stopVrmC: this.thresholds.vrmC,
    });
  }

  public get canReview(): boolean {
    return this.snapshot.state === 'idle' && !!this.preflightResult?.canStart;
  }

  /** Run preflight and, if it passes, open the confirmation with the plan. */
  public reviewSession(): void {
    this.recomputePreflight();
    let snap = reduce(this.snapshot, { type: 'START_PREFLIGHT', at: Date.now() });
    if (this.preflightResult?.canStart) {
      snap = reduce(snap, { type: 'PREFLIGHT_OK', at: Date.now() });
      this.snapshot = snap;
      this.acks = { settings: false, restart: false, interrupt: false, protection: false, restore: false };
      this.showConfirm = true;
    } else {
      const reason = this.preflightResult?.blockers.map(b => b.label).join(', ') || 'Preflight failed';
      this.snapshot = reduce(snap, { type: 'PREFLIGHT_FAIL', at: Date.now(), reason });
      this.toastr.warning('Preflight did not pass — see the blocking reasons.');
    }
  }

  public get allAcked(): boolean {
    return this.acks.settings && this.acks.restart && this.acks.interrupt && this.acks.protection && this.acks.restore;
  }

  public cancelConfirm(): void {
    this.showConfirm = false;
    this.snapshot = reduce(this.snapshot, { type: 'CANCEL', at: Date.now() });
  }

  /**
   * Real planned-restart count for THIS session, computed from the actual profile
   * diffs (baseline → each profile → restore) via the audited field table — not
   * an assumption. It is 0 for supported profiles because every field they write
   * is proven live-applicable, but a restart-only field would be counted.
   */
  public plannedRestarts(): number {
    if (!this.baseline) return 0;
    return sessionRestartCount(this.baseline, this.profiles.map(p => this.profileConfig(p)));
  }

  private profileConfig(p: StabilityProfile): TuningConfig {
    return {
      frequency: p.frequency, coreVoltage: p.coreVoltage, thermalControlMode: p.thermalControlMode,
      temptarget: p.temptarget, minFanSpeed: p.minFanSpeed, manualFanSpeed: p.manualFanSpeed,
      fanCurve: p.fanCurve, fanCurveHysteresis: p.fanCurveHysteresis,
    };
  }

  // =========================================================================
  // Session engine
  // =========================================================================

  public startSession(): void {
    if (!this.allAcked || this.snapshot.state !== 'awaiting-confirmation') return;
    this.showConfirm = false;
    this.sessionId = `nx-lab-${Date.now().toString(36)}`;
    this.sessionStartMs = Date.now();
    this.runs = [];
    this.results = [];
    this.liveSamples = [];
    this.activeStop = null;
    this.currentDeviceConfig = this.baseline; // device currently holds the baseline
    this.snapshot = reduce(this.snapshot, { type: 'CONFIRM', at: Date.now() });
    this.localStorage.setObject(ACTIVE_FLAG, { id: this.sessionId, startedAt: this.sessionStartMs });
    // The always-on heartbeat drives sampling; no separate polling loop is opened.
    this.applyProfile(0);
  }

  private dispatch(type: LabEventType, reason?: string): void {
    this.snapshot = reduce(this.snapshot, { type, at: Date.now(), reason });
  }

  private applyProfile(index: number): void {
    const profile = this.profiles[index];
    if (!profile) { this.beginRestore(); return; }
    this.warmupSamples = [];
    this.measureSamples = [];
    this.debounce = zeroDebounce();
    this.startCounters = { a: null, r: null };
    this.restartOccurredThisProfile = false;
    this.countersResetThisProfile = false;
    this.activeStop = null;

    // Real restart decision from the audited field diff (device→profile).
    const target = this.profileConfig(profile);
    const needsRestart = this.currentDeviceConfig !== null && profileRestartRequired(this.currentDeviceConfig, target);

    this.systemService.updateSystem('', profileToSettings(profile))
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        next: () => {
          this.currentDeviceConfig = target;
          if (needsRestart) {
            // Restart-only field changed: reboot and reconnect before warm-up.
            this.restartOccurredThisProfile = true;
            this.dispatch('APPLY_RESTART');
            this.beginDeviceRestart();
          } else {
            // Every stability field applies live on this firmware — no restart.
            this.dispatch('APPLY_APPLIED');
            this.beginWarmup(profile);
          }
        },
        error: (err: HttpErrorResponse) => {
          this.dispatch('APPLY_FAIL', `Applying "${profile.name}" failed: ${err.message}`);
          this.recordRun(true);
          this.beginRestore();
        },
      });
  }

  private beginWarmup(profile: StabilityProfile): void {
    this.phaseDeadline = Date.now() + profile.warmupSec * 1000;
  }

  /** Issue the restart and enter the reconnect wait (freshness-driven). */
  private beginDeviceRestart(): void {
    this.reconnectStartMono = this.nowMono();
    this.staleAtRestartMono = this.lastFreshMonoMs;
    this.systemService.restart('')
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        next: () => this.dispatch('RESTART_SENT'),
        error: (err: HttpErrorResponse) => {
          this.dispatch('RECONNECT_TIMEOUT', `Restart command failed: ${err.message}`);
          this.recordRun(true);
          this.beginRestore();
        },
      });
  }

  /** While reconnecting: a genuinely-new sample after the restart resumes the run;
   *  exceeding the reconnect grace aborts. Driven by the heartbeat. */
  private onReconnectTick(): void {
    const reconnected = this.lastFreshMonoMs !== null
      && this.lastFreshMonoMs !== this.staleAtRestartMono
      && this.freshness.online;
    if (reconnected) {
      this.dispatch('RECONNECTED');
      const profile = this.profiles[this.snapshot.profileIndex];
      if (profile) this.beginWarmup(profile);
      return;
    }
    if (this.nowMono() - this.reconnectStartMono > FRESHNESS_LIMIT_MS + SESSION_RECONNECT_GRACE_MS) {
      this.dispatch('RECONNECT_TIMEOUT', 'Device did not reconnect within the grace period after restart');
      this.recordRun(true);
      this.beginRestore();
    }
  }

  private onSessionTick(): void {
    const state = this.snapshot.state;
    const now = Date.now();
    const info = this.latestInfo;
    const profileIndex = this.snapshot.profileIndex;
    const profile = this.profiles[profileIndex];
    if (!profile) return;

    if (state === 'warmup' || state === 'measuring' || state === 'cooldown') {
      // Telemetry aging → reconnect/offline handling. Stale telemetry is never
      // recorded as a real sample (it becomes a gap), and staying stale beyond
      // the reconnect grace aborts the run.
      const fresh = this.freshness.online;
      if (!fresh && shouldAbortForStaleness(this.freshness.ageMs)) {
        this.activeStop = null;
        this.recordRun(false);
        this.dispatch('STOP', `Device offline — no fresh telemetry for over ${Math.round((FRESHNESS_LIMIT_MS + SESSION_RECONNECT_GRACE_MS) / 1000)} s`);
        this.beginRestore();
        return;
      }

      const phase = state === 'warmup' ? 'warmup' : state === 'measuring' ? 'measure' : 'cooldown';
      // A stale tick yields a gap sample (no telemetry), never the stale values.
      const sample = buildSample(fresh ? (info as any) : null, { sessionStartMs: this.sessionStartMs, now, phase, profileIndex });
      this.pushLiveSample(sample);

      // Track counter resets across the run.
      const delta = shareDelta(this.startCounters.a, this.startCounters.r, sample.sharesAccepted, sample.sharesRejected);
      if (delta.reset) this.countersResetThisProfile = true;
      if (this.startCounters.a === null && sample.sharesAccepted !== null) this.startCounters = { a: sample.sharesAccepted, r: sample.sharesRejected };

      if (state === 'warmup') this.warmupSamples.push(sample);
      else if (state === 'measuring') this.measureSamples.push(sample);

      // Stop-condition evaluation is active throughout the run (gap samples are
      // skipped inside evaluateSample so a dropout never fabricates a stop).
      const evaluation = evaluateSample(sample, this.thresholds, this.debounce);
      this.debounce = evaluation.debounce;
      if (evaluation.stop) {
        this.activeStop = evaluation.stop;
        this.recordRun(false);
        this.dispatch('STOP', evaluation.stop.message);
        this.beginRestore();
        return;
      }

      if (state === 'warmup' && now >= this.phaseDeadline) {
        this.dispatch('WARMUP_DONE');
        this.measureStartMs = now;
        this.phaseDeadline = now + profile.measureSec * 1000;
      } else if (state === 'measuring' && now >= this.phaseDeadline) {
        this.recordRun(false, true);
        this.dispatch('MEASURE_DONE');
        this.phaseDeadline = now + profile.cooldownSec * 1000;
      } else if (state === 'cooldown' && now >= this.phaseDeadline) {
        this.dispatch('COOLDOWN_DONE');
        if (this.snapshot.state === 'applying') {
          this.applyProfile(this.snapshot.profileIndex);
        } else {
          this.beginRestore();
        }
      }
    }
  }

  private pushLiveSample(sample: LabSample): void {
    this.liveSamples = [...this.liveSamples, sample].slice(-this.LIVE_SAMPLE_CAP);
  }

  /** Build and store the current profile's run (measurement outcome). */
  private recordRun(failed: boolean, completed = false): void {
    const index = this.snapshot.profileIndex;
    const profile = this.profiles[index];
    if (!profile) return;
    // If this profile already finished its measurement window (e.g. a stop fired
    // during cooldown), keep the completed result — the abort ends the session,
    // not that profile's valid measurement.
    if (!completed && this.runs.some(r => r.profileId === profile.id && r.ranFullWindow)) {
      return;
    }
    const aborted = !failed && !completed; // stop/abort mid-run
    const run: ProfileRun = {
      profileId: profile.id,
      profileName: profile.name,
      thermalControlMode: profile.thermalControlMode,
      warmupSamples: this.warmupSamples,
      measureSamples: this.measureSamples,
      requestedMeasureMs: profile.measureSec * 1000,
      measuredMeasureMs: this.measureStartMs > 0 ? Math.max(0, Date.now() - this.measureStartMs) : 0,
      restartOccurred: this.restartOccurredThisProfile,
      countersReset: this.countersResetThisProfile,
      ranFullWindow: completed,
      aborted,
      failed,
      abortReason: aborted || failed ? (this.activeStop?.message ?? this.snapshot.reason ?? undefined) : undefined,
    };
    // Each profile is recorded exactly once per session (on completion, stop,
    // or failure); drop any prior partial entry for the same profile first.
    this.runs = this.runs.filter(r => r.profileId !== profile.id);
    this.runs.push(run);
  }

  private beginRestore(): void {
    if (!this.baseline) { this.dispatch('RESTORE_FAIL', 'No captured baseline to restore'); this.finalize(); return; }
    this.systemService.updateSystem('', profileToSettings(this.baseline))
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        next: () => { this.dispatch('RESTORE_OK'); this.finalize(); },
        error: (err: HttpErrorResponse) => { this.dispatch('RESTORE_FAIL', `Restore failed: ${err.message}`); this.finalize(); },
      });
  }

  private finalize(): void {
    // The heartbeat stays alive (it drives idle freshness too); it simply stops
    // sampling once the state is terminal.
    this.localStorage.setObject(ACTIVE_FLAG, null as any);
    localStorage.removeItem(ACTIVE_FLAG);
    // Compute results for every recorded run, then comparative badges.
    this.results = applyComparativeBadges(this.runs.map(computeProfileResult));
    this.saveHistory();
  }

  private saveHistory(): void {
    if (!this.baseline) return;
    const info = this.latestInfo;
    const storedProfiles: StoredProfile[] = this.profiles.map(p => ({
      name: p.name,
      config: { frequency: p.frequency, coreVoltage: p.coreVoltage, thermalControlMode: p.thermalControlMode, temptarget: p.temptarget, minFanSpeed: p.minFanSpeed, manualFanSpeed: p.manualFanSpeed, fanCurve: p.fanCurve, fanCurveHysteresis: p.fanCurveHysteresis },
      warmupSec: p.warmupSec, measureSec: p.measureSec, cooldownSec: p.cooldownSec, notes: p.notes,
    }));
    const record = buildSessionRecord({
      id: this.sessionId,
      startedAt: this.sessionStartMs,
      finishedAt: Date.now(),
      device: {
        productName: info?.productName, productVersion: info?.productVersion,
        targetDevice: info?.targetDevice, targetBoard: info?.targetBoard, targetAsic: info?.targetAsic,
        firmware: info?.version,
      },
      hostname: info?.hostname,
      original: this.baseline,
      profiles: storedProfiles,
      thresholds: this.thresholds,
      results: this.results,
      timeline: this.snapshot.timeline,
      finalState: this.snapshot.state,
      reason: this.snapshot.reason,
      restoreResult: this.snapshot.restoreResult,
    });
    this.history = this.historyStore.save(record);
  }

  // =========================================================================
  // Abort / retry / promote
  // =========================================================================

  public requestAbort(): void { if (isActive(this.snapshot.state)) this.showAbort = true; }
  public cancelAbort(): void { this.showAbort = false; }

  public confirmAbort(): void {
    this.showAbort = false;
    if (!isActive(this.snapshot.state)) return;
    this.recordRun(false);
    this.dispatch('ABORT', 'Owner pressed Abort');
    this.beginRestore();
  }

  /** Manual restore retry after a failed restore. */
  public retryRestore(): void {
    if (this.snapshot.state !== 'failed') return;
    this.beginRestore();
  }

  public openPromote(result: ProfileResult): void {
    if (!isPromotable(result)) {
      this.toastr.info('Only a completed profile can be promoted.');
      return;
    }
    this.promoteTarget = result;
    this.showPromote = true;
  }

  public cancelPromote(): void { this.showPromote = false; this.promoteTarget = null; }

  public confirmPromote(): void {
    const result = this.promoteTarget;
    const profile = result ? this.profiles.find(p => p.id === result.profileId) : null;
    if (!result || !profile || !isPromotable(result)) { this.cancelPromote(); return; }
    this.systemService.updateSystem('', profileToSettings(profile))
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        next: () => { this.toastr.success(`Promoted "${profile.name}" — settings applied`); this.baseline = { ...profile } as TuningConfig; },
        error: (err: HttpErrorResponse) => this.toastr.error(`Could not promote "${profile.name}": ${err.message}`),
      });
    this.cancelPromote();
  }

  public promoteComparison() {
    return this.promoteTarget && this.baseline
      ? profileDiff(this.baseline, (this.profiles.find(p => p.id === this.promoteTarget!.profileId) as TuningConfig) ?? this.baseline)
      : [];
  }

  public resetSession(): void {
    if (isActive(this.snapshot.state)) return;
    this.snapshot = initialSnapshot(0);
    this.results = [];
    this.runs = [];
    this.liveSamples = [];
    this.activeStop = null;
  }

  // =========================================================================
  // History + export
  // =========================================================================

  public viewHistory(record: StabilitySessionRecord): void { this.selectedHistory = record; }
  public closeHistory(): void { this.selectedHistory = null; }
  public deleteHistory(id: string): void { this.history = this.historyStore.remove(id); if (this.selectedHistory?.id === id) this.selectedHistory = null; }

  public exportSession(record: StabilitySessionRecord, format: 'json' | 'csv' | 'md'): void {
    const privacy = this.sensitiveDataHidden();
    let content: string; let mime: string; let ext: string;
    if (format === 'json') { content = exportJson(record, privacy); mime = 'application/json'; ext = 'json'; }
    else if (format === 'csv') { content = exportCsv(record); mime = 'text/csv'; ext = 'csv'; }
    else { content = exportMarkdown(record, privacy); mime = 'text/markdown'; ext = 'md'; }
    this.download(`${record.id}.${ext}`, content, mime);
  }

  private download(filename: string, content: string, mime: string): void {
    const blob = new Blob([content], { type: mime });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
  }

  private sensitiveDataHidden(): boolean {
    let hidden = true;
    this.sensitiveData.hidden.pipe(first()).subscribe(v => hidden = v);
    return hidden;
  }

  // =========================================================================
  // Display helpers
  // =========================================================================

  public maskHost(host: string | undefined | null): string {
    if (!host) return '—';
    return AddressPipe.transform(String(host));
  }

  public isActiveState(): boolean { return isActive(this.snapshot.state); }
  public isTerminalState(): boolean { return isTerminal(this.snapshot.state); }

  /** Seconds of reconnect grace left before a stale running session aborts. */
  public reconnectGraceSeconds(): number | null {
    if (this.freshness.online) return null;
    const rem = reconnectGraceRemainingMs(this.freshness.ageMs);
    return rem === null ? null : Math.round(rem / 1000);
  }

  /** "3 s" telemetry-age label for the freshness indicator. */
  public telemetryAgeText(): string {
    if (this.freshness.ageMs === null) return '—';
    return `${(this.freshness.ageMs / 1000).toFixed(0)} s`;
  }

  public countdownSeconds(): number | null {
    if (!(this.snapshot.state === 'warmup' || this.snapshot.state === 'measuring' || this.snapshot.state === 'cooldown')) return null;
    return Math.max(0, Math.round((this.phaseDeadline - Date.now()) / 1000));
  }

  public currentProfile(): StabilityProfile | null {
    return this.profiles[this.snapshot.profileIndex] ?? null;
  }

  public timeline(): TimelineEntry[] { return this.snapshot.timeline; }

  public fmtDuration(seconds: number): string {
    const s = Math.max(0, Math.round(seconds));
    const m = Math.floor(s / 60);
    const r = s % 60;
    return m > 0 ? `${m}m ${r}s` : `${r}s`;
  }

  public trackById(_i: number, item: { id: string }): string { return item.id; }
  public trackByIndex(i: number): number { return i; }

  // ---- restrained SVG charts (warm-up distinct, threshold line, gaps) ----

  public readonly chartW = 320;
  public readonly chartH = 90;

  private seriesValues(field: keyof LabSample): number[] {
    return this.liveSamples
      .map(s => s[field])
      .filter((v): v is number => typeof v === 'number' && isFinite(v));
  }

  /** Min/max domain for a field across the live samples (with a small pad). */
  public chartDomain(field: keyof LabSample, extra: number[] = []): { min: number; max: number } | null {
    const values = [...this.seriesValues(field), ...extra];
    if (!values.length) return null;
    let min = Math.min(...values);
    let max = Math.max(...values);
    if (min === max) { min -= 1; max += 1; }
    const pad = (max - min) * 0.1;
    return { min: min - pad, max: max + pad };
  }

  /**
   * Polyline points for one phase of a field. Warm-up and measurement are
   * charted separately so the two phases render in distinct colours; a gap
   * sample breaks the line (returns multiple segments joined by a gap marker).
   */
  public chartPoints(field: keyof LabSample, phase: 'warmup' | 'measure', domain: { min: number; max: number } | null): string {
    if (!domain || this.liveSamples.length === 0) return '';
    const w = this.chartW, h = this.chartH;
    const n = this.liveSamples.length;
    const range = domain.max - domain.min || 1;
    const pts: string[] = [];
    this.liveSamples.forEach((s, i) => {
      if (s.phase !== phase) return;
      const v = s[field];
      if (typeof v !== 'number' || !isFinite(v)) return; // gap → line breaks
      const x = n <= 1 ? w / 2 : (i / (n - 1)) * w;
      const y = h - ((v - domain.min) / range) * h;
      pts.push(`${x.toFixed(1)},${y.toFixed(1)}`);
    });
    return pts.join(' ');
  }

  /** Y position of a threshold value within a domain (for the stop line). */
  public thresholdY(value: number, domain: { min: number; max: number } | null): number | null {
    if (!domain) return null;
    const range = domain.max - domain.min || 1;
    if (value < domain.min || value > domain.max) return null;
    return this.chartH - ((value - domain.min) / range) * this.chartH;
  }
}
