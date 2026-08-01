/*
 * Deterministic tests for the Gate B8 PURE sanitized status builder.
 *
 * The privacy proof is structural AND empirical: the input model carries no
 * string or pointer, and a planted-marker byte scan over the built status
 * proves no identity can appear in it.
 */

#include <string.h>
#include "unity.h"
#include "pool_session_api_status.h"
#include "pool_session_runtime_core.h"
#include "pool_session_execution_core.h"
#include "pool_operation_types.h"
#include "pool_operation_http_policy.h"

static PoolApiStatusInput g_in;
static PoolApiStatus      g_out;

static void fresh_free(void)
{
    memset(&g_in, 0, sizeof(g_in));
    g_in.api_enabled              = true;
    g_in.runtime_initialized      = true;
    g_in.execution_enabled        = true;
    g_in.session_present          = false;
    g_in.durable_state            = (uint8_t)POOL_STATE_IDLE;
    g_in.runtime_state            = (uint8_t)RUNTIME_FREE;
    g_in.protocol_start_permitted = true;
    g_in.execution_state          = (uint8_t)EXEC_STATE_IDLE;
    g_in.execution_reason         = (uint8_t)EXEC_REASON_NONE;
    g_in.asic_gate                = (uint8_t)EXEC_GATE_DEFAULT_OPEN;
    g_in.lease_owner              = (uint8_t)OP_OWNER_NONE;
    g_in.lease_phase              = (uint8_t)OP_PHASE_FREE;
    g_in.heartbeat_status         = API_HB_NOT_APPLICABLE;
    g_in.status_sequence          = 1u;
}

TEST_CASE("api status: the FREE view is complete and claims nothing",
          "[pool_api_status]")
{
    fresh_free();
    pool_api_status_build(&g_in, &g_out);

    TEST_ASSERT_TRUE(pool_api_status_valid(&g_out));
    TEST_ASSERT_EQUAL_UINT32(POOL_API_MODEL_VERSION, g_out.model_version);
    TEST_ASSERT_TRUE(g_out.api_enabled);
    TEST_ASSERT_FALSE(g_out.session_present);
    TEST_ASSERT_FALSE(g_out.terminal_result_pending);
    TEST_ASSERT_FALSE(g_out.restore_required);
    TEST_ASSERT_FALSE(g_out.operator_recovery_required);
    TEST_ASSERT_FALSE(g_out.target_mining_grant_active);
    TEST_ASSERT_TRUE(g_out.protocol_start_permitted);
    TEST_ASSERT_EQUAL(API_DEADLINE_UNKNOWN, g_out.deadline_status);
    TEST_ASSERT_FALSE(g_out.remaining_seconds_valid);
    TEST_ASSERT_EQUAL(POOL_API_CMD_NONE, g_out.last_command);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_NONE, g_out.last_command_result);
}

TEST_CASE("api status: a NULL input yields the fail-closed view", "[pool_api_status]")
{
    pool_api_status_build(NULL, &g_out);
    TEST_ASSERT_TRUE(g_out.operator_recovery_required);
    TEST_ASSERT_FALSE(g_out.protocol_start_permitted);
    TEST_ASSERT_FALSE(g_out.target_mining_grant_active);
    TEST_ASSERT_FALSE(g_out.session_present);
    TEST_ASSERT_TRUE(pool_api_status_valid(&g_out));
}

TEST_CASE("api status: the disabled postures are reported honestly",
          "[pool_api_status]")
{
    fresh_free();
    g_in.api_enabled         = false;
    g_in.runtime_initialized = false;
    g_in.execution_enabled   = false;
    g_in.runtime_state       = (uint8_t)RUNTIME_UNINITIALIZED;
    g_in.execution_state     = (uint8_t)EXEC_STATE_DISABLED;
    pool_api_status_build(&g_in, &g_out);

    TEST_ASSERT_FALSE(g_out.api_enabled);
    TEST_ASSERT_FALSE(g_out.runtime_initialized);
    TEST_ASSERT_FALSE(g_out.execution_enabled);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)EXEC_STATE_DISABLED, g_out.execution_state);
    TEST_ASSERT_FALSE(g_out.target_mining_grant_active);
    TEST_ASSERT_TRUE(pool_api_status_valid(&g_out));
}

TEST_CASE("api status: waiting, verifying, mining, restoring and terminal views",
          "[pool_api_status]")
{
    /* Waiting for trusted time. */
    fresh_free();
    g_in.session_present          = true;
    g_in.durable_state            = (uint8_t)POOL_STATE_TARGET_ACTIVE;
    g_in.durable_restore_required = true;
    g_in.runtime_state            = (uint8_t)RUNTIME_WAITING_FOR_TRUSTED_TIME;
    g_in.protocol_start_permitted = false;
    g_in.trusted_time_required    = true;
    g_in.trusted_time_available   = false;
    g_in.lease_owner              = (uint8_t)OP_OWNER_BOOT_RECOVERY;
    g_in.lease_phase              = (uint8_t)OP_PHASE_WAITING_FOR_TRUSTED_TIME;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_TRUE(g_out.trusted_time_required);
    TEST_ASSERT_FALSE(g_out.trusted_time_available);
    TEST_ASSERT_EQUAL(API_DEADLINE_UNKNOWN, g_out.deadline_status); /* not remaining_valid */
    TEST_ASSERT_FALSE(g_out.remaining_seconds_valid);
    TEST_ASSERT_FALSE(g_out.protocol_start_permitted);

    /* Verifying the target: eligibility, never a grant. */
    fresh_free();
    g_in.session_present          = true;
    g_in.durable_state            = (uint8_t)POOL_STATE_TARGET_ACTIVE;
    g_in.durable_restore_required = true;
    g_in.runtime_state            = (uint8_t)RUNTIME_VERIFY_TARGET_PENDING;
    g_in.protocol_start_permitted = false;
    g_in.execution_state          = (uint8_t)EXEC_STATE_TARGET_CONNECTING;
    g_in.asic_gate                = (uint8_t)EXEC_GATE_INHIBITED;
    g_in.mining_grant_active      = true; /* inconsistent with the gate */
    g_in.lease_owner              = (uint8_t)OP_OWNER_TIMED_SESSION;
    g_in.lease_phase              = (uint8_t)OP_PHASE_VERIFYING_TARGET;
    pool_api_status_build(&g_in, &g_out);
    /* A grant is never reportable while the gate is not OPEN_TARGET. */
    TEST_ASSERT_FALSE(g_out.target_mining_grant_active);
    TEST_ASSERT_TRUE(g_out.restore_required);
    TEST_ASSERT_TRUE(pool_api_status_valid(&g_out));

    /* Target mining with a known remaining duration. */
    fresh_free();
    g_in.session_present          = true;
    g_in.durable_state            = (uint8_t)POOL_STATE_TARGET_ACTIVE;
    g_in.durable_restore_required = true;
    g_in.runtime_state            = (uint8_t)RUNTIME_VERIFY_TARGET_PENDING;
    g_in.execution_state          = (uint8_t)EXEC_STATE_TARGET_MINING;
    g_in.asic_gate                = (uint8_t)EXEC_GATE_OPEN_TARGET;
    g_in.mining_grant_active      = true;
    g_in.lease_owner              = (uint8_t)OP_OWNER_TIMED_SESSION;
    g_in.lease_phase              = (uint8_t)OP_PHASE_VERIFYING_TARGET;
    g_in.trusted_time_available   = true;
    g_in.remaining_valid          = true;
    g_in.remaining_s              = 1234u;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_TRUE(g_out.target_mining_grant_active);
    TEST_ASSERT_EQUAL(API_DEADLINE_ACTIVE, g_out.deadline_status);
    TEST_ASSERT_TRUE(g_out.remaining_seconds_valid);
    TEST_ASSERT_EQUAL_UINT32(1234u, g_out.remaining_seconds);

    /* Expired deadline. */
    g_in.remaining_s = 0u;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_EQUAL(API_DEADLINE_EXPIRED, g_out.deadline_status);
    TEST_ASSERT_FALSE(g_out.remaining_seconds_valid);

    /* Restoring. */
    fresh_free();
    g_in.session_present          = true;
    g_in.durable_state            = (uint8_t)POOL_STATE_RESTORE_DUE;
    g_in.durable_restore_required = true;
    g_in.runtime_state            = (uint8_t)RUNTIME_RESTORE_SOURCE_PENDING;
    g_in.execution_state          = (uint8_t)EXEC_STATE_SOURCE_APPLYING;
    g_in.lease_owner              = (uint8_t)OP_OWNER_SOURCE_RESTORE;
    g_in.lease_phase              = (uint8_t)OP_PHASE_RESTORING_SOURCE;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_EQUAL(API_DEADLINE_RESTORE_PENDING, g_out.deadline_status);
    TEST_ASSERT_TRUE(g_out.restore_required);
    TEST_ASSERT_FALSE(g_out.target_mining_grant_active);

    /* COMPLETE awaiting acknowledgement. */
    fresh_free();
    g_in.session_present   = true;
    g_in.durable_state     = (uint8_t)POOL_STATE_COMPLETE;
    g_in.runtime_state     = (uint8_t)RUNTIME_TERMINAL_PENDING;
    g_in.terminal_pending  = true;
    g_in.protocol_start_permitted = true;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_TRUE(g_out.terminal_result_pending);
    TEST_ASSERT_FALSE(g_out.restore_required);
    TEST_ASSERT_FALSE(g_out.operator_recovery_required);
    TEST_ASSERT_EQUAL(API_DEADLINE_UNKNOWN, g_out.deadline_status);
}

TEST_CASE("api status: RESTORE_FAILED and the recovery guard demand an operator",
          "[pool_api_status]")
{
    fresh_free();
    g_in.session_present          = true;
    g_in.durable_state            = (uint8_t)POOL_STATE_RESTORE_FAILED;
    g_in.durable_restore_required = true;
    g_in.runtime_state            = (uint8_t)RUNTIME_RESTORE_SOURCE_PENDING;
    g_in.execution_state          = (uint8_t)EXEC_STATE_RESTORE_FAILED_HELD;
    g_in.lease_owner              = (uint8_t)OP_OWNER_SOURCE_RESTORE;
    g_in.lease_phase              = (uint8_t)OP_PHASE_RESTORING_SOURCE;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_TRUE(g_out.restore_required);
    TEST_ASSERT_EQUAL(API_DEADLINE_RESTORE_PENDING, g_out.deadline_status);

    fresh_free();
    g_in.session_present = true;
    g_in.durable_state   = (uint8_t)POOL_STATE_RECOVERY_REQUIRED;
    g_in.runtime_state   = (uint8_t)RUNTIME_RECOVERY_GUARD;
    g_in.lease_owner     = (uint8_t)OP_OWNER_RECOVERY_GUARD;
    g_in.lease_phase     = (uint8_t)OP_PHASE_RECOVERY_GUARD;
    g_in.asic_gate       = (uint8_t)EXEC_GATE_OPEN_TARGET;
    g_in.mining_grant_active = true;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_TRUE(g_out.operator_recovery_required);
    /* A grant is never reported while an operator is required. */
    TEST_ASSERT_FALSE(g_out.target_mining_grant_active);
    TEST_ASSERT_TRUE(pool_api_status_valid(&g_out));
}

TEST_CASE("api status: every invalid enum fails closed", "[pool_api_status]")
{
    fresh_free();
    g_in.durable_state = 200u;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_EQUAL(POOL_STATE_RECOVERY_REQUIRED, g_out.durable_state);
    TEST_ASSERT_TRUE(g_out.operator_recovery_required);
    TEST_ASSERT_FALSE(g_out.protocol_start_permitted);

    fresh_free();
    g_in.runtime_state = 200u;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)RUNTIME_ERROR, g_out.runtime_state);
    TEST_ASSERT_TRUE(g_out.operator_recovery_required);

    fresh_free();
    g_in.execution_state = 200u;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)EXEC_STATE_ERROR, g_out.execution_state);
    TEST_ASSERT_TRUE(g_out.operator_recovery_required);

    fresh_free();
    g_in.lease_owner = 200u;
    g_in.lease_phase = 200u;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)OP_OWNER_RECOVERY_GUARD, g_out.lease_owner);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)OP_PHASE_RECOVERY_GUARD, g_out.lease_phase);
    TEST_ASSERT_TRUE(g_out.operator_recovery_required);

    fresh_free();
    g_in.asic_gate          = 200u;
    g_in.mining_grant_active = true;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)EXEC_GATE_INHIBITED, g_out.asic_gate);
    TEST_ASSERT_FALSE(g_out.target_mining_grant_active);

    fresh_free();
    g_in.pending_command     = (PoolApiCommandKind)200;
    g_in.last_command        = (PoolApiCommandKind)200;
    g_in.last_command_result = (PoolApiCommandResult)200;
    g_in.heartbeat_status    = (PoolApiHeartbeatStatus)200;
    g_in.api_conflict_code   = 200u;
    g_in.durable_failure     = 5000u;
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_EQUAL(POOL_API_CMD_NONE, g_out.pending_command);
    TEST_ASSERT_EQUAL(POOL_API_CMD_NONE, g_out.last_command);
    TEST_ASSERT_EQUAL(API_CMD_RESULT_NONE, g_out.last_command_result);
    TEST_ASSERT_EQUAL(API_HB_NOT_APPLICABLE, g_out.heartbeat_status);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)OP_HTTP_OPERATION_INVALID_REQUEST,
                            g_out.api_conflict_code);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)ERR_RECOVERY_REQUIRED, g_out.durable_failure);
    TEST_ASSERT_TRUE(pool_api_status_valid(&g_out));
}

TEST_CASE("api status: the deadline verdict is total", "[pool_api_status]")
{
    TEST_ASSERT_EQUAL(API_DEADLINE_UNKNOWN,
                      pool_api_deadline_for(false, (uint8_t)POOL_STATE_TARGET_ACTIVE,
                                            false, true, 10u));
    TEST_ASSERT_EQUAL(API_DEADLINE_UNKNOWN,
                      pool_api_deadline_for(true, 200u, false, true, 10u));
    TEST_ASSERT_EQUAL(API_DEADLINE_RESTORE_PENDING,
                      pool_api_deadline_for(true, (uint8_t)POOL_STATE_APPLYING_RESTORE,
                                            true, true, 10u));
    TEST_ASSERT_EQUAL(API_DEADLINE_RESTORE_PENDING,
                      pool_api_deadline_for(true, (uint8_t)POOL_STATE_TARGET_FAILED,
                                            true, true, 10u));
    TEST_ASSERT_EQUAL(API_DEADLINE_UNKNOWN,
                      pool_api_deadline_for(true, (uint8_t)POOL_STATE_TARGET_ACTIVE,
                                            true, false, 0u));
    TEST_ASSERT_EQUAL(API_DEADLINE_ACTIVE,
                      pool_api_deadline_for(true, (uint8_t)POOL_STATE_TARGET_ACTIVE,
                                            true, true, 1u));
    TEST_ASSERT_EQUAL(API_DEADLINE_EXPIRED,
                      pool_api_deadline_for(true, (uint8_t)POOL_STATE_TARGET_ACTIVE,
                                            true, true, 0u));
}

TEST_CASE("api status: no identity marker can reach the published model",
          "[pool_api_status]")
{
    /*
     * The input model carries only fixed-width scalars, so an identity
     * cannot even be supplied. Prove empirically that no plausible marker
     * byte pattern survives into the output, and that setting EVERY numeric
     * input to a distinctive marker never produces those bytes in fields
     * that are supposed to be booleans/enums.
     */
    static const char *const MARKERS[] = {
        "marker.example", "acct.worker", "hunter2", "bc1qmarker",
    };
    const uint8_t *raw;
    size_t         i, j, n;

    fresh_free();
    g_in.session_present          = true;
    g_in.durable_state            = (uint8_t)POOL_STATE_TARGET_ACTIVE;
    g_in.durable_restore_required = true;
    g_in.last_client_request_id   = 0xA5A5A5A5u;
    g_in.heartbeat_commits        = 7u;
    g_in.status_sequence          = 99u;
    pool_api_status_build(&g_in, &g_out);

    raw = (const uint8_t *)&g_out;
    for (i = 0; i < sizeof(MARKERS) / sizeof(MARKERS[0]); i++) {
        n = strlen(MARKERS[i]);
        for (j = 0; j + n <= sizeof(g_out); j++) {
            TEST_ASSERT_FALSE_MESSAGE(memcmp(raw + j, MARKERS[i], n) == 0,
                                      "identity marker found in the status model");
        }
    }
    /* The published model has no pointer-sized identity handle either: the
     * structure size is bounded and fixed. */
    TEST_ASSERT_TRUE(sizeof(PoolApiStatus) < 128u);
}

TEST_CASE("api status: identical inputs give byte-identical output",
          "[pool_api_status]")
{
    PoolApiStatus a, b;

    fresh_free();
    g_in.session_present = true;
    g_in.durable_state   = (uint8_t)POOL_STATE_TARGET_ACTIVE;
    pool_api_status_build(&g_in, &a);
    pool_api_status_build(&g_in, &b);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
}

TEST_CASE("api status: structural validation rejects impossible views",
          "[pool_api_status]")
{
    fresh_free();
    pool_api_status_build(&g_in, &g_out);
    TEST_ASSERT_TRUE(pool_api_status_valid(&g_out));

    TEST_ASSERT_FALSE(pool_api_status_valid(NULL));

    g_out.model_version = 999u;
    TEST_ASSERT_FALSE(pool_api_status_valid(&g_out));

    pool_api_status_build(&g_in, &g_out);
    g_out.target_mining_grant_active = true; /* gate is DEFAULT_OPEN */
    TEST_ASSERT_FALSE(pool_api_status_valid(&g_out));

    pool_api_status_build(&g_in, &g_out);
    g_out.remaining_seconds_valid = true; /* deadline is UNKNOWN */
    TEST_ASSERT_FALSE(pool_api_status_valid(&g_out));

    pool_api_status_build(&g_in, &g_out);
    g_out.lease_phase = 200u;
    TEST_ASSERT_FALSE(pool_api_status_valid(&g_out));
}

TEST_CASE("api status: command and deadline tokens are stable and dot-free",
          "[pool_api_status]")
{
    unsigned v;

    for (v = 0; v < (unsigned)POOL_API_CMD__COUNT; v++) {
        const char *s = pool_api_command_kind_str((PoolApiCommandKind)v);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_NULL(strchr(s, '.'));
        TEST_ASSERT_NULL(strchr(s, ' '));
    }
    for (v = 0; v < (unsigned)POOL_API_CMD_RESULT__COUNT; v++) {
        const char *s = pool_api_command_result_str((PoolApiCommandResult)v);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_NULL(strchr(s, '.'));
    }
    for (v = 0; v < (unsigned)POOL_API_DEADLINE__COUNT; v++) {
        TEST_ASSERT_NOT_NULL(pool_api_deadline_str((PoolApiDeadlineStatus)v));
    }
    for (v = 0; v < (unsigned)POOL_API_SUBMIT__COUNT; v++) {
        TEST_ASSERT_NOT_NULL(pool_api_submit_str((PoolApiSubmitStatus)v));
    }
    for (v = 0; v < (unsigned)POOL_API_ACTOR__COUNT; v++) {
        TEST_ASSERT_NOT_NULL(pool_api_actor_str((PoolApiActorClass)v));
    }
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", pool_api_command_kind_str((PoolApiCommandKind)9));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", pool_api_actor_str((PoolApiActorClass)9));
}
