/*
 * NeuralAxe operation ownership — pure policy and transitions (Gate B5).
 * PURE: no ESP-IDF, no locking, no I/O, no heap, no global mutable state.
 */

#include <string.h>
#include "pool_operation_policy.h"

/* Session-class owners: the one logical timed-session/recovery lease. */
static bool owner_is_session_class(PoolOperationOwner o)
{
    return o == OP_OWNER_TIMED_SESSION || o == OP_OWNER_BOOT_RECOVERY ||
           o == OP_OWNER_SOURCE_RESTORE;
}

static const uint32_t SESSION_SCOPES = OP_SCOPE_SESSION_STORE |
                                       OP_SCOPE_POOL_CONFIGURATION |
                                       OP_SCOPE_STRATUM_CONTROL |
                                       OP_SCOPE_DEVICE_RESTART;

/* ------------------------------------------------------------------ */
/* Request properties (total; invalid values fail closed)              */
/* ------------------------------------------------------------------ */

bool pool_operation_request_is_read_only(PoolOperationRequestKind k)
{
    return k == OP_REQUEST_READ_ONLY;
}

bool pool_operation_request_is_mutating(PoolOperationRequestKind k)
{
    switch (k) {
    case OP_REQUEST_READ_ONLY:
        return false;
    case OP_REQUEST_TIMED_SESSION_START:
    case OP_REQUEST_TIMED_SESSION_INTERNAL:
    case OP_REQUEST_RESTORE_NOW:
    case OP_REQUEST_SESSION_ACKNOWLEDGE:
    case OP_REQUEST_MANUAL_POOL_PATCH:
    case OP_REQUEST_OTA_UPDATE:
    case OP_REQUEST_MANUAL_DEVICE_RESTART:
    case OP_REQUEST_DESTRUCTIVE_MAINTENANCE:
    case OP_REQUEST_OPERATOR_RECOVERY:
    case OP_REQUEST_PROTOCOL_RECONCILE:
        return true;
    default:
        return true; /* unknown requests are treated as mutating: fail closed */
    }
}

bool pool_operation_request_requires_exclusive_lease(PoolOperationRequestKind k)
{
    switch (k) {
    case OP_REQUEST_TIMED_SESSION_START:
    case OP_REQUEST_SESSION_ACKNOWLEDGE:
    case OP_REQUEST_MANUAL_POOL_PATCH:
    case OP_REQUEST_OTA_UPDATE:
    case OP_REQUEST_OPERATOR_RECOVERY:
        return true;
    default:
        return false; /* internal/restore-now ride the EXISTING lease */
    }
}

bool pool_operation_request_requires_session_context(PoolOperationRequestKind k)
{
    return k == OP_REQUEST_TIMED_SESSION_INTERNAL || k == OP_REQUEST_RESTORE_NOW ||
           k == OP_REQUEST_SESSION_ACKNOWLEDGE;
}

bool pool_operation_request_requires_terminal_record(PoolOperationRequestKind k)
{
    return k == OP_REQUEST_SESSION_ACKNOWLEDGE;
}

uint32_t pool_operation_request_scope(PoolOperationRequestKind k)
{
    switch (k) {
    case OP_REQUEST_TIMED_SESSION_START:
    case OP_REQUEST_TIMED_SESSION_INTERNAL:
    case OP_REQUEST_RESTORE_NOW:
        return SESSION_SCOPES;
    case OP_REQUEST_SESSION_ACKNOWLEDGE:
        return OP_SCOPE_SESSION_STORE;
    case OP_REQUEST_MANUAL_POOL_PATCH:
        /* the audited PATCH path requires a restart to take effect */
        return OP_SCOPE_POOL_CONFIGURATION | OP_SCOPE_STRATUM_CONTROL |
               OP_SCOPE_DEVICE_RESTART;
    case OP_REQUEST_OTA_UPDATE:
        return OP_SCOPE_OTA_FLASH | OP_SCOPE_DEVICE_RESTART;
    case OP_REQUEST_MANUAL_DEVICE_RESTART:
        return OP_SCOPE_DEVICE_RESTART;
    case OP_REQUEST_DESTRUCTIVE_MAINTENANCE:
        return OP_SCOPE_DESTRUCTIVE_MAINTENANCE;
    case OP_REQUEST_PROTOCOL_RECONCILE:
        return OP_SCOPE_STRATUM_CONTROL;
    default:
        return 0u;
    }
}

bool pool_operation_request_safe_with_terminal(PoolOperationRequestKind k)
{
    /* Invariant 17: a retained terminal result (restore_required=false)
     * does not block unrelated manual pool / OTA / restart work. Protocol
     * reconcile is NOT in this set: it is Stratum-mutating owner-internal
     * work and is never available without a current lease token. */
    switch (k) {
    case OP_REQUEST_READ_ONLY:
    case OP_REQUEST_SESSION_ACKNOWLEDGE:
    case OP_REQUEST_MANUAL_POOL_PATCH:
    case OP_REQUEST_OTA_UPDATE:
    case OP_REQUEST_MANUAL_DEVICE_RESTART:
        return true;
    default:
        return false; /* notably TIMED_SESSION_START: blocked until ack */
    }
}

/* ------------------------------------------------------------------ */
/* State init and helpers                                              */
/* ------------------------------------------------------------------ */

void pool_operation_state_init(PoolOperationState *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->owner = OP_OWNER_NONE;
    s->phase = OP_PHASE_UNBOOTSTRAPPED; /* fail closed until bootstrap */
    s->last_status = OP_OK;
}

static void make_token(const PoolOperationState *s, PoolOperationLeaseToken *t)
{
    if (t == NULL) {
        return;
    }
    t->valid = (s->owner != OP_OWNER_NONE && s->lease_generation != 0u);
    t->owner = s->owner;
    t->lease_generation = s->lease_generation;
}

static void invalidate_token(PoolOperationLeaseToken *t)
{
    if (t != NULL) {
        t->valid = false;
        t->owner = OP_OWNER_NONE;
        t->lease_generation = 0u;
    }
}

/* Generation bump: never wraps; exhaustion is a fail-safe error. */
static bool bump_generation(PoolOperationState *s)
{
    if (s->lease_generation == UINT32_MAX) {
        return false;
    }
    s->lease_generation++;
    return true;
}

static PoolOperationStatus check_token(const PoolOperationState *s,
                                       const PoolOperationLeaseToken *t)
{
    if (t == NULL || !t->valid || t->lease_generation == 0u) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    if (s->owner == OP_OWNER_NONE) {
        return OP_ERR_NO_ACTIVE_SESSION;
    }
    /* Generation staleness dominates: a superseded token is STALE even when
     * its owner class no longer matches (e.g. after the Restore Now
     * ownership rotation). Only a CURRENT-generation token with the wrong
     * owner class is a genuine ownership mismatch. */
    if (t->lease_generation != s->lease_generation) {
        return OP_ERR_STALE_LEASE;
    }
    if (t->owner != s->owner) {
        return OP_ERR_NOT_OWNER;
    }
    return OP_OK;
}

static void enter_recovery_guard(PoolOperationState *s)
{
    /* The guard keeps the durable claim and every binding as evidence; the
     * generation advances (best effort — at UINT32_MAX it stays put, which
     * is still fail-closed because the guard denies normal mutation). */
    (void)bump_generation(s);
    s->owner = OP_OWNER_RECOVERY_GUARD;
    s->phase = OP_PHASE_RECOVERY_GUARD;
    s->durable_claim = true;
    s->resource_scopes = 0u; /* all mutating scopes blocked */
}

/* ------------------------------------------------------------------ */
/* Bootstrap (Stage 7)                                                 */
/* ------------------------------------------------------------------ */

PoolOperationStatus pool_operation_bootstrap(PoolOperationState *s,
                                             const PoolOperationBootstrapInput *in,
                                             PoolOperationLeaseToken *out_token)
{
    invalidate_token(out_token);
    if (s == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    if (s->bootstrapped) {
        return OP_ERR_ALREADY_BOOTSTRAPPED; /* state unchanged */
    }
    if (in == NULL || in->plan == NULL) {
        return OP_ERR_INVALID_ARGUMENT; /* remains UNBOOTSTRAPPED: fail closed */
    }

    pool_operation_state_init(s);
    s->bootstrapped = true;

    switch (in->store_result) {
    case STORE_EMPTY:
    case STORE_CLEARED:
        if (in->record_present) {
            enter_recovery_guard(s); /* contradictory input */
            s->last_status = OP_ERR_INTERNAL_CONSISTENCY;
            return OP_ERR_INTERNAL_CONSISTENCY;
        }
        s->owner = OP_OWNER_NONE;
        s->phase = OP_PHASE_FREE;
        s->last_status = OP_OK;
        return OP_OK;

    case STORE_OK:
        break; /* handled below */

    default:
        /* Every corrupt / unsupported / ambiguous / uncertain store result
         * reconstructs the guard — never a free coordinator, never slot or
         * generation guessing. */
        enter_recovery_guard(s);
        s->last_status = OP_ERR_RECOVERY_LOCKED;
        return OP_OK;
    }

    /* STORE_OK path. */
    if (!in->record_present || in->record == NULL ||
        in->record->kind != (uint8_t)POOL_RECORD_KIND_SESSION ||
        pool_session_record_validate(in->record) != RECORD_OK) {
        enter_recovery_guard(s);
        s->last_status = OP_ERR_INTERNAL_CONSISTENCY;
        return OP_ERR_INTERNAL_CONSISTENCY;
    }

    s->restore_required = in->record->restore_required;
    s->bound_session_id = in->record->session_id;
    s->bound_record_generation =
        (in->committed_generation != 0u) ? in->committed_generation
                                         : in->record->generation;
    s->persistence_required_before_action =
        in->plan->counters.must_persist_before_action ||
        in->plan->record_proposal.update_needed;

    switch (in->plan->decision) {
    case POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE:
        if (in->record->restore_required) {
            enter_recovery_guard(s); /* retain+obligation is contradictory */
            s->last_status = OP_ERR_INTERNAL_CONSISTENCY;
            return OP_ERR_INTERNAL_CONSISTENCY;
        }
        /* ONLY COMPLETE and a safe pre-mutation CANCELLED are retained
         * acknowledgeable terminals (the committed B1 contract). A
         * RECOVERY_REQUIRED record is NOT acknowledgeable — even with
         * restore_required=false — and must never enter the normal
         * acknowledgement/tombstone flow: it reconstructs the guard, keeps
         * all evidence, and only the bounded operator-recovery policy may
         * later transform it into a legitimately acknowledgeable state. */
        if (in->record->state == POOL_STATE_COMPLETE ||
            in->record->state == POOL_STATE_CANCELLED) {
            s->owner = OP_OWNER_NONE;
            s->phase = OP_PHASE_FREE;
            s->terminal_pending = true;
            s->last_status = OP_OK;
            return OP_OK;
        }
        enter_recovery_guard(s);
        s->lease_generation = 1u;
        s->last_status = OP_ERR_RECOVERY_LOCKED;
        return OP_OK;

    case POOL_BOOT_DECISION_BOOT_NORMAL_SOURCE:
        /* Pre-mutation snapshot-committed record: reconciliation (the B4
         * cancel proposal) must persist before manual work resumes. */
        s->owner = OP_OWNER_BOOT_RECOVERY;
        s->phase = OP_PHASE_RESERVED_PENDING_PERSISTENCE;
        s->durable_claim = true;
        s->resource_scopes = SESSION_SCOPES;
        s->lease_generation = 1u;
        make_token(s, out_token);
        s->last_status = OP_OK;
        return OP_OK;

    case POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME:
        s->owner = OP_OWNER_BOOT_RECOVERY;
        s->phase = OP_PHASE_WAITING_FOR_TRUSTED_TIME;
        s->durable_claim = true;
        s->resource_scopes = SESSION_SCOPES;
        s->lease_generation = 1u;
        make_token(s, out_token);
        s->last_status = OP_OK;
        return OP_OK;

    case POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET:
        /* Eligibility only: live verification pending; NO mining grant. */
        s->owner = OP_OWNER_BOOT_RECOVERY;
        s->phase = OP_PHASE_VERIFYING_TARGET;
        s->durable_claim = true;
        s->resource_scopes = SESSION_SCOPES;
        s->lease_generation = 1u;
        make_token(s, out_token);
        s->last_status = OP_OK;
        return OP_OK;

    case POOL_BOOT_DECISION_RESTORE_SOURCE_NOW:
    case POOL_BOOT_DECISION_VERIFY_RESTORE:
        s->owner = OP_OWNER_SOURCE_RESTORE;
        s->phase = OP_PHASE_RESTORING_SOURCE;
        s->durable_claim = true;
        s->resource_scopes = SESSION_SCOPES;
        s->lease_generation = 1u;
        make_token(s, out_token);
        s->last_status = OP_OK;
        return OP_OK;

    case POOL_BOOT_DECISION_RECOVERY_REQUIRED:
    default:
        enter_recovery_guard(s);
        s->lease_generation = 1u;
        s->last_status = OP_ERR_RECOVERY_LOCKED;
        return OP_OK;
    }
}

/* ------------------------------------------------------------------ */
/* Conflict matrix (Stage 8)                                           */
/* ------------------------------------------------------------------ */

static PoolOperationDecision deny(PoolOperationStatus status,
                                  PoolOperationOwner blocking, bool http_conflict)
{
    PoolOperationDecision d;
    memset(&d, 0, sizeof(d));
    d.allowed = false;
    d.status = status;
    d.blocking_owner = blocking;
    d.http_conflict = http_conflict;
    return d;
}

static PoolOperationDecision allow(void)
{
    PoolOperationDecision d;
    memset(&d, 0, sizeof(d));
    d.allowed = true;
    d.status = OP_OK;
    d.blocking_owner = OP_OWNER_NONE;
    d.http_conflict = false;
    return d;
}

PoolOperationDecision pool_operation_evaluate(const PoolOperationState *s,
                                              const PoolOperationRequest *req)
{
    if (s == NULL || req == NULL) {
        return deny(OP_ERR_INVALID_ARGUMENT, OP_OWNER_NONE, false);
    }
    if ((unsigned)req->kind >= (unsigned)OP_REQUEST__COUNT) {
        return deny(OP_ERR_UNSUPPORTED_REQUEST, s->owner, true); /* fail closed */
    }

    /* Read-only classification is always allowed and never mutates. */
    if (req->kind == OP_REQUEST_READ_ONLY) {
        return allow();
    }

    if (!s->bootstrapped || s->phase == OP_PHASE_UNBOOTSTRAPPED) {
        return deny(OP_ERR_BOOTSTRAP_REQUIRED, OP_OWNER_NONE, true);
    }

    /* Recovery guard: only operator recovery is admitted. */
    if (s->phase == OP_PHASE_RECOVERY_GUARD || s->owner == OP_OWNER_RECOVERY_GUARD ||
        s->owner == OP_OWNER_OPERATOR_RECOVERY) {
        if (req->kind == OP_REQUEST_OPERATOR_RECOVERY &&
            s->owner == OP_OWNER_RECOVERY_GUARD) {
            return allow();
        }
        return deny(OP_ERR_RECOVERY_LOCKED, s->owner, true);
    }

    switch (req->kind) {
    case OP_REQUEST_TIMED_SESSION_START:
        if (s->owner != OP_OWNER_NONE) {
            return deny(OP_ERR_BUSY, s->owner, true);
        }
        if (s->terminal_pending) {
            return deny(OP_ERR_TERMINAL_ACK_REQUIRED, OP_OWNER_NONE, true);
        }
        if (s->restore_required) {
            return deny(OP_ERR_RESTORE_REQUIRED, OP_OWNER_NONE, true);
        }
        if (req->session_id == 0u) {
            return deny(OP_ERR_INVALID_ARGUMENT, OP_OWNER_NONE, false);
        }
        return allow();

    case OP_REQUEST_TIMED_SESSION_INTERNAL: {
        PoolOperationStatus ts;
        if (!owner_is_session_class(s->owner)) {
            return deny(s->owner == OP_OWNER_NONE ? OP_ERR_NO_ACTIVE_SESSION
                                                  : OP_ERR_NOT_OWNER,
                        s->owner, true);
        }
        ts = check_token(s, &req->token);
        if (ts != OP_OK) {
            return deny(ts, s->owner, true);
        }
        return allow();
    }

    case OP_REQUEST_RESTORE_NOW:
        if (!owner_is_session_class(s->owner)) {
            return deny(OP_ERR_NO_ACTIVE_SESSION, s->owner, true);
        }
        if (req->session_id != s->bound_session_id) {
            return deny(OP_ERR_SESSION_MISMATCH, s->owner, true);
        }
        return allow();

    case OP_REQUEST_SESSION_ACKNOWLEDGE:
        if (s->owner != OP_OWNER_NONE) {
            return deny(OP_ERR_BUSY, s->owner, true);
        }
        if (!s->terminal_pending) {
            return deny(OP_ERR_NO_ACTIVE_SESSION, OP_OWNER_NONE, true);
        }
        if (s->restore_required) {
            return deny(OP_ERR_RESTORE_REQUIRED, OP_OWNER_NONE, true);
        }
        if (req->session_id != 0u && req->session_id != s->bound_session_id) {
            return deny(OP_ERR_SESSION_MISMATCH, OP_OWNER_NONE, true);
        }
        return allow();

    case OP_REQUEST_MANUAL_POOL_PATCH:
    case OP_REQUEST_OTA_UPDATE:
    case OP_REQUEST_MANUAL_DEVICE_RESTART:
        /* Invariant 17: a retained safe terminal result does not block
         * unrelated manual work; any active owner or obligation does. */
        if (s->owner != OP_OWNER_NONE) {
            return deny(OP_ERR_BUSY, s->owner, true);
        }
        if (s->restore_required) {
            return deny(OP_ERR_RESTORE_REQUIRED, OP_OWNER_NONE, true);
        }
        return allow();

    case OP_REQUEST_DESTRUCTIVE_MAINTENANCE:
        /* Reserved: always denied in B5; no bypass exists. */
        return deny(OP_ERR_UNSUPPORTED_REQUEST, s->owner, true);

    case OP_REQUEST_OPERATOR_RECOVERY:
        /* Admitted only through the recovery-guard branch above. */
        return deny(OP_ERR_NO_ACTIVE_SESSION, s->owner, true);

    case OP_REQUEST_PROTOCOL_RECONCILE: {
        /* Stratum-MUTATING work: never an unowned bypass. It is permitted
         * only as internal work of the CURRENT lease owner, with the
         * matching current token, STRATUM_CONTROL scope and a compatible
         * phase. There is no free-state reconcile path in B5 — a later
         * runtime gate must supply an explicit reconcile-lease acquisition
         * before ownerless protocol mutation becomes available. */
        PoolOperationStatus ts;
        if (s->owner == OP_OWNER_NONE) {
            return deny(OP_ERR_NOT_OWNER, OP_OWNER_NONE, true);
        }
        if (s->phase == OP_PHASE_WAITING_FOR_TRUSTED_TIME) {
            /* HOLD_STRATUM: target inhibition outranks reconcile work. */
            return deny(OP_ERR_BUSY, s->owner, true);
        }
        if (s->phase == OP_PHASE_RESERVED_PENDING_PERSISTENCE) {
            return deny(OP_ERR_PERSISTENCE_REQUIRED, s->owner, true);
        }
        if (s->phase == OP_PHASE_TERMINAL_ACK_PENDING) {
            return deny(OP_ERR_INVALID_TRANSITION, s->owner, true);
        }
        if ((s->resource_scopes & OP_SCOPE_STRATUM_CONTROL) == 0u) {
            /* e.g. OTA or acknowledgement leases own no Stratum scope. */
            return deny(OP_ERR_NOT_OWNER, s->owner, true);
        }
        ts = check_token(s, &req->token);
        if (ts != OP_OK) {
            return deny(ts, s->owner, true);
        }
        return allow(); /* compatible internal reconcile of the owner itself */
    }

    default:
        return deny(OP_ERR_UNSUPPORTED_REQUEST, s->owner, true);
    }
}

/* ------------------------------------------------------------------ */
/* Acquisition (Stage 9)                                               */
/* ------------------------------------------------------------------ */

PoolOperationStatus pool_operation_acquire(PoolOperationState *s,
                                           const PoolOperationRequest *req,
                                           PoolOperationLeaseToken *out_token,
                                           PoolOperationDecision *out_decision)
{
    PoolOperationDecision d;
    PoolOperationRequest local_req;

    /* Alias-safe: copy the request before touching any output storage. */
    if (req != NULL) {
        local_req = *req;
        req = &local_req;
    }
    invalidate_token(out_token);
    if (out_decision != NULL) {
        memset(out_decision, 0, sizeof(*out_decision));
    }
    if (s == NULL || req == NULL) {
        if (out_decision != NULL) {
            *out_decision = deny(OP_ERR_INVALID_ARGUMENT, OP_OWNER_NONE, false);
        }
        return OP_ERR_INVALID_ARGUMENT;
    }

    d = pool_operation_evaluate(s, req);
    if (out_decision != NULL) {
        *out_decision = d;
    }
    if (!d.allowed) {
        return d.status; /* a denied request changes no state */
    }

    switch (req->kind) {
    case OP_REQUEST_READ_ONLY:
    case OP_REQUEST_TIMED_SESSION_INTERNAL:
    case OP_REQUEST_MANUAL_DEVICE_RESTART:
    case OP_REQUEST_PROTOCOL_RECONCILE:
        /* Allowed without creating a NEW lease; no state changes. The
         * internal request keeps using its existing verified token. */
        return OP_OK;

    case OP_REQUEST_RESTORE_NOW:
        return pool_operation_restore_now(s, req->session_id, out_token);

    case OP_REQUEST_TIMED_SESSION_START:
        if (!bump_generation(s)) {
            if (out_decision != NULL) {
                *out_decision = deny(OP_ERR_GENERATION_EXHAUSTED, OP_OWNER_NONE, true);
            }
            return OP_ERR_GENERATION_EXHAUSTED;
        }
        s->owner = OP_OWNER_TIMED_SESSION;
        s->phase = OP_PHASE_RESERVED_PENDING_PERSISTENCE;
        s->bound_session_id = req->session_id;
        s->bound_record_generation = 0u;
        s->resource_scopes = SESSION_SCOPES;
        s->persistence_required_before_action = true; /* Stage-10 barrier */
        s->durable_claim = false; /* durable only after the committed proof */
        make_token(s, out_token);
        return OP_OK;

    case OP_REQUEST_SESSION_ACKNOWLEDGE:
        if (!bump_generation(s)) {
            return OP_ERR_GENERATION_EXHAUSTED;
        }
        s->owner = OP_OWNER_SESSION_ACKNOWLEDGE;
        s->phase = OP_PHASE_ACTIVE;
        s->resource_scopes = OP_SCOPE_SESSION_STORE;
        s->persistence_required_before_action = true; /* tombstone commit */
        make_token(s, out_token);
        return OP_OK;

    case OP_REQUEST_MANUAL_POOL_PATCH:
        if (!bump_generation(s)) {
            return OP_ERR_GENERATION_EXHAUSTED;
        }
        s->owner = OP_OWNER_MANUAL_POOL_PATCH;
        s->phase = OP_PHASE_ACTIVE;
        s->resource_scopes = pool_operation_request_scope(req->kind);
        s->persistence_required_before_action = false;
        make_token(s, out_token);
        return OP_OK;

    case OP_REQUEST_OTA_UPDATE:
        if (!bump_generation(s)) {
            return OP_ERR_GENERATION_EXHAUSTED;
        }
        s->owner = OP_OWNER_OTA_UPDATE;
        s->phase = OP_PHASE_ACTIVE;
        s->resource_scopes = pool_operation_request_scope(req->kind);
        s->persistence_required_before_action = false;
        make_token(s, out_token);
        return OP_OK;

    case OP_REQUEST_OPERATOR_RECOVERY:
        if (!bump_generation(s)) {
            return OP_ERR_GENERATION_EXHAUSTED;
        }
        /* Admits the operator INTO the guard: the guard evidence, durable
         * claim and bindings all remain; nothing is cleared or freed. */
        s->owner = OP_OWNER_OPERATOR_RECOVERY;
        s->phase = OP_PHASE_RECOVERY_GUARD;
        s->resource_scopes = OP_SCOPE_SESSION_STORE;
        make_token(s, out_token);
        return OP_OK;

    default:
        return OP_ERR_UNSUPPORTED_REQUEST;
    }
}

/* ------------------------------------------------------------------ */
/* Restore Now (Stage 12)                                              */
/* ------------------------------------------------------------------ */

PoolOperationStatus pool_operation_restore_now(PoolOperationState *s,
                                               uint32_t session_id,
                                               PoolOperationLeaseToken *out_token)
{
    invalidate_token(out_token);
    if (s == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    if (!s->bootstrapped) {
        return OP_ERR_BOOTSTRAP_REQUIRED;
    }
    if (!owner_is_session_class(s->owner)) {
        return OP_ERR_NO_ACTIVE_SESSION;
    }
    if (session_id != s->bound_session_id) {
        return OP_ERR_SESSION_MISMATCH;
    }
    if (s->owner == OP_OWNER_SOURCE_RESTORE && s->phase == OP_PHASE_RESTORING_SOURCE) {
        /* Idempotent repeat: same ownership, same generation, same token. */
        make_token(s, out_token);
        return OP_OK;
    }
    if (!bump_generation(s)) {
        return OP_ERR_GENERATION_EXHAUSTED;
    }
    /* The SAME exclusive logical session lease transitions toward restore;
     * the generation bump makes every stale target-phase token invalid. */
    s->owner = OP_OWNER_SOURCE_RESTORE;
    s->phase = OP_PHASE_RESTORING_SOURCE;
    s->resource_scopes = SESSION_SCOPES;
    s->durable_claim = true;
    make_token(s, out_token);
    return OP_OK;
}

/* ------------------------------------------------------------------ */
/* Phase transitions (Stage 11)                                        */
/* ------------------------------------------------------------------ */

static bool phase_transition_legal(PoolOperationLeasePhase from,
                                   PoolOperationLeasePhase to)
{
    if (to == OP_PHASE_RECOVERY_GUARD) {
        return true; /* uncertainty escalation is always legal when owned */
    }
    switch (from) {
    case OP_PHASE_ACTIVE:
        return to == OP_PHASE_WAITING_FOR_TRUSTED_TIME ||
               to == OP_PHASE_VERIFYING_TARGET || to == OP_PHASE_RESTORING_SOURCE;
    case OP_PHASE_WAITING_FOR_TRUSTED_TIME:
        return to == OP_PHASE_VERIFYING_TARGET || to == OP_PHASE_RESTORING_SOURCE;
    case OP_PHASE_VERIFYING_TARGET:
        return to == OP_PHASE_ACTIVE || to == OP_PHASE_RESTORING_SOURCE;
    case OP_PHASE_RESERVED_PENDING_PERSISTENCE:
        return to == OP_PHASE_RESTORING_SOURCE; /* cancel-like path */
    default:
        return false;
    }
}

PoolOperationStatus pool_operation_transition_phase(PoolOperationState *s,
                                                    const PoolOperationLeaseToken *token,
                                                    PoolOperationLeasePhase target,
                                                    PoolOperationLeaseToken *out_token)
{
    PoolOperationStatus ts;
    PoolOperationLeaseToken in_tok;

    /* Alias-safe: callers may rotate a token in place (token == out_token). */
    if (token != NULL) {
        in_tok = *token;
        token = &in_tok;
    }
    invalidate_token(out_token);
    if (s == NULL || (unsigned)target >= (unsigned)OP_PHASE__COUNT) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    ts = check_token(s, token);
    if (ts != OP_OK) {
        return ts; /* stale/mismatched tokens change nothing */
    }
    if (!owner_is_session_class(s->owner)) {
        return OP_ERR_INVALID_TRANSITION; /* manual/ack leases have no phases */
    }
    if (target == OP_PHASE_ACTIVE && s->phase == OP_PHASE_RESERVED_PENDING_PERSISTENCE) {
        return OP_ERR_PERSISTENCE_REQUIRED; /* only a proof activates */
    }
    if (!phase_transition_legal(s->phase, target)) {
        return OP_ERR_INVALID_TRANSITION;
    }
    if (target == OP_PHASE_RECOVERY_GUARD) {
        enter_recovery_guard(s);
        return OP_OK; /* guard holds no caller token */
    }
    if (!bump_generation(s)) {
        return OP_ERR_GENERATION_EXHAUSTED;
    }
    if (target == OP_PHASE_RESTORING_SOURCE) {
        s->owner = OP_OWNER_SOURCE_RESTORE;
        s->durable_claim = true;
    }
    s->phase = target;
    make_token(s, out_token);
    return OP_OK;
}

/* ------------------------------------------------------------------ */
/* Persistence proofs (Stage 10)                                       */
/* ------------------------------------------------------------------ */

PoolOperationStatus pool_operation_apply_persistence_proof(
    PoolOperationState *s, const PoolOperationLeaseToken *token,
    const PoolOperationPersistenceProof *proof, PoolOperationLeaseToken *out_token)
{
    PoolOperationStatus ts;
    PoolOperationLeaseToken in_tok;

    /* Alias-safe: callers may rotate a token in place (token == out_token). */
    if (token != NULL) {
        in_tok = *token;
        token = &in_tok;
    }
    invalidate_token(out_token);
    if (s == NULL || proof == NULL ||
        (unsigned)proof->kind >= (unsigned)OP_PROOF__COUNT || proof->kind == 0u) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    ts = check_token(s, token);
    if (ts != OP_OK) {
        return ts;
    }

    /* An uncertain commit NEVER activates or releases anything: guard. */
    if (proof->store_result == STORE_COMMIT_UNCERTAIN) {
        enter_recovery_guard(s);
        return OP_ERR_PERSISTENCE_UNCERTAIN;
    }

    switch (proof->kind) {
    case OP_PROOF_SESSION_COMMITTED:
        if (s->phase != OP_PHASE_RESERVED_PENDING_PERSISTENCE ||
            !(s->owner == OP_OWNER_TIMED_SESSION || s->owner == OP_OWNER_BOOT_RECOVERY)) {
            return OP_ERR_INVALID_TRANSITION;
        }
        if (proof->store_result != STORE_OK ||
            proof->session_id != s->bound_session_id ||
            proof->committed_record_generation == 0u ||
            !pool_state_is_persistent(proof->persisted_state)) {
            return OP_ERR_PERSISTENCE_MISMATCH; /* stays reserved */
        }
        if (!bump_generation(s)) {
            return OP_ERR_GENERATION_EXHAUSTED;
        }
        s->phase = OP_PHASE_ACTIVE;
        s->bound_record_generation = proof->committed_record_generation;
        s->persistence_required_before_action = false;
        s->durable_claim = true;
        s->restore_required = proof->restore_required;
        make_token(s, out_token);
        return OP_OK;

    case OP_PROOF_RECOVERY_UPDATE_COMMITTED:
        if (!owner_is_session_class(s->owner)) {
            return OP_ERR_INVALID_TRANSITION;
        }
        if (proof->store_result != STORE_OK ||
            proof->session_id != s->bound_session_id) {
            return OP_ERR_PERSISTENCE_MISMATCH;
        }
        if (!bump_generation(s)) {
            return OP_ERR_GENERATION_EXHAUSTED;
        }
        s->bound_record_generation = proof->committed_record_generation;
        s->persistence_required_before_action = false;
        s->restore_required = proof->restore_required;
        make_token(s, out_token);
        return OP_OK;

    case OP_PROOF_TERMINAL_COMMITTED:
        if (!owner_is_session_class(s->owner)) {
            return OP_ERR_INVALID_TRANSITION;
        }
        /* Only COMPLETE and safe pre-mutation CANCELLED may enter the
         * acknowledgeable-terminal flow; RECOVERY_REQUIRED is never
         * acknowledgeable (owner-directed B5 correction). */
        if (proof->store_result != STORE_OK ||
            proof->session_id != s->bound_session_id || proof->restore_required ||
            !(proof->persisted_state == POOL_STATE_COMPLETE ||
              proof->persisted_state == POOL_STATE_CANCELLED)) {
            return OP_ERR_PERSISTENCE_MISMATCH;
        }
        if (!bump_generation(s)) {
            return OP_ERR_GENERATION_EXHAUSTED;
        }
        s->phase = OP_PHASE_TERMINAL_ACK_PENDING;
        s->bound_record_generation = proof->committed_record_generation;
        s->restore_required = false; /* proven discharged by the record */
        s->persistence_required_before_action = false;
        make_token(s, out_token);
        return OP_OK;

    case OP_PROOF_CLEARED:
    default:
        return OP_ERR_INVALID_TRANSITION; /* CLEARED flows through release_ack */
    }
}

/* ------------------------------------------------------------------ */
/* Releases (Stage 13/14)                                              */
/* ------------------------------------------------------------------ */

PoolOperationStatus pool_operation_release_session(PoolOperationState *s,
                                                   const PoolOperationLeaseToken *token)
{
    PoolOperationStatus ts;

    if (s == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    ts = check_token(s, token);
    if (ts != OP_OK) {
        return ts;
    }
    if (!owner_is_session_class(s->owner)) {
        return OP_ERR_NOT_OWNER;
    }
    if (s->phase != OP_PHASE_TERMINAL_ACK_PENDING) {
        /* No durable terminal proof: the lease cannot dissolve. */
        return OP_ERR_UNSAFE_RELEASE;
    }
    if (!bump_generation(s)) {
        return OP_ERR_GENERATION_EXHAUSTED;
    }
    /* Ownership frees; the terminal RESULT stays retained and keeps
     * blocking new timed sessions until acknowledgement. */
    s->owner = OP_OWNER_NONE;
    s->phase = OP_PHASE_FREE;
    s->terminal_pending = true;
    s->durable_claim = false;
    s->resource_scopes = 0u;
    s->persistence_required_before_action = false;
    return OP_OK;
}

PoolOperationStatus pool_operation_release_ack(PoolOperationState *s,
                                               const PoolOperationLeaseToken *token,
                                               const PoolOperationPersistenceProof *proof)
{
    PoolOperationStatus ts;

    if (s == NULL || proof == NULL) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    ts = check_token(s, token);
    if (ts != OP_OK) {
        return ts;
    }
    if (s->owner != OP_OWNER_SESSION_ACKNOWLEDGE) {
        return OP_ERR_NOT_OWNER;
    }
    if (proof->store_result == STORE_COMMIT_UNCERTAIN) {
        /* Never guess an acknowledgement succeeded. */
        enter_recovery_guard(s);
        return OP_ERR_PERSISTENCE_UNCERTAIN;
    }
    if (proof->kind != OP_PROOF_CLEARED || proof->store_result != STORE_CLEARED) {
        return OP_ERR_PERSISTENCE_MISMATCH; /* terminal_pending stays true */
    }
    if (!bump_generation(s)) {
        return OP_ERR_GENERATION_EXHAUSTED;
    }
    s->owner = OP_OWNER_NONE;
    s->phase = OP_PHASE_FREE;
    s->terminal_pending = false; /* a new session becomes eligible */
    s->bound_session_id = 0u;
    s->bound_record_generation = 0u;
    s->resource_scopes = 0u;
    s->persistence_required_before_action = false;
    return OP_OK;
}

PoolOperationStatus pool_operation_release_manual(PoolOperationState *s,
                                                  const PoolOperationLeaseToken *token,
                                                  PoolOperationOutcome outcome)
{
    PoolOperationStatus ts;

    if (s == NULL || (unsigned)outcome >= (unsigned)OP_OUTCOME__COUNT || outcome == 0u) {
        return OP_ERR_INVALID_ARGUMENT;
    }
    ts = check_token(s, token);
    if (ts != OP_OK) {
        return ts;
    }
    if (s->owner == OP_OWNER_OPERATOR_RECOVERY) {
        /* Operator work returns INTO the guard: evidence is never freed. */
        enter_recovery_guard(s);
        return OP_OK;
    }
    if (s->owner != OP_OWNER_MANUAL_POOL_PATCH && s->owner != OP_OWNER_OTA_UPDATE) {
        return OP_ERR_UNSAFE_RELEASE; /* session leases need durable proof */
    }
    if (outcome == OP_OUTCOME_MUTATION_UNCERTAIN) {
        enter_recovery_guard(s);
        return OP_ERR_PERSISTENCE_UNCERTAIN;
    }
    if (!bump_generation(s)) {
        return OP_ERR_GENERATION_EXHAUSTED;
    }
    s->owner = OP_OWNER_NONE;
    s->phase = OP_PHASE_FREE;
    s->resource_scopes = 0u;
    s->persistence_required_before_action = false;
    return OP_OK;
}

/* ------------------------------------------------------------------ */
/* Stable machine tokens                                               */
/* ------------------------------------------------------------------ */

const char *pool_operation_status_str(PoolOperationStatus st)
{
    switch (st) {
    case OP_OK:                       return "OP_OK";
    case OP_ERR_INVALID_ARGUMENT:     return "OP_ERR_INVALID_ARGUMENT";
    case OP_ERR_NOT_INITIALIZED:      return "OP_ERR_NOT_INITIALIZED";
    case OP_ERR_BOOTSTRAP_REQUIRED:   return "OP_ERR_BOOTSTRAP_REQUIRED";
    case OP_ERR_ALREADY_BOOTSTRAPPED: return "OP_ERR_ALREADY_BOOTSTRAPPED";
    case OP_ERR_BUSY:                 return "OP_ERR_BUSY";
    case OP_ERR_RECOVERY_LOCKED:      return "OP_ERR_RECOVERY_LOCKED";
    case OP_ERR_TERMINAL_ACK_REQUIRED:return "OP_ERR_TERMINAL_ACK_REQUIRED";
    case OP_ERR_NO_ACTIVE_SESSION:    return "OP_ERR_NO_ACTIVE_SESSION";
    case OP_ERR_SESSION_MISMATCH:     return "OP_ERR_SESSION_MISMATCH";
    case OP_ERR_NOT_OWNER:            return "OP_ERR_NOT_OWNER";
    case OP_ERR_STALE_LEASE:          return "OP_ERR_STALE_LEASE";
    case OP_ERR_INVALID_TRANSITION:   return "OP_ERR_INVALID_TRANSITION";
    case OP_ERR_PERSISTENCE_REQUIRED: return "OP_ERR_PERSISTENCE_REQUIRED";
    case OP_ERR_PERSISTENCE_MISMATCH: return "OP_ERR_PERSISTENCE_MISMATCH";
    case OP_ERR_PERSISTENCE_UNCERTAIN:return "OP_ERR_PERSISTENCE_UNCERTAIN";
    case OP_ERR_UNSAFE_RELEASE:       return "OP_ERR_UNSAFE_RELEASE";
    case OP_ERR_RESTORE_REQUIRED:     return "OP_ERR_RESTORE_REQUIRED";
    case OP_ERR_GENERATION_EXHAUSTED: return "OP_ERR_GENERATION_EXHAUSTED";
    case OP_ERR_UNSUPPORTED_REQUEST:  return "OP_ERR_UNSUPPORTED_REQUEST";
    case OP_ERR_INTERNAL_CONSISTENCY: return "OP_ERR_INTERNAL_CONSISTENCY";
    default:                          return "OP_ERR_UNKNOWN";
    }
}

const char *pool_operation_owner_str(PoolOperationOwner o)
{
    switch (o) {
    case OP_OWNER_NONE:                return "OWNER_NONE";
    case OP_OWNER_TIMED_SESSION:       return "OWNER_TIMED_SESSION";
    case OP_OWNER_BOOT_RECOVERY:       return "OWNER_BOOT_RECOVERY";
    case OP_OWNER_SOURCE_RESTORE:      return "OWNER_SOURCE_RESTORE";
    case OP_OWNER_MANUAL_POOL_PATCH:   return "OWNER_MANUAL_POOL_PATCH";
    case OP_OWNER_OTA_UPDATE:          return "OWNER_OTA_UPDATE";
    case OP_OWNER_SESSION_ACKNOWLEDGE: return "OWNER_SESSION_ACKNOWLEDGE";
    case OP_OWNER_OPERATOR_RECOVERY:   return "OWNER_OPERATOR_RECOVERY";
    case OP_OWNER_RECOVERY_GUARD:      return "OWNER_RECOVERY_GUARD";
    default:                           return "OWNER_UNKNOWN";
    }
}

const char *pool_operation_phase_str(PoolOperationLeasePhase p)
{
    switch (p) {
    case OP_PHASE_UNBOOTSTRAPPED:               return "PHASE_UNBOOTSTRAPPED";
    case OP_PHASE_FREE:                         return "PHASE_FREE";
    case OP_PHASE_RESERVED_PENDING_PERSISTENCE: return "PHASE_RESERVED_PENDING_PERSISTENCE";
    case OP_PHASE_ACTIVE:                       return "PHASE_ACTIVE";
    case OP_PHASE_WAITING_FOR_TRUSTED_TIME:     return "PHASE_WAITING_FOR_TRUSTED_TIME";
    case OP_PHASE_VERIFYING_TARGET:             return "PHASE_VERIFYING_TARGET";
    case OP_PHASE_RESTORING_SOURCE:             return "PHASE_RESTORING_SOURCE";
    case OP_PHASE_TERMINAL_ACK_PENDING:         return "PHASE_TERMINAL_ACK_PENDING";
    case OP_PHASE_RELEASING:                    return "PHASE_RELEASING";
    case OP_PHASE_RECOVERY_GUARD:               return "PHASE_RECOVERY_GUARD";
    default:                                    return "PHASE_UNKNOWN";
    }
}

const char *pool_operation_request_str(PoolOperationRequestKind k)
{
    switch (k) {
    case OP_REQUEST_READ_ONLY:               return "REQUEST_READ_ONLY";
    case OP_REQUEST_TIMED_SESSION_START:     return "REQUEST_TIMED_SESSION_START";
    case OP_REQUEST_TIMED_SESSION_INTERNAL:  return "REQUEST_TIMED_SESSION_INTERNAL";
    case OP_REQUEST_RESTORE_NOW:             return "REQUEST_RESTORE_NOW";
    case OP_REQUEST_SESSION_ACKNOWLEDGE:     return "REQUEST_SESSION_ACKNOWLEDGE";
    case OP_REQUEST_MANUAL_POOL_PATCH:       return "REQUEST_MANUAL_POOL_PATCH";
    case OP_REQUEST_OTA_UPDATE:              return "REQUEST_OTA_UPDATE";
    case OP_REQUEST_MANUAL_DEVICE_RESTART:   return "REQUEST_MANUAL_DEVICE_RESTART";
    case OP_REQUEST_DESTRUCTIVE_MAINTENANCE: return "REQUEST_DESTRUCTIVE_MAINTENANCE";
    case OP_REQUEST_OPERATOR_RECOVERY:       return "REQUEST_OPERATOR_RECOVERY";
    case OP_REQUEST_PROTOCOL_RECONCILE:      return "REQUEST_PROTOCOL_RECONCILE";
    default:                                 return "REQUEST_UNKNOWN";
    }
}
