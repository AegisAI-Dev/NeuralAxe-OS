/**
 * NeuralAxe Timed Pool Session dashboard (Phase 2M.1B, Gate B9).
 *
 * The operator surface for the committed Gate B8 control API. It does four
 * things and nothing else: read the sanitized status, submit ONE bounded
 * create, submit ONE Restore Now, submit ONE acknowledgement.
 *
 * THE DEVICE IS AUTHORITATIVE.
 *  - HTTP 202 means "queued", never "started". Progress is read back from the
 *    status route and from nowhere else.
 *  - Nothing is inferred from elapsed browser time.
 *  - No command is ever retried automatically; a lost response is surfaced,
 *    never resent.
 *
 * PRIVACY.
 *  - The form asks for the TARGET only. There is no password control, no
 *    source control and no session-identifier control — the committed request
 *    schema has none of them.
 *  - Target host/account live only in `form`, a plain in-memory object. They
 *    are never written to localStorage, sessionStorage, IndexedDB, a URL,
 *    router state, a toast, a log or the command banner, and they are cleared
 *    on an accepted submission and on destroy.
 */

import { Component, OnDestroy, OnInit } from '@angular/core';
import { ToastrService } from 'ngx-toastr';
import { Subject, Subscription, fromEvent, timer } from 'rxjs';
import { distinctUntilChanged, map, startWith, takeUntil } from 'rxjs/operators';

import { TimedSessionStatus } from 'src/app/generated/models';
import {
  MockPosture, TimedSessionApiService, TimedSessionFailure,
} from 'src/app/services/timed-session.service';
import {
  AvailabilityView, CommandKind, CommandView, TimedSessionAvailability, TimedSessionView, TimelineView,
} from './timed-session.models';
import {
  HEARTBEAT_NOTE, availabilityView, buildView, commandView, conflictView,
} from './timed-session-mapper';
import { buildTimeline } from './timed-session-timeline';
import {
  ACK_CONFIRM_STATEMENTS, CHAIN_OPTIONS, CREATE_CONFIRM_STATEMENTS, DURATION_PRESETS,
  FormCheck, FormField, MAX_ACCOUNT_LEN, MAX_HOST_LEN, PROTOCOL_OPTIONS,
  RESTORE_CONFIRM_STATEMENTS, ReviewRow, TLS_OPTIONS, TimedSessionFormValue,
  buildCreateRequest, buildReview, checkForm, emptyForm,
} from './timed-session-form';
import {
  availabilityForFailure, cadenceFor, nextBackoffMs, stopsPolling,
} from './timed-session-polling';
import { CommandBaseline, captureBaseline, resolveCommand } from './timed-session-correlation';
import { RequestIdAllocator, createRequestIdAllocator } from './timed-session-request-id';

@Component({
  selector: 'app-timed-session',
  templateUrl: './timed-session.component.html',
})
export class TimedSessionComponent implements OnInit, OnDestroy {
  private readonly destroy$ = new Subject<void>();
  private tickSub: Subscription | null = null;
  private requestSub: Subscription | null = null;

  /** Bounded consecutive device/network failures driving the backoff. */
  private consecutiveFailures = 0;
  /**
   * Non-wrapping request-number source for this component lifetime. It fails
   * closed rather than reusing a number the device may still be reporting.
   */
  private requestIds: RequestIdAllocator = createRequestIdAllocator();
  /** What the device reported immediately before the in-flight command. */
  private commandBaseline: CommandBaseline | null = null;
  /** Single-flight guard: exactly one status read is ever in the air. */
  private statusInFlight = false;
  private visible = true;

  // ---- exposed constants -------------------------------------------------
  public readonly protocolOptions = PROTOCOL_OPTIONS;
  public readonly tlsOptions = TLS_OPTIONS;
  public readonly chainOptions = CHAIN_OPTIONS;
  public readonly durationPresets = DURATION_PRESETS;
  public readonly createStatements = CREATE_CONFIRM_STATEMENTS;
  public readonly restoreStatements = RESTORE_CONFIRM_STATEMENTS;
  public readonly ackStatements = ACK_CONFIRM_STATEMENTS;
  public readonly heartbeatNote = HEARTBEAT_NOTE;
  public readonly maxHostLen = MAX_HOST_LEN;
  public readonly maxAccountLen = MAX_ACCOUNT_LEN;

  // ---- availability ------------------------------------------------------
  public availability: TimedSessionAvailability = 'CHECKING';
  public availabilityInfo: AvailabilityView = availabilityView('CHECKING');

  // ---- status ------------------------------------------------------------
  public status: TimedSessionStatus | null = null;
  public view: TimedSessionView | null = null;
  public timeline: TimelineView | null = null;
  /** Announced to screen readers when the durable state changes. */
  public liveAnnouncement = '';
  private lastAnnounced = '';

  // ---- form (in-memory ONLY) ---------------------------------------------
  public form: TimedSessionFormValue = emptyForm();
  public formCheck: FormCheck = checkForm(this.form);
  public formTouched = false;

  // ---- dialogs -----------------------------------------------------------
  public showCreateReview = false;
  public showRestoreConfirm = false;
  public showAckConfirm = false;
  public reviewRows: readonly ReviewRow[] = [];
  public createAcknowledged = false;
  public restoreAcknowledged = false;
  public ackAcknowledged = false;

  // ---- command banner ----------------------------------------------------
  public command: CommandView = commandView(null, 'idle');
  /** True while a POST is in flight; every submit button is disabled. */
  public submitting = false;

  constructor(
    private api: TimedSessionApiService,
    private toastr: ToastrService,
  ) {}

  // =========================================================================
  // Lifecycle
  // =========================================================================

  public ngOnInit(): void {
    fromEvent(document, 'visibilitychange')
      .pipe(
        map(() => document.visibilityState === 'visible'),
        startWith(document.visibilityState === 'visible'),
        distinctUntilChanged(),
        takeUntil(this.destroy$),
      )
      .subscribe((visible) => {
        this.visible = visible;
        // Returning to the tab triggers exactly ONE immediate refresh.
        if (visible && !this.pollingStopped) {
          this.scheduleTick(0);
        } else if (!this.pollingStopped) {
          this.scheduleTick(cadenceFor({ busy: this.busy, visible }));
        }
      });

    this.readStatus();
  }

  public ngOnDestroy(): void {
    this.destroy$.next();
    this.destroy$.complete();
    this.tickSub?.unsubscribe();
    this.tickSub = null;
    this.requestSub?.unsubscribe();
    this.requestSub = null;
    // Sensitive target values never outlive the component.
    this.clearSensitiveForm();
  }

  // =========================================================================
  // Polling
  // =========================================================================

  public get pollingStopped(): boolean {
    return stopsPolling(this.availability);
  }

  private get busy(): boolean {
    return this.view !== null && (this.view.sessionPresent || this.status?.commandPending === true);
  }

  private scheduleTick(delayMs: number): void {
    this.tickSub?.unsubscribe();
    this.tickSub = timer(delayMs)
      .pipe(takeUntil(this.destroy$))
      .subscribe(() => this.readStatus());
  }

  /** ONE status read. Never overlaps; never retries a command. */
  private readStatus(): void {
    if (this.statusInFlight) {
      return;
    }
    this.statusInFlight = true;
    this.api
      .getStatus()
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        next: (status) => {
          this.statusInFlight = false;
          this.consecutiveFailures = 0;
          this.applyStatus(status);
          this.setAvailability('AVAILABLE');
          this.scheduleTick(cadenceFor({ busy: this.busy, visible: this.visible }));
        },
        error: (failure: TimedSessionFailure) => {
          this.statusInFlight = false;
          const next = availabilityForFailure(failure);
          this.setAvailability(next);
          if (stopsPolling(next)) {
            this.tickSub?.unsubscribe();
            this.tickSub = null;
            return;
          }
          this.consecutiveFailures += 1;
          this.scheduleTick(nextBackoffMs(this.consecutiveFailures));
        },
      });
  }

  /** Operator-initiated capability re-check. Also the only way out of a stop. */
  public checkAgain(): void {
    this.consecutiveFailures = 0;
    this.setAvailability('CHECKING');
    this.readStatus();
  }

  /** Operator-initiated single refresh; never queues a command. */
  public refreshNow(): void {
    this.readStatus();
  }

  private setAvailability(a: TimedSessionAvailability): void {
    this.availability = a;
    this.availabilityInfo = availabilityView(a);
    if (a !== 'AVAILABLE') {
      this.view = null;
      this.timeline = null;
      this.status = null;
    }
  }

  private applyStatus(status: TimedSessionStatus): void {
    this.status = status;
    this.view = buildView(status);
    this.timeline = buildTimeline(status);

    this.resolveCommandFrom(status);

    const announce = `${this.view.durable.label}. ${this.view.grant.label} target mining.`;
    if (announce !== this.lastAnnounced) {
      this.lastAnnounced = announce;
      this.liveAnnouncement = announce;
    }
  }

  /**
   * Moves a queued command forward using ONLY what the device reports.
   *
   * The full contract lives in `timed-session-correlation.ts`: the echoed
   * request number, the echoed command kind, a status sequence strictly newer
   * than the pre-submit baseline, AND the command-specific state transition
   * must all hold. Anything less leaves the command queued, which is always
   * the truthful answer — the device simply has not reported on it yet.
   */
  private resolveCommandFrom(status: TimedSessionStatus): void {
    const kind = this.command.kind;
    const inFlight = this.command.phase === 'accepted' || this.command.phase === 'processing';
    if (kind === null || !inFlight) {
      return;
    }
    const carry = {
      requestSequence: this.command.requestSequence,
      clientRequestId: this.command.clientRequestId,
    };
    const resolution = resolveCommand({
      kind,
      clientRequestId: this.command.clientRequestId,
      baseline: this.commandBaseline,
      status,
    });
    switch (resolution.outcome) {
      case 'processing':
        if (this.command.phase === 'accepted') {
          this.command = commandView(kind, 'processing', carry);
        }
        return;
      case 'posture-reached':
        this.command = commandView(kind, 'posture-reached', carry);
        return;
      case 'refused':
        this.command = commandView(kind, 'device-refused', carry);
        return;
      case 'queued':
      default:
        return;   // Say nothing. Nothing attributable has been observed.
    }
  }

  /**
   * Allocate the next request number, or refuse.
   *
   * Returns `null` when the allocator is exhausted; the caller must then send
   * NOTHING. Reusing a number would let a result the device retained for an
   * earlier command be read as the answer to a new one — the exact confusion
   * the number exists to prevent — so the page fails closed and asks for a
   * reload instead.
   */
  private takeClientRequestId(kind: CommandKind): number | null {
    const id = this.requestIds.next();
    if (id === null) {
      this.command = commandView(kind, 'request-ids-exhausted');
      this.submitting = false;
      return null;
    }
    return id;
  }


  // =========================================================================
  // Form
  // =========================================================================

  public onFormChanged(): void {
    this.formTouched = true;
    this.formCheck = checkForm(this.form);
  }

  public applyPreset(hours: number, minutes: number): void {
    this.form.hours = hours;
    this.form.minutes = minutes;
    this.onFormChanged();
  }

  public problemFor(field: FormField): string {
    if (!this.formTouched) {
      return '';
    }
    return this.formCheck.problems.find((p) => p.field === field)?.message ?? '';
  }

  /**
   * Wipe every sensitive in-memory value. Called after an accepted submission
   * and on destroy. The enum selections are structural, not identities, and
   * are reset with the rest for a clean form.
   */
  private clearSensitiveForm(): void {
    this.form = emptyForm();
    this.formCheck = checkForm(this.form);
    this.formTouched = false;
    this.reviewRows = [];
  }

  // =========================================================================
  // Create
  // =========================================================================

  public openCreateReview(): void {
    this.formTouched = true;
    this.formCheck = checkForm(this.form);
    if (!this.formCheck.valid) {
      return;
    }
    // The review shows ONLY the values the operator just entered.
    this.reviewRows = buildReview(this.form);
    this.createAcknowledged = false;
    this.showCreateReview = true;
  }

  public cancelCreateReview(): void {
    this.showCreateReview = false;
    this.reviewRows = [];
  }

  public confirmCreate(): void {
    if (this.submitting || !this.createAcknowledged) {
      return;
    }
    if (!this.status) {
      return;   // No baseline means nothing could ever be correlated. Do not send.
    }
    const clientRequestId = this.takeClientRequestId('create');
    if (clientRequestId === null) {
      this.showCreateReview = false;
      return;
    }
    this.commandBaseline = captureBaseline(this.status);
    const body = { ...buildCreateRequest(this.form), client_request_id: clientRequestId };
    this.submitting = true;
    this.command = commandView('create', 'submitting', { clientRequestId });
    this.showCreateReview = false;
    this.requestSub?.unsubscribe();
    this.requestSub = this.api
      .createSession(body)
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        next: (accepted) => {
          this.submitting = false;
          this.command = commandView('create', 'accepted', {
            requestSequence: accepted.requestSequence, clientRequestId,
          });
          // No identity is echoed: the toast names the action only.
          this.toastr.info('The device queued the request.', 'Accepted for processing');
          this.clearSensitiveForm();
          this.scheduleTick(0);
        },
        error: (failure: TimedSessionFailure) => this.onCommandFailure('create', failure),
      });
  }

  // =========================================================================
  // Restore Now
  // =========================================================================

  public openRestoreConfirm(): void {
    this.restoreAcknowledged = false;
    this.showRestoreConfirm = true;
  }

  public confirmRestore(): void {
    if (this.submitting || !this.restoreAcknowledged) {
      return;
    }
    if (!this.status) {
      return;
    }
    const clientRequestId = this.takeClientRequestId('restore');
    if (clientRequestId === null) {
      this.showRestoreConfirm = false;
      return;
    }
    this.commandBaseline = captureBaseline(this.status);
    this.submitting = true;
    this.command = commandView('restore', 'submitting', { clientRequestId });
    this.showRestoreConfirm = false;
    this.requestSub?.unsubscribe();
    this.requestSub = this.api
      .restoreNow({ client_request_id: clientRequestId })
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        next: (accepted) => {
          this.submitting = false;
          this.command = commandView('restore', 'accepted', {
            requestSequence: accepted.requestSequence, clientRequestId,
          });
          this.toastr.info('The device queued the restoration request.', 'Accepted for processing');
          this.scheduleTick(0);
        },
        error: (failure: TimedSessionFailure) => this.onCommandFailure('restore', failure),
      });
  }

  // =========================================================================
  // Acknowledgement
  // =========================================================================

  public openAckConfirm(): void {
    this.ackAcknowledged = false;
    this.showAckConfirm = true;
  }

  public confirmAcknowledge(): void {
    if (this.submitting || !this.ackAcknowledged) {
      return;
    }
    if (!this.status) {
      return;
    }
    const clientRequestId = this.takeClientRequestId('acknowledge');
    if (clientRequestId === null) {
      this.showAckConfirm = false;
      return;
    }
    this.commandBaseline = captureBaseline(this.status);
    this.submitting = true;
    this.command = commandView('acknowledge', 'submitting', { clientRequestId });
    this.showAckConfirm = false;
    this.requestSub?.unsubscribe();
    this.requestSub = this.api
      .acknowledgeTerminal({ client_request_id: clientRequestId })
      .pipe(takeUntil(this.destroy$))
      .subscribe({
        next: (accepted) => {
          this.submitting = false;
          this.command = commandView('acknowledge', 'accepted', {
            requestSequence: accepted.requestSequence, clientRequestId,
          });
          this.toastr.info('The device queued the acknowledgement.', 'Accepted for processing');
          this.scheduleTick(0);
        },
        error: (failure: TimedSessionFailure) => this.onCommandFailure('acknowledge', failure),
      });
  }

  // =========================================================================
  // Shared failure handling — NEVER retries
  // =========================================================================

  private onCommandFailure(kind: CommandKind, failure: TimedSessionFailure): void {
    this.submitting = false;
    switch (failure?.kind) {
      case 'validation':
        this.command = commandView(kind, 'validation-failed', { validationCode: failure.validationCode });
        break;
      case 'conflict':
        this.command = commandView(kind, 'conflict', { conflict: conflictView(failure.conflict) });
        break;
      case 'unavailable':
        this.command = commandView(kind, 'temporarily-unavailable');
        break;
      case 'origin-denied':
        this.command = commandView(kind, 'origin-denied');
        this.setAvailability('ORIGIN_DENIED');
        break;
      case 'not-present':
        this.command = commandView(kind, 'failed');
        this.setAvailability('API_DISABLED_OR_NOT_PRESENT');
        break;
      case 'offline':
        this.command = commandView(kind, 'failed');
        break;
      default:
        this.command = commandView(kind, 'unknown');
        break;
    }
    // Exactly one status refresh — never a resubmission.
    if (!this.pollingStopped) {
      this.scheduleTick(0);
    }
  }

  // =========================================================================
  // Development mock switch (never present in production builds)
  // =========================================================================

  public get mocked(): boolean {
    return this.api.isMocked;
  }

  public get mockPosture(): MockPosture {
    return this.api.mockPosture;
  }

  public setMockPosture(posture: MockPosture): void {
    this.api.mockPosture = posture;
    this.checkAgain();
  }

  public readonly mockPostures: readonly MockPosture[] = [
    'not-present', 'free', 'queued', 'applying', 'verifying', 'mining', 'restoring',
    'complete-pending-ack', 'restore-failed', 'recovery-guard', 'time-untrusted',
    'heartbeat-failed', 'conflict', 'mailbox-unavailable',
    'api-disabled', 'execution-disabled', 'runtime-starting',
  ];

  // =========================================================================
  // Template helpers
  // =========================================================================

  /** Maps a semantic severity onto the existing `nx-pill-*` tokens. */
  public pillClass(severity: string): string {
    switch (severity) {
      case 'ok': return 'nx-pill-ok';
      case 'info': return 'nx-pill-info';
      case 'warn': return 'nx-pill-warn';
      case 'danger': return 'nx-pill-err';
      default: return 'nx-pill-neutral';
    }
  }

  public stageIcon(state: string): string {
    switch (state) {
      case 'done': return 'pi pi-check-circle';
      case 'active': return 'pi pi-spin pi-spinner';
      case 'failed': return 'pi pi-times-circle';
      case 'skipped': return 'pi pi-minus-circle';
      default: return 'pi pi-circle';
    }
  }

  /** Textual reason a submit button is disabled (never colour alone). */
  public get createDisabledReason(): string {
    if (this.submitting) {
      return 'A request is already being sent.';
    }
    const v = this.view;
    if (v && !v.capable) {
      return this.incapableReason;
    }
    if (v?.operatorRecoveryRequired) {
      return 'The device needs operator recovery before it will accept anything new.';
    }
    if (v?.terminalAckRequired) {
      return 'The result of the previous session is still retained. Acknowledge it first.';
    }
    if (v?.sessionPresent) {
      return 'A timed session already exists on the device. Only one runs at a time.';
    }
    if (!v?.createOffered) {
      return 'The device is not currently able to start a new session.';
    }
    if (!this.formCheck.valid) {
      return 'Complete the target details above.';
    }
    return '';
  }

  /**
   * Why a device that ANSWERED cannot nonetheless run a timed session. Each
   * case is a different build or runtime posture and needs a different action
   * from the operator, so none of them is collapsed into one vague sentence.
   */
  public get incapableReason(): string {
    const v = this.view;
    if (!v) {
      return '';
    }
    if (!v.apiEnabled) {
      return 'This firmware build answers the status route but has the timed-session control API turned off. '
        + 'No session can be created until a build with the API enabled is installed.';
    }
    if (!v.executionEnabled) {
      return 'This firmware build reports the timed-session runtime but not the execution layer, so it can '
        + 'describe a session and never carry one out. Nothing here would reach the pool.';
    }
    if (!v.runtimeInitialized) {
      return 'The timed-session runtime on the device has not finished starting. Nothing can be queued for it '
        + 'yet. This normally clears on its own shortly after boot.';
    }
    return '';
  }
}
