import { Component, HostListener, OnDestroy, OnInit } from '@angular/core';
import { HttpErrorResponse } from '@angular/common/http';
import { Observable, Subject, Subscription, first, interval, takeUntil } from 'rxjs';
import { ToastrService } from 'ngx-toastr';

import { SystemInfo as ISystemInfo } from 'src/app/generated/models';
import { LiveDataService } from 'src/app/services/live-data.service';
import { SystemApiService } from 'src/app/services/system.service';
import { WebVersionService } from 'src/app/services/web-version.service';
import { SensitiveData } from 'src/app/services/sensitive-data.service';
import { PoolStrategyService } from 'src/app/services/pool-strategy.service';
import { derivePairStatus, PairStatus } from 'src/app/services/version-state';
import {
  Freshness, FRESHNESS_LIMIT_MS, HEARTBEAT_MS, evaluateFreshness,
} from '../stability-lab/stability-freshness';

import {
  PoolProfile, PoolChain, PoolEndpoint, StratumProtocol, PasswordMode, SwitchSecrets,
  CHAIN_LABELS, PROFILE_LIMITS, buildStarterProfiles, canAddProfile,
  chainLabel, chainShort, endpointReplacesPassword, endpointUsable, fallbackProvided,
  isDuplicateProfile, maskAccount, maskHost, maskPassword, normalizePort, realSecret,
  profileToSettings, validateProfile, StarterSpec,
} from './pool-profile';
import {
  ActivePoolRecord, ChainContext, CHAIN_DISCLAIMER, deriveChainContext,
} from './pool-chain';
import { SwitchReview, SWITCH_CONFIRM_STATEMENTS, buildSwitchReview, tlsLabel } from './pool-diff';
import { PoolPreflightResult, poolPreflight } from './pool-preflight';
import {
  SwitchSnapshot, SwitchEventType, RecoveryResult,
  initialSwitchSnapshot, reduceSwitch, switchStateLabel, switchStateExplanation,
  isSwitchActive, isSwitchTerminal,
} from './pool-switch-machine';
import {
  PoolVerification, VerifyTarget, RECONNECT_TIMEOUT_MS, VERIFY_TIMEOUT_MS,
  evaluatePoolVerification, verificationLevels,
} from './pool-verify';
import {
  CapturedConfig, RestoreSnapshot, PoolSwitchRecord, RestoreSecretPlan,
  buildRestoreSnapshot, buildSwitchRecord, captureConfig, captureToSettings,
  containsForbiddenKeys, exportJson, exportMarkdown, maskedHostChange,
} from './pool-history';
import {
  SwitchInterruptionRecord, InterruptionRecovery, deriveInterruptionRecovery,
} from './pool-recovery';

/** Editor model for one endpoint. No password field — passwords are entered at switch time. */
interface EndpointEditor {
  host: string;
  port: number | null;
  user: string;
  passwordMode: PasswordMode;
  tls: number;
  protocol: StratumProtocol;
}

/**
 * Session-only password form for ONE active switch. Held in component memory,
 * cleared the instant an operation ends. NEVER persisted, serialized or logged.
 */
interface SecretForm {
  /** Target primary password (when primary passwordMode is 'set'). */
  target: string;
  /** Target fallback password (when an explicit fallback is 'set'). */
  targetFallback: string;
  /** Current/original password, required so rollback can restore it. */
  current: string;
  /** "Keep current password instead" — downgrades this switch to keep-mode. */
  keepOverride: boolean;
}

/** Editor model for a whole profile. */
interface ProfileEditor {
  id: string | null;
  name: string;
  chain: PoolChain;
  primary: EndpointEditor;
  fallbackEnabled: boolean;
  fallback: EndpointEditor;
  notes: string;
}

/** Which recovery flow the single rolling-back/restoring state is executing. */
type RecoveryPhase = 'apply' | 'restart' | 'reconnect' | 'verify' | null;

@Component({
  selector: 'app-pool-strategy',
  templateUrl: './pool-strategy.component.html',
})
export class PoolStrategyComponent implements OnInit, OnDestroy {
  private destroy$ = new Subject<void>();
  private heartbeatSub: Subscription | null = null;

  public readonly chainLabels = CHAIN_LABELS;
  public readonly profileLimits = PROFILE_LIMITS;
  public readonly disclaimer = CHAIN_DISCLAIMER;
  public readonly confirmStatements = SWITCH_CONFIRM_STATEMENTS;
  public readonly switchStateLabel = switchStateLabel;
  public readonly switchStateExplanation = switchStateExplanation;
  public readonly chainOptions: { value: PoolChain; label: string }[] = [
    { value: 'BTC', label: CHAIN_LABELS.BTC },
    { value: 'BCH', label: CHAIN_LABELS.BCH },
    { value: 'custom', label: CHAIN_LABELS.custom },
  ];
  public readonly protocolOptions: StratumProtocol[] = ['SV1', 'SV2'];
  public readonly tlsOptions = [
    { value: 0, label: 'No TLS' },
    { value: 1, label: 'TLS (system cert)' },
  ];
  public readonly tlsLabel = tlsLabel;
  public readonly maskHost = maskHost;
  public readonly maskAccount = maskAccount;
  public readonly maskPassword = maskPassword;
  public readonly chainLabelFn = chainLabel;
  public readonly chainShortFn = chainShort;

  // ---- live device ----
  public info$: Observable<ISystemInfo>;
  public latestInfo: ISystemInfo | null = null;
  private lastFreshMonoMs: number | null = null;
  public freshness: Freshness = evaluateFreshness(null, 0);
  public get online(): boolean { return this.freshness.online; }
  public connected = false;
  public installedWebVersion: string | null = null;
  public pairStatus: PairStatus | null = null;
  public privacyHidden$: Observable<boolean>;

  // ---- profiles + chain context ----
  public profiles: PoolProfile[] = [];
  public chainContext: ChainContext = deriveChainContext(null, null);
  public starters: StarterSpec[] = [];
  public starterNotes: string[] = [];
  public editor: ProfileEditor = this.blankEditor();
  public editorOpen = false;

  // ---- review + confirm ----
  public selectedProfile: PoolProfile | null = null;
  public review: SwitchReview | null = null;
  public preflightResult: PoolPreflightResult | null = null;
  public showConfirm = false;
  public acks = { interrupt: false, change: false, rollback: false, funds: false };

  // ---- session-only secrets (NEVER persisted) ----
  /** The confirm-dialog password form — cleared the instant an operation starts/ends. */
  public pwd: SecretForm = this.blankSecretForm();
  /** Secrets for the ACTIVE operation only (held out of the form, cleared on finish). */
  private opSecrets: SwitchSecrets & { original?: string } = {};
  /** Which pools this operation replaced a password on (for rollback restore). */
  private replaced = { primary: false, fallback: false };

  // ---- switch engine ----
  public snapshot: SwitchSnapshot = initialSwitchSnapshot();
  public verification: PoolVerification | null = null;
  private capturedOriginal: CapturedConfig | null = null;
  private verifyTarget: VerifyTarget = { primaryHost: '', fallbackHost: '' };
  private baselineShares: { a: number | null; r: number | null } = { a: null, r: null };
  private sessionId = '';
  private sessionStartMs = 0;
  private reconnectStartMono = 0;
  private staleAtRestartMono: number | null = null;
  private verifyStartMono = 0;
  private recoveryPhase: RecoveryPhase = null;
  private passwordWasReplaced = false;

  // ---- restore snapshot ----
  public restoreSnapshot: RestoreSnapshot | null = null;
  public showRestore = false;

  // ---- history ----
  public history: PoolSwitchRecord[] = [];
  public interruptedNotice = false;

  // ---- interrupted-switch recovery (Blocker 4) ----
  private interruptionRecord: SwitchInterruptionRecord | null = null;
  public interruptionRecovery: InterruptionRecovery | null = null;

  public readonly verificationLevels = verificationLevels;

  constructor(
    private liveDataService: LiveDataService,
    private systemService: SystemApiService,
    private webVersionService: WebVersionService,
    private sensitiveData: SensitiveData,
    private poolStrategy: PoolStrategyService,
    private toastr: ToastrService,
  ) {
    this.info$ = this.liveDataService.info$;
    this.privacyHidden$ = this.sensitiveData.hidden;
  }

  ngOnInit(): void {
    // Interruption recovery: a persisted record means a previous switch was cut
    // off (browser close/refresh). We KEEP the record so the recovery UI can act
    // on it; it is only cleared when the owner resolves it. The state is never
    // shown as "Complete", and no restore is claimed unless telemetry shows it.
    this.interruptionRecord = this.poolStrategy.getInterruptionRecord();
    if (this.interruptionRecord) {
      this.interruptedNotice = true;
    }

    this.poolStrategy.profiles$.pipe(takeUntil(this.destroy$)).subscribe(list => { this.profiles = list; });
    this.poolStrategy.chainContext$.pipe(takeUntil(this.destroy$)).subscribe(ctx => { this.chainContext = ctx; });
    this.history = this.poolStrategy.listHistory();
    this.restoreSnapshot = this.poolStrategy.getRestoreSnapshot();

    this.liveDataService.connected$.pipe(takeUntil(this.destroy$)).subscribe(c => { this.connected = c; });

    this.webVersionService.installedWebVersion$.pipe(takeUntil(this.destroy$)).subscribe(v => {
      this.installedWebVersion = v;
      if (this.latestInfo) this.pairStatus = derivePairStatus(this.latestInfo.version, this.latestInfo.axeOSVersion, v);
    });

    this.info$.pipe(takeUntil(this.destroy$)).subscribe(info => {
      this.latestInfo = info;
      this.lastFreshMonoMs = this.nowMono();
      this.pairStatus = derivePairStatus(info.version, info.axeOSVersion, this.installedWebVersion);
      this.refreshFreshness();
      this.rebuildStarters();
      if (this.selectedProfile) this.review = this.buildReview(this.selectedProfile);
      this.recomputePreflight();
      this.refreshInterruptionRecovery();
      this.onTelemetry();
    });

    this.heartbeatSub = interval(HEARTBEAT_MS).pipe(takeUntil(this.destroy$)).subscribe(() => this.onHeartbeat());
  }

  ngOnDestroy(): void {
    this.heartbeatSub?.unsubscribe();
    // A component teardown must not leave a live secret behind.
    this.clearSecrets();
    this.destroy$.next();
    this.destroy$.complete();
  }

  private blankSecretForm(): SecretForm {
    return { target: '', targetFallback: '', current: '', keepOverride: false };
  }

  /** Drop every session secret. Called on complete/rollback/cancel/abort/fail/destroy. */
  private clearSecrets(): void {
    this.opSecrets = {};
    this.replaced = { primary: false, fallback: false };
    this.pwd = this.blankSecretForm();
  }

  private refreshInterruptionRecovery(): void {
    this.interruptionRecovery = this.interruptionRecord
      ? deriveInterruptionRecovery(this.interruptionRecord, this.latestInfo)
      : null;
  }

  private nowMono(): number {
    return typeof performance !== 'undefined' && performance.now ? performance.now() : Date.now();
  }

  private refreshFreshness(): void {
    this.freshness = evaluateFreshness(this.lastFreshMonoMs, this.nowMono(), FRESHNESS_LIMIT_MS);
  }

  // ---- navigation / interruption guards ----

  @HostListener('window:beforeunload', ['$event'])
  onBeforeUnload(event: BeforeUnloadEvent): void {
    if (isSwitchActive(this.snapshot.state)) {
      event.preventDefault();
      event.returnValue = 'A pool switch is in progress. Leaving now interrupts it and may leave the device on the new pool unverified.';
    }
  }

  canDeactivate(): boolean {
    if (!isSwitchActive(this.snapshot.state)) return true;
    return window.confirm('A pool switch is in progress. Leave and interrupt it?');
  }

  // =========================================================================
  // Editor + profiles
  // =========================================================================

  private blankEndpointEditor(): EndpointEditor {
    return { host: '', port: null, user: '', passwordMode: 'keep', tls: 0, protocol: 'SV1' };
  }

  private blankEditor(): ProfileEditor {
    return {
      id: null, name: '', chain: 'BTC',
      primary: this.blankEndpointEditor(),
      fallbackEnabled: false, fallback: this.blankEndpointEditor(),
      notes: '',
    };
  }

  public openNewProfile(): void {
    this.editor = this.blankEditor();
    this.editorOpen = true;
  }

  public editProfile(profile: PoolProfile): void {
    this.editor = {
      id: profile.id,
      name: profile.name,
      chain: profile.chain,
      primary: this.endpointToEditor(profile.primary),
      fallbackEnabled: fallbackProvided(profile.fallback),
      fallback: profile.fallback ? this.endpointToEditor(profile.fallback) : this.blankEndpointEditor(),
      notes: profile.notes ?? '',
    };
    this.editorOpen = true;
  }

  public cancelEditor(): void {
    this.editorOpen = false;
    this.editor = this.blankEditor();
  }

  private endpointToEditor(ep: PoolEndpoint): EndpointEditor {
    return {
      host: ep.host, port: normalizePort(ep.port), user: ep.user,
      passwordMode: ep.passwordMode,
      tls: ep.tls ?? 0, protocol: ep.protocol,
    };
  }

  private editorEndpoint(e: EndpointEditor): PoolEndpoint {
    // No password is stored on the endpoint — a 'set' profile takes its password
    // at switch time. Only the mode is persisted.
    return {
      host: (e.host ?? '').trim(),
      port: normalizePort(e.port),
      user: (e.user ?? '').trim(),
      passwordMode: e.passwordMode,
      tls: e.tls,
      protocol: e.protocol,
    };
  }

  private editorToProfile(): PoolProfile {
    const now = Date.now();
    return {
      id: this.editor.id ?? 'draft',
      name: this.editor.name.trim(),
      chain: this.editor.chain,
      primary: this.editorEndpoint(this.editor.primary),
      fallback: this.editor.fallbackEnabled ? this.editorEndpoint(this.editor.fallback) : null,
      notes: this.editor.notes.trim() || undefined,
      createdAt: now, updatedAt: now,
    };
  }

  public editorErrors(): string[] {
    return validateProfile(this.editorToProfile());
  }

  public editorDuplicate(): boolean {
    const draft = this.editorToProfile();
    return isDuplicateProfile(draft, this.profiles.filter(p => p.id !== this.editor.id));
  }

  public canSaveEditor(): boolean {
    if (this.editorErrors().length > 0 || this.editorDuplicate()) return false;
    if (!this.editor.id && !canAddProfile(this.profiles.length)) return false;
    return true;
  }

  public saveEditor(): void {
    if (!this.canSaveEditor()) return;
    const draft = this.editorToProfile();
    if (this.editor.id) {
      this.poolStrategy.updateProfile({ ...draft, id: this.editor.id });
      this.toastr.success(`Updated profile "${draft.name}"`);
    } else {
      const { id, createdAt, updatedAt, ...rest } = draft;
      this.poolStrategy.addProfile(rest);
      this.toastr.success(`Created profile "${draft.name}"`);
    }
    this.cancelEditor();
  }

  public removeProfile(profile: PoolProfile): void {
    if (isSwitchActive(this.snapshot.state)) return;
    this.poolStrategy.removeProfile(profile.id);
    if (this.selectedProfile?.id === profile.id) {
      this.selectedProfile = null;
      this.review = null;
    }
    this.toastr.info(`Removed profile "${profile.name}"`);
  }

  private rebuildStarters(): void {
    const built = buildStarterProfiles(this.latestInfo);
    this.starters = built.specs;
    this.starterNotes = built.notes;
  }

  public useStarter(spec: StarterSpec): void {
    this.editor = {
      id: null,
      name: spec.draft.name || spec.label,
      chain: spec.draft.chain,
      primary: this.endpointToEditor(spec.draft.primary),
      fallbackEnabled: fallbackProvided(spec.draft.fallback),
      fallback: spec.draft.fallback ? this.endpointToEditor(spec.draft.fallback) : this.blankEndpointEditor(),
      notes: '',
    };
    this.editorOpen = true;
  }

  public profileErrors(profile: PoolProfile): string[] {
    return validateProfile(profile);
  }

  public hasFallback(profile: PoolProfile): boolean {
    return fallbackProvided(profile.fallback);
  }

  public profileUsable(profile: PoolProfile): boolean {
    return endpointUsable(profile.primary) && validateProfile(profile).length === 0;
  }

  public chainSeverityClass(chain: PoolChain): string {
    return chain === 'BTC' ? 'nx-pill-ok' : chain === 'BCH' ? 'nx-pill-info' : 'nx-pill-neutral';
  }

  // =========================================================================
  // Chain context helpers
  // =========================================================================

  public get contextIsBch(): boolean { return this.chainContext.labelled && this.chainContext.chain === 'BCH'; }
  public get contextIsBtc(): boolean { return this.chainContext.labelled && this.chainContext.chain === 'BTC'; }

  // =========================================================================
  // Review + preflight + confirmation
  // =========================================================================

  private buildReview(profile: PoolProfile): SwitchReview {
    return buildSwitchReview(this.latestInfo, this.chainContext, profile);
  }

  public recomputePreflight(): void {
    const profile = this.selectedProfile;
    const review = profile ? (this.review ?? this.buildReview(profile)) : null;
    this.preflightResult = poolPreflight({
      info: this.latestInfo,
      online: this.online,
      onlineDetail: this.freshness.reason,
      pairStatus: this.pairStatus,
      profile,
      profileErrors: profile ? validateProfile(profile) : [],
      reviewHasChange: review?.anyChange ?? false,
      originalCaptured: this.latestInfo !== null,
      stabilityActive: this.isStabilityActive(),
      otherSwitchActive: false,
    });
  }

  private isStabilityActive(): boolean {
    return !!this.getStabilityFlag();
  }

  private getStabilityFlag(): unknown {
    try { return JSON.parse(localStorage.getItem('NX_STABILITY_ACTIVE') || 'null'); } catch { return null; }
  }

  // ---- session-only secret gating (Blocker 1/2) ----

  /** The endpoint the fallback slot will actually write (mirror when none). */
  private effectiveFallback(profile: PoolProfile): PoolEndpoint {
    return fallbackProvided(profile.fallback) ? (profile.fallback as PoolEndpoint) : profile.primary;
  }

  /** What this switch will replace, honoring the owner's Keep-current override. */
  public effectivePlan(profile: PoolProfile): { replacePrimary: boolean; replaceFallback: boolean; needsOriginal: boolean } {
    if (this.pwd.keepOverride) {
      return { replacePrimary: false, replaceFallback: false, needsOriginal: false };
    }
    const replacePrimary = endpointReplacesPassword(profile.primary);
    const replaceFallback = endpointReplacesPassword(this.effectiveFallback(profile));
    return { replacePrimary, replaceFallback, needsOriginal: replacePrimary || replaceFallback };
  }

  /** True when the target profile replaces a password (before any override). */
  public get profileReplacesPassword(): boolean {
    const p = this.selectedProfile;
    return !!p && (endpointReplacesPassword(p.primary) || endpointReplacesPassword(this.effectiveFallback(p)));
  }

  /** The explicit blocker wording when a replace needs the current password. */
  public get replacePasswordBlockerText(): string | null {
    const p = this.selectedProfile;
    if (!p) return null;
    const plan = this.effectivePlan(p);
    if (!plan.needsOriginal) return null;
    if (realSecret(this.pwd.current) !== null) return null;
    return 'The target profile replaces the pool password, but the current password cannot be read from the device. '
      + 'Enter the current password to enable verified rollback, or use Keep current password.';
  }

  /** Whether the fallback needs its OWN target password (explicit set-mode fallback). */
  public get fallbackNeedsPassword(): boolean {
    const p = this.selectedProfile;
    return !!p && !this.pwd.keepOverride && fallbackProvided(p.fallback)
      && endpointReplacesPassword(p.fallback as PoolEndpoint);
  }

  /** All required session secrets are present for the current effective plan. */
  public get secretsSatisfied(): boolean {
    const p = this.selectedProfile;
    if (!p) return false;
    const plan = this.effectivePlan(p);
    if (!plan.needsOriginal) return true; // keep-mode or Keep-current override
    const primaryOk = !plan.replacePrimary || realSecret(this.pwd.target) !== null;
    const fbOk = !this.fallbackNeedsPassword || realSecret(this.pwd.targetFallback) !== null;
    const originalOk = realSecret(this.pwd.current) !== null;
    return primaryOk && fbOk && originalOk;
  }

  /** Open the masked review + confirm dialog for a profile (Stage 4). */
  public reviewSwitch(profile: PoolProfile): void {
    if (isSwitchActive(this.snapshot.state)) {
      return;
    }
    // From a terminal state, reset to idle first so a fresh switch is clean.
    if (isSwitchTerminal(this.snapshot.state)) {
      this.snapshot = reduceSwitch(this.snapshot, { type: 'RESET', at: Date.now() });
      this.verification = null;
      this.capturedOriginal = null;
    }
    this.selectedProfile = profile;
    this.review = this.buildReview(profile);
    this.recomputePreflight();
    let snap = reduceSwitch(this.snapshot, { type: 'START_PREFLIGHT', at: Date.now() });
    if (this.preflightResult?.canStart) {
      snap = reduceSwitch(snap, { type: 'PREFLIGHT_OK', at: Date.now() });
      this.snapshot = snap;
      this.acks = { interrupt: false, change: false, rollback: false, funds: false };
      this.pwd = this.blankSecretForm();
      this.showConfirm = true;
    } else {
      const reason = this.preflightResult?.blockers.map(b => b.label).join(', ') || 'Preflight failed';
      this.snapshot = reduceSwitch(snap, { type: 'PREFLIGHT_FAIL', at: Date.now(), reason });
      this.toastr.warning('Preflight did not pass — see the blocking reasons.');
    }
  }

  public get allAcked(): boolean {
    return this.acks.interrupt && this.acks.change && this.acks.rollback && this.acks.funds;
  }

  public cancelConfirm(): void {
    this.showConfirm = false;
    this.clearSecrets();
    this.snapshot = reduceSwitch(this.snapshot, { type: 'CANCEL', at: Date.now() });
  }

  /** Owner chose "Keep current password instead" — downgrade this switch to keep-mode. */
  public useKeepCurrentPassword(): void {
    this.pwd = { ...this.blankSecretForm(), keepOverride: true };
  }

  // =========================================================================
  // Switch engine
  // =========================================================================

  private dispatch(type: SwitchEventType, extra: Partial<{ reason: string; profileId: string; profileName: string; chain: PoolChain }> = {}): void {
    this.snapshot = reduceSwitch(this.snapshot, { type, at: Date.now(), ...extra });
  }

  public confirmSwitch(): void {
    const profile = this.selectedProfile;
    if (!profile || !this.allAcked || this.snapshot.state !== 'awaiting-confirmation') return;
    // Replace-password switches must have the required session secrets, or the
    // rollback contract cannot be honoured — never proceed without them.
    if (!this.secretsSatisfied) {
      this.toastr.warning('Enter the required passwords (or Keep current password) before switching.');
      return;
    }
    const plan = this.effectivePlan(profile);
    this.replaced = { primary: plan.replacePrimary, fallback: plan.replaceFallback };
    this.passwordWasReplaced = plan.needsOriginal;
    // Move the entered secrets into the operation holder, then WIPE the form so
    // the raw values live only in memory for this operation.
    this.opSecrets = {
      primaryPassword: plan.replacePrimary ? this.pwd.target : undefined,
      fallbackPassword: this.fallbackNeedsPassword ? this.pwd.targetFallback : undefined,
      original: plan.needsOriginal ? this.pwd.current : undefined,
    };
    this.pwd = this.blankSecretForm();

    this.showConfirm = false;
    this.sessionId = `nx-pool-${Date.now().toString(36)}`;
    this.sessionStartMs = Date.now();
    this.baselineShares = {
      a: typeof this.latestInfo?.sharesAccepted === 'number' ? this.latestInfo.sharesAccepted : null,
      r: typeof this.latestInfo?.sharesRejected === 'number' ? this.latestInfo.sharesRejected : null,
    };
    this.dispatch('CONFIRM', { profileId: profile.id, profileName: profile.name, chain: profile.chain });
    this.captureAndApply(profile);
  }

  private captureAndApply(profile: PoolProfile): void {
    const info = this.latestInfo;
    if (!info) {
      this.dispatch('CAPTURE_FAIL', { reason: 'No device telemetry — cannot capture the original configuration.' });
      this.finalizeFailure();
      return;
    }
    this.capturedOriginal = captureConfig(info);
    this.dispatch('CAPTURED');
    const fbHost = fallbackProvided(profile.fallback) ? (profile.fallback as PoolEndpoint).host : profile.primary.host;
    this.verifyTarget = { primaryHost: profile.primary.host, fallbackHost: fbHost };
    // Persist a NON-SECRET interruption record so a browser interruption can be
    // recovered honestly. No password is ever written here.
    this.poolStrategy.setInterruptionRecord({
      sessionId: this.sessionId,
      at: this.sessionStartMs,
      targetProfileName: profile.name,
      targetChain: profile.chain,
      original: this.capturedOriginal,
      target: { host: profile.primary.host, port: normalizePort(profile.primary.port), user: profile.primary.user },
      passwordWasReplaced: this.passwordWasReplaced,
    });
    // Session secrets are injected here for the PATCH and never persisted.
    const secrets: SwitchSecrets = { primaryPassword: this.opSecrets.primaryPassword, fallbackPassword: this.opSecrets.fallbackPassword };
    this.applyAndRestart(profileToSettings(profile, secrets), 'switch');
  }

  /**
   * Apply settings, then restart. `flow` decides which events fire on
   * success/failure: the switch flow moves applying→restarting→reconnecting; the
   * recovery flow stays in rolling-back/restoring and tracks recoveryPhase.
   */
  private applyAndRestart(settings: any, flow: 'switch' | 'recovery'): void {
    if (flow === 'recovery') this.recoveryPhase = 'apply';
    this.systemService.updateSystem('', settings).pipe(takeUntil(this.destroy$)).subscribe({
      next: () => {
        if (flow === 'switch') {
          this.dispatch('APPLY_RESTART');
          this.sendRestart('switch');
        } else {
          this.recoveryPhase = 'restart';
          this.sendRestart('recovery');
        }
      },
      error: (err: HttpErrorResponse) => {
        if (flow === 'switch') {
          this.dispatch('APPLY_FAIL', { reason: `Applying the profile failed: ${err.message}` });
          this.beginRollback();
        } else {
          this.finishRecovery('failed', `Reapplying the configuration failed: ${err.message}`);
        }
      },
    });
  }

  private sendRestart(flow: 'switch' | 'recovery'): void {
    this.reconnectStartMono = this.nowMono();
    this.staleAtRestartMono = this.lastFreshMonoMs;
    this.systemService.restart('').pipe(takeUntil(this.destroy$)).subscribe({
      next: () => {
        if (flow === 'switch') {
          this.dispatch('RESTART_SENT');
        } else {
          this.recoveryPhase = 'reconnect';
        }
      },
      error: (err: HttpErrorResponse) => {
        if (flow === 'switch') {
          this.dispatch('RESTART_FAIL', { reason: `Restart command failed: ${err.message}` });
          this.beginRollback();
        } else {
          this.finishRecovery('failed', `Restart during recovery failed: ${err.message}`);
        }
      },
    });
  }

  // ---- heartbeat / telemetry-driven supervision ----

  private onHeartbeat(): void {
    this.refreshFreshness();
    this.superviseSwitch();
  }

  private onTelemetry(): void {
    this.superviseSwitch();
  }

  private reconnected(): boolean {
    return this.lastFreshMonoMs !== null
      && this.lastFreshMonoMs !== this.staleAtRestartMono
      && this.freshness.online;
  }

  private superviseSwitch(): void {
    const state = this.snapshot.state;
    if (state === 'reconnecting') {
      this.superviseSwitchReconnect();
    } else if (state === 'verifying') {
      this.superviseSwitchVerify();
    } else if (state === 'rolling-back' || state === 'restoring') {
      this.superviseRecovery();
    }
  }

  private superviseSwitchReconnect(): void {
    if (this.reconnected()) {
      this.dispatch('RECONNECTED');
      this.verifyStartMono = this.nowMono();
      this.evaluate();
      return;
    }
    if (this.nowMono() - this.reconnectStartMono > RECONNECT_TIMEOUT_MS) {
      this.dispatch('RECONNECT_TIMEOUT', { reason: `Device did not reconnect within ${Math.round(RECONNECT_TIMEOUT_MS / 1000)} s of the restart.` });
      this.beginRollback();
    }
  }

  private superviseSwitchVerify(): void {
    this.evaluate();
    if (this.verification?.connectedAndMining) {
      this.dispatch('VERIFY_OK');
      this.dispatch('COMPLETE');
      this.finalizeSuccess();
      return;
    }
    if (this.verification?.faulted) {
      this.dispatch('VERIFY_FAIL', { reason: this.verification.faultReason ?? 'A device fault appeared during verification.' });
      this.beginRollback();
      return;
    }
    if (this.nowMono() - this.verifyStartMono > VERIFY_TIMEOUT_MS) {
      this.dispatch('VERIFY_FAIL', { reason: `Could not verify mining on the target pool within ${Math.round(VERIFY_TIMEOUT_MS / 1000)} s.` });
      this.beginRollback();
    }
  }

  private evaluate(): void {
    this.verification = evaluatePoolVerification({
      info: this.latestInfo,
      online: this.online,
      target: this.verifyTarget,
      baselineAccepted: this.baselineShares.a,
      baselineRejected: this.baselineShares.r,
    });
    this.verification.settingsApplied = true;
  }

  // ---- rollback (Stage 8) ----

  private beginRollback(): void {
    const original = this.capturedOriginal;
    if (!original) {
      this.dispatch('ROLLBACK_FAIL', { reason: 'No captured original configuration to roll back to.' });
      this.finalizeFailure();
      return;
    }
    this.verifyTarget = { primaryHost: original.primary.host, fallbackHost: original.fallback.host };
    this.recoveryPhase = 'apply';
    // In-session rollback can fully restore the original password when the switch
    // replaced one (the owner supplied it this session). It is injected here for
    // the PATCH and never persisted.
    const plan: RestoreSecretPlan = {
      originalPassword: this.opSecrets.original,
      restorePrimary: this.replaced.primary,
      restoreFallback: this.replaced.fallback,
    };
    this.applyAndRestart(captureToSettings(original, plan), 'recovery');
  }

  public retryRollback(): void {
    if (this.snapshot.state !== 'failed') return;
    this.dispatch('RETRY_ROLLBACK');
    this.beginRollback();
  }

  // ---- manual restore (Stage 9) ----

  /** Restore is available whenever a valid snapshot exists and no switch is active. */
  public get canRestore(): boolean {
    return !isSwitchActive(this.snapshot.state)
      && (this.snapshot.state === 'complete' || this.snapshot.state === 'idle')
      && !!this.poolStrategy.getRestoreSnapshot();
  }

  public openRestore(): void {
    if (!this.canRestore) return;
    this.restoreSnapshot = this.poolStrategy.getRestoreSnapshot();
    if (!this.restoreSnapshot) {
      this.toastr.info('No restore snapshot is available (it may have expired).');
      return;
    }
    this.showRestore = true;
  }

  public cancelRestore(): void { this.showRestore = false; }

  public confirmRestore(): void {
    const snap = this.restoreSnapshot;
    if (!snap || !(this.snapshot.state === 'complete' || this.snapshot.state === 'idle')) return;
    this.showRestore = false;
    this.dispatch('RESTORE_START', { profileName: this.snapshot.targetProfileName ?? undefined });
    this.beginManualRestore(snap.config);
  }

  public retryRestore(): void {
    const snap = this.restoreSnapshot;
    if (this.snapshot.state !== 'failed' || !snap) return;
    this.dispatch('RETRY_RESTORE');
    this.beginManualRestore(snap.config);
  }

  /**
   * Reapply a saved configuration. The original password is NOT available here
   * (it was session-only and is long gone), so no password is sent — the device
   * keeps its current password and the UI says so. Reuses the recovery flow.
   */
  private beginManualRestore(config: CapturedConfig): void {
    this.replaced = { primary: false, fallback: false };
    this.opSecrets = {};
    this.baselineShares = {
      a: typeof this.latestInfo?.sharesAccepted === 'number' ? this.latestInfo.sharesAccepted : null,
      r: typeof this.latestInfo?.sharesRejected === 'number' ? this.latestInfo.sharesRejected : null,
    };
    // Non-secret interruption record for the restore operation.
    if (this.latestInfo) {
      this.poolStrategy.setInterruptionRecord({
        sessionId: this.sessionId || `nx-pool-${Date.now().toString(36)}`,
        at: Date.now(),
        targetProfileName: this.restoreSnapshot?.fromProfileName ?? null,
        targetChain: 'custom',
        original: captureConfig(this.latestInfo),
        target: { host: config.primary.host, port: normalizePort(config.primary.port), user: config.primary.user },
        passwordWasReplaced: false,
      });
    }
    this.verifyTarget = { primaryHost: config.primary.host, fallbackHost: config.fallback.host };
    this.applyAndRestart(captureToSettings(config, {}), 'recovery');
  }

  public dismissRestore(): void {
    this.poolStrategy.clearRestoreSnapshot();
    this.restoreSnapshot = null;
    this.toastr.info('Cleared the saved restore snapshot.');
  }

  // ---- shared recovery supervision (rolling-back / restoring) ----

  private superviseRecovery(): void {
    if (this.recoveryPhase === 'reconnect') {
      if (this.reconnected()) {
        this.recoveryPhase = 'verify';
        this.verifyStartMono = this.nowMono();
        this.evaluate();
      } else if (this.nowMono() - this.reconnectStartMono > RECONNECT_TIMEOUT_MS) {
        this.finishRecovery('failed', 'The device did not reconnect after reapplying the configuration.');
      }
    } else if (this.recoveryPhase === 'verify') {
      this.evaluate();
      if (this.verification?.connectedAndMining) {
        this.finishRecovery('verified');
      } else if (this.nowMono() - this.verifyStartMono > VERIFY_TIMEOUT_MS) {
        // Reconnected but full verification did not complete — partial.
        this.finishRecovery('partial');
      }
    }
  }

  private finishRecovery(result: RecoveryResult, reason?: string): void {
    this.recoveryPhase = null;
    const restoring = this.snapshot.state === 'restoring';
    if (restoring) {
      if (result === 'verified') this.dispatch('RESTORE_OK');
      else if (result === 'partial') this.dispatch('RESTORE_PARTIAL', { reason });
      else this.dispatch('RESTORE_FAIL', { reason });
      this.afterRestore(result);
    } else {
      if (result === 'verified') this.dispatch('ROLLBACK_OK');
      else if (result === 'partial') this.dispatch('ROLLBACK_PARTIAL', { reason });
      else this.dispatch('ROLLBACK_FAIL', { reason });
      this.finalizeFailure();
    }
  }

  // ---- finalization ----

  private finalizeSuccess(): void {
    const profile = this.selectedProfile;
    if (profile && this.latestInfo) {
      const record: ActivePoolRecord = {
        profileId: profile.id,
        profileName: profile.name,
        chain: profile.chain,
        primaryHost: profile.primary.host,
        primaryPort: normalizePort(profile.primary.port),
        primaryUser: profile.primary.user,
        appliedAt: Date.now(),
      };
      this.poolStrategy.setActiveRecord(record);
    }
    if (this.capturedOriginal) {
      const restore = buildRestoreSnapshot({
        config: this.capturedOriginal,
        now: Date.now(),
        fromProfileName: profile?.name ?? null,
        passwordWasReplaced: this.passwordWasReplaced,
      });
      this.poolStrategy.setRestoreSnapshot(restore);
      this.restoreSnapshot = restore;
    }
    this.poolStrategy.clearSwitchActive();
    this.resolveInterruption();
    this.saveHistory();     // reads this.replaced — must run BEFORE clearSecrets
    this.clearSecrets();
    this.toastr.success(`Switched to "${profile?.name}" — reconnect and mining verified. The password was never read back; a successful pool connection implies it was accepted.`);
  }

  private afterRestore(result: RecoveryResult): void {
    this.poolStrategy.clearSwitchActive();
    if (result === 'verified') {
      // The device is back on the original config — the active labelled profile
      // is no longer applied, so the chain context becomes Unknown until a new
      // labelled switch. Clear the used restore snapshot and any interruption.
      this.poolStrategy.clearActiveRecord();
      this.poolStrategy.clearRestoreSnapshot();
      this.restoreSnapshot = null;
      this.resolveInterruption();
      this.toastr.success('Restored the previous pool configuration — verified by telemetry.');
    } else {
      this.toastr.warning('Restore could not be fully verified — check the device. The previous pool is NOT confirmed restored.');
    }
    this.saveHistory();
    this.clearSecrets();
  }

  private finalizeFailure(): void {
    this.poolStrategy.clearSwitchActive();
    // Only clear the interruption record when the device was verified back on the
    // original pool; otherwise keep it so a later reload can still recover.
    if (this.snapshot.rollbackResult === 'verified') {
      this.resolveInterruption();
    }
    this.saveHistory();     // reads this.replaced — must run BEFORE clearSecrets
    this.clearSecrets();
    this.toastr.error(this.snapshot.reason ?? 'The pool switch failed.');
  }

  /** Clear the persisted interruption record and its recovery UI state. */
  private resolveInterruption(): void {
    this.poolStrategy.clearSwitchActive();
    this.interruptionRecord = null;
    this.interruptionRecovery = null;
    this.interruptedNotice = false;
  }

  private saveHistory(): void {
    const profile = this.selectedProfile;
    const original = this.capturedOriginal;
    const changes = profile && original ? [
      maskedHostChange('primary', original.primary, profile.primary),
      maskedHostChange('fallback', original.fallback, fallbackProvided(profile.fallback) ? (profile.fallback as PoolEndpoint) : profile.primary),
    ] : [];
    const info = this.latestInfo;
    const record = buildSwitchRecord({
      id: this.sessionId || `nx-pool-${Date.now().toString(36)}`,
      startedAt: this.sessionStartMs || Date.now(),
      finishedAt: Date.now(),
      source: {
        profileId: null,
        profileName: this.chainContext.profileName,
        chain: this.chainContext.chain,
      },
      target: {
        profileId: profile?.id ?? null,
        profileName: profile?.name ?? null,
        chain: profile?.chain ?? 'custom',
      },
      changes,
      // Reflect what this operation ACTUALLY replaced (honoring Keep-current), not
      // just the profile's declared mode. Booleans only — never a value.
      credentialsReplaced: { primary: this.replaced.primary, fallback: this.replaced.fallback },
      finalState: this.snapshot.state,
      switchVerified: this.snapshot.switchVerified,
      rollbackResult: this.snapshot.rollbackResult,
      restoreResult: this.snapshot.restoreResult,
      reason: this.snapshot.reason,
      timeline: this.snapshot.timeline,
      device: info ? {
        productName: info.productName, productVersion: info.productVersion,
        targetDevice: info.targetDevice, targetBoard: info.targetBoard, targetAsic: info.targetAsic,
        firmware: info.version,
      } : undefined,
    });
    this.history = this.poolStrategy.saveHistory(record);
  }

  // ---- abort ----

  public abortSwitch(): void {
    if (!isSwitchActive(this.snapshot.state)) return;
    // Abort routes through rollback so a partially-applied switch is never left active.
    if (this.snapshot.state === 'capturing') {
      this.dispatch('ABORT', { reason: 'Owner aborted before any change.' });
      this.poolStrategy.clearSwitchActive();
      this.resolveInterruption();
      this.saveHistory();
      this.clearSecrets();
      return;
    }
    this.dispatch('ABORT', { reason: 'Owner aborted the switch.' });
    if (this.snapshot.state === 'rolling-back') this.beginRollback();
  }

  public resetSwitch(): void {
    if (isSwitchActive(this.snapshot.state)) return;
    this.dispatch('RESET');
    this.verification = null;
    this.selectedProfile = null;
    this.review = null;
    this.capturedOriginal = null;
    this.clearSecrets();
  }

  // ---- interrupted-switch recovery actions (Blocker 4) ----

  /** Reapply the recorded original configuration after an interruption (no secret available). */
  public retryInterruptionRollback(): void {
    const rec = this.interruptionRecord;
    if (!rec || isSwitchActive(this.snapshot.state)) return;
    // Fresh restoring operation from idle to the recorded original config.
    if (isSwitchTerminal(this.snapshot.state)) {
      this.snapshot = reduceSwitch(this.snapshot, { type: 'RESET', at: Date.now() });
    }
    this.sessionId = rec.sessionId;
    this.sessionStartMs = Date.now();
    this.dispatch('RESTORE_START', { profileName: rec.targetProfileName ?? undefined });
    this.beginManualRestore(rec.original);
  }

  /** Recompute the recovery state from live telemetry (owner "verify" action). */
  public verifyCurrentConfiguration(): void {
    this.refreshInterruptionRecovery();
    if (!this.latestInfo) {
      this.toastr.info('No fresh telemetry yet — reconnect and try again.');
      return;
    }
    const r = this.interruptionRecovery;
    this.toastr.info(r ? r.headline : 'No interrupted switch to verify.');
  }

  /** Dismiss the interrupted-switch recovery notice without acting. */
  public dismissInterruption(): void {
    this.resolveInterruption();
  }

  // =========================================================================
  // History + export
  // =========================================================================

  public exportHistory(format: 'json' | 'md'): void {
    const records = this.history;
    // Guard: prove no credential ever reaches an export.
    if (containsForbiddenKeys(records)) {
      this.toastr.error('Export blocked: the history contained an unexpected sensitive field.');
      return;
    }
    const content = format === 'json' ? exportJson(records) : exportMarkdown(records);
    const mime = format === 'json' ? 'application/json' : 'text/markdown';
    this.download(`neuralaxe-pool-history.${format}`, content, mime);
  }

  public deleteHistory(id: string): void {
    this.history = this.poolStrategy.removeHistory(id);
  }

  public clearHistory(): void {
    this.poolStrategy.clearHistory();
    this.history = [];
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

  // =========================================================================
  // Display helpers
  // =========================================================================

  public isActiveState(): boolean { return isSwitchActive(this.snapshot.state); }
  public isTerminalState(): boolean { return isSwitchTerminal(this.snapshot.state); }
  public get canReview(): boolean {
    return isSwitchTerminalOrIdle(this.snapshot.state);
  }

  public rollbackResultText(r: RecoveryResult | null): string {
    if (r === 'verified') return 'Rolled back and verified';
    if (r === 'partial') return 'Rolled back — partially verified';
    if (r === 'failed') return 'Rollback failed — check the device';
    return '—';
  }

  public verificationRows() {
    return this.verification ? verificationLevels(this.verification) : [];
  }

  public trackById(_i: number, item: { id: string }): string { return item.id; }
  public trackByIndex(i: number): number { return i; }

  public fmtTime(ms: number): string {
    return new Date(ms).toLocaleString();
  }
}

/** A switch may be reviewed only from idle or a terminal state. */
function isSwitchTerminalOrIdle(state: string): boolean {
  return state === 'idle' || state === 'complete' || state === 'aborted' || state === 'failed' || state === 'interrupted';
}
