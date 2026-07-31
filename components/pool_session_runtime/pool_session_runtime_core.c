/*
 * NeuralAxe timed pool sessions — PURE runtime controller (Gate B6).
 *
 * See pool_session_runtime_core.h for the full contract. PURE: no ESP-IDF,
 * no NVS, no SNTP, no FreeRTOS, no heap, no logging, no clock, no global
 * mutable state. Every table below is total and fails CLOSED.
 */

#include <string.h>
#include "pool_session_runtime_core.h"

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

uint32_t pool_runtime_event_sanitize(uint32_t raw_events)
{
    return raw_events & (uint32_t)RUNTIME_EVENT__ALL_VALID;
}

bool pool_runtime_event_is_known(uint32_t event_bit)
{
    if (event_bit == 0u) {
        return false;
    }
    /* exactly one bit, and that bit is known */
    if ((event_bit & (event_bit - 1u)) != 0u) {
        return false;
    }
    return (event_bit & (uint32_t)RUNTIME_EVENT__ALL_VALID) != 0u;
}

const char *pool_runtime_event_str(uint32_t event_bit)
{
    switch (event_bit) {
    case RUNTIME_EVENT_BOOTSTRAP_COMPLETE:    return "bootstrap_complete";
    case RUNTIME_EVENT_NETWORK_READY:         return "network_ready";
    case RUNTIME_EVENT_TIME_SYNC_CHANGED:     return "time_sync_changed";
    case RUNTIME_EVENT_MONOTONIC_BOUNDARY:    return "monotonic_boundary";
    case RUNTIME_EVENT_STORE_RELOAD_REQUIRED: return "store_reload_required";
    case RUNTIME_EVENT_SHUTDOWN_FOR_TEST:     return "shutdown_for_test";
    default:                                  return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Feature-flag boot action                                            */
/* ------------------------------------------------------------------ */

PoolRuntimeBootAction pool_runtime_boot_action_for_feature(bool feature_enabled)
{
    PoolRuntimeBootAction a;
    memset(&a, 0, sizeof(a));
    if (!feature_enabled) {
        /* No instance, no task, no store open, no SNTP: the existing boot
         * path is byte-for-byte unchanged, so protocol startup proceeds. */
        a.bootstrap_required = false;
        a.protocol_allowed   = true;
        return a;
    }
    /* Enabled: the real answer comes from the runtime decision; until then
     * the pre-decision default is fail-closed. */
    a.bootstrap_required = true;
    a.protocol_allowed   = false;
    return a;
}

/* ------------------------------------------------------------------ */
/* Protocol permission (total, fail-closed)                            */
/* ------------------------------------------------------------------ */

PoolRuntimeProtocolPermission pool_runtime_protocol_for_state(PoolRuntimeState s)
{
    switch (s) {
    case RUNTIME_FREE:
    case RUNTIME_TERMINAL_PENDING:
        return POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE;

    case RUNTIME_UNINITIALIZED:
    case RUNTIME_BOOTSTRAPPING:
    case RUNTIME_PERSISTENCE_PENDING:
    case RUNTIME_WAITING_FOR_TRUSTED_TIME:
    case RUNTIME_VERIFY_TARGET_PENDING:
    case RUNTIME_RESTORE_SOURCE_PENDING:
    case RUNTIME_OPERATOR_RECOVERY:
    case RUNTIME_RECOVERY_GUARD:
    case RUNTIME_STOPPED:
    case RUNTIME_ERROR:
    default:
        return POOL_RUNTIME_PROTOCOL_HOLD;
    }
}

bool pool_runtime_state_holds_protocol(PoolRuntimeState s)
{
    return pool_runtime_protocol_for_state(s) == POOL_RUNTIME_PROTOCOL_HOLD;
}

/* ------------------------------------------------------------------ */
/* Classification                                                      */
/* ------------------------------------------------------------------ */

static void decide(PoolRuntimeDecision *out, PoolRuntimeState st,
                   PoolRuntimeStatus status)
{
    memset(out, 0, sizeof(*out));
    out->state    = st;
    out->status   = status;
    out->protocol = pool_runtime_protocol_for_state(st);
    /* target_mining_authorized and pool_mutation_permitted stay false:
     * Gate B6 has no path that sets either, on any input. */
}

/* Is the plan structurally usable at all? A plan that claims a target-mining
 * grant is impossible from B4 and is treated as an internal inconsistency. */
static bool plan_structurally_sane(const PoolSessionRecoveryPlan *p)
{
    if (p == NULL) {
        return false;
    }
    if ((unsigned)p->decision >= (unsigned)POOL_BOOT_DECISION__COUNT) {
        return false;
    }
    if ((unsigned)p->allowed_config >= (unsigned)POOL_BOOT_ALLOW__COUNT) {
        return false;
    }
    if ((unsigned)p->mining_policy >= (unsigned)POOL_BOOT_MINING__COUNT) {
        return false;
    }
    /* B4 never emits ALLOW_TARGET_MINING; seeing it means the plan did not
     * come from the committed engine. Fail closed. */
    if (p->mining_policy == POOL_BOOT_MINING_ALLOW_TARGET) {
        return false;
    }
    return true;
}

PoolRuntimeStatus pool_runtime_classify(const PoolRuntimeClassifyInput *in,
                                        PoolRuntimeDecision *out)
{
    const PoolSessionRecoveryPlan *plan;
    bool obligation;

    if (out == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (in == NULL) {
        decide(out, RUNTIME_ERROR, RUNTIME_ERR_INVALID_ARGUMENT);
        return out->status;
    }

    /* ---- 1. Store lifecycle. A failed open or load is NEVER "empty". ---- */
    if (!in->store_opened) {
        decide(out, RUNTIME_RECOVERY_GUARD, RUNTIME_ERR_STORE_OPEN_FAILED);
        return out->status;
    }
    if (!in->store_loaded) {
        decide(out, RUNTIME_RECOVERY_GUARD, RUNTIME_ERR_STORE_LOAD_FAILED);
        return out->status;
    }
    if ((unsigned)in->store_result >= (unsigned)POOL_STORE_RESULT__COUNT) {
        decide(out, RUNTIME_RECOVERY_GUARD, RUNTIME_ERR_INTERNAL_CONSISTENCY);
        return out->status;
    }

    /* ---- 2. The B4 plan must exist and be structurally sane. ---- */
    plan = in->plan;
    if (!plan_structurally_sane(plan)) {
        decide(out, RUNTIME_RECOVERY_GUARD, RUNTIME_ERR_PLAN_INVALID);
        return out->status;
    }

    /* ---- 3. B5 bootstrap must have succeeded. ---- */
    if (in->bootstrap_status != OP_OK) {
        decide(out, RUNTIME_ERROR, RUNTIME_ERR_BOOTSTRAP_FAILED);
        return out->status;
    }
    if ((unsigned)in->lease_phase >= (unsigned)OP_PHASE__COUNT ||
        (unsigned)in->lease_owner >= (unsigned)OP_OWNER__COUNT) {
        decide(out, RUNTIME_RECOVERY_GUARD, RUNTIME_ERR_INTERNAL_CONSISTENCY);
        return out->status;
    }

    obligation = in->lease_restore_required || plan->restore_required;

    /* ---- 4. An uncertain commit (load or proposal) is always the guard. ---- */
    if (in->store_result == STORE_COMMIT_UNCERTAIN) {
        decide(out, RUNTIME_RECOVERY_GUARD, RUNTIME_ERR_STORE_UNCERTAIN);
        out->restore_required = obligation;
        return out->status;
    }
    if (in->persist_attempted && in->persist_result == STORE_COMMIT_UNCERTAIN) {
        decide(out, RUNTIME_RECOVERY_GUARD, RUNTIME_ERR_STORE_UNCERTAIN);
        out->restore_required = obligation;
        return out->status;
    }

    /* ---- 5. An operator-recovery lease dominates every other posture. ---- */
    if (in->lease_owner == OP_OWNER_OPERATOR_RECOVERY) {
        decide(out, RUNTIME_OPERATOR_RECOVERY, RUNTIME_OK);
        out->restore_required = obligation;
        return out->status;
    }

    /* ---- 6. The reconstructed guard. ---- */
    if (in->lease_phase == OP_PHASE_RECOVERY_GUARD ||
        in->lease_owner == OP_OWNER_RECOVERY_GUARD) {
        decide(out, RUNTIME_RECOVERY_GUARD, RUNTIME_OK);
        out->restore_required = obligation;
        return out->status;
    }

    /* ---- 7. Persistence before action. A required, not-yet-verified
     *         proposal holds regardless of what the plan wants next. ---- */
    if (pool_runtime_plan_requires_persistence(plan) || in->lease_persistence_required) {
        if (!in->persist_verified) {
            PoolRuntimeStatus why = RUNTIME_ERR_PERSIST_REQUIRED;
            if (in->persist_attempted) {
                why = (in->persist_result == STORE_OK) ? RUNTIME_ERR_PERSIST_READBACK
                                                       : RUNTIME_ERR_PERSIST_FAILED;
            }
            decide(out, RUNTIME_PERSISTENCE_PENDING, why);
            out->persistence_pending = true;
            out->restore_required    = obligation;
            return out->status;
        }
    }

    /* ---- 8. The agreed posture: B5 phase is authoritative, cross-checked
     *         against the B4 decision. Any disagreement fails closed. ---- */
    switch (in->lease_phase) {

    case OP_PHASE_FREE:
        if (in->lease_owner != OP_OWNER_NONE) {
            break; /* inconsistent: guard */
        }
        if (in->terminal_pending) {
            /* Safe retained COMPLETE / pre-mutation CANCELLED. */
            if (obligation || plan->decision != POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE ||
                plan->allowed_config != POOL_BOOT_ALLOW_SOURCE_ONLY) {
                break;
            }
            decide(out, RUNTIME_TERMINAL_PENDING, RUNTIME_OK);
            return out->status;
        }
        /* Proven no session facts: EMPTY or CLEARED only. */
        if (obligation || in->record_present ||
            (in->store_result != STORE_EMPTY && in->store_result != STORE_CLEARED) ||
            plan->decision != POOL_BOOT_DECISION_BOOT_NORMAL_SOURCE ||
            plan->allowed_config != POOL_BOOT_ALLOW_SOURCE_ONLY) {
            break;
        }
        decide(out, RUNTIME_FREE, RUNTIME_OK);
        return out->status;

    case OP_PHASE_RESERVED_PENDING_PERSISTENCE:
        /* Owned, external mutation blocked, awaiting the persistence proof. */
        decide(out, RUNTIME_PERSISTENCE_PENDING, RUNTIME_ERR_PERSIST_REQUIRED);
        out->persistence_pending = true;
        out->restore_required    = obligation;
        return out->status;

    case OP_PHASE_WAITING_FOR_TRUSTED_TIME:
        if (plan->decision != POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME ||
            plan->mining_policy != POOL_BOOT_MINING_INHIBIT) {
            break;
        }
        decide(out, RUNTIME_WAITING_FOR_TRUSTED_TIME, RUNTIME_ERR_TIME_WAIT_PENDING);
        out->trusted_time_required = true;
        out->restore_required      = obligation;
        return out->status;

    case OP_PHASE_VERIFYING_TARGET:
        /* Eligibility ONLY. Never a mining grant, never a pool mutation. */
        if (plan->decision != POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET ||
            plan->mining_policy != POOL_BOOT_MINING_VERIFY_BEFORE_MINING) {
            break;
        }
        decide(out, RUNTIME_VERIFY_TARGET_PENDING, RUNTIME_OK);
        out->trusted_time_required = plan->trusted_time_required;
        out->restore_required      = obligation;
        return out->status;

    case OP_PHASE_RESTORING_SOURCE:
        /* Restore is OWED and PENDING. Gate B6 executes nothing. */
        if (in->lease_owner != OP_OWNER_SOURCE_RESTORE ||
            (plan->decision != POOL_BOOT_DECISION_RESTORE_SOURCE_NOW &&
             plan->decision != POOL_BOOT_DECISION_VERIFY_RESTORE)) {
            break;
        }
        decide(out, RUNTIME_RESTORE_SOURCE_PENDING, RUNTIME_OK);
        out->restore_required = obligation;
        return out->status;

    case OP_PHASE_UNBOOTSTRAPPED:
    case OP_PHASE_ACTIVE:
    case OP_PHASE_TERMINAL_ACK_PENDING:
    case OP_PHASE_RELEASING:
    case OP_PHASE_RECOVERY_GUARD:
    default:
        /* Gate B6 never reconstructs these at boot. Fail closed. */
        break;
    }

    decide(out, RUNTIME_RECOVERY_GUARD, RUNTIME_ERR_OWNERSHIP_MISMATCH);
    out->restore_required = obligation;
    return out->status;
}

/* ------------------------------------------------------------------ */
/* Snapshot                                                            */
/* ------------------------------------------------------------------ */

void pool_runtime_snapshot_init(PoolRuntimeSnapshot *snap)
{
    if (snap == NULL) {
        return;
    }
    memset(snap, 0, sizeof(*snap));
    snap->model_version = POOL_RUNTIME_MODEL_VERSION;
    snap->state         = RUNTIME_UNINITIALIZED;
    snap->protocol      = POOL_RUNTIME_PROTOCOL_HOLD;
    snap->status        = RUNTIME_ERR_NOT_INITIALIZED;
}

PoolRuntimeStatus pool_runtime_snapshot_build(const PoolRuntimeClassifyInput *in,
                                              const PoolRuntimeDecision *dec,
                                              PoolSessionResetClass reset_class,
                                              uint32_t committed_generation,
                                              uint32_t wait_elapsed_s,
                                              uint32_t wait_limit_s,
                                              uint32_t proposal_commits,
                                              bool task_running,
                                              PoolRuntimeSnapshot *snap)
{
    pool_runtime_snapshot_init(snap);
    if (snap == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (in == NULL || dec == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }

    snap->state    = dec->state;
    snap->status   = dec->status;
    /* Never trust a caller-supplied permission: re-derive from the state. */
    snap->protocol = pool_runtime_protocol_for_state(dec->state);

    snap->store_result = in->store_result;
    if (in->plan != NULL) {
        snap->boot_decision    = in->plan->decision;
        snap->allowed_config   = in->plan->allowed_config;
        snap->mining_policy    = in->plan->mining_policy;
        snap->recovery_error   = in->plan->error;
        snap->plan_fingerprint = in->plan->plan_fingerprint;
    }
    snap->lease_owner      = in->lease_owner;
    snap->lease_phase      = in->lease_phase;
    snap->lease_generation = in->lease_generation;
    snap->reset_class      = ((unsigned)reset_class < (unsigned)POOL_RESET_CLASS__COUNT)
                                 ? reset_class
                                 : POOL_RESET_CLASS_UNKNOWN;
    snap->committed_generation = committed_generation;

    /* Presence only — the session identifier is deliberately NOT published. */
    snap->session_present = (in->record_present && in->record != NULL &&
                             in->record->kind == (uint8_t)POOL_RECORD_KIND_SESSION);

    snap->restore_required      = dec->restore_required;
    snap->trusted_time_required = dec->trusted_time_required;
    snap->persistence_pending   = dec->persistence_pending;
    /* Structurally pinned Gate B6 prohibitions. */
    snap->target_mining_authorized = false;
    snap->pool_mutation_permitted  = false;
    snap->task_running             = task_running;

    if (in->record_present && in->record != NULL) {
        snap->reboot_count                  = in->record->reboot_count;
        snap->recovery_attempt_count        = in->record->recovery_attempt_count;
        snap->consecutive_recovery_failures = in->record->consecutive_recovery_failures;
    }
    snap->trusted_time_wait_elapsed_s = wait_elapsed_s;
    snap->trusted_time_wait_limit_s   = wait_limit_s;
    snap->proposal_commits            = proposal_commits;

    return RUNTIME_OK;
}

bool pool_runtime_snapshot_valid(const PoolRuntimeSnapshot *snap)
{
    if (snap == NULL) {
        return false;
    }
    if (snap->model_version != POOL_RUNTIME_MODEL_VERSION) {
        return false;
    }
    if ((unsigned)snap->state >= (unsigned)POOL_RUNTIME_STATE__COUNT) {
        return false;
    }
    if ((unsigned)snap->protocol >= (unsigned)POOL_RUNTIME_PROTOCOL__COUNT) {
        return false;
    }
    if ((unsigned)snap->status >= (unsigned)POOL_RUNTIME_STATUS__COUNT) {
        return false;
    }
    if ((unsigned)snap->store_result >= (unsigned)POOL_STORE_RESULT__COUNT) {
        return false;
    }
    if ((unsigned)snap->boot_decision >= (unsigned)POOL_BOOT_DECISION__COUNT) {
        return false;
    }
    if ((unsigned)snap->allowed_config >= (unsigned)POOL_BOOT_ALLOW__COUNT) {
        return false;
    }
    if ((unsigned)snap->mining_policy >= (unsigned)POOL_BOOT_MINING__COUNT) {
        return false;
    }
    if ((unsigned)snap->recovery_error >= (unsigned)POOL_RECOVERY_ERR__COUNT) {
        return false;
    }
    if ((unsigned)snap->lease_owner >= (unsigned)OP_OWNER__COUNT) {
        return false;
    }
    if ((unsigned)snap->lease_phase >= (unsigned)OP_PHASE__COUNT) {
        return false;
    }
    if ((unsigned)snap->reset_class >= (unsigned)POOL_RESET_CLASS__COUNT) {
        return false;
    }
    /* The permission must always match the total state rule. */
    if (snap->protocol != pool_runtime_protocol_for_state(snap->state)) {
        return false;
    }
    /* Gate B6 prohibitions. */
    if (snap->target_mining_authorized || snap->pool_mutation_permitted) {
        return false;
    }
    if (snap->trusted_time_wait_elapsed_s > snap->trusted_time_wait_limit_s) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Bounded runtime control                                             */
/* ------------------------------------------------------------------ */

void pool_runtime_control_init(PoolRuntimeControl *c, uint32_t wait_limit_s)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->state      = RUNTIME_UNINITIALIZED;
    c->wait_limit_s = wait_limit_s;
}

PoolRuntimeStatus pool_runtime_control_adopt(PoolRuntimeControl *c,
                                             const PoolRuntimeDecision *dec)
{
    if (c == NULL || dec == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if ((unsigned)dec->state >= (unsigned)POOL_RUNTIME_STATE__COUNT) {
        c->state = RUNTIME_ERROR;
        return RUNTIME_ERR_INTERNAL_CONSISTENCY;
    }
    c->state = dec->state;
    return RUNTIME_OK;
}

PoolRuntimeStatus pool_runtime_control_apply(PoolRuntimeControl *c,
                                             uint32_t raw_events,
                                             PoolRuntimeEventOutcome *out)
{
    uint32_t         events;
    PoolRuntimeState before;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (c == NULL || out == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if ((unsigned)c->state >= (unsigned)POOL_RUNTIME_STATE__COUNT) {
        c->state   = RUNTIME_ERROR;
        out->state = c->state;
        return RUNTIME_ERR_INTERNAL_CONSISTENCY;
    }

    events              = pool_runtime_event_sanitize(raw_events);
    out->ignored_events = raw_events & ~(uint32_t)RUNTIME_EVENT__ALL_VALID;
    before              = c->state;

    if (events == 0u) {
        out->state = c->state;
        /* Unknown-only batches are dropped: nothing happens, nothing is
         * released, and the call still succeeds deterministically. */
        return (out->ignored_events != 0u) ? RUNTIME_ERR_UNSUPPORTED_EVENT : RUNTIME_OK;
    }

    if (events & RUNTIME_EVENT_BOOTSTRAP_COMPLETE) {
        /* Idempotent: a repeat sets nothing new and requests no work. */
        if (!c->bootstrap_complete) {
            c->bootstrap_complete = true;
            out->applied_events |= RUNTIME_EVENT_BOOTSTRAP_COMPLETE;
        }
    }

    if (events & RUNTIME_EVENT_NETWORK_READY) {
        if (!c->network_ready) {
            c->network_ready = true;
            out->applied_events |= RUNTIME_EVENT_NETWORK_READY;
            /* Only a state that is genuinely waiting for trusted time may
             * cause the provider to start. Nothing else touches the network. */
            if (c->state == RUNTIME_WAITING_FOR_TRUSTED_TIME) {
                out->start_time_provider = true;
            }
        }
    }

    if (events & RUNTIME_EVENT_TIME_SYNC_CHANGED) {
        c->time_sync_seen = true;
        out->applied_events |= RUNTIME_EVENT_TIME_SYNC_CHANGED;
        if (c->state == RUNTIME_WAITING_FOR_TRUSTED_TIME) {
            out->reevaluate_plan = true;
        }
    }

    if (events & RUNTIME_EVENT_MONOTONIC_BOUNDARY) {
        out->applied_events |= RUNTIME_EVENT_MONOTONIC_BOUNDARY;
        if (c->state == RUNTIME_WAITING_FOR_TRUSTED_TIME) {
            out->reevaluate_plan = true;
        }
    }

    if (events & RUNTIME_EVENT_STORE_RELOAD_REQUIRED) {
        out->applied_events |= RUNTIME_EVENT_STORE_RELOAD_REQUIRED;
        out->reload_store = true;
    }

    if (events & RUNTIME_EVENT_SHUTDOWN_FOR_TEST) {
        if (!c->shutdown_requested) {
            c->shutdown_requested = true;
            out->applied_events |= RUNTIME_EVENT_SHUTDOWN_FOR_TEST;
        }
        /* Stopping the task NEVER releases a protocol hold and never frees
         * ownership: the recorded state is preserved for the barrier. */
        out->stop_task = true;
    }

    out->state         = c->state;
    out->state_changed = (before != c->state);
    return RUNTIME_OK;
}

bool pool_runtime_wait_expired(uint32_t elapsed_s, uint32_t limit_s)
{
    if (limit_s == 0u) {
        return true; /* a zero window is already over — fail safe */
    }
    return elapsed_s >= limit_s;
}

bool pool_runtime_control_advance_wait(PoolRuntimeControl *c, uint32_t delta_s)
{
    uint32_t next;

    if (c == NULL) {
        return true; /* fail safe */
    }
    if (c->wait_limit_s == 0u) {
        return true;
    }
    next = c->wait_elapsed_s + delta_s;
    if (next < c->wait_elapsed_s || next > c->wait_limit_s) {
        next = c->wait_limit_s; /* saturate; never wrap, never exceed */
    }
    c->wait_elapsed_s = next;
    return pool_runtime_wait_expired(c->wait_elapsed_s, c->wait_limit_s);
}

/* ------------------------------------------------------------------ */
/* Persistence-before-action guards                                    */
/* ------------------------------------------------------------------ */

bool pool_runtime_plan_requires_persistence(const PoolSessionRecoveryPlan *plan)
{
    if (plan == NULL) {
        return false;
    }
    return plan->counters.must_persist_before_action || plan->record_proposal.update_needed;
}

bool pool_runtime_proposal_should_commit(const PoolRuntimeControl *c,
                                         uint32_t plan_fingerprint)
{
    if (c == NULL) {
        return false;
    }
    if (c->proposal_committed && c->proposal_fingerprint == plan_fingerprint) {
        return false; /* identical proposal already durable this boot */
    }
    return true;
}

PoolRuntimeStatus pool_runtime_proposal_record_commit(PoolRuntimeControl *c,
                                                      uint32_t plan_fingerprint)
{
    if (c == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!pool_runtime_proposal_should_commit(c, plan_fingerprint)) {
        return RUNTIME_ERR_PERSIST_DUPLICATE; /* no counter moves */
    }
    c->proposal_committed   = true;
    c->proposal_fingerprint = plan_fingerprint;
    if (c->proposal_commits < UINT32_MAX) {
        c->proposal_commits++;
    }
    return RUNTIME_OK;
}

PoolRuntimeStatus pool_runtime_verify_proposal_readback(
    const PoolSessionRecord *before, const PoolSessionRecord *reloaded,
    const PoolSessionRecoveryPlan *plan, PoolStoreResult reload_result)
{
    if (before == NULL || reloaded == NULL || plan == NULL) {
        return RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (reload_result != STORE_OK) {
        return RUNTIME_ERR_PERSIST_READBACK;
    }
    if (reloaded->kind != (uint8_t)POOL_RECORD_KIND_SESSION) {
        return RUNTIME_ERR_PERSIST_READBACK;
    }
    /* Identity must be preserved exactly. */
    if (reloaded->session_id != before->session_id) {
        return RUNTIME_ERR_PERSIST_READBACK;
    }
    /* The restore obligation is NEVER discharged by a B6 proposal. */
    if (reloaded->restore_required != before->restore_required) {
        return RUNTIME_ERR_PERSIST_READBACK;
    }
    /* The persisted trusted-epoch floor is never lowered. */
    if (before->latest_trusted_valid) {
        if (!reloaded->latest_trusted_valid ||
            reloaded->latest_trusted_epoch_s < before->latest_trusted_epoch_s) {
            return RUNTIME_ERR_PERSIST_READBACK;
        }
    }
    /* A commit must produce a strictly newer committed generation. */
    if (reloaded->generation <= before->generation) {
        return RUNTIME_ERR_PERSIST_READBACK;
    }
    /* The proposed state / failure code must be exactly what landed. */
    if (plan->record_proposal.update_needed) {
        if (reloaded->state != plan->record_proposal.proposed_state) {
            return RUNTIME_ERR_PERSIST_READBACK;
        }
        if (reloaded->last_failure_code != plan->record_proposal.proposed_failure_code) {
            return RUNTIME_ERR_PERSIST_READBACK;
        }
    } else if (reloaded->state != before->state) {
        return RUNTIME_ERR_PERSIST_READBACK;
    }
    /* The proposed counters must be exactly what landed. */
    if (reloaded->reboot_count != plan->counters.reboot_count ||
        reloaded->recovery_attempt_count != plan->counters.recovery_attempt_count ||
        reloaded->consecutive_recovery_failures != plan->counters.consecutive_recovery_failures ||
        reloaded->last_reset_class != plan->counters.reset_class_for_record) {
        return RUNTIME_ERR_PERSIST_READBACK;
    }
    return RUNTIME_OK;
}

/* ------------------------------------------------------------------ */
/* Stable machine tokens                                               */
/* ------------------------------------------------------------------ */

const char *pool_runtime_state_str(PoolRuntimeState s)
{
    switch (s) {
    case RUNTIME_UNINITIALIZED:            return "runtime_uninitialized";
    case RUNTIME_BOOTSTRAPPING:            return "runtime_bootstrapping";
    case RUNTIME_FREE:                     return "runtime_free";
    case RUNTIME_TERMINAL_PENDING:         return "runtime_terminal_pending";
    case RUNTIME_PERSISTENCE_PENDING:      return "runtime_persistence_pending";
    case RUNTIME_WAITING_FOR_TRUSTED_TIME: return "runtime_waiting_for_trusted_time";
    case RUNTIME_VERIFY_TARGET_PENDING:    return "runtime_verify_target_pending";
    case RUNTIME_RESTORE_SOURCE_PENDING:   return "runtime_restore_source_pending";
    case RUNTIME_OPERATOR_RECOVERY:        return "runtime_operator_recovery";
    case RUNTIME_RECOVERY_GUARD:           return "runtime_recovery_guard";
    case RUNTIME_STOPPED:                  return "runtime_stopped";
    case RUNTIME_ERROR:                    return "runtime_error";
    case POOL_RUNTIME_STATE__COUNT:
    default:                               return "runtime_invalid";
    }
}

const char *pool_runtime_status_str(PoolRuntimeStatus st)
{
    switch (st) {
    case RUNTIME_OK:                           return "runtime_ok";
    case RUNTIME_ERR_INVALID_ARGUMENT:         return "runtime_invalid_argument";
    case RUNTIME_ERR_FEATURE_DISABLED:         return "runtime_feature_disabled";
    case RUNTIME_ERR_ALREADY_INITIALIZED:      return "runtime_already_initialized";
    case RUNTIME_ERR_NOT_INITIALIZED:          return "runtime_not_initialized";
    case RUNTIME_ERR_STORE_OPEN_FAILED:        return "runtime_store_open_failed";
    case RUNTIME_ERR_STORE_LOAD_FAILED:        return "runtime_store_load_failed";
    case RUNTIME_ERR_STORE_UNCERTAIN:          return "runtime_store_uncertain";
    case RUNTIME_ERR_PLAN_INVALID:             return "runtime_plan_invalid";
    case RUNTIME_ERR_BOOTSTRAP_FAILED:         return "runtime_bootstrap_failed";
    case RUNTIME_ERR_OWNERSHIP_MISMATCH:       return "runtime_ownership_mismatch";
    case RUNTIME_ERR_PERSIST_REQUIRED:         return "runtime_persist_required";
    case RUNTIME_ERR_PERSIST_FAILED:           return "runtime_persist_failed";
    case RUNTIME_ERR_PERSIST_READBACK:         return "runtime_persist_readback";
    case RUNTIME_ERR_PERSIST_DUPLICATE:        return "runtime_persist_duplicate";
    case RUNTIME_ERR_TIME_SOURCE_UNCONFIGURED: return "runtime_time_source_unconfigured";
    case RUNTIME_ERR_TIME_WAIT_PENDING:        return "runtime_time_wait_pending";
    case RUNTIME_ERR_TIME_WAIT_EXPIRED:        return "runtime_time_wait_expired";
    case RUNTIME_ERR_TASK_CREATE_FAILED:       return "runtime_task_create_failed";
    case RUNTIME_ERR_TASK_ALREADY_RUNNING:     return "runtime_task_already_running";
    case RUNTIME_ERR_UNSUPPORTED_EVENT:        return "runtime_unsupported_event";
    case RUNTIME_ERR_INTERNAL_CONSISTENCY:     return "runtime_internal_consistency";
    case POOL_RUNTIME_STATUS__COUNT:
    default:                                   return "runtime_status_invalid";
    }
}

const char *pool_runtime_protocol_str(PoolRuntimeProtocolPermission p)
{
    switch (p) {
    case POOL_RUNTIME_PROTOCOL_HOLD:         return "protocol_hold";
    case POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE: return "protocol_allow_source";
    case POOL_RUNTIME_PROTOCOL__COUNT:
    default:                                 return "protocol_invalid";
    }
}
