#ifndef POOL_OPERATION_POLICY_H_
#define POOL_OPERATION_POLICY_H_

#include "pool_operation_types.h"

/*
 * NeuralAxe operation ownership — pure policy and transition logic
 * (Phase 2M.1B, Gate B5).
 *
 * PURE: no ESP-IDF includes, no locking, no I/O, no heap, no global mutable
 * state. The synchronized coordinator (pool_operation_coordinator.h)
 * serializes these functions; on their own they mutate only the caller's
 * PoolOperationState and are exhaustively unit-testable.
 *
 * FAIL-CLOSED CONTRACT:
 *  - UNBOOTSTRAPPED denies every mutating request (read-only classification
 *    is allowed). Bootstrap consumes the B3 store result + validated record
 *    + B4 recovery plan and reconstructs durable ownership; any corrupt,
 *    unsupported, ambiguous, uncertain or inconsistent input reconstructs
 *    the RECOVERY_GUARD, never a free coordinator.
 *  - At most one exclusive mutating owner exists; no shared or partial
 *    mutating ownership exists in B5.
 *  - No phase silently becomes FREE; releases demand explicit durable
 *    evidence; STORE_COMMIT_UNCERTAIN never releases anything.
 *  - Lease generations start at 1, never wrap; exhaustion is a stable
 *    fail-safe error, never an unlocked coordinator. No lease expires with
 *    time.
 */

/* ------------------------------------------------------------------ */
/* Bootstrap                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    PoolStoreResult                store_result;
    bool                           record_present;
    const PoolSessionRecord       *record; /* required when STORE_OK          */
    const PoolSessionRecoveryPlan *plan;   /* required always (B4 ran)        */
    uint32_t                       committed_generation; /* B3 generation     */
} PoolOperationBootstrapInput;

/* Zero-initialize to the fail-closed UNBOOTSTRAPPED state. */
void pool_operation_state_init(PoolOperationState *s);

/*
 * Fail-closed ownership bootstrap (Stage 7). Deterministic; a second call
 * on a bootstrapped state returns OP_ERR_ALREADY_BOOTSTRAPPED unchanged.
 * On durable-owner reconstruction, *out_token (optional) receives the
 * initial lease token for the future runtime integrator.
 */
PoolOperationStatus pool_operation_bootstrap(PoolOperationState *s,
                                             const PoolOperationBootstrapInput *in,
                                             PoolOperationLeaseToken *out_token);

/* ------------------------------------------------------------------ */
/* Total conflict matrix (Stage 8) — read-only evaluation              */
/* ------------------------------------------------------------------ */

PoolOperationDecision pool_operation_evaluate(const PoolOperationState *s,
                                              const PoolOperationRequest *req);

/* ------------------------------------------------------------------ */
/* Mutating operations (serialized by the coordinator)                 */
/* ------------------------------------------------------------------ */

/*
 * Check-and-acquire. Evaluates the matrix; on an allowed lease-acquiring
 * request it reserves exclusive ownership and returns the token. A failed
 * or read-only request changes no state. A successful reservation does NOT
 * itself authorize any external action (persistence barrier, Stage 10).
 */
PoolOperationStatus pool_operation_acquire(PoolOperationState *s,
                                           const PoolOperationRequest *req,
                                           PoolOperationLeaseToken *out_token,
                                           PoolOperationDecision *out_decision);

/*
 * Restore Now (Stage 12): a CONTROL request to the existing timed-session /
 * recovery owner — never a second competing lease. Matching session id
 * transitions ownership to SOURCE_RESTORE (generation bump makes stale
 * target-phase tokens invalid); repeated matching requests are idempotent.
 */
PoolOperationStatus pool_operation_restore_now(PoolOperationState *s,
                                               uint32_t session_id,
                                               PoolOperationLeaseToken *out_token);

/*
 * Token-verified phase transition (Stage 11) over the explicit legal graph:
 *   ACTIVE -> WAITING_FOR_TRUSTED_TIME | VERIFYING_TARGET | RESTORING_SOURCE
 *   WAITING_FOR_TRUSTED_TIME -> VERIFYING_TARGET | RESTORING_SOURCE
 *   VERIFYING_TARGET -> ACTIVE | RESTORING_SOURCE
 *   RESERVED_PENDING_PERSISTENCE -> RESTORING_SOURCE (cancel-like path)
 *   any owned phase -> RECOVERY_GUARD (uncertainty escalation)
 * RESERVED -> ACTIVE is refused here (persistence proof required);
 * TERMINAL_ACK_PENDING is reachable only through a terminal proof.
 * Success bumps the generation and returns the replacement token.
 */
PoolOperationStatus pool_operation_transition_phase(PoolOperationState *s,
                                                    const PoolOperationLeaseToken *token,
                                                    PoolOperationLeasePhase target,
                                                    PoolOperationLeaseToken *out_token);

/*
 * Apply durable persistence evidence (Stage 10/11):
 *   SESSION_COMMITTED  : RESERVED -> ACTIVE (matching session, STORE_OK)
 *   RECOVERY_UPDATE_COMMITTED : clears persistence_required in place
 *   TERMINAL_COMMITTED : session-class owner -> TERMINAL_ACK_PENDING, only
 *                        for COMPLETE / pre-mutation CANCELLED / audited
 *                        pre-mutation RECOVERY_REQUIRED with
 *                        restore_required=false
 *   STORE_COMMIT_UNCERTAIN in any proof -> RECOVERY_GUARD (never release)
 * Mismatched/stale proofs are rejected without state change.
 */
PoolOperationStatus pool_operation_apply_persistence_proof(
    PoolOperationState *s, const PoolOperationLeaseToken *token,
    const PoolOperationPersistenceProof *proof, PoolOperationLeaseToken *out_token);

/*
 * Release a session-class lease (Stage 13). Permitted ONLY from
 * TERMINAL_ACK_PENDING (a terminal proof was applied): ownership becomes
 * FREE while the terminal result stays retained (terminal_pending=true) —
 * blocking new timed sessions until acknowledgement.
 */
PoolOperationStatus pool_operation_release_session(PoolOperationState *s,
                                                   const PoolOperationLeaseToken *token);

/*
 * Release the acknowledgement lease with a committed-tombstone proof
 * (OP_PROOF_CLEARED + STORE_CLEARED): clears terminal_pending so a new
 * session becomes eligible. An uncertain commit becomes the guard; a
 * mismatched proof is rejected without change.
 */
PoolOperationStatus pool_operation_release_ack(PoolOperationState *s,
                                               const PoolOperationLeaseToken *token,
                                               const PoolOperationPersistenceProof *proof);

/*
 * Release a manual (pool PATCH / OTA / operator-recovery) lease with
 * explicit completion evidence: NO_MUTATION_OCCURRED or VERIFIED_COMPLETE
 * release; MUTATION_UNCERTAIN transitions to the RECOVERY_GUARD instead.
 * An operator-recovery lease returns to the guard (evidence retained).
 */
PoolOperationStatus pool_operation_release_manual(PoolOperationState *s,
                                                  const PoolOperationLeaseToken *token,
                                                  PoolOperationOutcome outcome);

#endif /* POOL_OPERATION_POLICY_H_ */
