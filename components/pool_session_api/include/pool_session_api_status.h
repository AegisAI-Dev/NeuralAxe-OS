#ifndef POOL_SESSION_API_STATUS_H_
#define POOL_SESSION_API_STATUS_H_

#include "pool_session_api_types.h"
#include "pool_session_api_heartbeat.h"

/*
 * NeuralAxe timed pool sessions — PURE sanitized status builder
 * (Phase 2M.1B, Gate B8).
 *
 * No ESP-IDF, no HTTP, no NVS, no store, no lease, no clock, no heap, no
 * globals. It never calls an external operation: it consumes an already
 * collected bounded snapshot and emits the bounded public model.
 *
 * PRIVACY BY CONSTRUCTION. The INPUT model below carries only fixed-width
 * scalars and booleans — no record pointer, no identity struct and no
 * string — so a hostname, port, account, worker, wallet, password, profile,
 * session id, lease token, lease generation, record generation, raw record
 * or raw NVS byte cannot reach the builder at all, let alone its output.
 *
 * FAIL-CLOSED. Any out-of-range enum in the input is treated as an internal
 * inconsistency: the output takes the conservative value, protocol start is
 * reported as held, the mining grant is reported absent and operator
 * recovery is reported as required.
 */

typedef struct {
    /* Feature posture. */
    bool api_enabled;
    bool runtime_initialized;
    bool execution_enabled;

    /* Durable (B3/B1) facts — states and flags only. */
    bool     session_present;
    uint8_t  durable_state;   /* PoolSessionState value  */
    uint16_t durable_failure; /* PoolSessionError value  */
    bool     durable_restore_required;

    /* B6 runtime posture. */
    uint8_t runtime_state; /* PoolRuntimeState value */
    bool    protocol_start_permitted;

    /* B7 execution posture (zeroed when the execution flag is off). */
    uint8_t execution_state;  /* PoolExecState value        */
    uint8_t execution_reason; /* PoolExecReason value       */
    uint8_t asic_gate;        /* PoolExecGatePosture value  */
    bool    mining_grant_active;

    /* B5 ownership (sanitized CLASS and phase only). */
    uint8_t lease_owner; /* PoolOperationOwner value      */
    uint8_t lease_phase; /* PoolOperationLeasePhase value */
    bool    terminal_pending;

    /* Trusted time and the bounded remaining-duration verdict. */
    bool     trusted_time_required;
    bool     trusted_time_available;
    bool     remaining_valid;
    uint64_t remaining_s;

    /* Command plane. */
    bool                 command_pending;
    PoolApiCommandKind   pending_command;
    PoolApiCommandKind   last_command;
    PoolApiCommandResult last_command_result;
    uint32_t             last_client_request_id;

    /* Heartbeat plane. */
    PoolApiHeartbeatStatus heartbeat_status;
    uint32_t               heartbeat_commits;

    /* Last denied API admission (PoolOperationHttpCode value; 0 = none). */
    uint8_t api_conflict_code;

    /* Bounded monotonic status sequence. */
    uint32_t status_sequence;
} PoolApiStatusInput;

/* Zero a status to the fail-closed "nothing is known" public view. */
void pool_api_status_init(PoolApiStatus *out);

/*
 * THE pure status build. Total, deterministic, input-immutable; *out is
 * fully written on every path. A NULL input yields the fail-closed view.
 */
void pool_api_status_build(const PoolApiStatusInput *in, PoolApiStatus *out);

/*
 * Structural consistency of a published status: model version, every enum
 * in range, and the two safety couplings (a mining grant is impossible
 * without an OPEN_TARGET ASIC gate; a grant is impossible while operator
 * recovery is required).
 */
bool pool_api_status_valid(const PoolApiStatus *s);

/* The bounded deadline verdict, exposed for tests. Total. */
PoolApiDeadlineStatus pool_api_deadline_for(bool session_present,
                                            uint8_t durable_state,
                                            bool restore_required,
                                            bool remaining_valid,
                                            uint64_t remaining_s);

#endif /* POOL_SESSION_API_STATUS_H_ */
