import { Component, HostListener, OnDestroy, OnInit } from '@angular/core';
import { HttpErrorResponse } from '@angular/common/http';
import { Observable, Subject, Subscription, first, interval, takeUntil } from 'rxjs';
import { ToastrService } from 'ngx-toastr';

import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { LiveDataService } from 'src/app/services/live-data.service';
import { TelemetryArrivalService } from 'src/app/services/telemetry-arrival.service';
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
  RestartAuditRow, restartAudit, profileRestartRequired, sessionRestartCount, maxSessionDurationMs,
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
  LabSample, SAMPLE_INTERVAL_MS, buildSample, shareDelta,
} from './stability-telemetry';
import {
  IntakeState, initialIntake, considerArrival, hasCoreTelemetry,
} from './stability-intake';
import {
  VisibilityAccumulator, VisibilityStats, VisibilityPhase,
  initialVisibility, applyVisibility, visibilityStats, visibilitySummary,
} from './stability-visibility';
import {
  CoverageStats, computeCoverage, sampleTargetText, coveragePctText,
} from './stability-coverage';
import {
  StopThresholds, DebounceState, STOP_TUNABLES, defaultStopThresholds, clampThresholds, zeroDebounce,
  evaluateSample, StopReason, migrateThresholds,
} from './stability-stop';
import {
  ProfileRun, ProfileResult, computeProfileResult, applyComparativeBadges, completionStatement, isPromotable,
} from './stability-results';
import {
  StabilitySessionRecord, StabilityHistoryStore, buildSessionRecord, StoredProfile, exportJson, exportCsv, exportMarkdown,
} from './stability-history';

/** localStorage flag proving a live run was in progress (interruption detection). */
const ACTIVE_FLAG = 'NX_STABILITY_ACTIVE';
/** localStorage key for the owner's tuned stop thresholds (migrated on load). */
const THRESHOLDS_KEY = 'NX_STABILITY_THRESHOLDS';

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
  public readonly sampleTargetText = sampleTargetText;
  public readonly coveragePctText = coveragePctText;
  public readonly visibilitySummary = visibilitySummary;

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
  /** Honest explanations for starters that were deliberately omitted. */
  public starterNotes: string[] = [];
  public thresholds: StopThresholds = defaultStopThresholds();
  public editor: EditorModel = this.blankEditor();

  // ---- preflight / plan ----
  public preflightResult: PreflightResult | null = null;
  public showConfirm = false;
  public acks = { settings: false, restart: false, interrupt: false, protection: false, restore: false };

  // ---- session runtime ----
  public snapshot: LabSnapshot = initialSnapshot(0);
  /** Wall-clock session start (Date.now) — record timestamps + max-wall-clock cap. */
  private sessionStartMs = 0;
  /** Monotonic session start (performance.now) — the basis for every sample tMs. */
  private sessionStartMono = 0;
  /** Monotonic start of the current phase and its duration (deadline = start+dur). */
  private phaseStartMono = 0;
  private phaseDurationMs = 0;
  /** Session-relative ms at which the current phase began (for sample windowing). */
  private phaseStartTMs = 0;
  /** Session-relative ms at which the measurement window began. */
  private measureStartTMs = 0;
  /** Monotonic measurement start — for the actual measured duration. */
  private measureStartMono = 0;
  /** Bounded maximum session DURATION (ms) — a MONOTONIC runtime cap, not a timestamp. */
  private maxSessionDurationMs = 0;
  private reconnectStartMono = 0;
  private staleAtRestartMono: number | null = null;
  private debounce: DebounceState = zeroDebounce();
  private startCounters: { a: number | null; r: number | null } = { a: null, r: null };
  private restartOccurredThisProfile = false;
  private countersResetThisProfile = false;
  /** Event-driven sample intake state (arrival-identity dedup + monotonic cadence). */
  private intake: IntakeState = initialIntake();
  /** Identity of the most recent telemetry arrival observed (freshness dedup). */
  private lastArrivalId: number | null = null;
  /** Page Visibility accumulator + latest snapshot (monotonic). */
  private visibility: VisibilityAccumulator = initialVisibility(false, 0);
  public visStats: VisibilityStats = visibilityStats(this.visibility, 0);
  /** Live coverage over the current phase (warm-up / measurement). */
  public liveCoverage: CoverageStats | null = null;
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
    private telemetryArrival: TelemetryArrivalService,
    private systemService: SystemApiService,
    private webVersionService: WebVersionService,
    private toastr: ToastrService,
    private localStorage: LocalStorageService,
    private sensitiveData: SensitiveData,
  ) {
    // The template renders from the shared info$ (display only); all evidence and
    // freshness logic keys off the arrival stream so it dedups on identity.
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

    // Load the owner's saved stop thresholds, migrating an untouched legacy
    // config (VRM 100 °C) to the conservative board-601 default (70 °C). An
    // owner-customised config is preserved verbatim (only clamped to safe bounds).
    const storedThresholds = this.localStorage.getObject(THRESHOLDS_KEY);
    this.thresholds = migrateThresholds(storedThresholds || null);

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

    // Subscribe ONCE to the shared arrival stream. Each callback carries the
    // source-assigned monotonic `arrivalId`; a replay / re-subscription carries an
    // id we have already seen (so it never re-stamps freshness or double-counts).
    this.telemetryArrival.arrivals$.pipe(takeUntil(this.destroy$)).subscribe(({ info, arrivalId }) => {
      const isNewArrival = this.lastArrivalId === null || arrivalId > this.lastArrivalId;
      this.lastArrivalId = arrivalId;
      this.latestInfo = info;
      const mono = this.nowMono();
      // Freshness reflects genuine ARRIVALS, not payload changes: a new arrival
      // carrying core telemetry re-stamps the receipt time; a replay of a known
      // arrival and a gap payload never do.
      if (isNewArrival && hasCoreTelemetry(info)) {
        this.lastFreshMonoMs = mono;
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
      // Event-driven evidence: a running measurement collects a sample when a
      // GENUINELY NEW telemetry arrival is delivered — not when a throttled timer
      // fires — and the same arrival drives the deadline/stale supervision so
      // progress does not depend on a background-throttled heartbeat.
      if (this.isSamplingState()) {
        this.ingestArrival(info, arrivalId, mono);
        if (this.isSamplingState()) this.superviseSession(mono);
      }
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

  /** True when the page is not foreground-visible (Page Visibility API). */
  private isPageHidden(): boolean {
    return typeof document !== 'undefined' && document.visibilityState === 'hidden';
  }

  /** Public read for the template banner. */
  public get pageHidden(): boolean {
    return this.visStats.currentlyHidden;
  }

  /** States in which a running measurement collects evidence. */
  private isSamplingState(): boolean {
    const s = this.snapshot.state;
    return s === 'warmup' || s === 'measuring' || s === 'cooldown';
  }

  private refreshFreshness(): void {
    this.freshness = evaluateFreshness(this.lastFreshMonoMs, this.nowMono(), FRESHNESS_LIMIT_MS);
  }

  /** Single always-on tick: freshness + idle preflight + running-session supervision. */
  private onHeartbeat(): void {
    const mono = this.nowMono();
    this.refreshFreshness();
    const state = this.snapshot.state;
    if (state === 'idle') {
      this.recomputePreflight();
    } else if (state === 'warmup' || state === 'measuring' || state === 'cooldown') {
      // The heartbeat supervises deadlines/staleness even when NO telemetry
      // arrives; it never builds an evidence sample (that is event-driven).
      this.superviseSession(mono);
    } else if (state === 'reconnecting') {
      this.visibility = applyVisibility(this.visibility, this.isPageHidden(), mono, 'other');
      this.visStats = visibilityStats(this.visibility, mono);
      this.onReconnectTick();
    }
  }

  /** Records a visibility change the instant it happens — never refreshes freshness. */
  @HostListener('document:visibilitychange')
  onVisibilityChange(): void {
    if (!this.isActiveState()) return;
    const mono = this.nowMono();
    this.visibility = applyVisibility(this.visibility, this.isPageHidden(), mono, this.currentVisibilityPhase());
    this.visStats = visibilityStats(this.visibility, mono);
  }

  private currentVisibilityPhase(): VisibilityPhase {
    const s = this.snapshot.state;
    return s === 'warmup' ? 'warmup' : s === 'measuring' ? 'measure' : 'other';
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
    const built = buildStarterProfiles(this.baseline, this.frequencyOptions, this.voltageOptions, this.defaultFrequency, this.defaultVoltage);
    this.starters = built.specs;
    this.starterNotes = built.notes;
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

  // ---- stop threshold editing (clamped + persisted) ----
  public setAsicStop(value: number): void { this.thresholds = { ...this.thresholds, asicC: clampAsicStop(value) }; this.persistThresholds(); this.recomputePreflight(); }
  public setVrmStop(value: number): void { this.thresholds = { ...this.thresholds, vrmC: clampVrmStop(value) }; this.persistThresholds(); this.recomputePreflight(); }
  public setThresholds(partial: Partial<StopThresholds>): void { this.thresholds = clampThresholds({ ...this.thresholds, ...partial }); this.persistThresholds(); this.recomputePreflight(); }

  /** Persist the owner's tuned thresholds so they survive across sessions. */
  private persistThresholds(): void { this.localStorage.setObject(THRESHOLDS_KEY, { ...this.thresholds }); }

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
    this.sessionStartMs = Date.now();          // wall-clock — human timestamps only
    this.sessionStartMono = this.nowMono();    // monotonic — all runtime calculations
    this.lastArrivalId = null;
    this.maxSessionDurationMs = maxSessionDurationMs(this.profiles);
    this.runs = [];
    this.results = [];
    this.liveSamples = [];
    this.liveCoverage = null;
    this.activeStop = null;
    // Start the visibility accumulator from the current page state (honestly
    // recording a session that begins hidden).
    this.visibility = initialVisibility(this.isPageHidden(), this.sessionStartMono);
    this.visStats = visibilityStats(this.visibility, this.sessionStartMono);
    this.currentDeviceConfig = this.baseline; // device currently holds the baseline
    this.snapshot = reduce(this.snapshot, { type: 'CONFIRM', at: Date.now() });
    this.localStorage.setObject(ACTIVE_FLAG, { id: this.sessionId, startedAt: this.sessionStartMs });
    // Evidence is driven by genuine telemetry arrival; the heartbeat only
    // supervises deadlines/staleness. No separate polling loop is opened.
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
    this.liveCoverage = null;
    // Reset intake so the first genuine telemetry of this profile is accepted.
    this.intake = initialIntake();

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
    this.startPhase(profile.warmupSec);
  }

  /**
   * Open a phase on the MONOTONIC clock. The deadline is start+duration, so a
   * wall-clock change cannot shorten or extend a phase, and a late supervise
   * callback simply crosses the deadline once rather than drifting.
   */
  private startPhase(seconds: number): void {
    const mono = this.nowMono();
    this.phaseStartMono = mono;
    this.phaseDurationMs = Math.max(0, seconds * 1000);
    this.phaseStartTMs = Math.max(0, Math.round(mono - this.sessionStartMono));
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

  /**
   * Event-driven evidence intake — called for each telemetry ARRIVAL while a
   * session is sampling. A sample is built and recorded ONLY when the intake
   * accepts it (a new arrival identity, cadence elapsed); a replay of a known
   * arrival, a re-render, or a too-soon arrival is ignored. This is what makes
   * the evidence count reflect real telemetry rather than a throttled timer, and
   * why byte-identical genuine readings still count.
   */
  private ingestArrival(info: ISystemInfo | null, arrivalId: number, mono: number): void {
    const decision = considerArrival(this.intake, info as any, arrivalId, mono, SAMPLE_INTERVAL_MS);
    this.intake = decision.state;
    if (!decision.accept) return;

    const state = this.snapshot.state;
    const phase = state === 'warmup' ? 'warmup' : state === 'measuring' ? 'measure' : 'cooldown';
    const sample = buildSample(info as any, {
      sessionStartMs: this.sessionStartMono, now: mono, phase, profileIndex: this.snapshot.profileIndex,
    });
    this.acceptSample(sample, state);
  }

  /** Record an accepted sample: live view, share tracking, stop evaluation. */
  private acceptSample(sample: LabSample, state: LabSnapshot['state']): void {
    this.pushLiveSample(sample);

    // Track counter resets across the run.
    const delta = shareDelta(this.startCounters.a, this.startCounters.r, sample.sharesAccepted, sample.sharesRejected);
    if (delta.reset) this.countersResetThisProfile = true;
    if (this.startCounters.a === null && sample.sharesAccepted !== null) this.startCounters = { a: sample.sharesAccepted, r: sample.sharesRejected };

    if (state === 'warmup') this.warmupSamples.push(sample);
    else if (state === 'measuring') this.measureSamples.push(sample);

    // Stop-condition evaluation runs on genuine telemetry only (a gap is never
    // built into a sample here, so a dropout can never fabricate a stop).
    const evaluation = evaluateSample(sample, this.thresholds, this.debounce);
    this.debounce = evaluation.debounce;
    if (evaluation.stop) {
      this.activeStop = evaluation.stop;
      this.recordRun(false);
      this.dispatch('STOP', evaluation.stop.message);
      this.beginRestore();
      return;
    }
    this.updateLiveCoverage();
  }

  /**
   * Supervise a running session on the monotonic clock: record visibility,
   * enforce the maximum wall-clock cap and the stale-telemetry abort, and cross
   * ONE phase deadline per call (never skipping multiple states silently). Builds
   * no evidence — the sample count is event-driven, not timer-driven.
   */
  private superviseSession(mono: number): void {
    const state = this.snapshot.state;
    if (!(state === 'warmup' || state === 'measuring' || state === 'cooldown')) return;
    const profile = this.profiles[this.snapshot.profileIndex];
    if (!profile) return;

    // Visibility snapshot (keeps the phase-affected flags honest while hidden).
    this.visibility = applyVisibility(this.visibility, this.isPageHidden(), mono, this.currentVisibilityPhase());
    this.visStats = visibilityStats(this.visibility, mono);

    // Hard maximum-DURATION cap on the MONOTONIC clock — never leave the tested
    // profile active indefinitely if the browser was throttled so phase callbacks
    // arrived very late. Using monotonic elapsed (not Date.now) means a system
    // wall-clock jump — forward or backward — can neither trip this early nor
    // extend it.
    if (this.maxSessionDurationMs > 0 && mono - this.sessionStartMono > this.maxSessionDurationMs) {
      this.activeStop = null;
      this.recordRun(false);
      this.dispatch('STOP', `Maximum session time (${Math.round(this.maxSessionDurationMs / 60000)} min) exceeded — restoring original configuration`);
      this.beginRestore();
      return;
    }

    // Telemetry staleness → reconnect/offline handling. Staying stale beyond the
    // reconnect grace aborts the run (unchanged freshness contract).
    if (!this.freshness.online && shouldAbortForStaleness(this.freshness.ageMs)) {
      this.activeStop = null;
      this.recordRun(false);
      this.dispatch('STOP', `Device offline — no fresh telemetry for over ${Math.round((FRESHNESS_LIMIT_MS + SESSION_RECONNECT_GRACE_MS) / 1000)} s`);
      this.beginRestore();
      return;
    }

    this.updateLiveCoverage();

    // Phase deadline on the monotonic clock — at most one transition per call.
    const elapsed = mono - this.phaseStartMono;
    if (elapsed < this.phaseDurationMs) return;

    if (state === 'warmup') {
      this.dispatch('WARMUP_DONE');
      this.measureStartMono = mono;
      this.startPhase(profile.measureSec);
      this.measureStartTMs = this.phaseStartTMs;
    } else if (state === 'measuring') {
      this.recordRun(false, true);
      this.dispatch('MEASURE_DONE');
      this.startPhase(profile.cooldownSec);
    } else if (state === 'cooldown') {
      this.dispatch('COOLDOWN_DONE');
      if (this.snapshot.state === 'applying') {
        this.applyProfile(this.snapshot.profileIndex);
      } else {
        this.beginRestore();
      }
    }
  }

  /** Recompute live coverage over the current phase's valid samples. */
  private updateLiveCoverage(): void {
    const state = this.snapshot.state;
    const profile = this.currentProfile();
    if (!profile || !(state === 'warmup' || state === 'measuring')) return;
    const samples = state === 'warmup' ? this.warmupSamples : this.measureSamples;
    const windowMs = (state === 'warmup' ? profile.warmupSec : profile.measureSec) * 1000;
    const times = samples.filter(s => !s.gap).map(s => s.tMs - this.phaseStartTMs);
    this.liveCoverage = computeCoverage({ sampleTimesMs: times, windowMs, cadenceMs: SAMPLE_INTERVAL_MS });
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
      measuredMeasureMs: this.measureStartMono > 0 ? Math.max(0, this.nowMono() - this.measureStartMono) : 0,
      cadenceMs: SAMPLE_INTERVAL_MS,
      measureStartTMs: this.measureStartTMs,
      visibility: visibilityStats(this.visibility, this.nowMono()),
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
      sampleCadenceMs: SAMPLE_INTERVAL_MS,
      maxSessionDurationMs: this.maxSessionDurationMs,
      restoreVerified: this.snapshot.restoreResult === 'ok',
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

  // ---- live coverage (Stage 7) ----

  /** "121 valid samples · target 120" for the active phase. */
  public liveSampleTargetText(): string {
    const c = this.liveCoverage;
    if (!c) return '—';
    return sampleTargetText(c.validSamples, c.expectedTarget);
  }

  public liveCoverageText(): string {
    return this.liveCoverage ? coveragePctText(this.liveCoverage.coveragePct) : '—';
  }

  /** Monotonic age of the most recently accepted evidence sample. */
  public latestSampleAgeMs(): number | null {
    if (this.intake.lastAcceptedMonoMs === null) return null;
    return Math.max(0, this.nowMono() - this.intake.lastAcceptedMonoMs);
  }

  public latestSampleAgeText(): string {
    const ms = this.latestSampleAgeMs();
    return ms === null ? '—' : `${(ms / 1000).toFixed(0)} s`;
  }

  public medianIntervalText(): string {
    const ms = this.liveCoverage?.medianIntervalMs ?? null;
    return ms === null ? '—' : `${(ms / 1000).toFixed(1)} s`;
  }

  /** Current evidence gap = time since the last accepted sample. */
  public currentGapText(): string {
    return this.latestSampleAgeText();
  }

  public hiddenTotalText(): string {
    return `${Math.round(this.visStats.totalHiddenMs / 1000)} s`;
  }

  /** "121 valid samples · target 120" for a finished result. */
  public resultSampleTargetText(r: ProfileResult): string {
    return sampleTargetText(r.validSamples, r.expectedSamples);
  }

  public countdownSeconds(): number | null {
    if (!(this.snapshot.state === 'warmup' || this.snapshot.state === 'measuring' || this.snapshot.state === 'cooldown')) return null;
    // Countdown on the monotonic clock (phase start + duration − now).
    return Math.max(0, Math.round((this.phaseStartMono + this.phaseDurationMs - this.nowMono()) / 1000));
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
