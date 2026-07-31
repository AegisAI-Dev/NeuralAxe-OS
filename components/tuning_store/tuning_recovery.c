/*
 * NeuralAxe Weather-Aware Tuning — pure boot-recovery decision (Gate W2).
 * PURE: no NVS, no clocks, no logging, no heap, no tuning application.
 */

#include <string.h>
#include "tuning_recovery.h"

static void copy_id(char *dst, const char *src)
{
    memset(dst, 0, TUNING_PROFILE_ID_MAX);
    if (src != NULL) {
        strncpy(dst, src, TUNING_PROFILE_ID_MAX - 1u);
    }
}

void tuning_boot_context_init(TuningBootContext *ctx)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
}

void tuning_boot_context_note_committed(TuningBootContext *ctx,
                                        uint32_t record_generation,
                                        uint8_t attempt_count)
{
    if (ctx == NULL) {
        return;
    }
    ctx->attempt_committed_this_boot = true;
    ctx->committed_record_generation = record_generation;
    ctx->committed_attempt_count = attempt_count;
}

/* Conservative default: RECOVERY_REQUIRED, upgrades inhibited, operator
 * surfaced, no proposal action beyond the recovery state itself. */
static void plan_conservative(TuningBootPlan *out, TuningBootCode code)
{
    memset(out, 0, sizeof(*out));
    out->decision = TUNING_BOOT_RECOVERY_REQUIRED; /* == 0, explicit anyway */
    out->upgrade_inhibit = true;
    out->operator_surface = true;
    out->proposed_tx_state = TUNING_TX_RECOVERY_REQUIRED;
    out->primary_code = code;
}

/*
 * Shared handler for an interrupted transaction (the five pending-apply
 * states and the two rollback states). NEVER assumes the requested profile
 * succeeded; the bounded boot-attempt budget prevents reboot loops. The
 * counter advances ONLY via the emitted persist-before-action proposal —
 * this function mutates nothing.
 *
 * Same-boot vs new-boot (owner final-blocker contract): when the RAM-only
 * boot context proves the increment for THIS boot was already durably
 * committed and read back for exactly this record (generation + count
 * match), the plan RESUMES the reserved attempt — no further increment,
 * rollback_action_eligible = true. Otherwise (fresh boot, or no durable
 * reservation yet) the plan proposes one increment and execution stays
 * ineligible until it is persisted and noted.
 */
static void plan_interrupted(const TuningBootInput *in,
                             const TuningPolicyRecord *rec, bool rolling,
                             TuningBootPlan *out)
{
    uint8_t next_attempts = tuning_record_counter_increment(
        rec->tx.boot_attempt_count, (uint8_t)TUNING_TX_BOOT_ATTEMPT_STORE_MAX);
    TuningBootCode why = rolling ? TUNING_BOOT_CODE_ROLLBACK_IN_PROGRESS
                                 : TUNING_BOOT_CODE_TX_INTERRUPTED;
    bool reserved_this_boot =
        in->context.attempt_committed_this_boot &&
        in->context.committed_record_generation == rec->generation &&
        in->context.committed_attempt_count == rec->tx.boot_attempt_count;

    if (!reserved_this_boot &&
        rec->tx.boot_attempt_count >= TUNING_BOOT_MAX_TX_BOOT_ATTEMPTS) {
        /* A NEW reservation at or beyond the threshold (including
         * corrupted large values): RECOVERY_REQUIRED, never another
         * automatic attempt. An already-reserved attempt (same boot) is
         * not re-judged — its reservation was granted below the limit. */
        plan_conservative(out, TUNING_BOOT_CODE_BOOT_BUDGET_EXHAUSTED);
        out->secondary_code = why;
        out->proposed_boot_attempt_count = next_attempts;
        return;
    }

    /* Rollback target: an in-progress rollback RESUMES its recorded
     * target (never a second independent rollback transaction);
     * otherwise the last-known-safe profile. Ids pass through as
     * recorded — W1 eligibility runs at execution time (W4). */
    {
        const char *target = NULL;
        if (rolling && rec->tx.rollback_profile_id[0] != '\0') {
            target = rec->tx.rollback_profile_id;
        } else if (rec->last_known_safe_id[0] != '\0') {
            target = rec->last_known_safe_id;
        }

        memset(out, 0, sizeof(*out));
        out->upgrade_inhibit = true; /* never auto-upgrade after this */
        out->primary_code = why;
        out->proposed_boot_attempt_count =
            reserved_this_boot ? rec->tx.boot_attempt_count : next_attempts;
        if (target != NULL) {
            out->decision = TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE;
            copy_id(out->rollback_profile_id, target);
            out->proposed_tx_state = TUNING_TX_ROLLBACK_PENDING;
            out->operator_surface = false;
            /* Execution eligibility exists ONLY for a durably reserved
             * same-boot attempt with a real target; a fresh proposal must
             * be persisted (read back) and noted first. */
            out->rollback_action_eligible = reserved_this_boot;
        } else {
            /* No rollback target exists (fresh device mid-first-apply):
             * retain whatever the hardware currently runs, mark it
             * unverified, surface the operator — never invent a safe
             * profile. The RECOVERY_REQUIRED proposal is terminal, so a
             * missing last-known-safe consumes at most this one
             * increment, never repeated automatic attempts. */
            out->decision = TUNING_BOOT_RETAIN_CURRENT_UNVERIFIED;
            out->proposed_tx_state = TUNING_TX_RECOVERY_REQUIRED;
            out->operator_surface = true;
            out->secondary_code = TUNING_BOOT_CODE_NO_LAST_KNOWN_SAFE;
        }
    }
}

void tuning_boot_plan(const TuningBootInput *in, TuningBootPlan *out)
{
    const TuningPolicyRecord *rec;

    if (out == NULL) {
        return;
    }
    plan_conservative(out, TUNING_BOOT_CODE_INVALID_INPUT);
    if (in == NULL) {
        return;
    }

    /* No persisted policy state at all: a clean start. The feature is
     * disabled by default; nothing to recover, nothing to inhibit. */
    if (in->store_result == TUNING_STORE_EMPTY ||
        in->store_result == TUNING_STORE_CLEARED) {
        memset(out, 0, sizeof(*out));
        out->decision = TUNING_BOOT_NORMAL_START;
        out->proposed_tx_state = TUNING_TX_IDLE;
        out->primary_code = TUNING_BOOT_CODE_NONE;
        return;
    }

    /* Any store failure: recovery, and the integrator must NOT write over
     * the evidence (the proposal is not committable on a failed store).
     * No boot-recovery decision ever emits a full-store tombstone. */
    if (in->store_result != TUNING_STORE_OK) {
        plan_conservative(out, TUNING_BOOT_CODE_STORE_FAILED);
        return;
    }

    if (!in->record_present || in->record == NULL) {
        plan_conservative(out, TUNING_BOOT_CODE_INVALID_INPUT);
        return;
    }
    rec = in->record;

    /* TOTAL per-state decision table (compile-pinned in the header). */
    switch (rec->tx.state) {
    case TUNING_TX_IDLE:
        /* No rollback, no apply, no counter increment; the durable
         * policy state is preserved as-is. */
        memset(out, 0, sizeof(*out));
        out->decision = TUNING_BOOT_NORMAL_RESUME;
        out->proposed_tx_state = TUNING_TX_IDLE;
        out->proposed_boot_attempt_count = 0u;
        out->primary_code = TUNING_BOOT_CODE_NONE;
        return;

    case TUNING_TX_COMMITTED:
        /* The selected profile is already verified and durably committed:
         * preserve last-known-safe, never roll back, never re-apply tuning
         * in W2. The only action is the canonicalize-to-IDLE finalization
         * proposal (tuning_record_finalize_transaction + normal commit —
         * NEVER a tombstone). */
        memset(out, 0, sizeof(*out));
        out->decision = TUNING_BOOT_NORMAL_RESUME;
        out->finalize_transaction = true;
        out->proposed_tx_state = TUNING_TX_IDLE;
        out->proposed_boot_attempt_count = 0u; /* no budget consumed */
        out->primary_code = TUNING_BOOT_CODE_TX_FINALIZE;
        return;

    case TUNING_TX_INTENT_PERSISTED:
    case TUNING_TX_APPLY_PENDING:
    case TUNING_TX_APPLYING:
    case TUNING_TX_RESTART_PENDING:
    case TUNING_TX_VERIFYING:
        plan_interrupted(in, rec, false, out);
        return;

    case TUNING_TX_ROLLBACK_PENDING:
    case TUNING_TX_ROLLING_BACK:
        plan_interrupted(in, rec, true, out);
        return;

    case TUNING_TX_RECOVERY_REQUIRED:
        /* Operator recovery only: no automatic apply, no automatic
         * rollback retry, no tombstone, no transition to IDLE. */
        plan_conservative(out, TUNING_BOOT_CODE_TX_RECOVERY_STATE);
        out->proposed_boot_attempt_count = rec->tx.boot_attempt_count;
        return;

    default:
        /* Unknown/future state: fail closed, preserve evidence. */
        plan_conservative(out, TUNING_BOOT_CODE_INVALID_INPUT);
        return;
    }
}

const char *tuning_boot_decision_str(TuningBootDecision d)
{
    switch (d) {
    case TUNING_BOOT_RECOVERY_REQUIRED: return "RECOVERY_REQUIRED";
    case TUNING_BOOT_NORMAL_START: return "NORMAL_START";
    case TUNING_BOOT_NORMAL_RESUME: return "NORMAL_RESUME";
    case TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE: return "ROLLBACK_TO_LAST_KNOWN_SAFE";
    case TUNING_BOOT_RETAIN_CURRENT_UNVERIFIED: return "RETAIN_CURRENT_UNVERIFIED";
    default: return "UNKNOWN";
    }
}

const char *tuning_boot_code_str(TuningBootCode c)
{
    switch (c) {
    case TUNING_BOOT_CODE_NONE: return "NONE";
    case TUNING_BOOT_CODE_INVALID_INPUT: return "INVALID_INPUT";
    case TUNING_BOOT_CODE_STORE_FAILED: return "STORE_FAILED";
    case TUNING_BOOT_CODE_TX_INTERRUPTED: return "TX_INTERRUPTED";
    case TUNING_BOOT_CODE_ROLLBACK_IN_PROGRESS: return "ROLLBACK_IN_PROGRESS";
    case TUNING_BOOT_CODE_NO_LAST_KNOWN_SAFE: return "NO_LAST_KNOWN_SAFE";
    case TUNING_BOOT_CODE_BOOT_BUDGET_EXHAUSTED: return "BOOT_BUDGET_EXHAUSTED";
    case TUNING_BOOT_CODE_TX_RECOVERY_STATE: return "TX_RECOVERY_STATE";
    case TUNING_BOOT_CODE_TX_FINALIZE: return "TX_FINALIZE";
    default: return "UNKNOWN";
    }
}
