#ifndef POOL_SESSION_API_TYPES_H_
#define POOL_SESSION_API_TYPES_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pool_session.h"

/*
 * NeuralAxe timed pool sessions — API domain types (Phase 2M.1B, Gate B8).
 * Board 601 / BM1370 only.
 *
 * This header is the PURE control-plane domain: bounded request/command/
 * status models and stable machine tokens. No ESP-IDF, no HTTP server, no
 * cJSON, no NVS, no FreeRTOS, no heap, no logging, no global mutable state.
 *
 * NON-NEGOTIABLE SECURITY INVARIANTS PINNED BY CONSTRUCTION HERE:
 *  - NO password field, password bytes or password pointer exists in ANY
 *    model in this component. Timed sessions remain Keep-current-password
 *    only; a password-like JSON field is REJECTED by the parser before a
 *    command can be built (see pool_session_api_parser.h).
 *  - NO client-supplied SOURCE identity exists. The source configuration is
 *    captured internally by the owner task from the device's own effective
 *    configuration and is immutable after durable session creation.
 *  - NO session id, lease token, lease generation or record generation is
 *    accepted from a client or exposed in any published status.
 *  - The published status model has NO string field by construction, so no
 *    hostname, account, worker, wallet, profile, raw record or raw NVS byte
 *    can ever be carried out of it.
 *
 * TRUST BOUNDARY (honest wording, unchanged from B5/B7): the actor class
 * below identifies a REQUEST SOURCE CLASS, not a human. The API is reachable
 * only through the repository's existing private-network admission check; it
 * is NOT user-authenticated, and HTTP 409 is conflict reporting, not access
 * control.
 */

#define POOL_API_MODEL_VERSION 1u

/* Strict body bound for every new B8 request (worst-case valid create body
 * is ~464 bytes: 79-byte host + 127-byte account + keys + punctuation). */
#define POOL_API_MAX_BODY_BYTES 640u

/* Bounded object-key budget: a strictly larger object fails closed. */
#define POOL_API_MAX_JSON_KEYS 16u

/* Bounded command queue depth (static storage; no heap per request). */
#define POOL_API_COMMAND_QUEUE_DEPTH 4u

/* ------------------------------------------------------------------ */
/* Actor class (request SOURCE class — never a human identity)         */
/* ------------------------------------------------------------------ */

typedef enum {
    POOL_API_ACTOR_UNKNOWN = 0, /* fail-closed default                  */
    POOL_API_ACTOR_LOCAL_HTTP,  /* private-network HTTP request         */
    POOL_API_ACTOR_INTERNAL,    /* firmware-internal submission (tests) */
    POOL_API_ACTOR__COUNT
} PoolApiActorClass;

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    POOL_API_CMD_NONE = 0, /* zeroed slot: never processed */
    POOL_API_CMD_CREATE_SESSION,
    POOL_API_CMD_RESTORE_NOW,
    POOL_API_CMD_ACKNOWLEDGE_TERMINAL,
    POOL_API_CMD__COUNT
} PoolApiCommandKind;

/* ------------------------------------------------------------------ */
/* Stable machine validation codes (HTTP 400 bodies)                   */
/* ------------------------------------------------------------------ */

typedef enum {
    API_VALID_OK = 0,
    API_ERR_BODY_TOO_LARGE,
    API_ERR_BODY_MALFORMED,
    API_ERR_BODY_NOT_OBJECT,
    API_ERR_UNKNOWN_FIELD,
    API_ERR_DUPLICATE_FIELD,
    API_ERR_MISSING_FIELD,
    API_ERR_NULL_FIELD,
    API_ERR_TYPE_MISMATCH,
    API_ERR_TOO_MANY_FIELDS,
    API_ERR_PASSWORD_FIELD_REJECTED,
    API_ERR_SOURCE_FIELD_REJECTED,
    API_ERR_SESSION_ID_REJECTED,
    API_ERR_INTERNAL_FIELD_REJECTED,
    API_ERR_DURATION_OUT_OF_RANGE,
    API_ERR_HOST_EMPTY,
    API_ERR_HOST_TOO_LONG,
    API_ERR_PORT_INVALID,
    API_ERR_ACCOUNT_EMPTY,
    API_ERR_ACCOUNT_TOO_LONG,
    API_ERR_PROTOCOL_UNSUPPORTED,
    API_ERR_TLS_MODE_UNSUPPORTED,
    API_ERR_TLS_CUSTOM_REJECTED,
    API_ERR_CHAIN_UNSUPPORTED,
    API_ERR_REQUEST_ID_INVALID,
    POOL_API_VALIDATION__COUNT
} PoolApiValidation;

/* ------------------------------------------------------------------ */
/* Submission status (immediate HTTP answer)                           */
/* ------------------------------------------------------------------ */

typedef enum {
    API_SUBMIT_ACCEPTED = 0, /* queued for the owner task (HTTP 202)     */
    API_SUBMIT_QUEUE_FULL,   /* bounded queue saturated  (HTTP 503)      */
    API_SUBMIT_NOT_READY,    /* processor unbound/uninit (HTTP 503)      */
    API_SUBMIT_INVALID,      /* malformed command        (HTTP 400)      */
    POOL_API_SUBMIT__COUNT
} PoolApiSubmitStatus;

/* ------------------------------------------------------------------ */
/* Command result (authoritative outcome, visible through status)      */
/* ------------------------------------------------------------------ */

typedef enum {
    API_CMD_RESULT_NONE = 0, /* nothing processed on this boot           */
    API_CMD_RESULT_PENDING,  /* queued, not yet consumed                 */
    API_CMD_RESULT_ACCEPTED, /* completed and durably proven             */
    API_CMD_RESULT_REJECTED_STATE,
    API_CMD_RESULT_REJECTED_CONFLICT,
    API_CMD_RESULT_REJECTED_HARDWARE,
    API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED,
    API_CMD_RESULT_REJECTED_TARGET_UNSUPPORTED,
    API_CMD_RESULT_REJECTED_VALIDATION,
    API_CMD_RESULT_FAILED_PERSIST,
    API_CMD_RESULT_FAILED_READBACK,
    API_CMD_RESULT_FAILED_OWNERSHIP,
    API_CMD_RESULT_RECOVERY_GUARD,
    /*
     * The session was created durably but the committed execution layer
     * refused to adopt it in this boot. The record was then safely
     * CANCELLED through the pre-mutation B1 path — no pool was touched and
     * no restoration is owed — and the terminal result awaits
     * acknowledgement. This is NEVER reported as an accepted create.
     */
    API_CMD_RESULT_ADOPTION_FAILED,
    POOL_API_CMD_RESULT__COUNT
} PoolApiCommandResult;

/* ------------------------------------------------------------------ */
/* Bounded TARGET_ACTIVE heartbeat status                              */
/* ------------------------------------------------------------------ */

typedef enum {
    API_HB_NOT_APPLICABLE = 0, /* not TARGET_ACTIVE / no grant           */
    API_HB_WAITING,            /* cadence window still open              */
    API_HB_DUE,                /* cadence + epoch advance both satisfied */
    API_HB_COMMITTED,          /* last attempt committed and proven      */
    API_HB_TIME_UNTRUSTED,     /* no trusted B2 epoch: never write       */
    API_HB_PERSIST_FAILED,     /* commit/readback/proof refused          */
    API_HB_RECOVERY_GUARD,     /* uncertainty: fail closed               */
    POOL_API_HB__COUNT
} PoolApiHeartbeatStatus;

/* ------------------------------------------------------------------ */
/* Deadline status                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    API_DEADLINE_UNKNOWN = 0,
    API_DEADLINE_ACTIVE,
    API_DEADLINE_EXPIRED,
    API_DEADLINE_RESTORE_PENDING,
    POOL_API_DEADLINE__COUNT
} PoolApiDeadlineStatus;

/* ------------------------------------------------------------------ */
/* Bounded create request (NO password, NO source, NO session id)      */
/* ------------------------------------------------------------------ */

/*
 * Only the TARGET primary endpoint is accepted. A target fallback is
 * deliberately NOT accepted in B8: the committed B7 configuration
 * transaction pins `use_fallback` false and verifies the PRIMARY endpoint,
 * so a target fallback would be written but never verified. The source
 * fallback IS captured internally and restored exactly.
 */
typedef struct {
    uint32_t            duration_s;        /* [900 .. 86400]              */
    char                target_host[POOL_SESSION_HOST_MAX];
    uint16_t            target_port;       /* 1 .. 65535                  */
    char                target_user[POOL_SESSION_USER_MAX]; /* account/worker */
    PoolSessionProtocol target_protocol;
    uint8_t             target_tls_mode;   /* POOL_API_TLS_* (never custom) */
    PoolChainType       target_chain;      /* explicit; never host-inferred */
    uint32_t            client_request_id; /* bounded diagnostics; 0 = none */
} PoolApiCreateRequest;

/* The only two representable TLS modes (they round-trip through the B1
 * boolean). A custom-certificate mode is rejected before command build. */
#define POOL_API_TLS_DISABLED 0u
#define POOL_API_TLS_BUNDLED  1u

/* ------------------------------------------------------------------ */
/* Bounded command (static storage; NO password field exists)          */
/* ------------------------------------------------------------------ */

typedef struct {
    PoolApiCommandKind kind;
    PoolApiActorClass  actor;
    uint32_t           client_request_id;   /* echoed diagnostics only    */
    uint32_t           submission_sequence; /* monotonic per boot          */
    PoolApiCreateRequest create;            /* CREATE_SESSION only         */
} PoolApiCommand;

/* ------------------------------------------------------------------ */
/* Sanitized status model (NO string field exists by construction)     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t model_version;

    /* Feature posture. */
    bool api_enabled;
    bool runtime_initialized;
    bool execution_enabled;

    /* Durable session facts (states only — never an identity). */
    bool             session_present;
    bool             terminal_result_pending;
    PoolSessionState durable_state;   /* POOL_STATE_IDLE when none         */
    uint16_t         durable_failure; /* PoolSessionError value            */

    /* Runtime / execution postures. */
    uint8_t runtime_state;   /* PoolRuntimeState value                     */
    uint8_t execution_state; /* PoolExecState value; 0 when disabled       */
    uint8_t execution_reason;/* PoolExecReason value                       */

    /* Ownership (sanitized CLASS + phase only — never a token). */
    uint8_t lease_owner; /* PoolOperationOwner value */
    uint8_t lease_phase; /* PoolOperationLeasePhase value */

    /* Protocol / ASIC posture. */
    bool    protocol_start_permitted; /* false == held                     */
    uint8_t asic_gate;                /* PoolExecGatePosture value         */
    bool    target_mining_grant_active;

    /* Obligations and operator duties. */
    bool restore_required;
    bool operator_recovery_required;

    /* Trusted time and the bounded deadline view. */
    bool                  trusted_time_required;
    bool                  trusted_time_available;
    PoolApiDeadlineStatus deadline_status;
    bool                  remaining_seconds_valid;
    uint32_t              remaining_seconds;

    /* Command plane. */
    bool                 command_pending;
    PoolApiCommandKind   pending_command;
    PoolApiCommandKind   last_command;
    PoolApiCommandResult last_command_result;
    uint32_t             last_client_request_id;

    /* Heartbeat plane. */
    PoolApiHeartbeatStatus heartbeat_status;
    uint32_t               heartbeat_commits;

    /* Conflict plane (last denied admission for an API mutation). */
    uint8_t api_conflict_code; /* PoolOperationHttpCode value; 0 == none */

    /* Bounded monotonic status sequence (saturating). */
    uint32_t status_sequence;
} PoolApiStatus;

/* ------------------------------------------------------------------ */
/* Stable machine tokens (dot-free; never carry identities or prose)   */
/* ------------------------------------------------------------------ */

const char *pool_api_command_kind_str(PoolApiCommandKind k);
const char *pool_api_actor_str(PoolApiActorClass a);
const char *pool_api_validation_str(PoolApiValidation v);
const char *pool_api_submit_str(PoolApiSubmitStatus s);
const char *pool_api_command_result_str(PoolApiCommandResult r);
const char *pool_api_heartbeat_str(PoolApiHeartbeatStatus h);
const char *pool_api_deadline_str(PoolApiDeadlineStatus d);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */

_Static_assert(POOL_API_ACTOR_UNKNOWN == 0,
               "UNKNOWN must be zero so a zeroed actor fails closed");
_Static_assert(POOL_API_CMD_NONE == 0,
               "NONE must be zero so a zeroed command slot is never processed");
_Static_assert(API_VALID_OK == 0,
               "OK must be zero so a zeroed validation result is explicit");
_Static_assert(API_SUBMIT_ACCEPTED == 0,
               "ACCEPTED must be zero for a deterministic default");
_Static_assert(API_CMD_RESULT_NONE == 0,
               "NONE must be zero so a zeroed result claims nothing");
_Static_assert(API_HB_NOT_APPLICABLE == 0,
               "NOT_APPLICABLE must be zero so a zeroed heartbeat writes nothing");
_Static_assert(API_DEADLINE_UNKNOWN == 0,
               "UNKNOWN must be zero so a zeroed deadline view claims nothing");
_Static_assert(POOL_API_CMD__COUNT == 4, "command count changed — review mailbox/tests");
_Static_assert(POOL_API_VALIDATION__COUNT == 25,
               "validation code count changed — review tokens/tests");
_Static_assert(POOL_API_CMD_RESULT__COUNT == 14,
               "command result count changed — review tokens/tests");
_Static_assert(POOL_API_HB__COUNT == 7, "heartbeat status count changed — review tokens/tests");
_Static_assert(POOL_API_COMMAND_QUEUE_DEPTH >= 2u && POOL_API_COMMAND_QUEUE_DEPTH <= 8u,
               "command queue depth must stay small and bounded");
_Static_assert(POOL_API_MAX_BODY_BYTES >= 512u && POOL_API_MAX_BODY_BYTES <= 1024u,
               "request body bound must stay strict");
/* The B1 duration contract is the ONLY duration authority. */
_Static_assert(POOL_SESSION_MIN_DURATION_S == 900u && POOL_SESSION_MAX_DURATION_S == 86400u,
               "B1 duration bounds changed — review the API contract and OpenAPI");
_Static_assert(POOL_API_TLS_DISABLED == 0u && POOL_API_TLS_BUNDLED == 1u,
               "TLS mode numbering must match the B7 representable modes");

#endif /* POOL_SESSION_API_TYPES_H_ */
