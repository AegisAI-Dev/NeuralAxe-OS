/*
 * NeuralAxe operation ownership — pure HTTP conflict mapping (Gate B5).
 * PURE: no HTTP server, no cJSON, no I/O. Conflict reporting only.
 */

#include <string.h>
#include "pool_operation_http_policy.h"

static PoolOperationHttpCode busy_code_for_owner(PoolOperationOwner o)
{
    switch (o) {
    case OP_OWNER_TIMED_SESSION:
    case OP_OWNER_BOOT_RECOVERY:
    case OP_OWNER_SESSION_ACKNOWLEDGE:
        return OP_HTTP_OPERATION_BUSY_TIMED_SESSION;
    case OP_OWNER_SOURCE_RESTORE:
        return OP_HTTP_OPERATION_BUSY_RESTORE;
    case OP_OWNER_MANUAL_POOL_PATCH:
        return OP_HTTP_OPERATION_BUSY_MANUAL_POOL_CHANGE;
    case OP_OWNER_OTA_UPDATE:
        return OP_HTTP_OPERATION_BUSY_OTA;
    case OP_OWNER_OPERATOR_RECOVERY:
    case OP_OWNER_RECOVERY_GUARD:
        return OP_HTTP_OPERATION_RECOVERY_LOCKED;
    default:
        return OP_HTTP_OPERATION_INVALID_REQUEST;
    }
}

void pool_operation_http_map(const PoolOperationDecision *decision,
                             const PoolOperationState *snapshot,
                             PoolOperationHttpConflict *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (decision == NULL) {
        out->http_status = 409;
        out->code = OP_HTTP_OPERATION_INVALID_REQUEST;
        out->retryable = false;
        return;
    }
    if (snapshot != NULL) {
        /* Two SAFE booleans only — never generations, IDs or identities. */
        out->restore_required = snapshot->restore_required;
        out->terminal_ack_required =
            snapshot->terminal_pending && !snapshot->restore_required;
    }
    if (decision->allowed) {
        out->http_status = 0;
        out->code = OP_HTTP_NONE;
        out->retryable = false;
        out->active_owner = OP_OWNER_NONE;
        return;
    }

    out->http_status = 409; /* every ownership conflict maps to 409 */
    out->active_owner = decision->blocking_owner; /* sanitized class only */

    switch (decision->status) {
    case OP_ERR_BOOTSTRAP_REQUIRED:
        out->code = OP_HTTP_OPERATION_BOOTSTRAP_REQUIRED;
        out->retryable = true; /* bootstrap completes shortly after boot */
        break;
    case OP_ERR_BUSY:
        out->code = busy_code_for_owner(decision->blocking_owner);
        out->retryable = true; /* the owning operation will finish */
        break;
    case OP_ERR_RECOVERY_LOCKED:
        out->code = OP_HTTP_OPERATION_RECOVERY_LOCKED;
        out->retryable = false; /* operator action is required */
        break;
    case OP_ERR_TERMINAL_ACK_REQUIRED:
        out->code = OP_HTTP_OPERATION_TERMINAL_ACK_REQUIRED;
        out->retryable = false; /* acknowledge the result first */
        break;
    case OP_ERR_RESTORE_REQUIRED:
        out->code = OP_HTTP_OPERATION_BUSY_RESTORE;
        out->retryable = true;
        break;
    case OP_ERR_PERSISTENCE_UNCERTAIN:
        out->code = OP_HTTP_OPERATION_PERSISTENCE_UNCERTAIN;
        out->retryable = true; /* a reload will learn the truth */
        break;
    case OP_ERR_NOT_OWNER:
    case OP_ERR_SESSION_MISMATCH:
        out->code = OP_HTTP_OPERATION_NOT_OWNER;
        out->retryable = false;
        break;
    case OP_ERR_STALE_LEASE:
        out->code = OP_HTTP_OPERATION_STALE_LEASE;
        out->retryable = false;
        break;
    case OP_ERR_NO_ACTIVE_SESSION:
        out->code = OP_HTTP_OPERATION_NO_ACTIVE_SESSION;
        out->retryable = false;
        break;
    case OP_ERR_UNSAFE_RELEASE:
        out->code = OP_HTTP_OPERATION_UNSAFE_RELEASE;
        out->retryable = false;
        break;
    default:
        out->code = OP_HTTP_OPERATION_INVALID_REQUEST;
        out->retryable = false;
        break;
    }
}

const char *pool_operation_http_code_str(PoolOperationHttpCode c)
{
    switch (c) {
    case OP_HTTP_NONE:                             return "OPERATION_ALLOWED";
    case OP_HTTP_OPERATION_BUSY_TIMED_SESSION:     return "OPERATION_BUSY_TIMED_SESSION";
    case OP_HTTP_OPERATION_BUSY_RESTORE:           return "OPERATION_BUSY_RESTORE";
    case OP_HTTP_OPERATION_BUSY_MANUAL_POOL_CHANGE:return "OPERATION_BUSY_MANUAL_POOL_CHANGE";
    case OP_HTTP_OPERATION_BUSY_OTA:               return "OPERATION_BUSY_OTA";
    case OP_HTTP_OPERATION_RECOVERY_LOCKED:        return "OPERATION_RECOVERY_LOCKED";
    case OP_HTTP_OPERATION_BOOTSTRAP_REQUIRED:     return "OPERATION_BOOTSTRAP_REQUIRED";
    case OP_HTTP_OPERATION_TERMINAL_ACK_REQUIRED:  return "OPERATION_TERMINAL_ACK_REQUIRED";
    case OP_HTTP_OPERATION_PERSISTENCE_UNCERTAIN:  return "OPERATION_PERSISTENCE_UNCERTAIN";
    case OP_HTTP_OPERATION_NOT_OWNER:              return "OPERATION_NOT_OWNER";
    case OP_HTTP_OPERATION_STALE_LEASE:            return "OPERATION_STALE_LEASE";
    case OP_HTTP_OPERATION_NO_ACTIVE_SESSION:      return "OPERATION_NO_ACTIVE_SESSION";
    case OP_HTTP_OPERATION_UNSAFE_RELEASE:         return "OPERATION_UNSAFE_RELEASE";
    case OP_HTTP_OPERATION_INVALID_REQUEST:        return "OPERATION_INVALID_REQUEST";
    default:                                       return "OPERATION_CODE_UNKNOWN";
    }
}
