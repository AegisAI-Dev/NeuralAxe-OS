/*
 * NeuralAxe timed pool sessions — production mutation fence (Gate B7).
 * See pool_session_runtime_admission.h for the contract.
 *
 * The evaluation core is pure and read-only: it takes a consistent B5
 * snapshot through the coordinator's own API and never acquires a lease,
 * never mutates coordinator state and never performs IO. The production
 * wrapper adds only the singleton lookup and the feature posture.
 */

#include <string.h>
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

/* Fill the sanitized fail-closed conflict used when ownership is unknown. */
static void admission_bootstrap_conflict(PoolOperationHttpConflict *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->http_status  = 409;
    out->code         = OP_HTTP_OPERATION_BOOTSTRAP_REQUIRED;
    out->retryable    = true; /* bootstrap completes shortly after boot */
    out->active_owner = OP_OWNER_NONE;
}

/*
 * The ONE read-only evaluation shared by the verdict and the standardized
 * HTTP conflict surface. It acquires nothing and mutates nothing.
 */
static NxAdmissionVerdict admission_core(PoolOperationCoordinator *coord,
                                         NxMutationKind kind,
                                         PoolOperationHttpConflict *out_conflict)
{
    PoolOperationRequest  req;
    PoolOperationDecision dec;
    PoolOperationState    snap;

    if (out_conflict != NULL) {
        memset(out_conflict, 0, sizeof(*out_conflict));
    }
    if (coord == NULL || (unsigned)kind >= (unsigned)NX_MUTATION__COUNT) {
        if (out_conflict != NULL) {
            memset(out_conflict, 0, sizeof(*out_conflict));
            out_conflict->http_status = 409;
            out_conflict->code        = OP_HTTP_OPERATION_INVALID_REQUEST;
        }
        return NX_ADMIT_DENY_INVALID; /* fail closed */
    }
    if (pool_operation_coordinator_snapshot(coord, &snap) != OP_OK ||
        !snap.bootstrapped) {
        /* Ownership is unknown — never assume the device is free. */
        admission_bootstrap_conflict(out_conflict);
        return NX_ADMIT_DENY_UNBOOTSTRAPPED;
    }

    req.kind       = nx_admission_request_kind(kind);
    req.session_id = 0u;
    req.token.valid            = false;
    req.token.owner            = OP_OWNER_NONE;
    req.token.lease_generation = 0u;

    dec = pool_operation_coordinator_evaluate(coord, &req);
    /* The committed B5 mapper is the ONLY conflict-body authority: every
     * denial becomes 409 with a stable code and no identity. */
    pool_operation_http_map(&dec, &snap, out_conflict);
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

NxAdmissionVerdict nx_admission_evaluate(PoolOperationCoordinator *coord,
                                         NxMutationKind kind)
{
    return admission_core(coord, kind, NULL);
}

NxAdmissionVerdict nx_admission_conflict(PoolOperationCoordinator *coord,
                                         NxMutationKind kind,
                                         PoolOperationHttpConflict *out_conflict)
{
    return admission_core(coord, kind, out_conflict);
}

bool nx_timed_sessions_mutation_allowed(NxMutationKind kind,
                                        NxAdmissionVerdict *out_verdict)
{
    return nx_timed_sessions_mutation_conflict(kind, out_verdict, NULL);
}

bool nx_timed_sessions_mutation_conflict(NxMutationKind kind,
                                         NxAdmissionVerdict *out_verdict,
                                         PoolOperationHttpConflict *out_conflict)
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
        admission_bootstrap_conflict(out_conflict);
    } else {
        v = admission_core(&rt->coord, kind, out_conflict);
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
    if (out_conflict != NULL) {
        memset(out_conflict, 0, sizeof(*out_conflict));
        out_conflict->code = OP_HTTP_NONE;
    }
    return true;
#endif
}
