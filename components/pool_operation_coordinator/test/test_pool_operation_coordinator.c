/*
 * Deterministic tests for the synchronized coordinator and the HTTP 409
 * policy surface (Gate B5).
 *
 * The two-contender test uses semaphore barriers (never sleeps/timing) so
 * exactly one exclusive acquisition succeeds deterministically. No
 * networking, no NVS, no OTA, no restart — nothing executes.
 */

#include <string.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "pool_operation_coordinator.h"
#include "pool_operation_http_policy.h"

#define EPOCH_A_S  1750000000ull
#define SESSION_ID 42u

static PoolOperationCoordinator g_coord;
static PoolSessionRecord g_rec;
static PoolSessionRecoveryPlan g_plan;
static PoolSessionBootContext g_bctx;
static PoolOperationBootstrapInput g_bi;
static PoolOperationState g_snap;
static PoolOperationState g_snap2;
static PoolOperationLeaseToken g_tok;
static PoolOperationLeaseToken g_tok2;
static PoolOperationDecision g_dec;
static PoolOperationHttpConflict g_http;

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
    r->generation = 3u;
    r->session_id = SESSION_ID;
    r->b1_model_version = POOL_SESSION_MODEL_VERSION;
    r->state = st;
    fill_identity(&r->source, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    fill_identity(&r->target, POOL_CHAIN_BITCOIN_CASH, "bch.example", 3334, "acct.worker");
    r->password_policy = POOL_SESSION_PW_KEEP_CURRENT;
    r->restore_required = rr;
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
    if (st == POOL_STATE_COMPLETE) {
        r->restore_verify.connection_observed = true;
        r->restore_verify.mining_observed = true;
        r->restore_verify.identity_verified = true;
    }
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(r));
}

static void coord_boot(PoolStoreResult res, const PoolSessionRecord *rec)
{
    memset(&g_bctx, 0, sizeof(g_bctx));
    g_bctx.store_result = res;
    g_bctx.record_present = (rec != NULL);
    g_bctx.record = rec;
    g_bctx.reset_class = POOL_RESET_CLASS_POWER_ON;
    g_bctx.sync_wait_limit_s = 600u;
    g_bctx.time_provider_initialized = true;
    g_bctx.mining_inhibition_available = true;
    g_bctx.time_snapshot.status = TIME_ERR_NOT_SYNCED;
    (void)pool_session_recovery_plan(&g_bctx, &g_plan);

    memset(&g_bi, 0, sizeof(g_bi));
    g_bi.store_result = res;
    g_bi.record_present = (rec != NULL);
    g_bi.record = rec;
    g_bi.plan = &g_plan;
    g_bi.committed_generation = (rec != NULL) ? rec->generation : 0u;

    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_init(&g_coord));
    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_coordinator_bootstrap(&g_coord, &g_bi, &g_tok));
}

static PoolOperationRequest req_of(PoolOperationRequestKind k, uint32_t sid)
{
    PoolOperationRequest r;
    memset(&r, 0, sizeof(r));
    r.kind = k;
    r.session_id = sid;
    return r;
}

static bool bytes_contain(const void *hay, size_t len, const void *needle, size_t nlen)
{
    const uint8_t *h = (const uint8_t *)hay;
    size_t i;
    if (nlen == 0u || nlen > len) {
        return false;
    }
    for (i = 0; i + nlen <= len; i++) {
        if (memcmp(h + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

/* ================================================================= */
/* Coordinator lifecycle and atomicity                                */
/* ================================================================= */

TEST_CASE("coord: lifecycle is deterministic and fail-closed", "[pool_op_coord]")
{
    PoolOperationRequest patch = req_of(OP_REQUEST_MANUAL_POOL_PATCH, 0u);

    memset(&g_coord, 0, sizeof(g_coord));
    TEST_ASSERT_EQUAL(OP_ERR_NOT_INITIALIZED,
                      pool_operation_coordinator_try_acquire(&g_coord, &patch, &g_tok, &g_dec));
    TEST_ASSERT_EQUAL(OP_ERR_NOT_INITIALIZED,
                      pool_operation_coordinator_snapshot(&g_coord, &g_snap));
    TEST_ASSERT_EQUAL(OP_ERR_INVALID_ARGUMENT, pool_operation_coordinator_init(NULL));

    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_init(&g_coord));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &g_snap));
    TEST_ASSERT_FALSE(g_snap.bootstrapped);
    /* unbootstrapped: mutation denied, read-only allowed */
    TEST_ASSERT_EQUAL(OP_ERR_BOOTSTRAP_REQUIRED,
                      pool_operation_coordinator_try_acquire(&g_coord, &patch, &g_tok, &g_dec));
    g_dec = pool_operation_coordinator_evaluate(&g_coord,
        &(PoolOperationRequest){ .kind = OP_REQUEST_READ_ONLY });
    TEST_ASSERT_TRUE(g_dec.allowed);

    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_deinit(&g_coord));
    TEST_ASSERT_EQUAL(OP_ERR_NOT_INITIALIZED,
                      pool_operation_coordinator_snapshot(&g_coord, &g_snap));
}

TEST_CASE("coord: the full session lifecycle runs through the wrapper", "[pool_op_coord]")
{
    PoolOperationRequest start = req_of(OP_REQUEST_TIMED_SESSION_START, 77u);
    PoolOperationRequest ack = req_of(OP_REQUEST_SESSION_ACKNOWLEDGE, 77u);
    PoolOperationPersistenceProof proof;

    coord_boot(STORE_EMPTY, NULL);
    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_coordinator_try_acquire(&g_coord, &start, &g_tok, &g_dec));
    TEST_ASSERT_TRUE(g_tok.valid);

    memset(&proof, 0, sizeof(proof));
    proof.kind = OP_PROOF_SESSION_COMMITTED;
    proof.store_result = STORE_OK;
    proof.session_id = 77u;
    proof.committed_record_generation = 1u;
    proof.persisted_state = POOL_STATE_TARGET_SNAPSHOT_COMMITTED;
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_apply_persistence_proof(
                                 &g_coord, &g_tok, &proof, &g_tok));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_transition_phase(
                                 &g_coord, &g_tok, OP_PHASE_WAITING_FOR_TRUSTED_TIME, &g_tok));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_restore_now(&g_coord, 77u, &g_tok));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &g_snap));
    TEST_ASSERT_EQUAL(OP_OWNER_SOURCE_RESTORE, g_snap.owner);

    proof.kind = OP_PROOF_TERMINAL_COMMITTED;
    proof.persisted_state = POOL_STATE_COMPLETE;
    proof.restore_required = false;
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_apply_persistence_proof(
                                 &g_coord, &g_tok, &proof, &g_tok));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_release_session(&g_coord, &g_tok));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &g_snap));
    TEST_ASSERT_TRUE(g_snap.terminal_pending);

    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_coordinator_try_acquire(&g_coord, &ack, &g_tok, &g_dec));
    proof.kind = OP_PROOF_CLEARED;
    proof.store_result = STORE_CLEARED;
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_release_ack(&g_coord, &g_tok, &proof));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &g_snap));
    TEST_ASSERT_FALSE(g_snap.terminal_pending);
    TEST_ASSERT_EQUAL(OP_PHASE_FREE, g_snap.phase);
    (void)pool_operation_coordinator_deinit(&g_coord);
}

TEST_CASE("coord: failed acquisitions never mutate the snapshot", "[pool_op_coord]")
{
    PoolOperationRequest start = req_of(OP_REQUEST_TIMED_SESSION_START, 88u);

    make_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    coord_boot(STORE_OK, &g_rec); /* SOURCE_RESTORE owner */
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &g_snap));
    TEST_ASSERT_EQUAL(OP_ERR_BUSY,
                      pool_operation_coordinator_try_acquire(&g_coord, &start, &g_tok2, &g_dec));
    TEST_ASSERT_FALSE(g_tok2.valid); /* no partially published token */
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &g_snap2));
    TEST_ASSERT_EQUAL(0, memcmp(&g_snap, &g_snap2, sizeof(g_snap)));
    (void)pool_operation_coordinator_deinit(&g_coord);
}

/* ---------------- deterministic two-contender test ---------------- */

typedef struct {
    SemaphoreHandle_t worker_acquired;
    SemaphoreHandle_t release_worker;
    SemaphoreHandle_t worker_done;
    PoolOperationStatus acquire_status;
    PoolOperationLeaseToken token;
} ContenderCtx;

static void contender_task(void *arg)
{
    ContenderCtx *cc = (ContenderCtx *)arg;
    PoolOperationRequest patch = req_of(OP_REQUEST_MANUAL_POOL_PATCH, 0u);
    PoolOperationDecision d;

    cc->acquire_status =
        pool_operation_coordinator_try_acquire(&g_coord, &patch, &cc->token, &d);
    xSemaphoreGive(cc->worker_acquired);          /* barrier 1: acquired    */
    xSemaphoreTake(cc->release_worker, portMAX_DELAY); /* barrier 2: wait   */
    (void)pool_operation_coordinator_release_manual(&g_coord, &cc->token,
                                                    OP_OUTCOME_NO_MUTATION_OCCURRED);
    xSemaphoreGive(cc->worker_done);              /* barrier 3: released   */
    vTaskDelete(NULL);
}

TEST_CASE("coord: exactly one of two contenders acquires", "[pool_op_coord]")
{
    ContenderCtx cc;
    PoolOperationRequest patch = req_of(OP_REQUEST_MANUAL_POOL_PATCH, 0u);

    coord_boot(STORE_EMPTY, NULL);
    cc.worker_acquired = xSemaphoreCreateBinary();
    cc.release_worker = xSemaphoreCreateBinary();
    cc.worker_done = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(cc.worker_acquired);
    TEST_ASSERT_NOT_NULL(cc.release_worker);
    TEST_ASSERT_NOT_NULL(cc.worker_done);

    TEST_ASSERT_EQUAL(pdPASS,
                      xTaskCreate(contender_task, "contender", 4096, &cc, 5, NULL));
    /* barrier 1: the worker holds the lease before we contend */
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(cc.worker_acquired, portMAX_DELAY));
    TEST_ASSERT_EQUAL(OP_OK, cc.acquire_status);
    TEST_ASSERT_TRUE(cc.token.valid);

    /* the second contender deterministically loses */
    TEST_ASSERT_EQUAL(OP_ERR_BUSY,
                      pool_operation_coordinator_try_acquire(&g_coord, &patch, &g_tok, &g_dec));
    TEST_ASSERT_FALSE(g_tok.valid);
    TEST_ASSERT_EQUAL(OP_OWNER_MANUAL_POOL_PATCH, g_dec.blocking_owner);

    /* after the worker's clean release, acquisition succeeds */
    xSemaphoreGive(cc.release_worker);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(cc.worker_done, portMAX_DELAY));
    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_coordinator_try_acquire(&g_coord, &patch, &g_tok, &g_dec));
    TEST_ASSERT_TRUE(g_tok.valid);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_release_manual(
                                 &g_coord, &g_tok, OP_OUTCOME_NO_MUTATION_OCCURRED));

    vSemaphoreDelete(cc.worker_acquired);
    vSemaphoreDelete(cc.release_worker);
    vSemaphoreDelete(cc.worker_done);
    (void)pool_operation_coordinator_deinit(&g_coord);
}

TEST_CASE("coord: uncertain outcomes surface as a guard in the snapshot", "[pool_op_coord]")
{
    PoolOperationRequest patch = req_of(OP_REQUEST_MANUAL_POOL_PATCH, 0u);

    coord_boot(STORE_EMPTY, NULL);
    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_coordinator_try_acquire(&g_coord, &patch, &g_tok, &g_dec));
    TEST_ASSERT_EQUAL(OP_ERR_PERSISTENCE_UNCERTAIN,
                      pool_operation_coordinator_release_manual(&g_coord, &g_tok,
                                                                OP_OUTCOME_MUTATION_UNCERTAIN));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &g_snap));
    TEST_ASSERT_EQUAL(OP_OWNER_RECOVERY_GUARD, g_snap.owner);
    /* the guard denies further manual work */
    TEST_ASSERT_EQUAL(OP_ERR_RECOVERY_LOCKED,
                      pool_operation_coordinator_try_acquire(&g_coord, &patch, &g_tok2, &g_dec));
    (void)pool_operation_coordinator_deinit(&g_coord);
}

/* ================================================================= */
/* HTTP 409 policy surface                                            */
/* ================================================================= */

TEST_CASE("http: every ownership conflict maps to 409 with a stable code", "[pool_op_coord]")
{
    PoolOperationDecision d;
    memset(&g_snap, 0, sizeof(g_snap));

    /* busy codes per sanitized owner class */
    memset(&d, 0, sizeof(d));
    d.allowed = false;
    d.status = OP_ERR_BUSY;
    d.blocking_owner = OP_OWNER_BOOT_RECOVERY;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL_UINT16(409u, g_http.http_status);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_BUSY_TIMED_SESSION, g_http.code);
    TEST_ASSERT_TRUE(g_http.retryable);

    d.blocking_owner = OP_OWNER_SOURCE_RESTORE;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_BUSY_RESTORE, g_http.code);
    d.blocking_owner = OP_OWNER_MANUAL_POOL_PATCH;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_BUSY_MANUAL_POOL_CHANGE, g_http.code);
    d.blocking_owner = OP_OWNER_OTA_UPDATE;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_BUSY_OTA, g_http.code);

    /* specific statuses */
    d.status = OP_ERR_BOOTSTRAP_REQUIRED;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_BOOTSTRAP_REQUIRED, g_http.code);
    TEST_ASSERT_TRUE(g_http.retryable);
    d.status = OP_ERR_RECOVERY_LOCKED;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_RECOVERY_LOCKED, g_http.code);
    TEST_ASSERT_FALSE(g_http.retryable);
    d.status = OP_ERR_TERMINAL_ACK_REQUIRED;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_TERMINAL_ACK_REQUIRED, g_http.code);
    d.status = OP_ERR_PERSISTENCE_UNCERTAIN;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_PERSISTENCE_UNCERTAIN, g_http.code);
    d.status = OP_ERR_STALE_LEASE;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_STALE_LEASE, g_http.code);
    d.status = OP_ERR_NO_ACTIVE_SESSION;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_NO_ACTIVE_SESSION, g_http.code);
    d.status = OP_ERR_UNSAFE_RELEASE;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_UNSAFE_RELEASE, g_http.code);
    d.status = OP_ERR_UNSUPPORTED_REQUEST;
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_INVALID_REQUEST, g_http.code);
    TEST_ASSERT_EQUAL_UINT16(409u, g_http.http_status);
}

TEST_CASE("http: allowed decisions are not conflicts", "[pool_op_coord]")
{
    PoolOperationDecision d;
    memset(&d, 0, sizeof(d));
    d.allowed = true;
    d.status = OP_OK;
    memset(&g_snap, 0, sizeof(g_snap));
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL_UINT16(0u, g_http.http_status);
    TEST_ASSERT_EQUAL(OP_HTTP_NONE, g_http.code);
    TEST_ASSERT_EQUAL(OP_OWNER_NONE, g_http.active_owner);
}

TEST_CASE("http: the surface leaks no generations, ids or identities", "[pool_op_coord]")
{
    PoolOperationDecision d;
    uint32_t sid = 0x11223344u;
    uint32_t gen = 0x55667788u;

    make_rec(&g_rec, POOL_STATE_RESTORE_DUE, true);
    g_rec.session_id = sid;
    coord_boot(STORE_OK, &g_rec);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &g_snap));
    g_snap.bound_record_generation = gen; /* plant a distinctive pattern too */

    d = pool_operation_coordinator_evaluate(&g_coord,
        &(PoolOperationRequest){ .kind = OP_REQUEST_MANUAL_POOL_PATCH });
    TEST_ASSERT_FALSE(d.allowed);
    pool_operation_http_map(&d, &g_snap, &g_http);
    TEST_ASSERT_EQUAL_UINT16(409u, g_http.http_status);
    TEST_ASSERT_TRUE(g_http.restore_required);
    /* the planted 32-bit patterns never appear in the surface bytes */
    TEST_ASSERT_FALSE(bytes_contain(&g_http, sizeof(g_http), &sid, sizeof(sid)));
    TEST_ASSERT_FALSE(bytes_contain(&g_http, sizeof(g_http), &gen, sizeof(gen)));
    /* nor does any identity text appear anywhere in the surface */
    TEST_ASSERT_FALSE(bytes_contain(&g_http, sizeof(g_http), "example", 7));
    TEST_ASSERT_FALSE(bytes_contain(&g_http, sizeof(g_http), "acct", 4));
    (void)pool_operation_coordinator_deinit(&g_coord);
}

TEST_CASE("http: terminal-ack and code tokens are clean", "[pool_op_coord]")
{
    int i;
    make_rec(&g_rec, POOL_STATE_COMPLETE, false);
    coord_boot(STORE_OK, &g_rec);
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &g_snap));
    {
        PoolOperationDecision d = pool_operation_coordinator_evaluate(&g_coord,
            &(PoolOperationRequest){ .kind = OP_REQUEST_TIMED_SESSION_START,
                                     .session_id = 9u });
        pool_operation_http_map(&d, &g_snap, &g_http);
        TEST_ASSERT_EQUAL(OP_HTTP_OPERATION_TERMINAL_ACK_REQUIRED, g_http.code);
        TEST_ASSERT_TRUE(g_http.terminal_ack_required);
        TEST_ASSERT_FALSE(g_http.restore_required);
    }
    for (i = 0; i < (int)OP_HTTP__COUNT; i++) {
        const char *tok = pool_operation_http_code_str((PoolOperationHttpCode)i);
        const char *c;
        TEST_ASSERT_NOT_NULL(tok);
        for (c = tok; *c != '\0'; c++) {
            TEST_ASSERT_TRUE((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                             *c == '_');
        }
    }
    TEST_ASSERT_NOT_NULL(pool_operation_http_code_str((PoolOperationHttpCode)999));
    (void)pool_operation_coordinator_deinit(&g_coord);
}

TEST_CASE("coord: no B5 surface grants target mining", "[pool_op_coord]")
{
    /* Bootstrap from the B4 target-eligibility plan: the coordinator holds
     * VERIFYING_TARGET, the plan stays VERIFY_BEFORE_MINING, and no B5
     * output anywhere carries a mining grant (the state/decision/HTTP
     * models have no such field by construction). */
    make_rec(&g_rec, POOL_STATE_TARGET_ACTIVE, true);
    memset(&g_bctx, 0, sizeof(g_bctx));
    g_bctx.store_result = STORE_OK;
    g_bctx.record_present = true;
    g_bctx.record = &g_rec;
    g_bctx.reset_class = POOL_RESET_CLASS_POWER_ON;
    g_bctx.sync_wait_limit_s = 600u;
    g_bctx.time_provider_initialized = true;
    g_bctx.mining_inhibition_available = true;
    g_bctx.time_snapshot.trusted = true;
    g_bctx.time_snapshot.status = TIME_OK;
    g_bctx.time_snapshot.trusted_epoch_s = g_rec.deadline_epoch_s - 1000u;
    g_bctx.time_snapshot.sync_generation = 1u;
    (void)pool_session_recovery_plan(&g_bctx, &g_plan);
    TEST_ASSERT_EQUAL(POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET, g_plan.decision);
    TEST_ASSERT_EQUAL(POOL_BOOT_MINING_VERIFY_BEFORE_MINING, g_plan.mining_policy);

    memset(&g_bi, 0, sizeof(g_bi));
    g_bi.store_result = STORE_OK;
    g_bi.record_present = true;
    g_bi.record = &g_rec;
    g_bi.plan = &g_plan;
    g_bi.committed_generation = g_rec.generation;
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_init(&g_coord));
    TEST_ASSERT_EQUAL(OP_OK,
                      pool_operation_coordinator_bootstrap(&g_coord, &g_bi, &g_tok));
    TEST_ASSERT_EQUAL(OP_OK, pool_operation_coordinator_snapshot(&g_coord, &g_snap));
    TEST_ASSERT_EQUAL(OP_PHASE_VERIFYING_TARGET, g_snap.phase);
    TEST_ASSERT_TRUE(g_snap.restore_required);
    /* the B4 plan input was never mutated by B5 */
    TEST_ASSERT_EQUAL(POOL_BOOT_MINING_VERIFY_BEFORE_MINING, g_plan.mining_policy);
    (void)pool_operation_coordinator_deinit(&g_coord);
}
