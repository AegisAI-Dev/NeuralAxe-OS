#ifndef POOL_OPERATION_COORDINATOR_H_
#define POOL_OPERATION_COORDINATOR_H_

#include "pool_operation_policy.h"

/*
 * NeuralAxe operation ownership — caller-owned synchronized coordinator
 * (Phase 2M.1B, Gate B5).
 *
 * A bounded wrapper serializing the pure policy over one instance. All
 * check-and-acquire / transition / proof / release operations run atomically
 * inside a single bounded spinlock critical section (the pure policy is
 * bounded arithmetic — no I/O, no callbacks, no NVS, no networking, no lock
 * recursion, no heap, no tasks, no queues). Snapshots are consistent copies.
 *
 * GATE B5 CONTRACT: compiled as a production component but NEVER
 * instantiated or acquired by any production runtime path — no singleton
 * exists. Gates B6+ create the single caller-owned instance during boot and
 * route the audited mutation paths through it.
 *
 * PROTOCOL-COORDINATOR FUTURE CONTRACT (Stage 17; enforced by B6+, not
 * here — a compile adapter was deliberately NOT added because
 * protocol_coordinator.h transitively includes the whole application
 * surface via global_state.h, which would destroy this component's
 * isolation; the API is 8 functions with no enums to pin):
 *  - pool-configuration mutation requires an exclusive lease whose scope
 *    includes OP_SCOPE_POOL_CONFIGURATION;
 *  - Stratum stop/start/reconnect beyond the coordinator's own failover
 *    housekeeping requires OP_SCOPE_STRATUM_CONTROL;
 *  - controlled restart requires OP_SCOPE_DEVICE_RESTART;
 *  - timed-session internal work must present the current session token
 *    (OP_REQUEST_TIMED_SESSION_INTERNAL); stale tokens are rejected;
 *  - protocol_coordinator_notify_failure/notify_success and the v1/v2
 *    shutdown handshakes remain the coordinator task's internal duty
 *    (OP_REQUEST_PROTOCOL_RECONCILE), denied only while
 *    WAITING_FOR_TRUSTED_TIME holds Stratum;
 *  - B4 target-verification eligibility remains VERIFY_BEFORE_MINING —
 *    nothing in B5 or this contract grants ALLOW_TARGET_MINING.
 * Future call sites: B6 scheduler (session internal work + verification),
 * B7 restore execution, B8 HTTP handlers (PATCH/OTA/restart/ack guards).
 */

typedef struct {
    bool               initialized;
    PoolOperationState state;
} PoolOperationCoordinator;

/* Deterministic lifecycle. Init yields the fail-closed UNBOOTSTRAPPED
 * state; deinit resets. Both are idempotent-safe on a zeroed instance. */
PoolOperationStatus pool_operation_coordinator_init(PoolOperationCoordinator *c);
PoolOperationStatus pool_operation_coordinator_deinit(PoolOperationCoordinator *c);

/* Fail-closed bootstrap from B3/B4 facts (atomic). */
PoolOperationStatus pool_operation_coordinator_bootstrap(
    PoolOperationCoordinator *c, const PoolOperationBootstrapInput *in,
    PoolOperationLeaseToken *out_token);

/* Consistent snapshot copy (never a torn read). */
PoolOperationStatus pool_operation_coordinator_snapshot(PoolOperationCoordinator *c,
                                                        PoolOperationState *out);

/* Read-only conflict evaluation against a consistent snapshot. */
PoolOperationDecision pool_operation_coordinator_evaluate(
    PoolOperationCoordinator *c, const PoolOperationRequest *req);

/* Atomic check-and-acquire (Stage 9). */
PoolOperationStatus pool_operation_coordinator_try_acquire(
    PoolOperationCoordinator *c, const PoolOperationRequest *req,
    PoolOperationLeaseToken *out_token, PoolOperationDecision *out_decision);

/* Atomic Restore Now control transition (Stage 12). */
PoolOperationStatus pool_operation_coordinator_restore_now(
    PoolOperationCoordinator *c, uint32_t session_id,
    PoolOperationLeaseToken *out_token);

/* Atomic token-verified phase transition (Stage 11). */
PoolOperationStatus pool_operation_coordinator_transition_phase(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    PoolOperationLeasePhase target, PoolOperationLeaseToken *out_token);

/* Atomic persistence-proof application (Stage 10). */
PoolOperationStatus pool_operation_coordinator_apply_persistence_proof(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    const PoolOperationPersistenceProof *proof, PoolOperationLeaseToken *out_token);

/* Atomic releases (Stage 13/14). */
PoolOperationStatus pool_operation_coordinator_release_session(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token);
PoolOperationStatus pool_operation_coordinator_release_ack(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    const PoolOperationPersistenceProof *proof);
PoolOperationStatus pool_operation_coordinator_release_manual(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    PoolOperationOutcome outcome);

/* Atomic Gate B8 no-mutation aborts (see pool_operation_policy.h for the
 * exact admissibility and evidence rules). */
PoolOperationStatus pool_operation_coordinator_abort_reservation(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    const PoolOperationNoMutationEvidence *evidence);
PoolOperationStatus pool_operation_coordinator_abort_acknowledge(
    PoolOperationCoordinator *c, const PoolOperationLeaseToken *token,
    const PoolOperationNoMutationEvidence *evidence);

#endif /* POOL_OPERATION_COORDINATOR_H_ */
