/*
 * Exhaustive deterministic tests for the Gate W2 pure boot-recovery
 * decision. No hardware, no NVS, no clocks — all inputs are synthetic.
 */

#include <string.h>
#include "unity.h"
#include "tuning_recovery.h"

#define EPOCH_S 1800000000ull

static TuningPolicyRecord g_rec;

static void rec_idle(TuningPolicyRecord *r)
{
    tuning_record_init_state(r);
    r->generation = 3u;
    strncpy(r->last_known_safe_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(r->last_known_safe_id) - 1);
    r->last_known_safe_revision = 2u;
}

static void rec_pending(TuningPolicyRecord *r, TuningTxState st)
{
    rec_idle(r);
    r->tx.state = st;
    strncpy(r->tx.requested_profile_id, TUNING_PROFILE_ID_SUPERSINK_MAX,
            sizeof(r->tx.requested_profile_id) - 1);
    r->tx.actor = TUNING_ACTOR_WEATHER_POLICY;
    r->tx.reason = TUNING_REASON_WEATHER_COOL_ELIGIBLE;
    r->tx.profile_revision = 2u;
    r->tx.started_epoch_s = EPOCH_S;
    r->tx.updated_epoch_s = EPOCH_S + 5ull;
}

static TuningBootInput in_of(TuningStoreResult res, const TuningPolicyRecord *rec)
{
    TuningBootInput in;
    memset(&in, 0, sizeof(in));
    in.store_result = res;
    in.record_present = (rec != NULL);
    in.record = rec;
    return in;
}

/* ---------------- conservative defaults ---------------- */

TEST_CASE("boot: NULL input yields the conservative recovery plan", "[tuning_recovery]")
{
    TuningBootPlan p;
    memset(&p, 0xFF, sizeof(p));
    tuning_boot_plan(NULL, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_RECOVERY_REQUIRED, p.decision);
    TEST_ASSERT_TRUE(p.upgrade_inhibit);
    TEST_ASSERT_TRUE(p.operator_surface);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_INVALID_INPUT, p.primary_code);
    TEST_ASSERT_EQUAL_STRING("", p.rollback_profile_id);
    /* out == NULL must not crash */
    tuning_boot_plan(NULL, NULL);
}

TEST_CASE("boot: OK without a record is invalid input", "[tuning_recovery]")
{
    TuningBootPlan p;
    TuningBootInput in = in_of(TUNING_STORE_OK, NULL);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_RECOVERY_REQUIRED, p.decision);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_INVALID_INPUT, p.primary_code);

    rec_idle(&g_rec);
    in = in_of(TUNING_STORE_OK, &g_rec);
    in.record_present = false; /* inconsistent flags */
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_RECOVERY_REQUIRED, p.decision);
}

/* ---------------- store-result table ---------------- */

TEST_CASE("boot: empty/cleared stores start clean", "[tuning_recovery]")
{
    TuningBootPlan p;
    TuningBootInput in = in_of(TUNING_STORE_EMPTY, NULL);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_NORMAL_START, p.decision);
    TEST_ASSERT_FALSE(p.upgrade_inhibit);
    TEST_ASSERT_FALSE(p.operator_surface);
    TEST_ASSERT_EQUAL(TUNING_TX_IDLE, p.proposed_tx_state);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_NONE, p.primary_code);

    in = in_of(TUNING_STORE_CLEARED, NULL);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_NORMAL_START, p.decision);
}

TEST_CASE("boot: every store failure is recovery with no mutation", "[tuning_recovery]")
{
    static const TuningStoreResult fails[] = {
        TUNING_STORE_NOT_INITIALIZED, TUNING_STORE_INVALID_ARGUMENT,
        TUNING_STORE_IO_ERROR, TUNING_STORE_CORRUPT,
        TUNING_STORE_UNSUPPORTED_SCHEMA, TUNING_STORE_INVALID_RECORD,
        TUNING_STORE_ACTIVE_POINTER_INVALID, TUNING_STORE_ACTIVE_SLOT_INVALID,
        TUNING_STORE_RECOVERY_REQUIRED, TUNING_STORE_GENERATION_EXHAUSTED,
        TUNING_STORE_READBACK_MISMATCH, TUNING_STORE_COMMIT_UNCERTAIN,
        TUNING_STORE_STATE_CONFLICT,
    };
    for (size_t i = 0; i < sizeof(fails) / sizeof(fails[0]); i++) {
        TuningBootPlan p;
        TuningBootInput in = in_of(fails[i], NULL);
        tuning_boot_plan(&in, &p);
        TEST_ASSERT_EQUAL(TUNING_BOOT_RECOVERY_REQUIRED, p.decision);
        TEST_ASSERT_TRUE(p.upgrade_inhibit);
        TEST_ASSERT_TRUE(p.operator_surface);
        TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_STORE_FAILED, p.primary_code);
        TEST_ASSERT_EQUAL_STRING("", p.rollback_profile_id);
    }
}

/* ---------------- safe transaction states ---------------- */

TEST_CASE("boot: IDLE resumes with no action and no budget use", "[tuning_recovery]")
{
    TuningBootPlan p;
    rec_idle(&g_rec);
    TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_NORMAL_RESUME, p.decision);
    TEST_ASSERT_FALSE(p.upgrade_inhibit);
    TEST_ASSERT_FALSE(p.operator_surface);
    TEST_ASSERT_FALSE(p.finalize_transaction);
    TEST_ASSERT_EQUAL(TUNING_TX_IDLE, p.proposed_tx_state);
    TEST_ASSERT_EQUAL_UINT8(0, p.proposed_boot_attempt_count);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_NONE, p.primary_code);
    TEST_ASSERT_EQUAL_STRING("", p.rollback_profile_id);
}

TEST_CASE("boot: COMMITTED finalizes — never rolls back or re-applies", "[tuning_recovery]")
{
    TuningBootPlan p;
    rec_pending(&g_rec, TUNING_TX_COMMITTED);
    g_rec.tx.boot_attempt_count = 2u; /* prior attempts do not matter here */
    TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_NORMAL_RESUME, p.decision);
    TEST_ASSERT_TRUE(p.finalize_transaction); /* canonicalize-to-IDLE only */
    TEST_ASSERT_FALSE(p.upgrade_inhibit);
    TEST_ASSERT_FALSE(p.operator_surface);
    TEST_ASSERT_EQUAL(TUNING_TX_IDLE, p.proposed_tx_state);
    /* no rollback, no budget consumption for a successful boot */
    TEST_ASSERT_EQUAL_STRING("", p.rollback_profile_id);
    TEST_ASSERT_EQUAL_UINT8(0, p.proposed_boot_attempt_count);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_TX_FINALIZE, p.primary_code);
}

/* ---------------- interrupted transactions ---------------- */

TEST_CASE("boot: every pending state rolls back to last known safe", "[tuning_recovery]")
{
    static const TuningTxState pend[] = {
        TUNING_TX_INTENT_PERSISTED, TUNING_TX_APPLY_PENDING, TUNING_TX_APPLYING,
        TUNING_TX_RESTART_PENDING, TUNING_TX_VERIFYING,
    };
    for (size_t i = 0; i < sizeof(pend) / sizeof(pend[0]); i++) {
        TuningBootPlan p;
        rec_pending(&g_rec, pend[i]);
        g_rec.tx.boot_attempt_count = 0u;
        TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
        tuning_boot_plan(&in, &p);
        /* NEVER assume the requested profile succeeded */
        TEST_ASSERT_EQUAL(TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE, p.decision);
        TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, p.rollback_profile_id);
        TEST_ASSERT_TRUE(p.upgrade_inhibit);
        TEST_ASSERT_FALSE(p.operator_surface);
        TEST_ASSERT_EQUAL(TUNING_TX_ROLLBACK_PENDING, p.proposed_tx_state);
        TEST_ASSERT_EQUAL_UINT8(1, p.proposed_boot_attempt_count);
        TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_TX_INTERRUPTED, p.primary_code);
    }
}

TEST_CASE("boot: an in-progress rollback keeps its recorded target", "[tuning_recovery]")
{
    TuningBootPlan p;
    rec_pending(&g_rec, TUNING_TX_ROLLING_BACK);
    strncpy(g_rec.tx.rollback_profile_id, TUNING_PROFILE_ID_EMERGENCY,
            sizeof(g_rec.tx.rollback_profile_id) - 1);
    g_rec.tx.boot_attempt_count = 1u;
    TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE, p.decision);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_EMERGENCY, p.rollback_profile_id);
    TEST_ASSERT_EQUAL_UINT8(2, p.proposed_boot_attempt_count);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_ROLLBACK_IN_PROGRESS, p.primary_code);

    /* without a recorded target it falls back to last-known-safe */
    rec_pending(&g_rec, TUNING_TX_ROLLBACK_PENDING);
    TEST_ASSERT_EQUAL_STRING("", g_rec.tx.rollback_profile_id);
    in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE, p.decision);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, p.rollback_profile_id);
}

TEST_CASE("boot: no last-known-safe retains current unverified", "[tuning_recovery]")
{
    TuningBootPlan p;
    rec_pending(&g_rec, TUNING_TX_APPLYING);
    memset(g_rec.last_known_safe_id, 0, sizeof(g_rec.last_known_safe_id));
    g_rec.last_known_safe_revision = 0u;
    TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_RETAIN_CURRENT_UNVERIFIED, p.decision);
    TEST_ASSERT_EQUAL_STRING("", p.rollback_profile_id);
    TEST_ASSERT_TRUE(p.upgrade_inhibit);
    TEST_ASSERT_TRUE(p.operator_surface);
    TEST_ASSERT_EQUAL(TUNING_TX_RECOVERY_REQUIRED, p.proposed_tx_state);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_TX_INTERRUPTED, p.primary_code);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_NO_LAST_KNOWN_SAFE, p.secondary_code);
}

TEST_CASE("boot: the boot-attempt budget prevents reboot loops", "[tuning_recovery]")
{
    TuningBootPlan p;
    /* one attempt below the threshold: still a rollback */
    rec_pending(&g_rec, TUNING_TX_APPLYING);
    g_rec.tx.boot_attempt_count = TUNING_BOOT_MAX_TX_BOOT_ATTEMPTS - 1u;
    TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE, p.decision);
    TEST_ASSERT_EQUAL_UINT8(TUNING_BOOT_MAX_TX_BOOT_ATTEMPTS,
                            p.proposed_boot_attempt_count);

    /* at the threshold: recovery, never another automatic attempt */
    rec_pending(&g_rec, TUNING_TX_APPLYING);
    g_rec.tx.boot_attempt_count = TUNING_BOOT_MAX_TX_BOOT_ATTEMPTS;
    in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_RECOVERY_REQUIRED, p.decision);
    TEST_ASSERT_TRUE(p.upgrade_inhibit);
    TEST_ASSERT_TRUE(p.operator_surface);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_BOOT_BUDGET_EXHAUSTED, p.primary_code);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_TX_INTERRUPTED, p.secondary_code);
    TEST_ASSERT_EQUAL(TUNING_TX_RECOVERY_REQUIRED, p.proposed_tx_state);
}

TEST_CASE("boot: a persisted recovery transaction stays recovery", "[tuning_recovery]")
{
    TuningBootPlan p;
    rec_pending(&g_rec, TUNING_TX_RECOVERY_REQUIRED);
    g_rec.tx.boot_attempt_count = 4u;
    TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_RECOVERY_REQUIRED, p.decision);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_TX_RECOVERY_STATE, p.primary_code);
    TEST_ASSERT_EQUAL_UINT8(4, p.proposed_boot_attempt_count); /* preserved */
    TEST_ASSERT_EQUAL(TUNING_TX_RECOVERY_REQUIRED, p.proposed_tx_state);
}

/* ---------------- total table + budget boundaries ---------------- */

TEST_CASE("boot: the decision table is total over all ten states", "[tuning_recovery]")
{
    /* One row per TuningTxState (compile-pinned in tuning_recovery.h):
     * expected decision + finalize flag + tombstone-freedom. */
    for (int s = 0; s < TUNING_TX__COUNT; s++) {
        TuningBootPlan p;
        TuningTxState st = (TuningTxState)s;
        if (st == TUNING_TX_IDLE) {
            rec_idle(&g_rec);
        } else {
            rec_pending(&g_rec, st);
        }
        TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
        tuning_boot_plan(&in, &p);

        switch (st) {
        case TUNING_TX_IDLE:
            TEST_ASSERT_EQUAL(TUNING_BOOT_NORMAL_RESUME, p.decision);
            TEST_ASSERT_FALSE(p.finalize_transaction);
            break;
        case TUNING_TX_COMMITTED:
            TEST_ASSERT_EQUAL(TUNING_BOOT_NORMAL_RESUME, p.decision);
            TEST_ASSERT_TRUE(p.finalize_transaction);
            break;
        case TUNING_TX_INTENT_PERSISTED:
        case TUNING_TX_APPLY_PENDING:
        case TUNING_TX_APPLYING:
        case TUNING_TX_RESTART_PENDING:
        case TUNING_TX_VERIFYING:
        case TUNING_TX_ROLLBACK_PENDING:
        case TUNING_TX_ROLLING_BACK:
            TEST_ASSERT_EQUAL(TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE, p.decision);
            TEST_ASSERT_TRUE(p.upgrade_inhibit);
            TEST_ASSERT_FALSE(p.finalize_transaction);
            TEST_ASSERT_EQUAL(TUNING_TX_ROLLBACK_PENDING, p.proposed_tx_state);
            break;
        case TUNING_TX_RECOVERY_REQUIRED:
            TEST_ASSERT_EQUAL(TUNING_BOOT_RECOVERY_REQUIRED, p.decision);
            TEST_ASSERT_FALSE(p.finalize_transaction);
            break;
        default:
            TEST_FAIL_MESSAGE("unhandled state in test table");
        }
        /* No plan for ANY state proposes anything outside the valid
         * transaction-state range — a full-store tombstone is not a
         * transaction state and is structurally unreachable from here. */
        TEST_ASSERT_TRUE((unsigned)p.proposed_tx_state < TUNING_TX__COUNT);
    }

    /* Unknown/future state fails closed with evidence preserved. */
    {
        TuningBootPlan p;
        rec_pending(&g_rec, TUNING_TX_APPLYING);
        g_rec.tx.state = (TuningTxState)TUNING_TX__COUNT; /* in-RAM only */
        TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
        tuning_boot_plan(&in, &p);
        TEST_ASSERT_EQUAL(TUNING_BOOT_RECOVERY_REQUIRED, p.decision);
        TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_INVALID_INPUT, p.primary_code);
    }
}

TEST_CASE("boot: exact attempt-budget boundaries", "[tuning_recovery]")
{
    /* 0 -> first attempt, 1 -> second, 2 -> third, 3 -> recovery,
     * storage max and corrupted large values -> recovery. */
    static const struct { uint8_t have; bool rollback; uint8_t next; } rows[] = {
        { 0u, true, 1u },
        { 1u, true, 2u },
        { 2u, true, 3u },
        { 3u, false, 4u },
        { TUNING_TX_BOOT_ATTEMPT_STORE_MAX, false,
          TUNING_TX_BOOT_ATTEMPT_STORE_MAX },
        { 255u, false, TUNING_TX_BOOT_ATTEMPT_STORE_MAX }, /* in-RAM only */
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        TuningBootPlan p;
        rec_pending(&g_rec, TUNING_TX_APPLYING);
        g_rec.tx.boot_attempt_count = rows[i].have;
        TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
        tuning_boot_plan(&in, &p);
        if (rows[i].rollback) {
            TEST_ASSERT_EQUAL(TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE, p.decision);
        } else {
            TEST_ASSERT_EQUAL(TUNING_BOOT_RECOVERY_REQUIRED, p.decision);
            TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_BOOT_BUDGET_EXHAUSTED, p.primary_code);
        }
        TEST_ASSERT_EQUAL_UINT8(rows[i].next, p.proposed_boot_attempt_count);
    }
}

TEST_CASE("boot: repeated evaluation never double-increments", "[tuning_recovery]")
{
    /* The plan is pure: evaluating the SAME committed record any number of
     * times yields the identical proposal — increments become durable only
     * through a committed persist-before-action proposal, once per boot. */
    TuningBootPlan p1, p2, p3;
    rec_pending(&g_rec, TUNING_TX_ROLLING_BACK);
    g_rec.tx.boot_attempt_count = 1u;
    TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p1);
    tuning_boot_plan(&in, &p2);
    tuning_boot_plan(&in, &p3);
    TEST_ASSERT_EQUAL_UINT8(2, p1.proposed_boot_attempt_count);
    TEST_ASSERT_EQUAL_MEMORY(&p1, &p2, sizeof(p1));
    TEST_ASSERT_EQUAL_MEMORY(&p1, &p3, sizeof(p1));
}

TEST_CASE("boot: missing last-known-safe consumes at most one increment", "[tuning_recovery]")
{
    /* First boot: RETAIN + terminal RECOVERY proposal (one increment).
     * Applying that proposal and rebooting: the RECOVERY state preserves
     * the counter — no further automatic attempts, ever. */
    TuningBootPlan p;
    rec_pending(&g_rec, TUNING_TX_APPLYING);
    memset(g_rec.last_known_safe_id, 0, sizeof(g_rec.last_known_safe_id));
    g_rec.last_known_safe_revision = 0u;
    TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_RETAIN_CURRENT_UNVERIFIED, p.decision);
    TEST_ASSERT_EQUAL_UINT8(1, p.proposed_boot_attempt_count);

    /* simulate the committed proposal, then the next boot */
    g_rec.tx.state = p.proposed_tx_state; /* RECOVERY_REQUIRED */
    g_rec.tx.boot_attempt_count = p.proposed_boot_attempt_count;
    in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_RECOVERY_REQUIRED, p.decision);
    TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_TX_RECOVERY_STATE, p.primary_code);
    TEST_ASSERT_EQUAL_UINT8(1, p.proposed_boot_attempt_count); /* preserved */
}

/* ---------------- boot context: same-boot vs new-boot budget ---------- */

/* Simulate the integrator durably committing a rollback proposal: apply it
 * to the record, bump the store-assigned generation, note the context. */
static void commit_proposal(TuningPolicyRecord *rec, const TuningBootPlan *p,
                            TuningBootContext *ctx)
{
    rec->tx.state = p->proposed_tx_state;
    rec->tx.boot_attempt_count = p->proposed_boot_attempt_count;
    memset(rec->tx.rollback_profile_id, 0, sizeof(rec->tx.rollback_profile_id));
    strncpy(rec->tx.rollback_profile_id, p->rollback_profile_id,
            sizeof(rec->tx.rollback_profile_id) - 1);
    rec->generation += 1u; /* the store assigns a new generation */
    tuning_boot_context_note_committed(ctx, rec->generation,
                                       rec->tx.boot_attempt_count);
}

TEST_CASE("budget: five same-boot reevaluations consume exactly one unit", "[tuning_recovery]")
{
    TuningBootPlan p;
    TuningBootContext ctx;
    tuning_boot_context_init(&ctx);

    rec_pending(&g_rec, TUNING_TX_APPLYING);
    g_rec.generation = 5u;
    TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
    in.context = ctx; /* fresh boot: reservation required first */
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_EQUAL(TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE, p.decision);
    TEST_ASSERT_EQUAL_UINT8(1, p.proposed_boot_attempt_count);
    TEST_ASSERT_FALSE(p.rollback_action_eligible); /* persist first */

    /* durable commit + read-back happened; context notes it */
    commit_proposal(&g_rec, &p, &ctx);

    /* five task wakeups in the SAME boot: resume the reserved attempt —
     * no further increment, execution eligible, byte-identical plans */
    {
        TuningBootPlan prev;
        memset(&prev, 0, sizeof(prev));
        for (int i = 0; i < 5; i++) {
            in = in_of(TUNING_STORE_OK, &g_rec);
            in.context = ctx;
            tuning_boot_plan(&in, &p);
            TEST_ASSERT_EQUAL(TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE, p.decision);
            TEST_ASSERT_TRUE(p.rollback_action_eligible);
            TEST_ASSERT_EQUAL_UINT8(1, p.proposed_boot_attempt_count);
            TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER,
                                     p.rollback_profile_id);
            if (i > 0) {
                TEST_ASSERT_EQUAL_MEMORY(&prev, &p, sizeof(p));
            }
            prev = p;
        }
    }
    /* exactly one durable unit was consumed this boot */
    TEST_ASSERT_EQUAL_UINT8(1, g_rec.tx.boot_attempt_count);
}

TEST_CASE("budget: a new boot reserves the next attempt for both rollback states", "[tuning_recovery]")
{
    static const TuningTxState states[] = {
        TUNING_TX_ROLLBACK_PENDING, /* reserved-but-not-started: a NEW boot
                                       must reserve the NEXT attempt       */
        TUNING_TX_ROLLING_BACK,     /* reboot = previous attempt interrupted:
                                       the next attempt must be reserved   */
    };
    for (size_t i = 0; i < 2; i++) {
        TuningBootPlan p;
        rec_pending(&g_rec, states[i]);
        strncpy(g_rec.tx.rollback_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
                sizeof(g_rec.tx.rollback_profile_id) - 1);
        g_rec.tx.boot_attempt_count = 1u;
        /* in_of zeroes the context: a REAL boot never inherits the
         * same-boot suppression flag */
        TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
        tuning_boot_plan(&in, &p);
        TEST_ASSERT_EQUAL(TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE, p.decision);
        TEST_ASSERT_EQUAL_UINT8(2, p.proposed_boot_attempt_count);
        TEST_ASSERT_FALSE(p.rollback_action_eligible); /* persist first */
    }
}

TEST_CASE("budget: three crash boots deterministically reach recovery", "[tuning_recovery]")
{
    /* Boot 1..3: each reserves one attempt (0->1, 1->2, 2->3), commits it,
     * starts rolling back, then crashes. Boot 4: budget exhausted. */
    TuningBootPlan p;
    rec_pending(&g_rec, TUNING_TX_APPLYING);
    g_rec.generation = 9u;

    for (int boot = 1; boot <= 3; boot++) {
        TuningBootContext ctx;
        tuning_boot_context_init(&ctx); /* each real boot starts clear */
        TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
        in.context = ctx;
        tuning_boot_plan(&in, &p);
        TEST_ASSERT_EQUAL(TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE, p.decision);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)boot, p.proposed_boot_attempt_count);
        TEST_ASSERT_FALSE(p.rollback_action_eligible);
        commit_proposal(&g_rec, &p, &ctx);
        /* same boot: now eligible; then the rollback runs and CRASHES */
        in = in_of(TUNING_STORE_OK, &g_rec);
        in.context = ctx;
        tuning_boot_plan(&in, &p);
        TEST_ASSERT_TRUE(p.rollback_action_eligible);
        g_rec.tx.state = TUNING_TX_ROLLING_BACK; /* crash mid-rollback */
    }

    /* Boot 4: stored count == 3 == limit -> operator recovery, never a
     * fourth automatic attempt. */
    {
        TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
        tuning_boot_plan(&in, &p);
        TEST_ASSERT_EQUAL(TUNING_BOOT_RECOVERY_REQUIRED, p.decision);
        TEST_ASSERT_EQUAL(TUNING_BOOT_CODE_BOOT_BUDGET_EXHAUSTED, p.primary_code);
        TEST_ASSERT_FALSE(p.rollback_action_eligible);
        TEST_ASSERT_TRUE(p.operator_surface);
    }
}

TEST_CASE("budget: unnoted (uncertain/mismatched) commits never grant eligibility", "[tuning_recovery]")
{
    /* The integrator saw STORE_COMMIT_UNCERTAIN or READBACK_MISMATCH and
     * therefore did NOT note the context. Whatever the on-flash truth is,
     * re-planning never grants execution eligibility and stays in the
     * persist-first posture. */
    TuningBootPlan p;
    TuningBootContext ctx;
    tuning_boot_context_init(&ctx); /* never noted */

    /* case a: the increment did not land (old record observed) */
    rec_pending(&g_rec, TUNING_TX_APPLYING);
    TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
    in.context = ctx;
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_FALSE(p.rollback_action_eligible);
    TEST_ASSERT_EQUAL_UINT8(1, p.proposed_boot_attempt_count);

    /* case b: the increment DID land (new record observed) — still not
     * noted, so still ineligible; the proposal resumes at the durable
     * count via a fresh reservation path */
    rec_pending(&g_rec, TUNING_TX_ROLLBACK_PENDING);
    strncpy(g_rec.tx.rollback_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(g_rec.tx.rollback_profile_id) - 1);
    g_rec.tx.boot_attempt_count = 1u;
    in = in_of(TUNING_STORE_OK, &g_rec);
    in.context = ctx;
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_FALSE(p.rollback_action_eligible);

    /* a stale context (different generation) is equally ineligible */
    tuning_boot_context_note_committed(&ctx, g_rec.generation + 7u, 1u);
    in = in_of(TUNING_STORE_OK, &g_rec);
    in.context = ctx;
    tuning_boot_plan(&in, &p);
    TEST_ASSERT_FALSE(p.rollback_action_eligible);
}

TEST_CASE("budget: context helpers are bounded and null-safe", "[tuning_recovery]")
{
    TuningBootContext ctx;
    memset(&ctx, 0xFF, sizeof(ctx));
    tuning_boot_context_init(&ctx);
    TEST_ASSERT_FALSE(ctx.attempt_committed_this_boot);
    TEST_ASSERT_EQUAL_UINT32(0, ctx.committed_record_generation);
    tuning_boot_context_note_committed(&ctx, 42u, 2u);
    TEST_ASSERT_TRUE(ctx.attempt_committed_this_boot);
    TEST_ASSERT_EQUAL_UINT32(42u, ctx.committed_record_generation);
    TEST_ASSERT_EQUAL_UINT8(2u, ctx.committed_attempt_count);
    tuning_boot_context_init(NULL);            /* must not crash */
    tuning_boot_context_note_committed(NULL, 1u, 1u);
}

/* ---------------- determinism + tokens ---------------- */

TEST_CASE("boot: identical inputs produce memcmp-identical plans", "[tuning_recovery]")
{
    TuningBootPlan p1, p2;
    rec_pending(&g_rec, TUNING_TX_VERIFYING);
    TuningBootInput in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_plan(&in, &p1);
    tuning_boot_plan(&in, &p2);
    TEST_ASSERT_EQUAL_MEMORY(&p1, &p2, sizeof(p1));

    /* Same complete boot CONTEXT and record: byte-identical plan too. */
    rec_pending(&g_rec, TUNING_TX_ROLLBACK_PENDING);
    strncpy(g_rec.tx.rollback_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(g_rec.tx.rollback_profile_id) - 1);
    g_rec.tx.boot_attempt_count = 2u;
    in = in_of(TUNING_STORE_OK, &g_rec);
    tuning_boot_context_note_committed(&in.context, g_rec.generation, 2u);
    tuning_boot_plan(&in, &p1);
    tuning_boot_plan(&in, &p2);
    TEST_ASSERT_EQUAL_MEMORY(&p1, &p2, sizeof(p1));
    TEST_ASSERT_TRUE(p1.rollback_action_eligible);
}

TEST_CASE("boot: tokens are distinct for every enum value", "[tuning_recovery]")
{
    for (int i = 0; i < TUNING_BOOT__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN",
                                        tuning_boot_decision_str((TuningBootDecision)i)));
    }
    for (int i = 0; i < TUNING_BOOT_CODE__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN",
                                        tuning_boot_code_str((TuningBootCode)i)));
    }
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", tuning_boot_decision_str((TuningBootDecision)77));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", tuning_boot_code_str((TuningBootCode)77));
}
