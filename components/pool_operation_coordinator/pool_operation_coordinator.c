/*
 * NeuralAxe operation ownership — synchronized coordinator (Gate B5).
 *
 * A bounded spinlock serializes every state-touching call; the pure policy
 * runs entirely inside the critical section (bounded arithmetic only — no
 * I/O, no callbacks, no NVS, no networking, no heap, no recursion).
 * Snapshot reads copy the state under the same lock, so no torn or
 * partially published lease/token state is ever observable.
 *
 * The lock is a file-static portMUX shared by all instances (Gate B2
 * precedent): the future runtime owns exactly ONE caller-allocated
 * instance, created at boot by Gate B6 — B5 creates none.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "pool_operation_coordinator.h"

static portMUX_TYPE s_op_lock = portMUX_INITIALIZER_UNLOCKED;

PoolOperationStatus pool_operation_coordinator_init(PoolOperationCoordinator *c)
{
    if (c == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    memset(c, 0, sizeof(*c));
    pool_operation_state_init(&c->state);
    c->initialized = true;
    portEXIT_CRITICAL(&s_op_lock);
    return OP_OK;
}

PoolOperationStatus pool_operation_coordinator_deinit(PoolOperationCoordinator *c)
{
    if (c == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    memset(c, 0, sizeof(*c)); /* back to uninitialized (fail closed) */
    portEXIT_CRITICAL(&s_op_lock);
    return OP_OK;
}

PoolOperationStatus pool_operation_coordinator_bootstrap(
    PoolOperationCoordinator *c, const PoolOperationBootstrapInput *in,
    PoolOperationLeaseToken *out_token)
{
    PoolOperationStatus st;
    if (c == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    if (!c->initialized) {
        st = OP_ERR_NOT_INITIALIZED;
        portEXIT_CRITICAL(&s_op_lock);
        if (out_token != NULL) {
            memset(out_token, 0, sizeof(*out_token));
        }
        return st;
    }
    st = pool_operation_bootstrap(&c->state, in, out_token);
    portEXIT_CRITICAL(&s_op_lock);
    return st;
}

PoolOperationStatus pool_operation_coordinator_snapshot(PoolOperationCoordinator *c,
                                                        PoolOperationState *out)
{
    if (c == NULL || out == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    if (!c->initialized) {
        portEXIT_CRITICAL(&s_op_lock);
        pool_operation_state_init(out);
        return OP_ERR_NOT_INITIALIZED;
    }
    *out = c->state; /* consistent copy */
    portEXIT_CRITICAL(&s_op_lock);
    return OP_OK;
}

PoolOperationDecision pool_operation_coordinator_evaluate(
    PoolOperationCoordinator *c, const PoolOperationRequest *req)
{
    PoolOperationState snap;
    PoolOperationDecision d;

    if (c == NULL || req == NULL) {
        memset(&d, 0, sizeof(d));
        d.allowed = false;
        d.status = OP_ERR_INVALID_ARGUMENT;
        return d;
    }
    if (pool_operation_coordinator_snapshot(c, &snap) != OP_OK) {
        memset(&d, 0, sizeof(d));
        d.allowed = (req->kind == OP_REQUEST_READ_ONLY);
        d.status = d.allowed ? OP_OK : OP_ERR_NOT_INITIALIZED;
        return d;
    }
    /* Pure evaluation on the consistent copy — never mutates. */
    return pool_operation_evaluate(&snap, req);
}

PoolOperationStatus pool_operation_coordinator_try_acquire(
    PoolOperationCoordinator *c, const PoolOperationRequest *req,
    PoolOperationLeaseToken *out_token, PoolOperationDecision *out_decision)
{
    PoolOperationStatus st;
    /* output init happens inside the alias-safe policy call */
    if (c == NULL || req == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    if (!c->initialized) {
        portEXIT_CRITICAL(&s_op_lock);
        return OP_ERR_NOT_INITIALIZED;
    }
    /* Atomic check-and-acquire: evaluation + reservation in one section. */
    st = pool_operation_acquire(&c->state, req, out_token, out_decision);
    portEXIT_CRITICAL(&s_op_lock);
    return st;
}

PoolOperationStatus pool_operation_coordinator_restore_now(
    PoolOperationCoordinator *c, uint32_t session_id,
    PoolOperationLeaseToken *out_token)
{
    PoolOperationStatus st;
    if (out_token != NULL) {
        memset(out_token, 0, sizeof(*out_token));
    }
    if (c == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    if (!c->initialized) {
        portEXIT_CRITICAL(&s_op_lock);
        return OP_ERR_NOT_INITIALIZED;
    }
    st = pool_operation_restore_now(&c->state, session_id, out_token);
    portEXIT_CRITICAL(&s_op_lock);
    return st;
}

PoolOperationStatus pool_operation_coordinator_transition_phase(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    PoolOperationLeasePhase target, PoolOperationLeaseToken *out_token)
{
    PoolOperationStatus st;
    /* out_token init happens inside the alias-safe policy call */
    if (c == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    if (!c->initialized) {
        portEXIT_CRITICAL(&s_op_lock);
        return OP_ERR_NOT_INITIALIZED;
    }
    st = pool_operation_transition_phase(&c->state, token, target, out_token);
    portEXIT_CRITICAL(&s_op_lock);
    return st;
}

PoolOperationStatus pool_operation_coordinator_apply_persistence_proof(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    const PoolOperationPersistenceProof *proof, PoolOperationLeaseToken *out_token)
{
    PoolOperationStatus st;
    /* out_token init happens inside the alias-safe policy call */
    if (c == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    if (!c->initialized) {
        portEXIT_CRITICAL(&s_op_lock);
        return OP_ERR_NOT_INITIALIZED;
    }
    st = pool_operation_apply_persistence_proof(&c->state, token, proof, out_token);
    portEXIT_CRITICAL(&s_op_lock);
    return st;
}

PoolOperationStatus pool_operation_coordinator_release_session(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token)
{
    PoolOperationStatus st;
    if (c == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    if (!c->initialized) {
        portEXIT_CRITICAL(&s_op_lock);
        return OP_ERR_NOT_INITIALIZED;
    }
    st = pool_operation_release_session(&c->state, token);
    portEXIT_CRITICAL(&s_op_lock);
    return st;
}

PoolOperationStatus pool_operation_coordinator_release_ack(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    const PoolOperationPersistenceProof *proof)
{
    PoolOperationStatus st;
    if (c == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    if (!c->initialized) {
        portEXIT_CRITICAL(&s_op_lock);
        return OP_ERR_NOT_INITIALIZED;
    }
    st = pool_operation_release_ack(&c->state, token, proof);
    portEXIT_CRITICAL(&s_op_lock);
    return st;
}

PoolOperationStatus pool_operation_coordinator_release_manual(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    PoolOperationOutcome outcome)
{
    PoolOperationStatus st;
    if (c == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    if (!c->initialized) {
        portEXIT_CRITICAL(&s_op_lock);
        return OP_ERR_NOT_INITIALIZED;
    }
    st = pool_operation_release_manual(&c->state, token, outcome);
    portEXIT_CRITICAL(&s_op_lock);
    return st;
}

PoolOperationStatus pool_operation_coordinator_abort_reservation(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    const PoolOperationNoMutationEvidence *evidence)
{
    PoolOperationStatus st;
    if (c == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    if (!c->initialized) {
        portEXIT_CRITICAL(&s_op_lock);
        return OP_ERR_NOT_INITIALIZED;
    }
    st = pool_operation_abort_reservation(&c->state, token, evidence);
    portEXIT_CRITICAL(&s_op_lock);
    return st;
}

PoolOperationStatus pool_operation_coordinator_abort_acknowledge(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    const PoolOperationNoMutationEvidence *evidence)
{
    PoolOperationStatus st;
    if (c == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&s_op_lock);
    if (!c->initialized) {
        portEXIT_CRITICAL(&s_op_lock);
        return OP_ERR_NOT_INITIALIZED;
    }
    st = pool_operation_abort_acknowledge(&c->state, token, evidence);
    portEXIT_CRITICAL(&s_op_lock);
    return st;
}
