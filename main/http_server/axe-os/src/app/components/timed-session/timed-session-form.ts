/**
 * NeuralAxe Timed Pool Session — PURE form model, duration conversion and
 * operator-aid validation (Phase 2M.1B, Gate B9).
 *
 * THE DEVICE REMAINS AUTHORITATIVE. Everything here is an operator aid that
 * catches obvious mistakes before a round trip; the device re-validates every
 * field itself and its answer always wins.
 *
 * BY CONSTRUCTION this module has no password field, no source field, no
 * session identifier and no custom-certificate TLS option — the committed
 * Gate B8 request schema has none of them, and the device rejects any such key
 * before queueing a command.
 */

import { TimedSessionCreateRequest } from 'src/app/generated/models';

/** Committed Gate B1 duration bounds, mirrored for the operator aid only. */
export const MIN_DURATION_S = 900;
export const MAX_DURATION_S = 86400;

/** Committed Gate B1 bounded field sizes (buffer size − NUL). */
export const MAX_HOST_LEN = 79;
export const MAX_ACCOUNT_LEN = 127;

export type TargetProtocol = TimedSessionCreateRequest['target_protocol'];
export type TargetTlsMode = TimedSessionCreateRequest['target_tls_mode'];
export type TargetChain = TimedSessionCreateRequest['target_chain'];

/** Only the enums the committed backend accepts are ever offered. */
export const PROTOCOL_OPTIONS: ReadonlyArray<{ value: TargetProtocol; label: string }> = [
  { value: 'stratum_v1', label: 'Stratum V1' },
  { value: 'stratum_v2', label: 'Stratum V2' },
];

/**
 * TLS options deliberately exclude any custom-certificate mode: the committed
 * execution layer could not restore one exactly, so the device rejects it.
 */
export const TLS_OPTIONS: ReadonlyArray<{ value: TargetTlsMode; label: string }> = [
  { value: 'disabled', label: 'Disabled' },
  { value: 'bundled', label: 'Bundled certificates' },
];

export const CHAIN_OPTIONS: ReadonlyArray<{ value: TargetChain; label: string }> = [
  { value: 'bitcoin', label: 'Bitcoin' },
  { value: 'bitcoin_cash', label: 'Bitcoin Cash' },
  { value: 'custom_unknown', label: 'Custom / unknown' },
];

/** Bounded duration presets offered alongside the hours/minutes inputs. */
export const DURATION_PRESETS: ReadonlyArray<{ label: string; hours: number; minutes: number }> = [
  { label: '15 min', hours: 0, minutes: 15 },
  { label: '1 hour', hours: 1, minutes: 0 },
  { label: '4 hours', hours: 4, minutes: 0 },
  { label: '12 hours', hours: 12, minutes: 0 },
  { label: '24 hours', hours: 24, minutes: 0 },
];

/**
 * The bounded in-memory form value. It lives ONLY in component memory: it is
 * never written to localStorage, sessionStorage, IndexedDB, a URL, router
 * state or a log, and it is cleared after an accepted submission and on
 * component destruction.
 */
export interface TimedSessionFormValue {
  hours: number | null;
  minutes: number | null;
  targetHost: string;
  targetPort: number | null;
  targetAccount: string;
  targetProtocol: TargetProtocol;
  targetTlsMode: TargetTlsMode;
  targetChain: TargetChain;
}

export function emptyForm(): TimedSessionFormValue {
  return {
    hours: 1,
    minutes: 0,
    targetHost: '',
    targetPort: null,
    targetAccount: '',
    targetProtocol: 'stratum_v1',
    targetTlsMode: 'disabled',
    targetChain: 'bitcoin',
  };
}

/**
 * Deterministic hours+minutes → seconds. Non-finite or negative parts count as
 * zero; the result is NOT clamped, so an out-of-range total is reported by
 * validation rather than silently corrected.
 */
export function toDurationSeconds(hours: number | null, minutes: number | null): number {
  const h = Number.isFinite(hours) ? Math.max(0, Math.floor(hours as number)) : 0;
  const m = Number.isFinite(minutes) ? Math.max(0, Math.floor(minutes as number)) : 0;
  return h * 3600 + m * 60;
}

/** Inverse of `toDurationSeconds`, for presets and display. */
export function fromDurationSeconds(seconds: number): { hours: number; minutes: number } {
  const total = Number.isFinite(seconds) && seconds > 0 ? Math.floor(seconds) : 0;
  return { hours: Math.floor(total / 3600), minutes: Math.floor((total % 3600) / 60) };
}

/** Bounded field keys a validation problem can point at. */
export type FormField = 'duration' | 'targetHost' | 'targetPort' | 'targetAccount'
  | 'targetProtocol' | 'targetTlsMode' | 'targetChain';

export interface FormProblem {
  readonly field: FormField;
  readonly message: string;
}

export interface FormCheck {
  readonly valid: boolean;
  readonly problems: readonly FormProblem[];
  readonly durationSeconds: number;
}

const PROTOCOL_VALUES = new Set<string>(PROTOCOL_OPTIONS.map((o) => o.value));
const TLS_VALUES = new Set<string>(TLS_OPTIONS.map((o) => o.value));
const CHAIN_VALUES = new Set<string>(CHAIN_OPTIONS.map((o) => o.value));

/**
 * Operator-aid validation. Deterministic and side-effect free.
 *
 * It deliberately does NOT trim host or account: trimming would change the
 * pool identity the device must later compare byte-for-byte, so leading or
 * trailing whitespace is reported as a problem instead of being silently
 * "fixed".
 */
export function checkForm(value: TimedSessionFormValue): FormCheck {
  const problems: FormProblem[] = [];
  const durationSeconds = toDurationSeconds(value.hours, value.minutes);

  if (durationSeconds < MIN_DURATION_S) {
    problems.push({ field: 'duration', message: 'The shortest session is 15 minutes.' });
  } else if (durationSeconds > MAX_DURATION_S) {
    problems.push({ field: 'duration', message: 'The longest session is 24 hours.' });
  }

  const host = value.targetHost ?? '';
  if (host.length === 0) {
    problems.push({ field: 'targetHost', message: 'A target pool host is required.' });
  } else if (host.length > MAX_HOST_LEN) {
    problems.push({ field: 'targetHost', message: `The host must be ${MAX_HOST_LEN} characters or fewer.` });
  } else if (host !== host.trim()) {
    problems.push({
      field: 'targetHost',
      message: 'Remove the leading or trailing spaces — they are part of the pool identity and are not removed for you.',
    });
  }

  const port = value.targetPort;
  if (port === null || !Number.isFinite(port)) {
    problems.push({ field: 'targetPort', message: 'A target pool port is required.' });
  } else if (!Number.isInteger(port) || port < 1 || port > 65535) {
    problems.push({ field: 'targetPort', message: 'The port must be a whole number between 1 and 65535.' });
  }

  const account = value.targetAccount ?? '';
  if (account.length === 0) {
    problems.push({ field: 'targetAccount', message: 'A target account or worker is required.' });
  } else if (account.length > MAX_ACCOUNT_LEN) {
    problems.push({ field: 'targetAccount', message: `The account must be ${MAX_ACCOUNT_LEN} characters or fewer.` });
  } else if (account !== account.trim()) {
    problems.push({
      field: 'targetAccount',
      message: 'Remove the leading or trailing spaces — they are part of the pool identity and are not removed for you.',
    });
  }

  if (!PROTOCOL_VALUES.has(value.targetProtocol)) {
    problems.push({ field: 'targetProtocol', message: 'Choose a supported Stratum protocol.' });
  }
  if (!TLS_VALUES.has(value.targetTlsMode)) {
    problems.push({ field: 'targetTlsMode', message: 'Choose a supported TLS mode.' });
  }
  if (!CHAIN_VALUES.has(value.targetChain)) {
    problems.push({ field: 'targetChain', message: 'Choose a supported chain label.' });
  }

  return { valid: problems.length === 0, problems, durationSeconds };
}

/**
 * Build the exact committed Gate B8 request. Only the eight documented fields
 * exist; there is no place to put a password, a source identity or a session
 * id even by accident.
 */
export function buildCreateRequest(value: TimedSessionFormValue): TimedSessionCreateRequest {
  return {
    duration_seconds: toDurationSeconds(value.hours, value.minutes),
    target_host: value.targetHost,
    target_port: value.targetPort ?? 0,
    target_account: value.targetAccount,
    target_protocol: value.targetProtocol,
    target_tls_mode: value.targetTlsMode,
    target_chain: value.targetChain,
  };
}

/** One row of the review dialog: only what the operator just typed. */
export interface ReviewRow {
  readonly label: string;
  readonly value: string;
}

function optionLabel(
  options: ReadonlyArray<{ value: string; label: string }>, value: string,
): string {
  return options.find((o) => o.value === value)?.label ?? value;
}

export function buildReview(value: TimedSessionFormValue): readonly ReviewRow[] {
  const seconds = toDurationSeconds(value.hours, value.minutes);
  const { hours, minutes } = fromDurationSeconds(seconds);
  const duration = hours > 0
    ? `${hours} h ${String(minutes).padStart(2, '0')} m`
    : `${minutes} m`;
  return [
    { label: 'Duration', value: `${duration} (${seconds} s)` },
    { label: 'Target host', value: value.targetHost },
    { label: 'Target port', value: value.targetPort === null ? '' : String(value.targetPort) },
    { label: 'Target account', value: value.targetAccount },
    { label: 'Protocol', value: optionLabel(PROTOCOL_OPTIONS, value.targetProtocol) },
    { label: 'TLS', value: optionLabel(TLS_OPTIONS, value.targetTlsMode) },
    { label: 'Chain', value: optionLabel(CHAIN_OPTIONS, value.targetChain) },
  ];
}

/** The statements the operator must read before confirming a create. */
export const CREATE_CONFIRM_STATEMENTS: readonly string[] = [
  'The device captures its OWN current pool configuration internally. You are not being asked for it, and it cannot be supplied.',
  'Your existing pool password is kept exactly as it is. It is never read, sent or changed by a timed session.',
  'Applying the target pool is transactional: the device verifies it live before authorising any mining on it.',
  'Returning to your original pool is mandatory and is owed from the moment the target is first applied.',
  'The duration and deadline cannot be edited afterwards, and no operation extends a running session.',
  'This is a controlled pool session — it is not a hardware performance-tuning operation.',
];

/** The statements shown before Restore Now. */
export const RESTORE_CONFIRM_STATEMENTS: readonly string[] = [
  'Mining on the target pool stops.',
  'The device begins returning to the pool configuration it captured when the session started.',
  'Restoration is verified before the session can complete, so it may take a little time.',
  'This request is sent once. It is never resent automatically.',
];

/** The statements shown before acknowledging a terminal result. */
export const ACK_CONFIRM_STATEMENTS: readonly string[] = [
  'The retained session result is cleared from the device.',
  'Your pool configuration is not changed in any way.',
  'A new timed session becomes possible afterwards.',
  'The device clears the result only after it has durably proven the record is gone.',
];
