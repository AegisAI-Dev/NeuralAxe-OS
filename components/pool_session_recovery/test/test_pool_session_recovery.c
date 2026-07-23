/*
 * Exhaustive deterministic tests for the pure boot-recovery engine (B4).
 *
 * No hardware, no NVS, no networking, no reset-reason reads — everything is
 * a deterministic synthetic fixture ("*.example" identities, fake reset
 * classes, fake trusted-time snapshots). The engine executes nothing.
 */

#include <string.h>
#include "unity.h"
#include "pool_session_recovery.h"

#define EPOCH_A_S    1750000000ull /* mid-2025, inside the sanity band */
#define WAIT_LIMIT_S 600u

/* File-static fixtures (records ~1 KB; keep off the task stack). */
static PoolSessionRecord g_rec;
static PoolSessionRecord g_rec_copy;
static PoolSessionBootContext g_ctx;
static PoolSessionBootContext g_ctx_copy;
static PoolSessionRecoveryPlan g_plan;
static PoolSessionRecoveryPlan g_plan2;

static void fill_identity(PoolConfigIdentity *c, PoolChainType chain, const char *host,
                          uint16_t port, const char *user)
{
    memset(c, 0, sizeof(*c));
    c->chain = chain;
    strncpy(c->primary.host, host, sizeof(c->primary.host) - 1);
    c->primary.port = port;
    strncpy(c->primary.user, user, sizeof(c->primary.user) - 1);
    c->primary.protocol = POOL_PROTO_STRATUM_V1;
}

/* Fully verified TARGET_ACTIVE record with time facts and the trusted floor. */
static void make_active_rec(PoolSessionRecord *r)
{
    pool_session_record_init(r);
    r->generation = 3u;
    r->session_id = 42u;
    r->b1_model_version = POOL_SESSION_MODEL_VERSION;
    r->state = POOL_STATE_TARGET_ACTIVE;
    fill_identity(&r->source, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    fill_identity(&r->target, POOL_CHAIN_BITCOIN_CASH, "bch.example", 3334, "acct.worker");
    r->password_policy = POOL_SESSION_PW_KEEP_CURRENT;
    r->restore_required = true;
    r->target_verify.connection_observed = true;
    r->target_verify.mining_observed = true;
    r->target_verify.identity_verified = true;
    r->duration_s = 3600u;
    r->verified_start_valid = true;
    r->verified_start_epoch_s = EPOCH_A_S;
    r->deadline_valid = true;
    r->deadline_epoch_s = EPOCH_A_S + 3600u;
    r->deadline_sync_generation = 1u;
    r->latest_trusted_valid = true;
    r->latest_trusted_epoch_s = EPOCH_A_S + 100u;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(r));
}

/* Record in an arbitrary persistent state with consistent invariants. */
static void make_state_rec(PoolSessionRecord *r, PoolSessionState st, bool rr)
{
    make_active_rec(r);
    r->state = st;
    r->restore_required = rr;
    if (st != POOL_STATE_TARGET_ACTIVE) {
        /* only TARGET_ACTIVE demands full target evidence; keep it anyway */
    }
    if (st == POOL_STATE_COMPLETE) {
        r->restore_verify.connection_observed = true;
        r->restore_verify.mining_observed = true;
        r->restore_verify.identity_verified = true;
    }
}

static void make_ctx(PoolStoreResult res, const PoolSessionRecord *rec,
                     PoolSessionResetClass rc)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.store_result = res;
    g_ctx.record_present = (rec != NULL);
    g_ctx.record = rec;
    g_ctx.reset_class = rc;
    g_ctx.sync_wait_elapsed_s = 0u;
    g_ctx.sync_wait_limit_s = WAIT_LIMIT_S;
    g_ctx.time_provider_initialized = true;
    g_ctx.mining_inhibition_available = true;
    /* snapshot defaults to untrusted */
    g_ctx.time_snapshot.status = TIME_ERR_NOT_SYNCED;
}

static void set_trusted(uint64_t epoch_s)
{
    g_ctx.time_snapshot.trusted = true;
    g_ctx.time_snapshot.status = TIME_OK;
    g_ctx.time_snapshot.trusted_epoch_s = epoch_s;
    g_ctx.time_snapshot.trusted_utc_us = epoch_s * 1000000ull;
    g_ctx.time_snapshot.sync_generation = 1u;
}

static PoolRecoveryError run_plan(void)
{
    return pool_session_recovery_plan(&g_ctx, &g_plan);
}

/* The plan must never allow the target unless it is the unique resume plan. */
static void assert_no_target(const PoolSessionRecoveryPlan *p)
{
    TEST_ASSERT_TRUE(p->decision != POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET);
    TEST_ASSERT_TRUE(p->mining_policy != POOL_BOOT_MINING_ALLOW_TARGET);
    TEST_ASSERT_TRUE(p->allowed_config != POOL_BOOT_ALLOW_TARGET_ONLY);
    TEST_ASSERT_TRUE(p->runtime_intent != POOL_BOOT_RUNTIME_RESUME_TARGET_CONFIGURATION);
    TEST_ASSERT_TRUE(p->runtime_intent != POOL_BOOT_RUNTIME_VERIFY_TARGET_CONFIGURATION);
}

static void assert_plan_enums_valid(const PoolSessionRecoveryPlan *p)
{
    TEST_ASSERT_TRUE(p->decision < POOL_BOOT_DECISION__COUNT);
    TEST_ASSERT_TRUE(p->allowed_config < POOL_BOOT_ALLOW__COUNT);
    TEST_ASSERT_TRUE(p->mining_policy < POOL_BOOT_MINING__COUNT);
    TEST_ASSERT_TRUE(p->persist_intent < POOL_BOOT_PERSIST__COUNT);
    TEST_ASSERT_TRUE(p->runtime_intent < POOL_BOOT_RUNTIME__COUNT);
    TEST_ASSERT_TRUE(p->error < POOL_RECOVERY_ERR__COUNT);
    TEST_ASSERT_TRUE(p->reason < POOL_RECOVERY_ERR__COUNT);
}

static void assert_clean_token(const char *tok)
{
    const char *c;
    TEST_ASSERT_NOT_NULL(tok);
    TEST_ASSERT_TRUE(strlen(tok) > 0u && strlen(tok) < 48u);
    for (c = tok; *c != '\0'; c++) {
        TEST_ASSERT_TRUE((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_');
    }
    TEST_ASSERT_NULL(strstr(tok, "EXAMPLE"));
    TEST_ASSERT_NULL(strstr(tok, "MARKER"));
}

static bool bytes_contain(const void *hay, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    const uint8_t *h = (const uint8_t *)hay;
    size_t i;
    if (n == 0u || n > len) {
        return false;
    }
    for (i = 0; i + n <= len; i++) {
        if (memcmp(h + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* ================================================================= */
/* A. Reset model tests                                               */
/* ================================================================= */

TEST_CASE("reset: every raw ESP-IDF reason maps to the documented class", "[pool_recovery]")
{
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_UNKNOWN, pool_session_reset_classify_raw(POOL_RESET_RAW_UNKNOWN));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_POWER_ON, pool_session_reset_classify_raw(POOL_RESET_RAW_POWERON));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_EXTERNAL, pool_session_reset_classify_raw(POOL_RESET_RAW_EXT));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_SOFTWARE, pool_session_reset_classify_raw(POOL_RESET_RAW_SW));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_PANIC, pool_session_reset_classify_raw(POOL_RESET_RAW_PANIC));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_INTERRUPT_WATCHDOG, pool_session_reset_classify_raw(POOL_RESET_RAW_INT_WDT));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_TASK_WATCHDOG, pool_session_reset_classify_raw(POOL_RESET_RAW_TASK_WDT));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_OTHER_WATCHDOG, pool_session_reset_classify_raw(POOL_RESET_RAW_WDT));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_DEEP_SLEEP, pool_session_reset_classify_raw(POOL_RESET_RAW_DEEPSLEEP));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_BROWNOUT, pool_session_reset_classify_raw(POOL_RESET_RAW_BROWNOUT));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_EXTERNAL, pool_session_reset_classify_raw(POOL_RESET_RAW_SDIO));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_EXTERNAL, pool_session_reset_classify_raw(POOL_RESET_RAW_USB));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_EXTERNAL, pool_session_reset_classify_raw(POOL_RESET_RAW_JTAG));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_UNKNOWN, pool_session_reset_classify_raw(POOL_RESET_RAW_EFUSE));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_BROWNOUT, pool_session_reset_classify_raw(POOL_RESET_RAW_PWR_GLITCH));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_PANIC, pool_session_reset_classify_raw(POOL_RESET_RAW_CPU_LOCKUP));
    /* invalid / future raw values are conservative */
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_UNKNOWN, pool_session_reset_classify_raw(-1));
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_UNKNOWN, pool_session_reset_classify_raw(99));
}

TEST_CASE("reset: expected/abnormal/conservative sets are exact", "[pool_recovery]")
{
    int i;
    for (i = 0; i < (int)POOL_RESET_CLASS__COUNT; i++) {
        PoolSessionResetClass c = (PoolSessionResetClass)i;
        bool expected = (c == POOL_RESET_CLASS_POWER_ON || c == POOL_RESET_CLASS_SOFTWARE);
        bool abnormal = (c == POOL_RESET_CLASS_PANIC || c == POOL_RESET_CLASS_TASK_WATCHDOG ||
                         c == POOL_RESET_CLASS_INTERRUPT_WATCHDOG ||
                         c == POOL_RESET_CLASS_OTHER_WATCHDOG ||
                         c == POOL_RESET_CLASS_BROWNOUT || c == POOL_RESET_CLASS_UNKNOWN);
        bool conservative = (c == POOL_RESET_CLASS_DEEP_SLEEP ||
                             c == POOL_RESET_CLASS_EXTERNAL || c == POOL_RESET_CLASS_UNKNOWN);
        TEST_ASSERT_EQUAL(expected, pool_reset_class_is_expected_restart(c));
        TEST_ASSERT_EQUAL(abnormal, pool_reset_class_is_abnormal_restart(c));
        TEST_ASSERT_EQUAL(conservative, pool_reset_class_requires_conservative_restore(c));
        TEST_ASSERT_TRUE(pool_reset_class_increments_reboot_count(c));
        TEST_ASSERT_EQUAL(abnormal, pool_reset_class_increments_consecutive_failures(c));
        TEST_ASSERT_EQUAL(abnormal, pool_reset_class_increments_recovery_attempt(c));
        /* conservative classes never resume the target */
        if (conservative) {
            TEST_ASSERT_FALSE(pool_reset_class_may_resume_target(c));
        }
    }
    /* invalid class values behave as UNKNOWN */
    TEST_ASSERT_TRUE(pool_reset_class_is_abnormal_restart((PoolSessionResetClass)99));
    TEST_ASSERT_FALSE(pool_reset_class_may_resume_target((PoolSessionResetClass)99));
    TEST_ASSERT_TRUE(pool_reset_class_requires_conservative_restore((PoolSessionResetClass)99));
}

TEST_CASE("reset: record-class mapping and tokens are stable", "[pool_recovery]")
{
    int i;
    TEST_ASSERT_EQUAL_UINT8(1u, pool_reset_class_to_record_class(POOL_RESET_CLASS_POWER_ON));
    TEST_ASSERT_EQUAL_UINT8(1u, pool_reset_class_to_record_class(POOL_RESET_CLASS_SOFTWARE));
    TEST_ASSERT_EQUAL_UINT8(1u, pool_reset_class_to_record_class(POOL_RESET_CLASS_DEEP_SLEEP));
    TEST_ASSERT_EQUAL_UINT8(2u, pool_reset_class_to_record_class(POOL_RESET_CLASS_PANIC));
    TEST_ASSERT_EQUAL_UINT8(2u, pool_reset_class_to_record_class(POOL_RESET_CLASS_BROWNOUT));
    TEST_ASSERT_EQUAL_UINT8(3u, pool_reset_class_to_record_class(POOL_RESET_CLASS_EXTERNAL));
    TEST_ASSERT_EQUAL_UINT8(3u, pool_reset_class_to_record_class(POOL_RESET_CLASS_UNKNOWN));
    TEST_ASSERT_EQUAL_UINT8(3u, pool_reset_class_to_record_class((PoolSessionResetClass)77));
    for (i = 0; i < (int)POOL_RESET_CLASS__COUNT; i++) {
        assert_clean_token(pool_reset_class_str((PoolSessionResetClass)i));
    }
    assert_clean_token(pool_reset_class_str((PoolSessionResetClass)77));
    /* every mapped record class fits the B3 persisted range */
    for (i = 0; i < (int)POOL_RESET_CLASS__COUNT; i++) {
        TEST_ASSERT_TRUE(pool_reset_class_to_record_class((PoolSessionResetClass)i) <
                         (uint8_t)POOL_RECORD_RESET_CLASS__COUNT);
    }
}

/* ================================================================= */
/* B. Store-result decision table                                     */
/* ================================================================= */

TEST_CASE("store-result: EMPTY and CLEARED boot the normal source", "[pool_recovery]")
{
    make_ctx(STORE_EMPTY, NULL, POOL_RESET_CLASS_POWER_ON);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_BOOT_NORMAL_SOURCE, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_ALLOW_SOURCE_ONLY, g_plan.allowed_config);
    TEST_ASSERT_EQUAL(POOL_BOOT_MINING_ALLOW_SOURCE, g_plan.mining_policy);
    TEST_ASSERT_EQUAL(POOL_BOOT_PERSIST_NO_RECORD_UPDATE, g_plan.persist_intent);
    TEST_ASSERT_EQUAL(POOL_BOOT_RUNTIME_START_SOURCE_MINING, g_plan.runtime_intent);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_STORE_EMPTY, g_plan.reason);
    TEST_ASSERT_FALSE(g_plan.restore_required);
    TEST_ASSERT_FALSE(g_plan.record_proposal.update_needed);
    TEST_ASSERT_FALSE(g_plan.counters.must_persist_before_action);
    TEST_ASSERT_TRUE(g_plan.terminal);

    make_ctx(STORE_CLEARED, NULL, POOL_RESET_CLASS_SOFTWARE);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_BOOT_NORMAL_SOURCE, g_plan.decision);
    assert_no_target(&g_plan);
}

TEST_CASE("store-result: every non-OK result is conservative, never target", "[pool_recovery]")
{
    static const PoolStoreResult bad[] = {
        STORE_NOT_INITIALIZED, STORE_INVALID_ARGUMENT, STORE_IO_ERROR, STORE_CORRUPT,
        STORE_UNSUPPORTED_SCHEMA, STORE_INVALID_RECORD, STORE_ACTIVE_POINTER_INVALID,
        STORE_ACTIVE_SLOT_INVALID, STORE_RECOVERY_REQUIRED, STORE_GENERATION_EXHAUSTED,
        STORE_READBACK_MISMATCH, STORE_COMMIT_UNCERTAIN, STORE_STATE_CONFLICT,
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        make_ctx(bad[i], NULL, POOL_RESET_CLASS_POWER_ON);
        (void)run_plan();
        TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RECOVERY_REQUIRED, g_plan.decision);
        TEST_ASSERT_EQUAL(POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY, g_plan.runtime_intent);
        TEST_ASSERT_EQUAL(POOL_BOOT_PERSIST_NO_RECORD_UPDATE, g_plan.persist_intent);
        assert_no_target(&g_plan);
        TEST_ASSERT_FALSE(g_plan.record_proposal.update_needed);
    }
    /* specific machine codes for the named corruption classes */
    make_ctx(STORE_CORRUPT, NULL, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL(RECOVERY_ERR_STORE_CORRUPT, g_plan.error);
    make_ctx(STORE_UNSUPPORTED_SCHEMA, NULL, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL(RECOVERY_ERR_STORE_UNSUPPORTED, g_plan.error);
    make_ctx(STORE_ACTIVE_POINTER_INVALID, NULL, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL(RECOVERY_ERR_STORE_AMBIGUOUS, g_plan.error);
    make_ctx(STORE_COMMIT_UNCERTAIN, NULL, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL(RECOVERY_ERR_STORE_UNCERTAIN, g_plan.error);
}

TEST_CASE("store-result: OK demands a valid SESSION record", "[pool_recovery]")
{
    /* OK without a record */
    make_ctx(STORE_OK, NULL, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL(RECOVERY_ERR_RECORD_MISSING, g_plan.error);
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RECOVERY_REQUIRED, g_plan.decision);

    /* OK with a tombstone-kind record */
    pool_session_record_init_tombstone(&g_rec);
    g_rec.generation = 2u;
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL(RECOVERY_ERR_RECORD_INVALID, g_plan.error);

    /* OK with a semantically invalid record */
    make_active_rec(&g_rec);
    g_rec.restore_required = false; /* TARGET_ACTIVE without the obligation */
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL(RECOVERY_ERR_RECORD_INVALID, g_plan.error);
    assert_no_target(&g_plan);

    /* EMPTY with a contradictory record present */
    make_active_rec(&g_rec);
    make_ctx(STORE_EMPTY, &g_rec, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL(RECOVERY_ERR_INVALID_ARGUMENT, g_plan.error);

    /* null arguments fail safely */
    TEST_ASSERT_EQUAL(RECOVERY_ERR_INVALID_ARGUMENT,
                      pool_session_recovery_plan(NULL, &g_plan));
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RECOVERY_REQUIRED, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_INVALID_ARGUMENT,
                      pool_session_recovery_plan(&g_ctx, NULL));
}

/* ================================================================= */
/* C/G. Persisted-state decision table                                */
/* ================================================================= */

TEST_CASE("state: snapshot-committed boots source and proposes cancellation", "[pool_recovery]")
{
    make_state_rec(&g_rec, POOL_STATE_TARGET_SNAPSHOT_COMMITTED, false);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_BOOT_NORMAL_SOURCE, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_ALLOW_SOURCE_ONLY, g_plan.allowed_config);
    TEST_ASSERT_EQUAL(POOL_BOOT_MINING_ALLOW_SOURCE, g_plan.mining_policy);
    TEST_ASSERT_EQUAL(POOL_BOOT_PERSIST_PROPOSE_RECOVERY_STATE_UPDATE, g_plan.persist_intent);
    TEST_ASSERT_TRUE(g_plan.record_proposal.update_needed);
    TEST_ASSERT_EQUAL(POOL_STATE_CANCELLED, g_plan.record_proposal.proposed_state);
    TEST_ASSERT_FALSE(g_plan.restore_required);
    assert_no_target(&g_plan);
}

TEST_CASE("state: interrupted target work restores the source", "[pool_recovery]")
{
    static const PoolSessionState states[] = {
        POOL_STATE_APPLYING_TARGET, POOL_STATE_TARGET_FAILED, POOL_STATE_INTERRUPTED,
    };
    size_t i;
    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        make_state_rec(&g_rec, states[i], true);
        make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
        TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
        TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
        TEST_ASSERT_EQUAL(POOL_BOOT_ALLOW_SOURCE_ONLY, g_plan.allowed_config);
        TEST_ASSERT_EQUAL(POOL_BOOT_MINING_VERIFY_BEFORE_MINING, g_plan.mining_policy);
        TEST_ASSERT_EQUAL(POOL_BOOT_RUNTIME_APPLY_SOURCE_CONFIGURATION, g_plan.runtime_intent);
        TEST_ASSERT_EQUAL(POOL_BOOT_PERSIST_PROPOSE_RESTORE_DUE, g_plan.persist_intent);
        TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, g_plan.record_proposal.proposed_state);
        TEST_ASSERT_TRUE(g_plan.restore_required);
        assert_no_target(&g_plan);
    }
}

TEST_CASE("state: restore-side states continue toward the source", "[pool_recovery]")
{
    make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_PERSIST_PROPOSE_COUNTER_UPDATE, g_plan.persist_intent);
    TEST_ASSERT_FALSE(g_plan.record_proposal.update_needed); /* state already right */

    make_state_rec(&g_rec, POOL_STATE_APPLYING_RESTORE, true);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_PANIC);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_ALLOW_SOURCE_ONLY, g_plan.allowed_config);
    assert_no_target(&g_plan);
}

TEST_CASE("state: terminal results are retained, source-only", "[pool_recovery]")
{
    make_state_rec(&g_rec, POOL_STATE_COMPLETE, false);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_MINING_ALLOW_SOURCE, g_plan.mining_policy);
    TEST_ASSERT_EQUAL(POOL_BOOT_PERSIST_RETAIN_TERMINAL_RESULT, g_plan.persist_intent);
    TEST_ASSERT_FALSE(g_plan.record_proposal.update_needed); /* never cleared */
    assert_no_target(&g_plan);

    make_state_rec(&g_rec, POOL_STATE_CANCELLED, false);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_SOFTWARE);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE, g_plan.decision);
    TEST_ASSERT_FALSE(g_plan.restore_required);
}

TEST_CASE("state: restore-failed retries bounded, then operator", "[pool_recovery]")
{
    /* below the budget: bounded automatic re-attempt */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_FAILED, true);
    g_rec.last_failure_code = (uint16_t)ERR_RESTORE_VERIFY_TIMEOUT;
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_PERSIST_PROPOSE_RESTORE_DUE, g_plan.persist_intent);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, g_plan.record_proposal.proposed_state);

    /* attempts exhausted: operator recovery, no automatic restart */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_FAILED, true);
    g_rec.recovery_attempt_count = POOL_RECORD_RECOVERY_ATTEMPT_MAX;
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RECOVERY_REQUIRED, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_COUNTER_EXHAUSTED, g_plan.error);
    TEST_ASSERT_EQUAL(POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY, g_plan.runtime_intent);
    TEST_ASSERT_EQUAL(POOL_STATE_RECOVERY_REQUIRED, g_plan.record_proposal.proposed_state);
    TEST_ASSERT_TRUE(g_plan.restore_required); /* the obligation survives */
    assert_no_target(&g_plan);
}

TEST_CASE("state: recovery-required splits on the obligation", "[pool_recovery]")
{
    /* obligation held + budget available: bounded restore */
    make_state_rec(&g_rec, POOL_STATE_RECOVERY_REQUIRED, true);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_UNKNOWN);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    TEST_ASSERT_TRUE(g_plan.restore_required);

    /* pre-mutation recovery result: retain + surface the operator */
    make_state_rec(&g_rec, POOL_STATE_RECOVERY_REQUIRED, false);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY, g_plan.runtime_intent);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_OPERATOR_REQUIRED, g_plan.reason);
    assert_no_target(&g_plan);
}

TEST_CASE("state: ephemeral persisted states are invalid records", "[pool_recovery]")
{
    static const PoolSessionState eph[] = {
        POOL_STATE_IDLE, POOL_STATE_PREPARING, POOL_STATE_RESTARTING_FOR_TARGET,
        POOL_STATE_VERIFYING_TARGET, POOL_STATE_RESTARTING_FOR_RESTORE,
        POOL_STATE_VERIFYING_RESTORE,
    };
    size_t i;
    for (i = 0; i < sizeof(eph) / sizeof(eph[0]); i++) {
        make_state_rec(&g_rec, eph[i], true);
        make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
        (void)run_plan();
        TEST_ASSERT_EQUAL(RECOVERY_ERR_RECORD_INVALID, g_plan.error);
        TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RECOVERY_REQUIRED, g_plan.decision);
        assert_no_target(&g_plan);
    }
}

/* ================================================================= */
/* D. TARGET_ACTIVE time decisions                                    */
/* ================================================================= */

TEST_CASE("time: trusted before the deadline yields the eligibility plan", "[pool_recovery]")
{
    make_active_rec(&g_rec);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    set_trusted(g_rec.deadline_epoch_s - 1234u);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_ALLOW_TARGET_ONLY, g_plan.allowed_config);
    /* ELIGIBILITY, not authorization: mining stays gated on LIVE
     * re-verification by the future runtime owner. */
    TEST_ASSERT_EQUAL(POOL_BOOT_MINING_VERIFY_BEFORE_MINING, g_plan.mining_policy);
    TEST_ASSERT_TRUE(g_plan.mining_policy != POOL_BOOT_MINING_ALLOW_TARGET);
    TEST_ASSERT_EQUAL(POOL_BOOT_RUNTIME_VERIFY_TARGET_CONFIGURATION, g_plan.runtime_intent);
    TEST_ASSERT_EQUAL(POOL_BOOT_PERSIST_PROPOSE_COUNTER_UPDATE, g_plan.persist_intent);
    TEST_ASSERT_TRUE(g_plan.remaining_valid);
    TEST_ASSERT_EQUAL_UINT64(1234ull, g_plan.remaining_target_s);
    TEST_ASSERT_TRUE(g_plan.trusted_time_required);
    TEST_ASSERT_TRUE(g_plan.restore_required); /* the obligation stays held */
    TEST_ASSERT_TRUE(g_plan.counters.must_persist_before_action);
}

TEST_CASE("time: persisted evidence alone never authorizes mining", "[pool_recovery]")
{
    /* The persisted target verification flags are pre-reboot facts. Even a
     * perfect record + perfect time yields VERIFY_BEFORE_MINING — the pure
     * engine has no live evidence and can never grant ALLOW_TARGET_MINING. */
    make_active_rec(&g_rec);
    TEST_ASSERT_TRUE(g_rec.target_verify.connection_observed);
    TEST_ASSERT_TRUE(g_rec.target_verify.mining_observed);
    TEST_ASSERT_TRUE(g_rec.target_verify.identity_verified);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    set_trusted(g_rec.deadline_epoch_s - 600u);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_MINING_VERIFY_BEFORE_MINING, g_plan.mining_policy);
    TEST_ASSERT_EQUAL(POOL_BOOT_RUNTIME_VERIFY_TARGET_CONFIGURATION, g_plan.runtime_intent);
    TEST_ASSERT_TRUE(g_plan.restore_required);
}

TEST_CASE("time: at or past the deadline restores, never resumes", "[pool_recovery]")
{
    make_active_rec(&g_rec);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    set_trusted(g_rec.deadline_epoch_s); /* exactly at */
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_DEADLINE_EXPIRED, g_plan.reason);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, g_plan.record_proposal.proposed_state);
    assert_no_target(&g_plan);

    set_trusted(g_rec.deadline_epoch_s + 7200u); /* long past */
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_DEADLINE_EXPIRED, g_plan.reason);
}

TEST_CASE("time: untrusted waits bounded, then fails safe", "[pool_recovery]")
{
    make_active_rec(&g_rec);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON); /* snapshot untrusted */

    g_ctx.sync_wait_elapsed_s = 0u;
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_TIME_UNTRUSTED, g_plan.reason);
    TEST_ASSERT_FALSE(g_plan.terminal);

    g_ctx.sync_wait_elapsed_s = WAIT_LIMIT_S - 1u;
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME, g_plan.decision);

    g_ctx.sync_wait_elapsed_s = WAIT_LIMIT_S; /* exactly at the bound */
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_TIME_TIMEOUT, g_plan.reason);

    g_ctx.sync_wait_elapsed_s = WAIT_LIMIT_S + 500u;
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    assert_no_target(&g_plan);
}

TEST_CASE("time: a missing UTC deadline restores immediately", "[pool_recovery]")
{
    make_active_rec(&g_rec);
    g_rec.deadline_valid = false;
    g_rec.deadline_epoch_s = 0u;
    g_rec.deadline_sync_generation = 0u;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    g_ctx.sync_wait_elapsed_s = 0u; /* no waiting is granted at all */
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_DEADLINE_MISSING, g_plan.reason);
    assert_no_target(&g_plan);
}

TEST_CASE("time: the persisted floor gates trust claims", "[pool_recovery]")
{
    PoolTimeTrustPolicy pol;
    /* the latest-accepted floor wins */
    make_active_rec(&g_rec);
    pool_session_recovery_build_time_policy(&g_rec, &pol);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 100u, pol.required_min_epoch_s);
    /* fallback: verified start only when no stronger floor exists */
    g_rec.latest_trusted_valid = false;
    g_rec.latest_trusted_epoch_s = 0u;
    pool_session_recovery_build_time_policy(&g_rec, &pol);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S, pol.required_min_epoch_s);
    /* neither: no floor */
    g_rec.verified_start_valid = false;
    g_rec.verified_start_epoch_s = 0u;
    g_rec.deadline_valid = false;
    g_rec.deadline_epoch_s = 0u;
    g_rec.deadline_sync_generation = 0u;
    pool_session_recovery_build_time_policy(&g_rec, &pol);
    TEST_ASSERT_EQUAL_UINT64(0ull, pol.required_min_epoch_s);

    /* a trusted snapshot EARLIER than the floor is a rejected trust claim:
     * bounded wait with TIME_REGRESSION, then fail-safe restore */
    make_active_rec(&g_rec); /* floor = EPOCH_A + 100 */
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    set_trusted(EPOCH_A_S + 50u); /* >= verified start, < floor */
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_TIME_REGRESSION, g_plan.reason);
    assert_no_target(&g_plan);
    g_ctx.sync_wait_elapsed_s = WAIT_LIMIT_S;
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
}

TEST_CASE("time: structural resume-predicate failures fail safe", "[pool_recovery]")
{
    /* invalid deadline sync generation */
    make_active_rec(&g_rec);
    g_rec.deadline_sync_generation = 0u; /* valid deadline, missing provenance */
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    set_trusted(g_rec.deadline_epoch_s - 1000u);
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_RECORD_INVALID, g_plan.reason);
    assert_no_target(&g_plan);

    /* time provider not initialized: untrusted path */
    make_active_rec(&g_rec);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    set_trusted(g_rec.deadline_epoch_s - 1000u);
    g_ctx.time_provider_initialized = false;
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME, g_plan.decision);

    /* snapshot trusted flag without TIME_OK status: rejected */
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    set_trusted(g_rec.deadline_epoch_s - 1000u);
    g_ctx.time_snapshot.status = TIME_ERR_NOT_SYNCED;
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME, g_plan.decision);
}

TEST_CASE("time: counter exhaustion blocks resume, surfaces the operator", "[pool_recovery]")
{
    make_active_rec(&g_rec);
    g_rec.consecutive_recovery_failures = POOL_RECORD_CONSECUTIVE_RECOVERY_FAIL_MAX;
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    set_trusted(g_rec.deadline_epoch_s - 1000u); /* time would allow resume */
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RECOVERY_REQUIRED, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_COUNTER_EXHAUSTED, g_plan.error);
    TEST_ASSERT_EQUAL(POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY, g_plan.runtime_intent);
    TEST_ASSERT_TRUE(g_plan.restore_required);
    assert_no_target(&g_plan);
}

TEST_CASE("time: conservative reset classes restore instead of resuming", "[pool_recovery]")
{
    static const PoolSessionResetClass cons[] = {
        POOL_RESET_CLASS_DEEP_SLEEP, POOL_RESET_CLASS_EXTERNAL, POOL_RESET_CLASS_UNKNOWN,
    };
    size_t i;
    for (i = 0; i < sizeof(cons) / sizeof(cons[0]); i++) {
        make_active_rec(&g_rec);
        make_ctx(STORE_OK, &g_rec, cons[i]);
        set_trusted(g_rec.deadline_epoch_s - 1000u); /* perfect time */
        (void)run_plan();
        TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
        TEST_ASSERT_EQUAL(RECOVERY_ERR_RESET_ABNORMAL, g_plan.reason);
        assert_no_target(&g_plan);
    }
    /* a watchdog reset within budget still goes through the time decision */
    make_active_rec(&g_rec);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_TASK_WATCHDOG);
    set_trusted(g_rec.deadline_epoch_s - 555u);
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET, g_plan.decision);
    TEST_ASSERT_EQUAL_UINT64(555ull, g_plan.remaining_target_s);
    /* the crash consumed consecutive-failure budget */
    TEST_ASSERT_TRUE(g_plan.counters.consecutive_changed);
}

/* ================================================================= */
/* E. Wait-state contract                                             */
/* ================================================================= */

TEST_CASE("wait: the wait plan inhibits everything and grants nothing", "[pool_recovery]")
{
    make_active_rec(&g_rec);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_ALLOW_NO_POOL, g_plan.allowed_config);
    TEST_ASSERT_EQUAL(POOL_BOOT_MINING_INHIBIT, g_plan.mining_policy);
    TEST_ASSERT_EQUAL(POOL_BOOT_RUNTIME_START_TRUSTED_TIME_WAIT, g_plan.runtime_intent);
    TEST_ASSERT_TRUE(g_plan.inhibit_target_stratum);
    TEST_ASSERT_TRUE(g_plan.trusted_time_required);
    TEST_ASSERT_TRUE(g_plan.restore_required);
    TEST_ASSERT_FALSE(g_plan.remaining_valid); /* no new duration is granted */
    TEST_ASSERT_FALSE(g_plan.terminal);        /* bounded progression pending */
}

TEST_CASE("wait: without guaranteed inhibition the plan restores immediately", "[pool_recovery]")
{
    make_active_rec(&g_rec);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    g_ctx.mining_inhibition_available = false; /* runtime cannot hold mining */
    TEST_ASSERT_EQUAL(RECOVERY_OK, run_plan());
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_MINING_INHIBITION_UNAVAILABLE, g_plan.reason);
    TEST_ASSERT_TRUE(g_plan.decision != POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME);
    assert_no_target(&g_plan);
}

/* ================================================================= */
/* I. Counter proposals                                               */
/* ================================================================= */

TEST_CASE("counters: proposals increment once, saturate and stay idempotent", "[pool_recovery]")
{
    make_active_rec(&g_rec);
    g_rec.reboot_count = 5u;
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL_UINT8(6u, g_plan.counters.reboot_count);
    TEST_ASSERT_TRUE(g_plan.counters.reboot_changed);
    TEST_ASSERT_FALSE(g_plan.counters.consecutive_changed); /* expected reset */
    /* re-evaluating the SAME context does not double-increment */
    (void)pool_session_recovery_plan(&g_ctx, &g_plan2);
    TEST_ASSERT_EQUAL_UINT8(6u, g_plan2.counters.reboot_count);
    TEST_ASSERT_EQUAL(0, memcmp(&g_plan, &g_plan2, sizeof(g_plan)));

    /* saturation at the storage maximum */
    g_rec.reboot_count = POOL_RECORD_REBOOT_COUNT_MAX;
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL_UINT8(POOL_RECORD_REBOOT_COUNT_MAX, g_plan.counters.reboot_count);
}

TEST_CASE("counters: attempts advance only on recovery actions", "[pool_recovery]")
{
    /* a restore plan consumes one attempt */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    g_rec.recovery_attempt_count = 2u;
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    TEST_ASSERT_EQUAL_UINT8(3u, g_plan.counters.recovery_attempt_count);
    TEST_ASSERT_TRUE(g_plan.counters.must_persist_before_action);

    /* a terminal retain plan does not */
    make_state_rec(&g_rec, POOL_STATE_COMPLETE, false);
    g_rec.recovery_attempt_count = 2u;
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    TEST_ASSERT_EQUAL_UINT8(2u, g_plan.counters.recovery_attempt_count);
    TEST_ASSERT_FALSE(g_plan.counters.recovery_attempt_changed);
}

TEST_CASE("counters: abnormal resets progress toward bounded exhaustion", "[pool_recovery]")
{
    /* one crash away from the limit: this boot exhausts the budget */
    make_active_rec(&g_rec);
    g_rec.consecutive_recovery_failures = POOL_RECORD_CONSECUTIVE_RECOVERY_FAIL_MAX - 1u;
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_BROWNOUT);
    set_trusted(g_rec.deadline_epoch_s - 1000u);
    (void)run_plan();
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RECOVERY_REQUIRED, g_plan.decision);
    TEST_ASSERT_EQUAL(RECOVERY_ERR_COUNTER_EXHAUSTED, g_plan.error);
    /* no plan ever requests an automatic reboot: the runtime intent is the
     * operator surface, not a restart */
    TEST_ASSERT_EQUAL(POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY, g_plan.runtime_intent);
}

/* ================================================================= */
/* J. Record-update proposal bounds                                   */
/* ================================================================= */

TEST_CASE("proposal: inputs stay byte-identical and bounded fields only", "[pool_recovery]")
{
    static const PoolSessionState states[] = {
        POOL_STATE_TARGET_SNAPSHOT_COMMITTED, POOL_STATE_APPLYING_TARGET,
        POOL_STATE_TARGET_ACTIVE, POOL_STATE_RESTORE_DUE, POOL_STATE_APPLYING_RESTORE,
        POOL_STATE_COMPLETE, POOL_STATE_TARGET_FAILED, POOL_STATE_RESTORE_FAILED,
        POOL_STATE_INTERRUPTED, POOL_STATE_RECOVERY_REQUIRED, POOL_STATE_CANCELLED,
    };
    size_t i;
    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        bool rr = !(states[i] == POOL_STATE_TARGET_SNAPSHOT_COMMITTED ||
                    states[i] == POOL_STATE_COMPLETE || states[i] == POOL_STATE_CANCELLED ||
                    states[i] == POOL_STATE_RECOVERY_REQUIRED);
        make_state_rec(&g_rec, states[i], rr);
        g_rec_copy = g_rec;
        make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_SOFTWARE);
        g_ctx_copy = g_ctx;
        (void)run_plan();
        /* the record and the context are never mutated */
        TEST_ASSERT_EQUAL(0, memcmp(&g_rec_copy, &g_rec, sizeof(g_rec)));
        TEST_ASSERT_EQUAL(0, memcmp(&g_ctx_copy, &g_ctx, sizeof(g_ctx)));
        /* proposals may only move toward safe states, never clear */
        if (g_plan.record_proposal.update_needed) {
            PoolSessionState ps = g_plan.record_proposal.proposed_state;
            TEST_ASSERT_TRUE(ps == POOL_STATE_RESTORE_DUE ||
                             ps == POOL_STATE_RECOVERY_REQUIRED ||
                             ps == POOL_STATE_CANCELLED);
            /* CANCELLED may only be proposed pre-mutation */
            if (ps == POOL_STATE_CANCELLED) {
                TEST_ASSERT_FALSE(g_rec.restore_required);
            }
        }
        /* the obligation is echoed, never discharged */
        TEST_ASSERT_EQUAL(g_rec.restore_required, g_plan.restore_required);
    }
}

/* ================================================================= */
/* K. Privacy                                                         */
/* ================================================================= */

TEST_CASE("privacy: every token table is clean", "[pool_recovery]")
{
    int i;
    for (i = 0; i < (int)POOL_RECOVERY_ERR__COUNT; i++) {
        assert_clean_token(pool_recovery_error_str((PoolRecoveryError)i));
    }
    for (i = 0; i < (int)POOL_BOOT_DECISION__COUNT; i++) {
        assert_clean_token(pool_boot_decision_str((PoolBootDecision)i));
    }
    for (i = 0; i < (int)POOL_BOOT_ALLOW__COUNT; i++) {
        assert_clean_token(pool_boot_allowed_config_str((PoolBootAllowedConfig)i));
    }
    for (i = 0; i < (int)POOL_BOOT_MINING__COUNT; i++) {
        assert_clean_token(pool_boot_mining_policy_str((PoolBootMiningPolicy)i));
    }
    for (i = 0; i < (int)POOL_BOOT_PERSIST__COUNT; i++) {
        assert_clean_token(pool_boot_persist_intent_str((PoolBootPersistIntent)i));
    }
    for (i = 0; i < (int)POOL_BOOT_RUNTIME__COUNT; i++) {
        assert_clean_token(pool_boot_runtime_intent_str((PoolBootRuntimeIntent)i));
        /* no intent can describe an automatic reboot */
        TEST_ASSERT_NULL(strstr(pool_boot_runtime_intent_str((PoolBootRuntimeIntent)i), "RESTART"));
        TEST_ASSERT_NULL(strstr(pool_boot_runtime_intent_str((PoolBootRuntimeIntent)i), "REBOOT"));
    }
    assert_clean_token(pool_recovery_error_str((PoolRecoveryError)999));
    assert_clean_token(pool_boot_decision_str((PoolBootDecision)999));
}

TEST_CASE("privacy: planted identity markers never reach the plan", "[pool_recovery]")
{
    make_active_rec(&g_rec);
    strncpy(g_rec.source.primary.host, "host-marker.example",
            sizeof(g_rec.source.primary.host) - 1);
    strncpy(g_rec.source.primary.user, "acct-marker.worker",
            sizeof(g_rec.source.primary.user) - 1);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    set_trusted(g_rec.deadline_epoch_s - 1000u);
    (void)run_plan();
    TEST_ASSERT_FALSE(bytes_contain(&g_plan, sizeof(g_plan), "marker"));
    TEST_ASSERT_FALSE(bytes_contain(&g_plan, sizeof(g_plan), "example"));
    TEST_ASSERT_FALSE(bytes_contain(&g_plan, sizeof(g_plan), "worker"));
}

/* ================================================================= */
/* L. Property-style safety proofs                                    */
/* ================================================================= */

TEST_CASE("property: store-result and reset-class sweep is total and safe", "[pool_recovery]")
{
    int res, rc;
    for (res = 0; res < (int)POOL_STORE_RESULT__COUNT + 3; res++) {
        for (rc = 0; rc < (int)POOL_RESET_CLASS__COUNT + 2; rc++) {
            make_active_rec(&g_rec);
            make_ctx((PoolStoreResult)res,
                     (res == (int)STORE_OK) ? &g_rec : NULL,
                     (PoolSessionResetClass)rc);
            g_ctx_copy = g_ctx;
            (void)run_plan();
            assert_plan_enums_valid(&g_plan);
            TEST_ASSERT_EQUAL(0, memcmp(&g_ctx_copy, &g_ctx, sizeof(g_ctx)));
            /* B4 never directly authorizes target mining, anywhere */
            TEST_ASSERT_TRUE(g_plan.mining_policy != POOL_BOOT_MINING_ALLOW_TARGET);
            if (res != (int)STORE_OK) {
                assert_no_target(&g_plan);
                TEST_ASSERT_FALSE(g_plan.record_proposal.update_needed);
            }
            /* determinism: byte-identical on repeat */
            (void)pool_session_recovery_plan(&g_ctx, &g_plan2);
            TEST_ASSERT_EQUAL(0, memcmp(&g_plan, &g_plan2, sizeof(g_plan)));
        }
    }
}

TEST_CASE("property: state and reset sweep preserves the obligation", "[pool_recovery]")
{
    static const PoolSessionState states[] = {
        POOL_STATE_TARGET_SNAPSHOT_COMMITTED, POOL_STATE_APPLYING_TARGET,
        POOL_STATE_TARGET_ACTIVE, POOL_STATE_RESTORE_DUE, POOL_STATE_APPLYING_RESTORE,
        POOL_STATE_COMPLETE, POOL_STATE_TARGET_FAILED, POOL_STATE_RESTORE_FAILED,
        POOL_STATE_INTERRUPTED, POOL_STATE_RECOVERY_REQUIRED, POOL_STATE_CANCELLED,
    };
    size_t si;
    int rc;
    for (si = 0; si < sizeof(states) / sizeof(states[0]); si++) {
        for (rc = 0; rc < (int)POOL_RESET_CLASS__COUNT; rc++) {
            bool rr = !(states[si] == POOL_STATE_TARGET_SNAPSHOT_COMMITTED ||
                        states[si] == POOL_STATE_COMPLETE ||
                        states[si] == POOL_STATE_CANCELLED ||
                        states[si] == POOL_STATE_RECOVERY_REQUIRED);
            make_state_rec(&g_rec, states[si], rr);
            g_rec_copy = g_rec;
            make_ctx(STORE_OK, &g_rec, (PoolSessionResetClass)rc);
            (void)run_plan();
            assert_plan_enums_valid(&g_plan);
            /* B4 never directly authorizes target mining, anywhere */
            TEST_ASSERT_TRUE(g_plan.mining_policy != POOL_BOOT_MINING_ALLOW_TARGET);
            /* the source identity is never touched */
            TEST_ASSERT_EQUAL(0, memcmp(&g_rec_copy.source, &g_rec.source,
                                        sizeof(g_rec.source)));
            TEST_ASSERT_EQUAL(0, memcmp(&g_rec_copy, &g_rec, sizeof(g_rec)));
            if (rr) {
                /* an owed restore can never end in idle/cancel/clear */
                TEST_ASSERT_TRUE(g_plan.decision != POOL_BOOT_DECISION_BOOT_NORMAL_SOURCE);
                TEST_ASSERT_TRUE(g_plan.decision != POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE);
                if (g_plan.record_proposal.update_needed) {
                    TEST_ASSERT_TRUE(g_plan.record_proposal.proposed_state !=
                                     POOL_STATE_CANCELLED);
                    TEST_ASSERT_TRUE(g_plan.record_proposal.proposed_state !=
                                     POOL_STATE_IDLE);
                    TEST_ASSERT_TRUE(g_plan.record_proposal.proposed_state !=
                                     POOL_STATE_COMPLETE);
                }
            }
            /* no counter ever wraps */
            TEST_ASSERT_TRUE(g_plan.counters.reboot_count >= g_rec.reboot_count);
            TEST_ASSERT_TRUE(g_plan.counters.recovery_attempt_count >=
                             g_rec.recovery_attempt_count);
            TEST_ASSERT_TRUE(g_plan.counters.consecutive_recovery_failures >=
                             g_rec.consecutive_recovery_failures);
        }
    }
}

TEST_CASE("property: TARGET_ACTIVE timing permutations yield one safe class", "[pool_recovery]")
{
    const uint64_t deadline = EPOCH_A_S + 3600u;
    const uint64_t epochs[4] = {EPOCH_A_S + 100u, deadline - 1u, deadline, deadline + 1u};
    const uint32_t elapsed[3] = {0u, WAIT_LIMIT_S - 1u, WAIT_LIMIT_S};
    int tr, ep, el, inh;
    int resumes = 0;

    for (tr = 0; tr <= 1; tr++)
    for (ep = 0; ep < 4; ep++)
    for (el = 0; el < 3; el++)
    for (inh = 0; inh <= 1; inh++) {
        make_active_rec(&g_rec);
        make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
        if (tr == 1) {
            set_trusted(epochs[ep]);
        }
        g_ctx.sync_wait_elapsed_s = elapsed[el];
        g_ctx.mining_inhibition_available = (inh == 1);
        (void)run_plan();
        /* exactly one safe class */
        TEST_ASSERT_TRUE(g_plan.decision == POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME ||
                         g_plan.decision == POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET ||
                         g_plan.decision == POOL_BOOT_DECISION_RESTORE_SOURCE_NOW ||
                         g_plan.decision == POOL_BOOT_DECISION_RECOVERY_REQUIRED);
        /* NO B4 plan ever directly authorizes target mining */
        TEST_ASSERT_TRUE(g_plan.mining_policy != POOL_BOOT_MINING_ALLOW_TARGET);
        /* the eligibility plan requires genuine trust before the deadline
         * and always demands live verification before mining */
        if (g_plan.decision == POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET) {
            TEST_ASSERT_EQUAL(1, tr);
            TEST_ASSERT_TRUE(epochs[ep] < deadline);
            TEST_ASSERT_EQUAL(POOL_BOOT_MINING_VERIFY_BEFORE_MINING, g_plan.mining_policy);
            TEST_ASSERT_EQUAL(POOL_BOOT_RUNTIME_VERIFY_TARGET_CONFIGURATION,
                              g_plan.runtime_intent);
            TEST_ASSERT_TRUE(g_plan.remaining_valid);
            TEST_ASSERT_TRUE(g_plan.remaining_target_s > 0u);
            TEST_ASSERT_TRUE(g_plan.restore_required);
            resumes++;
        }
        /* WAIT only within the bound and only with inhibition available */
        if (g_plan.decision == POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME) {
            TEST_ASSERT_TRUE(elapsed[el] < WAIT_LIMIT_S);
            TEST_ASSERT_EQUAL(1, inh);
            TEST_ASSERT_EQUAL(POOL_BOOT_MINING_INHIBIT, g_plan.mining_policy);
        }
    }
    TEST_ASSERT_TRUE(resumes > 0); /* the eligibility path is genuinely reachable */
}

TEST_CASE("property: plan fingerprints are deterministic and discriminating", "[pool_recovery]")
{
    uint32_t fp_wait, fp_resume;
    make_active_rec(&g_rec);
    make_ctx(STORE_OK, &g_rec, POOL_RESET_CLASS_POWER_ON);
    (void)run_plan();
    fp_wait = g_plan.plan_fingerprint;
    (void)pool_session_recovery_plan(&g_ctx, &g_plan2);
    TEST_ASSERT_EQUAL_UINT32(fp_wait, g_plan2.plan_fingerprint);

    set_trusted(g_rec.deadline_epoch_s - 1000u);
    (void)run_plan();
    fp_resume = g_plan.plan_fingerprint;
    TEST_ASSERT_TRUE(fp_wait != fp_resume); /* different plans, different prints */
}
