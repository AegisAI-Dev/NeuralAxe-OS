/*
 * Deterministic tests for the PURE Gate B6 runtime controller.
 *
 * Every plan used here is produced by the REAL committed B4 engine and every
 * ownership posture by the REAL committed B5 bootstrap — the runtime table is
 * never fed a hand-written plan except where an impossible plan is
 * deliberately injected to prove the fail-closed path.
 *
 * Nothing in this file touches NVS, SNTP, networking, pools, OTA or restart.
 * All identities are synthetic "*.example" fixtures.
 */

#include <string.h>
#include "unity.h"
#include "pool_session_runtime_core.h"
#include "pool_session_runtime_boot.h"
#include "pool_session_runtime.h"
#include "pool_operation_policy.h"

#define EPOCH_A_S  1750000000ull
#define SESSION_ID 4242u

static PoolSessionRecord        g_rec;
static PoolSessionBootContext   g_bctx;
static PoolSessionRecoveryPlan  g_plan;
static PoolOperationState       g_lease;
static PoolOperationLeaseToken  g_tok;
static PoolRuntimeClassifyInput g_in;
static PoolRuntimeDecision      g_dec;
static PoolRuntimeSnapshot      g_snap;

/* ---------------- fixtures ---------------- */

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

static void make_rec(PoolSessionRecord *r, PoolSessionState st, bool restore_required)
{
    pool_session_record_init(r);
    r->generation        = 3u;
    r->session_id        = SESSION_ID;
    r->b1_model_version  = POOL_SESSION_MODEL_VERSION;
    r->state             = st;
    fill_identity(&r->source, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    fill_identity(&r->target, POOL_CHAIN_BITCOIN_CASH, "bch.example", 3334, "acct.worker");
    r->password_policy   = POOL_SESSION_PW_KEEP_CURRENT;
    r->restore_required  = restore_required;
    r->target_verify.connection_observed = true;
    r->target_verify.mining_observed     = true;
    r->target_verify.identity_verified   = true;
    r->duration_s               = 3600u;
    r->verified_start_valid     = true;
    r->verified_start_epoch_s   = EPOCH_A_S;
    r->deadline_valid           = true;
    r->deadline_epoch_s         = EPOCH_A_S + 3600u;
    r->deadline_sync_generation = 1u;
    r->latest_trusted_valid     = true;
    r->latest_trusted_epoch_s   = EPOCH_A_S + 100u;
    if (st == POOL_STATE_COMPLETE) {
        r->restore_verify.connection_observed = true;
        r->restore_verify.mining_observed     = true;
        r->restore_verify.identity_verified   = true;
    }
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(r));
}

static void set_trusted_time(PoolSessionBootContext *ctx, uint64_t epoch_s)
{
    ctx->time_provider_initialized       = true;
    ctx->time_snapshot.trusted           = true;
    ctx->time_snapshot.status            = TIME_OK;
    ctx->time_snapshot.trusted_epoch_s   = epoch_s;
    ctx->time_snapshot.trusted_utc_us    = epoch_s * POOL_TIME_US_PER_S;
    ctx->time_snapshot.sync_generation   = 1u;
    ctx->time_snapshot.monotonic_now_us  = 5ull * POOL_TIME_US_PER_S;
    ctx->time_snapshot.anchor_age_us     = 1ull * POOL_TIME_US_PER_S;
}

/* Run the real B4 engine and the real B5 bootstrap, then build the runtime
 * classification input exactly as the adapter does. */
static void boot_into_epoch(PoolStoreResult res, const PoolSessionRecord *rec,
                            uint64_t trusted_epoch_s /* 0 = untrusted */,
                            uint32_t wait_elapsed_s)
{
    PoolOperationBootstrapInput bi;

    memset(&g_bctx, 0, sizeof(g_bctx));
    g_bctx.store_result              = res;
    g_bctx.record_present            = (rec != NULL);
    g_bctx.record                    = rec;
    g_bctx.reset_class               = POOL_RESET_CLASS_POWER_ON;
    g_bctx.sync_wait_elapsed_s       = wait_elapsed_s;
    g_bctx.sync_wait_limit_s         = 600u;
    g_bctx.mining_inhibition_available = true;
    if (trusted_epoch_s != 0u) {
        set_trusted_time(&g_bctx, trusted_epoch_s);
    } else {
        g_bctx.time_provider_initialized = true;
        g_bctx.time_snapshot.status      = TIME_ERR_NOT_SYNCED;
    }
    (void)pool_session_recovery_plan(&g_bctx, &g_plan);

    memset(&bi, 0, sizeof(bi));
    bi.store_result         = res;
    bi.record_present       = (rec != NULL);
    bi.record               = rec;
    bi.plan                 = &g_plan;
    bi.committed_generation = (rec != NULL) ? rec->generation : 0u;

    pool_operation_state_init(&g_lease);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_bootstrap(&g_lease, &bi, &g_tok));

    memset(&g_in, 0, sizeof(g_in));
    g_in.store_opened               = true;
    g_in.store_loaded               = true;
    g_in.store_result               = res;
    g_in.record_present             = (rec != NULL);
    g_in.record                     = rec;
    g_in.plan                       = &g_plan;
    g_in.bootstrap_status           = OP_OK;
    g_in.lease_owner                = g_lease.owner;
    g_in.lease_phase                = g_lease.phase;
    g_in.lease_generation           = g_lease.lease_generation;
    g_in.terminal_pending           = g_lease.terminal_pending;
    g_in.lease_restore_required     = g_lease.restore_required;
    g_in.lease_persistence_required = g_lease.persistence_required_before_action;
    /* Assume any mandatory proposal already committed + read-back verified,
     * so the tests below observe the POST-persistence posture. */
    if (pool_runtime_plan_requires_persistence(&g_plan)) {
        g_in.persist_attempted = true;
        g_in.persist_verified  = true;
        g_in.persist_result    = STORE_OK;
    }
}

static void boot_into(PoolStoreResult res, const PoolSessionRecord *rec,
                      bool trusted, uint32_t wait_elapsed_s)
{
    boot_into_epoch(res, rec, trusted ? (EPOCH_A_S + 100u) : 0u, wait_elapsed_s);
}

static void classify(void)
{
    /* Evaluate the call FIRST: the returned status must equal the status the
     * call just wrote into the decision. */
    PoolRuntimeStatus ret = pool_runtime_classify(&g_in, &g_dec);
    TEST_ASSERT_EQUAL(g_dec.status, ret);
    /* Gate B6 prohibitions hold on EVERY produced decision, unconditionally. */
    TEST_ASSERT_FALSE(g_dec.target_mining_authorized);
    TEST_ASSERT_FALSE(g_dec.pool_mutation_permitted);
    /* The permission always matches the single total state rule. */
    TEST_ASSERT_EQUAL(pool_runtime_protocol_for_state(g_dec.state), g_dec.protocol);
}

/* ================================================================= */
/* Feature flag                                                       */
/* ================================================================= */

TEST_CASE("rt core: feature-disabled boot action is no-bootstrap + unchanged startup",
          "[pool_runtime]")
{
    PoolRuntimeBootAction off = pool_runtime_boot_action_for_feature(false);
    TEST_ASSERT_FALSE(off.bootstrap_required);
    TEST_ASSERT_TRUE(off.protocol_allowed);
}

TEST_CASE("rt core: feature-enabled boot action requires bootstrap and holds first",
          "[pool_runtime]")
{
    PoolRuntimeBootAction on = pool_runtime_boot_action_for_feature(true);
    TEST_ASSERT_TRUE(on.bootstrap_required);
    TEST_ASSERT_FALSE(on.protocol_allowed);
}

TEST_CASE("rt core: the default build reports the feature disabled and has no instance",
          "[pool_runtime]")
{
    /* CONFIG_NX_TIMED_SESSIONS defaults to n, so this build must expose NO
     * production instance at all: nothing to bootstrap, no task, no nx_tps. */
    TEST_ASSERT_FALSE(nx_timed_sessions_enabled());
    TEST_ASSERT_NULL(pool_session_runtime_default_instance());
    TEST_ASSERT_FALSE(nx_timed_sessions_boot_init());
    /* Protocol startup is unchanged when the feature is off. */
    TEST_ASSERT_TRUE(nx_timed_sessions_protocol_start_allowed());
    nx_timed_sessions_notify_network_ready(); /* no-op, must not crash */
    TEST_ASSERT_EQUAL(0u, pool_session_runtime_task_count());
}

/* ================================================================= */
/* Protocol permission totality                                       */
/* ================================================================= */

TEST_CASE("rt core: only FREE and TERMINAL_PENDING allow protocol start", "[pool_runtime]")
{
    int i;
    for (i = 0; i < (int)POOL_RUNTIME_STATE__COUNT; i++) {
        PoolRuntimeState s = (PoolRuntimeState)i;
        bool expect_allow = (s == RUNTIME_FREE || s == RUNTIME_TERMINAL_PENDING);
        TEST_ASSERT_EQUAL(expect_allow ? POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE
                                       : POOL_RUNTIME_PROTOCOL_HOLD,
                          pool_runtime_protocol_for_state(s));
        TEST_ASSERT_EQUAL(!expect_allow, pool_runtime_state_holds_protocol(s));
    }
    /* Out-of-range values fail closed. */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_runtime_protocol_for_state((PoolRuntimeState)99));
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD,
                      pool_runtime_protocol_for_state(POOL_RUNTIME_STATE__COUNT));
}

/* ================================================================= */
/* Store lifecycle: a failure is never "empty"                        */
/* ================================================================= */

TEST_CASE("rt core: a failed store OPEN guards and never becomes STORE_EMPTY",
          "[pool_runtime]")
{
    boot_into(STORE_EMPTY, NULL, false, 0u);
    g_in.store_opened = false;
    g_in.store_loaded = false;
    g_in.store_result = STORE_NOT_INITIALIZED;
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_STORE_OPEN_FAILED, g_dec.status);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
}

TEST_CASE("rt core: a failed store LOAD guards and never becomes STORE_EMPTY",
          "[pool_runtime]")
{
    boot_into(STORE_EMPTY, NULL, false, 0u);
    g_in.store_loaded = false;
    g_in.store_result = STORE_IO_ERROR;
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_STORE_LOAD_FAILED, g_dec.status);
}

/* ================================================================= */
/* Proven-safe postures (the only two ALLOW paths)                    */
/* ================================================================= */

TEST_CASE("rt core: STORE_EMPTY is RUNTIME_FREE and permits normal startup",
          "[pool_runtime]")
{
    boot_into(STORE_EMPTY, NULL, false, 0u);
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_FREE, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE, g_dec.protocol);
    TEST_ASSERT_FALSE(g_dec.restore_required);
}

TEST_CASE("rt core: STORE_CLEARED is RUNTIME_FREE and permits normal startup",
          "[pool_runtime]")
{
    boot_into(STORE_CLEARED, NULL, false, 0u);
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_FREE, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE, g_dec.protocol);
}

TEST_CASE("rt core: retained COMPLETE is TERMINAL_PENDING and permits source startup",
          "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_COMPLETE, false);
    boot_into(STORE_OK, &g_rec, true, 0u);
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE, g_plan.decision);
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_TERMINAL_PENDING, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE, g_dec.protocol);
}

TEST_CASE("rt core: retained pre-mutation CANCELLED is TERMINAL_PENDING and allows",
          "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_CANCELLED, false);
    boot_into(STORE_OK, &g_rec, true, 0u);
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_TERMINAL_PENDING, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE, g_dec.protocol);
    TEST_ASSERT_FALSE(g_dec.restore_required);
}

TEST_CASE("rt core: a terminal with an unresolved obligation never allows",
          "[pool_runtime]")
{
    /* Only a proven-safe terminal may allow. Inject the contradictory
     * obligation on the lease and the runtime must fail closed. */
    make_rec(&g_rec, POOL_STATE_COMPLETE, false);
    boot_into(STORE_OK, &g_rec, true, 0u);
    g_in.lease_restore_required = true;
    classify();
    TEST_ASSERT_NOT_EQUAL(RUNTIME_TERMINAL_PENDING, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
    TEST_ASSERT_TRUE(g_dec.restore_required);
}

/* ================================================================= */
/* Held postures                                                      */
/* ================================================================= */

TEST_CASE("rt core: TARGET_ACTIVE resume is verification-only and holds", "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_MINING_VERIFY_BEFORE_MINING, g_plan.mining_policy);
    TEST_ASSERT_EQUAL(OP_PHASE_VERIFYING_TARGET, g_lease.phase);
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_VERIFY_TARGET_PENDING, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
    TEST_ASSERT_TRUE(g_dec.restore_required);
    /* Eligibility is never authorization. */
    TEST_ASSERT_FALSE(g_dec.target_mining_authorized);
    TEST_ASSERT_FALSE(g_dec.pool_mutation_permitted);
}

TEST_CASE("rt core: an untrusted clock inside the window WAITS and holds", "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    boot_into(STORE_OK, &g_rec, false, 0u);
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME, g_plan.decision);
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
    TEST_ASSERT_TRUE(g_dec.trusted_time_required);
}

TEST_CASE("rt core: an exhausted bounded wait becomes restore-pending and still holds",
          "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    boot_into(STORE_OK, &g_rec, false, 600u); /* window fully elapsed */
    TEST_ASSERT_NOT_EQUAL(POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME, g_plan.decision);
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RESTORE_SOURCE_PENDING, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
    TEST_ASSERT_TRUE(g_dec.restore_required);
}

TEST_CASE("rt core: an expired deadline is restore-pending, never executed",
          "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    /* Boot with the clock already past the persisted UTC deadline. */
    boot_into_epoch(STORE_OK, &g_rec, g_rec.deadline_epoch_s + 60u, 0u);
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESTORE_SOURCE_NOW, g_plan.decision);
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RESTORE_SOURCE_PENDING, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
}

TEST_CASE("rt core: RESTORE_DUE is restore-pending and holds", "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RESTORE_SOURCE_PENDING, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
}

TEST_CASE("rt core: RESTORE_FAILED holds and never allows startup", "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_RESTORE_FAILED, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    classify();
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
    TEST_ASSERT_TRUE(g_dec.restore_required);
}

TEST_CASE("rt core: a persisted RECOVERY_REQUIRED record guards and holds", "[pool_runtime]")
{
    /* Without an outstanding obligation this is the never-acknowledgeable
     * terminal that B5 turns into the guard (its owner-directed correction). */
    make_rec(&g_rec, POOL_STATE_RECOVERY_REQUIRED, false);
    boot_into(STORE_OK, &g_rec, true, 0u);
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_lease.phase);
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);

    /* With an outstanding obligation the restore duty dominates — still HELD,
     * and still nothing is executed. */
    make_rec(&g_rec, POOL_STATE_RECOVERY_REQUIRED, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    classify();
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
    TEST_ASSERT_TRUE(g_dec.restore_required);
    TEST_ASSERT_FALSE(g_dec.target_mining_authorized);
}

TEST_CASE("rt core: an operator-recovery lease dominates and holds", "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    g_in.lease_owner = OP_OWNER_OPERATOR_RECOVERY;
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_OPERATOR_RECOVERY, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
    TEST_ASSERT_FALSE(g_dec.target_mining_authorized);
}

/* ================================================================= */
/* Every non-OK store result, exhaustively                            */
/* ================================================================= */

TEST_CASE("rt core: no non-OK store result other than EMPTY/CLEARED ever allows",
          "[pool_runtime]")
{
    int i;
    for (i = 0; i < (int)POOL_STORE_RESULT__COUNT; i++) {
        PoolStoreResult res = (PoolStoreResult)i;
        bool with_record = (res == STORE_OK);
        if (with_record) {
            make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
        }
        boot_into(res, with_record ? &g_rec : NULL, true, 0u);
        classify();
        if (res == STORE_EMPTY || res == STORE_CLEARED) {
            TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE, g_dec.protocol);
            TEST_ASSERT_EQUAL(RUNTIME_FREE, g_dec.state);
        } else {
            TEST_ASSERT_EQUAL_MESSAGE(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol,
                                      pool_store_result_str(res));
        }
        TEST_ASSERT_FALSE(g_dec.target_mining_authorized);
        TEST_ASSERT_FALSE(g_dec.pool_mutation_permitted);
    }
}

TEST_CASE("rt core: STORE_COMMIT_UNCERTAIN on load guards and holds", "[pool_runtime]")
{
    boot_into(STORE_COMMIT_UNCERTAIN, NULL, true, 0u);
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_STORE_UNCERTAIN, g_dec.status);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
}

TEST_CASE("rt core: STORE_COMMIT_UNCERTAIN on a proposal guards and holds", "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    g_in.persist_attempted = true;
    g_in.persist_verified  = false;
    g_in.persist_result    = STORE_COMMIT_UNCERTAIN;
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_STORE_UNCERTAIN, g_dec.status);
}

/* ================================================================= */
/* Persistence before action                                          */
/* ================================================================= */

TEST_CASE("rt core: an unpersisted mandatory proposal is PERSISTENCE_PENDING and holds",
          "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_APPLYING_TARGET, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    TEST_ASSERT_TRUE(pool_runtime_plan_requires_persistence(&g_plan));
    g_in.persist_attempted = false;
    g_in.persist_verified  = false;
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_PERSISTENCE_PENDING, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_REQUIRED, g_dec.status);
    TEST_ASSERT_TRUE(g_dec.persistence_pending);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
}

TEST_CASE("rt core: a failed proposal commit stays PERSISTENCE_PENDING", "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_APPLYING_TARGET, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    g_in.persist_attempted = true;
    g_in.persist_verified  = false;
    g_in.persist_result    = STORE_IO_ERROR;
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_PERSISTENCE_PENDING, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_FAILED, g_dec.status);
}

TEST_CASE("rt core: a committed proposal that fails read-back stays pending",
          "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_APPLYING_TARGET, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    g_in.persist_attempted = true;
    g_in.persist_verified  = false;
    g_in.persist_result    = STORE_OK; /* wrote, but the reload disagreed */
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_PERSISTENCE_PENDING, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_READBACK, g_dec.status);
}

TEST_CASE("rt core: an identical proposal is never committed twice", "[pool_runtime]")
{
    PoolRuntimeControl c;
    pool_runtime_control_init(&c, 600u);

    TEST_ASSERT_TRUE(pool_runtime_proposal_should_commit(&c, 0xABCDu));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_runtime_proposal_record_commit(&c, 0xABCDu));
    TEST_ASSERT_EQUAL(1u, c.proposal_commits);

    /* Duplicate: refused, and the counter does NOT move. */
    TEST_ASSERT_FALSE(pool_runtime_proposal_should_commit(&c, 0xABCDu));
    TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_DUPLICATE,
                      pool_runtime_proposal_record_commit(&c, 0xABCDu));
    TEST_ASSERT_EQUAL(1u, c.proposal_commits);

    /* A genuinely different proposal is a new commit. */
    TEST_ASSERT_TRUE(pool_runtime_proposal_should_commit(&c, 0x1234u));
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_runtime_proposal_record_commit(&c, 0x1234u));
    TEST_ASSERT_EQUAL(2u, c.proposal_commits);
}

TEST_CASE("rt core: proposal read-back proof accepts only an exact landing",
          "[pool_runtime]")
{
    PoolSessionRecord after;

    make_rec(&g_rec, POOL_STATE_APPLYING_TARGET, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    TEST_ASSERT_TRUE(pool_runtime_plan_requires_persistence(&g_plan));

    after = g_rec;
    after.generation = g_rec.generation + 1u;
    if (g_plan.record_proposal.update_needed) {
        after.state             = g_plan.record_proposal.proposed_state;
        after.last_failure_code = g_plan.record_proposal.proposed_failure_code;
    }
    after.reboot_count                  = g_plan.counters.reboot_count;
    after.recovery_attempt_count        = g_plan.counters.recovery_attempt_count;
    after.consecutive_recovery_failures = g_plan.counters.consecutive_recovery_failures;
    after.last_reset_class              = g_plan.counters.reset_class_for_record;

    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_runtime_verify_proposal_readback(&g_rec, &after, &g_plan, STORE_OK));

    /* A non-OK reload is never a proof. */
    TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_READBACK,
                      pool_runtime_verify_proposal_readback(&g_rec, &after, &g_plan,
                                                            STORE_IO_ERROR));
    /* The restore obligation may never be discharged by a B6 proposal. */
    {
        PoolSessionRecord bad = after;
        bad.restore_required = !g_rec.restore_required;
        TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_READBACK,
                          pool_runtime_verify_proposal_readback(&g_rec, &bad, &g_plan, STORE_OK));
    }
    /* The trusted-epoch floor may never be lowered. */
    {
        PoolSessionRecord bad = after;
        bad.latest_trusted_epoch_s = g_rec.latest_trusted_epoch_s - 1u;
        TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_READBACK,
                          pool_runtime_verify_proposal_readback(&g_rec, &bad, &g_plan, STORE_OK));
    }
    /* Counters must be exactly the proposed values. */
    {
        PoolSessionRecord bad = after;
        bad.reboot_count = (uint8_t)(after.reboot_count + 1u);
        TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_READBACK,
                          pool_runtime_verify_proposal_readback(&g_rec, &bad, &g_plan, STORE_OK));
    }
    /* A commit must produce a strictly newer generation. */
    {
        PoolSessionRecord bad = after;
        bad.generation = g_rec.generation;
        TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_READBACK,
                          pool_runtime_verify_proposal_readback(&g_rec, &bad, &g_plan, STORE_OK));
    }
    /* The session identity must survive. */
    {
        PoolSessionRecord bad = after;
        bad.session_id = SESSION_ID + 1u;
        TEST_ASSERT_EQUAL(RUNTIME_ERR_PERSIST_READBACK,
                          pool_runtime_verify_proposal_readback(&g_rec, &bad, &g_plan, STORE_OK));
    }
    TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT,
                      pool_runtime_verify_proposal_readback(NULL, &after, &g_plan, STORE_OK));
}

/* ================================================================= */
/* Fail-closed inputs                                                 */
/* ================================================================= */

TEST_CASE("rt core: NULL and malformed inputs fail closed", "[pool_runtime]")
{
    TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT, pool_runtime_classify(NULL, NULL));

    TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT, pool_runtime_classify(NULL, &g_dec));
    TEST_ASSERT_EQUAL(RUNTIME_ERROR, g_dec.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);

    /* A missing plan can never allow. */
    boot_into(STORE_EMPTY, NULL, false, 0u);
    g_in.plan = NULL;
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_PLAN_INVALID, g_dec.status);
}

TEST_CASE("rt core: a failed B5 bootstrap is RUNTIME_ERROR and holds", "[pool_runtime]")
{
    boot_into(STORE_EMPTY, NULL, false, 0u);
    g_in.bootstrap_status = OP_ERR_INTERNAL_CONSISTENCY;
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_ERROR, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_BOOTSTRAP_FAILED, g_dec.status);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol);
}

TEST_CASE("rt core: a plan claiming a target-mining grant is impossible and guards",
          "[pool_runtime]")
{
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    g_plan.mining_policy = POOL_BOOT_MINING_ALLOW_TARGET; /* B4 never emits this */
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_PLAN_INVALID, g_dec.status);
    TEST_ASSERT_FALSE(g_dec.target_mining_authorized);
}

TEST_CASE("rt core: a lease phase disagreeing with the plan guards", "[pool_runtime]")
{
    /* B5 says "verify the target", the plan says "restore now": disagreement
     * is never resolved by guessing. */
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    TEST_ASSERT_EQUAL(OP_PHASE_VERIFYING_TARGET, g_in.lease_phase);
    g_plan.decision = POOL_BOOT_DECISION_RESTORE_SOURCE_NOW;
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_OWNERSHIP_MISMATCH, g_dec.status);
}

TEST_CASE("rt core: every B5 phase either maps explicitly or fails closed", "[pool_runtime]")
{
    int i;
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    for (i = 0; i < (int)OP_PHASE__COUNT; i++) {
        boot_into(STORE_OK, &g_rec, true, 0u);
        g_in.lease_phase = (PoolOperationLeasePhase)i;
        classify();
        /* Only a coherent FREE/terminal posture may ever allow, and neither
         * is reachable from this record. */
        TEST_ASSERT_EQUAL_MESSAGE(POOL_RUNTIME_PROTOCOL_HOLD, g_dec.protocol,
                                  pool_operation_phase_str((PoolOperationLeasePhase)i));
    }
    /* An out-of-range phase fails closed too. */
    boot_into(STORE_OK, &g_rec, true, 0u);
    g_in.lease_phase = (PoolOperationLeasePhase)77;
    classify();
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, g_dec.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_INTERNAL_CONSISTENCY, g_dec.status);
}

/* ================================================================= */
/* Snapshot                                                           */
/* ================================================================= */

TEST_CASE("rt core: the snapshot is consistent, sanitized and carries no identity",
          "[pool_runtime]")
{
    const char *needles[] = { "btc.example", "bch.example", "acct.worker" };
    size_t n;

    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    boot_into(STORE_OK, &g_rec, true, 0u);
    classify();

    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_runtime_snapshot_build(&g_in, &g_dec, POOL_RESET_CLASS_PANIC,
                                                  g_rec.generation, 30u, 600u, 1u, true,
                                                  &g_snap));
    TEST_ASSERT_TRUE(pool_runtime_snapshot_valid(&g_snap));
    TEST_ASSERT_EQUAL(POOL_RUNTIME_MODEL_VERSION, g_snap.model_version);
    TEST_ASSERT_EQUAL(g_dec.state, g_snap.state);
    TEST_ASSERT_EQUAL(pool_runtime_protocol_for_state(g_dec.state), g_snap.protocol);
    TEST_ASSERT_EQUAL(POOL_RESET_CLASS_PANIC, g_snap.reset_class);
    TEST_ASSERT_TRUE(g_snap.session_present);
    TEST_ASSERT_FALSE(g_snap.target_mining_authorized);
    TEST_ASSERT_FALSE(g_snap.pool_mutation_permitted);

    /* Privacy: no identity byte sequence appears anywhere in the snapshot, and
     * the session identifier is deliberately absent. */
    for (n = 0; n < sizeof(needles) / sizeof(needles[0]); n++) {
        const uint8_t *h = (const uint8_t *)&g_snap;
        size_t nl = strlen(needles[n]);
        size_t i;
        for (i = 0; i + nl <= sizeof(g_snap); i++) {
            TEST_ASSERT_FALSE(memcmp(h + i, needles[n], nl) == 0);
        }
    }
    {
        const uint8_t *h = (const uint8_t *)&g_snap;
        uint32_t sid = SESSION_ID;
        size_t i;
        for (i = 0; i + sizeof(sid) <= sizeof(g_snap); i++) {
            TEST_ASSERT_FALSE(memcmp(h + i, &sid, sizeof(sid)) == 0);
        }
    }
}

TEST_CASE("rt core: snapshot validation rejects inconsistent views", "[pool_runtime]")
{
    pool_runtime_snapshot_init(&g_snap);
    TEST_ASSERT_EQUAL(RUNTIME_UNINITIALIZED, g_snap.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, g_snap.protocol);
    TEST_ASSERT_TRUE(pool_runtime_snapshot_valid(&g_snap));

    TEST_ASSERT_FALSE(pool_runtime_snapshot_valid(NULL));

    g_snap.model_version = 99u;
    TEST_ASSERT_FALSE(pool_runtime_snapshot_valid(&g_snap));
    g_snap.model_version = POOL_RUNTIME_MODEL_VERSION;

    /* A permission that does not match the state is rejected. */
    g_snap.protocol = POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE;
    TEST_ASSERT_FALSE(pool_runtime_snapshot_valid(&g_snap));
    g_snap.protocol = POOL_RUNTIME_PROTOCOL_HOLD;

    /* The Gate B6 prohibitions are structural. */
    g_snap.target_mining_authorized = true;
    TEST_ASSERT_FALSE(pool_runtime_snapshot_valid(&g_snap));
    g_snap.target_mining_authorized = false;
    g_snap.pool_mutation_permitted = true;
    TEST_ASSERT_FALSE(pool_runtime_snapshot_valid(&g_snap));
    g_snap.pool_mutation_permitted = false;

    g_snap.state = (PoolRuntimeState)99;
    TEST_ASSERT_FALSE(pool_runtime_snapshot_valid(&g_snap));

    TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT,
                      pool_runtime_snapshot_build(NULL, NULL, POOL_RESET_CLASS_POWER_ON,
                                                  0u, 0u, 0u, 0u, false, &g_snap));
}

/* ================================================================= */
/* Events and the bounded wait                                        */
/* ================================================================= */

TEST_CASE("rt core: unknown event bits are dropped and change nothing", "[pool_runtime]")
{
    PoolRuntimeControl      c;
    PoolRuntimeEventOutcome o;

    pool_runtime_control_init(&c, 600u);
    c.state = RUNTIME_WAITING_FOR_TRUSTED_TIME;

    TEST_ASSERT_EQUAL(RUNTIME_EVENT__ALL_VALID, pool_runtime_event_sanitize(UINT32_MAX));
    TEST_ASSERT_EQUAL(0u, pool_runtime_event_sanitize(0xFFFFFF00u));
    TEST_ASSERT_TRUE(pool_runtime_event_is_known(RUNTIME_EVENT_NETWORK_READY));
    TEST_ASSERT_FALSE(pool_runtime_event_is_known(0u));
    TEST_ASSERT_FALSE(pool_runtime_event_is_known(1u << 31));
    TEST_ASSERT_FALSE(pool_runtime_event_is_known(RUNTIME_EVENT_NETWORK_READY |
                                                  RUNTIME_EVENT_TIME_SYNC_CHANGED));

    TEST_ASSERT_EQUAL(RUNTIME_ERR_UNSUPPORTED_EVENT,
                      pool_runtime_control_apply(&c, 0xFFFFFF00u, &o));
    TEST_ASSERT_EQUAL(0u, o.applied_events);
    TEST_ASSERT_EQUAL(0xFFFFFF00u, o.ignored_events);
    TEST_ASSERT_FALSE(o.start_time_provider);
    TEST_ASSERT_FALSE(o.reevaluate_plan);
    TEST_ASSERT_FALSE(o.stop_task);
    TEST_ASSERT_FALSE(o.state_changed);
    TEST_ASSERT_EQUAL(RUNTIME_WAITING_FOR_TRUSTED_TIME, c.state);
    TEST_ASSERT_EQUAL(RUNTIME_ERR_INVALID_ARGUMENT, pool_runtime_control_apply(NULL, 0u, &o));
}

TEST_CASE("rt core: duplicate events are idempotent", "[pool_runtime]")
{
    PoolRuntimeControl      c;
    PoolRuntimeEventOutcome o;

    pool_runtime_control_init(&c, 600u);
    c.state = RUNTIME_WAITING_FOR_TRUSTED_TIME;

    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_runtime_control_apply(&c, RUNTIME_EVENT_NETWORK_READY, &o));
    TEST_ASSERT_TRUE(o.start_time_provider);
    TEST_ASSERT_TRUE(c.network_ready);

    /* A repeat must not re-request the provider start. */
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_runtime_control_apply(&c, RUNTIME_EVENT_NETWORK_READY, &o));
    TEST_ASSERT_FALSE(o.start_time_provider);
    TEST_ASSERT_EQUAL(0u, o.applied_events);

    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_runtime_control_apply(&c, RUNTIME_EVENT_BOOTSTRAP_COMPLETE, &o));
    TEST_ASSERT_TRUE(c.bootstrap_complete);
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_runtime_control_apply(&c, RUNTIME_EVENT_BOOTSTRAP_COMPLETE, &o));
    TEST_ASSERT_EQUAL(0u, o.applied_events);
}

TEST_CASE("rt core: network-ready only starts a provider while waiting for time",
          "[pool_runtime]")
{
    int i;
    for (i = 0; i < (int)POOL_RUNTIME_STATE__COUNT; i++) {
        PoolRuntimeControl      c;
        PoolRuntimeEventOutcome o;
        pool_runtime_control_init(&c, 600u);
        c.state = (PoolRuntimeState)i;
        TEST_ASSERT_EQUAL(RUNTIME_OK,
                          pool_runtime_control_apply(&c, RUNTIME_EVENT_NETWORK_READY, &o));
        TEST_ASSERT_EQUAL(c.state == RUNTIME_WAITING_FOR_TRUSTED_TIME, o.start_time_provider);
        /* No event ever moves the runtime state on its own. */
        TEST_ASSERT_EQUAL((PoolRuntimeState)i, c.state);
        TEST_ASSERT_FALSE(o.state_changed);
    }
}

TEST_CASE("rt core: shutdown stops the task without releasing anything", "[pool_runtime]")
{
    PoolRuntimeControl      c;
    PoolRuntimeEventOutcome o;

    pool_runtime_control_init(&c, 600u);
    c.state = RUNTIME_RESTORE_SOURCE_PENDING;

    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_runtime_control_apply(&c, RUNTIME_EVENT_SHUTDOWN_FOR_TEST, &o));
    TEST_ASSERT_TRUE(o.stop_task);
    TEST_ASSERT_TRUE(c.shutdown_requested);
    /* The held posture survives the shutdown: protocol stays HELD. */
    TEST_ASSERT_EQUAL(RUNTIME_RESTORE_SOURCE_PENDING, c.state);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_HOLD, pool_runtime_protocol_for_state(c.state));
}

TEST_CASE("rt core: the trusted-time wait is bounded and saturates", "[pool_runtime]")
{
    PoolRuntimeControl c;

    TEST_ASSERT_FALSE(pool_runtime_wait_expired(0u, 600u));
    TEST_ASSERT_TRUE(pool_runtime_wait_expired(600u, 600u));
    TEST_ASSERT_TRUE(pool_runtime_wait_expired(0u, 0u)); /* a zero window is over */

    pool_runtime_control_init(&c, 10u);
    TEST_ASSERT_FALSE(pool_runtime_control_advance_wait(&c, 5u));
    TEST_ASSERT_EQUAL(5u, c.wait_elapsed_s);
    TEST_ASSERT_TRUE(pool_runtime_control_advance_wait(&c, 5u));
    TEST_ASSERT_EQUAL(10u, c.wait_elapsed_s);
    /* Saturating: never wraps, never exceeds the limit. */
    TEST_ASSERT_TRUE(pool_runtime_control_advance_wait(&c, UINT32_MAX));
    TEST_ASSERT_EQUAL(10u, c.wait_elapsed_s);
    TEST_ASSERT_TRUE(pool_runtime_control_advance_wait(NULL, 1u)); /* fail safe */
}

TEST_CASE("rt core: store-reload requests a reload and never a mutation", "[pool_runtime]")
{
    PoolRuntimeControl      c;
    PoolRuntimeEventOutcome o;

    pool_runtime_control_init(&c, 600u);
    c.state = RUNTIME_RECOVERY_GUARD;
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_runtime_control_apply(&c, RUNTIME_EVENT_STORE_RELOAD_REQUIRED, &o));
    TEST_ASSERT_TRUE(o.reload_store);
    TEST_ASSERT_FALSE(o.start_time_provider);
    TEST_ASSERT_EQUAL(RUNTIME_RECOVERY_GUARD, c.state);
}

/* ================================================================= */
/* Tokens                                                             */
/* ================================================================= */

TEST_CASE("rt core: machine tokens are stable, non-empty and dot-free", "[pool_runtime]")
{
    int i;
    for (i = 0; i <= (int)POOL_RUNTIME_STATE__COUNT; i++) {
        const char *t = pool_runtime_state_str((PoolRuntimeState)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_TRUE(t[0] != '\0');
        TEST_ASSERT_NULL(strchr(t, '.'));
    }
    for (i = 0; i <= (int)POOL_RUNTIME_STATUS__COUNT; i++) {
        const char *t = pool_runtime_status_str((PoolRuntimeStatus)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_TRUE(t[0] != '\0');
        TEST_ASSERT_NULL(strchr(t, '.'));
    }
    for (i = 0; i <= (int)POOL_RUNTIME_PROTOCOL__COUNT; i++) {
        const char *t = pool_runtime_protocol_str((PoolRuntimeProtocolPermission)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_NULL(strchr(t, '.'));
    }
    TEST_ASSERT_EQUAL_STRING("runtime_invalid",
                             pool_runtime_state_str((PoolRuntimeState)123));
    TEST_ASSERT_EQUAL_STRING("network_ready",
                             pool_runtime_event_str(RUNTIME_EVENT_NETWORK_READY));
    TEST_ASSERT_EQUAL_STRING("unknown", pool_runtime_event_str(1u << 30));
}
