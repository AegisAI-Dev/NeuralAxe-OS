#ifndef POOL_OPERATION_HTTP_POLICY_H_
#define POOL_OPERATION_HTTP_POLICY_H_

#include "pool_operation_types.h"

/*
 * NeuralAxe operation ownership — pure HTTP conflict policy surface
 * (Phase 2M.1B, Gate B5).
 *
 * A pure, future-facing mapper for the Gate B8 API layer. It is NOT wired
 * into any HTTP handler in B5: no esp_http_server dependency, no cJSON, no
 * response writes. Every ownership conflict for the future manual pool
 * PATCH / OTA / session surfaces maps to HTTP 409 Conflict.
 *
 * This mapping is conflict REPORTING for cooperating clients — it is not
 * authentication or access control; the LAN API trust posture is a
 * separate, documented residual.
 *
 * PRIVACY: the surface never exposes lease generations, session IDs,
 * record generations, pool hostnames, accounts/workers, wallets,
 * passwords, raw records, NVS keys, pointers or prose — only the sanitized
 * owner class, two safe booleans and a stable machine code.
 */

typedef enum {
    OP_HTTP_NONE = 0, /* the request was allowed — no conflict to report */
    OP_HTTP_OPERATION_BUSY_TIMED_SESSION,
    OP_HTTP_OPERATION_BUSY_RESTORE,
    OP_HTTP_OPERATION_BUSY_MANUAL_POOL_CHANGE,
    OP_HTTP_OPERATION_BUSY_OTA,
    OP_HTTP_OPERATION_RECOVERY_LOCKED,
    OP_HTTP_OPERATION_BOOTSTRAP_REQUIRED,
    OP_HTTP_OPERATION_TERMINAL_ACK_REQUIRED,
    OP_HTTP_OPERATION_PERSISTENCE_UNCERTAIN,
    OP_HTTP_OPERATION_NOT_OWNER,
    OP_HTTP_OPERATION_STALE_LEASE,
    OP_HTTP_OPERATION_NO_ACTIVE_SESSION,
    OP_HTTP_OPERATION_UNSAFE_RELEASE,
    OP_HTTP_OPERATION_INVALID_REQUEST,
    OP_HTTP__COUNT
} PoolOperationHttpCode;

typedef struct {
    uint16_t              http_status;   /* 409 on conflict; 0 when allowed  */
    PoolOperationHttpCode code;          /* stable machine code              */
    bool                  retryable;     /* may the client simply retry later */
    PoolOperationOwner    active_owner;  /* sanitized owner CLASS only       */
    bool                  restore_required;      /* safe boolean             */
    bool                  terminal_ack_required; /* safe boolean             */
} PoolOperationHttpConflict;

/*
 * Map a coordinator decision (+ a consistent state snapshot for the two
 * safe booleans) onto the future HTTP surface. Total: every denial maps to
 * 409 with a stable code; an allowed decision maps to status 0 / NONE.
 */
void pool_operation_http_map(const PoolOperationDecision *decision,
                             const PoolOperationState *snapshot,
                             PoolOperationHttpConflict *out);

/* Stable machine token for the HTTP code (dot-free; no data). */
const char *pool_operation_http_code_str(PoolOperationHttpCode c);

_Static_assert(OP_HTTP__COUNT == 14, "http code count changed — review mapping/tests");

#endif /* POOL_OPERATION_HTTP_POLICY_H_ */
