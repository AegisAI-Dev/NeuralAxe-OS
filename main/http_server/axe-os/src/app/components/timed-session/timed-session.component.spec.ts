/**
 * Gate B9 — component tests.
 *
 * Availability detection, single-flight polling and teardown, the create
 * review step, 202-is-not-started, no automatic retry, no duplicate
 * submission, and the privacy contract for the in-memory form.
 *
 * The API service is a deterministic stub: no HTTP, no timers beyond the
 * component's own, no outbound networking.
 */

import { ComponentFixture, TestBed, fakeAsync, tick, discardPeriodicTasks } from '@angular/core/testing';
import { NO_ERRORS_SCHEMA } from '@angular/core';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { FormsModule } from '@angular/forms';
import { ToastrService } from 'ngx-toastr';
import { Observable, Subject, of, throwError } from 'rxjs';

import { TimedSessionComponent } from './timed-session.component';
import {
  MockPosture, TimedSessionApiService, TimedSessionFailure, mockStatus,
} from 'src/app/services/timed-session.service';
import { TimedSessionAccepted, TimedSessionStatus } from 'src/app/generated/models';
import { ACTIVE_CADENCE_MS, BACKOFF_BASE_MS, IDLE_CADENCE_MS } from './timed-session-polling';
import { RequestIdAllocator, createRequestIdAllocator } from './timed-session-request-id';

const MARKER_HOST = 'zz-marker-host.example';
const MARKER_ACCOUNT = 'zz-marker-account.worker';

/**
 * A toast stub. The real ToastrService schedules dismissal timers that the
 * component does not own and cannot cancel, which would leave `fakeAsync`
 * timers outstanding at teardown; the assertions here only ever care about
 * WHAT was announced, never about the toast widget itself.
 */
class StubToastr {
  public calls: { level: string; message: string; title?: string }[] = [];
  public success(message: string, title?: string): void { this.calls.push({ level: 'success', message, title }); }
  public info(message: string, title?: string): void { this.calls.push({ level: 'info', message, title }); }
  public warning(message: string, title?: string): void { this.calls.push({ level: 'warning', message, title }); }
  public error(message: string, title?: string): void { this.calls.push({ level: 'error', message, title }); }
}

class StubApi {
  public mockPosture: MockPosture = 'free';
  public isMocked = false;

  public statusCalls = 0;
  public createCalls = 0;
  public restoreCalls = 0;
  public ackCalls = 0;
  /** Every create body the component sent, for privacy assertions. */
  public createBodies: unknown[] = [];
  /** Every Restore Now / acknowledge body the component sent. */
  public actionBodies: unknown[] = [];

  public statusResponse: () => Observable<TimedSessionStatus> = () => of(mockStatus('free', 1));
  public createResponse: () => Observable<TimedSessionAccepted> =
    () => of({ accepted: true, requestSequence: 11 });
  public restoreResponse: () => Observable<TimedSessionAccepted> =
    () => of({ accepted: true, requestSequence: 12 });
  public ackResponse: () => Observable<TimedSessionAccepted> =
    () => of({ accepted: true, requestSequence: 13 });

  public getStatus(): Observable<TimedSessionStatus> {
    this.statusCalls += 1;
    return this.statusResponse();
  }
  public createSession(b: unknown): Observable<TimedSessionAccepted> {
    this.createCalls += 1;
    this.createBodies.push(b);
    return this.createResponse();
  }
  public restoreNow(b: unknown = {}): Observable<TimedSessionAccepted> {
    this.restoreCalls += 1;
    this.actionBodies.push(b);
    return this.restoreResponse();
  }
  public acknowledgeTerminal(b: unknown = {}): Observable<TimedSessionAccepted> {
    this.ackCalls += 1;
    this.actionBodies.push(b);
    return this.ackResponse();
  }
}

interface Ctx {
  fixture: ComponentFixture<TimedSessionComponent>;
  component: TimedSessionComponent;
  api: StubApi;
}

const fixtures: ComponentFixture<TimedSessionComponent>[] = [];

function build(): Ctx {
  const api = new StubApi();
  TestBed.configureTestingModule({
    declarations: [TimedSessionComponent],
    imports: [NoopAnimationsModule, FormsModule],
    providers: [
      { provide: ToastrService, useValue: new StubToastr() },
      { provide: TimedSessionApiService, useValue: api },
    ],
    schemas: [NO_ERRORS_SCHEMA],
  });
  const fixture = TestBed.createComponent(TimedSessionComponent);
  fixtures.push(fixture);
  return { fixture, component: fixture.componentInstance, api };
}

function fillValidForm(c: TimedSessionComponent): void {
  c.form.hours = 1;
  c.form.minutes = 0;
  c.form.targetHost = MARKER_HOST;
  c.form.targetPort = 3334;
  c.form.targetAccount = MARKER_ACCOUNT;
  c.onFormChanged();
}

afterEach(() => {
  TestBed.resetTestingModule();
  fixtures.length = 0;
});

// ===========================================================================
// Availability
// ===========================================================================

describe('TimedSessionComponent: availability', () => {
  it('becomes AVAILABLE on a 200 status', fakeAsync(() => {
    const { fixture, component } = build();
    fixture.detectChanges();
    tick();
    expect(component.availability).toBe('AVAILABLE');
    expect(component.availabilityInfo.operable).toBeTrue();
    expect(component.view).not.toBeNull();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('an initial 404 becomes API_DISABLED_OR_NOT_PRESENT and STOPS polling', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => throwError(() => ({ kind: 'not-present' } as TimedSessionFailure));
    fixture.detectChanges();
    tick();

    expect(component.availability).toBe('API_DISABLED_OR_NOT_PRESENT');
    expect(component.pollingStopped).toBeTrue();
    expect(component.view).toBeNull();
    const after = api.statusCalls;

    // No further request is ever scheduled, however long we wait.
    tick(120000);
    expect(api.statusCalls).toBe(after);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('a manual re-check is the only way out of a stopped state', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => throwError(() => ({ kind: 'not-present' } as TimedSessionFailure));
    fixture.detectChanges();
    tick();
    const stopped = api.statusCalls;

    api.statusResponse = () => of(mockStatus('free', 2));
    component.checkAgain();
    tick();
    expect(api.statusCalls).toBe(stopped + 1);
    expect(component.availability).toBe('AVAILABLE');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('401 becomes ORIGIN_DENIED and stops polling', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => throwError(() => ({ kind: 'origin-denied' } as TimedSessionFailure));
    fixture.detectChanges();
    tick();
    expect(component.availability).toBe('ORIGIN_DENIED');
    expect(component.pollingStopped).toBeTrue();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('a transport error backs off and keeps retrying, bounded', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => throwError(() => ({ kind: 'offline' } as TimedSessionFailure));
    fixture.detectChanges();
    tick();
    expect(component.availability).toBe('DEVICE_OFFLINE');
    expect(component.pollingStopped).toBeFalse();

    const first = api.statusCalls;
    tick(BACKOFF_BASE_MS);
    expect(api.statusCalls).toBe(first + 1);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('503 becomes TEMPORARILY_UNAVAILABLE', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => throwError(() => ({ kind: 'unavailable' } as TimedSessionFailure));
    fixture.detectChanges();
    tick();
    expect(component.availability).toBe('TEMPORARILY_UNAVAILABLE');
    fixture.destroy();
    discardPeriodicTasks();
  }));
});

// ===========================================================================
// Polling
// ===========================================================================

describe('TimedSessionComponent: polling', () => {
  it('uses the idle cadence when nothing is running', fakeAsync(() => {
    const { fixture, api } = build();
    fixture.detectChanges();
    tick();
    const after = api.statusCalls;

    tick(IDLE_CADENCE_MS - 1);
    expect(api.statusCalls).toBe(after);
    tick(1);
    expect(api.statusCalls).toBe(after + 1);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('uses the fast cadence while a session is running', fakeAsync(() => {
    const { fixture, api } = build();
    api.statusResponse = () => of(mockStatus('mining', 3));
    fixture.detectChanges();
    tick();
    const after = api.statusCalls;

    tick(ACTIVE_CADENCE_MS);
    expect(api.statusCalls).toBe(after + 1);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('never overlaps status requests', fakeAsync(() => {
    const { fixture, api } = build();
    const gate = new Subject<TimedSessionStatus>();
    api.statusResponse = () => gate.asObservable();
    fixture.detectChanges();
    tick();
    expect(api.statusCalls).toBe(1);

    // A second read is requested while the first is still in flight.
    (fixture.componentInstance as unknown as { readStatus(): void }).readStatus();
    expect(api.statusCalls).toBe(1);

    gate.next(mockStatus('free', 2));
    gate.complete();
    tick();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('stops polling completely on destroy', fakeAsync(() => {
    const { fixture, api } = build();
    fixture.detectChanges();
    tick();
    const after = api.statusCalls;

    fixture.destroy();
    tick(120000);
    expect(api.statusCalls).toBe(after);
    discardPeriodicTasks();
  }));
});

// ===========================================================================
// Create
// ===========================================================================

describe('TimedSessionComponent: create', () => {
  it('requires a valid form before the review opens', fakeAsync(() => {
    const { fixture, component } = build();
    fixture.detectChanges();
    tick();

    component.openCreateReview();
    expect(component.showCreateReview).toBeFalse();
    expect(component.formTouched).toBeTrue();

    fillValidForm(component);
    component.openCreateReview();
    expect(component.showCreateReview).toBeTrue();
    expect(component.reviewRows.length).toBeGreaterThan(0);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('does not submit until the operator confirms', fakeAsync(() => {
    const { fixture, component, api } = build();
    fixture.detectChanges();
    tick();
    fillValidForm(component);
    component.openCreateReview();

    // Confirming without the acknowledgement does nothing.
    component.confirmCreate();
    expect(api.createCalls).toBe(0);

    component.createAcknowledged = true;
    component.confirmCreate();
    tick();
    expect(api.createCalls).toBe(1);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('a double click produces exactly ONE request', fakeAsync(() => {
    const { fixture, component, api } = build();
    const gate = new Subject<TimedSessionAccepted>();
    api.createResponse = () => gate.asObservable();
    fixture.detectChanges();
    tick();
    fillValidForm(component);
    component.openCreateReview();
    component.createAcknowledged = true;

    component.confirmCreate();
    component.confirmCreate();
    component.confirmCreate();
    expect(api.createCalls).toBe(1);
    expect(component.submitting).toBeTrue();

    gate.next({ accepted: true, requestSequence: 5 });
    gate.complete();
    tick();
    expect(component.submitting).toBeFalse();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('202 is shown as accepted — never as started', fakeAsync(() => {
    const { fixture, component } = build();
    fixture.detectChanges();
    tick();
    fillValidForm(component);
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    tick();

    expect(component.command.phase).toBe('accepted');
    expect(component.command.title).toBe('Request accepted for processing');
    expect(component.command.detail).toContain('NOT started');
    const text = `${component.command.title} ${component.command.detail}`.toLowerCase();
    expect(text).not.toContain('session started');
    expect(text).not.toContain('mining');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('resolves to succeeded only from the device status, never from the 202', fakeAsync(() => {
    const { fixture, component, api } = build();
    fixture.detectChanges();
    tick();
    fillValidForm(component);
    component.openCreateReview();
    component.createAcknowledged = true;

    api.statusResponse = () => of(mockStatus('queued', 4));
    component.confirmCreate();
    // The 202 has been received and NOTHING has been read back yet.
    expect(component.command.phase).toBe('accepted');

    // The device now reports that it holds the command…
    tick();
    expect(component.command.phase).toBe('processing');

    // …and only then, once the device echoes this request's number AND its
    // kind, reports a newer sequence, AND actually shows a durable session.
    const mine = component.command.clientRequestId;
    expect(mine).toBeGreaterThan(0);
    api.statusResponse = () => of({
      ...mockStatus('applying', 50), commandPending: false,
      lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED', lastClientRequestId: mine,
    });
    (component as unknown as { readStatus(): void }).readStatus();
    tick();
    expect(component.command.phase).toBe('posture-reached');
    expect(component.command.title).toBe('The device now reports the requested posture');
    fixture.destroy();
    discardPeriodicTasks();
  }));


  it('clears the sensitive form after an accepted submission', fakeAsync(() => {
    const { fixture, component } = build();
    fixture.detectChanges();
    tick();
    fillValidForm(component);
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    tick();

    expect(component.form.targetHost).toBe('');
    expect(component.form.targetAccount).toBe('');
    expect(component.form.targetPort).toBeNull();
    expect(component.reviewRows.length).toBe(0);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('keeps the form in memory after a retryable failure, and never resubmits', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.createResponse = () => throwError(() => ({ kind: 'unavailable' } as TimedSessionFailure));
    fixture.detectChanges();
    tick();
    fillValidForm(component);
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    tick();

    expect(component.command.phase).toBe('temporarily-unavailable');
    expect(component.form.targetHost).toBe(MARKER_HOST);
    expect(api.createCalls).toBe(1);

    // No automatic retry, ever.
    tick(120000);
    expect(api.createCalls).toBe(1);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('maps a 400 validation answer without resubmitting', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.createResponse = () => throwError(() => (
      { kind: 'validation', validationCode: 'DURATION_OUT_OF_RANGE' } as TimedSessionFailure));
    fixture.detectChanges();
    tick();
    fillValidForm(component);
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    tick();

    expect(component.command.phase).toBe('validation-failed');
    expect(component.command.validationLabel).toContain('15 minutes and 24 hours');
    expect(api.createCalls).toBe(1);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('maps a 409 conflict without resubmitting', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.createResponse = () => throwError(() => ({
      kind: 'conflict',
      conflict: {
        code: 'OPERATION_BUSY_OTA', activeOwner: 'OWNER_OTA_UPDATE',
        retryable: true, restoreRequired: false, terminalAckRequired: false,
      },
    } as TimedSessionFailure));
    fixture.detectChanges();
    tick();
    fillValidForm(component);
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    tick();

    expect(component.command.phase).toBe('conflict');
    expect(component.command.conflict?.title).toContain('firmware update');
    expect(component.command.conflict?.retryable).toBeTrue();
    expect(api.createCalls).toBe(1);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('a lost response is surfaced, never resent', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.createResponse = () => throwError(() => ({ kind: 'offline' } as TimedSessionFailure));
    fixture.detectChanges();
    tick();
    fillValidForm(component);
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    tick(120000);

    expect(component.command.phase).toBe('failed');
    expect(api.createCalls).toBe(1);
    fixture.destroy();
    discardPeriodicTasks();
  }));
});

// ===========================================================================
// Restore Now and acknowledgement
// ===========================================================================

describe('TimedSessionComponent: restore and acknowledge', () => {
  it('Restore Now requires confirmation and submits exactly once', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('mining', 3));
    fixture.detectChanges();
    tick();
    expect(component.view?.restoreOffered).toBeTrue();

    component.openRestoreConfirm();
    component.confirmRestore();          // not acknowledged
    expect(api.restoreCalls).toBe(0);

    component.restoreAcknowledged = true;
    const gate = new Subject<TimedSessionAccepted>();
    api.restoreResponse = () => gate.asObservable();
    component.confirmRestore();
    component.confirmRestore();          // duplicate click
    expect(api.restoreCalls).toBe(1);

    gate.next({ accepted: true, requestSequence: 9 });
    gate.complete();
    tick();
    expect(component.command.phase).toBe('accepted');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('Restore Now is not offered without an active session', fakeAsync(() => {
    const { fixture, component } = build();
    fixture.detectChanges();
    tick();
    expect(component.view?.restoreOffered).toBeFalse();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('acknowledgement is offered only for a safe retained terminal', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('complete-pending-ack', 6));
    fixture.detectChanges();
    tick();
    expect(component.view?.terminalAckRequired).toBeTrue();
    expect(component.view?.acknowledgeOffered).toBeTrue();

    component.openAckConfirm();
    component.confirmAcknowledge();      // not acknowledged
    expect(api.ackCalls).toBe(0);
    component.ackAcknowledged = true;
    component.confirmAcknowledge();
    tick();
    expect(api.ackCalls).toBe(1);
    expect(component.command.phase).toBe('accepted');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('acknowledgement is refused for an obligated or guarded terminal', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('restore-failed', 7));
    fixture.detectChanges();
    tick();
    expect(component.view?.acknowledgeOffered).toBeFalse();

    api.statusResponse = () => of(mockStatus('recovery-guard', 8));
    (component as unknown as { readStatus(): void }).readStatus();
    tick();
    expect(component.view?.acknowledgeOffered).toBeFalse();
    expect(component.view?.operatorRecoveryRequired).toBeTrue();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('never optimistically clears the terminal card', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('complete-pending-ack', 6));
    fixture.detectChanges();
    tick();
    component.openAckConfirm();
    component.ackAcknowledged = true;
    component.confirmAcknowledge();
    tick();

    // The device still reports the retained result, so the card stays.
    expect(component.view?.terminalAckRequired).toBeTrue();

    // Only when the device says it is cleared does the card go.
    api.statusResponse = () => of(mockStatus('free', 9));
    (component as unknown as { readStatus(): void }).readStatus();
    tick();
    expect(component.view?.terminalAckRequired).toBeFalse();
    fixture.destroy();
    discardPeriodicTasks();
  }));
});

// ===========================================================================
// Status mapping through the component
// ===========================================================================

describe('TimedSessionComponent: status presentation', () => {
  it('never shows target mining active while only verifying', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('verifying', 2));
    fixture.detectChanges();
    tick();
    expect(component.view?.grant.label).toBe('Not authorised');
    expect(component.timeline?.stages.find((s) => s.id === 'mining')?.state).toBe('pending');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('shows target mining active only with an explicit backend grant', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('mining', 2));
    fixture.detectChanges();
    tick();
    expect(component.view?.grant.label).toBe('Active');
    expect(component.view?.deadline.remainingKnown).toBeTrue();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('never infers remaining time from elapsed browser time', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('time-untrusted', 2));
    fixture.detectChanges();
    tick();
    const before = component.view?.deadline.remainingSeconds;
    expect(component.view?.trustedTime.warn).toBeTrue();
    expect(component.view?.deadline.remainingKnown).toBeFalse();

    // Time passes; the view must not move on its own.
    tick(60000);
    expect(component.view?.deadline.remainingSeconds).toBe(before as number);
    expect(component.view?.deadline.remainingKnown).toBeFalse();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('announces a state change once for screen readers', fakeAsync(() => {
    const { fixture, component, api } = build();
    fixture.detectChanges();
    tick();
    const first = component.liveAnnouncement;
    expect(first.length).toBeGreaterThan(0);

    api.statusResponse = () => of(mockStatus('mining', 3));
    (component as unknown as { readStatus(): void }).readStatus();
    tick();
    expect(component.liveAnnouncement).not.toBe(first);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('gives every disabled submit a textual reason', fakeAsync(() => {
    const { fixture, component } = build();
    fixture.detectChanges();
    tick();
    expect(component.createDisabledReason).toBe('Complete the target details above.');
    fillValidForm(component);
    expect(component.createDisabledReason).toBe('');
    fixture.destroy();
    discardPeriodicTasks();
  }));
});

// ===========================================================================
// Build and runtime capability
// ===========================================================================

describe('TimedSessionComponent: capability', () => {
  /**
   * Posture B and C firmware answer the status route and report that they
   * cannot execute. Offering a create there would promise something the
   * device is structurally unable to do.
   */
  const CASES: { posture: MockPosture; phrase: string }[] = [
    { posture: 'api-disabled', phrase: 'control API turned off' },
    { posture: 'execution-disabled', phrase: 'never carry one out' },
    { posture: 'runtime-starting', phrase: 'has not finished starting' },
  ];

  CASES.forEach(({ posture, phrase }) => {
    it(`withholds the create form and says why: ${posture}`, fakeAsync(() => {
      const { fixture, component, api } = build();
      api.statusResponse = () => of(mockStatus(posture, 1));
      fixture.detectChanges();
      tick();
      fixture.detectChanges();

      expect(component.view?.capable).withContext('not capable').toBeFalse();
      expect(component.view?.createOffered).withContext('create withheld').toBeFalse();
      expect(component.createDisabledReason).toContain(phrase);

      // The page still EXPLAINS itself rather than silently dropping the form.
      const root = fixture.nativeElement as HTMLElement;
      expect(root.querySelector('.nx-ts-unavailable')).not.toBeNull();
      expect(root.querySelectorAll('#ts-host').length).withContext('no form').toBe(0);
      fixture.destroy();
      discardPeriodicTasks();
    }));
  });

  it('a device that reports every capability is offered the form', fakeAsync(() => {
    const { fixture, component } = build();
    fixture.detectChanges();
    tick();
    fixture.detectChanges();
    expect(component.view?.capable).toBeTrue();
    expect(component.view?.createOffered).toBeTrue();
    expect(component.createDisabledReason).toBe('Complete the target details above.');
    expect((fixture.nativeElement as HTMLElement).querySelector('#ts-host')).not.toBeNull();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('an absent capability flag counts as NOT capable, never as probably fine', fakeAsync(() => {
    const { fixture, component, api } = build();
    const stripped = { ...mockStatus('free', 1) };
    delete (stripped as { executionEnabled?: boolean }).executionEnabled;
    api.statusResponse = () => of(stripped as TimedSessionStatus);
    fixture.detectChanges();
    tick();
    expect(component.view?.capable).toBeFalse();
    expect(component.view?.createOffered).toBeFalse();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('names the specific blocker rather than one vague sentence', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('complete-pending-ack', 2));
    fixture.detectChanges();
    tick();
    expect(component.createDisabledReason).toContain('Acknowledge it first');

    api.statusResponse = () => of(mockStatus('mining', 3));
    (component as unknown as { readStatus(): void }).readStatus();
    tick();
    expect(component.createDisabledReason).toContain('Only one runs at a time');

    api.statusResponse = () => of(mockStatus('recovery-guard', 4));
    (component as unknown as { readStatus(): void }).readStatus();
    tick();
    expect(component.createDisabledReason).toContain('operator recovery');
    fixture.destroy();
    discardPeriodicTasks();
  }));
});

// ===========================================================================
// Command correlation, end to end through the component
// ===========================================================================

describe('TimedSessionComponent: command correlation', () => {
  /** Drive a create through to ACCEPTED and return the number it used. */
  function submitCreate(component: TimedSessionComponent): number {
    fillValidForm(component);
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    return component.command.clientRequestId;
  }

  it('a stale retained result with the same id but an OLD sequence stays queued', fakeAsync(() => {
    // The reload case: a previous page instance used this number, the device
    // still reports it, and the sequence has not moved past the baseline.
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('free', 40));
    fixture.detectChanges();
    tick();

    const mine = submitCreate(component);
    api.statusResponse = () => of({
      ...mockStatus('mining', 40), commandPending: false,
      lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED', lastClientRequestId: mine,
    });
    tick();
    tick(ACTIVE_CADENCE_MS);

    expect(component.command.phase).withContext('stale result rejected').toBe('accepted');
    expect(component.command.detail).toContain('NOT started');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('a matching id with the WRONG command kind stays queued', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('free', 40));
    fixture.detectChanges();
    tick();

    const mine = submitCreate(component);
    api.statusResponse = () => of({
      ...mockStatus('mining', 60), commandPending: false,
      lastCommand: 'RESTORE_NOW', lastCommandResult: 'ACCEPTED', lastClientRequestId: mine,
    });
    tick();
    tick(ACTIVE_CADENCE_MS);
    expect(component.command.phase).toBe('accepted');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('a matching id and kind WITHOUT the durable state change stays queued', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('free', 40));
    fixture.detectChanges();
    tick();

    const mine = submitCreate(component);
    api.statusResponse = () => of({
      ...mockStatus('free', 60), commandPending: false,
      lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED', lastClientRequestId: mine,
    });
    tick();
    tick(ACTIVE_CADENCE_MS);
    expect(component.command.phase)
      .withContext('lastCommandResult alone is never enough').toBe('accepted');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('RESTORE resolves only after restoration actually progresses', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('mining', 40));
    fixture.detectChanges();
    tick();

    component.restoreAcknowledged = true;
    component.confirmRestore();
    const mine = component.command.clientRequestId;
    tick();

    // Same durable posture as the baseline — nothing has moved.
    api.statusResponse = () => of({
      ...mockStatus('mining', 60), commandPending: false,
      lastCommand: 'RESTORE_NOW', lastCommandResult: 'ACCEPTED', lastClientRequestId: mine,
    });
    (component as unknown as { readStatus(): void }).readStatus();
    tick();
    expect(component.command.phase).toBe('accepted');

    // Now restoration is genuinely under way.
    api.statusResponse = () => of({
      ...mockStatus('restoring', 61), commandPending: false,
      lastCommand: 'RESTORE_NOW', lastCommandResult: 'ACCEPTED', lastClientRequestId: mine,
    });
    (component as unknown as { readStatus(): void }).readStatus();
    tick();
    expect(component.command.phase).toBe('posture-reached');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('ACKNOWLEDGE resolves only after the retained result truly clears', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('complete-pending-ack', 40));
    fixture.detectChanges();
    tick();

    component.ackAcknowledged = true;
    component.confirmAcknowledge();
    const mine = component.command.clientRequestId;
    tick();

    // Still retained — the card must never be cleared optimistically.
    api.statusResponse = () => of({
      ...mockStatus('complete-pending-ack', 60), commandPending: false,
      lastCommand: 'ACKNOWLEDGE_TERMINAL', lastCommandResult: 'ACCEPTED', lastClientRequestId: mine,
    });
    (component as unknown as { readStatus(): void }).readStatus();
    tick();
    expect(component.command.phase).toBe('accepted');
    expect(component.view?.terminalAckRequired).toBeTrue();

    // Cleared for real.
    api.statusResponse = () => of({
      ...mockStatus('free', 61), commandPending: false,
      lastCommand: 'ACKNOWLEDGE_TERMINAL', lastCommandResult: 'ACCEPTED', lastClientRequestId: mine,
    });
    (component as unknown as { readStatus(): void }).readStatus();
    tick();
    expect(component.command.phase).toBe('posture-reached');
    expect(component.view?.terminalAckRequired).toBeFalse();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('never claims authenticated ownership, even when everything correlates', fakeAsync(() => {
    // On an unauthenticated LAN another client could have used the same
    // number, so the wording must stay a statement about the device.
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('free', 40));
    fixture.detectChanges();
    tick();

    const mine = submitCreate(component);
    api.statusResponse = () => of({
      ...mockStatus('applying', 60), commandPending: false,
      lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED', lastClientRequestId: mine,
    });
    tick();
    tick(ACTIVE_CADENCE_MS);

    expect(component.command.phase).toBe('posture-reached');
    const text = (component.command.title + ' ' + component.command.detail).toLowerCase();
    ['your request completed', 'your request was', 'you started', 'confirmed your',
      'we started', 'proof', 'guarantee'].forEach((claim) => {
      expect(text).withContext(claim).not.toContain(claim);
    });
    expect(text).toContain('the device');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('a device refusal is reported as a refusal, not as a lost request', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('free', 40));
    fixture.detectChanges();
    tick();

    const mine = submitCreate(component);
    api.statusResponse = () => of({
      ...mockStatus('free', 60), commandPending: false,
      lastCommand: 'CREATE_SESSION', lastCommandResult: 'REJECTED', lastClientRequestId: mine,
    });
    tick();
    tick(ACTIVE_CADENCE_MS);

    expect(component.command.phase).toBe('device-refused');
    expect(component.command.title).toBe('The device did not accept the request');
    expect(api.createCalls).withContext('never resubmitted').toBe(1);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('HTTP 202 alone is still only ACCEPTED, never completed', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('free', 40));
    fixture.detectChanges();
    tick();
    submitCreate(component);
    expect(component.command.phase).toBe('accepted');
    expect(component.command.title).toBe('Request accepted for processing');
    fixture.destroy();
    discardPeriodicTasks();
  }));
});

// ===========================================================================
// Request-number allocation and exhaustion
// ===========================================================================

describe('TimedSessionComponent: request numbers', () => {
  it('allocates monotonically and never repeats within one lifetime', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('mining', 3));
    fixture.detectChanges();
    tick();

    component.restoreAcknowledged = true;
    component.confirmRestore();
    tick();
    const first = component.command.clientRequestId;

    component.ackAcknowledged = true;
    component.confirmAcknowledge();
    tick();
    const second = component.command.clientRequestId;

    expect(first).toBeGreaterThan(0);
    expect(second).toBe(first + 1);
    const bodies = JSON.stringify(api.actionBodies);
    expect(bodies).toContain('client_request_id');
    expect(bodies).not.toContain(MARKER_HOST);
    expect(bodies).not.toContain(MARKER_ACCOUNT);
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('uses the final number once, then fails closed WITHOUT sending anything', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('mining', 3));
    fixture.detectChanges();
    tick();

    // A two-number allocator so exhaustion is reachable in finite time.
    (component as unknown as { requestIds: RequestIdAllocator }).requestIds =
      createRequestIdAllocator(2);

    component.restoreAcknowledged = true;
    component.confirmRestore();
    tick();
    expect(api.restoreCalls).toBe(1);
    expect(component.command.clientRequestId).toBe(1);

    component.restoreAcknowledged = true;
    component.confirmRestore();
    tick();
    expect(api.restoreCalls).withContext('the final number is usable once').toBe(2);
    expect(component.command.clientRequestId).toBe(2);

    // Exhausted: refuse, and send NOTHING.
    component.restoreAcknowledged = true;
    component.confirmRestore();
    tick();
    expect(api.restoreCalls).withContext('no HTTP request was sent').toBe(2);
    expect(component.command.phase).toBe('request-ids-exhausted');
    expect(component.command.detail).toContain('Reload the page');
    expect(component.submitting).toBeFalse();
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('never silently returns to 1 after exhaustion', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('mining', 3));
    fixture.detectChanges();
    tick();
    (component as unknown as { requestIds: RequestIdAllocator }).requestIds =
      createRequestIdAllocator(1);

    component.restoreAcknowledged = true;
    component.confirmRestore();
    tick();
    const only = component.command.clientRequestId;

    component.restoreAcknowledged = true;
    component.confirmRestore();
    tick();
    component.restoreAcknowledged = true;
    component.confirmRestore();
    tick();

    expect(only).toBe(1);
    expect(api.restoreCalls).withContext('exactly one POST ever went out').toBe(1);
    expect(JSON.stringify(api.actionBodies)).toBe(JSON.stringify([{ client_request_id: 1 }]));
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('a lost POST response is still never retried', fakeAsync(() => {
    const { fixture, component, api } = build();
    api.statusResponse = () => of(mockStatus('mining', 3));
    fixture.detectChanges();
    tick();

    api.restoreResponse = () => throwError(() => ({ kind: 'offline' } as TimedSessionFailure));
    component.restoreAcknowledged = true;
    component.confirmRestore();
    tick(120000);

    expect(api.restoreCalls).toBe(1);
    expect(component.command.phase).toBe('failed');
    fixture.destroy();
    discardPeriodicTasks();
  }));
});
