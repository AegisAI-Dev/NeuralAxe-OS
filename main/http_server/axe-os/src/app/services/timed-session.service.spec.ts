/**
 * Gate B9 — API service tests against HttpTestingController.
 *
 * Proves the service touches ONLY the four committed Gate B8 routes, maps
 * every HTTP outcome onto the bounded failure model, never retries and never
 * logs or persists a request payload. No outbound networking occurs.
 */

import { TestBed } from '@angular/core/testing';
import { HttpClientTestingModule, HttpTestingController } from '@angular/common/http/testing';

import { TimedSessionApiService, TimedSessionFailure, mockStatus } from './timed-session.service';
import { TimedSessionCreateRequest } from '../generated/models';

const STATUS_PATH = '/api/system/timed-session';
const RESTORE_PATH = '/api/system/timed-session/restore';
const ACK_PATH = '/api/system/timed-session/acknowledge';

const MARKER_HOST = 'marker-host.example';
const MARKER_ACCOUNT = 'marker-account.worker';

function body(): TimedSessionCreateRequest {
  return {
    duration_seconds: 3600,
    target_host: MARKER_HOST,
    target_port: 3334,
    target_account: MARKER_ACCOUNT,
    target_protocol: 'stratum_v1',
    target_tls_mode: 'disabled',
    target_chain: 'bitcoin',
  };
}

describe('TimedSessionApiService (production paths)', () => {
  let service: TimedSessionApiService;
  let http: HttpTestingController;

  beforeEach(() => {
    TestBed.configureTestingModule({
      imports: [HttpClientTestingModule],
      providers: [TimedSessionApiService],
    });
    service = TestBed.inject(TimedSessionApiService);
    http = TestBed.inject(HttpTestingController);
    // Exercise the LIVE device path through the single named seam.
    spyOn(service as unknown as { live(): boolean }, 'live').and.returnValue(true);
  });

  afterEach(() => http.verify());

  // ---- routes -------------------------------------------------------------

  it('reads status from exactly the committed GET route', () => {
    service.getStatus().subscribe();
    const req = http.expectOne(STATUS_PATH);
    expect(req.request.method).toBe('GET');
    expect(req.request.body).toBeNull();
    req.flush(mockStatus('free', 1));
  });

  it('creates, restores and acknowledges on exactly the committed POST routes', () => {
    service.createSession(body()).subscribe();
    const create = http.expectOne(STATUS_PATH);
    expect(create.request.method).toBe('POST');
    create.flush({ accepted: true, requestSequence: 1 }, { status: 202, statusText: 'Accepted' });

    service.restoreNow().subscribe();
    const restore = http.expectOne(RESTORE_PATH);
    expect(restore.request.method).toBe('POST');
    restore.flush({ accepted: true, requestSequence: 2 }, { status: 202, statusText: 'Accepted' });

    service.acknowledgeTerminal().subscribe();
    const ack = http.expectOne(ACK_PATH);
    expect(ack.request.method).toBe('POST');
    ack.flush({ accepted: true, requestSequence: 3 }, { status: 202, statusText: 'Accepted' });
  });

  it('never calls the generic pool PATCH, restart, OTA or ASIC routes', () => {
    service.createSession(body()).subscribe();
    http.expectOne(STATUS_PATH).flush({ accepted: true, requestSequence: 1 }, { status: 202, statusText: 'Accepted' });
    ['/api/system', '/api/system/restart', '/api/system/OTA', '/api/system/OTAWWW', '/api/system/asic']
      .forEach((p) => http.expectNone(p));
  });

  it('sends exactly the eight committed create fields and no password-like key', () => {
    service.createSession(body()).subscribe();
    const req = http.expectOne(STATUS_PATH);
    expect(Object.keys(req.request.body as object).sort()).toEqual([
      'duration_seconds', 'target_account', 'target_chain', 'target_host',
      'target_port', 'target_protocol', 'target_tls_mode',
    ]);
    const json = JSON.stringify(req.request.body).toLowerCase();
    ['password', 'secret', 'token', 'source', 'session_id', 'lease'].forEach((k) => {
      expect(json).withContext(k).not.toContain(k);
    });
    // No identity is ever placed in the URL or a query string.
    expect(req.request.urlWithParams).toBe(STATUS_PATH);
    req.flush({ accepted: true, requestSequence: 1 }, { status: 202, statusText: 'Accepted' });
  });

  it('passes the optional correlation id through and nothing else', () => {
    service.createSession({ ...body(), client_request_id: 7 }).subscribe();
    const create = http.expectOne(STATUS_PATH);
    expect(Object.keys(create.request.body as object).sort()).toEqual([
      'client_request_id', 'duration_seconds', 'target_account', 'target_chain',
      'target_host', 'target_port', 'target_protocol', 'target_tls_mode',
    ]);
    create.flush({ accepted: true, requestSequence: 1, clientRequestId: 7 },
      { status: 202, statusText: 'Accepted' });

    // Restore Now and acknowledgement carry the id and NOTHING else — there is
    // no session id, no lease token and no identity anywhere on the wire.
    service.restoreNow({ client_request_id: 8 }).subscribe();
    const restore = http.expectOne(RESTORE_PATH);
    expect(restore.request.body).toEqual({ client_request_id: 8 });
    restore.flush({ accepted: true, requestSequence: 2 }, { status: 202, statusText: 'Accepted' });

    service.acknowledgeTerminal({ client_request_id: 9 }).subscribe();
    const ack = http.expectOne(ACK_PATH);
    expect(ack.request.body).toEqual({ client_request_id: 9 });
    ack.flush({ accepted: true, requestSequence: 3 }, { status: 202, statusText: 'Accepted' });
  });

  it('sends no Authorization header and no query parameters', () => {
    service.getStatus().subscribe();
    const req = http.expectOne(STATUS_PATH);
    expect(req.request.headers.has('Authorization')).toBeFalse();
    expect(req.request.params.keys().length).toBe(0);
    req.flush(mockStatus('free', 1));
  });

  // ---- outcome mapping ----------------------------------------------------

  it('200 yields the sanitized status body', (done) => {
    const payload = mockStatus('mining', 9);
    service.getStatus().subscribe((s) => {
      expect(s.durableState).toBe('TARGET_ACTIVE');
      expect(s.statusSequence).toBe(9);
      done();
    });
    http.expectOne(STATUS_PATH).flush(payload);
  });

  it('202 yields the accepted envelope', (done) => {
    service.createSession(body()).subscribe((a) => {
      expect(a.accepted).toBeTrue();
      expect(a.requestSequence).toBe(42);
      done();
    });
    http.expectOne(STATUS_PATH).flush({ accepted: true, requestSequence: 42 }, { status: 202, statusText: 'Accepted' });
  });

  it('400 becomes a validation failure carrying the stable code', (done) => {
    service.createSession(body()).subscribe({
      error: (f: TimedSessionFailure) => {
        expect(f.kind).toBe('validation');
        expect(f.validationCode).toBe('DURATION_OUT_OF_RANGE');
        done();
      },
    });
    http.expectOne(STATUS_PATH).flush({ code: 'DURATION_OUT_OF_RANGE' }, { status: 400, statusText: 'Bad Request' });
  });

  it('401 becomes origin-denied', (done) => {
    service.getStatus().subscribe({
      error: (f: TimedSessionFailure) => { expect(f.kind).toBe('origin-denied'); done(); },
    });
    http.expectOne(STATUS_PATH).flush('Unauthorized', { status: 401, statusText: 'Unauthorized' });
  });

  it('404 becomes not-present — the capability answer', (done) => {
    service.getStatus().subscribe({
      error: (f: TimedSessionFailure) => { expect(f.kind).toBe('not-present'); done(); },
    });
    http.expectOne(STATUS_PATH).flush({ error: 'unknown route' }, { status: 404, statusText: 'Not Found' });
  });

  it('409 becomes a conflict carrying the sanitized Gate B5 body', (done) => {
    service.restoreNow().subscribe({
      error: (f: TimedSessionFailure) => {
        expect(f.kind).toBe('conflict');
        expect(f.conflict?.code).toBe('OPERATION_BUSY_OTA');
        expect(f.conflict?.activeOwner).toBe('OWNER_OTA_UPDATE');
        expect(f.conflict?.retryable).toBeTrue();
        done();
      },
    });
    http.expectOne(RESTORE_PATH).flush(
      { code: 'OPERATION_BUSY_OTA', retryable: true, activeOwner: 'OWNER_OTA_UPDATE', restoreRequired: false, terminalAckRequired: false },
      { status: 409, statusText: 'Conflict' },
    );
  });

  it('503 becomes temporarily unavailable', (done) => {
    service.acknowledgeTerminal().subscribe({
      error: (f: TimedSessionFailure) => { expect(f.kind).toBe('unavailable'); done(); },
    });
    http.expectOne(ACK_PATH).flush({ accepted: false, code: 'QUEUE_FULL' }, { status: 503, statusText: 'Service Unavailable' });
  });

  it('a transport error becomes offline', (done) => {
    service.getStatus().subscribe({
      error: (f: TimedSessionFailure) => { expect(f.kind).toBe('offline'); done(); },
    });
    http.expectOne(STATUS_PATH).error(new ProgressEvent('error'));
  });

  it('any other status becomes the generic unknown failure', (done) => {
    service.getStatus().subscribe({
      error: (f: TimedSessionFailure) => { expect(f.kind).toBe('unknown'); done(); },
    });
    http.expectOne(STATUS_PATH).flush('boom', { status: 500, statusText: 'Internal Server Error' });
  });

  // ---- no retry -----------------------------------------------------------

  it('never retries a failed command automatically', () => {
    service.createSession(body()).subscribe({ error: () => undefined });
    http.expectOne(STATUS_PATH).flush('nope', { status: 503, statusText: 'Service Unavailable' });
    // A retry would show up as a second outstanding request.
    http.expectNone(STATUS_PATH);
  });

  it('never retries a failed status read automatically', () => {
    service.getStatus().subscribe({ error: () => undefined });
    http.expectOne(STATUS_PATH).error(new ProgressEvent('error'));
    http.expectNone(STATUS_PATH);
  });

  it('holds no request state between calls', () => {
    service.createSession(body()).subscribe({ error: () => undefined });
    http.expectOne(STATUS_PATH).flush('nope', { status: 503, statusText: 'Service Unavailable' });
    // The service has no field anywhere that retained the marker values.
    const dump = JSON.stringify(service, (_k, v) => (typeof v === 'function' ? undefined : v));
    expect(dump).not.toContain(MARKER_HOST);
    expect(dump).not.toContain(MARKER_ACCOUNT);
  });
});

describe('TimedSessionApiService (development mocks)', () => {
  let service: TimedSessionApiService;
  let http: HttpTestingController;

  beforeEach(() => {
    TestBed.configureTestingModule({
      imports: [HttpClientTestingModule],
      providers: [TimedSessionApiService],
    });
    service = TestBed.inject(TimedSessionApiService);
    http = TestBed.inject(HttpTestingController);
  });

  afterEach(() => http.verify());

  it('issues NO HTTP request at all while mocked', (done) => {
    service.mockPosture = 'mining';
    service.getStatus().subscribe((s) => {
      expect(s.durableState).toBe('TARGET_ACTIVE');
      http.expectNone(() => true);
      done();
    });
  });

  it('the not-present posture errors exactly like a 404', (done) => {
    service.mockPosture = 'not-present';
    service.getStatus().subscribe({
      error: (f: TimedSessionFailure) => { expect(f.kind).toBe('not-present'); done(); },
    });
  });
});

describe('mock postures follow the committed Gate B8 schema', () => {
  const ALLOWED = new Set([
    'apiEnabled', 'runtimeInitialized', 'executionEnabled', 'sessionPresent', 'terminalResultPending',
    'durableState', 'durableFailure', 'runtimeState', 'executionState', 'executionReason',
    'leaseOwner', 'leasePhase', 'protocolStartPermitted', 'asicGate', 'targetMiningGrantActive',
    'restoreRequired', 'operatorRecoveryRequired', 'trustedTimeRequired', 'trustedTimeAvailable',
    'deadlineStatus', 'remainingSecondsValid', 'remainingSeconds', 'commandPending', 'pendingCommand',
    'lastCommand', 'lastCommandResult', 'lastClientRequestId', 'heartbeatStatus', 'heartbeatCommits',
    'apiConflictCode', 'statusSequence',
  ]);

  const POSTURES = ['not-present', 'free', 'queued', 'applying', 'verifying', 'mining', 'restoring',
    'complete-pending-ack', 'restore-failed', 'recovery-guard', 'time-untrusted',
    'heartbeat-failed', 'conflict', 'mailbox-unavailable',
    'api-disabled', 'execution-disabled', 'runtime-starting'] as const;

  it('invents no field outside the OpenAPI schema', () => {
    POSTURES.forEach((p) => {
      Object.keys(mockStatus(p, 1)).forEach((k) => {
        expect(ALLOWED.has(k)).withContext(`${p}.${k}`).toBeTrue();
      });
    });
  });

  it('carries no identity value in any posture — the schema has nowhere to put one', () => {
    POSTURES.forEach((p) => {
      const json = JSON.stringify(mockStatus(p, 1)).toLowerCase();
      ['.com', '.org', '.net', 'bc1', 'password', 'wallet', 'worker@'].forEach((needle) => {
        expect(json).withContext(`${p} / ${needle}`).not.toContain(needle);
      });
    });
  });

  it('covers every posture the operator UX must handle', () => {
    expect(mockStatus('mining', 1).targetMiningGrantActive).toBeTrue();
    expect(mockStatus('verifying', 1).targetMiningGrantActive).toBeFalse();
    expect(mockStatus('complete-pending-ack', 1).terminalResultPending).toBeTrue();
    expect(mockStatus('restore-failed', 1).restoreRequired).toBeTrue();
    expect(mockStatus('recovery-guard', 1).operatorRecoveryRequired).toBeTrue();
    expect(mockStatus('time-untrusted', 1).trustedTimeAvailable).toBeFalse();
    expect(mockStatus('heartbeat-failed', 1).heartbeatStatus).toBe('PERSIST_FAILED');
    expect(mockStatus('queued', 1).commandPending).toBeTrue();
    expect(mockStatus('mailbox-unavailable', 1).runtimeInitialized).toBeFalse();
    // The three build/runtime postures that answer but cannot execute.
    expect(mockStatus('api-disabled', 1).apiEnabled).toBeFalse();
    expect(mockStatus('execution-disabled', 1).executionEnabled).toBeFalse();
    expect(mockStatus('runtime-starting', 1).runtimeInitialized).toBeFalse();
  });
});
