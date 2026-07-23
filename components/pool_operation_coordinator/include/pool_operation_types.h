#ifndef POOL_OPERATION_TYPES_H_
#define POOL_OPERATION_TYPES_H_

#include <stdint.h>
#include <stdbool.h>
#include "pool_session.h"
#include "pool_session_record.h"
#include "pool_session_store.h"
#include "pool_session_recovery.h"

/*
 * NeuralAxe timed pool sessions — operation ownership types (Phase 2M.1B,
 * Gate B5). Board 601 / BM1370 only.
 *
 * One exclusive firmware-owned operation lease coordinates every path that
 * could mutate pool configuration, Stratum, the session store, OTA flash or
 * restart. Gate B5 is COORDINATION AND POLICY ONLY: nothing here executes a
 * pool change, Stratum call, NVS write, OTA, restart or HTTP response, and
 * no production runtime path acquires the coordinator in B5.
 *
 * TRUST BOUNDARY (honest wording — B5 Stage 19):
 *  - The coordinator prevents ACCIDENTAL and CONCURRENT operation conflicts
 *    among cooperating firmware paths. It is NOT a security sandbox.
 *  - Lease tokens are stale-reference guards and internal consistency
 *    tokens — NOT cryptographically unforgeable capabilities. Arbitrary
 *    firmware code could bypass the coordinator or fabricate calls.
 *  - The HTTP 409 mapping is conflict reporting, not authentication. The
 *    unauthenticated LAN API / CORS posture and the B3 plaintext
 *    account-at-rest + offline-flash residuals remain separate, documented
 *    residuals (Phase 2M.1A security report).
 *  - Because B5 has no runtime call sites, this is a coordination
 *    FOUNDATION, not yet active global enforcement — enforcement lands
 *    when B6+ routes the audited mutation paths through the coordinator.
 */

/* ------------------------------------------------------------------ */
/* Operation requests                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    OP_REQUEST_READ_ONLY = 0,          /* status/GET-class work; never a lease */
    OP_REQUEST_TIMED_SESSION_START,    /* create a new timed session           */
    OP_REQUEST_TIMED_SESSION_INTERNAL, /* scheduler work under the session lease */
    OP_REQUEST_RESTORE_NOW,            /* control request to the session owner */
    OP_REQUEST_SESSION_ACKNOWLEDGE,    /* ack terminal result (future tombstone) */
    OP_REQUEST_MANUAL_POOL_PATCH,      /* HTTP PATCH pool-identity fields      */
    OP_REQUEST_OTA_UPDATE,             /* firmware/www OTA                     */
    OP_REQUEST_MANUAL_DEVICE_RESTART,  /* POST /restart and BAP restarts       */
    OP_REQUEST_DESTRUCTIVE_MAINTENANCE,/* reserved; always denied in B5        */
    OP_REQUEST_OPERATOR_RECOVERY,      /* admitted only via the recovery guard */
    OP_REQUEST_PROTOCOL_RECONCILE,     /* coordinator failover housekeeping    */
    OP_REQUEST__COUNT
} PoolOperationRequestKind;

/* ------------------------------------------------------------------ */
/* Owners                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    OP_OWNER_NONE = 0,
    OP_OWNER_TIMED_SESSION,
    OP_OWNER_BOOT_RECOVERY,
    OP_OWNER_SOURCE_RESTORE,
    OP_OWNER_MANUAL_POOL_PATCH,
    OP_OWNER_OTA_UPDATE,
    OP_OWNER_SESSION_ACKNOWLEDGE,
    OP_OWNER_OPERATOR_RECOVERY,
    OP_OWNER_RECOVERY_GUARD,
    OP_OWNER__COUNT
} PoolOperationOwner;

/* ------------------------------------------------------------------ */
/* Lease phases                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    OP_PHASE_UNBOOTSTRAPPED = 0,           /* fail-closed: all mutation denied */
    OP_PHASE_FREE,                         /* no owner                          */
    OP_PHASE_RESERVED_PENDING_PERSISTENCE, /* owned; external mutation blocked  */
    OP_PHASE_ACTIVE,                       /* owned; still no auto-authorization */
    OP_PHASE_WAITING_FOR_TRUSTED_TIME,     /* pool/Stratum held; target inhibited */
    OP_PHASE_VERIFYING_TARGET,             /* live verification pending; NO mining */
    OP_PHASE_RESTORING_SOURCE,             /* same exclusive session ownership   */
    OP_PHASE_TERMINAL_ACK_PENDING,         /* durable terminal proof applied     */
    OP_PHASE_RELEASING,                    /* reserved; releases are atomic      */
    OP_PHASE_RECOVERY_GUARD,               /* normal mutation denied             */
    OP_PHASE__COUNT
} PoolOperationLeasePhase;

/* ------------------------------------------------------------------ */
/* Resource scopes (documentation/validation mask; leases stay globally */
/* exclusive in B5 regardless of mask)                                  */
/* ------------------------------------------------------------------ */

#define OP_SCOPE_SESSION_STORE           (1u << 0)
#define OP_SCOPE_POOL_CONFIGURATION      (1u << 1)
#define OP_SCOPE_STRATUM_CONTROL         (1u << 2)
#define OP_SCOPE_DEVICE_RESTART          (1u << 3)
#define OP_SCOPE_OTA_FLASH               (1u << 4)
#define OP_SCOPE_DESTRUCTIVE_MAINTENANCE (1u << 5)
#define OP_SCOPE__ALL_VALID              0x3Fu

/* ------------------------------------------------------------------ */
/* Stable machine status codes                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    OP_OK = 0,
    OP_ERR_INVALID_ARGUMENT,
    OP_ERR_NOT_INITIALIZED,
    OP_ERR_BOOTSTRAP_REQUIRED,
    OP_ERR_ALREADY_BOOTSTRAPPED,
    OP_ERR_BUSY,
    OP_ERR_RECOVERY_LOCKED,
    OP_ERR_TERMINAL_ACK_REQUIRED,
    OP_ERR_NO_ACTIVE_SESSION,
    OP_ERR_SESSION_MISMATCH,
    OP_ERR_NOT_OWNER,
    OP_ERR_STALE_LEASE,
    OP_ERR_INVALID_TRANSITION,
    OP_ERR_PERSISTENCE_REQUIRED,
    OP_ERR_PERSISTENCE_MISMATCH,
    OP_ERR_PERSISTENCE_UNCERTAIN,
    OP_ERR_UNSAFE_RELEASE,
    OP_ERR_RESTORE_REQUIRED,
    OP_ERR_GENERATION_EXHAUSTED,
    OP_ERR_UNSUPPORTED_REQUEST,
    OP_ERR_INTERNAL_CONSISTENCY,
    POOL_OPERATION_STATUS__COUNT
} PoolOperationStatus;

/* ------------------------------------------------------------------ */
/* Lease token and state                                               */
/* ------------------------------------------------------------------ */

/*
 * Stale-reference guard. Generation 0 is reserved invalid. The token binds
 * an owner class to one lease generation; any transition or release bumps
 * the generation, making prior tokens stale. It carries no secrets and is
 * NOT a cryptographic capability.
 */
typedef struct {
    bool               valid;
    PoolOperationOwner owner;
    uint32_t           lease_generation; /* 0 = invalid */
} PoolOperationLeaseToken;

/*
 * The coordinator's bounded state. No password, hostname, account, worker,
 * wallet, record bytes, pointers, task handles, callbacks, timestamps or
 * auto-expiry deadlines — leases never expire with time (a lost owner is a
 * diagnostic problem for later gates, never a silent obligation discharge).
 * This struct is NOT persisted: durable timed-session ownership is
 * reconstructed from the committed B3 record + B4 plan at bootstrap.
 */
typedef struct {
    bool                    bootstrapped;
    PoolOperationOwner      owner;
    PoolOperationLeasePhase phase;
    uint32_t                lease_generation;     /* current; 0 = never leased */
    bool                    durable_claim;        /* reconstructed from B3/B4  */
    uint32_t                bound_session_id;     /* 0 = none                  */
    uint32_t                bound_record_generation;
    uint32_t                resource_scopes;      /* OP_SCOPE_* mask           */
    bool                    persistence_required_before_action;
    bool                    terminal_pending;     /* retained terminal result  */
    bool                    restore_required;     /* durable obligation echo   */
    PoolOperationStatus     last_status;
} PoolOperationState;

/* ------------------------------------------------------------------ */
/* Requests and decisions                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    PoolOperationRequestKind kind;
    uint32_t                 session_id; /* RESTORE_NOW / INTERNAL / ACK match */
    PoolOperationLeaseToken  token;      /* INTERNAL work only; else ignored   */
} PoolOperationRequest;

typedef struct {
    bool                allowed;
    PoolOperationStatus status;         /* OP_OK or the denial code            */
    PoolOperationOwner  blocking_owner; /* sanitized owner class when denied   */
    bool                http_conflict;  /* future HTTP surface should 409      */
} PoolOperationDecision;

/* ------------------------------------------------------------------ */
/* Persistence proofs and manual outcomes                              */
/* ------------------------------------------------------------------ */

/* Durable evidence kinds consumed by activation/transition/release. B5
 * never writes these proofs — the future runtime produces them from real
 * B3 store results. No raw record bytes, no pool identity. */
typedef enum {
    OP_PROOF_SESSION_COMMITTED = 1,  /* snapshot/record committed; activates  */
    OP_PROOF_RECOVERY_UPDATE_COMMITTED, /* B4 proposal persisted              */
    OP_PROOF_TERMINAL_COMMITTED,     /* COMPLETE / pre-mutation CANCELLED      */
    OP_PROOF_CLEARED,                /* committed tombstone (STORE_CLEARED)    */
    OP_PROOF__COUNT
} PoolOperationProofKind;

typedef struct {
    PoolOperationProofKind kind;
    PoolStoreResult        store_result;
    uint32_t               committed_record_generation;
    uint32_t               session_id;
    PoolSessionState       persisted_state;
    bool                   restore_required;
} PoolOperationPersistenceProof;

/* Manual (pool PATCH / OTA) completion evidence. */
typedef enum {
    OP_OUTCOME_NO_MUTATION_OCCURRED = 1,
    OP_OUTCOME_VERIFIED_COMPLETE,
    OP_OUTCOME_MUTATION_UNCERTAIN, /* transitions to the recovery guard */
    OP_OUTCOME__COUNT
} PoolOperationOutcome;

/* ------------------------------------------------------------------ */
/* Pure request-property helpers (total; invalid values fail closed)   */
/* ------------------------------------------------------------------ */

bool pool_operation_request_is_read_only(PoolOperationRequestKind k);
bool pool_operation_request_is_mutating(PoolOperationRequestKind k);
bool pool_operation_request_requires_exclusive_lease(PoolOperationRequestKind k);
bool pool_operation_request_requires_session_context(PoolOperationRequestKind k);
bool pool_operation_request_requires_terminal_record(PoolOperationRequestKind k);
/* Documented future call-site requirement mask for the request kind. */
uint32_t pool_operation_request_scope(PoolOperationRequestKind k);
/* Safe while a terminal result is retained (restore_required=false)? */
bool pool_operation_request_safe_with_terminal(PoolOperationRequestKind k);

/* Stable machine tokens (dot-free; never carry identities or prose). */
const char *pool_operation_status_str(PoolOperationStatus s);
const char *pool_operation_owner_str(PoolOperationOwner o);
const char *pool_operation_phase_str(PoolOperationLeasePhase p);
const char *pool_operation_request_str(PoolOperationRequestKind k);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */
_Static_assert(OP_REQUEST__COUNT == 11, "request count changed — review the conflict matrix");
_Static_assert(OP_OWNER__COUNT == 9, "owner count changed — review policy/tests");
_Static_assert(OP_PHASE__COUNT == 10, "phase count changed — review policy/tests");
_Static_assert(POOL_OPERATION_STATUS__COUNT == 21, "status count changed — review tokens/tests");
_Static_assert(OP_SCOPE__ALL_VALID == 0x3Fu, "scope mask changed — review validation");

#endif /* POOL_OPERATION_TYPES_H_ */
