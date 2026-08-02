/**
 * NeuralAxe Timed Pool Session — frontend view models (Phase 2M.1B, Gate B9).
 *
 * PURE TYPES ONLY. Nothing here performs IO, reads a clock or touches browser
 * storage.
 *
 * THE BACKEND IS AUTHORITATIVE. Every type below is a presentation of a
 * sanitized Gate B8 status or response — never a source of truth and never a
 * firmware safety boundary. Frontend validation is an operator aid.
 *
 * PRIVACY BY CONSTRUCTION: no model in this file carries a pool password, a
 * source identity, a session id, a lease token, a lease generation or a record
 * generation, because the committed B8 API never emits any of them.
 */

/** Bounded runtime availability of the Gate B8 control API. */
export type TimedSessionAvailability =
  | 'CHECKING'
  | 'AVAILABLE'
  | 'API_DISABLED_OR_NOT_PRESENT'
  | 'ORIGIN_DENIED'
  | 'DEVICE_OFFLINE'
  | 'TEMPORARILY_UNAVAILABLE'
  | 'UNKNOWN_ERROR';

/** Bounded lifecycle of ONE submitted command. */
export type CommandPhase =
  | 'idle'
  | 'submitting'
  | 'accepted'
  | 'processing'
  /**
   * The device's own status now reports the posture the command asked for.
   * Deliberately NOT called `succeeded`: request identifiers are best-effort
   * correlation on an unauthenticated LAN, so this page can describe what the
   * DEVICE reports — never assert that it was this page's request that did it.
   */
  | 'posture-reached'
  | 'validation-failed'
  | 'conflict'
  | 'temporarily-unavailable'
  | 'origin-denied'
  | 'failed'
  /**
   * The device processed the command and reported that it was NOT accepted.
   * Distinct from `failed`, which means the request never got a reply at all —
   * the two need different operator actions and must not share wording.
   */
  | 'device-refused'
  /** The page ran out of request numbers it can safely issue; reload needed. */
  | 'request-ids-exhausted'
  | 'unknown';

/** Which command a banner is describing. */
export type CommandKind = 'create' | 'restore' | 'acknowledge';

/** Shared semantic severity, mapped onto the existing `nx-pill-*` tokens. */
export type Severity = 'ok' | 'info' | 'warn' | 'danger' | 'neutral';

/** One label + explanation pair. Both are bounded, human-authored strings. */
export interface LabelledState {
  readonly label: string;
  readonly detail: string;
  readonly severity: Severity;
}

/** The availability view: what the operator may do right now. */
export interface AvailabilityView extends LabelledState {
  readonly availability: TimedSessionAvailability;
  /** Create/Restore/Acknowledge controls may render at all. */
  readonly operable: boolean;
  /** A manual "check again" button should be offered. */
  readonly retryable: boolean;
  /** Automatic polling must stay stopped until the operator re-checks. */
  readonly pollingStopped: boolean;
}

/** Bounded operator timeline stage. */
export type TimelineStageId =
  | 'accepted'
  | 'snapshot'
  | 'apply'
  | 'verify'
  | 'mining'
  | 'restoring'
  | 'restore-verify'
  | 'complete'
  | 'acknowledge';

export type TimelineStageState = 'pending' | 'active' | 'done' | 'failed' | 'skipped';

export interface TimelineStage {
  readonly id: TimelineStageId;
  readonly label: string;
  readonly state: TimelineStageState;
}

export interface TimelineView {
  readonly stages: readonly TimelineStage[];
  /**
   * True when the recovery guard has interrupted the normal progression. The
   * timeline is then descriptive history, not a forecast.
   */
  readonly interrupted: boolean;
  readonly interruptionDetail: string;
}

/** Deadline presentation. Never computed from browser time. */
export interface DeadlineView extends LabelledState {
  /** True only when the backend reported a trusted remaining duration. */
  readonly remainingKnown: boolean;
  /** Backend-provided seconds; meaningful only when `remainingKnown`. */
  readonly remainingSeconds: number;
  /** Pre-formatted "1 h 04 m" style text; empty when unknown. */
  readonly remainingText: string;
}

/** Heartbeat presentation. */
export interface HeartbeatView extends LabelledState {
  readonly commits: number;
}

/** Trusted-time presentation. */
export interface TrustedTimeView extends LabelledState {
  readonly required: boolean;
  readonly available: boolean;
  /** True when the operator must be warned that timing facts are unproven. */
  readonly warn: boolean;
}

/** One sanitized conflict, mapped from the committed B5 machine code. */
export interface ConflictView {
  readonly title: string;
  readonly detail: string;
  readonly severity: Severity;
  readonly retryable: boolean;
  /** Sanitized owner CLASS label, or empty when the backend sent none. */
  readonly ownerLabel: string;
  readonly restoreRequired: boolean;
  readonly terminalAckRequired: boolean;
}

/** Bounded record of the last submitted command. NEVER holds request values. */
export interface CommandView {
  readonly kind: CommandKind | null;
  readonly phase: CommandPhase;
  readonly title: string;
  readonly detail: string;
  readonly severity: Severity;
  /** Present only when the phase is `conflict`. */
  readonly conflict: ConflictView | null;
  /** Stable backend validation code, already mapped to a human label. */
  readonly validationLabel: string;
  /** Monotonic per-boot submission sequence returned with HTTP 202. */
  readonly requestSequence: number;
  /**
   * The bounded diagnostics id this page sent with the command. The device
   * echoes the id of the command it actually PROCESSED in
   * `lastClientRequestId`, and that echo is the only thing that may resolve
   * this command: without it, a `lastCommandResult` left over from an earlier
   * command would be read as a result for this one.
   */
  readonly clientRequestId: number;
}

/**
 * The complete operator status view. Every field is derived from ONE sanitized
 * backend status payload; nothing is inferred from elapsed browser time.
 */
export interface TimedSessionView {
  readonly sessionPresent: boolean;
  readonly durable: LabelledState;
  readonly runtime: LabelledState;
  readonly execution: LabelledState;
  readonly owner: LabelledState;
  readonly phase: LabelledState;
  readonly protocol: LabelledState;
  readonly asicGate: LabelledState;
  readonly grant: LabelledState;
  readonly obligation: LabelledState;
  readonly trustedTime: TrustedTimeView;
  readonly deadline: DeadlineView;
  readonly heartbeat: HeartbeatView;
  readonly pendingCommand: LabelledState;
  readonly lastCommandResult: LabelledState;
  readonly terminalAckRequired: boolean;
  readonly operatorRecoveryRequired: boolean;
  /** Restore Now may be OFFERED (the backend still decides). */
  readonly restoreOffered: boolean;
  /** Acknowledgement may be OFFERED (the backend still decides). */
  readonly acknowledgeOffered: boolean;
  /** Create may be OFFERED (the backend still decides). */
  readonly createOffered: boolean;
  /** Bounded monotonic status sequence, for operator diagnostics only. */
  readonly statusSequence: number;
  readonly executionEnabled: boolean;
  readonly runtimeInitialized: boolean;
  readonly apiEnabled: boolean;
  /**
   * True when the build and runtime can actually carry out a timed session.
   * A device that answers the status route is not automatically a device that
   * can run one: the execution flag can be off, or the runtime may not have
   * come up. Offering a create in that state would promise a capability the
   * device does not have.
   */
  readonly capable: boolean;
}

/** The single generic fallback for any value the frontend does not know. */
export const UNKNOWN_LABEL = 'Unrecognised device state';
export const UNKNOWN_DETAIL =
  'The device reported a value this dashboard does not recognise. The device remains ' +
  'authoritative — no action is inferred from an unknown value.';
