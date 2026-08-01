/*
 * Exhaustive deterministic tests for the pure ownership policy (Gate B5).
 *
 * No hardware, no networking, no NVS, no locks in this file — everything is
 * a synthetic fixture. B4 plans are produced by the REAL committed B4
 * engine wherever possible so the bootstrap consumes genuine plans.
 */

#include <string.h>
#include "unity.h"
#include "pool_operation_policy.h"

#define EPOCH_A_S    1750000000ull
#define WAIT_LIMIT_S 600u
#define SESSION_ID   42u

/* File-static fixtures (records ~1 KB; keep off the task stack). */
static PoolSessionRecord g_rec;
static PoolSessionRecoveryPlan g_plan;
static PoolSessionBootContext g_bctx;
static PoolOperationState g_st;
static PoolOperationState g_st2;
static PoolOperationBootstrapInput g_bi;
static PoolOperationLeaseToken g_tok;
static PoolOperationLeaseToken g_tok2;
static PoolOperationDecision g_dec;

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

static void make_active_rec(PoolSessionRecord *r)
{
    pool_session_record_init(r);
    r->generation = 3u;
    r->session_id = SESSION_ID;
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
}

static void make_state_rec(PoolSessionRecord *r, PoolSessionState st, bool rr)
{
    make_active_rec(r);
    r->state = st;
    r->restore_required = rr;
    if (st == POOL_STATE_COMPLETE) {
        r->restore_verify.connection_observed = true;
        r->restore_verify.mining_observed = true;
        r->restore_verify.identity_verified = true;
    }
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(r));
}

/* Produce a REAL B4 plan for the record via the committed engine. */
static void make_plan(PoolStoreResult res, const PoolSessionRecord *rec, bool trusted)
{
    memset(&g_bctx, 0, sizeof(g_bctx));
    g_bctx.store_result = res;
    g_bctx.record_present = (rec != NULL);
    g_bctx.record = rec;
    g_bctx.reset_class = POOL_RESET_CLASS_POWER_ON;
    g_bctx.sync_wait_elapsed_s = 0u;
    g_bctx.sync_wait_limit_s = WAIT_LIMIT_S;
    g_bctx.time_provider_initialized = true;
    g_bctx.mining_inhibition_available = true;
    g_bctx.time_snapshot.status = TIME_ERR_NOT_SYNCED;
    if (trusted && rec != NULL) {
        g_bctx.time_snapshot.trusted = true;
        g_bctx.time_snapshot.status = TIME_OK;
        g_bctx.time_snapshot.trusted_epoch_s = rec->deadline_epoch_s - 1000u;
        g_bctx.time_snapshot.sync_generation = 1u;
    }
    (void)pool_session_recovery_plan(&g_bctx, &g_plan);
}

static void make_bi(PoolStoreResult res, const PoolSessionRecord *rec)
{
    memset(&g_bi, 0, sizeof(g_bi));
    g_bi.store_result = res;
    g_bi.record_present = (rec != NULL);
    g_bi.record = rec;
    g_bi.plan = &g_plan;
    g_bi.committed_generation = (rec != NULL) ? rec->generation : 0u;
}

/* Bootstrap g_st from (result, record, trusted) through the real B4 plan. */
static PoolOperationStatus boot(PoolStoreResult res, const PoolSessionRecord *rec,
                                bool trusted)
{
    make_plan(res, rec, trusted);
    make_bi(res, rec);
    pool_operation_state_init(&g_st);
    return pool_operation_bootstrap(&g_st, &g_bi, &g_tok);
}

static PoolOperationRequest req_of(PoolOperationRequestKind k, uint32_t sid)
{
    PoolOperationRequest r;
    memset(&r, 0, sizeof(r));
    r.kind = k;
    r.session_id = sid;
    return r;
}

static void assert_clean_token_str(const char *tok)
{
    const char *c;
    TEST_ASSERT_NOT_NULL(tok);
    TEST_ASSERT_TRUE(strlen(tok) > 0u && strlen(tok) < 48u);
    for (c = tok; *c != '\0'; c++) {
        TEST_ASSERT_TRUE((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_');
    }
}

/* ================================================================= */
/* A. Model tests                                                     */
/* ================================================================= */

TEST_CASE("op-model: state init is fail-closed", "[pool_op]")
{
    memset(&g_st, 0xAA, sizeof(g_st));
    pool_operation_state_init(&g_st);
    TEST_ASSERT_FALSE(g_st.bootstrapped);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_UNBOOTSTRAPPED, g_st.phase);
    TEST_ASSERT_EQUAL_UINT32(0u, g_st.lease_generation);
    TEST_ASSERT_FALSE(g_st.terminal_pending);
    TEST_ASSERT_FALSE(g_st.restore_required);
}

TEST_CASE("op-model: request properties are total and exact", "[pool_op]")
{
    int k;
    TEST_ASSERT_TRUE(pool_operation_request_is_read_only(OP_REQUEST_READ_ONLY));
    TEST_ASSERT_FALSE(pool_operation_request_is_mutating(OP_REQUEST_READ_ONLY));
    for (k = 1; k < (int)OP_REQUEST__COUNT; k++) {
        TEST_ASSERT_TRUE(pool_operation_request_is_mutating((PoolOperationRequestKind)k));
        TEST_ASSERT_FALSE(pool_operation_request_is_read_only((PoolOperationRequestKind)k));
    }
    /* unknown kinds fail closed as mutating */
    TEST_ASSERT_TRUE(pool_operation_request_is_mutating((PoolOperationRequestKind)99));
    /* exclusive-lease set */
    TEST_ASSERT_TRUE(pool_operation_request_requires_exclusive_lease(OP_REQUEST_TIMED_SESSION_START));
    TEST_ASSERT_TRUE(pool_operation_request_requires_exclusive_lease(OP_REQUEST_MANUAL_POOL_PATCH));
    TEST_ASSERT_TRUE(pool_operation_request_requires_exclusive_lease(OP_REQUEST_OTA_UPDATE));
    TEST_ASSERT_FALSE(pool_operation_request_requires_exclusive_lease(OP_REQUEST_RESTORE_NOW));
    TEST_ASSERT_FALSE(pool_operation_request_requires_exclusive_lease(OP_REQUEST_TIMED_SESSION_INTERNAL));
    /* session context + terminal record */
    TEST_ASSERT_TRUE(pool_operation_request_requires_session_context(OP_REQUEST_RESTORE_NOW));
    TEST_ASSERT_TRUE(pool_operation_request_requires_terminal_record(OP_REQUEST_SESSION_ACKNOWLEDGE));
    /* invariant 17 set */
    TEST_ASSERT_TRUE(pool_operation_request_safe_with_terminal(OP_REQUEST_MANUAL_POOL_PATCH));
    TEST_ASSERT_TRUE(pool_operation_request_safe_with_terminal(OP_REQUEST_OTA_UPDATE));
    TEST_ASSERT_FALSE(pool_operation_request_safe_with_terminal(OP_REQUEST_TIMED_SESSION_START));
}

TEST_CASE("op-model: request scopes match the audited call sites", "[pool_op]")
{
    uint32_t sess = OP_SCOPE_SESSION_STORE | OP_SCOPE_POOL_CONFIGURATION |
                    OP_SCOPE_STRATUM_CONTROL | OP_SCOPE_DEVICE_RESTART;
    TEST_ASSERT_EQUAL_UINT32(sess, pool_operation_request_scope(OP_REQUEST_TIMED_SESSION_START));
    TEST_ASSERT_EQUAL_UINT32(sess, pool_operation_request_scope(OP_REQUEST_RESTORE_NOW));
    TEST_ASSERT_EQUAL_UINT32(OP_SCOPE_POOL_CONFIGURATION | OP_SCOPE_STRATUM_CONTROL |
                                 OP_SCOPE_DEVICE_RESTART,
                             pool_operation_request_scope(OP_REQUEST_MANUAL_POOL_PATCH));
    TEST_ASSERT_EQUAL_UINT32(OP_SCOPE_OTA_FLASH | OP_SCOPE_DEVICE_RESTART,
                             pool_operation_request_scope(OP_REQUEST_OTA_UPDATE));
    TEST_ASSERT_EQUAL_UINT32(OP_SCOPE_SESSION_STORE,
                             pool_operation_request_scope(OP_REQUEST_SESSION_ACKNOWLEDGE));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_operation_request_scope((PoolOperationRequestKind)99));
    /* every returned mask stays within the valid bits */
    {
        int k;
        for (k = 0; k < (int)OP_REQUEST__COUNT; k++) {
            TEST_ASSERT_EQUAL_UINT32(0u,
                pool_operation_request_scope((PoolOperationRequestKind)k) & ~OP_SCOPE__ALL_VALID);
        }
    }
}

TEST_CASE("op-model: machine tokens are clean and total", "[pool_op]")
{
    int i;
    for (i = 0; i < (int)POOL_OPERATION_STATUS__COUNT; i++) {
        assert_clean_token_str(pool_operation_status_str((PoolOperationStatus)i));
    }
    for (i = 0; i < (int)OP_OWNER__COUNT; i++) {
        assert_clean_token_str(pool_operation_owner_str((PoolOperationOwner)i));
    }
    for (i = 0; i < (int)OP_PHASE__COUNT; i++) {
        assert_clean_token_str(pool_operation_phase_str((PoolOperationLeasePhase)i));
    }
    for (i = 0; i < (int)OP_REQUEST__COUNT; i++) {
        assert_clean_token_str(pool_operation_request_str((PoolOperationRequestKind)i));
    }
    assert_clean_token_str(pool_operation_status_str((PoolOperationStatus)999));
    assert_clean_token_str(pool_operation_owner_str((PoolOperationOwner)999));
    assert_clean_token_str(pool_operation_phase_str((PoolOperationLeasePhase)999));
    assert_clean_token_str(pool_operation_request_str((PoolOperationRequestKind)999));
}

/* ================================================================= */
/* B. Bootstrap tests                                                 */
/* ================================================================= */

TEST_CASE("op-bootstrap: unbootstrapped denies every mutation, allows reads", "[pool_op]")
{
    int k;
    pool_operation_state_init(&g_st);
    for (k = 0; k < (int)OP_REQUEST__COUNT; k++) {
        PoolOperationRequest r = req_of((PoolOperationRequestKind)k, SESSION_ID);
        PoolOperationDecision d = pool_operation_evaluate(&g_st, &r);
        if (k == (int)OP_REQUEST_READ_ONLY) {
            TEST_ASSERT_TRUE(d.allowed);
        } else {
            TEST_ASSERT_FALSE(d.allowed);
            TEST_ASSERT_EQUAL(OP_ERR_BOOTSTRAP_REQUIRED, d.status);
        }
    }
}

TEST_CASE("op-bootstrap: EMPTY and CLEARED bootstrap FREE", "[pool_op]")
{
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    TEST_ASSERT_TRUE(g_st.bootstrapped);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_st.phase);
    TEST_ASSERT_FALSE(g_st.terminal_pending);
    TEST_ASSERT_FALSE(g_tok.valid);

    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_CLEARED, NULL, false));
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_st.phase);
}

TEST_CASE("op-bootstrap: terminal records retain, hold no execution lease", "[pool_op]")
{
    make_state_rec(&g_rec, POOL_STATE_COMPLETE, false);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_st.phase);
    TEST_ASSERT_TRUE(g_st.terminal_pending);
    TEST_ASSERT_FALSE(g_st.restore_required);
    TEST_ASSERT_FALSE(g_tok.valid);
    TEST_ASSERT_EQUAL_UINT32(SESSION_ID, g_st.bound_session_id);

    make_state_rec(&g_rec, POOL_STATE_CANCELLED, false);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    TEST_ASSERT_TRUE(g_st.terminal_pending);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_st.owner);
}

TEST_CASE("op-bootstrap: recovery plans reconstruct durable owners", "[pool_op]")
{
    /* pre-mutation snapshot-committed: reserved boot recovery */
    make_state_rec(&g_rec, POOL_STATE_TARGET_SNAPSHOT_COMMITTED, false);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    TEST_ASSERT_EQUAL(OP_OWNER_BOOT_RECOVERY, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_RESERVED_PENDING_PERSISTENCE, g_st.phase);
    TEST_ASSERT_TRUE(g_st.durable_claim);
    TEST_ASSERT_TRUE(g_st.persistence_required_before_action);
    TEST_ASSERT_TRUE(g_tok.valid);
    TEST_ASSERT_EQUAL_UINT32(1u, g_tok.lease_generation);

    /* untrusted TARGET_ACTIVE: waiting for trusted time */
    make_active_rec(&g_rec);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    TEST_ASSERT_EQUAL(OP_OWNER_BOOT_RECOVERY, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_WAITING_FOR_TRUSTED_TIME, g_st.phase);
    TEST_ASSERT_TRUE(g_st.restore_required);

    /* trusted TARGET_ACTIVE: verification eligibility — never mining */
    make_active_rec(&g_rec);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, true));
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_MINING_VERIFY_BEFORE_MINING, g_plan.mining_policy);
    TEST_ASSERT_EQUAL(OP_PHASE_VERIFYING_TARGET, g_st.phase);

    /* restore-obligated interruption: source-restore ownership */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    TEST_ASSERT_EQUAL(OP_OWNER_SOURCE_RESTORE, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_RESTORING_SOURCE, g_st.phase);
    TEST_ASSERT_TRUE(g_st.restore_required);
}

TEST_CASE("op-bootstrap: guards for every non-OK store result", "[pool_op]")
{
    static const PoolStoreResult bad[] = {
        STORE_NOT_INITIALIZED, STORE_INVALID_ARGUMENT, STORE_IO_ERROR, STORE_CORRUPT,
        STORE_UNSUPPORTED_SCHEMA, STORE_INVALID_RECORD, STORE_ACTIVE_POINTER_INVALID,
        STORE_ACTIVE_SLOT_INVALID, STORE_RECOVERY_REQUIRED, STORE_GENERATION_EXHAUSTED,
        STORE_READBACK_MISMATCH, STORE_COMMIT_UNCERTAIN, STORE_STATE_CONFLICT,
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL(OP_OK, boot(bad[i], NULL, false));
        TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_st.owner);
        TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_st.phase);
        TEST_ASSERT_TRUE(g_st.durable_claim);
    }
    /* RECOVERY_REQUIRED plan on a valid record also guards */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_FAILED, true);
    g_rec.recovery_attempt_count = POOL_RECORD_RECOVERY_ATTEMPT_MAX; /* exhausted */
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RECOVERY_REQUIRED, g_plan.decision);
    TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_st.owner);
}

TEST_CASE("op-bootstrap: inconsistent inputs fail closed", "[pool_op]")
{
    /* EMPTY with a record present */
    make_active_rec(&g_rec);
    make_plan(STORE_EMPTY, NULL, false);
    make_bi(STORE_EMPTY, &g_rec);
    pool_operation_state_init(&g_st);
    TEST_ASSERT_EQUAL(OP_ERR_INTERNAL_CONSISTENCY,
                      pool_operation_bootstrap(&g_st, &g_bi, &g_tok));
    TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_st.owner);

    /* STORE_OK without a record */
    make_plan(STORE_OK, NULL, false);
    make_bi(STORE_OK, NULL);
    pool_operation_state_init(&g_st);
    TEST_ASSERT_EQUAL(OP_ERR_INTERNAL_CONSISTENCY,
                      pool_operation_bootstrap(&g_st, &g_bi, &g_tok));
    TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_st.owner);

    /* null plan: remains unbootstrapped (fail closed) */
    pool_operation_state_init(&g_st);
    make_bi(STORE_EMPTY, NULL);
    g_bi.plan = NULL;
    TEST_ASSERT_EQUAL(OP_ERR_INVALID_ARGUMENT,
                      pool_operation_bootstrap(&g_st, &g_bi, &g_tok));
    TEST_ASSERT_FALSE(g_st.bootstrapped);

    /* double bootstrap is a deterministic error */
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    make_bi(STORE_EMPTY, NULL);
    TEST_ASSERT_EQUAL(OP_ERR_ALREADY_BOOTSTRAPPED,
                      pool_operation_bootstrap(&g_st, &g_bi, &g_tok2));

    /* idempotent: identical inputs from scratch yield identical states */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    make_plan(STORE_OK, &g_rec, false);
    make_bi(STORE_OK, &g_rec);
    pool_operation_state_init(&g_st);
    (void)pool_operation_bootstrap(&g_st, &g_bi, NULL);
    pool_operation_state_init(&g_st2);
    (void)pool_operation_bootstrap(&g_st2, &g_bi, NULL);
    TEST_ASSERT_EQUAL(0, memcmp(&g_st, &g_st2, sizeof(g_st)));
}

/* ================================================================= */
/* C. Conflict matrix tests                                           */
/* ================================================================= */

TEST_CASE("op-matrix: timed-session start only from clean FREE", "[pool_op]")
{
    PoolOperationRequest r = req_of(OP_REQUEST_TIMED_SESSION_START, 77u);
    PoolOperationDecision d;

    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    d = pool_operation_evaluate(&g_st, &r);
    TEST_ASSERT_TRUE(d.allowed);

    /* session id zero is invalid */
    r.session_id = 0u;
    d = pool_operation_evaluate(&g_st, &r);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_INVALID_ARGUMENT, d.status);
    r.session_id = 77u;

    /* a retained terminal result blocks a NEW session until ack */
    make_state_rec(&g_rec, POOL_STATE_COMPLETE, false);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    d = pool_operation_evaluate(&g_st, &r);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_TERMINAL_ACK_REQUIRED, d.status);
    TEST_ASSERT_TRUE(d.http_conflict);

    /* an active session-class owner blocks it */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    d = pool_operation_evaluate(&g_st, &r);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_BUSY, d.status);
    TEST_ASSERT_EQUAL(OP_OWNER_SOURCE_RESTORE, d.blocking_owner);
}

TEST_CASE("op-matrix: manual PATCH/OTA/restart denied under session owners", "[pool_op]")
{
    static const PoolOperationRequestKind kinds[] = {
        OP_REQUEST_MANUAL_POOL_PATCH, OP_REQUEST_OTA_UPDATE,
        OP_REQUEST_MANUAL_DEVICE_RESTART,
    };
    size_t i;

    /* durable restore ownership blocks all three */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        PoolOperationRequest r = req_of(kinds[i], 0u);
        PoolOperationDecision d = pool_operation_evaluate(&g_st, &r);
        TEST_ASSERT_FALSE(d.allowed);
        TEST_ASSERT_EQUAL(OP_ERR_BUSY, d.status);
        TEST_ASSERT_TRUE(d.http_conflict);
    }

    /* the waiting phase blocks them too (and protocol reconcile) */
    make_active_rec(&g_rec);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false)); /* WAITING */
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        PoolOperationRequest r = req_of(kinds[i], 0u);
        TEST_ASSERT_FALSE(pool_operation_evaluate(&g_st, &r).allowed);
    }
    {
        PoolOperationRequest r = req_of(OP_REQUEST_PROTOCOL_RECONCILE, 0u);
        PoolOperationDecision d = pool_operation_evaluate(&g_st, &r);
        TEST_ASSERT_FALSE(d.allowed); /* Stratum is held while waiting */
        TEST_ASSERT_EQUAL(OP_ERR_BUSY, d.status);
    }
}

TEST_CASE("op-matrix: retained safe terminal permits manual work", "[pool_op]")
{
    PoolOperationDecision d;
    /* invariant 17: COMPLETE retained, restore_required=false */
    make_state_rec(&g_rec, POOL_STATE_COMPLETE, false);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    TEST_ASSERT_TRUE(pool_operation_evaluate(&g_st,
        &(PoolOperationRequest){ .kind = OP_REQUEST_MANUAL_POOL_PATCH }).allowed);
    TEST_ASSERT_TRUE(pool_operation_evaluate(&g_st,
        &(PoolOperationRequest){ .kind = OP_REQUEST_OTA_UPDATE }).allowed);
    TEST_ASSERT_TRUE(pool_operation_evaluate(&g_st,
        &(PoolOperationRequest){ .kind = OP_REQUEST_MANUAL_DEVICE_RESTART }).allowed);
    /* protocol reconcile is Stratum-mutating owner work: with no lease
     * held, there is no owner to authorize it — denied, never a bypass */
    d = pool_operation_evaluate(&g_st,
        &(PoolOperationRequest){ .kind = OP_REQUEST_PROTOCOL_RECONCILE });
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_NOT_OWNER, d.status);
}

TEST_CASE("op-bootstrap: RECOVERY_REQUIRED is never an acknowledgeable terminal", "[pool_op]")
{
    PoolOperationDecision d;
    static const PoolOperationRequestKind blocked[] = {
        OP_REQUEST_TIMED_SESSION_START, OP_REQUEST_SESSION_ACKNOWLEDGE,
        OP_REQUEST_MANUAL_POOL_PATCH, OP_REQUEST_OTA_UPDATE,
        OP_REQUEST_MANUAL_DEVICE_RESTART, OP_REQUEST_PROTOCOL_RECONCILE,
    };
    size_t i;

    /* pre-mutation RECOVERY_REQUIRED (restore_required=false): the B4 plan
     * is RETAIN, but B5 must NOT treat it as an acknowledgeable terminal —
     * it reconstructs the guard and keeps every piece of evidence. */
    make_state_rec(&g_rec, POOL_STATE_RECOVERY_REQUIRED, false);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE, g_plan.decision);
    TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_st.phase);
    TEST_ASSERT_FALSE(g_st.terminal_pending); /* NOT terminal-pending */
    TEST_ASSERT_TRUE(g_st.durable_claim);     /* evidence retained */
    TEST_ASSERT_EQUAL_UINT32(SESSION_ID, g_st.bound_session_id);

    /* every normal mutation — including acknowledgement — is locked */
    for (i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        PoolOperationRequest r = req_of(blocked[i], SESSION_ID);
        d = pool_operation_evaluate(&g_st, &r);
        TEST_ASSERT_FALSE(d.allowed);
        TEST_ASSERT_EQUAL(OP_ERR_RECOVERY_LOCKED, d.status);
    }
    /* only the bounded operator-recovery admission exists; it clears or
     * tombstones nothing */
    d = pool_operation_evaluate(&g_st,
        &(PoolOperationRequest){ .kind = OP_REQUEST_OPERATOR_RECOVERY });
    TEST_ASSERT_TRUE(d.allowed);

    /* the obligated variant keeps its unchanged restore ownership */
    make_state_rec(&g_rec, POOL_STATE_RECOVERY_REQUIRED, true);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    TEST_ASSERT_EQUAL(OP_OWNER_SOURCE_RESTORE, g_st.owner);
    TEST_ASSERT_FALSE(g_st.terminal_pending);
}

TEST_CASE("op-matrix: protocol reconcile requires the owning token", "[pool_op]")
{
    PoolOperationRequest rec_req = req_of(OP_REQUEST_PROTOCOL_RECONCILE, 0u);
    PoolOperationDecision d;

    /* FREE: no owner exists to authorize Stratum mutation */
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    d = pool_operation_evaluate(&g_st, &rec_req);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_NOT_OWNER, d.status);

    /* source-restore owner: only the matching current token may reconcile */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false)); /* RESTORING */
    g_st2 = g_st;
    d = pool_operation_evaluate(&g_st, &rec_req); /* token missing */
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_INVALID_ARGUMENT, d.status);
    rec_req.token = g_tok;
    d = pool_operation_evaluate(&g_st, &rec_req); /* matching token */
    TEST_ASSERT_TRUE(d.allowed);
    rec_req.token.lease_generation += 1u; /* stale */
    d = pool_operation_evaluate(&g_st, &rec_req);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_STALE_LEASE, d.status);
    rec_req.token = g_tok;
    rec_req.token.owner = OP_OWNER_MANUAL_POOL_PATCH; /* wrong owner class */
    d = pool_operation_evaluate(&g_st, &rec_req);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_NOT_OWNER, d.status);
    /* failed decisions never mutated the state */
    TEST_ASSERT_EQUAL(0, memcmp(&g_st2, &g_st, sizeof(g_st)));

    /* boot-recovery owner in VERIFYING_TARGET: only the matching current
     * token may drive the internal verification reconcile */
    make_active_rec(&g_rec);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, true)); /* VERIFYING */
    TEST_ASSERT_EQUAL(OP_PHASE_VERIFYING_TARGET, g_st.phase);
    g_st2 = g_st;
    rec_req = req_of(OP_REQUEST_PROTOCOL_RECONCILE, 0u);
    d = pool_operation_evaluate(&g_st, &rec_req); /* token missing */
    TEST_ASSERT_FALSE(d.allowed);
    rec_req.token = g_tok;
    d = pool_operation_evaluate(&g_st, &rec_req); /* matching token */
    TEST_ASSERT_TRUE(d.allowed);
    /* the allowed decision created no second owner and mutated nothing */
    TEST_ASSERT_EQUAL(0, memcmp(&g_st2, &g_st, sizeof(g_st)));

    /* manual-PATCH owner: only its own token may drive its reconcile */
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    {
        PoolOperationRequest patch = req_of(OP_REQUEST_MANUAL_POOL_PATCH, 0u);
        TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &patch, &g_tok, &g_dec));
        rec_req = req_of(OP_REQUEST_PROTOCOL_RECONCILE, 0u);
        d = pool_operation_evaluate(&g_st, &rec_req); /* another caller, no token */
        TEST_ASSERT_FALSE(d.allowed); /* the manual owner cannot be bypassed */
        rec_req.token = g_tok;
        d = pool_operation_evaluate(&g_st, &rec_req); /* the owner itself */
        TEST_ASSERT_TRUE(d.allowed);
        TEST_ASSERT_EQUAL(OP_OK,
                          pool_operation_release_manual(&g_st, &g_tok,
                                                        OP_OUTCOME_NO_MUTATION_OCCURRED));
    }

    /* OTA owner: no STRATUM_CONTROL scope — reconcile denied even with the
     * matching token; OTA ownership cannot be bypassed either */
    {
        PoolOperationRequest ota = req_of(OP_REQUEST_OTA_UPDATE, 0u);
        TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &ota, &g_tok, &g_dec));
        rec_req = req_of(OP_REQUEST_PROTOCOL_RECONCILE, 0u);
        rec_req.token = g_tok;
        d = pool_operation_evaluate(&g_st, &rec_req);
        TEST_ASSERT_FALSE(d.allowed);
        TEST_ASSERT_EQUAL(OP_ERR_NOT_OWNER, d.status);
        rec_req.token.valid = false; /* and an outside caller is denied too */
        d = pool_operation_evaluate(&g_st, &rec_req);
        TEST_ASSERT_FALSE(d.allowed);
        TEST_ASSERT_EQUAL(OP_OK,
                          pool_operation_release_manual(&g_st, &g_tok,
                                                        OP_OUTCOME_NO_MUTATION_OCCURRED));
    }

    /* WAITING holds Stratum even against the owner's own token */
    make_active_rec(&g_rec);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false)); /* WAITING */
    rec_req = req_of(OP_REQUEST_PROTOCOL_RECONCILE, 0u);
    rec_req.token = g_tok;
    d = pool_operation_evaluate(&g_st, &rec_req);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_BUSY, d.status);

    /* the guard denies normal reconcile outright */
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_CORRUPT, NULL, false));
    d = pool_operation_evaluate(&g_st, &rec_req);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_RECOVERY_LOCKED, d.status);
}

TEST_CASE("op-matrix: the guard denies everything except operator recovery", "[pool_op]")
{
    int k;
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_CORRUPT, NULL, false)); /* guard */
    for (k = 1; k < (int)OP_REQUEST__COUNT; k++) {
        PoolOperationRequest r = req_of((PoolOperationRequestKind)k, SESSION_ID);
        PoolOperationDecision d = pool_operation_evaluate(&g_st, &r);
        if (k == (int)OP_REQUEST_OPERATOR_RECOVERY) {
            TEST_ASSERT_TRUE(d.allowed);
        } else {
            TEST_ASSERT_FALSE(d.allowed);
            TEST_ASSERT_EQUAL(OP_ERR_RECOVERY_LOCKED, d.status);
        }
    }
    /* the operator lease then locks everything (incl. a second operator) */
    {
        PoolOperationRequest r = req_of(OP_REQUEST_OPERATOR_RECOVERY, 0u);
        TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &r, &g_tok, &g_dec));
        TEST_ASSERT_EQUAL(OP_OWNER_OPERATOR_RECOVERY, g_st.owner);
        TEST_ASSERT_FALSE(pool_operation_evaluate(&g_st, &r).allowed);
        TEST_ASSERT_FALSE(pool_operation_evaluate(&g_st,
            &(PoolOperationRequest){ .kind = OP_REQUEST_MANUAL_POOL_PATCH }).allowed);
    }
}

TEST_CASE("op-matrix: internal session work demands the current token", "[pool_op]")
{
    PoolOperationRequest r = req_of(OP_REQUEST_TIMED_SESSION_INTERNAL, SESSION_ID);
    PoolOperationDecision d;

    /* no session at all */
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    d = pool_operation_evaluate(&g_st, &r);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_NO_ACTIVE_SESSION, d.status);

    /* durable session: the bootstrap token is the current one */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    r.token = g_tok;
    d = pool_operation_evaluate(&g_st, &r);
    TEST_ASSERT_TRUE(d.allowed);

    /* a stale generation is rejected */
    r.token.lease_generation = g_tok.lease_generation + 1u;
    d = pool_operation_evaluate(&g_st, &r);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_STALE_LEASE, d.status);

    /* a mismatched owner class is rejected */
    r.token = g_tok;
    r.token.owner = OP_OWNER_MANUAL_POOL_PATCH;
    d = pool_operation_evaluate(&g_st, &r);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_NOT_OWNER, d.status);

    /* generation zero is structurally invalid */
    r.token = g_tok;
    r.token.lease_generation = 0u;
    r.token.valid = true;
    d = pool_operation_evaluate(&g_st, &r);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_INVALID_ARGUMENT, d.status);
}

TEST_CASE("op-matrix: destructive maintenance and unknown kinds fail closed", "[pool_op]")
{
    PoolOperationDecision d;
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    d = pool_operation_evaluate(&g_st,
        &(PoolOperationRequest){ .kind = OP_REQUEST_DESTRUCTIVE_MAINTENANCE });
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_UNSUPPORTED_REQUEST, d.status);
    d = pool_operation_evaluate(&g_st,
        &(PoolOperationRequest){ .kind = (PoolOperationRequestKind)99 });
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_UNSUPPORTED_REQUEST, d.status);
    /* operator recovery without a guard has nothing to recover */
    d = pool_operation_evaluate(&g_st,
        &(PoolOperationRequest){ .kind = OP_REQUEST_OPERATOR_RECOVERY });
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_NO_ACTIVE_SESSION, d.status);
}

/* ================================================================= */
/* D. Acquisition + persistence barrier                               */
/* ================================================================= */

TEST_CASE("op-acquire: session start reserves and demands persistence", "[pool_op]")
{
    PoolOperationRequest r = req_of(OP_REQUEST_TIMED_SESSION_START, 77u);
    PoolOperationPersistenceProof proof;

    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &r, &g_tok, &g_dec));
    TEST_ASSERT_TRUE(g_tok.valid);
    TEST_ASSERT_EQUAL(OP_OWNER_TIMED_SESSION, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_RESERVED_PENDING_PERSISTENCE, g_st.phase);
    TEST_ASSERT_TRUE(g_st.persistence_required_before_action);
    TEST_ASSERT_EQUAL_UINT32(77u, g_st.bound_session_id);

    /* a second exclusive acquisition conflicts */
    TEST_ASSERT_EQUAL(OP_ERR_BUSY, pool_operation_acquire(&g_st, &r, &g_tok2, &g_dec));
    TEST_ASSERT_FALSE(g_tok2.valid);

    /* direct transition to ACTIVE is refused: persistence required */
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_REQUIRED,
                      pool_operation_transition_phase(&g_st, &g_tok, OP_PHASE_ACTIVE, &g_tok2));

    /* mismatched proofs are rejected and the lease stays reserved */
    memset(&proof, 0, sizeof(proof));
    proof.kind = OP_PROOF_SESSION_COMMITTED;
    proof.store_result = STORE_OK;
    proof.session_id = 999u; /* wrong session */
    proof.committed_record_generation = 5u;
    proof.persisted_state = POOL_STATE_TARGET_SNAPSHOT_COMMITTED;
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_MISMATCH,
                      pool_operation_apply_persistence_proof(&g_st, &g_tok, &proof, &g_tok2));
    TEST_ASSERT_EQUAL(OP_PHASE_RESERVED_PENDING_PERSISTENCE, g_st.phase);

    /* the matching committed proof activates and rotates the token */
    proof.session_id = 77u;
    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_apply_persistence_proof(&g_st, &g_tok, &proof, &g_tok2));
    TEST_ASSERT_EQUAL(OP_PHASE_ACTIVE, g_st.phase);
    TEST_ASSERT_TRUE(g_st.durable_claim);
    TEST_ASSERT_FALSE(g_st.persistence_required_before_action);
    TEST_ASSERT_TRUE(g_tok2.valid);
    /* the pre-activation token is now stale */
    TEST_ASSERT_EQUAL(OP_ERR_STALE_LEASE,
                      pool_operation_transition_phase(&g_st, &g_tok,
                                                      OP_PHASE_WAITING_FOR_TRUSTED_TIME,
                                                      &g_tok));
}

TEST_CASE("op-acquire: an uncertain commit becomes the guard, never active", "[pool_op]")
{
    PoolOperationRequest r = req_of(OP_REQUEST_TIMED_SESSION_START, 77u);
    PoolOperationPersistenceProof proof;

    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &r, &g_tok, &g_dec));
    memset(&proof, 0, sizeof(proof));
    proof.kind = OP_PROOF_SESSION_COMMITTED;
    proof.store_result = STORE_COMMIT_UNCERTAIN;
    proof.session_id = 77u;
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_UNCERTAIN,
                      pool_operation_apply_persistence_proof(&g_st, &g_tok, &proof, &g_tok2));
    TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_st.phase);
    TEST_ASSERT_FALSE(g_tok2.valid);
    /* the guard now denies manual work */
    TEST_ASSERT_FALSE(pool_operation_evaluate(&g_st,
        &(PoolOperationRequest){ .kind = OP_REQUEST_MANUAL_POOL_PATCH }).allowed);
}

TEST_CASE("op-acquire: manual PATCH and OTA are mutually exclusive", "[pool_op]")
{
    PoolOperationRequest patch = req_of(OP_REQUEST_MANUAL_POOL_PATCH, 0u);
    PoolOperationRequest ota = req_of(OP_REQUEST_OTA_UPDATE, 0u);

    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &patch, &g_tok, &g_dec));
    TEST_ASSERT_EQUAL(OP_OWNER_MANUAL_POOL_PATCH, g_st.owner);
    TEST_ASSERT_EQUAL(OP_ERR_BUSY, pool_operation_acquire(&g_st, &ota, &g_tok2, &g_dec));
    TEST_ASSERT_EQUAL(OP_OWNER_MANUAL_POOL_PATCH, g_dec.blocking_owner);

    /* verified-complete release frees; OTA then acquires */
    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_release_manual(&g_st, &g_tok, OP_OUTCOME_VERIFIED_COMPLETE));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_st.owner);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &ota, &g_tok2, &g_dec));
    TEST_ASSERT_EQUAL(OP_OWNER_OTA_UPDATE, g_st.owner);
    /* no-mutation release frees again */
    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_release_manual(&g_st, &g_tok2, OP_OUTCOME_NO_MUTATION_OCCURRED));
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_st.phase);
}

TEST_CASE("op-acquire: generation exhaustion fails closed", "[pool_op]")
{
    PoolOperationRequest r = req_of(OP_REQUEST_MANUAL_POOL_PATCH, 0u);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    g_st.lease_generation = UINT32_MAX; /* white-box: force the ceiling */
    TEST_ASSERT_EQUAL(OP_ERR_GENERATION_EXHAUSTED,
                      pool_operation_acquire(&g_st, &r, &g_tok, &g_dec));
    TEST_ASSERT_FALSE(g_tok.valid);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_st.owner); /* unchanged, still locked-consistent */
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, g_st.lease_generation); /* never wrapped */
}

/* ================================================================= */
/* E/F. Restore Now and transitions                                   */
/* ================================================================= */

TEST_CASE("op-restore-now: controls the existing owner, never a second lease", "[pool_op]")
{
    make_active_rec(&g_rec);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false)); /* WAITING owner */
    TEST_ASSERT_EQUAL(OP_OWNER_BOOT_RECOVERY, g_st.owner);
    {
        uint32_t gen_before = g_st.lease_generation;
        TEST_ASSERT_EQUAL(OP_OK,
                          pool_operation_restore_now(&g_st, SESSION_ID, &g_tok2));
        TEST_ASSERT_EQUAL(OP_OWNER_SOURCE_RESTORE, g_st.owner);
        TEST_ASSERT_EQUAL(OP_PHASE_RESTORING_SOURCE, g_st.phase);
        TEST_ASSERT_TRUE(g_st.lease_generation > gen_before);
        /* the old bootstrap token is now stale */
        TEST_ASSERT_EQUAL(OP_ERR_STALE_LEASE,
                          pool_operation_transition_phase(&g_st, &g_tok,
                                                          OP_PHASE_RECOVERY_GUARD, NULL));
        /* the repeat is idempotent: same token, no generation bump */
        {
            uint32_t gen_now = g_st.lease_generation;
            PoolOperationLeaseToken again;
            TEST_ASSERT_EQUAL(OP_OK,
                              pool_operation_restore_now(&g_st, SESSION_ID, &again));
            TEST_ASSERT_EQUAL_UINT32(gen_now, again.lease_generation);
            TEST_ASSERT_EQUAL_UINT32(gen_now, g_st.lease_generation);
        }
    }
    /* wrong session and no-session results are stable */
    TEST_ASSERT_EQUAL(OP_ERR_SESSION_MISMATCH,
                      pool_operation_restore_now(&g_st, 9999u, &g_tok2));
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    TEST_ASSERT_EQUAL(OP_ERR_NO_ACTIVE_SESSION,
                      pool_operation_restore_now(&g_st, SESSION_ID, &g_tok2));
}

TEST_CASE("op-transition: the legal phase graph rotates tokens", "[pool_op]")
{
    PoolOperationRequest r = req_of(OP_REQUEST_TIMED_SESSION_START, 77u);
    PoolOperationPersistenceProof proof;

    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &r, &g_tok, &g_dec));
    memset(&proof, 0, sizeof(proof));
    proof.kind = OP_PROOF_SESSION_COMMITTED;
    proof.store_result = STORE_OK;
    proof.session_id = 77u;
    proof.committed_record_generation = 1u;
    proof.persisted_state = POOL_STATE_TARGET_SNAPSHOT_COMMITTED;
    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_apply_persistence_proof(&g_st, &g_tok, &proof, &g_tok));

    /* ACTIVE -> WAITING -> VERIFYING -> ACTIVE -> RESTORING */
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_transition_phase(
                                 &g_st, &g_tok, OP_PHASE_WAITING_FOR_TRUSTED_TIME, &g_tok));
    TEST_ASSERT_EQUAL(OP_PHASE_WAITING_FOR_TRUSTED_TIME, g_st.phase);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_transition_phase(
                                 &g_st, &g_tok, OP_PHASE_VERIFYING_TARGET, &g_tok));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_transition_phase(
                                 &g_st, &g_tok, OP_PHASE_ACTIVE, &g_tok));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_transition_phase(
                                 &g_st, &g_tok, OP_PHASE_RESTORING_SOURCE, &g_tok));
    TEST_ASSERT_EQUAL(OP_OWNER_SOURCE_RESTORE, g_st.owner);

    /* illegal transitions change nothing */
    TEST_ASSERT_EQUAL(OP_ERR_INVALID_TRANSITION,
                      pool_operation_transition_phase(&g_st, &g_tok, OP_PHASE_FREE, &g_tok2));
    TEST_ASSERT_EQUAL(OP_ERR_INVALID_TRANSITION,
                      pool_operation_transition_phase(&g_st, &g_tok,
                                                      OP_PHASE_TERMINAL_ACK_PENDING, &g_tok2));
    TEST_ASSERT_EQUAL(OP_PHASE_RESTORING_SOURCE, g_st.phase);

    /* guard escalation is always available to the owner */
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_transition_phase(
                                 &g_st, &g_tok, OP_PHASE_RECOVERY_GUARD, &g_tok2));
    TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_st.owner);
}

/* ================================================================= */
/* G. Terminal proofs, releases, acknowledgement                      */
/* ================================================================= */

TEST_CASE("op-release: session release demands a durable terminal proof", "[pool_op]")
{
    PoolOperationPersistenceProof proof;

    make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false)); /* SOURCE_RESTORE */

    /* no proof: releasing the active restore lease is unsafe */
    TEST_ASSERT_EQUAL(OP_ERR_UNSAFE_RELEASE,
                      pool_operation_release_session(&g_st, &g_tok));
    TEST_ASSERT_EQUAL(OP_OWNER_SOURCE_RESTORE, g_st.owner);

    /* a terminal proof with the obligation still held is rejected */
    memset(&proof, 0, sizeof(proof));
    proof.kind = OP_PROOF_TERMINAL_COMMITTED;
    proof.store_result = STORE_OK;
    proof.session_id = SESSION_ID;
    proof.committed_record_generation = 9u;
    proof.persisted_state = POOL_STATE_COMPLETE;
    proof.restore_required = true;
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_MISMATCH,
                      pool_operation_apply_persistence_proof(&g_st, &g_tok, &proof, &g_tok2));

    /* a TARGET_FAILED "terminal" proof is rejected outright */
    proof.restore_required = false;
    proof.persisted_state = POOL_STATE_TARGET_FAILED;
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_MISMATCH,
                      pool_operation_apply_persistence_proof(&g_st, &g_tok, &proof, &g_tok2));

    /* RECOVERY_REQUIRED is never an acknowledgeable terminal either */
    proof.persisted_state = POOL_STATE_RECOVERY_REQUIRED;
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_MISMATCH,
                      pool_operation_apply_persistence_proof(&g_st, &g_tok, &proof, &g_tok2));

    /* the genuine COMPLETE proof reaches TERMINAL_ACK_PENDING and releases */
    proof.persisted_state = POOL_STATE_COMPLETE;
    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_apply_persistence_proof(&g_st, &g_tok, &proof, &g_tok));
    TEST_ASSERT_EQUAL(OP_PHASE_TERMINAL_ACK_PENDING, g_st.phase);
    TEST_ASSERT_FALSE(g_st.restore_required);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_release_session(&g_st, &g_tok));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_st.phase);
    TEST_ASSERT_TRUE(g_st.terminal_pending); /* the result stays retained */

    /* the released token cannot act again */
    TEST_ASSERT_EQUAL(OP_ERR_NO_ACTIVE_SESSION,
                      pool_operation_release_session(&g_st, &g_tok));
}

TEST_CASE("op-ack: acknowledgement clears only with a CLEARED proof", "[pool_op]")
{
    PoolOperationRequest ack = req_of(OP_REQUEST_SESSION_ACKNOWLEDGE, SESSION_ID);
    PoolOperationRequest start = req_of(OP_REQUEST_TIMED_SESSION_START, 88u);
    PoolOperationPersistenceProof proof;

    make_state_rec(&g_rec, POOL_STATE_COMPLETE, false);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false)); /* terminal pending */

    /* a new session is blocked until acknowledgement */
    TEST_ASSERT_EQUAL(OP_ERR_TERMINAL_ACK_REQUIRED,
                      pool_operation_acquire(&g_st, &start, &g_tok2, &g_dec));

    /* the ack lease is exclusive */
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &ack, &g_tok, &g_dec));
    TEST_ASSERT_EQUAL(OP_OWNER_SESSION_ACKNOWLEDGE, g_st.owner);
    TEST_ASSERT_EQUAL(OP_ERR_BUSY,
                      pool_operation_acquire(&g_st,
                          &(PoolOperationRequest){ .kind = OP_REQUEST_MANUAL_POOL_PATCH },
                          &g_tok2, &g_dec));

    /* a mismatched proof leaves the terminal result pending */
    memset(&proof, 0, sizeof(proof));
    proof.kind = OP_PROOF_CLEARED;
    proof.store_result = STORE_OK; /* wrong: must be STORE_CLEARED */
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_MISMATCH,
                      pool_operation_release_ack(&g_st, &g_tok, &proof));
    TEST_ASSERT_TRUE(g_st.terminal_pending);

    /* an uncertain tombstone commit becomes the guard — never guessed */
    proof.store_result = STORE_COMMIT_UNCERTAIN;
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_UNCERTAIN,
                      pool_operation_release_ack(&g_st, &g_tok, &proof));
    TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_st.owner);

    /* fresh bootstrap; the clean CLEARED proof completes the flow */
    make_state_rec(&g_rec, POOL_STATE_COMPLETE, false);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &ack, &g_tok, &g_dec));
    proof.store_result = STORE_CLEARED;
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_release_ack(&g_st, &g_tok, &proof));
    TEST_ASSERT_FALSE(g_st.terminal_pending);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_st.phase);
    /* a new timed session is now eligible */
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &start, &g_tok2, &g_dec));
    TEST_ASSERT_EQUAL(OP_OWNER_TIMED_SESSION, g_st.owner);
}

TEST_CASE("op-ack: unresolved sessions can never be acknowledged", "[pool_op]")
{
    PoolOperationRequest ack = req_of(OP_REQUEST_SESSION_ACKNOWLEDGE, SESSION_ID);
    PoolOperationDecision d;

    /* active restore ownership: ack denied busy */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    d = pool_operation_evaluate(&g_st, &ack);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_BUSY, d.status);

    /* RESTORE_FAILED keeps the obligation: the guard/owner path denies */
    make_state_rec(&g_rec, POOL_STATE_RESTORE_FAILED, true);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    d = pool_operation_evaluate(&g_st, &ack);
    TEST_ASSERT_FALSE(d.allowed);

    /* no terminal result: nothing to acknowledge */
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    d = pool_operation_evaluate(&g_st, &ack);
    TEST_ASSERT_FALSE(d.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_NO_ACTIVE_SESSION, d.status);
}

TEST_CASE("op-release: stale tokens and wrong owners never release", "[pool_op]")
{
    PoolOperationRequest patch = req_of(OP_REQUEST_MANUAL_POOL_PATCH, 0u);
    PoolOperationLeaseToken stale;

    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &patch, &g_tok, &g_dec));
    stale = g_tok;
    stale.lease_generation += 1u;
    TEST_ASSERT_EQUAL(OP_ERR_STALE_LEASE,
                      pool_operation_release_manual(&g_st, &stale, OP_OUTCOME_VERIFIED_COMPLETE));
    TEST_ASSERT_EQUAL(OP_OWNER_MANUAL_POOL_PATCH, g_st.owner); /* unchanged */

    /* an uncertain manual outcome escalates to the guard */
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_UNCERTAIN,
                      pool_operation_release_manual(&g_st, &g_tok, OP_OUTCOME_MUTATION_UNCERTAIN));
    TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_st.owner);
}

/* ================================================================= */
/* H. Property-style sweeps                                           */
/* ================================================================= */

TEST_CASE("op-property: denied and read-only requests never mutate", "[pool_op]")
{
    static const PoolStoreResult boots[] = { STORE_EMPTY, STORE_OK, STORE_CORRUPT };
    size_t b;
    int k;
    for (b = 0; b < sizeof(boots) / sizeof(boots[0]); b++) {
        if (boots[b] == STORE_OK) {
            make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
            TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
        } else {
            TEST_ASSERT_EQUAL(OP_OK, boot(boots[b], NULL, false));
        }
        for (k = 0; k < (int)OP_REQUEST__COUNT + 2; k++) {
            PoolOperationRequest r = req_of((PoolOperationRequestKind)k, 1u);
            PoolOperationDecision d;
            PoolOperationStatus st;
            g_st2 = g_st; /* byte snapshot */
            d = pool_operation_evaluate(&g_st, &r);
            TEST_ASSERT_EQUAL(0, memcmp(&g_st2, &g_st, sizeof(g_st))); /* eval pure */
            TEST_ASSERT_TRUE(d.status < POOL_OPERATION_STATUS__COUNT);
            st = pool_operation_acquire(&g_st, &r, &g_tok2, &g_dec);
            if (!g_dec.allowed) {
                TEST_ASSERT_EQUAL(0, memcmp(&g_st2, &g_st, sizeof(g_st)));
                TEST_ASSERT_FALSE(g_tok2.valid); /* no partial token */
                TEST_ASSERT_TRUE(st != OP_OK || r.kind == OP_REQUEST_READ_ONLY);
            } else if (r.kind == OP_REQUEST_READ_ONLY ||
                       r.kind == OP_REQUEST_PROTOCOL_RECONCILE ||
                       r.kind == OP_REQUEST_MANUAL_DEVICE_RESTART ||
                       r.kind == OP_REQUEST_TIMED_SESSION_INTERNAL) {
                /* allowed but lease-free kinds also change nothing */
                TEST_ASSERT_EQUAL(0, memcmp(&g_st2, &g_st, sizeof(g_st)));
            } else {
                /* a lease was acquired: restore the base state for the sweep */
                g_st = g_st2;
            }
        }
    }
}

TEST_CASE("op-property: one exclusive owner and obligation safety", "[pool_op]")
{
    /* under every session-class bootstrap, no second mutating owner can be
     * created and the restore obligation blocks manual/OTA/new-session */
    static const PoolSessionState states[] = {
        POOL_STATE_APPLYING_TARGET, POOL_STATE_TARGET_ACTIVE, POOL_STATE_RESTORE_DUE,
        POOL_STATE_APPLYING_RESTORE, POOL_STATE_TARGET_FAILED, POOL_STATE_INTERRUPTED,
        POOL_STATE_RESTORE_FAILED, POOL_STATE_RECOVERY_REQUIRED,
    };
    static const PoolOperationRequestKind muts[] = {
        OP_REQUEST_TIMED_SESSION_START, OP_REQUEST_MANUAL_POOL_PATCH,
        OP_REQUEST_OTA_UPDATE, OP_REQUEST_MANUAL_DEVICE_RESTART,
        OP_REQUEST_SESSION_ACKNOWLEDGE, OP_REQUEST_PROTOCOL_RECONCILE,
    };
    size_t si, mi;
    for (si = 0; si < sizeof(states) / sizeof(states[0]); si++) {
        make_state_rec(&g_rec, states[si], true);
        TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
        TEST_ASSERT_TRUE(g_st.owner == OP_OWNER_BOOT_RECOVERY ||
                         g_st.owner == OP_OWNER_SOURCE_RESTORE ||
                         g_st.owner == OP_OWNER_RECOVERY_GUARD);
        TEST_ASSERT_TRUE(g_st.restore_required || g_st.owner == OP_OWNER_RECOVERY_GUARD);
        for (mi = 0; mi < sizeof(muts) / sizeof(muts[0]); mi++) {
            PoolOperationRequest r = req_of(muts[mi], 88u);
            PoolOperationStatus st = pool_operation_acquire(&g_st, &r, &g_tok2, &g_dec);
            TEST_ASSERT_TRUE(st != OP_OK);
            TEST_ASSERT_FALSE(g_tok2.valid);
        }
    }
}

TEST_CASE("op-property: pure calls are deterministic and input-immutable", "[pool_op]")
{
    PoolOperationDecision d1, d2;
    PoolOperationRequest r = req_of(OP_REQUEST_MANUAL_POOL_PATCH, 0u);
    PoolOperationRequest r_copy;
    PoolSessionRecord rec_copy;
    PoolSessionRecoveryPlan plan_copy;

    make_state_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    rec_copy = g_rec;
    make_plan(STORE_OK, &g_rec, false);
    plan_copy = g_plan;
    make_bi(STORE_OK, &g_rec);
    pool_operation_state_init(&g_st);
    (void)pool_operation_bootstrap(&g_st, &g_bi, NULL);
    /* bootstrap consumed but never mutated the record or the plan */
    TEST_ASSERT_EQUAL(0, memcmp(&rec_copy, &g_rec, sizeof(g_rec)));
    TEST_ASSERT_EQUAL(0, memcmp(&plan_copy, &g_plan, sizeof(g_plan)));

    r_copy = r;
    d1 = pool_operation_evaluate(&g_st, &r);
    d2 = pool_operation_evaluate(&g_st, &r);
    TEST_ASSERT_EQUAL(0, memcmp(&d1, &d2, sizeof(d1)));
    TEST_ASSERT_EQUAL(0, memcmp(&r_copy, &r, sizeof(r)));
}

/* ================================================================= */
/* H. Gate B8 correction — audited no-mutation aborts                 */
/* ================================================================= */

static PoolOperationNoMutationEvidence good_ev(PoolStoreResult r)
{
    PoolOperationNoMutationEvidence e;
    memset(&e, 0, sizeof(e));
    e.store_result               = r;
    e.store_unchanged            = true;
    e.pool_config_untouched      = true;
    e.protocol_untouched         = true;
    e.restore_required_never_set = true;
    return e;
}

/* Acquire an uncommitted timed-session reservation from a clean FREE. */
static void reserve_session(void)
{
    PoolOperationRequest r = req_of(OP_REQUEST_TIMED_SESSION_START, SESSION_ID);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &r, &g_tok, &g_dec));
    TEST_ASSERT_EQUAL(OP_OWNER_TIMED_SESSION, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_RESERVED_PENDING_PERSISTENCE, g_st.phase);
    TEST_ASSERT_FALSE(g_st.durable_claim);
}

TEST_CASE("op-abort: a proven no-mutation reservation releases to FREE", "[pool_op]")
{
    PoolOperationNoMutationEvidence ev = good_ev(STORE_EMPTY);
    PoolOperationLeaseToken stale;
    uint32_t gen_before;
    PoolOperationRequest start;

    reserve_session();
    stale      = g_tok;
    gen_before = g_st.lease_generation;

    TEST_ASSERT_EQUAL(OP_OK, pool_operation_abort_reservation(&g_st, &g_tok, &ev));
    /* Ownership is gone, the generation rotated and no terminal was invented. */
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_st.phase);
    TEST_ASSERT_TRUE(g_st.lease_generation > gen_before);
    TEST_ASSERT_FALSE(g_st.terminal_pending);
    TEST_ASSERT_FALSE(g_st.durable_claim);
    TEST_ASSERT_EQUAL_UINT32(0u, g_st.bound_session_id);
    TEST_ASSERT_EQUAL_UINT32(0u, g_st.resource_scopes);

    /* The old token no longer works for anything (the exact refusal code
     * depends on the committed check_token ordering; what matters is that
     * it is refused and nothing changes). */
    TEST_ASSERT_NOT_EQUAL(OP_OK,
                          pool_operation_abort_reservation(&g_st, &stale, &ev));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_st.phase);

    /* A later create acquires normally — no client retry cleaned anything. */
    start = req_of(OP_REQUEST_TIMED_SESSION_START, SESSION_ID + 1u);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &start, &g_tok2, &g_dec));
    TEST_ASSERT_EQUAL(OP_OWNER_TIMED_SESSION, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_RESERVED_PENDING_PERSISTENCE, g_st.phase);
}

TEST_CASE("op-abort: a CLEARED or OK store also proves an unchanged state", "[pool_op]")
{
    PoolOperationNoMutationEvidence ev;

    reserve_session();
    ev = good_ev(STORE_CLEARED);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_abort_reservation(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_st.phase);

    reserve_session();
    ev = good_ev(STORE_OK);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_abort_reservation(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_st.phase);
}

TEST_CASE("op-abort: incomplete evidence never releases a reservation", "[pool_op]")
{
    PoolOperationNoMutationEvidence ev;
    unsigned i;

    /* Each missing claim, one at a time. */
    for (i = 0; i < 4u; i++) {
        reserve_session();
        ev = good_ev(STORE_EMPTY);
        switch (i) {
        case 0: ev.store_unchanged = false; break;
        case 1: ev.pool_config_untouched = false; break;
        case 2: ev.protocol_untouched = false; break;
        default: ev.restore_required_never_set = false; break;
        }
        TEST_ASSERT_EQUAL(OP_ERR_UNSAFE_RELEASE,
                          pool_operation_abort_reservation(&g_st, &g_tok, &ev));
        TEST_ASSERT_EQUAL(OP_OWNER_TIMED_SESSION, g_st.owner);
        TEST_ASSERT_EQUAL(OP_PHASE_RESERVED_PENDING_PERSISTENCE, g_st.phase);
    }

    /* A store result that proves nothing about the old state. */
    reserve_session();
    ev = good_ev(STORE_IO_ERROR);
    TEST_ASSERT_EQUAL(OP_ERR_UNSAFE_RELEASE,
                      pool_operation_abort_reservation(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_PHASE_RESERVED_PENDING_PERSISTENCE, g_st.phase);

    reserve_session();
    ev = good_ev(STORE_READBACK_MISMATCH);
    TEST_ASSERT_EQUAL(OP_ERR_UNSAFE_RELEASE,
                      pool_operation_abort_reservation(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_PHASE_RESERVED_PENDING_PERSISTENCE, g_st.phase);

    TEST_ASSERT_EQUAL(OP_ERR_INVALID_ARGUMENT,
                      pool_operation_abort_reservation(&g_st, &g_tok, NULL));
    TEST_ASSERT_EQUAL(OP_ERR_INVALID_ARGUMENT,
                      pool_operation_abort_reservation(NULL, &g_tok, &ev));
}

TEST_CASE("op-abort: an uncertain commit guards and never releases", "[pool_op]")
{
    PoolOperationNoMutationEvidence ev;
    PoolOperationRequest r;

    reserve_session();
    ev = good_ev(STORE_COMMIT_UNCERTAIN);
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_UNCERTAIN,
                      pool_operation_abort_reservation(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_st.phase);
    /* A guarded device admits only operator recovery. */
    r = req_of(OP_REQUEST_TIMED_SESSION_START, SESSION_ID);
    g_dec = pool_operation_evaluate(&g_st, &r);
    TEST_ASSERT_FALSE(g_dec.allowed);
    TEST_ASSERT_EQUAL(OP_ERR_RECOVERY_LOCKED, g_dec.status);
}

TEST_CASE("op-abort: only an uncommitted TIMED_SESSION reservation is abortable",
          "[pool_op]")
{
    PoolOperationNoMutationEvidence ev = good_ev(STORE_EMPTY);
    PoolOperationPersistenceProof pr;
    PoolOperationRequest r;

    /* A durable BOOT_RECOVERY reservation is never abortable. */
    make_state_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    if (g_st.phase == OP_PHASE_RESERVED_PENDING_PERSISTENCE) {
        TEST_ASSERT_EQUAL(OP_ERR_NOT_OWNER,
                          pool_operation_abort_reservation(&g_st, &g_tok, &ev));
    }

    /* A manual lease is not this operation. */
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_EMPTY, NULL, false));
    r = req_of(OP_REQUEST_MANUAL_POOL_PATCH, 0u);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &r, &g_tok, &g_dec));
    TEST_ASSERT_EQUAL(OP_ERR_NOT_OWNER,
                      pool_operation_abort_reservation(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_OWNER_MANUAL_POOL_PATCH, g_st.owner);

    /* An ACTIVATED session (proof accepted) is past the abortable window. */
    reserve_session();
    memset(&pr, 0, sizeof(pr));
    pr.kind = OP_PROOF_SESSION_COMMITTED;
    pr.store_result = STORE_OK;
    pr.committed_record_generation = 7u;
    pr.session_id = SESSION_ID;
    pr.persisted_state = POOL_STATE_TARGET_SNAPSHOT_COMMITTED;
    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_apply_persistence_proof(&g_st, &g_tok, &pr, &g_tok));
    TEST_ASSERT_EQUAL(OP_PHASE_ACTIVE, g_st.phase);
    TEST_ASSERT_EQUAL(OP_ERR_INVALID_TRANSITION,
                      pool_operation_abort_reservation(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_PHASE_ACTIVE, g_st.phase);
}

TEST_CASE("op-abort: an acknowledgement releases with the terminal retained",
          "[pool_op]")
{
    PoolOperationNoMutationEvidence ev;
    PoolOperationRequest r;
    PoolOperationRequest start;
    uint32_t gen_before;

    make_state_rec(&g_rec, POOL_STATE_COMPLETE, false);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    TEST_ASSERT_TRUE(g_st.terminal_pending);

    r = req_of(OP_REQUEST_SESSION_ACKNOWLEDGE, SESSION_ID);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &r, &g_tok, &g_dec));
    TEST_ASSERT_EQUAL(OP_OWNER_SESSION_ACKNOWLEDGE, g_st.owner);
    gen_before = g_st.lease_generation;

    /* The tombstone definitely did not commit; the original record stands. */
    ev = good_ev(STORE_OK);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_abort_acknowledge(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_st.owner);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_st.phase);
    TEST_ASSERT_TRUE(g_st.lease_generation > gen_before);
    /* THE point: the retained terminal result survives for a later retry. */
    TEST_ASSERT_TRUE(g_st.terminal_pending);

    /* A later acknowledgement may retry and acquire normally. */
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &r, &g_tok2, &g_dec));
    TEST_ASSERT_EQUAL(OP_OWNER_SESSION_ACKNOWLEDGE, g_st.owner);
    /* ... but a NEW session still cannot start while the terminal is retained. */
    start = req_of(OP_REQUEST_TIMED_SESSION_START, 99u);
    g_dec = pool_operation_evaluate(&g_st, &start);
    TEST_ASSERT_FALSE(g_dec.allowed);
}

TEST_CASE("op-abort: an acknowledgement abort demands the unchanged terminal",
          "[pool_op]")
{
    PoolOperationNoMutationEvidence ev;
    PoolOperationRequest r;

    make_state_rec(&g_rec, POOL_STATE_COMPLETE, false);
    TEST_ASSERT_EQUAL(OP_OK, boot(STORE_OK, &g_rec, false));
    r = req_of(OP_REQUEST_SESSION_ACKNOWLEDGE, SESSION_ID);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_acquire(&g_st, &r, &g_tok, &g_dec));

    /* STORE_CLEARED means the tombstone DID land: not a no-mutation abort. */
    ev = good_ev(STORE_CLEARED);
    TEST_ASSERT_EQUAL(OP_ERR_UNSAFE_RELEASE,
                      pool_operation_abort_acknowledge(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_OWNER_SESSION_ACKNOWLEDGE, g_st.owner);

    ev = good_ev(STORE_OK);
    ev.store_unchanged = false;
    TEST_ASSERT_EQUAL(OP_ERR_UNSAFE_RELEASE,
                      pool_operation_abort_acknowledge(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_OWNER_SESSION_ACKNOWLEDGE, g_st.owner);

    /* Uncertainty guards and keeps the terminal pending. */
    ev = good_ev(STORE_COMMIT_UNCERTAIN);
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_UNCERTAIN,
                      pool_operation_abort_acknowledge(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_PHASE_RECOVERY_GUARD, g_st.phase);
    TEST_ASSERT_TRUE(g_st.terminal_pending);

    /* A wrong owner is refused outright. */
    reserve_session();
    ev = good_ev(STORE_OK);
    TEST_ASSERT_EQUAL(OP_ERR_NOT_OWNER,
                      pool_operation_abort_acknowledge(&g_st, &g_tok, &ev));
    TEST_ASSERT_EQUAL(OP_OWNER_TIMED_SESSION, g_st.owner);
}
