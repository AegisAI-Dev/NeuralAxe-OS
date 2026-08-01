/*
 * NeuralAxe timed pool sessions — PURE sanitized status builder
 * (Phase 2M.1B, Gate B8). See pool_session_api_status.h.
 */

#include <string.h>
#include "pool_session_api_status.h"
#include "pool_session_runtime_core.h"
#include "pool_session_execution_core.h"
#include "pool_operation_types.h"
#include "pool_operation_http_policy.h"

static bool in_range(unsigned v, unsigned count)
{
    return v < count;
}

void pool_api_status_init(PoolApiStatus *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->model_version   = POOL_API_MODEL_VERSION;
    out->durable_state   = POOL_STATE_IDLE;
    out->runtime_state   = (uint8_t)RUNTIME_UNINITIALIZED;
    out->execution_state = (uint8_t)EXEC_STATE_DISABLED;
    out->lease_owner     = (uint8_t)OP_OWNER_NONE;
    out->lease_phase     = (uint8_t)OP_PHASE_UNBOOTSTRAPPED;
    out->asic_gate       = (uint8_t)EXEC_GATE_DEFAULT_OPEN;
    out->deadline_status = API_DEADLINE_UNKNOWN;
    out->heartbeat_status = API_HB_NOT_APPLICABLE;
    out->pending_command  = POOL_API_CMD_NONE;
    out->last_command     = POOL_API_CMD_NONE;
    out->last_command_result = API_CMD_RESULT_NONE;
    /* protocol_start_permitted stays false: held until proven otherwise. */
}

PoolApiDeadlineStatus pool_api_deadline_for(bool session_present,
                                            uint8_t durable_state,
                                            bool restore_required,
                                            bool remaining_valid,
                                            uint64_t remaining_s)
{
    PoolSessionState st;

    if (!session_present || !in_range(durable_state, POOL_STATE__COUNT)) {
        return API_DEADLINE_UNKNOWN;
    }
    st = (PoolSessionState)durable_state;

    /* A restore-side state, or any state that still owes a restoration,
     * reports RESTORE_PENDING regardless of a remaining-time verdict: the
     * deadline is no longer the operative fact. */
    if (pool_state_is_restore_side(st) ||
        (restore_required && st != POOL_STATE_TARGET_ACTIVE)) {
        return API_DEADLINE_RESTORE_PENDING;
    }
    if (st != POOL_STATE_TARGET_ACTIVE) {
        return API_DEADLINE_UNKNOWN;
    }
    if (!remaining_valid) {
        return API_DEADLINE_UNKNOWN; /* untrusted time: never guess */
    }
    return (remaining_s == 0u) ? API_DEADLINE_EXPIRED : API_DEADLINE_ACTIVE;
}

void pool_api_status_build(const PoolApiStatusInput *in, PoolApiStatus *out)
{
    PoolApiStatus s;
    bool          inconsistent = false;

    pool_api_status_init(&s);
    if (in == NULL) {
        s.operator_recovery_required = true;
        if (out != NULL) {
            *out = s;
        }
        return;
    }

    s.api_enabled         = in->api_enabled;
    s.runtime_initialized = in->runtime_initialized;
    s.execution_enabled   = in->execution_enabled;

    /* ---- Durable session facts (fail closed on an unknown state) ---- */
    s.session_present  = in->session_present;
    s.restore_required = in->durable_restore_required;
    if (in_range(in->durable_state, POOL_STATE__COUNT)) {
        s.durable_state = (PoolSessionState)in->durable_state;
    } else {
        s.durable_state = POOL_STATE_RECOVERY_REQUIRED;
        inconsistent    = true;
    }
    s.durable_failure = (in->durable_failure < (uint16_t)POOL_SESSION_ERR__COUNT)
                            ? in->durable_failure
                            : (uint16_t)ERR_RECOVERY_REQUIRED;
    s.terminal_result_pending = in->terminal_pending;

    /* ---- Runtime posture ---- */
    if (in_range(in->runtime_state, POOL_RUNTIME_STATE__COUNT)) {
        s.runtime_state = in->runtime_state;
    } else {
        s.runtime_state = (uint8_t)RUNTIME_ERROR;
        inconsistent    = true;
    }
    s.protocol_start_permitted = in->protocol_start_permitted;

    /* ---- Execution posture ---- */
    if (in_range(in->execution_state, POOL_EXEC_STATE__COUNT)) {
        s.execution_state = in->execution_state;
    } else {
        s.execution_state = (uint8_t)EXEC_STATE_ERROR;
        inconsistent      = true;
    }
    s.execution_reason = (in->execution_reason < (uint8_t)POOL_EXEC_REASON__COUNT)
                             ? in->execution_reason
                             : (uint8_t)EXEC_REASON_NONE;
    if (in_range(in->asic_gate, POOL_EXEC_GATE__COUNT)) {
        s.asic_gate = in->asic_gate;
    } else {
        s.asic_gate  = (uint8_t)EXEC_GATE_INHIBITED;
        inconsistent = true;
    }
    /* A grant is only reportable while the ASIC gate is genuinely open for
     * the target. Any other gate posture reports no grant. */
    s.target_mining_grant_active =
        in->mining_grant_active && (s.asic_gate == (uint8_t)EXEC_GATE_OPEN_TARGET);

    /* ---- Ownership (sanitized class + phase only) ---- */
    if (in_range(in->lease_owner, OP_OWNER__COUNT)) {
        s.lease_owner = in->lease_owner;
    } else {
        s.lease_owner = (uint8_t)OP_OWNER_RECOVERY_GUARD;
        inconsistent  = true;
    }
    if (in_range(in->lease_phase, OP_PHASE__COUNT)) {
        s.lease_phase = in->lease_phase;
    } else {
        s.lease_phase = (uint8_t)OP_PHASE_RECOVERY_GUARD;
        inconsistent  = true;
    }

    /* ---- Operator recovery ---- */
    s.operator_recovery_required =
        inconsistent ||
        s.lease_owner == (uint8_t)OP_OWNER_RECOVERY_GUARD ||
        s.lease_owner == (uint8_t)OP_OWNER_OPERATOR_RECOVERY ||
        s.lease_phase == (uint8_t)OP_PHASE_RECOVERY_GUARD ||
        s.runtime_state == (uint8_t)RUNTIME_RECOVERY_GUARD ||
        s.runtime_state == (uint8_t)RUNTIME_OPERATOR_RECOVERY ||
        s.runtime_state == (uint8_t)RUNTIME_ERROR ||
        s.execution_state == (uint8_t)EXEC_STATE_RECOVERY_GUARD ||
        s.execution_state == (uint8_t)EXEC_STATE_ERROR ||
        s.durable_state == POOL_STATE_RECOVERY_REQUIRED;
    if (s.operator_recovery_required) {
        /* Never report a live mining grant while an operator is required. */
        s.target_mining_grant_active = false;
    }
    if (inconsistent) {
        s.protocol_start_permitted = false;
    }

    /* ---- Trusted time and the bounded deadline view ---- */
    s.trusted_time_required  = in->trusted_time_required;
    s.trusted_time_available = in->trusted_time_available;
    s.deadline_status = pool_api_deadline_for(s.session_present, s.durable_state,
                                              s.restore_required,
                                              in->remaining_valid, in->remaining_s);
    if (s.deadline_status == API_DEADLINE_ACTIVE) {
        s.remaining_seconds_valid = true;
        s.remaining_seconds       = (in->remaining_s > (uint64_t)0xFFFFFFFFu)
                                        ? 0xFFFFFFFFu
                                        : (uint32_t)in->remaining_s;
    }

    /* ---- Command plane ---- */
    s.command_pending = in->command_pending;
    s.pending_command = in_range((unsigned)in->pending_command, POOL_API_CMD__COUNT)
                            ? in->pending_command
                            : POOL_API_CMD_NONE;
    s.last_command    = in_range((unsigned)in->last_command, POOL_API_CMD__COUNT)
                            ? in->last_command
                            : POOL_API_CMD_NONE;
    s.last_command_result =
        in_range((unsigned)in->last_command_result, POOL_API_CMD_RESULT__COUNT)
            ? in->last_command_result
            : API_CMD_RESULT_NONE;
    s.last_client_request_id = in->last_client_request_id;

    /* ---- Heartbeat plane ---- */
    s.heartbeat_status = in_range((unsigned)in->heartbeat_status, POOL_API_HB__COUNT)
                             ? in->heartbeat_status
                             : API_HB_NOT_APPLICABLE;
    s.heartbeat_commits = in->heartbeat_commits;

    /* ---- Conflict plane ---- */
    s.api_conflict_code = in_range(in->api_conflict_code, OP_HTTP__COUNT)
                              ? in->api_conflict_code
                              : (uint8_t)OP_HTTP_OPERATION_INVALID_REQUEST;

    s.status_sequence = in->status_sequence;

    if (out != NULL) {
        *out = s;
    }
}

bool pool_api_status_valid(const PoolApiStatus *s)
{
    if (s == NULL || s->model_version != POOL_API_MODEL_VERSION) {
        return false;
    }
    if (!in_range((unsigned)s->durable_state, POOL_STATE__COUNT)) return false;
    if (s->durable_failure >= (uint16_t)POOL_SESSION_ERR__COUNT) return false;
    if (!in_range(s->runtime_state, POOL_RUNTIME_STATE__COUNT)) return false;
    if (!in_range(s->execution_state, POOL_EXEC_STATE__COUNT)) return false;
    if (!in_range(s->execution_reason, POOL_EXEC_REASON__COUNT)) return false;
    if (!in_range(s->asic_gate, POOL_EXEC_GATE__COUNT)) return false;
    if (!in_range(s->lease_owner, OP_OWNER__COUNT)) return false;
    if (!in_range(s->lease_phase, OP_PHASE__COUNT)) return false;
    if (!in_range((unsigned)s->deadline_status, POOL_API_DEADLINE__COUNT)) return false;
    if (!in_range((unsigned)s->pending_command, POOL_API_CMD__COUNT)) return false;
    if (!in_range((unsigned)s->last_command, POOL_API_CMD__COUNT)) return false;
    if (!in_range((unsigned)s->last_command_result, POOL_API_CMD_RESULT__COUNT)) return false;
    if (!in_range((unsigned)s->heartbeat_status, POOL_API_HB__COUNT)) return false;
    if (!in_range(s->api_conflict_code, OP_HTTP__COUNT)) return false;

    /* Safety couplings. */
    if (s->target_mining_grant_active &&
        s->asic_gate != (uint8_t)EXEC_GATE_OPEN_TARGET) {
        return false;
    }
    if (s->target_mining_grant_active && s->operator_recovery_required) {
        return false;
    }
    if (s->remaining_seconds_valid && s->deadline_status != API_DEADLINE_ACTIVE) {
        return false;
    }
    return true;
}
