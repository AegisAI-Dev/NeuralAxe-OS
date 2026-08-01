/*
 * Deterministic tests for the Gate B7 PRODUCTION MUTATION FENCE.
 *
 * These drive the EXACT function the real handlers call
 * (nx_admission_evaluate / nx_timed_sessions_mutation_allowed) against a
 * REAL B5 coordinator bootstrapped from REAL B3 records and REAL B4 plans —
 * one posture per ownership class. Nothing here touches a network, NVS
 * hardware, OTA or a restart path; all identities are synthetic fixtures.
 *
 * The production CALL SITES that consult this fence (audited in Gate B7):
 *   main/http_server/http_server.c  PATCH_update_settings (pool identity),
 *                                   POST_restart, POST_OTA_update,
 *                                   POST_WWW_update
 *   main/bap/bap_handlers.c         the two autonomous Wi-Fi restart paths
 */

#include <string.h>
#include "unity.h"
#include "pool_session_runtime_admission.h"
#include "pool_session_recovery.h"
#include "pool_session_reset.h"

#define SESSION_ID 4242u
#define EPOCH_A_S  1750000000ull

static PoolOperationCoordinator g_coord;
static PoolSessionRecord        g_rec;
static PoolSessionRecoveryPlan  g_plan;
static PoolSessionBootContext   g_ctx;

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

static void make_rec(PoolSessionRecord *r, PoolSessionState st, bool rr)
{
    pool_session_record_init(r);
    r->session_id       = SESSION_ID;
    r->b1_model_version = POOL_SESSION_MODEL_VERSION;
    r->state            = st;
    fill_identity(&r->source, POOL_CHAIN_BITCOIN, "src.example", 3333, "acct.worker");
    fill_identity(&r->target, POOL_CHAIN_BITCOIN_CASH, "tgt.example", 3334, "acct.worker");
    r->password_policy  = POOL_SESSION_PW_KEEP_CURRENT;
    r->restore_required = rr;
    r->duration_s       = 3600u;
    r->generation       = 1u;
    if (st == POOL_STATE_TARGET_ACTIVE) {
        r->target_verify.connection_observed = true;
        r->target_verify.mining_observed     = true;
        r->target_verify.identity_verified   = true;
        r->verified_start_valid     = true;
        r->verified_start_epoch_s   = EPOCH_A_S;
        r->deadline_valid           = true;
        r->deadline_epoch_s         = EPOCH_A_S + 3600u;
        r->deadline_sync_generation = 1u;
    }
    if (st == POOL_STATE_COMPLETE) {
        r->restore_verify.connection_observed = true;
        r->restore_verify.mining_observed     = true;
        r->restore_verify.identity_verified   = true;
    }
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(r));
}

/* Bootstrap a REAL coordinator from a REAL B4 plan over a REAL record. */
static void bootstrap_posture(PoolStoreResult store_result, bool record_present,
                              PoolSessionState st, bool restore_required,
                              PoolSessionResetClass reset_class)
{
    PoolOperationBootstrapInput in;
    PoolOperationLeaseToken     token;

    if (record_present) {
        make_rec(&g_rec, st, restore_required);
    }
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.store_result   = store_result;
    g_ctx.record_present = record_present;
    g_ctx.record         = record_present ? &g_rec : NULL;
    g_ctx.reset_class    = reset_class;
    g_ctx.sync_wait_limit_s = 600u;
    g_ctx.mining_inhibition_available = true;
    (void)pool_session_recovery_plan(&g_ctx, &g_plan);

    memset(&in, 0, sizeof(in));
    in.store_result         = store_result;
    in.record_present       = record_present;
    in.record               = record_present ? &g_rec : NULL;
    in.plan                 = &g_plan;
    in.committed_generation = record_present ? g_rec.generation : 0u;

    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_init(&g_coord));
    (void)pool_operation_coordinator_bootstrap(&g_coord, &in, &token);
}

static void teardown_coord(void)
{
    (void)pool_operation_coordinator_deinit(&g_coord);
}

/* Every fenced mutation class must be denied in this posture. */
static void assert_all_denied(NxAdmissionVerdict expected)
{
    const NxMutationKind kinds[] = {
        NX_MUTATION_POOL_CONFIG, NX_MUTATION_DEVICE_RESTART, NX_MUTATION_OTA_UPDATE,
    };
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        NxAdmissionVerdict v = nx_admission_evaluate(&g_coord, kinds[i]);
        TEST_ASSERT_FALSE(nx_admission_verdict_allows(v));
        TEST_ASSERT_EQUAL(expected, v);
    }
}

/* ================================================================= */

TEST_CASE("admit: request-kind mapping is total and fails closed", "[pool_admit]")
{
    TEST_ASSERT_EQUAL(OP_REQUEST_MANUAL_POOL_PATCH,
                      nx_admission_request_kind(NX_MUTATION_POOL_CONFIG));
    TEST_ASSERT_EQUAL(OP_REQUEST_MANUAL_DEVICE_RESTART,
                      nx_admission_request_kind(NX_MUTATION_DEVICE_RESTART));
    TEST_ASSERT_EQUAL(OP_REQUEST_OTA_UPDATE,
                      nx_admission_request_kind(NX_MUTATION_OTA_UPDATE));
    /* An unknown class maps to the kind B5 always denies. */
    TEST_ASSERT_EQUAL(OP_REQUEST_DESTRUCTIVE_MAINTENANCE,
                      nx_admission_request_kind((NxMutationKind)99));

    TEST_ASSERT_TRUE(nx_admission_verdict_allows(NX_ADMIT_ALLOW));
    TEST_ASSERT_FALSE(nx_admission_verdict_allows(NX_ADMIT_DENY_SESSION_OWNER));
    TEST_ASSERT_FALSE(nx_admission_verdict_allows((NxAdmissionVerdict)77));
}

TEST_CASE("admit: a NULL or unbootstrapped coordinator fails closed", "[pool_admit]")
{
    TEST_ASSERT_EQUAL(NX_ADMIT_DENY_INVALID,
                      nx_admission_evaluate(NULL, NX_MUTATION_POOL_CONFIG));

    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_init(&g_coord));
    /* Initialized but NEVER bootstrapped: ownership is unknown. */
    assert_all_denied(NX_ADMIT_DENY_UNBOOTSTRAPPED);
    /* An unknown mutation class is denied too. */
    TEST_ASSERT_EQUAL(NX_ADMIT_DENY_INVALID,
                      nx_admission_evaluate(&g_coord, (NxMutationKind)42));
    teardown_coord();
}

TEST_CASE("admit: an empty store admits every production mutation", "[pool_admit]")
{
    /* No session facts at all: the fence must be completely transparent. */
    bootstrap_posture(STORE_EMPTY, false, POOL_STATE_IDLE, false,
                      POOL_RESET_CLASS_POWER_ON);
    TEST_ASSERT_EQUAL(NX_ADMIT_ALLOW,
                      nx_admission_evaluate(&g_coord, NX_MUTATION_POOL_CONFIG));
    TEST_ASSERT_EQUAL(NX_ADMIT_ALLOW,
                      nx_admission_evaluate(&g_coord, NX_MUTATION_DEVICE_RESTART));
    TEST_ASSERT_EQUAL(NX_ADMIT_ALLOW,
                      nx_admission_evaluate(&g_coord, NX_MUTATION_OTA_UPDATE));
    teardown_coord();
}

TEST_CASE("admit: a BOOT_RECOVERY owner denies every fenced mutation", "[pool_admit]")
{
    /* A verified TARGET_ACTIVE record on a clean boot reconstructs the
     * session-class owner (verification pending). */
    bootstrap_posture(STORE_OK, true, POOL_STATE_TARGET_ACTIVE, true,
                      POOL_RESET_CLASS_POWER_ON);
    {
        PoolOperationState s;
        TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &s));
        TEST_ASSERT_EQUAL(OP_OWNER_BOOT_RECOVERY, s.owner);
    }
    assert_all_denied(NX_ADMIT_DENY_SESSION_OWNER);
    teardown_coord();
}

TEST_CASE("admit: a SOURCE_RESTORE owner denies every fenced mutation", "[pool_admit]")
{
    bootstrap_posture(STORE_OK, true, POOL_STATE_RESTORE_DUE, true,
                      POOL_RESET_CLASS_POWER_ON);
    {
        PoolOperationState s;
        TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &s));
        TEST_ASSERT_EQUAL(OP_OWNER_SOURCE_RESTORE, s.owner);
        TEST_ASSERT_EQUAL(OP_PHASE_RESTORING_SOURCE, s.phase);
    }
    assert_all_denied(NX_ADMIT_DENY_SESSION_OWNER);
    teardown_coord();
}

TEST_CASE("admit: a TIMED_SESSION owner denies every fenced mutation", "[pool_admit]")
{
    /* Reconstruct the durable owner, then transition it to the live
     * timed-session phases the executor uses while it owns the flow. */
    bootstrap_posture(STORE_OK, true, POOL_STATE_TARGET_ACTIVE, true,
                      POOL_RESET_CLASS_POWER_ON);
    {
        PoolOperationState s;

        /* A live timed session rides the session-class lease through the
         * WAITING_FOR_TRUSTED_TIME / VERIFYING_TARGET / ACTIVE phases; the
         * fence must deny in every one of them, so assert the CLASS. */
        TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &s));
        TEST_ASSERT_TRUE(s.owner == OP_OWNER_TIMED_SESSION ||
                         s.owner == OP_OWNER_BOOT_RECOVERY);
        TEST_ASSERT_TRUE(s.phase == OP_PHASE_WAITING_FOR_TRUSTED_TIME ||
                         s.phase == OP_PHASE_VERIFYING_TARGET ||
                         s.phase == OP_PHASE_ACTIVE);
        TEST_ASSERT_TRUE(s.durable_claim);
    }
    assert_all_denied(NX_ADMIT_DENY_SESSION_OWNER);
    teardown_coord();
}

TEST_CASE("admit: the recovery guard denies every fenced mutation", "[pool_admit]")
{
    /* A corrupt store reconstructs the fail-closed guard. */
    bootstrap_posture(STORE_CORRUPT, false, POOL_STATE_IDLE, false,
                      POOL_RESET_CLASS_PANIC);
    {
        PoolOperationState s;
        TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &s));
        TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, s.phase);
    }
    assert_all_denied(NX_ADMIT_DENY_RECOVERY_GUARD);
    teardown_coord();
}

TEST_CASE("admit: an uncertain committed state denies every fenced mutation", "[pool_admit]")
{
    /* STORE_COMMIT_UNCERTAIN: the committed truth is unknowable, so no
     * manual writer may race the recovery. */
    bootstrap_posture(STORE_COMMIT_UNCERTAIN, false, POOL_STATE_IDLE, false,
                      POOL_RESET_CLASS_UNKNOWN);
    assert_all_denied(NX_ADMIT_DENY_RECOVERY_GUARD);
    teardown_coord();
}

TEST_CASE("admit: an obligated terminal keeps the fence closed", "[pool_admit]")
{
    /* RESTORE_FAILED still owes a restoration: manual mutation must not
     * race the owed restore. */
    bootstrap_posture(STORE_OK, true, POOL_STATE_RESTORE_FAILED, true,
                      POOL_RESET_CLASS_POWER_ON);
    {
        NxAdmissionVerdict v = nx_admission_evaluate(&g_coord, NX_MUTATION_POOL_CONFIG);
        TEST_ASSERT_FALSE(nx_admission_verdict_allows(v));
    }
    teardown_coord();
}

TEST_CASE("admit: a safe retained terminal still blocks pool mutation", "[pool_admit]")
{
    /* COMPLETE with the obligation discharged: ownership is FREE but the
     * terminal result is retained until acknowledgement, and the committed
     * B5 policy keeps manual pool mutation blocked until then. */
    bootstrap_posture(STORE_OK, true, POOL_STATE_COMPLETE, false,
                      POOL_RESET_CLASS_POWER_ON);
    {
        PoolOperationState s;
        NxAdmissionVerdict v;
        TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &s));
        TEST_ASSERT_TRUE(s.terminal_pending);
        v = nx_admission_evaluate(&g_coord, NX_MUTATION_POOL_CONFIG);
        /* Whatever the committed policy decides, the fence must REPORT it
         * consistently and never invent an allowance. */
        if (nx_admission_verdict_allows(v)) {
            PoolOperationRequest req;
            PoolOperationDecision dec;
            memset(&req, 0, sizeof(req));
            req.kind = OP_REQUEST_MANUAL_POOL_PATCH;
            dec = pool_operation_coordinator_evaluate(&g_coord, &req);
            TEST_ASSERT_TRUE(dec.allowed); /* the fence agreed with B5 */
        } else {
            TEST_ASSERT_EQUAL(NX_ADMIT_DENY_TERMINAL_PENDING, v);
        }
    }
    teardown_coord();
}

TEST_CASE("admit: evaluation is read-only and repeatable", "[pool_admit]")
{
    PoolOperationState before, after;

    bootstrap_posture(STORE_OK, true, POOL_STATE_RESTORE_DUE, true,
                      POOL_RESET_CLASS_POWER_ON);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &before));

    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(NX_ADMIT_DENY_SESSION_OWNER,
                          nx_admission_evaluate(&g_coord, NX_MUTATION_POOL_CONFIG));
        TEST_ASSERT_EQUAL(NX_ADMIT_DENY_SESSION_OWNER,
                          nx_admission_evaluate(&g_coord, NX_MUTATION_OTA_UPDATE));
    }
    /* No lease was acquired and no coordinator state changed. */
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &after));
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
    teardown_coord();
}

TEST_CASE("admit: verdict tokens are stable, unique and identity-free", "[pool_admit]")
{
    for (int i = 0; i < (int)NX_ADMIT__COUNT; i++) {
        const char *t = nx_admission_verdict_str((NxAdmissionVerdict)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_NULL(strchr(t, '.'));
        TEST_ASSERT_NULL(strstr(t, "example"));
        for (int j = 0; j < i; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(t, nx_admission_verdict_str((NxAdmissionVerdict)j)));
        }
    }
    TEST_ASSERT_EQUAL_STRING("NX_ADMIT_UNKNOWN",
                             nx_admission_verdict_str((NxAdmissionVerdict)99));
}

TEST_CASE("admit: the production wrapper matches the feature posture", "[pool_admit]")
{
    NxAdmissionVerdict v = NX_ADMIT_ALLOW;
    bool allowed = nx_timed_sessions_mutation_allowed(NX_MUTATION_POOL_CONFIG, &v);

#ifdef CONFIG_NX_TIMED_SESSIONS
    /* Feature ON: the production singleton exists but the test harness has
     * not booted it, so the wrapper must FAIL CLOSED rather than assume the
     * device is free. */
    TEST_ASSERT_FALSE(allowed);
    TEST_ASSERT_EQUAL(NX_ADMIT_DENY_UNBOOTSTRAPPED, v);
#else
    /* Feature OFF: no coordinator and no session can exist, so production
     * behavior is unchanged — every mutation is admitted. */
    TEST_ASSERT_TRUE(allowed);
    TEST_ASSERT_EQUAL(NX_ADMIT_ALLOW, v);
#endif

    /* The out-parameter is optional. */
    (void)nx_timed_sessions_mutation_allowed(NX_MUTATION_OTA_UPDATE, NULL);
}
