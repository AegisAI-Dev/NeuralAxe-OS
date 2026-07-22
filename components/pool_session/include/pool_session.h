#ifndef POOL_SESSION_H_
#define POOL_SESSION_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * NeuralAxe timed pool sessions — pure domain models and deterministic finite
 * state machine (Phase 2M.1B, Gate B1). Board 601 / BM1370 only.
 *
 * This module is PURE: no NVS, no SNTP, no wall clock, no FreeRTOS, no queues,
 * no Stratum, no HTTP, no logging dependency, no heap allocation, no restart.
 * It only decides, given a session and one typed event, what the next state,
 * abstract side-effect INTENT, machine error code, retry decision and terminal
 * classification are. Nothing here executes a side effect — the future
 * scheduler (Gate B6+) does that.
 *
 * Architectural source of truth (committed Phase 2M.1A audit):
 *   docs/NEURALAXE_PHASE_2M1A_TIMED_POOL_SESSIONS_AUDIT.md
 *   docs/NEURALAXE_PHASE_2M1A_TIMED_POOL_SESSIONS_STATE_MACHINE.md
 *   docs/NEURALAXE_PHASE_2M1A_TIMED_POOL_SESSIONS_SECURITY.md
 *
 * SECURITY INVARIANT: there is NO password field anywhere in this module — not
 * in models, requests, events, transition results, status or errors. Timed
 * sessions are Keep-current-password only (Phase 2M.1A Policy 1); a replacement
 * password mode is representable ONLY as a rejected policy tag carrying no bytes.
 *
 * SERIALIZATION BOUNDARY (Gate B3, not B1): these structs are NOT the on-flash
 * format. Persistence must go through an explicit versioned serializer — do NOT
 * assume sizeof(PoolSession) or struct padding is the NVS layout. CRC/NVS/dual
 * A/B slots are out of scope for B1. POOL_SESSION_MODEL_VERSION gates future
 * migrations.
 */

#define POOL_SESSION_MODEL_VERSION 1u

/* Board / ASIC eligibility (Phase 2M.1A: board 601 / BM1370 only). */
#define POOL_SESSION_SUPPORTED_BOARD "601"
#define POOL_SESSION_SUPPORTED_ASIC  "BM1370"

/* Duration bounds (seconds): 15 minutes .. 24 hours (Phase 2M.1A MVP). */
#define POOL_SESSION_MIN_DURATION_S 900u    /* 15 min */
#define POOL_SESSION_MAX_DURATION_S 86400u  /* 24 h   */

/*
 * Bounded, fixed field sizes (buffer sizes including the NUL terminator).
 * Chosen to hold any realistic Stratum identity without truncation while
 * staying small enough for a future bounded NVS record. These sizes are part
 * of the model contract; changing them is a model-version change.
 */
#define POOL_SESSION_HOST_MAX       80  /* pool hostname (strlen <= 79)       */
#define POOL_SESSION_USER_MAX       128 /* account/worker (wallet+worker)     */
#define POOL_SESSION_PROFILE_ID_MAX 24  /* optional non-secret profile label  */
#define POOL_SESSION_BOARD_MAX      16  /* board version string, e.g. "601"   */
#define POOL_SESSION_ASIC_MAX       16  /* asic model string, e.g. "BM1370"   */

/* Bounded retry maxima (one place — Phase 2M.1B Stage 11). No timing here. */
#define POOL_SESSION_MAX_TARGET_APPLY_RETRIES    3u
#define POOL_SESSION_MAX_TARGET_RESTART_RETRIES  3u
#define POOL_SESSION_MAX_TARGET_VERIFY_RETRIES   3u
#define POOL_SESSION_MAX_RESTORE_APPLY_RETRIES   3u
#define POOL_SESSION_MAX_RESTORE_RESTART_RETRIES 3u
#define POOL_SESSION_MAX_RESTORE_VERIFY_RETRIES  3u

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */

/* Explicit chain label. NEVER inferred from a hostname (Phase 2M.1A Inv 14). */
typedef enum {
    POOL_CHAIN_BITCOIN       = 0,
    POOL_CHAIN_BITCOIN_CASH  = 1,
    POOL_CHAIN_CUSTOM_UNKNOWN = 2,
    POOL_CHAIN__COUNT
} PoolChainType;

/*
 * Password policy. B1/MVP supports only KEEP_CURRENT. REPLACE is a rejected tag
 * (validation returns ERR_PW_MODE_UNSUPPORTED) and carries NO password bytes;
 * it exists only so a replacement request is representable and rejectable.
 */
typedef enum {
    POOL_SESSION_PW_KEEP_CURRENT = 0,
    POOL_SESSION_PW_REPLACE      = 1, /* rejected in B1 — no secret is stored  */
    POOL_SESSION_PW__COUNT
} PoolSessionPasswordPolicy;

/* Stratum protocol of an endpoint (non-secret identity only). */
typedef enum {
    POOL_PROTO_STRATUM_V1 = 0,
    POOL_PROTO_STRATUM_V2 = 1,
    POOL_PROTO__COUNT
} PoolSessionProtocol;

/* Deterministic FSM states (Phase 2M.1A state-machine report §1). */
typedef enum {
    POOL_STATE_IDLE = 0,
    POOL_STATE_PREPARING,
    POOL_STATE_TARGET_SNAPSHOT_COMMITTED,
    POOL_STATE_APPLYING_TARGET,
    POOL_STATE_RESTARTING_FOR_TARGET,
    POOL_STATE_VERIFYING_TARGET,
    POOL_STATE_TARGET_ACTIVE,
    POOL_STATE_RESTORE_DUE,
    POOL_STATE_APPLYING_RESTORE,
    POOL_STATE_RESTARTING_FOR_RESTORE,
    POOL_STATE_VERIFYING_RESTORE,
    POOL_STATE_COMPLETE,
    POOL_STATE_TARGET_FAILED,
    POOL_STATE_RESTORE_FAILED,
    POOL_STATE_INTERRUPTED,
    POOL_STATE_RECOVERY_REQUIRED,
    POOL_STATE_CANCELLED,
    POOL_STATE__COUNT
} PoolSessionState;

/* Typed input events (Phase 2M.1B Stage 5). Carry bounded scalars only. */
typedef enum {
    POOL_EVT_CREATE_REQUESTED = 0,
    POOL_EVT_SOURCE_SNAPSHOT_COMMITTED,
    POOL_EVT_TARGET_APPLY_REQUESTED,
    POOL_EVT_TARGET_RESTART_STARTED,
    POOL_EVT_TARGET_RESTART_COMPLETE,
    POOL_EVT_TARGET_CONNECTION_OBSERVED,
    POOL_EVT_TARGET_MINING_OBSERVED,
    POOL_EVT_TARGET_HOST_VERIFIED,
    POOL_EVT_TARGET_VERIFY_TIMEOUT,
    POOL_EVT_TARGET_APPLY_FAILED,
    POOL_EVT_TARGET_RETRY_REQUESTED,
    POOL_EVT_DEADLINE_REACHED,
    POOL_EVT_RESTORE_NOW_REQUESTED,
    POOL_EVT_CANCEL_REQUESTED,
    POOL_EVT_RESTORE_APPLY_REQUESTED,
    POOL_EVT_RESTORE_RESTART_STARTED,
    POOL_EVT_RESTORE_RESTART_COMPLETE,
    POOL_EVT_RESTORE_CONNECTION_OBSERVED,
    POOL_EVT_RESTORE_MINING_OBSERVED,
    POOL_EVT_RESTORE_IDENTITY_VERIFIED,
    POOL_EVT_RESTORE_VERIFY_TIMEOUT,
    POOL_EVT_RESTORE_APPLY_FAILED,
    POOL_EVT_RESTORE_RETRY_REQUESTED,
    POOL_EVT_DEVICE_RESTART_OBSERVED,
    POOL_EVT_RECORD_CORRUPT,
    POOL_EVT_RECORD_SCHEMA_UNSUPPORTED,
    POOL_EVT_INTERRUPT_OBSERVED,
    POOL_EVT_ACKNOWLEDGE_TERMINAL,
    POOL_EVT__COUNT
} PoolSessionEventType;

/* Stable machine-readable codes (Phase 2M.1B Stage 6). No user-facing prose. */
typedef enum {
    ERR_NONE = 0,
    ERR_INVALID_REQUEST,
    ERR_UNSUPPORTED_BOARD,
    ERR_SESSION_ALREADY_ACTIVE,
    ERR_NO_ACTIVE_SESSION,
    ERR_INVALID_DURATION,
    ERR_PW_MODE_UNSUPPORTED,
    ERR_SOURCE_IDENTITY_INVALID,
    ERR_TARGET_IDENTITY_INVALID,
    ERR_TARGET_EQUALS_SOURCE,
    ERR_STATE_CONFLICT,
    ERR_ILLEGAL_TRANSITION,
    ERR_TARGET_APPLY,
    ERR_TARGET_RESTART,
    ERR_TARGET_CONNECT_TIMEOUT,
    ERR_TARGET_VERIFY_TIMEOUT,
    ERR_TARGET_IDENTITY_MISMATCH,
    ERR_RESTORE_APPLY,
    ERR_RESTORE_RESTART,
    ERR_RESTORE_CONNECT_TIMEOUT,
    ERR_RESTORE_VERIFY_TIMEOUT,
    ERR_RESTORE_IDENTITY_MISMATCH,
    ERR_RETRY_EXHAUSTED,
    ERR_RECORD_CORRUPT,
    ERR_SCHEMA_UNSUPPORTED,
    ERR_RECOVERY_REQUIRED,
    ERR_CANCELLED,
    ERR_INTERRUPTED,
    POOL_SESSION_ERR__COUNT
} PoolSessionError;

/*
 * Abstract side-effect INTENTS. Descriptions only — B1 never executes them.
 * The scheduler (B6+) maps these to real NVS/restart/verification actions.
 */
typedef enum {
    POOL_SIDE_NO_EFFECT = 0,
    POOL_SIDE_COMMIT_SOURCE_SNAPSHOT,
    POOL_SIDE_APPLY_TARGET_CONFIGURATION,
    POOL_SIDE_RESTART_FOR_TARGET,
    POOL_SIDE_BEGIN_TARGET_VERIFICATION,
    POOL_SIDE_ARM_MONOTONIC_DEADLINE,
    POOL_SIDE_APPLY_SOURCE_CONFIGURATION,
    POOL_SIDE_RESTART_FOR_RESTORE,
    POOL_SIDE_BEGIN_RESTORE_VERIFICATION,
    POOL_SIDE_PERSIST_TRANSITION,
    POOL_SIDE_CLEAR_SESSION_RECORD,
    POOL_SIDE_RETAIN_TERMINAL_RESULT,
    POOL_SIDE__COUNT
} PoolSessionSideEffect;

/* ------------------------------------------------------------------ */
/* Bounded domain models                                              */
/* ------------------------------------------------------------------ */

/* Non-secret endpoint identity. NO password field, by design. */
typedef struct {
    char                host[POOL_SESSION_HOST_MAX];
    uint16_t            port;
    char                user[POOL_SESSION_USER_MAX]; /* account/worker only */
    PoolSessionProtocol protocol;
    bool                tls;
} PoolEndpoint;

/* Pool configuration identity: primary + optional fallback + explicit chain. */
typedef struct {
    PoolEndpoint  primary;
    PoolEndpoint  fallback;
    bool          fallback_enabled;
    PoolChainType chain;                         /* explicit; never host-inferred */
    char          profile_id[POOL_SESSION_PROFILE_ID_MAX]; /* optional, may be "" */
} PoolConfigIdentity;

/* Create request. Bounded, no secret. */
typedef struct {
    uint32_t                  model_version; /* must equal POOL_SESSION_MODEL_VERSION */
    uint32_t                  session_id;    /* non-zero idempotency/session identity  */
    uint32_t                  duration_s;    /* [MIN..MAX] seconds                      */
    PoolSessionPasswordPolicy password_policy;
    PoolConfigIdentity        source;
    PoolConfigIdentity        target;
    char                      board_version[POOL_SESSION_BOARD_MAX];
    char                      asic_model[POOL_SESSION_ASIC_MAX];
} PoolSessionRequest;

/* Layered verification evidence (connection + mining + identity). */
typedef struct {
    bool connection_observed;
    bool mining_observed;
    bool identity_verified;
} PoolSessionVerify;

/* Bounded retry counters (saturate at their maxima; never wrap). */
typedef struct {
    uint8_t target_apply;
    uint8_t target_restart;
    uint8_t target_verify;
    uint8_t restore_apply;
    uint8_t restore_restart;
    uint8_t restore_verify;
} PoolSessionRetries;

/*
 * The session model as seen by the pure FSM. NO password field. `generation`
 * is a reserved placeholder for the Gate B3 dual-slot record and is not
 * interpreted by B1 logic.
 */
typedef struct {
    uint32_t                  model_version;
    uint32_t                  session_id;
    uint32_t                  generation;      /* reserved for B3 record slots */
    PoolSessionState          state;
    PoolConfigIdentity        source;          /* immutable after snapshot commit */
    PoolConfigIdentity        target;
    uint32_t                  duration_s;
    PoolSessionPasswordPolicy password_policy;
    PoolSessionVerify         target_verify;
    PoolSessionVerify         restore_verify;
    PoolSessionRetries        retries;
    PoolSessionError          last_error;
    bool                      cancel_requested;  /* operator cancel drove restore */
    bool                      restore_requested; /* operator Restore Now drove restore */
    /*
     * Monotonic restore obligation. Becomes true together with the first
     * APPLY_TARGET_CONFIGURATION intent (the target pool MAY be mutated from that
     * point) and NEVER returns to false due to any apply/restart/verify failure,
     * identity mismatch, retry exhaustion, cancel, interruption or device
     * restart. It clears ONLY on entering COMPLETE (verified source restoration +
     * mining resumed). Persistence-facing: Gate B3 and boot recovery rely on this
     * safety fact surviving a reboot.
     */
    bool                      restore_required;
    PoolSessionState          terminal_result;   /* retained terminal outcome, or IDLE */
} PoolSession;

/* Result of one pure transition. `next` (via out param) is the new session. */
typedef struct {
    PoolSessionState      next_state;
    PoolSessionSideEffect side_effect;
    PoolSessionError      error;
    bool                  retry;    /* a bounded retry was taken this transition */
    bool                  terminal; /* next_state is a terminal state             */
    bool                  changed;  /* the session materially changed             */
} PoolSessionOutcome;

/* One typed event. `request` is used only for CREATE_REQUESTED (else NULL). */
typedef struct {
    PoolSessionEventType      type;
    uint32_t                  session_id; /* target session identity for the event */
    const PoolSessionRequest *request;    /* CREATE_REQUESTED only; read-only; no secret */
} PoolSessionEvent;

/* ------------------------------------------------------------------ */
/* Pure API                                                            */
/* ------------------------------------------------------------------ */

/* Initialize a fresh IDLE session (no active work). */
void pool_session_init(PoolSession *s);

/* Pure request validation. Returns ERR_NONE when valid. */
PoolSessionError pool_session_validate_request(const PoolSessionRequest *req);

/* Overflow-safe minutes->seconds. Returns false on overflow / out of bound. */
bool pool_session_minutes_to_seconds(uint32_t minutes, uint32_t *out_seconds);

/*
 * The deterministic transition engine. `current` is never modified. `out_next`
 * (may be NULL) receives the resulting session; it is safe for out_next to
 * alias current — the input is copied first. No heap, no globals, no I/O.
 */
PoolSessionOutcome pool_session_transition(const PoolSession *current,
                                           const PoolSessionEvent *event,
                                           PoolSession *out_next);

/* Non-secret equality of endpoint / config identity (operational fields). */
bool pool_endpoint_equal(const PoolEndpoint *a, const PoolEndpoint *b);
bool pool_config_identity_equal(const PoolConfigIdentity *a, const PoolConfigIdentity *b);

/* Stable machine token for a code, e.g. "ERR_TARGET_APPLY". Never a secret. */
const char *pool_session_error_str(PoolSessionError e);
/* Stable machine token for a state / side effect (diagnostics; no secret). */
const char *pool_session_state_str(PoolSessionState st);
const char *pool_session_side_effect_str(PoolSessionSideEffect se);

/* ---- Total, invalid-safe state classification helpers (Stage 4) ---- */
bool pool_state_is_persistent(PoolSessionState st);
bool pool_state_is_terminal(PoolSessionState st);       /* result state: rests awaiting operator */
bool pool_state_is_active_session(PoolSessionState st); /* non-IDLE, non-result (in progress)     */
bool pool_state_is_target_side(PoolSessionState st);
bool pool_state_is_restore_side(PoolSessionState st);
bool pool_state_allows_restore_now(PoolSessionState st);
bool pool_state_allows_cancel(PoolSessionState st);
bool pool_state_requires_source_snapshot(PoolSessionState st);
bool pool_state_requires_target_identity(PoolSessionState st);

/* ---- Session-aware safety helpers (restore obligation) ---- */
/* The monotonic restore obligation (see PoolSession.restore_required). */
bool pool_session_restore_required(const PoolSession *s);
/*
 * A result state that is SAFE to acknowledge back to IDLE — ONLY when the miner
 * cannot be left on a mutated target, i.e. the restore obligation is already
 * discharged (COMPLETE) or was never incurred (CANCELLED / a pre-mutation
 * failure). RESTORE_FAILED, RECOVERY_REQUIRED and any state with
 * restore_required==true are NOT acknowledgeable.
 */
bool pool_session_is_acknowledgeable_terminal(const PoolSession *s);
/*
 * Operation lease: manual pool change / OTA must be blocked while a session is
 * unresolved — in progress, OR a result state that still owes a restore. Only
 * IDLE and an acknowledgeable terminal are unblocked.
 */
bool pool_session_blocks_manual_pool_change(const PoolSession *s);
bool pool_session_blocks_ota(const PoolSession *s);

/* ------------------------------------------------------------------ */
/* Compile-time guards (serialization-boundary preparation, Stage 12) */
/* ------------------------------------------------------------------ */
_Static_assert(POOL_SESSION_MODEL_VERSION >= 1u, "model version must be >= 1");
_Static_assert(POOL_SESSION_MIN_DURATION_S < POOL_SESSION_MAX_DURATION_S,
               "duration bounds inverted");
_Static_assert(POOL_SESSION_MAX_DURATION_S == 86400u, "max duration must be 24h");
_Static_assert(POOL_SESSION_HOST_MAX >= 32 && POOL_SESSION_USER_MAX >= 64,
               "identity buffers too small for realistic pool identities");
_Static_assert(POOL_STATE__COUNT == 17, "state count changed — review helpers/tests");
_Static_assert(POOL_EVT__COUNT == 28, "event count changed — review transition table");
/* Reserve the generation placeholder without implying it is the NVS layout. */
_Static_assert(sizeof(((PoolSession *)0)->generation) == sizeof(uint32_t),
               "generation must remain a bounded scalar placeholder");

#endif /* POOL_SESSION_H_ */
