/*
 * NeuralAxe timed pool sessions — PURE controlled-execution domain
 * (Phase 2M.1B, Gate B7). See pool_session_execution_core.h for the
 * contracts. Everything here is deterministic, total and side-effect free.
 */

#include <string.h>
#include "pool_session_execution_core.h"

/* ------------------------------------------------------------------ */
/* Small total helpers                                                 */
/* ------------------------------------------------------------------ */

static bool enum_in_range(unsigned v, unsigned count)
{
    return v < count;
}

static bool str_bounded(const char *s, size_t cap)
{
    return s != NULL && memchr(s, '\0', cap) != NULL;
}

/* ------------------------------------------------------------------ */
/* Feature posture                                                     */
/* ------------------------------------------------------------------ */

PoolExecBootAction pool_exec_boot_action_for_features(bool sessions_enabled,
                                                      bool execution_enabled)
{
    PoolExecBootAction a;
    /* Execution NEVER exists without the base feature: the Kconfig
     * dependency makes the sessions=n/execution=y combination unbuildable,
     * and this pure rule still fails closed for it. */
    a.executor_permitted = sessions_enabled && execution_enabled;
    a.actions_permitted  = sessions_enabled && execution_enabled;
    return a;
}

/* ------------------------------------------------------------------ */
/* Substate contract map                                               */
/* ------------------------------------------------------------------ */

PoolExecActionClass pool_exec_state_action(PoolExecState s)
{
    switch (s) {
    case EXEC_STATE_DISABLED:
    case EXEC_STATE_IDLE:
    case EXEC_STATE_DONE:
    case EXEC_STATE_HANDOFF_FAILED:
    case EXEC_STATE_RESTORE_FAILED_HELD:
    case EXEC_STATE_RECOVERY_GUARD:
    case EXEC_STATE_ERROR:
    case EXEC_STATE_ENTRY_PENDING:
        return EXEC_ACTION_NONE;
    case EXEC_STATE_TARGET_READBACK:
        return EXEC_ACTION_CONFIG_READ;
    case EXEC_STATE_TARGET_APPLYING:
    case EXEC_STATE_SOURCE_APPLYING:
        return EXEC_ACTION_CONFIG_WRITE;
    case EXEC_STATE_TARGET_CONFIG_VERIFIED:
    case EXEC_STATE_SOURCE_CONFIG_VERIFIED:
        return EXEC_ACTION_PROTOCOL_START;
    case EXEC_STATE_TARGET_CONNECTING:
    case EXEC_STATE_SOURCE_CONNECTING:
    case EXEC_STATE_TARGET_PROTOCOL_VERIFIED:
    case EXEC_STATE_SOURCE_PROTOCOL_VERIFIED:
    case EXEC_STATE_TARGET_MINING:
    case EXEC_STATE_SOURCE_MINING_VERIFYING:
        return EXEC_ACTION_MONITOR;
    case EXEC_STATE_RESTORE_PENDING:
        return EXEC_ACTION_PROTOCOL_STOP;
    case EXEC_STATE_COMPLETE_HANDOFF:
        return EXEC_ACTION_HANDOFF;
    default:
        return EXEC_ACTION_NONE; /* fail closed */
    }
}

PoolExecProtoPosture pool_exec_state_proto_posture(PoolExecState s)
{
    switch (s) {
    case EXEC_STATE_DISABLED:
    case EXEC_STATE_IDLE:
    case EXEC_STATE_DONE:
        return EXEC_PROTO_POSTURE_ANY; /* production/source posture applies */
    case EXEC_STATE_ENTRY_PENDING:
    case EXEC_STATE_TARGET_READBACK:
    case EXEC_STATE_TARGET_APPLYING:
    case EXEC_STATE_TARGET_CONFIG_VERIFIED:
    case EXEC_STATE_RESTORE_PENDING:
    case EXEC_STATE_SOURCE_APPLYING:
    case EXEC_STATE_SOURCE_CONFIG_VERIFIED:
    case EXEC_STATE_RESTORE_FAILED_HELD:
    case EXEC_STATE_RECOVERY_GUARD:
    case EXEC_STATE_ERROR:
        return EXEC_PROTO_POSTURE_STOPPED;
    case EXEC_STATE_HANDOFF_FAILED:
        /* Truthful: the controlled engine may or may not have exited, so no
         * posture is claimed — and no work is delivered either way. */
        return EXEC_PROTO_POSTURE_ANY;
    case EXEC_STATE_TARGET_CONNECTING:
    case EXEC_STATE_SOURCE_CONNECTING:
    case EXEC_STATE_TARGET_PROTOCOL_VERIFIED:
        return EXEC_PROTO_POSTURE_VERIFICATION;
    case EXEC_STATE_SOURCE_PROTOCOL_VERIFIED:
    case EXEC_STATE_SOURCE_MINING_VERIFYING:
    case EXEC_STATE_TARGET_MINING:
        return EXEC_PROTO_POSTURE_CONNECTED;
    case EXEC_STATE_COMPLETE_HANDOFF:
        /* Stopping the controlled engine, then starting the production
         * coordinator: no stable posture is claimed during the handover. */
        return EXEC_PROTO_POSTURE_ANY;
    default:
        return EXEC_PROTO_POSTURE_STOPPED; /* fail closed */
    }
}

PoolExecGatePosture pool_exec_state_gate(PoolExecState s)
{
    switch (s) {
    case EXEC_STATE_DISABLED:
    case EXEC_STATE_IDLE:
    case EXEC_STATE_DONE:
        return EXEC_GATE_DEFAULT_OPEN; /* no execution epoch */
    case EXEC_STATE_TARGET_MINING:
        return EXEC_GATE_OPEN_TARGET;
    case EXEC_STATE_SOURCE_PROTOCOL_VERIFIED:
    case EXEC_STATE_SOURCE_MINING_VERIFYING:
        return EXEC_GATE_OPEN_SOURCE_RESTORED;
    case EXEC_STATE_COMPLETE_HANDOFF:
    case EXEC_STATE_HANDOFF_FAILED:
        /* The controlled engine is being (or failed to be) torn down: no
         * work is delivered until the production coordinator owns mining. */
        return EXEC_GATE_INHIBITED;
    case EXEC_STATE_ENTRY_PENDING:
    case EXEC_STATE_TARGET_READBACK:
    case EXEC_STATE_TARGET_APPLYING:
    case EXEC_STATE_TARGET_CONFIG_VERIFIED:
    case EXEC_STATE_TARGET_CONNECTING:
    case EXEC_STATE_TARGET_PROTOCOL_VERIFIED:
    case EXEC_STATE_RESTORE_PENDING:
    case EXEC_STATE_SOURCE_APPLYING:
    case EXEC_STATE_SOURCE_CONFIG_VERIFIED:
    case EXEC_STATE_SOURCE_CONNECTING:
    case EXEC_STATE_RESTORE_FAILED_HELD:
    case EXEC_STATE_RECOVERY_GUARD:
    case EXEC_STATE_ERROR:
        return EXEC_GATE_INHIBITED;
    default:
        return EXEC_GATE_INHIBITED; /* fail closed */
    }
}

PoolExecOwnerReq pool_exec_state_owner_req(PoolExecState s)
{
    switch (s) {
    case EXEC_STATE_DISABLED:
    case EXEC_STATE_IDLE:
    case EXEC_STATE_DONE:
        return EXEC_OWNER_REQ_NONE;
    case EXEC_STATE_ENTRY_PENDING:
        /* Entry validation dispatches BOTH sides; it needs only an owned
         * lease here — the precise phase routing happens at dispatch. */
        return EXEC_OWNER_REQ_ANY_OWNED;
    case EXEC_STATE_TARGET_READBACK:
    case EXEC_STATE_TARGET_APPLYING:
    case EXEC_STATE_TARGET_CONFIG_VERIFIED:
    case EXEC_STATE_TARGET_CONNECTING:
    case EXEC_STATE_TARGET_PROTOCOL_VERIFIED:
    case EXEC_STATE_TARGET_MINING:
        return EXEC_OWNER_REQ_SESSION;
    case EXEC_STATE_RESTORE_PENDING:
    case EXEC_STATE_SOURCE_APPLYING:
    case EXEC_STATE_SOURCE_CONFIG_VERIFIED:
    case EXEC_STATE_SOURCE_CONNECTING:
    case EXEC_STATE_SOURCE_PROTOCOL_VERIFIED:
    case EXEC_STATE_SOURCE_MINING_VERIFYING:
    case EXEC_STATE_COMPLETE_HANDOFF:
    case EXEC_STATE_HANDOFF_FAILED:
    case EXEC_STATE_RESTORE_FAILED_HELD:
        return EXEC_OWNER_REQ_RESTORE_SIDE;
    case EXEC_STATE_RECOVERY_GUARD:
    case EXEC_STATE_ERROR:
        return EXEC_OWNER_REQ_ANY_OWNED;
    default:
        return EXEC_OWNER_REQ_ANY_OWNED; /* fail closed */
    }
}

bool pool_exec_state_b1_compatible(PoolExecState s, PoolSessionState b1)
{
    if (!enum_in_range((unsigned)b1, POOL_STATE__COUNT)) {
        return false;
    }
    switch (s) {
    case EXEC_STATE_DISABLED:
    case EXEC_STATE_IDLE:
        return true; /* no session interpretation is made */
    case EXEC_STATE_ENTRY_PENDING:
        /* any active or restore-owing persisted posture may be entered */
        return pool_state_is_active_session(b1) ||
               b1 == POOL_STATE_RESTORE_FAILED ||
               b1 == POOL_STATE_RECOVERY_REQUIRED;
    case EXEC_STATE_TARGET_READBACK:
    case EXEC_STATE_TARGET_CONNECTING:
    case EXEC_STATE_TARGET_PROTOCOL_VERIFIED:
        return b1 == POOL_STATE_VERIFYING_TARGET;
    case EXEC_STATE_TARGET_APPLYING:
        return b1 == POOL_STATE_APPLYING_TARGET;
    case EXEC_STATE_TARGET_CONFIG_VERIFIED:
        /* the RAM activation boundary between apply and verification */
        return b1 == POOL_STATE_RESTARTING_FOR_TARGET ||
               b1 == POOL_STATE_VERIFYING_TARGET;
    case EXEC_STATE_TARGET_MINING:
        return b1 == POOL_STATE_TARGET_ACTIVE;
    case EXEC_STATE_RESTORE_PENDING:
        return b1 == POOL_STATE_RESTORE_DUE || b1 == POOL_STATE_TARGET_FAILED ||
               b1 == POOL_STATE_INTERRUPTED || b1 == POOL_STATE_TARGET_ACTIVE;
    case EXEC_STATE_SOURCE_APPLYING:
        return b1 == POOL_STATE_APPLYING_RESTORE;
    case EXEC_STATE_SOURCE_CONFIG_VERIFIED:
        return b1 == POOL_STATE_RESTARTING_FOR_RESTORE ||
               b1 == POOL_STATE_VERIFYING_RESTORE;
    case EXEC_STATE_SOURCE_CONNECTING:
    case EXEC_STATE_SOURCE_PROTOCOL_VERIFIED:
    case EXEC_STATE_SOURCE_MINING_VERIFYING:
        return b1 == POOL_STATE_VERIFYING_RESTORE;
    case EXEC_STATE_COMPLETE_HANDOFF:
    case EXEC_STATE_HANDOFF_FAILED:
    case EXEC_STATE_DONE:
        return pool_state_is_terminal(b1);
    case EXEC_STATE_RESTORE_FAILED_HELD:
        return b1 == POOL_STATE_RESTORE_FAILED;
    case EXEC_STATE_RECOVERY_GUARD:
    case EXEC_STATE_ERROR:
        return true; /* the guard may sit over any committed truth */
    default:
        return false; /* fail closed */
    }
}

static bool owner_is_session_class(PoolOperationOwner o)
{
    return o == OP_OWNER_TIMED_SESSION || o == OP_OWNER_BOOT_RECOVERY;
}

bool pool_exec_state_ownership_compatible(PoolExecState s,
                                          PoolOperationOwner owner,
                                          PoolOperationLeasePhase phase)
{
    if (!enum_in_range((unsigned)owner, OP_OWNER__COUNT) ||
        !enum_in_range((unsigned)phase, OP_PHASE__COUNT)) {
        return false;
    }
    switch (pool_exec_state_owner_req(s)) {
    case EXEC_OWNER_REQ_NONE:
        return true;
    case EXEC_OWNER_REQ_SESSION:
        if (!owner_is_session_class(owner)) {
            return false;
        }
        /* Target-side work rides the verification/active phases only. */
        return phase == OP_PHASE_VERIFYING_TARGET || phase == OP_PHASE_ACTIVE ||
               phase == OP_PHASE_WAITING_FOR_TRUSTED_TIME;
    case EXEC_OWNER_REQ_RESTORE_SIDE:
        if (owner == OP_OWNER_SOURCE_RESTORE) {
            return phase == OP_PHASE_RESTORING_SOURCE ||
                   phase == OP_PHASE_TERMINAL_ACK_PENDING;
        }
        if (owner_is_session_class(owner)) {
            return phase == OP_PHASE_RESTORING_SOURCE ||
                   phase == OP_PHASE_TERMINAL_ACK_PENDING ||
                   /* the pre-restore quiesce may begin from the active or
                    * target-verification phases before the phase moves */
                   phase == OP_PHASE_ACTIVE || phase == OP_PHASE_VERIFYING_TARGET;
        }
        /* Post-release retained terminal (COMPLETE handoff tail). */
        return owner == OP_OWNER_NONE && phase == OP_PHASE_FREE;
    case EXEC_OWNER_REQ_ANY_OWNED:
        return owner != OP_OWNER_NONE || phase == OP_PHASE_RECOVERY_GUARD;
    default:
        return false; /* fail closed */
    }
}

bool pool_exec_state_owns_flow(PoolExecState s)
{
    switch (s) {
    case EXEC_STATE_DISABLED:
    case EXEC_STATE_IDLE:
    case EXEC_STATE_DONE:
        return false;
    default:
        return enum_in_range((unsigned)s, POOL_EXEC_STATE__COUNT);
    }
}

bool pool_exec_state_permits_mutation(PoolExecState s)
{
    switch (pool_exec_state_action(s)) {
    case EXEC_ACTION_CONFIG_WRITE:
    case EXEC_ACTION_PROTOCOL_START:
    case EXEC_ACTION_PROTOCOL_STOP:
    case EXEC_ACTION_HANDOFF:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Board / ASIC compatibility                                          */
/* ------------------------------------------------------------------ */

static bool exact_bounded_match(const char *value, const char *expected, size_t cap)
{
    if (!str_bounded(value, cap) || value[0] == '\0') {
        return false;
    }
    return strncmp(value, expected, cap) == 0;
}

bool pool_exec_board_supported(const char *board_version)
{
    return exact_bounded_match(board_version, POOL_SESSION_SUPPORTED_BOARD,
                               POOL_SESSION_BOARD_MAX);
}

bool pool_exec_asic_supported(const char *asic_model)
{
    return exact_bounded_match(asic_model, POOL_SESSION_SUPPORTED_ASIC,
                               POOL_SESSION_ASIC_MAX);
}

/* ------------------------------------------------------------------ */
/* TLS-mode representability (Blocker 2 gate)                          */
/* ------------------------------------------------------------------ */

bool pool_exec_tls_mode_representable(uint8_t mode)
{
    /* ONLY the two modes the B1 boolean denotes exactly. CUSTOM (2) and any
     * unknown value would be silently downgraded on restore. */
    return mode == POOL_EXEC_TLS_MODE_DISABLED || mode == POOL_EXEC_TLS_MODE_BUNDLED;
}

uint8_t pool_exec_tls_mode_for_flag(bool tls)
{
    return tls ? (uint8_t)POOL_EXEC_TLS_MODE_BUNDLED
               : (uint8_t)POOL_EXEC_TLS_MODE_DISABLED;
}

PoolExecTlsVerdict pool_exec_tls_representable(const PoolExecEffectiveConfig *cfg)
{
    if (cfg == NULL || !cfg->valid) {
        return EXEC_TLS_UNREADABLE; /* fail closed: never assume a mode */
    }
    if (!pool_exec_tls_mode_representable(cfg->primary_tls_mode)) {
        return EXEC_TLS_PRIMARY_UNSUPPORTED;
    }
    if (!pool_exec_tls_mode_representable(cfg->fallback_tls_mode)) {
        return EXEC_TLS_FALLBACK_UNSUPPORTED;
    }
    /* The stored mode must also AGREE with the boolean the B1 identity would
     * carry — otherwise the snapshot already lost information. */
    if (cfg->primary_tls_mode != pool_exec_tls_mode_for_flag(cfg->primary.tls)) {
        return EXEC_TLS_PRIMARY_UNSUPPORTED;
    }
    if (cfg->fallback_tls_mode != pool_exec_tls_mode_for_flag(cfg->fallback.tls)) {
        return EXEC_TLS_FALLBACK_UNSUPPORTED;
    }
    return EXEC_TLS_OK;
}

void pool_exec_identity_tls_modes(const PoolConfigIdentity *identity,
                                  uint8_t *out_primary, uint8_t *out_fallback)
{
    uint8_t p = POOL_EXEC_TLS_MODE_DISABLED;
    uint8_t f = POOL_EXEC_TLS_MODE_DISABLED;

    if (identity != NULL) {
        p = pool_exec_tls_mode_for_flag(identity->primary.tls);
        /* A disabled fallback is canonically empty (tls=false => DISABLED). */
        if (identity->fallback_enabled) {
            f = pool_exec_tls_mode_for_flag(identity->fallback.tls);
        }
    }
    if (out_primary != NULL) {
        *out_primary = p;
    }
    if (out_fallback != NULL) {
        *out_fallback = f;
    }
}

/* ------------------------------------------------------------------ */
/* Effective configuration                                             */
/* ------------------------------------------------------------------ */

void pool_exec_desired_from_identity(const PoolConfigIdentity *identity,
                                     PoolExecEffectiveConfig *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (identity == NULL) {
        return; /* invalid: matches nothing (valid=false) */
    }
    out->valid   = true;
    out->primary = identity->primary;
    if (identity->fallback_enabled) {
        out->fallback = identity->fallback;
    } /* else: canonical zeroed fallback (the B3 rule) */
    out->use_fallback = false; /* sessions pin the PRIMARY as the identity */
    /* The exact modes a timed session may write — always representable. */
    pool_exec_identity_tls_modes(identity, &out->primary_tls_mode,
                                 &out->fallback_tls_mode);
}

/* Per-endpoint mismatch bits with a caller-selected bit base. */
static uint32_t endpoint_mismatch(const PoolEndpoint *a, const PoolEndpoint *b,
                                  uint32_t host_bit)
{
    uint32_t mask = 0u;

    if (!str_bounded(a->host, POOL_SESSION_HOST_MAX) ||
        !str_bounded(b->host, POOL_SESSION_HOST_MAX) ||
        strncmp(a->host, b->host, POOL_SESSION_HOST_MAX) != 0) {
        mask |= host_bit;
    }
    if (a->port != b->port) {
        mask |= host_bit << 1;
    }
    if (!str_bounded(a->user, POOL_SESSION_USER_MAX) ||
        !str_bounded(b->user, POOL_SESSION_USER_MAX) ||
        strncmp(a->user, b->user, POOL_SESSION_USER_MAX) != 0) {
        mask |= host_bit << 2;
    }
    if (a->protocol != b->protocol) {
        mask |= host_bit << 3;
    }
    if (a->tls != b->tls) {
        mask |= host_bit << 4;
    }
    return mask;
}

static uint32_t effective_mismatch(const PoolExecEffectiveConfig *a,
                                   const PoolExecEffectiveConfig *b)
{
    uint32_t mask = 0u;
    mask |= endpoint_mismatch(&a->primary, &b->primary, EXEC_MISMATCH_PRIMARY_HOST);
    mask |= endpoint_mismatch(&a->fallback, &b->fallback, EXEC_MISMATCH_FALLBACK_HOST);
    if (a->use_fallback != b->use_fallback) {
        mask |= EXEC_MISMATCH_ROLE;
    }
    /*
     * The RAW stored TLS mode must match too, not merely the lossy boolean:
     * a custom-certificate mode reading back as "bundled" satisfies the
     * boolean but is NOT the same configuration, and must never be reported
     * as an exact restoration.
     */
    if (a->primary_tls_mode != b->primary_tls_mode) {
        mask |= EXEC_MISMATCH_PRIMARY_TLS;
    }
    if (a->fallback_tls_mode != b->fallback_tls_mode) {
        mask |= EXEC_MISMATCH_FALLBACK_TLS;
    }
    return mask;
}

bool pool_exec_effective_matches_identity(const PoolExecEffectiveConfig *effective,
                                          const PoolConfigIdentity *identity,
                                          uint32_t *out_mask)
{
    PoolExecEffectiveConfig desired;
    uint32_t mask;

    if (out_mask != NULL) {
        *out_mask = EXEC_MISMATCH__ALL;
    }
    if (effective == NULL || identity == NULL) {
        return false;
    }
    if (!effective->valid) {
        if (out_mask != NULL) {
            *out_mask = EXEC_MISMATCH_READBACK;
        }
        return false;
    }
    pool_exec_desired_from_identity(identity, &desired);
    mask = effective_mismatch(effective, &desired);
    if (out_mask != NULL) {
        *out_mask = mask;
    }
    return mask == 0u;
}

bool pool_exec_effective_equal(const PoolExecEffectiveConfig *a,
                               const PoolExecEffectiveConfig *b,
                               uint32_t *out_mask)
{
    uint32_t mask;

    if (out_mask != NULL) {
        *out_mask = EXEC_MISMATCH__ALL;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    if (!a->valid || !b->valid) {
        if (out_mask != NULL) {
            *out_mask = EXEC_MISMATCH_READBACK;
        }
        return false;
    }
    mask = effective_mismatch(a, b);
    if (out_mask != NULL) {
        *out_mask = mask;
    }
    return mask == 0u;
}

static uint32_t popcount32(uint32_t v)
{
    uint32_t n = 0u;
    while (v != 0u) {
        v &= v - 1u;
        n++;
    }
    return n;
}

uint32_t pool_exec_writes_needed(const PoolExecEffectiveConfig *from,
                                 const PoolConfigIdentity *identity)
{
    PoolExecEffectiveConfig desired;

    if (from == NULL || identity == NULL || !from->valid) {
        return UINT32_MAX;
    }
    pool_exec_desired_from_identity(identity, &desired);
    return popcount32(effective_mismatch(from, &desired));
}

/* ------------------------------------------------------------------ */
/* Configuration transaction classification                            */
/* ------------------------------------------------------------------ */

PoolExecConfigApplyResult pool_exec_classify_apply(
    const PoolExecEffectiveConfig *pre, const PoolConfigIdentity *identity,
    const PoolExecEffectiveConfig *now, bool stage_rejected, bool timed_out)
{
    PoolExecEffectiveConfig desired;
    uint32_t vs_desired;
    uint32_t vs_pre;
    uint32_t writes_needed;

    if (pre == NULL || identity == NULL || now == NULL || !pre->valid) {
        /* No trustworthy frame of reference: an unknown pre-state can never
         * prove anything about mutation. */
        return EXEC_CONFIG_APPLY_UNCERTAIN;
    }
    if (!now->valid) {
        /* The independent readback itself failed. */
        return timed_out ? EXEC_CONFIG_APPLY_UNCERTAIN : EXEC_CONFIG_PENDING;
    }

    pool_exec_desired_from_identity(identity, &desired);
    vs_desired    = effective_mismatch(now, &desired);
    vs_pre        = effective_mismatch(now, pre);
    writes_needed = popcount32(effective_mismatch(pre, &desired));

    if (vs_desired == 0u) {
        /* Every field reads back exactly as desired. */
        return (writes_needed == 0u) ? EXEC_CONFIG_APPLY_NO_MUTATION
                                     : EXEC_CONFIG_APPLY_EXACT;
    }

    /* Any field that matches NEITHER the pre value NOR the desired value is
     * a FOREIGN mutation — never continue over it. A field appearing in both
     * mismatch masks is exactly such a field. */
    {
        uint32_t bit;
        for (bit = 1u; bit <= EXEC_MISMATCH_ROLE; bit <<= 1) {
            if ((vs_desired & bit) != 0u && (vs_pre & bit) != 0u) {
                return EXEC_CONFIG_APPLY_READBACK_MISMATCH;
            }
        }
    }

    if (vs_pre == 0u) {
        /* Nothing has changed yet. A definite pre-enqueue rejection means no
         * mutation was ever staged; a timeout with staged writes possibly in
         * flight is UNCERTAIN (the asynchronous writer may still land). */
        if (stage_rejected) {
            return EXEC_CONFIG_APPLY_NO_MUTATION;
        }
        return timed_out ? EXEC_CONFIG_APPLY_UNCERTAIN : EXEC_CONFIG_PENDING;
    }

    /* A mix of pre and desired fields: mutation is in progress. */
    return timed_out ? EXEC_CONFIG_APPLY_PARTIAL : EXEC_CONFIG_PENDING;
}

bool pool_exec_apply_result_final(PoolExecConfigApplyResult r)
{
    switch (r) {
    case EXEC_CONFIG_APPLY_EXACT:
    case EXEC_CONFIG_APPLY_NO_MUTATION:
    case EXEC_CONFIG_APPLY_PARTIAL:
    case EXEC_CONFIG_APPLY_UNCERTAIN:
    case EXEC_CONFIG_APPLY_READBACK_MISMATCH:
        return true;
    default:
        return false;
    }
}

bool pool_exec_apply_result_uncertain_mutation(PoolExecConfigApplyResult r)
{
    switch (r) {
    case EXEC_CONFIG_APPLY_PARTIAL:
    case EXEC_CONFIG_APPLY_UNCERTAIN:
    case EXEC_CONFIG_APPLY_READBACK_MISMATCH:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Protocol evidence                                                   */
/* ------------------------------------------------------------------ */

uint32_t pool_exec_protocol_events_sanitize(uint32_t raw)
{
    return raw & EXEC_PEVT__ALL_VALID;
}

void pool_exec_evaluate_evidence(const PoolExecProtocolCounters *baseline,
                                 const PoolExecProtocolCounters *now,
                                 uint32_t events, PoolExecEvidence *out)
{
    uint32_t ev;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (baseline == NULL || now == NULL) {
        out->anomaly = true; /* no frame of reference: never evidence */
        return;
    }
    ev = pool_exec_protocol_events_sanitize(events);

    /* Counter regression is stale/foreign state, never evidence. The ASIC
     * counters are per-work-generation and reset to zero on a generation
     * change, so a regression there means the baseline belongs to an OLDER
     * generation — exactly the stale case that must be rejected. */
    if (now->work_received < baseline->work_received ||
        now->jobs_forwarded < baseline->jobs_forwarded ||
        now->asic_job_results < baseline->asic_job_results) {
        out->anomaly = true;
        return;
    }
    out->job_delta        = now->work_received - baseline->work_received;
    out->forward_delta    = now->jobs_forwarded - baseline->jobs_forwarded;
    out->asic_result_delta = now->asic_job_results - baseline->asic_job_results;

    out->job_evidence        = out->job_delta >= 1u;
    out->forward_evidence    = out->forward_delta >= 1u;
    out->connection_evidence = out->job_evidence ||
                               (ev & EXEC_PEVT_SETUP_SUCCESS) != 0u;
    /*
     * HARDWARE fact only: the chip returned results for work THIS generation
     * delivered, each proven against its own exact header and each consuming
     * a DISTINCT delivered-work record. Never set by a queue dequeue, an
     * ASIC_send_work call or the delivery-gate counter.
     *
     * Requiring POOL_EXEC_REQUIRED_WORK_PROOFS distinct records squares the
     * per-result false-positive bound and proves sustained processing rather
     * than one fluke. It is a COUNT of local proofs, never a hashrate
     * threshold and never a pool share.
     */
    out->asic_processing_evidence =
        out->asic_result_delta >= (uint64_t)POOL_EXEC_REQUIRED_WORK_PROOFS;
}

bool pool_exec_evidence_mining_verified(const PoolExecEvidence *ev)
{
    if (ev == NULL || ev->anomaly) {
        return false;
    }
    /* All three, in the only order that means anything: the pool served
     * work, the work reached the ASIC, and the ASIC processed it. */
    return ev->job_evidence && ev->forward_evidence && ev->asic_processing_evidence;
}

/* ------------------------------------------------------------------ */
/* Generation-unique work contract                                     */
/* ------------------------------------------------------------------ */

bool pool_exec_work_facts_equal(const PoolExecWorkFacts *a,
                                const PoolExecWorkFacts *b)
{
    if (a == NULL || b == NULL) {
        return false; /* an absent side can never prove equality */
    }
    /* Field-by-field: padding bytes can never fake or hide a difference. */
    return a->version == b->version &&
           a->ntime == b->ntime &&
           a->nbits == b->nbits &&
           memcmp(a->prev_block_hash, b->prev_block_hash,
                  sizeof(a->prev_block_hash)) == 0 &&
           memcmp(a->merkle_root, b->merkle_root,
                  sizeof(a->merkle_root)) == 0;
}

PoolExecHeaderDiscriminator pool_exec_discriminator_capability(
    bool protocol_v2, bool sv2_extended_channel, uint32_t extranonce2_len)
{
    if (extranonce2_len < POOL_EXEC_DISCRIMINATOR_MIN_EN2_BYTES) {
        return POOL_EXEC_DISCRIMINATOR_NONE; /* tag byte would be truncated */
    }
    if (!protocol_v2) {
        return POOL_EXEC_DISCRIMINATOR_EXTRANONCE2; /* V1 miner-owned space */
    }
    if (sv2_extended_channel) {
        return POOL_EXEC_DISCRIMINATOR_EXTRANONCE2; /* SV2 ext rollable space */
    }
    /* SV2 standard channel: the pool owns the merkle root outright. */
    return POOL_EXEC_DISCRIMINATOR_NONE;
}

uint32_t pool_exec_generation_tag(uint32_t allocation_index)
{
    /* Monotonic allocation only — NEVER modulo. Index 0 is "nothing
     * allocated" and anything past the per-boot budget is fail-closed 0:
     * a wrapped or reused domain is unrepresentable here by construction.
     * Issued tags always carry the top bit, so they can never collide
     * with the untagged stock encoding (byte value 0). */
    if (allocation_index == 0u ||
        allocation_index > POOL_EXEC_GENERATION_TAG_LIMIT) {
        return 0u;
    }
    return 0x80u + allocation_index;
}

uint64_t pool_exec_apply_generation_tag(uint64_t counter, uint32_t tag)
{
    return (counter & POOL_EXEC_GENERATION_COUNTER_MASK) |
           ((uint64_t)(tag & POOL_EXEC_GENERATION_TAG_MASK)
            << POOL_EXEC_GENERATION_TAG_SHIFT);
}

/* ------------------------------------------------------------------ */
/* Target-mining grant                                                 */
/* ------------------------------------------------------------------ */

void pool_exec_grant_init(PoolExecMiningGrant *g)
{
    if (g != NULL) {
        memset(g, 0, sizeof(*g));
    }
}

bool pool_exec_grant_issue(PoolExecMiningGrant *g, uint32_t session_id,
                           uint32_t lease_generation, uint32_t record_generation,
                           uint32_t protocol_generation, uint32_t issue_sequence)
{
    if (g == NULL) {
        return false;
    }
    pool_exec_grant_init(g);
    /* A grant can never bind to an absent session, lease, record or
     * protocol epoch — zero is the reserved-invalid value everywhere. */
    if (session_id == 0u || lease_generation == 0u || record_generation == 0u ||
        protocol_generation == 0u) {
        return false;
    }
    g->valid               = true;
    g->revoked             = false;
    g->session_id          = session_id;
    g->lease_generation    = lease_generation;
    g->record_generation   = record_generation;
    g->protocol_generation = protocol_generation;
    g->issue_sequence      = issue_sequence;
    return true;
}

void pool_exec_grant_revoke(PoolExecMiningGrant *g)
{
    if (g != NULL && g->valid) {
        g->revoked = true; /* permanent for this issuance */
    }
}

bool pool_exec_grant_valid(const PoolExecMiningGrant *g, uint32_t session_id,
                           uint32_t lease_generation, uint32_t record_generation,
                           uint32_t protocol_generation)
{
    if (g == NULL || !g->valid || g->revoked) {
        return false;
    }
    return g->session_id == session_id &&
           g->lease_generation == lease_generation &&
           g->record_generation == record_generation &&
           g->protocol_generation == protocol_generation &&
           session_id != 0u && lease_generation != 0u &&
           record_generation != 0u && protocol_generation != 0u;
}

bool pool_exec_gate_allows(PoolExecGatePosture gate, bool target_grant_valid)
{
    switch (gate) {
    case EXEC_GATE_DEFAULT_OPEN:
        return true; /* no execution epoch: source/default mining unaffected */
    case EXEC_GATE_OPEN_TARGET:
        return target_grant_valid; /* only a currently-valid grant releases */
    case EXEC_GATE_OPEN_SOURCE_RESTORED:
        return true; /* verified source restoration posture */
    case EXEC_GATE_INHIBITED:
    default:
        return false; /* fail closed for INHIBITED and any unknown value */
    }
}

/* ------------------------------------------------------------------ */
/* Bounded policy and monotonic deadlines                              */
/* ------------------------------------------------------------------ */

void pool_exec_policy_defaults(PoolExecPolicy *p)
{
    if (p == NULL) {
        return;
    }
    p->config_timeout_s  = EXEC_POLICY_CONFIG_TIMEOUT_DEFAULT_S;
    p->stop_timeout_s    = EXEC_POLICY_STOP_TIMEOUT_DEFAULT_S;
    p->connect_timeout_s = EXEC_POLICY_CONNECT_TIMEOUT_DEFAULT_S;
    p->job_timeout_s     = EXEC_POLICY_JOB_TIMEOUT_DEFAULT_S;
    p->target_health_s   = EXEC_POLICY_TARGET_HEALTH_DEFAULT_S;
    p->source_health_s   = EXEC_POLICY_SOURCE_HEALTH_DEFAULT_S;
    p->stop_retry_max    = EXEC_POLICY_STOP_RETRY_MAX;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

void pool_exec_policy_clamp(PoolExecPolicy *p)
{
    if (p == NULL) {
        return;
    }
    p->config_timeout_s  = clamp_u32(p->config_timeout_s,  EXEC_POLICY_TIMEOUT_MIN_S, EXEC_POLICY_TIMEOUT_MAX_S);
    p->stop_timeout_s    = clamp_u32(p->stop_timeout_s,    EXEC_POLICY_TIMEOUT_MIN_S, EXEC_POLICY_TIMEOUT_MAX_S);
    p->connect_timeout_s = clamp_u32(p->connect_timeout_s, EXEC_POLICY_TIMEOUT_MIN_S, EXEC_POLICY_TIMEOUT_MAX_S);
    p->job_timeout_s     = clamp_u32(p->job_timeout_s,     EXEC_POLICY_TIMEOUT_MIN_S, EXEC_POLICY_TIMEOUT_MAX_S);
    /* The job window contains the connection window by definition. */
    if (p->job_timeout_s < p->connect_timeout_s) {
        p->job_timeout_s = p->connect_timeout_s;
    }
    p->target_health_s = clamp_u32(p->target_health_s, EXEC_POLICY_TIMEOUT_MIN_S, EXEC_POLICY_TIMEOUT_MAX_S);
    p->source_health_s = clamp_u32(p->source_health_s, EXEC_POLICY_TIMEOUT_MIN_S, EXEC_POLICY_TIMEOUT_MAX_S);
    if (p->stop_retry_max > EXEC_POLICY_STOP_RETRY_MAX) {
        p->stop_retry_max = EXEC_POLICY_STOP_RETRY_MAX;
    }
}

uint64_t pool_exec_deadline_us(uint64_t now_us, uint32_t seconds)
{
    uint64_t add = (uint64_t)seconds * 1000000ull;
    uint64_t d   = now_us + add;
    return (d < now_us) ? UINT64_MAX : d; /* saturate; never wrap */
}

bool pool_exec_deadline_reached(uint64_t now_us, uint64_t deadline_us)
{
    return now_us >= deadline_us;
}

/* ------------------------------------------------------------------ */
/* Record staging and exact transition readback                        */
/* ------------------------------------------------------------------ */

PoolRecordCodecError pool_exec_stage_record(const PoolSession *session,
                                            const PoolSessionRecord *committed,
                                            PoolSessionRecord *out)
{
    PoolRecordCodecError err;

    if (session == NULL || committed == NULL || out == NULL) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    err = pool_session_record_from_session(session, out);
    if (err != RECORD_OK) {
        return err;
    }
    /* Time facts and bounded recovery counters are NOT part of the B1
     * session model (from_session zeroes them): restore them exactly from
     * the committed record so a transition commit never erases them. */
    out->duration_s               = committed->duration_s;
    out->verified_start_valid     = committed->verified_start_valid;
    out->verified_start_epoch_s   = committed->verified_start_epoch_s;
    out->deadline_valid           = committed->deadline_valid;
    out->deadline_epoch_s         = committed->deadline_epoch_s;
    out->deadline_sync_generation = committed->deadline_sync_generation;
    out->latest_trusted_valid     = committed->latest_trusted_valid;
    out->latest_trusted_epoch_s   = committed->latest_trusted_epoch_s;

    out->reboot_count                  = committed->reboot_count;
    out->recovery_attempt_count        = committed->recovery_attempt_count;
    out->consecutive_recovery_failures = committed->consecutive_recovery_failures;
    out->last_reset_class              = committed->last_reset_class;

    return pool_session_record_validate(out);
}

static bool identity_equal_full(const PoolConfigIdentity *a,
                                const PoolConfigIdentity *b)
{
    if (!pool_config_identity_equal(a, b)) {
        return false;
    }
    if (!str_bounded(a->profile_id, POOL_SESSION_PROFILE_ID_MAX) ||
        !str_bounded(b->profile_id, POOL_SESSION_PROFILE_ID_MAX)) {
        return false;
    }
    return strncmp(a->profile_id, b->profile_id, POOL_SESSION_PROFILE_ID_MAX) == 0;
}

static bool verify_equal(const PoolSessionVerify *a, const PoolSessionVerify *b)
{
    return a->connection_observed == b->connection_observed &&
           a->mining_observed == b->mining_observed &&
           a->identity_verified == b->identity_verified;
}

bool pool_exec_record_equal_exact(const PoolSessionRecord *a,
                                  const PoolSessionRecord *b,
                                  bool ignore_generation)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    if (!ignore_generation && a->generation != b->generation) {
        return false;
    }
    if (a->kind != b->kind || a->session_id != b->session_id ||
        a->b1_model_version != b->b1_model_version || a->state != b->state ||
        a->password_policy != b->password_policy) {
        return false;
    }
    if (!identity_equal_full(&a->source, &b->source) ||
        !identity_equal_full(&a->target, &b->target)) {
        return false;
    }
    if (a->restore_required != b->restore_required ||
        a->cancel_requested != b->cancel_requested ||
        a->restore_requested != b->restore_requested) {
        return false;
    }
    if (!verify_equal(&a->target_verify, &b->target_verify) ||
        !verify_equal(&a->restore_verify, &b->restore_verify)) {
        return false;
    }
    if (a->last_failure_code != b->last_failure_code) {
        return false;
    }
    if (a->retries.target_apply != b->retries.target_apply ||
        a->retries.target_restart != b->retries.target_restart ||
        a->retries.target_verify != b->retries.target_verify ||
        a->retries.restore_apply != b->retries.restore_apply ||
        a->retries.restore_restart != b->retries.restore_restart ||
        a->retries.restore_verify != b->retries.restore_verify) {
        return false;
    }
    if (a->duration_s != b->duration_s ||
        a->verified_start_valid != b->verified_start_valid ||
        a->verified_start_epoch_s != b->verified_start_epoch_s ||
        a->deadline_valid != b->deadline_valid ||
        a->deadline_epoch_s != b->deadline_epoch_s ||
        a->deadline_sync_generation != b->deadline_sync_generation ||
        a->latest_trusted_valid != b->latest_trusted_valid ||
        a->latest_trusted_epoch_s != b->latest_trusted_epoch_s) {
        return false;
    }
    return a->reboot_count == b->reboot_count &&
           a->recovery_attempt_count == b->recovery_attempt_count &&
           a->consecutive_recovery_failures == b->consecutive_recovery_failures &&
           a->last_reset_class == b->last_reset_class;
}

PoolExecReason pool_exec_verify_transition_readback(const PoolSessionRecord *staged,
                                                    const PoolSessionRecord *reloaded,
                                                    uint32_t pre_commit_generation,
                                                    bool pre_commit_restore_required,
                                                    PoolStoreResult reload_result)
{
    if (staged == NULL || reloaded == NULL) {
        return EXEC_REASON_INTERNAL;
    }
    if (reload_result != STORE_OK) {
        return EXEC_REASON_PERSIST_READBACK;
    }
    if (!pool_exec_record_equal_exact(staged, reloaded, /*ignore_generation=*/true)) {
        return EXEC_REASON_PERSIST_READBACK;
    }
    if (reloaded->generation <= pre_commit_generation) {
        return EXEC_REASON_PERSIST_READBACK;
    }
    /* Obligation monotonicity: once incurred, only a COMPLETE transition may
     * discharge it. A pre-mutation posture (never incurred) may stay false. */
    if (pre_commit_restore_required && !reloaded->restore_required &&
        reloaded->state != POOL_STATE_COMPLETE) {
        return EXEC_REASON_PERSIST_READBACK;
    }
    return EXEC_REASON_NONE;
}

/* ------------------------------------------------------------------ */
/* Sanitized snapshot                                                  */
/* ------------------------------------------------------------------ */

void pool_exec_snapshot_init(PoolExecutionSnapshot *snap)
{
    if (snap == NULL) {
        return;
    }
    memset(snap, 0, sizeof(*snap));
    snap->model_version = POOL_EXEC_MODEL_VERSION;
    snap->state         = EXEC_STATE_DISABLED;
    snap->gate          = EXEC_GATE_DEFAULT_OPEN;
}

bool pool_exec_snapshot_valid(const PoolExecutionSnapshot *snap)
{
    if (snap == NULL || snap->model_version != POOL_EXEC_MODEL_VERSION) {
        return false;
    }
    if (!enum_in_range((unsigned)snap->state, POOL_EXEC_STATE__COUNT) ||
        !enum_in_range((unsigned)snap->reason, POOL_EXEC_REASON__COUNT) ||
        !enum_in_range((unsigned)snap->last_apply_result, POOL_EXEC_CONFIG_RESULT__COUNT) ||
        !enum_in_range((unsigned)snap->owner, OP_OWNER__COUNT) ||
        !enum_in_range((unsigned)snap->phase, OP_PHASE__COUNT) ||
        !enum_in_range((unsigned)snap->gate, POOL_EXEC_GATE__COUNT)) {
        return false;
    }
    if (snap->target_role > 2u) {
        return false;
    }
    /* The published gate must match the substate contract exactly. */
    if (snap->gate != pool_exec_state_gate(snap->state)) {
        return false;
    }
    /* A grant flag may be true only in the target-mining posture, and the
     * target gate may be open only with the grant flag. */
    if (snap->mining_grant_active && snap->state != EXEC_STATE_TARGET_MINING) {
        return false;
    }
    if (snap->gate == EXEC_GATE_OPEN_TARGET && !snap->mining_grant_active) {
        return false;
    }
    /* A hardware fact can only have been observed inside a real work
     * generation: claiming one without a generation is inconsistent. */
    if (snap->asic_evidence_seen && snap->work_generation == 0u) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Stable machine tokens                                               */
/* ------------------------------------------------------------------ */

const char *pool_exec_state_str(PoolExecState s)
{
    switch (s) {
    case EXEC_STATE_DISABLED:                 return "EXEC_DISABLED";
    case EXEC_STATE_IDLE:                     return "EXEC_IDLE";
    case EXEC_STATE_ENTRY_PENDING:            return "EXEC_ENTRY_PENDING";
    case EXEC_STATE_TARGET_READBACK:          return "EXEC_TARGET_READBACK";
    case EXEC_STATE_TARGET_APPLYING:          return "EXEC_TARGET_APPLYING";
    case EXEC_STATE_TARGET_CONFIG_VERIFIED:   return "EXEC_TARGET_CONFIG_VERIFIED";
    case EXEC_STATE_TARGET_CONNECTING:        return "EXEC_TARGET_CONNECTING";
    case EXEC_STATE_TARGET_PROTOCOL_VERIFIED: return "EXEC_TARGET_PROTOCOL_VERIFIED";
    case EXEC_STATE_TARGET_MINING:            return "EXEC_TARGET_MINING";
    case EXEC_STATE_RESTORE_PENDING:          return "EXEC_RESTORE_PENDING";
    case EXEC_STATE_SOURCE_APPLYING:          return "EXEC_SOURCE_APPLYING";
    case EXEC_STATE_SOURCE_CONFIG_VERIFIED:   return "EXEC_SOURCE_CONFIG_VERIFIED";
    case EXEC_STATE_SOURCE_CONNECTING:        return "EXEC_SOURCE_CONNECTING";
    case EXEC_STATE_SOURCE_PROTOCOL_VERIFIED: return "EXEC_SOURCE_PROTOCOL_VERIFIED";
    case EXEC_STATE_SOURCE_MINING_VERIFYING:  return "EXEC_SOURCE_MINING_VERIFYING";
    case EXEC_STATE_COMPLETE_HANDOFF:         return "EXEC_COMPLETE_HANDOFF";
    case EXEC_STATE_DONE:                     return "EXEC_DONE";
    case EXEC_STATE_HANDOFF_FAILED:           return "EXEC_HANDOFF_FAILED";
    case EXEC_STATE_RESTORE_FAILED_HELD:      return "EXEC_RESTORE_FAILED_HELD";
    case EXEC_STATE_RECOVERY_GUARD:           return "EXEC_RECOVERY_GUARD";
    case EXEC_STATE_ERROR:                    return "EXEC_ERROR";
    default:                                  return "EXEC_UNKNOWN";
    }
}

const char *pool_exec_reason_str(PoolExecReason r)
{
    switch (r) {
    case EXEC_REASON_NONE:                  return "EXEC_NONE";
    case EXEC_REASON_FEATURE_DISABLED:      return "EXEC_FEATURE_DISABLED";
    case EXEC_REASON_NOT_BOUND:             return "EXEC_NOT_BOUND";
    case EXEC_REASON_SYSTEM_NOT_READY:      return "EXEC_SYSTEM_NOT_READY";
    case EXEC_REASON_BOARD_UNSUPPORTED:     return "EXEC_BOARD_UNSUPPORTED";
    case EXEC_REASON_ASIC_UNSUPPORTED:      return "EXEC_ASIC_UNSUPPORTED";
    case EXEC_REASON_RECORD_INCOMPATIBLE:   return "EXEC_RECORD_INCOMPATIBLE";
    case EXEC_REASON_OWNERSHIP_MISMATCH:    return "EXEC_OWNERSHIP_MISMATCH";
    case EXEC_REASON_PHASE_MISMATCH:        return "EXEC_PHASE_MISMATCH";
    case EXEC_REASON_STALE_TOKEN:           return "EXEC_STALE_TOKEN";
    case EXEC_REASON_PERSIST_FAILED:        return "EXEC_PERSIST_FAILED";
    case EXEC_REASON_PERSIST_UNCERTAIN:     return "EXEC_PERSIST_UNCERTAIN";
    case EXEC_REASON_PERSIST_READBACK:      return "EXEC_PERSIST_READBACK";
    case EXEC_REASON_TRANSITION_REJECTED:   return "EXEC_TRANSITION_REJECTED";
    case EXEC_REASON_CONFIG_STAGE_REJECTED: return "EXEC_CONFIG_STAGE_REJECTED";
    case EXEC_REASON_CONFIG_TIMEOUT:        return "EXEC_CONFIG_TIMEOUT";
    case EXEC_REASON_CONFIG_PARTIAL:        return "EXEC_CONFIG_PARTIAL";
    case EXEC_REASON_CONFIG_UNCERTAIN:      return "EXEC_CONFIG_UNCERTAIN";
    case EXEC_REASON_CONFIG_MISMATCH:       return "EXEC_CONFIG_MISMATCH";
    case EXEC_REASON_CONFIG_VERIFIED:       return "EXEC_CONFIG_VERIFIED";
    case EXEC_REASON_PROTOCOL_START_FAILED: return "EXEC_PROTOCOL_START_FAILED";
    case EXEC_REASON_PROTOCOL_STOP_FAILED:  return "EXEC_PROTOCOL_STOP_FAILED";
    case EXEC_REASON_PROTOCOL_FAILED_EVENT: return "EXEC_PROTOCOL_FAILED_EVENT";
    case EXEC_REASON_CONNECT_TIMEOUT:       return "EXEC_CONNECT_TIMEOUT";
    case EXEC_REASON_JOB_TIMEOUT:           return "EXEC_JOB_TIMEOUT";
    case EXEC_REASON_EVIDENCE_STALE:        return "EXEC_EVIDENCE_STALE";
    case EXEC_REASON_IDENTITY_MISMATCH:     return "EXEC_IDENTITY_MISMATCH";
    case EXEC_REASON_GRANT_ISSUED:          return "EXEC_GRANT_ISSUED";
    case EXEC_REASON_GRANT_REVOKED:         return "EXEC_GRANT_REVOKED";
    case EXEC_REASON_HEALTH_FAILED:         return "EXEC_HEALTH_FAILED";
    case EXEC_REASON_DEADLINE_REACHED:      return "EXEC_DEADLINE_REACHED";
    case EXEC_REASON_RESTORE_STARTED:       return "EXEC_RESTORE_STARTED";
    case EXEC_REASON_RESTORE_FAILED:        return "EXEC_RESTORE_FAILED";
    case EXEC_REASON_COMPLETE_VERIFIED:     return "EXEC_COMPLETE_VERIFIED";
    case EXEC_REASON_HANDOFF_STOP_FAILED:   return "EXEC_HANDOFF_STOP_FAILED";
    case EXEC_REASON_HANDOFF_START_FAILED:  return "EXEC_HANDOFF_START_FAILED";
    case EXEC_REASON_ASIC_EVIDENCE_MISSING: return "EXEC_ASIC_EVIDENCE_MISSING";
    case EXEC_REASON_TLS_MODE_UNSUPPORTED:  return "EXEC_TLS_MODE_UNSUPPORTED";
    case EXEC_REASON_GENERATION_EXHAUSTED:  return "EXEC_GENERATION_EXHAUSTED";
    case EXEC_REASON_RECOVERY_GUARD:        return "EXEC_RECOVERY_GUARD";
    case EXEC_REASON_INTERNAL:              return "EXEC_INTERNAL";
    default:                                return "EXEC_UNKNOWN";
    }
}

const char *pool_exec_gate_str(PoolExecGatePosture g)
{
    switch (g) {
    case EXEC_GATE_DEFAULT_OPEN:         return "GATE_DEFAULT_OPEN";
    case EXEC_GATE_INHIBITED:            return "GATE_INHIBITED";
    case EXEC_GATE_OPEN_TARGET:          return "GATE_OPEN_TARGET";
    case EXEC_GATE_OPEN_SOURCE_RESTORED: return "GATE_OPEN_SOURCE_RESTORED";
    default:                             return "GATE_UNKNOWN";
    }
}

const char *pool_exec_apply_result_str(PoolExecConfigApplyResult r)
{
    switch (r) {
    case EXEC_CONFIG_PENDING:                return "CONFIG_PENDING";
    case EXEC_CONFIG_APPLY_EXACT:            return "CONFIG_APPLY_EXACT";
    case EXEC_CONFIG_APPLY_NO_MUTATION:      return "CONFIG_APPLY_NO_MUTATION";
    case EXEC_CONFIG_APPLY_PARTIAL:          return "CONFIG_APPLY_PARTIAL";
    case EXEC_CONFIG_APPLY_UNCERTAIN:        return "CONFIG_APPLY_UNCERTAIN";
    case EXEC_CONFIG_APPLY_READBACK_MISMATCH:return "CONFIG_APPLY_READBACK_MISMATCH";
    default:                                 return "CONFIG_UNKNOWN";
    }
}
