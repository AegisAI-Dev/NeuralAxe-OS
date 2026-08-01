/*
 * NeuralAxe timed pool sessions — API stable machine tokens
 * (Phase 2M.1B, Gate B8). See pool_session_api_types.h.
 *
 * Every token is a fixed, dot-free machine string. None of them carries a
 * hostname, account, worker, wallet, password, session id, generation,
 * lease token, raw record byte, NVS key or ESP-IDF error string, and none
 * is user-facing prose.
 */

#include "pool_session_api_types.h"

const char *pool_api_command_kind_str(PoolApiCommandKind k)
{
    switch (k) {
    case POOL_API_CMD_NONE:                return "NONE";
    case POOL_API_CMD_CREATE_SESSION:      return "CREATE_SESSION";
    case POOL_API_CMD_RESTORE_NOW:         return "RESTORE_NOW";
    case POOL_API_CMD_ACKNOWLEDGE_TERMINAL:return "ACKNOWLEDGE_TERMINAL";
    default:                               return "UNKNOWN";
    }
}

const char *pool_api_actor_str(PoolApiActorClass a)
{
    switch (a) {
    case POOL_API_ACTOR_UNKNOWN:    return "UNKNOWN";
    case POOL_API_ACTOR_LOCAL_HTTP: return "LOCAL_HTTP";
    case POOL_API_ACTOR_INTERNAL:   return "INTERNAL";
    default:                        return "UNKNOWN";
    }
}

const char *pool_api_validation_str(PoolApiValidation v)
{
    switch (v) {
    case API_VALID_OK:                    return "OK";
    case API_ERR_BODY_TOO_LARGE:          return "BODY_TOO_LARGE";
    case API_ERR_BODY_MALFORMED:          return "BODY_MALFORMED";
    case API_ERR_BODY_NOT_OBJECT:         return "BODY_NOT_OBJECT";
    case API_ERR_UNKNOWN_FIELD:           return "UNKNOWN_FIELD";
    case API_ERR_DUPLICATE_FIELD:         return "DUPLICATE_FIELD";
    case API_ERR_MISSING_FIELD:           return "MISSING_FIELD";
    case API_ERR_NULL_FIELD:              return "NULL_FIELD";
    case API_ERR_TYPE_MISMATCH:           return "TYPE_MISMATCH";
    case API_ERR_TOO_MANY_FIELDS:         return "TOO_MANY_FIELDS";
    case API_ERR_PASSWORD_FIELD_REJECTED: return "PASSWORD_FIELD_REJECTED";
    case API_ERR_SOURCE_FIELD_REJECTED:   return "SOURCE_FIELD_REJECTED";
    case API_ERR_SESSION_ID_REJECTED:     return "SESSION_ID_REJECTED";
    case API_ERR_INTERNAL_FIELD_REJECTED: return "INTERNAL_FIELD_REJECTED";
    case API_ERR_DURATION_OUT_OF_RANGE:   return "DURATION_OUT_OF_RANGE";
    case API_ERR_HOST_EMPTY:              return "HOST_EMPTY";
    case API_ERR_HOST_TOO_LONG:           return "HOST_TOO_LONG";
    case API_ERR_PORT_INVALID:            return "PORT_INVALID";
    case API_ERR_ACCOUNT_EMPTY:           return "ACCOUNT_EMPTY";
    case API_ERR_ACCOUNT_TOO_LONG:        return "ACCOUNT_TOO_LONG";
    case API_ERR_PROTOCOL_UNSUPPORTED:    return "PROTOCOL_UNSUPPORTED";
    case API_ERR_TLS_MODE_UNSUPPORTED:    return "TLS_MODE_UNSUPPORTED";
    case API_ERR_TLS_CUSTOM_REJECTED:     return "TLS_CUSTOM_REJECTED";
    case API_ERR_CHAIN_UNSUPPORTED:       return "CHAIN_UNSUPPORTED";
    case API_ERR_REQUEST_ID_INVALID:      return "REQUEST_ID_INVALID";
    default:                              return "UNKNOWN";
    }
}

const char *pool_api_submit_str(PoolApiSubmitStatus s)
{
    switch (s) {
    case API_SUBMIT_ACCEPTED:   return "ACCEPTED";
    case API_SUBMIT_QUEUE_FULL: return "QUEUE_FULL";
    case API_SUBMIT_NOT_READY:  return "NOT_READY";
    case API_SUBMIT_INVALID:    return "INVALID";
    default:                    return "UNKNOWN";
    }
}

const char *pool_api_command_result_str(PoolApiCommandResult r)
{
    switch (r) {
    case API_CMD_RESULT_NONE:                       return "NONE";
    case API_CMD_RESULT_PENDING:                    return "PENDING";
    case API_CMD_RESULT_ACCEPTED:                   return "ACCEPTED";
    case API_CMD_RESULT_REJECTED_STATE:             return "REJECTED_STATE";
    case API_CMD_RESULT_REJECTED_CONFLICT:          return "REJECTED_CONFLICT";
    case API_CMD_RESULT_REJECTED_HARDWARE:          return "REJECTED_HARDWARE";
    case API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED:return "REJECTED_SOURCE_UNSUPPORTED";
    case API_CMD_RESULT_REJECTED_TARGET_UNSUPPORTED:return "REJECTED_TARGET_UNSUPPORTED";
    case API_CMD_RESULT_REJECTED_VALIDATION:        return "REJECTED_VALIDATION";
    case API_CMD_RESULT_FAILED_PERSIST:             return "FAILED_PERSIST";
    case API_CMD_RESULT_FAILED_READBACK:            return "FAILED_READBACK";
    case API_CMD_RESULT_FAILED_OWNERSHIP:           return "FAILED_OWNERSHIP";
    case API_CMD_RESULT_RECOVERY_GUARD:             return "RECOVERY_GUARD";
    case API_CMD_RESULT_ADOPTION_FAILED:            return "ADOPTION_FAILED";
    default:                                        return "UNKNOWN";
    }
}

const char *pool_api_heartbeat_str(PoolApiHeartbeatStatus h)
{
    switch (h) {
    case API_HB_NOT_APPLICABLE: return "NOT_APPLICABLE";
    case API_HB_WAITING:        return "WAITING";
    case API_HB_DUE:            return "DUE";
    case API_HB_COMMITTED:      return "COMMITTED";
    case API_HB_TIME_UNTRUSTED: return "TIME_UNTRUSTED";
    case API_HB_PERSIST_FAILED: return "PERSIST_FAILED";
    case API_HB_RECOVERY_GUARD: return "RECOVERY_GUARD";
    default:                    return "UNKNOWN";
    }
}

const char *pool_api_deadline_str(PoolApiDeadlineStatus d)
{
    switch (d) {
    case API_DEADLINE_UNKNOWN:         return "UNKNOWN";
    case API_DEADLINE_ACTIVE:          return "ACTIVE";
    case API_DEADLINE_EXPIRED:         return "EXPIRED";
    case API_DEADLINE_RESTORE_PENDING: return "RESTORE_PENDING";
    default:                           return "UNKNOWN";
    }
}
