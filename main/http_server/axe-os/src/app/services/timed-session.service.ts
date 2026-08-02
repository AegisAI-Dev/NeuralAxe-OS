/**
 * NeuralAxe Timed Pool Session API service (Phase 2M.1B, Gate B9).
 *
 * The ONLY frontend path to the committed Gate B8 control routes:
 *
 *   GET  /api/system/timed-session
 *   POST /api/system/timed-session
 *   POST /api/system/timed-session/restore
 *   POST /api/system/timed-session/acknowledge
 *
 * It NEVER calls the generic pool PATCH, restart, OTA, ASIC or protocol
 * routes: a timed session is created only by the device, only through these
 * four routes, and the device remains authoritative for every decision.
 *
 * WHY `HttpClient` AND NOT THE GENERATED `Api.invoke`: the generated helper
 * returns a Promise of the body and discards the HTTP status. Gate B9 must
 * distinguish 200 from 202, and 400 / 401 / 404 / 409 / 503 from one another,
 * so it observes full responses. The generated MODELS remain authoritative and
 * are imported unchanged, and the paths come from the generated operations.
 *
 * PRIVACY: no request payload is ever logged, cached or persisted. The service
 * holds no form state at all — the create body is passed straight through and
 * dropped.
 *
 * RETRY: there is NO automatic command retry anywhere in this file. A lost
 * response is surfaced to the operator; it is never resent.
 */

import { HttpClient, HttpErrorResponse, HttpResponse } from '@angular/common/http';
import { Injectable } from '@angular/core';
import { Observable, delay, of, throwError } from 'rxjs';
import { catchError, map } from 'rxjs/operators';

import {
  OperationConflict,
  TimedSessionAccepted,
  TimedSessionActionRequest,
  TimedSessionCreateRequest,
  TimedSessionStatus,
} from '../generated/models';
import {
  acknowledgeTimedSession, createTimedSession, getTimedSession, restoreTimedSession,
} from '../generated/functions';
import { environment } from '../../environments/environment';

/** Bounded classification of every outcome the four routes can produce. */
export type TimedSessionOutcomeKind =
  | 'ok'            // 200 with a status body
  | 'accepted'      // 202 queued for the owner task
  | 'validation'    // 400 with a stable machine code
  | 'origin-denied' // 401
  | 'not-present'   // 404 — the API is absent from this firmware build
  | 'conflict'      // 409 with the sanitized Gate B5 body
  | 'unavailable'   // 503 — runtime or command mailbox not ready
  | 'offline'       // no response at all
  | 'unknown';      // anything else

export interface TimedSessionFailure {
  readonly kind: Exclude<TimedSessionOutcomeKind, 'ok' | 'accepted'>;
  /** Stable backend validation code (400 only). */
  readonly validationCode?: string;
  /** Sanitized Gate B5 conflict body (409 only). */
  readonly conflict?: OperationConflict;
}

/** Request timeout budget, matching the rest of the dashboard. */
const API_TIMEOUT = 15000;

/** Development mock postures, selectable from the component while not in production. */
export type MockPosture =
  | 'not-present' | 'free' | 'queued' | 'applying' | 'verifying' | 'mining'
  | 'restoring' | 'complete-pending-ack' | 'restore-failed' | 'recovery-guard'
  | 'time-untrusted' | 'heartbeat-failed' | 'conflict' | 'mailbox-unavailable'
  | 'api-disabled' | 'execution-disabled' | 'runtime-starting';

/** Base of every synthetic status. Values are structural only — no identities. */
function baseStatus(): TimedSessionStatus {
  return {
    apiEnabled: true,
    runtimeInitialized: true,
    executionEnabled: true,
    sessionPresent: false,
    terminalResultPending: false,
    durableState: 'IDLE',
    durableFailure: 'ERR_NONE',
    runtimeState: 'runtime_free',
    executionState: 'EXEC_STATE_IDLE',
    executionReason: 'EXEC_REASON_NONE',
    leaseOwner: 'OWNER_NONE',
    leasePhase: 'PHASE_FREE',
    protocolStartPermitted: true,
    asicGate: 'GATE_DEFAULT_OPEN',
    targetMiningGrantActive: false,
    restoreRequired: false,
    operatorRecoveryRequired: false,
    trustedTimeRequired: false,
    trustedTimeAvailable: false,
    deadlineStatus: 'UNKNOWN',
    remainingSecondsValid: false,
    remainingSeconds: 0,
    commandPending: false,
    pendingCommand: 'NONE',
    lastCommand: 'NONE',
    lastCommandResult: 'NONE',
    lastClientRequestId: 0,
    heartbeatStatus: 'NOT_APPLICABLE',
    heartbeatCommits: 0,
    apiConflictCode: 'OPERATION_ALLOWED',
    statusSequence: 1,
  };
}

/**
 * Synthetic development postures. Every field follows the committed Gate B8
 * schema exactly — no invented field exists — and no identity value appears,
 * because the sanitized status has nowhere to put one.
 */
export function mockStatus(posture: MockPosture, sequence: number): TimedSessionStatus {
  const s = { ...baseStatus(), statusSequence: sequence };
  switch (posture) {
    case 'free':
      return s;
    // The three build/runtime postures in which the device answers but cannot
    // actually run a session. Posture B and C firmware really do report these.
    case 'api-disabled':
      return { ...s, apiEnabled: false };
    case 'execution-disabled':
      return { ...s, executionEnabled: false, executionState: 'EXEC_STATE_DISABLED' };
    case 'runtime-starting':
      return { ...s, runtimeInitialized: false };
    case 'queued':
      return { ...s, commandPending: true, pendingCommand: 'CREATE_SESSION', lastCommand: 'CREATE_SESSION', lastCommandResult: 'PENDING' };
    case 'applying':
      return {
        ...s, sessionPresent: true, durableState: 'APPLYING_TARGET', restoreRequired: true,
        runtimeState: 'runtime_verify_target_pending', executionState: 'EXEC_STATE_TARGET_APPLYING',
        leaseOwner: 'OWNER_TIMED_SESSION', leasePhase: 'PHASE_VERIFYING_TARGET',
        protocolStartPermitted: false, asicGate: 'GATE_INHIBITED',
        lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED',
      };
    case 'verifying':
      return {
        ...s, sessionPresent: true, durableState: 'VERIFYING_TARGET', restoreRequired: true,
        runtimeState: 'runtime_verify_target_pending', executionState: 'EXEC_STATE_TARGET_CONNECTING',
        leaseOwner: 'OWNER_TIMED_SESSION', leasePhase: 'PHASE_VERIFYING_TARGET',
        protocolStartPermitted: false, asicGate: 'GATE_INHIBITED',
        lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED',
      };
    case 'mining':
      return {
        ...s, sessionPresent: true, durableState: 'TARGET_ACTIVE', restoreRequired: true,
        runtimeState: 'runtime_verify_target_pending', executionState: 'EXEC_STATE_TARGET_MINING',
        leaseOwner: 'OWNER_TIMED_SESSION', leasePhase: 'PHASE_VERIFYING_TARGET',
        protocolStartPermitted: false, asicGate: 'GATE_OPEN_TARGET', targetMiningGrantActive: true,
        trustedTimeRequired: true, trustedTimeAvailable: true,
        deadlineStatus: 'ACTIVE', remainingSecondsValid: true, remainingSeconds: 3542,
        heartbeatStatus: 'COMMITTED', heartbeatCommits: 12,
        lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED',
      };
    case 'restoring':
      return {
        ...s, sessionPresent: true, durableState: 'APPLYING_RESTORE', restoreRequired: true,
        runtimeState: 'runtime_restore_source_pending', executionState: 'EXEC_STATE_SOURCE_APPLYING',
        leaseOwner: 'OWNER_SOURCE_RESTORE', leasePhase: 'PHASE_RESTORING_SOURCE',
        protocolStartPermitted: false, asicGate: 'GATE_INHIBITED',
        deadlineStatus: 'RESTORE_PENDING',
        lastCommand: 'RESTORE_NOW', lastCommandResult: 'ACCEPTED',
      };
    case 'complete-pending-ack':
      return {
        ...s, sessionPresent: true, terminalResultPending: true, durableState: 'COMPLETE',
        runtimeState: 'runtime_terminal_pending', executionState: 'EXEC_STATE_DONE',
        asicGate: 'GATE_OPEN_SOURCE_RESTORED',
        lastCommand: 'CREATE_SESSION', lastCommandResult: 'ACCEPTED',
      };
    case 'restore-failed':
      return {
        ...s, sessionPresent: true, durableState: 'RESTORE_FAILED', restoreRequired: true,
        durableFailure: 'ERR_RESTORE_VERIFY_TIMEOUT',
        runtimeState: 'runtime_restore_source_pending', executionState: 'EXEC_STATE_RESTORE_FAILED_HELD',
        leaseOwner: 'OWNER_SOURCE_RESTORE', leasePhase: 'PHASE_RESTORING_SOURCE',
        protocolStartPermitted: false, asicGate: 'GATE_INHIBITED',
        deadlineStatus: 'RESTORE_PENDING',
      };
    case 'recovery-guard':
      return {
        ...s, sessionPresent: true, durableState: 'RECOVERY_REQUIRED', operatorRecoveryRequired: true,
        durableFailure: 'ERR_RECOVERY_REQUIRED',
        runtimeState: 'runtime_recovery_guard', executionState: 'EXEC_STATE_RECOVERY_GUARD',
        leaseOwner: 'OWNER_RECOVERY_GUARD', leasePhase: 'PHASE_RECOVERY_GUARD',
        protocolStartPermitted: false, asicGate: 'GATE_INHIBITED',
        apiConflictCode: 'OPERATION_RECOVERY_LOCKED',
      };
    case 'time-untrusted':
      return {
        ...s, sessionPresent: true, durableState: 'TARGET_ACTIVE', restoreRequired: true,
        runtimeState: 'runtime_waiting_for_trusted_time', executionState: 'EXEC_STATE_IDLE',
        leaseOwner: 'OWNER_BOOT_RECOVERY', leasePhase: 'PHASE_WAITING_FOR_TRUSTED_TIME',
        protocolStartPermitted: false, asicGate: 'GATE_INHIBITED',
        trustedTimeRequired: true, trustedTimeAvailable: false,
        deadlineStatus: 'UNKNOWN', heartbeatStatus: 'TIME_UNTRUSTED',
      };
    case 'heartbeat-failed':
      return {
        ...s, sessionPresent: true, durableState: 'TARGET_ACTIVE', restoreRequired: true,
        runtimeState: 'runtime_verify_target_pending', executionState: 'EXEC_STATE_TARGET_MINING',
        leaseOwner: 'OWNER_TIMED_SESSION', leasePhase: 'PHASE_VERIFYING_TARGET',
        protocolStartPermitted: false, asicGate: 'GATE_OPEN_TARGET', targetMiningGrantActive: true,
        trustedTimeRequired: true, trustedTimeAvailable: true,
        deadlineStatus: 'ACTIVE', remainingSecondsValid: true, remainingSeconds: 1800,
        heartbeatStatus: 'PERSIST_FAILED', heartbeatCommits: 3,
      };
    case 'conflict':
      return { ...s, apiConflictCode: 'OPERATION_BUSY_MANUAL_POOL_CHANGE', leaseOwner: 'OWNER_MANUAL_POOL_PATCH', leasePhase: 'PHASE_ACTIVE' };
    case 'mailbox-unavailable':
      return { ...s, runtimeInitialized: false, runtimeState: 'runtime_bootstrapping' };
    case 'not-present':
    default:
      return s;
  }
}

function classify(err: HttpErrorResponse): TimedSessionFailure {
  switch (err.status) {
    case 400: {
      const code = typeof err.error?.code === 'string' ? err.error.code : undefined;
      return { kind: 'validation', validationCode: code };
    }
    case 401:
      return { kind: 'origin-denied' };
    case 404:
      return { kind: 'not-present' };
    case 409: {
      const body = err.error && typeof err.error === 'object' ? (err.error as OperationConflict) : undefined;
      return { kind: 'conflict', conflict: body };
    }
    case 503:
      return { kind: 'unavailable' };
    case 0:
      return { kind: 'offline' };
    default:
      return { kind: 'unknown' };
  }
}

@Injectable({ providedIn: 'root' })
export class TimedSessionApiService {
  /** Development-only posture selector; ignored entirely in production builds. */
  public mockPosture: MockPosture = 'free';
  private mockSequence = 1;
  private mockPendingId = 0;
  private mockPendingReads = 0;
  private mockLastId = 0;
  private mockPendingKind: TimedSessionStatus['lastCommand'] = 'NONE';
  private mockLastKind: TimedSessionStatus['lastCommand'] = 'NONE';

  constructor(private http: HttpClient) {}

  /**
   * True when the real device routes must be used. It is a single named seam
   * so tests can exercise BOTH the live HTTP path (through
   * HttpTestingController) and the development mocks deterministically,
   * without mutating the shared `environment` object.
   */
  protected live(): boolean {
    return environment.production;
  }

  public get isMocked(): boolean {
    return !this.live();
  }

  /**
   * One status read. Emits the sanitized status on 200 and errors with a
   * bounded `TimedSessionFailure` for every other outcome. No caching, no
   * retry, no logging.
   */
  public getStatus(): Observable<TimedSessionStatus> {
    if (!this.live()) {
      if (this.mockPosture === 'not-present') {
        return throwError(() => ({ kind: 'not-present' } as TimedSessionFailure)).pipe(delay(120));
      }
      return of(this.applyMockCommandEcho(mockStatus(this.mockPosture, this.mockSequence++))).pipe(delay(120));
    }
    return this.http
      .get<TimedSessionStatus>(getTimedSession.PATH, { observe: 'response' })
      .pipe(
        map((r: HttpResponse<TimedSessionStatus>) => r.body as TimedSessionStatus),
        catchError((e: HttpErrorResponse) => throwError(() => classify(e))),
      );
  }

  /**
   * Submit exactly ONE create command. HTTP 202 means the device queued it —
   * never that a session started. The request body is passed straight through
   * and is not retained, cached or logged anywhere in this service.
   */
  public createSession(body: TimedSessionCreateRequest): Observable<TimedSessionAccepted> {
    if (!this.live()) {
      return this.mockAccept(body.client_request_id ?? 0, 'CREATE_SESSION');
    }
    return this.postAccepted(createTimedSession.PATH, body);
  }

  /** Submit exactly ONE Restore Now command. */
  public restoreNow(body: TimedSessionActionRequest = {}): Observable<TimedSessionAccepted> {
    if (!this.live()) {
      return this.mockAccept(body.client_request_id ?? 0, 'RESTORE_NOW');
    }
    return this.postAccepted(restoreTimedSession.PATH, body);
  }

  /** Submit exactly ONE terminal-acknowledgement command. */
  public acknowledgeTerminal(body: TimedSessionActionRequest = {}): Observable<TimedSessionAccepted> {
    if (!this.live()) {
      return this.mockAccept(body.client_request_id ?? 0, 'ACKNOWLEDGE_TERMINAL');
    }
    return this.postAccepted(acknowledgeTimedSession.PATH, body);
  }

  /**
   * Development-mock acceptance. Mirrors the device contract exactly: the id
   * is echoed on the 202, the very next status read still reports the command
   * as pending, and only the read after that reports it as processed. Without
   * that second step the mock would let the page resolve a command from a
   * status the device produced before it — the precise mistake the real
   * correlation rule exists to prevent.
   */
  private mockAccept(clientRequestId: number, kind: TimedSessionStatus['lastCommand']): Observable<TimedSessionAccepted> {
    this.mockPendingId = clientRequestId;
    this.mockPendingKind = kind ?? 'NONE';
    this.mockPendingReads = 1;
    return of<TimedSessionAccepted>({
      accepted: true,
      requestSequence: this.mockSequence++,
      clientRequestId,
    }).pipe(delay(200));
  }

  /** Overlays the mock command lifecycle onto a synthetic posture. */
  private applyMockCommandEcho(status: TimedSessionStatus): TimedSessionStatus {
    if (this.mockPendingId === 0) {
      return status;
    }
    if (this.mockPendingReads > 0) {
      this.mockPendingReads--;
      return {
        ...status, commandPending: true,
        pendingCommand: this.mockPendingKind,
        lastClientRequestId: this.mockLastId,
      };
    }
    this.mockLastId = this.mockPendingId;
    this.mockLastKind = this.mockPendingKind;
    this.mockPendingId = 0;
    // The device echoes the id AND the kind it processed. The posture itself
    // is whatever the selected mock scenario says, so a mocked command only
    // reaches "posture reached" when the operator selects a posture that
    // genuinely shows the transition — exactly like real hardware.
    return {
      ...status,
      commandPending: false,
      lastClientRequestId: this.mockLastId,
      lastCommand: this.mockLastKind,
      lastCommandResult: 'ACCEPTED',
    };
  }

  private postAccepted(path: string, body: unknown): Observable<TimedSessionAccepted> {
    return this.http
      .post<TimedSessionAccepted>(path, body, { observe: 'response' })
      .pipe(
        map((r: HttpResponse<TimedSessionAccepted>) => (r.body ?? { accepted: true, requestSequence: 0 })),
        catchError((e: HttpErrorResponse) => throwError(() => classify(e))),
      );
  }
}

/** Exposed for tests: the request timeout budget the dashboard uses. */
export const TIMED_SESSION_API_TIMEOUT = API_TIMEOUT;
