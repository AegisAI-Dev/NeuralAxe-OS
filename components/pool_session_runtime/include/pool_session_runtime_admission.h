#ifndef POOL_SESSION_RUNTIME_ADMISSION_H_
#define POOL_SESSION_RUNTIME_ADMISSION_H_

#include <stdbool.h>
#include <stdint.h>
#include "pool_operation_coordinator.h"

/*
 * NeuralAxe timed pool sessions — PRODUCTION MUTATION FENCE
 * (Phase 2M.1B, Gate B7). Board 601 / BM1370 only.
 *
 * Gate B5 defined the ownership policy; Gate B6 reconstructed the single
 * owner at boot. This header is the missing piece: the ONE bounded function
 * that real production mutation paths call BEFORE their first side effect,
 * so an incompatible writer fails while a timed-session, boot-recovery or
 * source-restore owner exists — instead of relying on a future gate to wire
 * the check up.
 *
 * WHAT IT GUARANTEES AT THE CALL SITES (all audited in Gate B7):
 *  - no NVS queue entry is enqueued and no RAM configuration cache changes;
 *  - no OTA session begins and no firmware/web image is written;
 *  - no restart is executed;
 *  - no second protocol lifecycle owner starts.
 *
 * FEATURE POSTURE: the whole fence is compiled only under
 * CONFIG_NX_TIMED_SESSIONS. With the feature disabled the production
 * wrapper is a no-op that always admits, so unflagged firmware behaves
 * exactly as before (no coordinator exists, no session can exist, and the
 * call sites compile the check out entirely).
 *
 * TRUST BOUNDARY (unchanged from B5): this is conflict prevention among
 * cooperating firmware paths, not a security sandbox. It stops accidental
 * and concurrent mutation; it is not an authorization system, and the
 * unauthenticated-LAN-API posture remains a separate documented residual.
 *
 * PRIVACY: a denial carries a stable machine token and an owner CLASS only
 * — never a hostname, account, worker, wallet, password, session id, record
 * byte or lease token.
 */

/* The production mutation classes that must yield to a session owner. */
typedef enum {
    NX_MUTATION_POOL_CONFIG = 0,  /* any pool-identity configuration write */
    NX_MUTATION_DEVICE_RESTART,   /* API/BAP restart, incl. autonomous ones */
    NX_MUTATION_OTA_UPDATE,       /* firmware or web-asset OTA             */
    NX_MUTATION__COUNT
} NxMutationKind;

/* Stable machine-readable admission verdicts (no identity, no prose). */
typedef enum {
    NX_ADMIT_ALLOW = 0,
    NX_ADMIT_DENY_SESSION_OWNER,   /* a timed-session/recovery owner exists */
    NX_ADMIT_DENY_RECOVERY_GUARD,  /* fail-closed guard is engaged          */
    NX_ADMIT_DENY_TERMINAL_PENDING,/* terminal retained; ack required first */
    NX_ADMIT_DENY_UNBOOTSTRAPPED,  /* ownership unknown: fail closed        */
    NX_ADMIT_DENY_INVALID,         /* malformed request: fail closed        */
    NX_ADMIT__COUNT
} NxAdmissionVerdict;

/* Pure mapping onto the committed B5 request kinds. Total; an unknown kind
 * maps to the reserved destructive-maintenance kind, which B5 always denies. */
PoolOperationRequestKind nx_admission_request_kind(NxMutationKind kind);

/*
 * Testable core: evaluate one mutation against an EXPLICIT coordinator,
 * read-only (no lease is acquired and no state changes). A NULL or
 * uninitialized coordinator fails closed.
 */
NxAdmissionVerdict nx_admission_evaluate(PoolOperationCoordinator *coord,
                                         NxMutationKind kind);

/* True only for NX_ADMIT_ALLOW (total; unknown values never admit). */
bool nx_admission_verdict_allows(NxAdmissionVerdict v);

/* Stable machine token (dot-free; never carries identity or prose). */
const char *nx_admission_verdict_str(NxAdmissionVerdict v);

/*
 * THE production entry point called by real mutation paths.
 *
 * Returns true when the mutation may proceed. With CONFIG_NX_TIMED_SESSIONS
 * disabled it always returns true (unchanged firmware behavior). With the
 * feature enabled it consults the live B5 coordinator inside the single
 * production runtime instance; an un-booted or incoherent runtime fails
 * closed. `out_verdict` (optional) receives the machine token for logging
 * or a future HTTP 409 body.
 */
bool nx_timed_sessions_mutation_allowed(NxMutationKind kind,
                                        NxAdmissionVerdict *out_verdict);

_Static_assert(NX_MUTATION__COUNT == 3, "mutation class count changed — review call sites");
_Static_assert(NX_ADMIT__COUNT == 6, "verdict count changed — review tokens/tests");
_Static_assert(NX_ADMIT_ALLOW == 0, "ALLOW must be zero so a zeroed verdict is explicit");

#endif /* POOL_SESSION_RUNTIME_ADMISSION_H_ */
