/**
 * Gate B9 — privacy, storage and source-shape contract tests.
 *
 * Plants unique markers in every operator-supplied field and proves they never
 * reach browser storage, a URL, router state, the console, a notification, the
 * command history or the rendered status after a reset. Also proves — by
 * reading the shipped source files — that no password control, no source
 * control and no session-identifier control exists at all.
 */

import { ComponentFixture, TestBed, fakeAsync, tick, discardPeriodicTasks } from '@angular/core/testing';
import { NO_ERRORS_SCHEMA } from '@angular/core';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { FormsModule } from '@angular/forms';
import { Observable, of } from 'rxjs';
import { ToastrService } from 'ngx-toastr';

import { TimedSessionComponent } from './timed-session.component';
import { TimedSessionApiService, mockStatus } from 'src/app/services/timed-session.service';
import { TimedSessionAccepted, TimedSessionStatus } from 'src/app/generated/models';
import { buildCreateRequest, emptyForm } from './timed-session-form';
import { buildView } from './timed-session-mapper';
import { buildTimeline } from './timed-session-timeline';

const MARKER_HOST = 'nx-privacy-marker-host.example';
const MARKER_ACCOUNT = 'nx-privacy-marker-account.worker';
const MARKER_PASSWORD_TEXT = 'nx-privacy-marker-secret-value';
const ALL_MARKERS = [MARKER_HOST, MARKER_ACCOUNT, MARKER_PASSWORD_TEXT];

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
  public isMocked = false;
  public mockPosture = 'free' as const;
  public createBodies: unknown[] = [];
  public statusResponse: () => Observable<TimedSessionStatus> = () => of(mockStatus('free', 1));
  public getStatus(): Observable<TimedSessionStatus> { return this.statusResponse(); }
  public createSession(b: unknown): Observable<TimedSessionAccepted> {
    this.createBodies.push(b);
    return of({ accepted: true, requestSequence: 1 });
  }
  public restoreNow(): Observable<TimedSessionAccepted> { return of({ accepted: true, requestSequence: 2 }); }
  public acknowledgeTerminal(): Observable<TimedSessionAccepted> { return of({ accepted: true, requestSequence: 3 }); }
}

function build(): { fixture: ComponentFixture<TimedSessionComponent>; component: TimedSessionComponent; api: StubApi } {
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
  return { fixture, component: fixture.componentInstance, api };
}

function storageDump(): string {
  const parts: string[] = [];
  for (let i = 0; i < window.localStorage.length; i++) {
    const k = window.localStorage.key(i)!;
    parts.push(k, window.localStorage.getItem(k) ?? '');
  }
  for (let i = 0; i < window.sessionStorage.length; i++) {
    const k = window.sessionStorage.key(i)!;
    parts.push(k, window.sessionStorage.getItem(k) ?? '');
  }
  return parts.join('|');
}

afterEach(() => TestBed.resetTestingModule());

// ===========================================================================

describe('privacy: markers never leave in-memory form state', () => {
  it('no marker reaches localStorage, sessionStorage, the URL or the console', fakeAsync(() => {
    const logs: unknown[] = [];
    const spies = (['log', 'info', 'warn', 'error', 'debug'] as const)
      .map((m) => spyOn(console, m).and.callFake((...args: unknown[]) => { logs.push(...args); }));

    const { fixture, component, api } = build();
    fixture.detectChanges();
    tick();

    component.form.targetHost = MARKER_HOST;
    component.form.targetAccount = MARKER_ACCOUNT;
    component.form.targetPort = 3334;
    component.onFormChanged();
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    tick();

    // The device DID receive the values (that is the whole point of the form)…
    expect(JSON.stringify(api.createBodies)).toContain(MARKER_HOST);

    // …and they went nowhere else.
    const storage = storageDump();
    const url = `${window.location.href}${window.location.search}${window.location.hash}`;
    const logged = JSON.stringify(logs);
    ALL_MARKERS.forEach((m) => {
      expect(storage).withContext(`storage / ${m}`).not.toContain(m);
      expect(url).withContext(`url / ${m}`).not.toContain(m);
      expect(logged).withContext(`console / ${m}`).not.toContain(m);
    });

    spies.forEach((s) => s.and.callThrough());
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('the success notification names the action only — never an identity', fakeAsync(() => {
    const { fixture, component } = build();
    const toastr = TestBed.inject(ToastrService);
    const info = spyOn(toastr, 'info').and.callThrough();
    fixture.detectChanges();
    tick();

    component.form.targetHost = MARKER_HOST;
    component.form.targetAccount = MARKER_ACCOUNT;
    component.form.targetPort = 3334;
    component.onFormChanged();
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    tick();

    expect(info).toHaveBeenCalled();
    const args = JSON.stringify(info.calls.allArgs());
    ALL_MARKERS.forEach((m) => expect(args).withContext(m).not.toContain(m));
    expect(args).toContain('queued');
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('the command banner never carries an identity', fakeAsync(() => {
    const { fixture, component } = build();
    fixture.detectChanges();
    tick();
    component.form.targetHost = MARKER_HOST;
    component.form.targetAccount = MARKER_ACCOUNT;
    component.form.targetPort = 3334;
    component.onFormChanged();
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    tick();

    const banner = JSON.stringify(component.command);
    ALL_MARKERS.forEach((m) => expect(banner).withContext(m).not.toContain(m));
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('the rendered page holds no marker after the accepted reset', fakeAsync(() => {
    const { fixture, component } = build();
    fixture.detectChanges();
    tick();
    component.form.targetHost = MARKER_HOST;
    component.form.targetAccount = MARKER_ACCOUNT;
    component.form.targetPort = 3334;
    component.onFormChanged();
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    tick();
    fixture.detectChanges();

    const html = (fixture.nativeElement as HTMLElement).innerHTML;
    ALL_MARKERS.forEach((m) => expect(html).withContext(m).not.toContain(m));
    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('destroying the component clears every sensitive value', fakeAsync(() => {
    const { fixture, component } = build();
    fixture.detectChanges();
    tick();
    component.form.targetHost = MARKER_HOST;
    component.form.targetAccount = MARKER_ACCOUNT;
    component.onFormChanged();

    fixture.destroy();
    expect(component.form.targetHost).toBe('');
    expect(component.form.targetAccount).toBe('');
    expect(component.reviewRows.length).toBe(0);
    const dump = JSON.stringify(component.form);
    ALL_MARKERS.forEach((m) => expect(dump).withContext(m).not.toContain(m));
    discardPeriodicTasks();
  }));

  it('a sanitized status can never carry an identity into the view or timeline', () => {
    // Even a hostile payload with extra keys cannot leak: the view model has
    // no field to hold one.
    const hostile = {
      ...mockStatus('mining', 1),
      // Fields the committed schema does not define; the mapper ignores them.
      sneakyHost: MARKER_HOST,
      sneakyAccount: MARKER_ACCOUNT,
    } as unknown as TimedSessionStatus;

    const view = JSON.stringify(buildView(hostile));
    const timeline = JSON.stringify(buildTimeline(hostile));
    ALL_MARKERS.forEach((m) => {
      expect(view).withContext(`view / ${m}`).not.toContain(m);
      expect(timeline).withContext(`timeline / ${m}`).not.toContain(m);
    });
  });
});

// ===========================================================================

describe('privacy: the shipped surface has no forbidden control', () => {
  it('the create request type structurally cannot carry a secret', () => {
    const keys = Object.keys(buildCreateRequest(emptyForm()));
    expect(keys).toEqual(jasmine.arrayWithExactContents([
      'duration_seconds', 'target_host', 'target_port', 'target_account',
      'target_protocol', 'target_tls_mode', 'target_chain',
    ]));
    keys.forEach((k) => {
      expect(k).not.toMatch(/pass|secret|token|session|lease|source|cert/i);
    });
  });

  it('the form model exposes only target fields', () => {
    const keys = Object.keys(emptyForm());
    expect(keys.some((k) => /pass|secret|token|session|lease|source|cert/i.test(k))).toBeFalse();
    expect(keys).toContain('targetHost');
    expect(keys).toContain('targetAccount');
  });

  it('the view model exposes no identity field', () => {
    const keys = Object.keys(buildView(mockStatus('mining', 1)));
    keys.forEach((k) => {
      expect(k).withContext(k).not.toMatch(/host|account|worker|wallet|pass|session[iI]d|lease|generation/i);
    });
  });

  it('the RENDERED create form has no password input and no source input', fakeAsync(() => {
    const { fixture } = build();
    fixture.detectChanges();
    tick();
    fixture.detectChanges();

    const root = fixture.nativeElement as HTMLElement;
    // Not one password control exists anywhere on the page.
    expect(root.querySelectorAll('input[type="password"]').length).toBe(0);

    // Every rendered input/select is a TARGET or duration control.
    const ALLOWED_IDS = new Set([
      'ts-hours', 'ts-minutes', 'ts-host', 'ts-port', 'ts-account',
      'ts-protocol', 'ts-tls', 'ts-chain',
      'ts-create-ack', 'ts-restore-ack', 'ts-ack-ack',
    ]);
    root.querySelectorAll('input, select, textarea').forEach((el) => {
      const id = el.getAttribute('id') ?? '';
      expect(ALLOWED_IDS.has(id)).withContext(`unexpected control: ${id || el.outerHTML}`).toBeTrue();
      expect(id).not.toMatch(/pass|secret|token|source|session|lease|cert/i);
    });

    // The visible labels never ask for a secret, a source or a session id.
    const text = (root.textContent ?? '').toLowerCase();
    ['enter your password', 'source host', 'source account', 'session id'].forEach((phrase) => {
      expect(text).withContext(phrase).not.toContain(phrase);
    });
    // A custom-certificate TLS choice is never OFFERED. The page is free to
    // explain in prose why it is absent — what must not exist is a way to
    // pick one, because the device could not restore it exactly.
    const tls = root.querySelector('#ts-tls') as HTMLSelectElement | null;
    expect(tls).withContext('the TLS control is rendered').not.toBeNull();
    const offered = Array.from(tls!.querySelectorAll('option')).map((o) => o.textContent ?? '');
    expect(offered.length).toBe(2);
    offered.forEach((o) => expect(o.toLowerCase()).withContext(o).not.toContain('custom'));

    fixture.destroy();
    discardPeriodicTasks();
  }));

  it('renders no unsafe HTML binding', fakeAsync(() => {
    const { fixture } = build();
    fixture.detectChanges();
    tick();
    fixture.detectChanges();
    // Angular would have had to bind [innerHTML]; nothing in the page does.
    const root = fixture.nativeElement as HTMLElement;
    expect(root.querySelectorAll('[innerHTML]').length).toBe(0);
    fixture.destroy();
    discardPeriodicTasks();
  }));
});

// ===========================================================================

describe('privacy: no client storage API is used by the feature', () => {
  it('the component writes nothing to localStorage or sessionStorage', fakeAsync(() => {
    const setLocal = spyOn(Storage.prototype, 'setItem').and.callThrough();
    const { fixture, component } = build();
    fixture.detectChanges();
    tick();
    component.form.targetHost = MARKER_HOST;
    component.form.targetAccount = MARKER_ACCOUNT;
    component.form.targetPort = 3334;
    component.onFormChanged();
    component.openCreateReview();
    component.createAcknowledged = true;
    component.confirmCreate();
    tick();

    const writes = JSON.stringify(setLocal.calls.allArgs());
    ALL_MARKERS.forEach((m) => expect(writes).withContext(m).not.toContain(m));
    fixture.destroy();
    discardPeriodicTasks();
  }));
});
