/*
 * NeuralAxe timed pool sessions — pure boot-recovery decision engine
 * (Gate B4).
 *
 * PURE: no ESP-IDF calls, no NVS, no SNTP, no networking, no clock or
 * reset-reason reads, no logging, no heap, no global mutable state. The
 * engine composes the committed B1/B2/B3 helpers; it never re-implements
 * their rules where they can be reused.
 */

#include <string.h>
#include "pool_session_recovery.h"

/* ------------------------------------------------------------------ */
/* Deterministic fingerprint (FNV-1a 32)                               */
/* ------------------------------------------------------------------ */

static uint32_t fnv1a(const uint8_t *data, size_t len)
{
    uint32_t h = 0x811C9DC5u;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

static void plan_finalize_fingerprint(PoolSessionRecoveryPlan *p)
{
    uint8_t buf[40];
    size_t o = 0;
    int i;
    buf[o++] = (uint8_t)p->decision;
    buf[o++] = (uint8_t)p->allowed_config;
    buf[o++] = (uint8_t)p->mining_policy;
    buf[o++] = (uint8_t)p->persist_intent;
    buf[o++] = (uint8_t)p->runtime_intent;
    buf[o++] = (uint8_t)p->error;
    buf[o++] = (uint8_t)p->reason;
    buf[o++] = p->remaining_valid ? 1u : 0u;
    for (i = 0; i < 8; i++) {
        buf[o++] = (uint8_t)((p->remaining_target_s >> (8 * i)) & 0xFFu);
    }
    buf[o++] = p->trusted_time_required ? 1u : 0u;
    buf[o++] = p->restore_required ? 1u : 0u;
    buf[o++] = p->inhibit_target_stratum ? 1u : 0u;
    buf[o++] = p->terminal ? 1u : 0u;
    buf[o++] = p->counters.reboot_count;
    buf[o++] = p->counters.recovery_attempt_count;
    buf[o++] = p->counters.consecutive_recovery_failures;
    buf[o++] = p->counters.reset_class_for_record;
    buf[o++] = (uint8_t)((p->counters.any_exhausted ? 1u : 0u) |
                         (p->counters.must_persist_before_action ? 2u : 0u));
    buf[o++] = p->record_proposal.update_needed ? 1u : 0u;
    buf[o++] = (uint8_t)p->record_proposal.proposed_state;
    buf[o++] = (uint8_t)(p->record_proposal.proposed_failure_code & 0xFFu);
    buf[o++] = (uint8_t)(p->record_proposal.proposed_failure_code >> 8);
    p->plan_fingerprint = fnv1a(buf, o);
}

/* ------------------------------------------------------------------ */
/* Plan construction helpers                                           */
/* ------------------------------------------------------------------ */

static void plan_defaults_conservative(PoolSessionRecoveryPlan *p)
{
    memset(p, 0, sizeof(*p));
    p->decision       = POOL_BOOT_DECISION_RECOVERY_REQUIRED;
    p->allowed_config = POOL_BOOT_ALLOW_NO_POOL;
    p->mining_policy  = POOL_BOOT_MINING_INHIBIT;
    p->persist_intent = POOL_BOOT_PERSIST_NO_RECORD_UPDATE;
    p->runtime_intent = POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY;
    p->error          = RECOVERY_ERR_INVALID_ARGUMENT;
    p->reason         = RECOVERY_ERR_INVALID_ARGUMENT;
    p->terminal       = true;
    p->record_proposal.proposed_state = POOL_STATE_IDLE;
}

static void set_plan(PoolSessionRecoveryPlan *p,
                     PoolBootDecision d, PoolBootAllowedConfig a,
                     PoolBootMiningPolicy m, PoolBootPersistIntent pi,
                     PoolBootRuntimeIntent ri, PoolRecoveryError err,
                     PoolRecoveryError reason, bool terminal)
{
    p->decision       = d;
    p->allowed_config = a;
    p->mining_policy  = m;
    p->persist_intent = pi;
    p->runtime_intent = ri;
    p->error          = err;
    p->reason         = reason;
    p->terminal       = terminal;
}

/* Conservative store-failure plan: no session, no target, no mutation. The
 * committed truth is unknowable, so the runtime may only run whatever
 * configuration is already in main NVS — and must surface the operator. */
static void set_store_failure_plan(PoolSessionRecoveryPlan *p, PoolRecoveryError err)
{
    set_plan(p, POOL_BOOT_DECISION_RECOVERY_REQUIRED,
             POOL_BOOT_ALLOW_CURRENT_CONFIG_UNVERIFIED,
             POOL_BOOT_MINING_VERIFY_BEFORE_MINING,
             POOL_BOOT_PERSIST_NO_RECORD_UPDATE,
             POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY,
             err, RECOVERY_ERR_OPERATOR_REQUIRED, true);
}

static void set_record_proposal(PoolSessionRecoveryPlan *p, PoolSessionState state,
                                uint16_t failure_code)
{
    p->record_proposal.update_needed = true;
    p->record_proposal.proposed_state = state;
    p->record_proposal.proposed_failure_code = failure_code;
}

/* ------------------------------------------------------------------ */
/* Counter proposals (pure; values derived from the record only)       */
/* ------------------------------------------------------------------ */

/* Would this boot's budget be exhausted? Prospective reboot/consecutive
 * values plus the CURRENT recovery-attempt value (the attempt increment is
 * a consequence of the plan, persisted before the next evaluation). */
static bool budget_exhausted_pre(const PoolSessionRecord *rec, PoolSessionResetClass rc)
{
    uint8_t reboot = pool_record_counter_increment(rec->reboot_count,
                                                   POOL_RECORD_REBOOT_COUNT_MAX);
    uint8_t consec = pool_reset_class_increments_consecutive_failures(rc)
                         ? pool_record_counter_increment(
                               rec->consecutive_recovery_failures,
                               POOL_RECORD_CONSECUTIVE_RECOVERY_FAIL_MAX)
                         : rec->consecutive_recovery_failures;
    return reboot >= POOL_RECORD_REBOOT_COUNT_MAX ||
           consec >= POOL_RECORD_CONSECUTIVE_RECOVERY_FAIL_MAX ||
           rec->recovery_attempt_count >= POOL_RECORD_RECOVERY_ATTEMPT_MAX;
}

static void propose_counters(PoolSessionRecoveryPlan *p,
                             const PoolSessionRecord *rec,
                             PoolSessionResetClass rc)
{
    PoolBootCounterProposal *c = &p->counters;
    memset(c, 0, sizeof(*c));
    c->reset_class_for_record = pool_reset_class_to_record_class(rc);
    if (rec == NULL) {
        return; /* no record, no counters, nothing to persist */
    }
    c->reboot_count = pool_record_counter_increment(rec->reboot_count,
                                                    POOL_RECORD_REBOOT_COUNT_MAX);
    c->reboot_changed = (c->reboot_count != rec->reboot_count);

    if (pool_reset_class_increments_consecutive_failures(rc)) {
        c->consecutive_recovery_failures = pool_record_counter_increment(
            rec->consecutive_recovery_failures, POOL_RECORD_CONSECUTIVE_RECOVERY_FAIL_MAX);
    } else {
        c->consecutive_recovery_failures = rec->consecutive_recovery_failures;
    }
    c->consecutive_changed =
        (c->consecutive_recovery_failures != rec->consecutive_recovery_failures);

    /* The attempt counter advances when THIS plan is a recovery action. */
    if (p->decision == POOL_BOOT_DECISION_RESTORE_SOURCE_NOW ||
        p->decision == POOL_BOOT_DECISION_VERIFY_RESTORE) {
        c->recovery_attempt_count = pool_record_counter_increment(
            rec->recovery_attempt_count, POOL_RECORD_RECOVERY_ATTEMPT_MAX);
    } else {
        c->recovery_attempt_count = rec->recovery_attempt_count;
    }
    c->recovery_attempt_changed =
        (c->recovery_attempt_count != rec->recovery_attempt_count);

    c->any_exhausted =
        c->reboot_count >= POOL_RECORD_REBOOT_COUNT_MAX ||
        c->recovery_attempt_count >= POOL_RECORD_RECOVERY_ATTEMPT_MAX ||
        c->consecutive_recovery_failures >= POOL_RECORD_CONSECUTIVE_RECOVERY_FAIL_MAX;

    c->must_persist_before_action =
        c->reboot_changed || c->recovery_attempt_changed || c->consecutive_changed ||
        (p->persist_intent != POOL_BOOT_PERSIST_NO_RECORD_UPDATE &&
         p->persist_intent != POOL_BOOT_PERSIST_RETAIN_TERMINAL_RESULT);
}

/* Counter-exhaustion plan: stop automatic pool actions, keep the source as
 * the only allowed destination, surface the operator — never a reboot loop. */
static void set_exhausted_plan(PoolSessionRecoveryPlan *p)
{
    set_plan(p, POOL_BOOT_DECISION_RECOVERY_REQUIRED, POOL_BOOT_ALLOW_SOURCE_ONLY,
             POOL_BOOT_MINING_VERIFY_BEFORE_MINING,
             POOL_BOOT_PERSIST_PROPOSE_RECOVERY_REQUIRED,
             POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY,
             RECOVERY_ERR_COUNTER_EXHAUSTED, RECOVERY_ERR_OPERATOR_REQUIRED, true);
    set_record_proposal(p, POOL_STATE_RECOVERY_REQUIRED, (uint16_t)ERR_RETRY_EXHAUSTED);
}

/* Restore-toward-source plan (the universal fail-safe direction). */
static void set_restore_plan(PoolSessionRecoveryPlan *p, PoolRecoveryError reason,
                             PoolSessionState current_state, uint16_t failure_code)
{
    set_plan(p, POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, POOL_BOOT_ALLOW_SOURCE_ONLY,
             POOL_BOOT_MINING_VERIFY_BEFORE_MINING,
             (current_state == POOL_STATE_RESTORE_DUE ||
              current_state == POOL_STATE_APPLYING_RESTORE)
                 ? POOL_BOOT_PERSIST_PROPOSE_COUNTER_UPDATE
                 : POOL_BOOT_PERSIST_PROPOSE_RESTORE_DUE,
             POOL_BOOT_RUNTIME_APPLY_SOURCE_CONFIGURATION, RECOVERY_OK, reason, true);
    if (current_state != POOL_STATE_RESTORE_DUE &&
        current_state != POOL_STATE_APPLYING_RESTORE) {
        set_record_proposal(p, POOL_STATE_RESTORE_DUE, failure_code);
    }
}

/* ------------------------------------------------------------------ */
/* B2 time policy                                                      */
/* ------------------------------------------------------------------ */

void pool_session_recovery_build_time_policy(const PoolSessionRecord *record,
                                             PoolTimeTrustPolicy *out_policy)
{
    if (out_policy == NULL) {
        return;
    }
    pool_time_trust_policy_defaults(out_policy);
    if (record == NULL) {
        return;
    }
    /* Anti-regression floor: the persisted latest accepted trusted epoch
     * when valid; otherwise the weaker verified-start epoch (explicitly
     * permitted fallback, audit §7 predicate (c)); otherwise none. */
    if (record->latest_trusted_valid) {
        out_policy->required_min_epoch_s = record->latest_trusted_epoch_s;
    } else if (record->verified_start_valid) {
        out_policy->required_min_epoch_s = record->verified_start_epoch_s;
    }
}

/* Map a B2 fail-safe status onto a recovery reason token. */
static PoolRecoveryError map_time_status(PoolTimeError status)
{
    switch (status) {
    case TIME_ERR_SYNC_TIMEOUT:             return RECOVERY_ERR_TIME_TIMEOUT;
    case TIME_ERR_NO_PERSISTED_DEADLINE:    return RECOVERY_ERR_DEADLINE_MISSING;
    case TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN:return RECOVERY_ERR_TIME_REGRESSION;
    case TIME_ERR_INVALID_WAIT_WINDOW:      return RECOVERY_ERR_INVALID_ARGUMENT;
    case TIME_ERR_NOT_SYNCED:               return RECOVERY_ERR_TIME_UNTRUSTED;
    default:                                return RECOVERY_ERR_TIME_UNTRUSTED;
    }
}

/* ------------------------------------------------------------------ */
/* TARGET_ACTIVE time-aware planning (Stage 8/9)                       */
/* ------------------------------------------------------------------ */

static void plan_target_active(const PoolSessionBootContext *ctx,
                               const PoolSessionRecord *rec,
                               PoolSessionResetClass rc,
                               PoolSessionRecoveryPlan *p)
{
    PoolTimeTrustPolicy pol;
    PoolTimeRecoveryInput in;
    PoolTimeRecoveryResult r;
    bool trusted;
    PoolRecoveryError untrusted_reason = RECOVERY_ERR_TIME_UNTRUSTED;

    /* Structural resume predicate. The B3 validator already guarantees all
     * of this for a loadable TARGET_ACTIVE record; the checks stay explicit
     * (defense in depth, precise codes). */
    if (!rec->restore_required) {
        set_plan(p, POOL_BOOT_DECISION_RECOVERY_REQUIRED, POOL_BOOT_ALLOW_SOURCE_ONLY,
                 POOL_BOOT_MINING_VERIFY_BEFORE_MINING,
                 POOL_BOOT_PERSIST_PROPOSE_RECOVERY_REQUIRED,
                 POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY,
                 RECOVERY_ERR_RESTORE_OBLIGATION_MISMATCH,
                 RECOVERY_ERR_OPERATOR_REQUIRED, true);
        set_record_proposal(p, POOL_STATE_RECOVERY_REQUIRED, (uint16_t)ERR_RECOVERY_REQUIRED);
        return;
    }
    if (!rec->target_verify.connection_observed || !rec->target_verify.mining_observed ||
        !rec->target_verify.identity_verified) {
        set_restore_plan(p, RECOVERY_ERR_TARGET_INVALID, rec->state,
                         (uint16_t)ERR_INTERRUPTED);
        return;
    }
    if (rec->source.primary.host[0] == '\0') {
        set_plan(p, POOL_BOOT_DECISION_RECOVERY_REQUIRED, POOL_BOOT_ALLOW_NO_POOL,
                 POOL_BOOT_MINING_INHIBIT, POOL_BOOT_PERSIST_PROPOSE_RECOVERY_REQUIRED,
                 POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY,
                 RECOVERY_ERR_SOURCE_MISSING, RECOVERY_ERR_OPERATOR_REQUIRED, true);
        set_record_proposal(p, POOL_STATE_RECOVERY_REQUIRED, (uint16_t)ERR_RECOVERY_REQUIRED);
        return;
    }
    if (budget_exhausted_pre(rec, rc)) {
        set_exhausted_plan(p);
        return;
    }
    if (!pool_reset_class_may_resume_target(rc)) {
        /* Deep-sleep wake, external/tool reset or unclassifiable reset:
         * never resume the temporary target — conservative restore. */
        set_restore_plan(p, RECOVERY_ERR_RESET_ABNORMAL, rec->state,
                         (uint16_t)ERR_INTERRUPTED);
        return;
    }
    if (rec->deadline_valid && rec->deadline_sync_generation == 0u) {
        set_restore_plan(p, RECOVERY_ERR_RECORD_INVALID, rec->state,
                         (uint16_t)ERR_INTERRUPTED);
        return;
    }

    /* Build the B2 policy with the persisted anti-regression floor and
     * evaluate the supplied snapshot against it (consistency checking; the
     * snapshot's own trust verdict came from the B2 predicate). */
    pool_session_recovery_build_time_policy(rec, &pol);
    trusted = ctx->time_provider_initialized && ctx->time_snapshot.trusted &&
              ctx->time_snapshot.status == TIME_OK;
    if (trusted && pol.required_min_epoch_s != 0u &&
        ctx->time_snapshot.trusted_epoch_s < pol.required_min_epoch_s) {
        trusted = false; /* earlier than the floor: rejected trust claim */
        untrusted_reason = RECOVERY_ERR_TIME_REGRESSION;
    }

    memset(&in, 0, sizeof(in));
    in.source_snapshot_valid      = true;
    in.utc_deadline_valid         = rec->deadline_valid;
    in.deadline_epoch_s           = rec->deadline_epoch_s;
    in.verified_start_epoch_valid = rec->verified_start_valid;
    in.verified_start_epoch_s     = rec->verified_start_epoch_s;
    in.time_trusted               = trusted;
    in.trusted_epoch_s            = ctx->time_snapshot.trusted_epoch_s;
    in.sync_wait_elapsed_s        = ctx->sync_wait_elapsed_s;
    in.sync_wait_limit_s          = ctx->sync_wait_limit_s;

    r = pool_time_decide_recovery(&in, &pol);

    switch (r.decision) {
    case POOL_TIME_DECISION_WAIT_FOR_TRUSTED_TIME:
        if (!ctx->mining_inhibition_available) {
            /* Waiting requires guaranteed target-mining inhibition; without
             * it the only safe answer is immediate restore. */
            set_restore_plan(p, RECOVERY_ERR_MINING_INHIBITION_UNAVAILABLE, rec->state,
                             (uint16_t)ERR_INTERRUPTED);
            return;
        }
        set_plan(p, POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME, POOL_BOOT_ALLOW_NO_POOL,
                 POOL_BOOT_MINING_INHIBIT, POOL_BOOT_PERSIST_PROPOSE_COUNTER_UPDATE,
                 POOL_BOOT_RUNTIME_START_TRUSTED_TIME_WAIT, RECOVERY_OK,
                 untrusted_reason, false);
        p->trusted_time_required = true;
        p->inhibit_target_stratum = true;
        return;

    case POOL_TIME_DECISION_RESUME_TARGET_WITH_REMAINING_TIME:
        if (!r.remaining_valid || r.remaining_s == 0u) {
            set_restore_plan(p, RECOVERY_ERR_DEADLINE_EXPIRED, rec->state,
                             (uint16_t)ERR_NONE);
            return;
        }
        /* ELIGIBILITY, not authorization: B4 holds only persisted facts and
         * cannot observe the live post-boot configuration. The target was
         * verified BEFORE the reboot; mining stays unauthorized until the
         * future runtime owner re-verifies the live target identity and the
         * required mining evidence. ALLOW_TARGET_MINING is therefore never
         * emitted by this pure engine. */
        set_plan(p, POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET, POOL_BOOT_ALLOW_TARGET_ONLY,
                 POOL_BOOT_MINING_VERIFY_BEFORE_MINING,
                 POOL_BOOT_PERSIST_PROPOSE_COUNTER_UPDATE,
                 POOL_BOOT_RUNTIME_VERIFY_TARGET_CONFIGURATION, RECOVERY_OK,
                 RECOVERY_OK, true);
        p->remaining_valid = true;
        p->remaining_target_s = r.remaining_s;
        p->trusted_time_required = true; /* the resume rests on fresh trust */
        return;

    case POOL_TIME_DECISION_RESTORE_DUE:
        set_restore_plan(p, RECOVERY_ERR_DEADLINE_EXPIRED, rec->state, (uint16_t)ERR_NONE);
        return;

    case POOL_TIME_DECISION_FAIL_SAFE_RESTORE:
        set_restore_plan(p, map_time_status(r.status), rec->state,
                         (uint16_t)ERR_INTERRUPTED);
        return;

    case POOL_TIME_DECISION_RECOVERY_REQUIRED:
    default:
        set_plan(p, POOL_BOOT_DECISION_RECOVERY_REQUIRED, POOL_BOOT_ALLOW_SOURCE_ONLY,
                 POOL_BOOT_MINING_VERIFY_BEFORE_MINING,
                 POOL_BOOT_PERSIST_PROPOSE_RECOVERY_REQUIRED,
                 POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY,
                 RECOVERY_ERR_INVALID_ARGUMENT, RECOVERY_ERR_OPERATOR_REQUIRED, true);
        set_record_proposal(p, POOL_STATE_RECOVERY_REQUIRED, (uint16_t)ERR_RECOVERY_REQUIRED);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* The engine                                                          */
/* ------------------------------------------------------------------ */

PoolRecoveryError pool_session_recovery_plan(const PoolSessionBootContext *ctx,
                                             PoolSessionRecoveryPlan *out)
{
    const PoolSessionRecord *rec = NULL;
    PoolSessionResetClass rc;

    if (out == NULL) {
        return RECOVERY_ERR_INVALID_ARGUMENT;
    }
    plan_defaults_conservative(out);
    if (ctx == NULL) {
        plan_finalize_fingerprint(out);
        return out->error;
    }
    rc = ((unsigned)ctx->reset_class < (unsigned)POOL_RESET_CLASS__COUNT)
             ? ctx->reset_class
             : POOL_RESET_CLASS_UNKNOWN;

    /* ---- Stage-6 store-result decision table (total; no unsafe default) */
    switch (ctx->store_result) {
    case STORE_EMPTY:
    case STORE_CLEARED:
        if (ctx->record_present) {
            set_store_failure_plan(out, RECOVERY_ERR_INVALID_ARGUMENT);
            break;
        }
        set_plan(out, POOL_BOOT_DECISION_BOOT_NORMAL_SOURCE, POOL_BOOT_ALLOW_SOURCE_ONLY,
                 POOL_BOOT_MINING_ALLOW_SOURCE, POOL_BOOT_PERSIST_NO_RECORD_UPDATE,
                 POOL_BOOT_RUNTIME_START_SOURCE_MINING, RECOVERY_OK,
                 RECOVERY_ERR_STORE_EMPTY, true);
        break;

    case STORE_OK:
        if (!ctx->record_present || ctx->record == NULL) {
            set_store_failure_plan(out, RECOVERY_ERR_RECORD_MISSING);
            break;
        }
        rec = ctx->record;
        if (rec->kind != (uint8_t)POOL_RECORD_KIND_SESSION ||
            pool_session_record_validate(rec) != RECORD_OK) {
            set_store_failure_plan(out, RECOVERY_ERR_RECORD_INVALID);
            rec = NULL;
            break;
        }
        out->restore_required = rec->restore_required;

        /* ---- Stage-7 persisted-state decision table ---- */
        switch (rec->state) {
        case POOL_STATE_TARGET_SNAPSHOT_COMMITTED:
            /* Pre-mutation: the source is still the active config. Propose a
             * safe cancellation; never apply the target automatically. */
            set_plan(out, POOL_BOOT_DECISION_BOOT_NORMAL_SOURCE,
                     POOL_BOOT_ALLOW_SOURCE_ONLY, POOL_BOOT_MINING_ALLOW_SOURCE,
                     POOL_BOOT_PERSIST_PROPOSE_RECOVERY_STATE_UPDATE,
                     POOL_BOOT_RUNTIME_START_SOURCE_MINING, RECOVERY_OK, RECOVERY_OK,
                     true);
            set_record_proposal(out, POOL_STATE_CANCELLED, (uint16_t)ERR_INTERRUPTED);
            break;

        case POOL_STATE_APPLYING_TARGET:
        case POOL_STATE_TARGET_FAILED:
        case POOL_STATE_INTERRUPTED:
            /* Target mutation may have occurred: drive toward the source,
             * never retry the target automatically after a boot. */
            if (budget_exhausted_pre(rec, rc)) {
                set_exhausted_plan(out);
                break;
            }
            set_restore_plan(out, RECOVERY_OK, rec->state, (uint16_t)ERR_INTERRUPTED);
            break;

        case POOL_STATE_RESTORE_DUE:
        case POOL_STATE_APPLYING_RESTORE:
            if (budget_exhausted_pre(rec, rc)) {
                set_exhausted_plan(out);
                break;
            }
            set_restore_plan(out, RECOVERY_OK, rec->state, (uint16_t)ERR_INTERRUPTED);
            break;

        case POOL_STATE_TARGET_ACTIVE:
            plan_target_active(ctx, rec, rc, out);
            break;

        case POOL_STATE_RESTORE_FAILED:
            if (budget_exhausted_pre(rec, rc)) {
                set_exhausted_plan(out);
                break;
            }
            /* Bounded automatic re-attempt toward the source. */
            set_restore_plan(out, RECOVERY_OK, rec->state, rec->last_failure_code);
            out->persist_intent = POOL_BOOT_PERSIST_PROPOSE_RESTORE_DUE;
            set_record_proposal(out, POOL_STATE_RESTORE_DUE, rec->last_failure_code);
            break;

        case POOL_STATE_RECOVERY_REQUIRED:
            if (rec->restore_required) {
                if (budget_exhausted_pre(rec, rc)) {
                    set_exhausted_plan(out);
                    break;
                }
                set_restore_plan(out, RECOVERY_OK, rec->state, rec->last_failure_code);
                out->persist_intent = POOL_BOOT_PERSIST_PROPOSE_RESTORE_DUE;
                set_record_proposal(out, POOL_STATE_RESTORE_DUE, rec->last_failure_code);
            } else {
                /* Pre-mutation recovery result: source untouched; retain the
                 * result and surface the operator. */
                set_plan(out, POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE,
                         POOL_BOOT_ALLOW_SOURCE_ONLY, POOL_BOOT_MINING_ALLOW_SOURCE,
                         POOL_BOOT_PERSIST_RETAIN_TERMINAL_RESULT,
                         POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY, RECOVERY_OK,
                         RECOVERY_ERR_OPERATOR_REQUIRED, true);
            }
            break;

        case POOL_STATE_COMPLETE:
        case POOL_STATE_CANCELLED:
            /* Terminal results: keep them for acknowledgement; only the
             * source/default pool runs; B4 never clears anything. */
            set_plan(out, POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE,
                     POOL_BOOT_ALLOW_SOURCE_ONLY, POOL_BOOT_MINING_ALLOW_SOURCE,
                     POOL_BOOT_PERSIST_RETAIN_TERMINAL_RESULT,
                     POOL_BOOT_RUNTIME_START_SOURCE_MINING, RECOVERY_OK, RECOVERY_OK,
                     true);
            break;

        case POOL_STATE_IDLE:
        case POOL_STATE_PREPARING:
        case POOL_STATE_RESTARTING_FOR_TARGET:
        case POOL_STATE_VERIFYING_TARGET:
        case POOL_STATE_RESTARTING_FOR_RESTORE:
        case POOL_STATE_VERIFYING_RESTORE:
        default:
            /* Ephemeral/contradictory persisted states are unreachable
             * through the B3 validator; handled conservatively anyway. */
            set_store_failure_plan(out, RECOVERY_ERR_STATE_UNSUPPORTED);
            break;
        }
        break;

    case STORE_CORRUPT:
        set_store_failure_plan(out, RECOVERY_ERR_STORE_CORRUPT);
        break;
    case STORE_UNSUPPORTED_SCHEMA:
        set_store_failure_plan(out, RECOVERY_ERR_STORE_UNSUPPORTED);
        break;
    case STORE_ACTIVE_POINTER_INVALID:
    case STORE_ACTIVE_SLOT_INVALID:
    case STORE_RECOVERY_REQUIRED:
    case STORE_GENERATION_EXHAUSTED:
        set_store_failure_plan(out, RECOVERY_ERR_STORE_AMBIGUOUS);
        break;
    case STORE_COMMIT_UNCERTAIN:
    case STORE_IO_ERROR:
    case STORE_READBACK_MISMATCH:
        set_store_failure_plan(out, RECOVERY_ERR_STORE_UNCERTAIN);
        break;
    case STORE_INVALID_RECORD:
        set_store_failure_plan(out, RECOVERY_ERR_RECORD_INVALID);
        break;
    case STORE_NOT_INITIALIZED:
    case STORE_STATE_CONFLICT:
    case STORE_INVALID_ARGUMENT:
        set_store_failure_plan(out, RECOVERY_ERR_INVALID_ARGUMENT);
        break;
    default:
        /* A future PoolStoreResult value can never fall through to an
         * unsafe plan (the header also pins the enum count). */
        set_store_failure_plan(out, RECOVERY_ERR_STORE_AMBIGUOUS);
        break;
    }

    propose_counters(out, rec, rc);
    plan_finalize_fingerprint(out);
    return out->error;
}

/* ------------------------------------------------------------------ */
/* Stable machine tokens                                               */
/* ------------------------------------------------------------------ */

const char *pool_recovery_error_str(PoolRecoveryError e)
{
    switch (e) {
    case RECOVERY_OK:                              return "RECOVERY_OK";
    case RECOVERY_ERR_INVALID_ARGUMENT:            return "RECOVERY_ERR_INVALID_ARGUMENT";
    case RECOVERY_ERR_STORE_EMPTY:                 return "RECOVERY_ERR_STORE_EMPTY";
    case RECOVERY_ERR_STORE_CORRUPT:               return "RECOVERY_ERR_STORE_CORRUPT";
    case RECOVERY_ERR_STORE_UNSUPPORTED:           return "RECOVERY_ERR_STORE_UNSUPPORTED";
    case RECOVERY_ERR_STORE_AMBIGUOUS:             return "RECOVERY_ERR_STORE_AMBIGUOUS";
    case RECOVERY_ERR_STORE_UNCERTAIN:             return "RECOVERY_ERR_STORE_UNCERTAIN";
    case RECOVERY_ERR_RECORD_MISSING:              return "RECOVERY_ERR_RECORD_MISSING";
    case RECOVERY_ERR_RECORD_INVALID:              return "RECOVERY_ERR_RECORD_INVALID";
    case RECOVERY_ERR_SOURCE_MISSING:              return "RECOVERY_ERR_SOURCE_MISSING";
    case RECOVERY_ERR_TARGET_INVALID:              return "RECOVERY_ERR_TARGET_INVALID";
    case RECOVERY_ERR_RESTORE_OBLIGATION_MISMATCH: return "RECOVERY_ERR_RESTORE_OBLIGATION_MISMATCH";
    case RECOVERY_ERR_STATE_UNSUPPORTED:           return "RECOVERY_ERR_STATE_UNSUPPORTED";
    case RECOVERY_ERR_TIME_UNTRUSTED:              return "RECOVERY_ERR_TIME_UNTRUSTED";
    case RECOVERY_ERR_TIME_TIMEOUT:                return "RECOVERY_ERR_TIME_TIMEOUT";
    case RECOVERY_ERR_DEADLINE_MISSING:            return "RECOVERY_ERR_DEADLINE_MISSING";
    case RECOVERY_ERR_DEADLINE_EXPIRED:            return "RECOVERY_ERR_DEADLINE_EXPIRED";
    case RECOVERY_ERR_TIME_REGRESSION:             return "RECOVERY_ERR_TIME_REGRESSION";
    case RECOVERY_ERR_COUNTER_EXHAUSTED:           return "RECOVERY_ERR_COUNTER_EXHAUSTED";
    case RECOVERY_ERR_RESET_ABNORMAL:              return "RECOVERY_ERR_RESET_ABNORMAL";
    case RECOVERY_ERR_MINING_INHIBITION_UNAVAILABLE:
        return "RECOVERY_ERR_MINING_INHIBITION_UNAVAILABLE";
    case RECOVERY_ERR_OPERATOR_REQUIRED:           return "RECOVERY_ERR_OPERATOR_REQUIRED";
    default:                                       return "RECOVERY_ERR_UNKNOWN";
    }
}

const char *pool_boot_decision_str(PoolBootDecision d)
{
    switch (d) {
    case POOL_BOOT_DECISION_BOOT_NORMAL_SOURCE:    return "BOOT_NORMAL_SOURCE";
    case POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE:return "RETAIN_TERMINAL_SOURCE";
    case POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME: return "WAIT_FOR_TRUSTED_TIME";
    case POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET:return "RESUME_VERIFIED_TARGET";
    case POOL_BOOT_DECISION_RESTORE_SOURCE_NOW:    return "RESTORE_SOURCE_NOW";
    case POOL_BOOT_DECISION_VERIFY_RESTORE:        return "VERIFY_RESTORE";
    case POOL_BOOT_DECISION_RECOVERY_REQUIRED:     return "RECOVERY_REQUIRED";
    default:                                       return "BOOT_DECISION_UNKNOWN";
    }
}

const char *pool_boot_allowed_config_str(PoolBootAllowedConfig a)
{
    switch (a) {
    case POOL_BOOT_ALLOW_SOURCE_ONLY:               return "SOURCE_ONLY";
    case POOL_BOOT_ALLOW_TARGET_ONLY:               return "TARGET_ONLY";
    case POOL_BOOT_ALLOW_NO_POOL:                   return "NO_POOL_ALLOWED";
    case POOL_BOOT_ALLOW_CURRENT_CONFIG_UNVERIFIED: return "CURRENT_CONFIG_UNVERIFIED";
    default:                                        return "ALLOWED_CONFIG_UNKNOWN";
    }
}

const char *pool_boot_mining_policy_str(PoolBootMiningPolicy m)
{
    switch (m) {
    case POOL_BOOT_MINING_ALLOW_SOURCE:         return "ALLOW_SOURCE_MINING";
    case POOL_BOOT_MINING_ALLOW_TARGET:         return "ALLOW_TARGET_MINING";
    case POOL_BOOT_MINING_INHIBIT:              return "INHIBIT_MINING";
    case POOL_BOOT_MINING_VERIFY_BEFORE_MINING: return "VERIFY_BEFORE_MINING";
    default:                                    return "MINING_POLICY_UNKNOWN";
    }
}

const char *pool_boot_persist_intent_str(PoolBootPersistIntent p)
{
    switch (p) {
    case POOL_BOOT_PERSIST_NO_RECORD_UPDATE:              return "NO_RECORD_UPDATE";
    case POOL_BOOT_PERSIST_PROPOSE_RECOVERY_STATE_UPDATE: return "PROPOSE_RECOVERY_STATE_UPDATE";
    case POOL_BOOT_PERSIST_PROPOSE_COUNTER_UPDATE:        return "PROPOSE_COUNTER_UPDATE";
    case POOL_BOOT_PERSIST_PROPOSE_RESTORE_DUE:           return "PROPOSE_RESTORE_DUE";
    case POOL_BOOT_PERSIST_PROPOSE_RECOVERY_REQUIRED:     return "PROPOSE_RECOVERY_REQUIRED";
    case POOL_BOOT_PERSIST_RETAIN_TERMINAL_RESULT:        return "RETAIN_TERMINAL_RESULT";
    default:                                              return "PERSIST_INTENT_UNKNOWN";
    }
}

const char *pool_boot_runtime_intent_str(PoolBootRuntimeIntent r)
{
    switch (r) {
    case POOL_BOOT_RUNTIME_NONE:                        return "RUNTIME_NONE";
    case POOL_BOOT_RUNTIME_START_TRUSTED_TIME_WAIT:     return "START_TRUSTED_TIME_WAIT";
    case POOL_BOOT_RUNTIME_APPLY_SOURCE_CONFIGURATION:  return "APPLY_SOURCE_CONFIGURATION";
    case POOL_BOOT_RUNTIME_VERIFY_SOURCE_CONFIGURATION: return "VERIFY_SOURCE_CONFIGURATION";
    case POOL_BOOT_RUNTIME_START_SOURCE_MINING:         return "START_SOURCE_MINING";
    case POOL_BOOT_RUNTIME_RESUME_TARGET_CONFIGURATION: return "RESUME_TARGET_CONFIGURATION";
    case POOL_BOOT_RUNTIME_VERIFY_TARGET_CONFIGURATION: return "VERIFY_TARGET_CONFIGURATION";
    case POOL_BOOT_RUNTIME_HOLD_STRATUM:                return "HOLD_STRATUM";
    case POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY:   return "SURFACE_OPERATOR_RECOVERY";
    default:                                            return "RUNTIME_INTENT_UNKNOWN";
    }
}
