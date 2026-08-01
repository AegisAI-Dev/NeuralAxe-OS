/*
 * NeuralAxe timed pool sessions — production mutation fence (Gate B7).
 * See pool_session_runtime_admission.h for the contract.
 *
 * The evaluation core is pure and read-only: it takes a consistent B5
 * snapshot through the coordinator's own API and never acquires a lease,
 * never mutates coordinator state and never performs IO. The production
 * wrapper adds only the singleton lookup and the feature posture.
 */

#include "sdkconfig.h"
#include "pool_session_runtime_admission.h"
#include "pool_session_runtime.h"

PoolOperationRequestKind nx_admission_request_kind(NxMutationKind kind)
{
    switch (kind) {
    case NX_MUTATION_POOL_CONFIG:
        return OP_REQUEST_MANUAL_POOL_PATCH;
    case NX_MUTATION_DEVICE_RESTART:
        return OP_REQUEST_MANUAL_DEVICE_RESTART;
    case NX_MUTATION_OTA_UPDATE:
        return OP_REQUEST_OTA_UPDATE;
    default:
        /* Unknown class: map to the reserved kind B5 always denies. */
        return OP_REQUEST_DESTRUCTIVE_MAINTENANCE;
    }
}

bool nx_admission_verdict_allows(NxAdmissionVerdict v)
{
    return v == NX_ADMIT_ALLOW;
}

const char *nx_admission_verdict_str(NxAdmissionVerdict v)
{
    switch (v) {
    case NX_ADMIT_ALLOW:                 return "NX_ADMIT_ALLOW";
    case NX_ADMIT_DENY_SESSION_OWNER:    return "NX_ADMIT_DENY_SESSION_OWNER";
    case NX_ADMIT_DENY_RECOVERY_GUARD:   return "NX_ADMIT_DENY_RECOVERY_GUARD";
    case NX_ADMIT_DENY_TERMINAL_PENDING: return "NX_ADMIT_DENY_TERMINAL_PENDING";
    case NX_ADMIT_DENY_UNBOOTSTRAPPED:   return "NX_ADMIT_DENY_UNBOOTSTRAPPED";
    case NX_ADMIT_DENY_INVALID:          return "NX_ADMIT_DENY_INVALID";
    default:                             return "NX_ADMIT_UNKNOWN";
    }
}

NxAdmissionVerdict nx_admission_evaluate(PoolOperationCoordinator *coord,
                                         NxMutationKind kind)
{
    PoolOperationRequest  req;
    PoolOperationDecision dec;
    PoolOperationState    snap;

    if (coord == NULL || (unsigned)kind >= (unsigned)NX_MUTATION__COUNT) {
        return NX_ADMIT_DENY_INVALID; /* fail closed */
    }
    if (pool_operation_coordinator_snapshot(coord, &snap) != OP_OK ||
        !snap.bootstrapped) {
        /* Ownership is unknown — never assume the device is free. */
        return NX_ADMIT_DENY_UNBOOTSTRAPPED;
    }

    req.kind       = nx_admission_request_kind(kind);
    req.session_id = 0u;
    req.token.valid            = false;
    req.token.owner            = OP_OWNER_NONE;
    req.token.lease_generation = 0u;

    dec = pool_operation_coordinator_evaluate(coord, &req);
    if (dec.allowed) {
        return NX_ADMIT_ALLOW;
    }

    /* Classify the denial by the committed B5 state, owner CLASS only. */
    if (snap.phase == OP_PHASE_RECOVERY_GUARD ||
        snap.owner == OP_OWNER_RECOVERY_GUARD ||
        snap.owner == OP_OWNER_OPERATOR_RECOVERY) {
        return NX_ADMIT_DENY_RECOVERY_GUARD;
    }
    if (snap.owner != OP_OWNER_NONE) {
        return NX_ADMIT_DENY_SESSION_OWNER;
    }
    if (snap.terminal_pending) {
        return NX_ADMIT_DENY_TERMINAL_PENDING;
    }
    return NX_ADMIT_DENY_SESSION_OWNER; /* denied for an ownership reason */
}

bool nx_timed_sessions_mutation_allowed(NxMutationKind kind,
                                        NxAdmissionVerdict *out_verdict)
{
#ifdef CONFIG_NX_TIMED_SESSIONS
    PoolSessionRuntime *rt = pool_session_runtime_default_instance();
    NxAdmissionVerdict  v;

    if (rt == NULL || !rt->initialized || !rt->booted) {
        /*
         * The feature is compiled in but the runtime has not produced a
         * coherent ownership view yet. A session record may exist, so fail
         * closed rather than let a writer race the bootstrap.
         */
        v = NX_ADMIT_DENY_UNBOOTSTRAPPED;
    } else {
        v = nx_admission_evaluate(&rt->coord, kind);
    }
    if (out_verdict != NULL) {
        *out_verdict = v;
    }
    return nx_admission_verdict_allows(v);
#else
    /* Feature disabled: no coordinator, no session, nothing to fence —
     * production behavior is unchanged. */
    (void)kind;
    if (out_verdict != NULL) {
        *out_verdict = NX_ADMIT_ALLOW;
    }
    return true;
#endif
}
