/*
 * Deterministic tests for the Gate B7 PURE controlled-execution domain.
 *
 * Everything here is pure: no NVS, no networking, no tasks, no clock. All
 * identities are synthetic "*.example" fixtures; no real pool, account,
 * wallet or password exists anywhere.
 */

#include <string.h>
#include "unity.h"
#include "pool_session_execution_core.h"
#include "pool_session_execution.h"
#include "mining.h"
#include "utils.h"

#define EPOCH_A_S 1750000000ull

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

static void fill_identity_with_fallback(PoolConfigIdentity *c)
{
    fill_identity(c, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    c->fallback_enabled = true;
    strncpy(c->fallback.host, "fb.example", sizeof(c->fallback.host) - 1);
    c->fallback.port = 3335;
    strncpy(c->fallback.user, "acct.fb", sizeof(c->fallback.user) - 1);
    c->fallback.protocol = POOL_PROTO_STRATUM_V1;
}

static void make_record(PoolSessionRecord *r, PoolSessionState st, bool rr)
{
    pool_session_record_init(r);
    r->session_id       = 4242u;
    r->b1_model_version = POOL_SESSION_MODEL_VERSION;
    r->state            = st;
    fill_identity(&r->source, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    fill_identity(&r->target, POOL_CHAIN_BITCOIN_CASH, "bch.example", 3334, "acct.worker");
    r->password_policy  = POOL_SESSION_PW_KEEP_CURRENT;
    r->restore_required = rr;
    r->duration_s       = 3600u;
    if (st == POOL_STATE_TARGET_ACTIVE) {
        r->target_verify.connection_observed = true;
        r->target_verify.mining_observed     = true;
        r->target_verify.identity_verified   = true;
        r->verified_start_valid   = true;
        r->verified_start_epoch_s = EPOCH_A_S;
        r->deadline_valid         = true;
        r->deadline_epoch_s       = EPOCH_A_S + 3600u;
        r->deadline_sync_generation = 1u;
    }
    if (st == POOL_STATE_COMPLETE) {
        r->restore_verify.connection_observed = true;
        r->restore_verify.mining_observed     = true;
        r->restore_verify.identity_verified   = true;
    }
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(r));
}

static void make_session(PoolSession *s, PoolSessionState st, bool rr)
{
    PoolSessionRecord rec;
    make_record(&rec, st, rr);
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_to_session(&rec, s));
}

static void effective_from_identity(PoolExecEffectiveConfig *e,
                                    const PoolConfigIdentity *id)
{
    pool_exec_desired_from_identity(id, e);
}

/* ================================================================= */
/* Feature posture                                                    */
/* ================================================================= */

TEST_CASE("exec: boot action fails closed for every flag combination", "[pool_exec]")
{
    PoolExecBootAction a;

    a = pool_exec_boot_action_for_features(false, false);
    TEST_ASSERT_FALSE(a.executor_permitted);
    TEST_ASSERT_FALSE(a.actions_permitted);

    a = pool_exec_boot_action_for_features(true, false);
    TEST_ASSERT_FALSE(a.executor_permitted); /* B6 hold-only */
    TEST_ASSERT_FALSE(a.actions_permitted);

    a = pool_exec_boot_action_for_features(false, true);
    TEST_ASSERT_FALSE(a.executor_permitted); /* unbuildable combo fails closed */
    TEST_ASSERT_FALSE(a.actions_permitted);

    a = pool_exec_boot_action_for_features(true, true);
    TEST_ASSERT_TRUE(a.executor_permitted);
    TEST_ASSERT_TRUE(a.actions_permitted);
}

/* ================================================================= */
/* Substate contract map                                              */
/* ================================================================= */

TEST_CASE("exec: substate contract map is total and fail-closed", "[pool_exec]")
{
    for (int s = 0; s < (int)POOL_EXEC_STATE__COUNT + 3; s++) {
        PoolExecState st = (PoolExecState)s;
        PoolExecActionClass  a = pool_exec_state_action(st);
        PoolExecProtoPosture p = pool_exec_state_proto_posture(st);
        PoolExecGatePosture  g = pool_exec_state_gate(st);
        PoolExecOwnerReq     o = pool_exec_state_owner_req(st);
        TEST_ASSERT_TRUE(a < POOL_EXEC_ACTION__COUNT);
        TEST_ASSERT_TRUE(p < POOL_EXEC_PROTO_POSTURE__COUNT);
        TEST_ASSERT_TRUE(g < POOL_EXEC_GATE__COUNT);
        TEST_ASSERT_TRUE(o < POOL_EXEC_OWNER_REQ__COUNT);
        if (s >= (int)POOL_EXEC_STATE__COUNT) {
            /* Out-of-range substates get the fail-closed row. */
            TEST_ASSERT_EQUAL(EXEC_ACTION_NONE, a);
            TEST_ASSERT_EQUAL(EXEC_PROTO_POSTURE_STOPPED, p);
            TEST_ASSERT_EQUAL(EXEC_GATE_INHIBITED, g);
        }
    }
    /* The only gate-opening substates are the two verified postures plus
     * the no-epoch defaults. */
    TEST_ASSERT_EQUAL(EXEC_GATE_OPEN_TARGET, pool_exec_state_gate(EXEC_STATE_TARGET_MINING));
    TEST_ASSERT_EQUAL(EXEC_GATE_OPEN_SOURCE_RESTORED,
                      pool_exec_state_gate(EXEC_STATE_SOURCE_MINING_VERIFYING));
    TEST_ASSERT_EQUAL(EXEC_GATE_DEFAULT_OPEN, pool_exec_state_gate(EXEC_STATE_IDLE));
    TEST_ASSERT_EQUAL(EXEC_GATE_DEFAULT_OPEN, pool_exec_state_gate(EXEC_STATE_DONE));
    TEST_ASSERT_EQUAL(EXEC_GATE_INHIBITED,
                      pool_exec_state_gate(EXEC_STATE_TARGET_CONNECTING));
    TEST_ASSERT_EQUAL(EXEC_GATE_INHIBITED,
                      pool_exec_state_gate(EXEC_STATE_RESTORE_FAILED_HELD));
}

TEST_CASE("exec: B1 compatibility rows accept only their spans", "[pool_exec]")
{
    TEST_ASSERT_TRUE(pool_exec_state_b1_compatible(EXEC_STATE_TARGET_READBACK,
                                                   POOL_STATE_VERIFYING_TARGET));
    TEST_ASSERT_FALSE(pool_exec_state_b1_compatible(EXEC_STATE_TARGET_READBACK,
                                                    POOL_STATE_APPLYING_TARGET));
    TEST_ASSERT_TRUE(pool_exec_state_b1_compatible(EXEC_STATE_TARGET_APPLYING,
                                                   POOL_STATE_APPLYING_TARGET));
    TEST_ASSERT_TRUE(pool_exec_state_b1_compatible(EXEC_STATE_TARGET_MINING,
                                                   POOL_STATE_TARGET_ACTIVE));
    TEST_ASSERT_FALSE(pool_exec_state_b1_compatible(EXEC_STATE_TARGET_MINING,
                                                    POOL_STATE_VERIFYING_TARGET));
    TEST_ASSERT_TRUE(pool_exec_state_b1_compatible(EXEC_STATE_SOURCE_APPLYING,
                                                   POOL_STATE_APPLYING_RESTORE));
    TEST_ASSERT_TRUE(pool_exec_state_b1_compatible(EXEC_STATE_SOURCE_MINING_VERIFYING,
                                                   POOL_STATE_VERIFYING_RESTORE));
    TEST_ASSERT_TRUE(pool_exec_state_b1_compatible(EXEC_STATE_RESTORE_FAILED_HELD,
                                                   POOL_STATE_RESTORE_FAILED));
    TEST_ASSERT_FALSE(pool_exec_state_b1_compatible(EXEC_STATE_RESTORE_FAILED_HELD,
                                                    POOL_STATE_COMPLETE));
    /* Out-of-range values are never compatible. */
    TEST_ASSERT_FALSE(pool_exec_state_b1_compatible(EXEC_STATE_TARGET_MINING,
                                                    (PoolSessionState)99));
    TEST_ASSERT_FALSE(pool_exec_state_b1_compatible((PoolExecState)99,
                                                    POOL_STATE_IDLE));
}

TEST_CASE("exec: ownership compatibility is fail-closed", "[pool_exec]")
{
    /* Target-side work rides session-class owners on verification phases. */
    TEST_ASSERT_TRUE(pool_exec_state_ownership_compatible(
        EXEC_STATE_TARGET_CONNECTING, OP_OWNER_TIMED_SESSION, OP_PHASE_VERIFYING_TARGET));
    TEST_ASSERT_TRUE(pool_exec_state_ownership_compatible(
        EXEC_STATE_TARGET_MINING, OP_OWNER_BOOT_RECOVERY, OP_PHASE_ACTIVE));
    TEST_ASSERT_FALSE(pool_exec_state_ownership_compatible(
        EXEC_STATE_TARGET_MINING, OP_OWNER_SOURCE_RESTORE, OP_PHASE_RESTORING_SOURCE));
    TEST_ASSERT_FALSE(pool_exec_state_ownership_compatible(
        EXEC_STATE_TARGET_MINING, OP_OWNER_NONE, OP_PHASE_FREE));
    /* Restore-side work accepts SOURCE_RESTORE and session-class owners. */
    TEST_ASSERT_TRUE(pool_exec_state_ownership_compatible(
        EXEC_STATE_SOURCE_APPLYING, OP_OWNER_SOURCE_RESTORE, OP_PHASE_RESTORING_SOURCE));
    TEST_ASSERT_TRUE(pool_exec_state_ownership_compatible(
        EXEC_STATE_SOURCE_APPLYING, OP_OWNER_TIMED_SESSION, OP_PHASE_RESTORING_SOURCE));
    TEST_ASSERT_FALSE(pool_exec_state_ownership_compatible(
        EXEC_STATE_SOURCE_APPLYING, OP_OWNER_MANUAL_POOL_PATCH, OP_PHASE_ACTIVE));
    /* The COMPLETE handoff tail runs on the released FREE lease. */
    TEST_ASSERT_TRUE(pool_exec_state_ownership_compatible(
        EXEC_STATE_COMPLETE_HANDOFF, OP_OWNER_NONE, OP_PHASE_FREE));
    /* Unknown enum values are never compatible. */
    TEST_ASSERT_FALSE(pool_exec_state_ownership_compatible(
        EXEC_STATE_TARGET_MINING, (PoolOperationOwner)77, OP_PHASE_ACTIVE));
    TEST_ASSERT_FALSE(pool_exec_state_ownership_compatible(
        EXEC_STATE_TARGET_MINING, OP_OWNER_TIMED_SESSION, (PoolOperationLeasePhase)77));
}

TEST_CASE("exec: owns-flow and mutation classification", "[pool_exec]")
{
    TEST_ASSERT_FALSE(pool_exec_state_owns_flow(EXEC_STATE_DISABLED));
    TEST_ASSERT_FALSE(pool_exec_state_owns_flow(EXEC_STATE_IDLE));
    TEST_ASSERT_FALSE(pool_exec_state_owns_flow(EXEC_STATE_DONE));
    TEST_ASSERT_TRUE(pool_exec_state_owns_flow(EXEC_STATE_TARGET_MINING));
    TEST_ASSERT_TRUE(pool_exec_state_owns_flow(EXEC_STATE_RESTORE_FAILED_HELD));
    TEST_ASSERT_FALSE(pool_exec_state_owns_flow((PoolExecState)99));

    TEST_ASSERT_TRUE(pool_exec_state_permits_mutation(EXEC_STATE_TARGET_APPLYING));
    TEST_ASSERT_TRUE(pool_exec_state_permits_mutation(EXEC_STATE_RESTORE_PENDING));
    TEST_ASSERT_FALSE(pool_exec_state_permits_mutation(EXEC_STATE_TARGET_CONNECTING));
    TEST_ASSERT_FALSE(pool_exec_state_permits_mutation(EXEC_STATE_RECOVERY_GUARD));
    TEST_ASSERT_FALSE(pool_exec_state_permits_mutation(EXEC_STATE_RESTORE_FAILED_HELD));
}

/* ================================================================= */
/* Board / ASIC compatibility                                         */
/* ================================================================= */

TEST_CASE("exec: board and ASIC support is exact and fail-closed", "[pool_exec]")
{
    TEST_ASSERT_TRUE(pool_exec_board_supported("601"));
    TEST_ASSERT_TRUE(pool_exec_asic_supported("BM1370"));
    TEST_ASSERT_FALSE(pool_exec_board_supported("602"));
    TEST_ASSERT_FALSE(pool_exec_board_supported("60"));
    TEST_ASSERT_FALSE(pool_exec_board_supported("6011"));
    TEST_ASSERT_FALSE(pool_exec_board_supported(""));
    TEST_ASSERT_FALSE(pool_exec_board_supported(NULL));
    TEST_ASSERT_FALSE(pool_exec_asic_supported("BM1366"));
    TEST_ASSERT_FALSE(pool_exec_asic_supported("BM1370XP"));
    TEST_ASSERT_FALSE(pool_exec_asic_supported(""));
    TEST_ASSERT_FALSE(pool_exec_asic_supported(NULL));
}

/* ================================================================= */
/* Effective configuration and exact comparison                       */
/* ================================================================= */

TEST_CASE("exec: desired form canonicalizes fallback and role", "[pool_exec]")
{
    PoolConfigIdentity id;
    PoolExecEffectiveConfig d;
    PoolEndpoint zero;

    memset(&zero, 0, sizeof(zero));

    fill_identity(&id, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    id.fallback_enabled = false;
    /* Stale fallback bytes must never reach the canonical desired form. */
    strncpy(id.fallback.host, "stale.example", sizeof(id.fallback.host) - 1);
    id.fallback.port = 9;
    pool_exec_desired_from_identity(&id, &d);
    TEST_ASSERT_TRUE(d.valid);
    TEST_ASSERT_FALSE(d.use_fallback);
    TEST_ASSERT_TRUE(pool_endpoint_equal(&d.primary, &id.primary));
    TEST_ASSERT_TRUE(pool_endpoint_equal(&d.fallback, &zero));

    fill_identity_with_fallback(&id);
    pool_exec_desired_from_identity(&id, &d);
    TEST_ASSERT_TRUE(pool_endpoint_equal(&d.fallback, &id.fallback));
    TEST_ASSERT_FALSE(d.use_fallback);

    pool_exec_desired_from_identity(NULL, &d);
    TEST_ASSERT_FALSE(d.valid); /* invalid: matches nothing */
}

TEST_CASE("exec: effective-vs-identity comparison flags every field", "[pool_exec]")
{
    PoolConfigIdentity id;
    PoolExecEffectiveConfig e;
    uint32_t mask;

    fill_identity_with_fallback(&id);
    effective_from_identity(&e, &id);
    TEST_ASSERT_TRUE(pool_exec_effective_matches_identity(&e, &id, &mask));
    TEST_ASSERT_EQUAL_UINT32(0u, mask);

    effective_from_identity(&e, &id);
    e.primary.host[0] = 'x';
    TEST_ASSERT_FALSE(pool_exec_effective_matches_identity(&e, &id, &mask));
    TEST_ASSERT_EQUAL_UINT32(EXEC_MISMATCH_PRIMARY_HOST, mask);

    effective_from_identity(&e, &id);
    e.primary.port++;
    TEST_ASSERT_FALSE(pool_exec_effective_matches_identity(&e, &id, &mask));
    TEST_ASSERT_EQUAL_UINT32(EXEC_MISMATCH_PRIMARY_PORT, mask);

    effective_from_identity(&e, &id);
    e.primary.user[0] = 'z';
    TEST_ASSERT_FALSE(pool_exec_effective_matches_identity(&e, &id, &mask));
    TEST_ASSERT_EQUAL_UINT32(EXEC_MISMATCH_PRIMARY_USER, mask);

    effective_from_identity(&e, &id);
    e.primary.protocol = POOL_PROTO_STRATUM_V2;
    TEST_ASSERT_FALSE(pool_exec_effective_matches_identity(&e, &id, &mask));
    TEST_ASSERT_EQUAL_UINT32(EXEC_MISMATCH_PRIMARY_PROTOCOL, mask);

    effective_from_identity(&e, &id);
    e.primary.tls = !e.primary.tls;
    TEST_ASSERT_FALSE(pool_exec_effective_matches_identity(&e, &id, &mask));
    TEST_ASSERT_EQUAL_UINT32(EXEC_MISMATCH_PRIMARY_TLS, mask);

    effective_from_identity(&e, &id);
    e.fallback.port++;
    TEST_ASSERT_FALSE(pool_exec_effective_matches_identity(&e, &id, &mask));
    TEST_ASSERT_EQUAL_UINT32(EXEC_MISMATCH_FALLBACK_PORT, mask);

    effective_from_identity(&e, &id);
    e.use_fallback = true;
    TEST_ASSERT_FALSE(pool_exec_effective_matches_identity(&e, &id, &mask));
    TEST_ASSERT_EQUAL_UINT32(EXEC_MISMATCH_ROLE, mask);

    effective_from_identity(&e, &id);
    e.valid = false;
    TEST_ASSERT_FALSE(pool_exec_effective_matches_identity(&e, &id, &mask));
    TEST_ASSERT_EQUAL_UINT32(EXEC_MISMATCH_READBACK, mask);

    TEST_ASSERT_FALSE(pool_exec_effective_matches_identity(NULL, &id, &mask));
    TEST_ASSERT_FALSE(pool_exec_effective_matches_identity(&e, NULL, &mask));
}

TEST_CASE("exec: writes-needed counts exactly the differing fields", "[pool_exec]")
{
    PoolConfigIdentity id;
    PoolExecEffectiveConfig e;

    fill_identity(&id, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    effective_from_identity(&e, &id);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_exec_writes_needed(&e, &id));

    e.primary.port++;
    strncpy(e.primary.host, "other.example", sizeof(e.primary.host) - 1);
    TEST_ASSERT_EQUAL_UINT32(2u, pool_exec_writes_needed(&e, &id));

    e.valid = false;
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, pool_exec_writes_needed(&e, &id));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, pool_exec_writes_needed(NULL, &id));
}

/* ================================================================= */
/* Configuration transaction classification                           */
/* ================================================================= */

TEST_CASE("exec: apply classification covers every verdict", "[pool_exec]")
{
    PoolConfigIdentity target;
    PoolExecEffectiveConfig pre, now;

    fill_identity(&target, POOL_CHAIN_BITCOIN_CASH, "bch.example", 3334, "acct.worker");
    /* pre = a DIFFERENT source configuration. */
    PoolConfigIdentity source;
    fill_identity(&source, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    effective_from_identity(&pre, &source);

    /* Nothing landed yet: pending inside the window, UNCERTAIN at timeout
     * (an asynchronous writer may still mutate later). */
    now = pre;
    TEST_ASSERT_EQUAL(EXEC_CONFIG_PENDING,
                      pool_exec_classify_apply(&pre, &target, &now, false, false));
    TEST_ASSERT_EQUAL(EXEC_CONFIG_APPLY_UNCERTAIN,
                      pool_exec_classify_apply(&pre, &target, &now, false, true));

    /* A definite pre-enqueue rejection with the pre state intact is a clean
     * no-mutation verdict. */
    TEST_ASSERT_EQUAL(EXEC_CONFIG_APPLY_NO_MUTATION,
                      pool_exec_classify_apply(&pre, &target, &now, true, false));

    /* Exact landing. */
    effective_from_identity(&now, &target);
    TEST_ASSERT_EQUAL(EXEC_CONFIG_APPLY_EXACT,
                      pool_exec_classify_apply(&pre, &target, &now, false, false));

    /* Already the target before staging: NO_MUTATION success. */
    TEST_ASSERT_EQUAL(EXEC_CONFIG_APPLY_NO_MUTATION,
                      pool_exec_classify_apply(&now, &target, &now, false, false));

    /* Partial: the port landed, the host still reads EXACTLY the pre value
     * (never a foreign one). */
    effective_from_identity(&now, &target);
    memcpy(now.primary.host, pre.primary.host, sizeof(now.primary.host));
    TEST_ASSERT_EQUAL(EXEC_CONFIG_PENDING,
                      pool_exec_classify_apply(&pre, &target, &now, false, false));
    TEST_ASSERT_EQUAL(EXEC_CONFIG_APPLY_PARTIAL,
                      pool_exec_classify_apply(&pre, &target, &now, false, true));

    /* Foreign value (neither pre nor desired) is an immediate mismatch. */
    effective_from_identity(&now, &target);
    now.primary.port = 9999;
    TEST_ASSERT_EQUAL(EXEC_CONFIG_APPLY_READBACK_MISMATCH,
                      pool_exec_classify_apply(&pre, &target, &now, false, false));

    /* Readback failure: pending, then UNCERTAIN. */
    memset(&now, 0, sizeof(now));
    TEST_ASSERT_EQUAL(EXEC_CONFIG_PENDING,
                      pool_exec_classify_apply(&pre, &target, &now, false, false));
    TEST_ASSERT_EQUAL(EXEC_CONFIG_APPLY_UNCERTAIN,
                      pool_exec_classify_apply(&pre, &target, &now, false, true));

    /* An invalid PRE frame can never prove anything. */
    {
        PoolExecEffectiveConfig bad = pre;
        bad.valid = false;
        effective_from_identity(&now, &target);
        TEST_ASSERT_EQUAL(EXEC_CONFIG_APPLY_UNCERTAIN,
                          pool_exec_classify_apply(&bad, &target, &now, false, false));
    }
    TEST_ASSERT_EQUAL(EXEC_CONFIG_APPLY_UNCERTAIN,
                      pool_exec_classify_apply(NULL, &target, &now, false, false));
}

TEST_CASE("exec: classification helpers are total", "[pool_exec]")
{
    TEST_ASSERT_FALSE(pool_exec_apply_result_final(EXEC_CONFIG_PENDING));
    TEST_ASSERT_TRUE(pool_exec_apply_result_final(EXEC_CONFIG_APPLY_EXACT));
    TEST_ASSERT_TRUE(pool_exec_apply_result_final(EXEC_CONFIG_APPLY_READBACK_MISMATCH));
    TEST_ASSERT_FALSE(pool_exec_apply_result_final((PoolExecConfigApplyResult)99));

    TEST_ASSERT_FALSE(pool_exec_apply_result_uncertain_mutation(EXEC_CONFIG_APPLY_EXACT));
    TEST_ASSERT_FALSE(pool_exec_apply_result_uncertain_mutation(EXEC_CONFIG_APPLY_NO_MUTATION));
    TEST_ASSERT_TRUE(pool_exec_apply_result_uncertain_mutation(EXEC_CONFIG_APPLY_PARTIAL));
    TEST_ASSERT_TRUE(pool_exec_apply_result_uncertain_mutation(EXEC_CONFIG_APPLY_UNCERTAIN));
    TEST_ASSERT_TRUE(pool_exec_apply_result_uncertain_mutation(EXEC_CONFIG_APPLY_READBACK_MISMATCH));
}

TEST_CASE("exec: classification is deterministic (byte-identical repeats)", "[pool_exec]")
{
    PoolConfigIdentity target, source;
    PoolExecEffectiveConfig pre, now;

    fill_identity(&target, POOL_CHAIN_BITCOIN_CASH, "bch.example", 3334, "a.w");
    fill_identity(&source, POOL_CHAIN_BITCOIN, "btc.example", 3333, "a.w");
    effective_from_identity(&pre, &source);
    effective_from_identity(&now, &target);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(EXEC_CONFIG_APPLY_EXACT,
                          pool_exec_classify_apply(&pre, &target, &now, false, false));
    }
}

/* ================================================================= */
/* Protocol evidence                                                  */
/* ================================================================= */

TEST_CASE("exec: event sanitization drops unknown bits", "[pool_exec]")
{
    TEST_ASSERT_EQUAL_UINT32(0u, pool_exec_protocol_events_sanitize(0xFFFFFFF0u));
    TEST_ASSERT_EQUAL_UINT32(EXEC_PEVT_SETUP_SUCCESS,
                             pool_exec_protocol_events_sanitize(0xFFFFFFF0u |
                                                                EXEC_PEVT_SETUP_SUCCESS));
}

TEST_CASE("exec: evidence is generation-bound and rejects regression", "[pool_exec]")
{
    PoolExecProtocolCounters base, now;
    PoolExecEvidence ev;

    memset(&base, 0, sizeof(base));
    memset(&now, 0, sizeof(now));

    /* No movement, no events: nothing is evidence. */
    pool_exec_evaluate_evidence(&base, &now, 0u, &ev);
    TEST_ASSERT_FALSE(ev.anomaly);
    TEST_ASSERT_FALSE(ev.connection_evidence);
    TEST_ASSERT_FALSE(ev.job_evidence);
    TEST_ASSERT_FALSE(ev.forward_evidence);
    TEST_ASSERT_FALSE(ev.asic_processing_evidence);

    /* Setup success alone is connection evidence, never job evidence. */
    pool_exec_evaluate_evidence(&base, &now, EXEC_PEVT_SETUP_SUCCESS, &ev);
    TEST_ASSERT_TRUE(ev.connection_evidence);
    TEST_ASSERT_FALSE(ev.job_evidence);

    /* One parse-accepted job is both. */
    now.work_received = 1u;
    pool_exec_evaluate_evidence(&base, &now, 0u, &ev);
    TEST_ASSERT_TRUE(ev.connection_evidence);
    TEST_ASSERT_TRUE(ev.job_evidence);
    TEST_ASSERT_EQUAL_UINT64(1u, ev.job_delta);

    /* Forwarded jobs count separately. */
    now.jobs_forwarded = 2u;
    pool_exec_evaluate_evidence(&base, &now, 0u, &ev);
    TEST_ASSERT_TRUE(ev.forward_evidence);
    TEST_ASSERT_EQUAL_UINT64(2u, ev.forward_delta);

    /* Counter regression is an anomaly: NOTHING is evidence. */
    base.work_received = 10u;
    now.work_received  = 3u;
    pool_exec_evaluate_evidence(&base, &now, EXEC_PEVT_SETUP_SUCCESS, &ev);
    TEST_ASSERT_TRUE(ev.anomaly);
    TEST_ASSERT_FALSE(ev.connection_evidence);
    TEST_ASSERT_FALSE(ev.job_evidence);

    /* An ASIC-counter regression is equally an anomaly: it means the
     * baseline belongs to an OLDER work generation. */
    memset(&base, 0, sizeof(base));
    memset(&now, 0, sizeof(now));
    base.asic_job_results = 7u;
    now.asic_job_results  = 2u;
    pool_exec_evaluate_evidence(&base, &now, 0u, &ev);
    TEST_ASSERT_TRUE(ev.anomaly);
    TEST_ASSERT_FALSE(ev.asic_processing_evidence);

    /* A missing frame of reference is an anomaly too. */
    pool_exec_evaluate_evidence(NULL, &now, 0u, &ev);
    TEST_ASSERT_TRUE(ev.anomaly);
}

/* ---------- ASIC-side processing evidence (Blocker 3) ---------- */

TEST_CASE("exec: software delivery alone is NEVER mining-verified", "[pool_exec]")
{
    PoolExecProtocolCounters base, now;
    PoolExecEvidence ev;

    memset(&base, 0, sizeof(base));
    memset(&now, 0, sizeof(now));

    /* The pool served work and the delivery gate released jobs toward the
     * ASIC — but the chip returned nothing. That is NOT proof of ASIC
     * processing and must never satisfy a mining verification. */
    now.work_received  = 5u;
    now.jobs_forwarded = 5u;
    pool_exec_evaluate_evidence(&base, &now, EXEC_PEVT_SETUP_SUCCESS, &ev);
    TEST_ASSERT_TRUE(ev.job_evidence);
    TEST_ASSERT_TRUE(ev.forward_evidence);
    TEST_ASSERT_FALSE(ev.asic_processing_evidence);
    TEST_ASSERT_FALSE(pool_exec_evidence_mining_verified(&ev));

    /* Register reads prove chip liveness but not job processing. */
    now.asic_register_reads = 40u;
    pool_exec_evaluate_evidence(&base, &now, 0u, &ev);
    TEST_ASSERT_FALSE(ev.asic_processing_evidence);
    TEST_ASSERT_FALSE(pool_exec_evidence_mining_verified(&ev));

    /* The required number of DISTINCT job-bound ASIC results completes it. */
    now.asic_job_results = (uint64_t)POOL_EXEC_REQUIRED_WORK_PROOFS - 1u;
    pool_exec_evaluate_evidence(&base, &now, 0u, &ev);
    TEST_ASSERT_FALSE(pool_exec_evidence_mining_verified(&ev)); /* one short */
    now.asic_job_results = (uint64_t)POOL_EXEC_REQUIRED_WORK_PROOFS;
    pool_exec_evaluate_evidence(&base, &now, 0u, &ev);
    TEST_ASSERT_TRUE(ev.asic_processing_evidence);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)POOL_EXEC_REQUIRED_WORK_PROOFS,
                             ev.asic_result_delta);
    TEST_ASSERT_TRUE(pool_exec_evidence_mining_verified(&ev));
}

TEST_CASE("exec: mining-verified requires all three facts together", "[pool_exec]")
{
    PoolExecProtocolCounters base, now;
    PoolExecEvidence ev;

    memset(&base, 0, sizeof(base));

    /* ASIC result without pool work / delivery this generation: rejected. */
    memset(&now, 0, sizeof(now));
    now.asic_job_results = (uint64_t)POOL_EXEC_REQUIRED_WORK_PROOFS + 1u;
    pool_exec_evaluate_evidence(&base, &now, 0u, &ev);
    TEST_ASSERT_TRUE(ev.asic_processing_evidence);
    TEST_ASSERT_FALSE(pool_exec_evidence_mining_verified(&ev));

    /* Pool work + ASIC result but nothing released through the gate. */
    memset(&now, 0, sizeof(now));
    now.work_received    = 2u;
    now.asic_job_results = (uint64_t)POOL_EXEC_REQUIRED_WORK_PROOFS;
    pool_exec_evaluate_evidence(&base, &now, 0u, &ev);
    TEST_ASSERT_FALSE(pool_exec_evidence_mining_verified(&ev));

    /* An anomalous verdict is never mining-verified. */
    memset(&ev, 0, sizeof(ev));
    ev.anomaly = true;
    ev.job_evidence = ev.forward_evidence = ev.asic_processing_evidence = true;
    TEST_ASSERT_FALSE(pool_exec_evidence_mining_verified(&ev));
    TEST_ASSERT_FALSE(pool_exec_evidence_mining_verified(NULL));
}

/* ---------- TLS-mode representability (Blocker 2) ---------- */

TEST_CASE("exec: only exactly-restorable TLS modes are representable", "[pool_exec]")
{
    TEST_ASSERT_TRUE(pool_exec_tls_mode_representable(POOL_EXEC_TLS_MODE_DISABLED));
    TEST_ASSERT_TRUE(pool_exec_tls_mode_representable(POOL_EXEC_TLS_MODE_BUNDLED));
    TEST_ASSERT_FALSE(pool_exec_tls_mode_representable(POOL_EXEC_TLS_MODE_CUSTOM));
    TEST_ASSERT_FALSE(pool_exec_tls_mode_representable(3u));
    TEST_ASSERT_FALSE(pool_exec_tls_mode_representable(255u));

    TEST_ASSERT_EQUAL_UINT8(POOL_EXEC_TLS_MODE_DISABLED, pool_exec_tls_mode_for_flag(false));
    TEST_ASSERT_EQUAL_UINT8(POOL_EXEC_TLS_MODE_BUNDLED, pool_exec_tls_mode_for_flag(true));
}

TEST_CASE("exec: the TLS gate rejects custom-cert and inconsistent configs", "[pool_exec]")
{
    PoolConfigIdentity id;
    PoolExecEffectiveConfig e;

    /* disabled -> disabled: supported. */
    fill_identity(&id, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    id.primary.tls = false;
    effective_from_identity(&e, &id);
    TEST_ASSERT_EQUAL(EXEC_TLS_OK, pool_exec_tls_representable(&e));

    /* bundled -> bundled: supported. */
    id.primary.tls = true;
    effective_from_identity(&e, &id);
    TEST_ASSERT_EQUAL_UINT8(POOL_EXEC_TLS_MODE_BUNDLED, e.primary_tls_mode);
    TEST_ASSERT_EQUAL(EXEC_TLS_OK, pool_exec_tls_representable(&e));

    /* A CUSTOM primary can never round-trip through the B1 boolean. */
    e.primary_tls_mode = POOL_EXEC_TLS_MODE_CUSTOM;
    TEST_ASSERT_EQUAL(EXEC_TLS_PRIMARY_UNSUPPORTED, pool_exec_tls_representable(&e));

    /* A CUSTOM fallback is equally rejected. */
    effective_from_identity(&e, &id);
    e.fallback_tls_mode = POOL_EXEC_TLS_MODE_CUSTOM;
    TEST_ASSERT_EQUAL(EXEC_TLS_FALLBACK_UNSUPPORTED, pool_exec_tls_representable(&e));

    /* A stored mode disagreeing with the boolean already lost information. */
    effective_from_identity(&e, &id);
    e.primary.tls = false; /* boolean says DISABLED, stored mode says BUNDLED */
    TEST_ASSERT_EQUAL(EXEC_TLS_PRIMARY_UNSUPPORTED, pool_exec_tls_representable(&e));

    /* An unreadable configuration is never representable (fails closed). */
    memset(&e, 0, sizeof(e));
    TEST_ASSERT_EQUAL(EXEC_TLS_UNREADABLE, pool_exec_tls_representable(&e));
    TEST_ASSERT_EQUAL(EXEC_TLS_UNREADABLE, pool_exec_tls_representable(NULL));
}

TEST_CASE("exec: a session identity can only ever write representable modes", "[pool_exec]")
{
    PoolConfigIdentity id;
    uint8_t p, f;

    fill_identity_with_fallback(&id);
    id.primary.tls  = true;
    id.fallback.tls = false;
    pool_exec_identity_tls_modes(&id, &p, &f);
    TEST_ASSERT_EQUAL_UINT8(POOL_EXEC_TLS_MODE_BUNDLED, p);
    TEST_ASSERT_EQUAL_UINT8(POOL_EXEC_TLS_MODE_DISABLED, f);
    TEST_ASSERT_TRUE(pool_exec_tls_mode_representable(p));
    TEST_ASSERT_TRUE(pool_exec_tls_mode_representable(f));

    /* A DISABLED fallback is canonically empty regardless of its flag. */
    fill_identity(&id, POOL_CHAIN_BITCOIN, "btc.example", 3333, "a.w");
    id.fallback_enabled = false;
    id.fallback.tls     = true;
    pool_exec_identity_tls_modes(&id, &p, &f);
    TEST_ASSERT_EQUAL_UINT8(POOL_EXEC_TLS_MODE_DISABLED, f);

    pool_exec_identity_tls_modes(NULL, &p, &f);
    TEST_ASSERT_EQUAL_UINT8(POOL_EXEC_TLS_MODE_DISABLED, p);
}

TEST_CASE("exec: a mode-only TLS difference is a readback MISMATCH", "[pool_exec]")
{
    PoolConfigIdentity id;
    PoolExecEffectiveConfig e;
    uint32_t mask;

    fill_identity(&id, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    id.primary.tls = true;
    effective_from_identity(&e, &id);
    TEST_ASSERT_TRUE(pool_exec_effective_matches_identity(&e, &id, &mask));

    /* The boolean still "matches" but the stored mode is a custom
     * certificate: this must NEVER be reported as an exact restoration. */
    e.primary_tls_mode = POOL_EXEC_TLS_MODE_CUSTOM;
    TEST_ASSERT_FALSE(pool_exec_effective_matches_identity(&e, &id, &mask));
    TEST_ASSERT_EQUAL_UINT32(EXEC_MISMATCH_PRIMARY_TLS, mask);
}

/* ================================================================= */
/* Target-mining grant and delivery gate                              */
/* ================================================================= */

TEST_CASE("exec: grant issuance requires every non-zero binding", "[pool_exec]")
{
    PoolExecMiningGrant g;

    TEST_ASSERT_FALSE(pool_exec_grant_issue(&g, 0u, 1u, 1u, 1u, 1u));
    TEST_ASSERT_FALSE(g.valid);
    TEST_ASSERT_FALSE(pool_exec_grant_issue(&g, 1u, 0u, 1u, 1u, 1u));
    TEST_ASSERT_FALSE(pool_exec_grant_issue(&g, 1u, 1u, 0u, 1u, 1u));
    TEST_ASSERT_FALSE(pool_exec_grant_issue(&g, 1u, 1u, 1u, 0u, 1u));
    TEST_ASSERT_TRUE(pool_exec_grant_issue(&g, 42u, 3u, 7u, 2u, 1u));
    TEST_ASSERT_TRUE(pool_exec_grant_valid(&g, 42u, 3u, 7u, 2u));
}

TEST_CASE("exec: grant is stale after ANY binding change and revocation is permanent", "[pool_exec]")
{
    PoolExecMiningGrant g;

    TEST_ASSERT_TRUE(pool_exec_grant_issue(&g, 42u, 3u, 7u, 2u, 1u));
    /* Lease rotation, record generation change, protocol generation change,
     * session change: each alone invalidates. */
    TEST_ASSERT_FALSE(pool_exec_grant_valid(&g, 42u, 4u, 7u, 2u));
    TEST_ASSERT_FALSE(pool_exec_grant_valid(&g, 42u, 3u, 8u, 2u));
    TEST_ASSERT_FALSE(pool_exec_grant_valid(&g, 42u, 3u, 7u, 3u));
    TEST_ASSERT_FALSE(pool_exec_grant_valid(&g, 43u, 3u, 7u, 2u));
    TEST_ASSERT_TRUE(pool_exec_grant_valid(&g, 42u, 3u, 7u, 2u));

    pool_exec_grant_revoke(&g);
    TEST_ASSERT_FALSE(pool_exec_grant_valid(&g, 42u, 3u, 7u, 2u));
    pool_exec_grant_revoke(&g); /* idempotent */
    TEST_ASSERT_FALSE(pool_exec_grant_valid(&g, 42u, 3u, 7u, 2u));

    /* A zeroed (reboot-fresh) grant is never valid. */
    pool_exec_grant_init(&g);
    TEST_ASSERT_FALSE(pool_exec_grant_valid(&g, 42u, 3u, 7u, 2u));
    TEST_ASSERT_FALSE(pool_exec_grant_valid(NULL, 42u, 3u, 7u, 2u));
}

TEST_CASE("exec: the delivery-gate rule releases only proven postures", "[pool_exec]")
{
    TEST_ASSERT_TRUE(pool_exec_gate_allows(EXEC_GATE_DEFAULT_OPEN, false));
    TEST_ASSERT_FALSE(pool_exec_gate_allows(EXEC_GATE_INHIBITED, true));
    TEST_ASSERT_FALSE(pool_exec_gate_allows(EXEC_GATE_OPEN_TARGET, false));
    TEST_ASSERT_TRUE(pool_exec_gate_allows(EXEC_GATE_OPEN_TARGET, true));
    TEST_ASSERT_TRUE(pool_exec_gate_allows(EXEC_GATE_OPEN_SOURCE_RESTORED, false));
    TEST_ASSERT_FALSE(pool_exec_gate_allows((PoolExecGatePosture)99, true));
}

/* ================================================================= */
/* Policy and deadlines                                               */
/* ================================================================= */

TEST_CASE("exec: policy defaults and clamping are bounded", "[pool_exec]")
{
    PoolExecPolicy p;

    pool_exec_policy_defaults(&p);
    TEST_ASSERT_EQUAL_UINT32(EXEC_POLICY_CONFIG_TIMEOUT_DEFAULT_S, p.config_timeout_s);
    TEST_ASSERT_EQUAL_UINT32(EXEC_POLICY_JOB_TIMEOUT_DEFAULT_S, p.job_timeout_s);

    memset(&p, 0, sizeof(p));
    pool_exec_policy_clamp(&p);
    TEST_ASSERT_EQUAL_UINT32(EXEC_POLICY_TIMEOUT_MIN_S, p.config_timeout_s);
    TEST_ASSERT_TRUE(p.job_timeout_s >= p.connect_timeout_s);

    memset(&p, 0xFF, sizeof(p));
    pool_exec_policy_clamp(&p);
    TEST_ASSERT_EQUAL_UINT32(EXEC_POLICY_TIMEOUT_MAX_S, p.config_timeout_s);
    TEST_ASSERT_EQUAL_UINT8(EXEC_POLICY_STOP_RETRY_MAX, p.stop_retry_max);
}

TEST_CASE("exec: monotonic deadlines saturate and never wrap", "[pool_exec]")
{
    TEST_ASSERT_EQUAL_UINT64(5000000ull, pool_exec_deadline_us(0u, 5u));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX,
                             pool_exec_deadline_us(UINT64_MAX - 10u, 5u));
    TEST_ASSERT_FALSE(pool_exec_deadline_reached(4999999ull, 5000000ull));
    TEST_ASSERT_TRUE(pool_exec_deadline_reached(5000000ull, 5000000ull));
}

/* ================================================================= */
/* Record staging and exact transition readback                       */
/* ================================================================= */

TEST_CASE("exec: staging preserves time facts and counters exactly", "[pool_exec]")
{
    PoolSessionRecord committed, staged;
    PoolSession s;

    make_record(&committed, POOL_STATE_TARGET_ACTIVE, true);
    committed.reboot_count = 3u;
    committed.recovery_attempt_count = 1u;
    committed.consecutive_recovery_failures = 2u;
    committed.last_reset_class = POOL_RECORD_RESET_CLASS_CLEAN;
    committed.latest_trusted_valid = true;
    committed.latest_trusted_epoch_s = EPOCH_A_S + 100u;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&committed));

    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_to_session(&committed, &s));
    /* A same-state restage must be field-identical (ignoring generation). */
    TEST_ASSERT_EQUAL(RECORD_OK, pool_exec_stage_record(&s, &committed, &staged));
    TEST_ASSERT_TRUE(pool_exec_record_equal_exact(&staged, &committed, true));
    TEST_ASSERT_EQUAL_UINT64(committed.deadline_epoch_s, staged.deadline_epoch_s);
    TEST_ASSERT_EQUAL_UINT8(3u, staged.reboot_count);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 100u, staged.latest_trusted_epoch_s);

    /* Non-persistent states are never stageable. */
    s.state = POOL_STATE_VERIFYING_TARGET;
    TEST_ASSERT_NOT_EQUAL(RECORD_OK, pool_exec_stage_record(&s, &committed, &staged));

    TEST_ASSERT_EQUAL(RECORD_ERR_INVALID_ARGUMENT,
                      pool_exec_stage_record(NULL, &committed, &staged));
}

TEST_CASE("exec: exact record equality flags every field", "[pool_exec]")
{
    PoolSessionRecord a, b;

    make_record(&a, POOL_STATE_TARGET_ACTIVE, true);
    b = a;
    TEST_ASSERT_TRUE(pool_exec_record_equal_exact(&a, &b, false));

    b = a; b.state = POOL_STATE_RESTORE_DUE;
    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(&a, &b, true));
    b = a; b.restore_required = false;
    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(&a, &b, true));
    b = a; b.target_verify.mining_observed = false;
    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(&a, &b, true));
    b = a; b.retries.restore_apply = 1u;
    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(&a, &b, true));
    b = a; b.deadline_epoch_s += 1u;
    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(&a, &b, true));
    b = a; b.reboot_count += 1u;
    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(&a, &b, true));
    b = a; b.last_failure_code = (uint16_t)ERR_TARGET_APPLY;
    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(&a, &b, true));
    b = a; b.source.primary.port += 1u;
    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(&a, &b, true));
    b = a; strncpy(b.target.profile_id, "other", sizeof(b.target.profile_id) - 1);
    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(&a, &b, true));

    /* Generation participates only when not ignored. */
    b = a; b.generation = a.generation + 5u;
    TEST_ASSERT_TRUE(pool_exec_record_equal_exact(&a, &b, true));
    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(&a, &b, false));

    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(NULL, &b, true));
    TEST_ASSERT_FALSE(pool_exec_record_equal_exact(&a, NULL, true));
}

TEST_CASE("exec: transition readback proof accepts only an exact newer landing", "[pool_exec]")
{
    PoolSessionRecord staged, reloaded;

    make_record(&staged, POOL_STATE_RESTORE_DUE, true);
    reloaded = staged;
    reloaded.generation = 5u;

    TEST_ASSERT_EQUAL(EXEC_REASON_NONE,
                      pool_exec_verify_transition_readback(&staged, &reloaded, 4u,
                                                           true, STORE_OK));
    /* Reload failure. */
    TEST_ASSERT_EQUAL(EXEC_REASON_PERSIST_READBACK,
                      pool_exec_verify_transition_readback(&staged, &reloaded, 4u,
                                                           true, STORE_IO_ERROR));
    /* Field mismatch. */
    reloaded.retries.restore_apply = 2u;
    TEST_ASSERT_EQUAL(EXEC_REASON_PERSIST_READBACK,
                      pool_exec_verify_transition_readback(&staged, &reloaded, 4u,
                                                           true, STORE_OK));
    reloaded = staged;
    reloaded.generation = 4u; /* not strictly newer */
    TEST_ASSERT_EQUAL(EXEC_REASON_PERSIST_READBACK,
                      pool_exec_verify_transition_readback(&staged, &reloaded, 4u,
                                                           true, STORE_OK));

    /* Obligation monotonicity: a non-COMPLETE landing may never read the
     * obligation as discharged. */
    make_record(&staged, POOL_STATE_CANCELLED, false);
    reloaded = staged;
    reloaded.generation = 9u;
    TEST_ASSERT_EQUAL(EXEC_REASON_PERSIST_READBACK,
                      pool_exec_verify_transition_readback(&staged, &reloaded, 8u,
                                                           true, STORE_OK));
    /* COMPLETE is the one legitimate discharge. */
    make_record(&staged, POOL_STATE_COMPLETE, false);
    reloaded = staged;
    reloaded.generation = 9u;
    TEST_ASSERT_EQUAL(EXEC_REASON_NONE,
                      pool_exec_verify_transition_readback(&staged, &reloaded, 8u,
                                                           true, STORE_OK));
    /* A never-incurred obligation may stay false pre-mutation. */
    make_record(&staged, POOL_STATE_CANCELLED, false);
    reloaded = staged;
    reloaded.generation = 2u;
    TEST_ASSERT_EQUAL(EXEC_REASON_NONE,
                      pool_exec_verify_transition_readback(&staged, &reloaded, 1u,
                                                           false, STORE_OK));

    TEST_ASSERT_EQUAL(EXEC_REASON_INTERNAL,
                      pool_exec_verify_transition_readback(NULL, &reloaded, 1u,
                                                           false, STORE_OK));
}

/* ================================================================= */
/* Sanitized snapshot                                                 */
/* ================================================================= */

TEST_CASE("exec: snapshot init/validation enforce the gate and grant rules", "[pool_exec]")
{
    PoolExecutionSnapshot s;

    pool_exec_snapshot_init(&s);
    TEST_ASSERT_TRUE(pool_exec_snapshot_valid(&s));
    TEST_ASSERT_EQUAL(EXEC_STATE_DISABLED, s.state);
    TEST_ASSERT_EQUAL(EXEC_GATE_DEFAULT_OPEN, s.gate);

    /* Gate must match the substate contract. */
    s.state = EXEC_STATE_TARGET_CONNECTING;
    s.gate  = EXEC_GATE_DEFAULT_OPEN;
    TEST_ASSERT_FALSE(pool_exec_snapshot_valid(&s));
    s.gate = EXEC_GATE_INHIBITED;
    TEST_ASSERT_TRUE(pool_exec_snapshot_valid(&s));

    /* OPEN_TARGET requires the grant flag; a grant flag outside the mining
     * posture is inconsistent. */
    s.state = EXEC_STATE_TARGET_MINING;
    s.gate  = EXEC_GATE_OPEN_TARGET;
    s.mining_grant_active = false;
    TEST_ASSERT_FALSE(pool_exec_snapshot_valid(&s));
    s.mining_grant_active = true;
    TEST_ASSERT_TRUE(pool_exec_snapshot_valid(&s));
    s.state = EXEC_STATE_TARGET_CONNECTING;
    s.gate  = EXEC_GATE_INHIBITED;
    TEST_ASSERT_FALSE(pool_exec_snapshot_valid(&s)); /* grant outside mining */

    pool_exec_snapshot_init(&s);
    s.model_version = 99u;
    TEST_ASSERT_FALSE(pool_exec_snapshot_valid(&s));
    TEST_ASSERT_FALSE(pool_exec_snapshot_valid(NULL));
}

/* ================================================================= */
/* Stable machine tokens                                              */
/* ================================================================= */

TEST_CASE("exec: machine tokens are stable, unique and dot-free", "[pool_exec]")
{
    for (int i = 0; i < (int)POOL_EXEC_STATE__COUNT; i++) {
        const char *t = pool_exec_state_str((PoolExecState)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_NULL(strchr(t, '.'));
        TEST_ASSERT_NOT_EQUAL(0, strcmp(t, "EXEC_UNKNOWN"));
        for (int j = 0; j < i; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(t, pool_exec_state_str((PoolExecState)j)));
        }
    }
    for (int i = 0; i < (int)POOL_EXEC_REASON__COUNT; i++) {
        const char *t = pool_exec_reason_str((PoolExecReason)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_NULL(strchr(t, '.'));
        for (int j = 0; j < i; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(t, pool_exec_reason_str((PoolExecReason)j)));
        }
    }
    TEST_ASSERT_EQUAL_STRING("EXEC_UNKNOWN", pool_exec_state_str((PoolExecState)99));
    TEST_ASSERT_EQUAL_STRING("EXEC_UNKNOWN", pool_exec_reason_str((PoolExecReason)99));
    TEST_ASSERT_EQUAL_STRING("GATE_UNKNOWN", pool_exec_gate_str((PoolExecGatePosture)99));
    TEST_ASSERT_EQUAL_STRING("CONFIG_UNKNOWN",
                             pool_exec_apply_result_str((PoolExecConfigApplyResult)99));
}

/* ================================================================= */
/* Property-style checks                                              */
/* ================================================================= */

TEST_CASE("exec: property — B1 sessions never expose a password field to staging", "[pool_exec]")
{
    /* The staging input is the bounded B1 session; the B1 model has no
     * password field by design. This check pins the STAGED record to the
     * committed keep-current-only policy explicitly. */
    PoolSessionRecord committed, staged;
    PoolSession s;

    make_record(&committed, POOL_STATE_RESTORE_DUE, true);
    make_session(&s, POOL_STATE_RESTORE_DUE, true);
    TEST_ASSERT_EQUAL(RECORD_OK, pool_exec_stage_record(&s, &committed, &staged));
    TEST_ASSERT_EQUAL(POOL_SESSION_PW_KEEP_CURRENT, staged.password_policy);
}

TEST_CASE("exec: property — no substate both opens the target gate and skips the grant", "[pool_exec]")
{
    for (int s = 0; s < (int)POOL_EXEC_STATE__COUNT; s++) {
        PoolExecGatePosture g = pool_exec_state_gate((PoolExecState)s);
        if (g == EXEC_GATE_OPEN_TARGET) {
            /* The ONLY target-open substate is the mining posture, and the
             * pure gate rule refuses it without a valid grant. */
            TEST_ASSERT_EQUAL(EXEC_STATE_TARGET_MINING, (PoolExecState)s);
            TEST_ASSERT_FALSE(pool_exec_gate_allows(g, false));
        }
    }
}

TEST_CASE("exec: property — every held/guard substate inhibits and stops", "[pool_exec]")
{
    const PoolExecState held[] = {
        EXEC_STATE_RESTORE_FAILED_HELD, EXEC_STATE_RECOVERY_GUARD, EXEC_STATE_ERROR,
    };
    for (unsigned i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
        TEST_ASSERT_EQUAL(EXEC_GATE_INHIBITED, pool_exec_state_gate(held[i]));
        TEST_ASSERT_EQUAL(EXEC_PROTO_POSTURE_STOPPED,
                          pool_exec_state_proto_posture(held[i]));
        TEST_ASSERT_EQUAL(EXEC_ACTION_NONE, pool_exec_state_action(held[i]));
    }
    /* The handoff-failure posture never claims mining either. */
    TEST_ASSERT_EQUAL(EXEC_GATE_INHIBITED, pool_exec_state_gate(EXEC_STATE_HANDOFF_FAILED));
    TEST_ASSERT_EQUAL(EXEC_ACTION_NONE, pool_exec_state_action(EXEC_STATE_HANDOFF_FAILED));
    TEST_ASSERT_FALSE(pool_exec_state_permits_mutation(EXEC_STATE_HANDOFF_FAILED));
}

TEST_CASE("exec: the handoff window never delivers work", "[pool_exec]")
{
    /* Between COMPLETE and a proven handoff the controlled engine is being
     * torn down: no work may be released in either posture. */
    TEST_ASSERT_EQUAL(EXEC_GATE_INHIBITED, pool_exec_state_gate(EXEC_STATE_COMPLETE_HANDOFF));
    TEST_ASSERT_FALSE(pool_exec_gate_allows(
        pool_exec_state_gate(EXEC_STATE_COMPLETE_HANDOFF), true));
    TEST_ASSERT_FALSE(pool_exec_gate_allows(
        pool_exec_state_gate(EXEC_STATE_HANDOFF_FAILED), true));
    /* Only DONE (the production coordinator owns mining again) reopens. */
    TEST_ASSERT_EQUAL(EXEC_GATE_DEFAULT_OPEN, pool_exec_state_gate(EXEC_STATE_DONE));
}

/* ---------- delivered-work registry: per-item generation binding ---------- */

/* Distinct synthetic canonical facts (unique per seq). */
static PoolExecWorkFacts mk_facts(uint32_t seq)
{
    PoolExecWorkFacts f;
    memset(&f, 0, sizeof(f));
    f.version        = 0x20000000u;
    f.ntime          = 0x66000000u + seq;
    f.nbits          = 0x17034219u;
    f.merkle_root[0] = (uint8_t)seq;
    f.merkle_root[1] = (uint8_t)(seq >> 8);
    f.merkle_root[2] = (uint8_t)(seq >> 16);
    f.prev_block_hash[0] = 0x5Au;
    return f;
}

/*
 * Production-shaped DISCRIMINATOR-CARRYING delivery: the tag query runs on
 * the same task immediately before registration (exactly the job-pipeline
 * sequence), so the record is stamped with the extranonce2 domain.
 */
static void deliver_tagged(uint8_t job_id, const PoolExecWorkFacts *f,
                           uint64_t counter)
{
    uint64_t tagged = 0u;
    pool_session_execution_set_work_domain_active(true);
    if (pool_session_execution_generation_tag_current() == 0u) {
        TEST_ASSERT_TRUE(pool_session_execution_allocate_generation_tag());
    }
    (void)pool_session_execution_extranonce2_tag(false, false, 8u, counter, &tagged);
    pool_session_execution_note_work_delivered(job_id, f);
}

/* Stock (no-domain) delivery: what normal mining, SV2 standard channels
 * and too-narrow extranonce2 widths produce. */
static void deliver_untagged(uint8_t job_id, const PoolExecWorkFacts *f)
{
    pool_session_execution_note_work_delivered(job_id, f);
}

/* Forward to the identity fixture defined with the identity tests below. */
static uint32_t publish_identity_fwd(const char *host, const char *user);

TEST_CASE("exec: a result resolves only against its own delivered-work record", "[pool_exec]")
{
    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(7u);
    (void)pool_session_execution_begin_work_generation();

    /* No delivery record: an unknown job can never be evidence. */
    TEST_ASSERT_EQUAL(EXEC_RESULT_UNKNOWN_JOB,
                      pool_session_execution_resolve_asic_result(5u, true));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());

    /* A delivered item resolves exactly once. */
    {
        PoolExecWorkFacts f = mk_facts(1u);
        deliver_tagged(5u, &f, 1u);
    }
    TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                      pool_session_execution_resolve_asic_result(5u, true));
    TEST_ASSERT_EQUAL_UINT64(1u, pool_session_execution_asic_job_results());

    /* The chip keeps returning nonces for the same job: consumed once. */
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(EXEC_RESULT_ALREADY_CONSUMED,
                          pool_session_execution_resolve_asic_result(5u, true));
    }
    TEST_ASSERT_EQUAL_UINT64(1u, pool_session_execution_asic_job_results());
    TEST_ASSERT_EQUAL_UINT64(5u,
        pool_session_execution_asic_rejected(EXEC_RESULT_ALREADY_CONSUMED));

    /* An out-of-range job id is rejected, never counted. */
    TEST_ASSERT_EQUAL(EXEC_RESULT_INVALID,
                      pool_session_execution_resolve_asic_result(200u, true));
}

TEST_CASE("exec: a result from an older WORK generation is rejected", "[pool_exec]")
{
    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();

    {
        PoolExecWorkFacts f = mk_facts(2u);
        deliver_tagged(9u, &f, 1u); /* stamped: work gen N */
    }
    (void)pool_session_execution_begin_work_generation(); /* -> gen N+1 */

    /* The chip answers for the OLD item. The record is RETAINED (it is the
     * next generation's header-comparison set) but its generation stamp is
     * stale, so it can never be credited to the new generation. */
    TEST_ASSERT_EQUAL(EXEC_RESULT_STALE_GENERATION,
                      pool_session_execution_resolve_asic_result(9u, true));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
}

TEST_CASE("exec: a result from an older PROTOCOL generation is rejected", "[pool_exec]")
{
    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(3u);
    (void)pool_session_execution_begin_work_generation();
    {
        PoolExecWorkFacts f = mk_facts(3u);
        deliver_tagged(11u, &f, 1u);
    }

    /* The protocol reconnected (new instance) without a work-generation
     * bump: the stamp no longer matches the current epoch. */
    pool_session_execution_set_protocol_generation(4u);
    TEST_ASSERT_EQUAL(EXEC_RESULT_STALE_GENERATION,
                      pool_session_execution_resolve_asic_result(11u, true));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
    TEST_ASSERT_EQUAL_UINT64(1u,
        pool_session_execution_asic_rejected(EXEC_RESULT_STALE_GENERATION));
}

TEST_CASE("exec: a result from an older CONFIGURATION generation is rejected", "[pool_exec]")
{
    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(2u);
    (void)pool_session_execution_begin_work_generation();
    {
        PoolExecWorkFacts f = mk_facts(4u);
        deliver_tagged(3u, &f, 1u);
    }

    /* A new verified configuration was published: work delivered under the
     * previous one is no longer attributable. */
    (void)pool_session_execution_begin_config_generation();
    TEST_ASSERT_EQUAL(EXEC_RESULT_STALE_GENERATION,
                      pool_session_execution_resolve_asic_result(3u, true));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
}

TEST_CASE("exec: a reused ASIC job slot rebinds to the newest delivery", "[pool_exec]")
{
    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();

    {
        PoolExecWorkFacts f = mk_facts(5u);
        deliver_tagged(2u, &f, 1u);
    }
    TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                      pool_session_execution_resolve_asic_result(2u, true));
    /* The chip reuses slot 2 for a NEW DISTINCT item: the record rebinds
     * and one fresh result is accepted — consumption does not leak across,
     * and same-epoch records are never treated as temporal aliases. */
    {
        PoolExecWorkFacts f = mk_facts(6u);
        deliver_tagged(2u, &f, 2u);
    }
    TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                      pool_session_execution_resolve_asic_result(2u, true));
    TEST_ASSERT_EQUAL_UINT64(2u, pool_session_execution_asic_job_results());
}

TEST_CASE("exec: work delivered with no generation is never evidence", "[pool_exec]")
{
    pool_session_execution_gate_reset(); /* generations all 0 */
    pool_session_execution_note_work_delivered(1u, NULL);
    TEST_ASSERT_EQUAL(EXEC_RESULT_STALE_GENERATION,
                      pool_session_execution_resolve_asic_result(1u, true));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
}

TEST_CASE("exec: result verdict tokens are stable and unique", "[pool_exec]")
{
    for (int i = 0; i < (int)POOL_EXEC_RESULT_VERDICT__COUNT; i++) {
        const char *t = pool_session_execution_result_verdict_str((PoolExecResultVerdict)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_NULL(strchr(t, '.'));
        for (int j = 0; j < i; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(t,
                pool_session_execution_result_verdict_str((PoolExecResultVerdict)j)));
        }
    }
    TEST_ASSERT_EQUAL_STRING("ASIC_RESULT_UNKNOWN",
        pool_session_execution_result_verdict_str((PoolExecResultVerdict)99));
}

/* ---------- job-id reuse: real proof-of-work binding ---------- */

/*
 * These tests reproduce EXACTLY the information the production result path
 * has: a job id, a nonce and a rolled version, validated against the job
 * currently registered in that slot with the SAME test_nonce_value() the
 * ASIC result task uses. No fake "expected generation" is supplied — the
 * binding is recomputed from real headers, exactly as on hardware.
 */

/* Build a distinct, valid mining job. */
static void make_job(bm_job *j, uint8_t seed)
{
    memset(j, 0, sizeof(*j));
    for (int i = 0; i < 32; i++) {
        j->prev_block_hash[i] = (uint8_t)(seed * 7u + i);
        j->merkle_root[i]     = (uint8_t)(seed * 13u + i * 3u);
    }
    j->version   = 0x20000000u;
    j->ntime     = 0x65000000u + seed;
    j->target    = 0x1d00ffffu;
    j->pool_diff = 1.0; /* the binding floor dominates */
}

/*
 * SEARCH SCALE, STATED HONESTLY.
 *
 * Production requires the returned nonce to prove work at
 * max(job difficulty, POOL_EXEC_WORK_BINDING_MIN_DIFF = 64). Reaching
 * difficulty 64 takes about 2^38 hashes — real mining, and far beyond what a
 * unit test may do. These tests therefore exercise the IDENTICAL mechanism
 * (the same test_nonce_value() over the same real headers) at a
 * search-feasible scale: the best nonce found for a job in a bounded
 * deterministic search, with the acceptance threshold set from that search.
 *
 * The property under test is the discriminating one and it is scale-free:
 * a nonce that is good for header A is worthless for header B, because the
 * double-SHA256 of a different header is an independent draw. Deploying the
 * absolute floor of 64 only makes the same separation astronomically
 * sharper (a foreign nonce clears it with probability 2^-38/64).
 */
#define NX_TEST_NONCE_SEARCH 8000u

/* Deterministic: fixed header, fixed search range, fixed result. */
static double best_nonce_for(const bm_job *j, uint32_t *out_nonce)
{
    double best = 0.0;
    uint32_t best_n = 0u;

    for (uint32_t n = 0; n < NX_TEST_NONCE_SEARCH; n++) {
        double d = test_nonce_value(j, n, j->version);
        if (d > best) {
            best = d;
            best_n = n;
        }
    }
    *out_nonce = best_n;
    return best;
}

/* The production binding rule with an explicit threshold (mirrors the
 * fixed-local-difficulty computation in asic_result_task.c; the pool share
 * difficulty is never an input). */
static bool work_bound_for(const bm_job *j, uint32_t nonce, double threshold)
{
    return test_nonce_value(j, nonce, j->version) >= threshold;
}

/* Canonical facts extracted from a real bm_job EXACTLY as the ASIC send
 * path (BM1370_send_work) extracts them. */
static PoolExecWorkFacts facts_of_job(const bm_job *j)
{
    PoolExecWorkFacts f;
    memset(&f, 0, sizeof(f));
    f.version = j->version;
    f.ntime   = j->ntime;
    f.nbits   = j->target;
    memcpy(f.prev_block_hash, j->prev_block_hash, sizeof(f.prev_block_hash));
    memcpy(f.merkle_root, j->merkle_root, sizeof(f.merkle_root));
    return f;
}

TEST_CASE("exec: a nonce binds to its own job and to no other", "[pool_exec]")
{
    bm_job a, b;
    uint32_t nonce_a = 0u, nonce_b = 0u;
    double best_a, best_b;

    make_job(&a, 1u);
    make_job(&b, 2u);
    best_a = best_nonce_for(&a, &nonce_a);
    best_b = best_nonce_for(&b, &nonce_b);
    TEST_ASSERT_TRUE(best_a > 0.0);
    TEST_ASSERT_TRUE(best_b > 0.0);

    /* Each nonce proves work for its OWN header at its own threshold... */
    TEST_ASSERT_TRUE(work_bound_for(&a, nonce_a, best_a));
    TEST_ASSERT_TRUE(work_bound_for(&b, nonce_b, best_b));
    /* ...and is worthless against the other header. This is the entire
     * anti-aliasing mechanism: a property of the hash, not of bookkeeping. */
    TEST_ASSERT_FALSE(work_bound_for(&b, nonce_a, best_a));
    TEST_ASSERT_FALSE(work_bound_for(&a, nonce_b, best_b));
}

TEST_CASE("exec: a delayed result survives job-id reuse and is REJECTED", "[pool_exec]")
{
    bm_job work_a, work_b;
    uint32_t nonce_a = 0u, nonce_b = 0u;
    double thr_a, thr_b;
    const uint8_t JOB_X = 24u; /* a real BM1370 job id (id = (id+24)%128) */

    make_job(&work_a, 3u);
    make_job(&work_b, 4u);
    thr_a = best_nonce_for(&work_a, &nonce_a);
    thr_b = best_nonce_for(&work_b, &nonce_b);
    TEST_ASSERT_TRUE(thr_a > 0.0 && thr_b > 0.0);

    pool_session_execution_gate_reset();

    /* 1. Work A delivered under generation A using job id X. */
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    {
        PoolExecWorkFacts fa = facts_of_job(&work_a);
        deliver_tagged(JOB_X, &fa, 0u);
    }

    /* 2-3. Generation changes and the SAME job id is reused for work B
     * (a genuinely DIFFERENT template — the identical-template case has
     * its own tests below). */
    pool_session_execution_set_protocol_generation(2u);
    (void)pool_session_execution_begin_work_generation();
    {
        PoolExecWorkFacts fb = facts_of_job(&work_b);
        deliver_tagged(JOB_X, &fb, 0u);
    }

    /* 4-5. The delayed result for work A now arrives. The registry entry for
     * X is CURRENT (it holds work B), so only the proof-of-work check can
     * save us — and it does: A's nonce does not prove work B. */
    TEST_ASSERT_EQUAL(EXEC_RESULT_WORK_MISMATCH,
                      pool_session_execution_resolve_asic_result(
                          JOB_X, work_bound_for(&work_b, nonce_a, thr_b)));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());

    /* 6. The genuine result for work B is accepted. */
    TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                      pool_session_execution_resolve_asic_result(
                          JOB_X, work_bound_for(&work_b, nonce_b, thr_b)));
    TEST_ASSERT_EQUAL_UINT64(1u, pool_session_execution_asic_job_results());

    /* 9. The same exact result cannot be counted twice. */
    TEST_ASSERT_EQUAL(EXEC_RESULT_ALREADY_CONSUMED,
                      pool_session_execution_resolve_asic_result(
                          JOB_X, work_bound_for(&work_b, nonce_b, thr_b)));
    TEST_ASSERT_EQUAL_UINT64(1u, pool_session_execution_asic_job_results());
}

TEST_CASE("exec: the same job id across many generations rejects every old result", "[pool_exec]")
{
    bm_job jobs[4];
    uint32_t nonces[4];
    double thr[4];
    const uint8_t JOB_X = 48u;
    unsigned g, older;

    for (g = 0u; g < 4u; g++) {
        make_job(&jobs[g], (uint8_t)(10u + g));
        thr[g] = best_nonce_for(&jobs[g], &nonces[g]);
        TEST_ASSERT_TRUE(thr[g] > 0.0);
    }

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();

    for (g = 0u; g < 4u; g++) {
        pool_session_execution_set_protocol_generation(g + 1u);
        (void)pool_session_execution_begin_work_generation();
        {
            PoolExecWorkFacts fg = facts_of_job(&jobs[g]);
            deliver_tagged(JOB_X, &fg, 0u); /* id REUSED, distinct template */
        }

        /* Every OLDER generation's result arrives late: all rejected. */
        for (older = 0u; older < g; older++) {
            TEST_ASSERT_EQUAL(EXEC_RESULT_WORK_MISMATCH,
                              pool_session_execution_resolve_asic_result(
                                  JOB_X, work_bound_for(&jobs[g], nonces[older], thr[g])));
        }
        /* The current generation's own result is accepted exactly once. */
        TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                          pool_session_execution_resolve_asic_result(
                              JOB_X, work_bound_for(&jobs[g], nonces[g], thr[g])));
    }
    /* The acceptance counter is PER GENERATION (each new work generation
     * zeroes it), so only the final generation's single acceptance remains
     * — no evidence is ever carried across a generation boundary. */
    TEST_ASSERT_EQUAL_UINT64(1u, pool_session_execution_asic_job_results());
    /* Rejections accumulate across the epoch: 0+1+2+3 late results. */
    TEST_ASSERT_EQUAL_UINT64(6u,
        pool_session_execution_asic_rejected(EXEC_RESULT_WORK_MISMATCH));
}

TEST_CASE("exec: a matching job id with mismatching work identity is rejected", "[pool_exec]")
{
    bm_job current, foreign;
    uint32_t foreign_nonce = 0u, cur_nonce = 0u;
    double thr_cur;

    make_job(&current, 21u);
    make_job(&foreign, 22u);
    (void)best_nonce_for(&foreign, &foreign_nonce);
    thr_cur = best_nonce_for(&current, &cur_nonce);
    TEST_ASSERT_TRUE(thr_cur > 0.0);

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    {
        PoolExecWorkFacts fc = facts_of_job(&current);
        deliver_tagged(8u, &fc, 0u);
    }

    /* Same job id, same generation, but the nonce proves OTHER work. */
    TEST_ASSERT_EQUAL(EXEC_RESULT_WORK_MISMATCH,
                      pool_session_execution_resolve_asic_result(
                          8u, work_bound_for(&current, foreign_nonce, thr_cur)));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
}

TEST_CASE("exec: a generation change before job-id reuse stays safe", "[pool_exec]")
{
    bm_job work_a;
    uint32_t nonce_a = 0u;
    double thr_a;

    make_job(&work_a, 31u);
    thr_a = best_nonce_for(&work_a, &nonce_a);
    TEST_ASSERT_TRUE(thr_a > 0.0);

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    {
        PoolExecWorkFacts fa = facts_of_job(&work_a);
        deliver_tagged(16u, &fa, 0u);
    }

    /* The generation changes and the id is NOT reused yet: the retained
     * record's stale generation stamp alone already rejects the delayed
     * result, even though its proof-of-work is perfectly valid for its own
     * job — and the record stays available as the header-comparison set. */
    pool_session_execution_set_protocol_generation(2u);
    (void)pool_session_execution_begin_work_generation();
    TEST_ASSERT_EQUAL(EXEC_RESULT_STALE_GENERATION,
                      pool_session_execution_resolve_asic_result(
                          16u, work_bound_for(&work_a, nonce_a, thr_a)));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
}

/* ---------- generation-unique work contract ---------- */

TEST_CASE("exec: work facts equality is exact and field-by-field", "[pool_exec]")
{
    PoolExecWorkFacts a = mk_facts(100u);
    PoolExecWorkFacts b = a;

    TEST_ASSERT_TRUE(pool_exec_work_facts_equal(&a, &b));
    /* Every single header field participates; no fingerprint shortcut. */
    b = a; b.version ^= 1u;
    TEST_ASSERT_FALSE(pool_exec_work_facts_equal(&a, &b));
    b = a; b.ntime ^= 1u;
    TEST_ASSERT_FALSE(pool_exec_work_facts_equal(&a, &b));
    b = a; b.nbits ^= 1u;
    TEST_ASSERT_FALSE(pool_exec_work_facts_equal(&a, &b));
    b = a; b.prev_block_hash[31] ^= 1u;
    TEST_ASSERT_FALSE(pool_exec_work_facts_equal(&a, &b));
    b = a; b.merkle_root[31] ^= 1u;
    TEST_ASSERT_FALSE(pool_exec_work_facts_equal(&a, &b));
    /* Total: an absent side never proves equality. */
    TEST_ASSERT_FALSE(pool_exec_work_facts_equal(NULL, &a));
    TEST_ASSERT_FALSE(pool_exec_work_facts_equal(&a, NULL));
    TEST_ASSERT_FALSE(pool_exec_work_facts_equal(NULL, NULL));
}

TEST_CASE("exec: discriminator capability is protocol- and width-gated", "[pool_exec]")
{
    /* V1: miner-owned extranonce2, width must fit the tag byte. */
    TEST_ASSERT_EQUAL(POOL_EXEC_DISCRIMINATOR_EXTRANONCE2,
                      pool_exec_discriminator_capability(false, false, 4u));
    TEST_ASSERT_EQUAL(POOL_EXEC_DISCRIMINATOR_EXTRANONCE2,
                      pool_exec_discriminator_capability(false, false, 8u));
    TEST_ASSERT_EQUAL(POOL_EXEC_DISCRIMINATOR_NONE,
                      pool_exec_discriminator_capability(false, false, 3u));
    TEST_ASSERT_EQUAL(POOL_EXEC_DISCRIMINATOR_NONE,
                      pool_exec_discriminator_capability(false, false, 0u));
    /* SV2 extended: the miner-rollable portion hosts the tag. */
    TEST_ASSERT_EQUAL(POOL_EXEC_DISCRIMINATOR_EXTRANONCE2,
                      pool_exec_discriminator_capability(true, true, 4u));
    TEST_ASSERT_EQUAL(POOL_EXEC_DISCRIMINATOR_NONE,
                      pool_exec_discriminator_capability(true, true, 3u));
    /* SV2 STANDARD: the pool owns the merkle root — never capable. */
    TEST_ASSERT_EQUAL(POOL_EXEC_DISCRIMINATOR_NONE,
                      pool_exec_discriminator_capability(true, false, 32u));
}

TEST_CASE("exec: the generation tag allocation can never express a wrapped domain", "[pool_exec]")
{
    unsigned g;

    /* Valid allocation indices produce strictly increasing, non-zero,
     * top-bit-marked tags: monotone implies NEVER a repeat. */
    for (g = 1u; g <= POOL_EXEC_GENERATION_TAG_LIMIT; g++) {
        uint32_t t = pool_exec_generation_tag(g);
        TEST_ASSERT_TRUE(t != 0u);
        TEST_ASSERT_TRUE((t & 0x80u) != 0u);
        TEST_ASSERT_TRUE(t <= 0xFFu);
        if (g > 1u) {
            TEST_ASSERT_EQUAL_UINT32(pool_exec_generation_tag(g - 1u) + 1u, t);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0x81u, pool_exec_generation_tag(1u));
    TEST_ASSERT_EQUAL_UINT32(0xFFu, pool_exec_generation_tag(POOL_EXEC_GENERATION_TAG_LIMIT));
    /* Index 0 and EVERYTHING past the budget is the fail-closed 0 — a
     * wrapped or reused domain is unrepresentable by this function. */
    TEST_ASSERT_EQUAL_UINT32(0u, pool_exec_generation_tag(0u));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_exec_generation_tag(128u));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_exec_generation_tag(129u));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_exec_generation_tag(255u));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_exec_generation_tag(0xFFFFFFFFu));

    /* The tag occupies counter bits 24..31 and leaves the rolling space. */
    TEST_ASSERT_EQUAL_UINT64(0x81000123ull,
        pool_exec_apply_generation_tag(0x123ull, pool_exec_generation_tag(1u)));
    TEST_ASSERT_EQUAL_UINT64(0x82FFFFFFull,
        pool_exec_apply_generation_tag(0xFFFFFFull, pool_exec_generation_tag(2u)));
    /* Rolling bits above 24 are cleared — they are zero padding in both
     * audited encodings, so nothing legitimate is lost. */
    TEST_ASSERT_EQUAL_UINT64(0x83000001ull,
        pool_exec_apply_generation_tag(0xAB000001ull, pool_exec_generation_tag(3u)));
}

/*
 * Shared REAL-construction fixture: one fixed provider template, exactly
 * what a pool resend after reconnect delivers. Coinbase halves are the
 * public Stratum documentation example (synthetic; no real account).
 */
static const char *NX_COINB1 =
    "01000000010000000000000000000000000000000000000000000000000000000000000000"
    "ffffffff20020862062f503253482f04b8864e5008";
static const char *NX_COINB2 =
    "072f736c7573682f000000000100f2052a010000001976a914d23fcdf86f7e756a64a7a9"
    "688ef9903327048ed988ac00000000";
static const char *NX_EXTRANONCE1 = "abcd1234";
static const char *NX_PREVHASH =
    "00000000000000000004a2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f7";

static void build_v1_job_from_counter(uint64_t en2_counter, bm_job *out)
{
    mining_notify n;
    char en2_str[17];
    uint8_t coinbase_hash[32];
    uint8_t merkle_root[32];

    memset(&n, 0, sizeof(n));
    n.job_id          = (char *)"tpl-1";  /* pool job id: NOT a header field */
    n.prev_block_hash = (char *)NX_PREVHASH;
    n.coinbase_1      = (char *)NX_COINB1;
    n.coinbase_2      = (char *)NX_COINB2;
    n.merkle_branches   = NULL;
    n.n_merkle_branches = 0;
    n.version = 0x20000000u;
    n.target  = 0x1d00ffffu;
    n.ntime   = 0x66aabbccu;

    extranonce_2_generate(en2_counter, 8u, en2_str);
    calculate_coinbase_tx_hash(n.coinbase_1, n.coinbase_2, NX_EXTRANONCE1,
                               en2_str, coinbase_hash);
    calculate_merkle_root_hash(coinbase_hash, NULL, 0, merkle_root);
    memset(out, 0, sizeof(*out));
    construct_bm_job(&n, merkle_root, 0u, 1.0, out);
    out->jobid = NULL;
    out->extranonce2 = NULL;
}

TEST_CASE("exec: a reconnect resend currently rebuilds a byte-identical header", "[pool_exec]")
{
    /*
     * STAGE-1 PROOF, with the REAL work-construction code: the firmware
     * resets its extranonce2 counter to 0 for every dequeued work item
     * (create_jobs_task), so two connections receiving the same provider
     * template with the same extranonce1 produce bm_jobs whose canonical
     * header facts are EQUAL byte-for-byte. This is the hazard: proof of
     * work cannot distinguish their results.
     */
    bm_job first, second;
    PoolExecWorkFacts fa, fb;

    build_v1_job_from_counter(0u, &first);   /* connection 1, counter 0 */
    build_v1_job_from_counter(0u, &second);  /* reconnect,   counter 0 */
    fa = facts_of_job(&first);
    fb = facts_of_job(&second);
    TEST_ASSERT_TRUE(pool_exec_work_facts_equal(&fa, &fb));
    TEST_ASSERT_EQUAL_MEMORY(first.merkle_root, second.merkle_root, 32);

    /* A nonce found for the first header is GENUINELY VALID for the
     * second — identical headers, identical hashes. */
    {
        uint32_t nonce_a = 0u;
        double thr_a = best_nonce_for(&first, &nonce_a);
        TEST_ASSERT_TRUE(thr_a > 0.0);
        TEST_ASSERT_TRUE(work_bound_for(&second, nonce_a, thr_a));
    }

    /* The extranonce2 generation domain breaks the identity: the tagged
     * counter changes coinbase bytes, so the merkle root — a field inside
     * the hashed 80-byte header — differs byte-for-byte. */
    {
        bm_job tagged_job;
        PoolExecWorkFacts ft;
        uint64_t tagged = pool_exec_apply_generation_tag(0u, pool_exec_generation_tag(2u));
        build_v1_job_from_counter(tagged, &tagged_job);
        ft = facts_of_job(&tagged_job);
        TEST_ASSERT_FALSE(pool_exec_work_facts_equal(&fa, &ft));
        TEST_ASSERT_TRUE(memcmp(first.merkle_root, tagged_job.merkle_root, 32) != 0);
    }
}

TEST_CASE("exec: an identical rebuilt header is excluded from evidence deterministically", "[pool_exec]")
{
    /*
     * Blocker scenario 1-7, end to end, with real headers. NO generation
     * discriminator (stock behaviour): generation B rebuilds identical H,
     * the delayed generation-A result is genuinely valid for it — and the
     * registry rejects it as freshness evidence anyway, deterministically.
     */
    bm_job h;
    PoolExecWorkFacts f;
    uint32_t nonce_a = 0u;
    double thr;
    const uint8_t JOB_X = 24u;
    PoolExecDeliveredView view;

    build_v1_job_from_counter(0u, &h);
    f = facts_of_job(&h);
    thr = best_nonce_for(&h, &nonce_a);
    TEST_ASSERT_TRUE(thr > 0.0);

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();

    /* 1. Generation A delivers header H using slot X (no domain). */
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    deliver_untagged(JOB_X, &f);

    /* 2-5. Reconnect; generation B; the pool resends the template; the
     * firmware rebuilds byte-identical H into the SAME slot. */
    pool_session_execution_set_protocol_generation(2u);
    (void)pool_session_execution_begin_work_generation();
    deliver_untagged(JOB_X, &f);

    /* The record was proven NON-unique against the outgoing prior-
     * generation record before delivery. */
    TEST_ASSERT_TRUE(pool_session_execution_delivered_view(JOB_X, &view));
    TEST_ASSERT_TRUE(view.occupied);
    TEST_ASSERT_FALSE(view.header_unique);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JOB_X, view.prior_match_slot);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)POOL_EXEC_DISCRIMINATOR_NONE,
                            view.discriminator_kind);

    /* 6-7. The delayed A result arrives: its proof of work HOLDS against
     * the identical current header (work_bound true — computed with the
     * real hash), and it is STILL excluded. Evidence stays zero, so
     * mining verification, target health and source COMPLETE all remain
     * unreachable on this path. */
    TEST_ASSERT_TRUE(work_bound_for(&h, nonce_a, thr));
    TEST_ASSERT_EQUAL(EXEC_RESULT_NO_DISCRIMINATOR,
                      pool_session_execution_resolve_asic_result(
                          JOB_X, work_bound_for(&h, nonce_a, thr)));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
    TEST_ASSERT_EQUAL_UINT64(1u,
        pool_session_execution_asic_rejected(EXEC_RESULT_NO_DISCRIMINATOR));
}

TEST_CASE("exec: the extranonce2 domain makes reconnect headers differ and gates evidence", "[pool_exec]")
{
    /*
     * Blocker scenarios 8-11: with the domain ACTIVE, generation B's
     * header H2 differs from H byte-for-byte, the delayed A result FAILS
     * proof of work against H2, and only generation B's genuine result is
     * accepted.
     */
    bm_job job_a, job_b;
    PoolExecWorkFacts fa, fb;
    uint32_t nonce_a = 0u, nonce_b = 0u;
    double thr_a, thr_b;
    uint64_t tagged = 0u;
    const uint8_t JOB_X = 48u;
    PoolExecDeliveredView view;

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    pool_session_execution_set_work_domain_active(true);
    TEST_ASSERT_TRUE(pool_session_execution_allocate_generation_tag());

    /* Generation A: tagged counter 0 -> header H. */
    TEST_ASSERT_TRUE(pool_session_execution_extranonce2_tag(false, false, 8u,
                                                            0u, &tagged));
    build_v1_job_from_counter(tagged, &job_a);
    fa = facts_of_job(&job_a);
    pool_session_execution_note_work_delivered(JOB_X, &fa);
    thr_a = best_nonce_for(&job_a, &nonce_a);
    TEST_ASSERT_TRUE(thr_a > 0.0);

    /* Reconnect -> generation B: SAME template, SAME counter reset — but
     * the tag differs, so H2 != H byte-for-byte (scenario 9). */
    pool_session_execution_set_protocol_generation(2u);
    (void)pool_session_execution_begin_work_generation();
    TEST_ASSERT_TRUE(pool_session_execution_allocate_generation_tag());
    TEST_ASSERT_TRUE(pool_session_execution_extranonce2_tag(false, false, 8u,
                                                            0u, &tagged));
    build_v1_job_from_counter(tagged, &job_b);
    fb = facts_of_job(&job_b);
    TEST_ASSERT_FALSE(pool_exec_work_facts_equal(&fa, &fb));
    TEST_ASSERT_TRUE(memcmp(job_a.merkle_root, job_b.merkle_root, 32) != 0);
    pool_session_execution_note_work_delivered(JOB_X, &fb);

    TEST_ASSERT_TRUE(pool_session_execution_delivered_view(JOB_X, &view));
    TEST_ASSERT_TRUE(view.header_unique);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)POOL_EXEC_DISCRIMINATOR_EXTRANONCE2,
                            view.discriminator_kind);
    TEST_ASSERT_EQUAL_UINT32(pool_session_execution_generation_tag_current(),
                             view.discriminator_tag);

    /* Scenario 10: the delayed A result fails validation against H2. */
    thr_b = best_nonce_for(&job_b, &nonce_b);
    TEST_ASSERT_TRUE(thr_b > 0.0);
    TEST_ASSERT_FALSE(work_bound_for(&job_b, nonce_a, thr_b));
    TEST_ASSERT_EQUAL(EXEC_RESULT_WORK_MISMATCH,
                      pool_session_execution_resolve_asic_result(
                          JOB_X, work_bound_for(&job_b, nonce_a, thr_b)));
    /* Scenario 11: generation B's own result validates and is counted. */
    TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                      pool_session_execution_resolve_asic_result(
                          JOB_X, work_bound_for(&job_b, nonce_b, thr_b)));
    TEST_ASSERT_EQUAL_UINT64(1u, pool_session_execution_asic_job_results());
}

TEST_CASE("exec: repeated generations with reused job ids stay generation-unique", "[pool_exec]")
{
    /* Blocker scenario 12: the domain across several generations, the
     * SAME chip slot reused every time, the SAME provider template. */
    const uint8_t JOB_X = 72u;
    uint64_t tagged = 0u;
    unsigned g;
    PoolExecDeliveredView view;

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_work_domain_active(true);

    for (g = 1u; g <= 3u; g++) {
        bm_job job_g;
        PoolExecWorkFacts fg;

        pool_session_execution_set_protocol_generation(g);
        (void)pool_session_execution_begin_work_generation();
        TEST_ASSERT_TRUE(pool_session_execution_allocate_generation_tag());
        TEST_ASSERT_TRUE(pool_session_execution_extranonce2_tag(
            false, false, 8u, 0u, &tagged));
        build_v1_job_from_counter(tagged, &job_g);
        fg = facts_of_job(&job_g);
        pool_session_execution_note_work_delivered(JOB_X, &fg);
        TEST_ASSERT_TRUE(pool_session_execution_delivered_view(JOB_X, &view));
        TEST_ASSERT_TRUE(view.header_unique);
        TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                          pool_session_execution_resolve_asic_result(JOB_X, true));
    }
    TEST_ASSERT_EQUAL_UINT64(1u, pool_session_execution_asic_job_results());
}

TEST_CASE("exec: two stale slots cannot fake the two-proof requirement", "[pool_exec]")
{
    /*
     * Blocker scenario 13: generation A leaves TWO chip slots holding
     * headers that generation B rebuilds identically. Both delayed A
     * results are genuinely valid for the rebuilt headers — and neither
     * counts, so the two-proof requirement is unsatisfiable from stale
     * work. Identical pool job ids / resent templates imply nothing
     * (scenario 14): the registry never even sees a pool job id.
     */
    PoolExecWorkFacts h1 = mk_facts(900u);
    PoolExecWorkFacts h2 = mk_facts(901u);
    const uint8_t X = 8u, Y = 16u;

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();

    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    deliver_untagged(X, &h1);
    deliver_untagged(Y, &h2);

    pool_session_execution_set_protocol_generation(2u);
    (void)pool_session_execution_begin_work_generation();
    deliver_untagged(X, &h1); /* identical headers rebuilt */
    deliver_untagged(Y, &h2);

    TEST_ASSERT_EQUAL(EXEC_RESULT_NO_DISCRIMINATOR,
                      pool_session_execution_resolve_asic_result(X, true));
    TEST_ASSERT_EQUAL(EXEC_RESULT_NO_DISCRIMINATOR,
                      pool_session_execution_resolve_asic_result(Y, true));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
    TEST_ASSERT_TRUE(pool_session_execution_asic_job_results() <
                     (uint64_t)POOL_EXEC_REQUIRED_WORK_PROOFS);
}

TEST_CASE("exec: a discriminator-carrying delivery with an aliased header is still excluded", "[pool_exec]")
{
    /*
     * The tag-wrap safety net: the EXACT comparison — not the tag — is the
     * authority. Even a delivery that carries the extranonce2 domain is
     * excluded when its header still equals a still-relevant prior-
     * generation header (e.g. after 128 generations the 7-bit tag space
     * wraps). The discriminator makes headers differ; the comparison
     * PROVES they differ.
     */
    PoolExecWorkFacts f = mk_facts(950u);
    const uint8_t X = 32u, Y = 40u;
    PoolExecDeliveredView view;

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();

    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    deliver_tagged(X, &f, 0u);

    pool_session_execution_set_protocol_generation(2u);
    (void)pool_session_execution_begin_work_generation();
    deliver_tagged(Y, &f, 0u); /* same header despite a tag issue */

    TEST_ASSERT_TRUE(pool_session_execution_delivered_view(Y, &view));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)POOL_EXEC_DISCRIMINATOR_EXTRANONCE2,
                            view.discriminator_kind);
    TEST_ASSERT_FALSE(view.header_unique);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)X, view.prior_match_slot);

    TEST_ASSERT_EQUAL(EXEC_RESULT_HEADER_ALIASED,
                      pool_session_execution_resolve_asic_result(Y, true));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());
    TEST_ASSERT_EQUAL_UINT64(1u,
        pool_session_execution_asic_rejected(EXEC_RESULT_HEADER_ALIASED));
}

TEST_CASE("exec: unsupported discriminator capability fails closed before verification", "[pool_exec]")
{
    /*
     * Blocker scenarios 15-16, SV2 split. STANDARD channels: the pool owns
     * the merkle root, no discriminator exists, and even a header-unique,
     * work-bound result is refused BEFORE any mining verification. A
     * too-narrow V1/extended extranonce2 behaves identically.
     */
    PoolExecWorkFacts f = mk_facts(960u);
    uint64_t tagged = 0xDEADu;

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    pool_session_execution_set_work_domain_active(true);

    /* SV2 standard: the query declines and never issues a domain... */
    TEST_ASSERT_FALSE(pool_session_execution_extranonce2_tag(true, false, 32u,
                                                             7u, &tagged));
    TEST_ASSERT_EQUAL_UINT64(7u, tagged); /* counter passes through */
    /* ...so the delivered record carries no discriminator... */
    pool_session_execution_note_work_delivered(50u, &f);
    /* ...and a UNIQUE, WORK-BOUND result is still refused. */
    TEST_ASSERT_EQUAL(EXEC_RESULT_NO_DISCRIMINATOR,
                      pool_session_execution_resolve_asic_result(50u, true));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());

    /* Too-narrow V1 width: same refusal. */
    TEST_ASSERT_FALSE(pool_session_execution_extranonce2_tag(false, false, 3u,
                                                             9u, &tagged));
    /* An INACTIVE domain also declines (normal mining stays stock). */
    pool_session_execution_set_work_domain_active(false);
    TEST_ASSERT_FALSE(pool_session_execution_extranonce2_tag(false, false, 8u,
                                                             9u, &tagged));
}

TEST_CASE("exec: sv2 extended coinbase carries the tag in the rollable extranonce", "[pool_exec]")
{
    /*
     * Blocker scenario 15 for the SV2 EXTENDED path, with the REAL binary
     * coinbase code and the REAL big-endian counter encoding used by
     * create_jobs_task: an untagged reconnect reproduces the identical
     * coinbase hash; the tagged counter lands in byte [len-4] and changes
     * it.
     */
    static const uint8_t prefix[] = {0x01, 0x00, 0x00, 0x00, 0x01, 0xAA};
    static const uint8_t en_prefix[] = {0x11, 0x22, 0x33, 0x44};
    static const uint8_t suffix[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    uint8_t en2_a[8], en2_b[8], en2_t[8];
    uint8_t hash_a[32], hash_b[32], hash_t[32];
    uint64_t counter, tagged;
    int i;

    /* The exact BE encoding loop from generate_work_sv2_ext. */
    memset(en2_a, 0, sizeof(en2_a));
    counter = 0u;
    for (i = 7; i >= 0 && counter > 0u; i--) {
        en2_a[i] = (uint8_t)(counter & 0xFFu);
        counter >>= 8;
    }
    memcpy(en2_b, en2_a, sizeof(en2_b)); /* reconnect: counter 0 again */

    memset(en2_t, 0, sizeof(en2_t));
    tagged = pool_exec_apply_generation_tag(0u, pool_exec_generation_tag(5u));
    for (i = 7; i >= 0 && tagged > 0u; i--) {
        en2_t[i] = (uint8_t)(tagged & 0xFFu);
        tagged >>= 8;
    }
    /* The tag byte sits at [len-4] — inside the hashed coinbase. */
    TEST_ASSERT_EQUAL_UINT8(pool_exec_generation_tag(5u), en2_t[4]);

    calculate_coinbase_tx_hash_bin(prefix, sizeof(prefix), en_prefix,
                                   sizeof(en_prefix), en2_a, sizeof(en2_a),
                                   suffix, sizeof(suffix), hash_a);
    calculate_coinbase_tx_hash_bin(prefix, sizeof(prefix), en_prefix,
                                   sizeof(en_prefix), en2_b, sizeof(en2_b),
                                   suffix, sizeof(suffix), hash_b);
    calculate_coinbase_tx_hash_bin(prefix, sizeof(prefix), en_prefix,
                                   sizeof(en_prefix), en2_t, sizeof(en2_t),
                                   suffix, sizeof(suffix), hash_t);
    /* Identical without the domain; different with it. */
    TEST_ASSERT_EQUAL_MEMORY(hash_a, hash_b, 32);
    TEST_ASSERT_TRUE(memcmp(hash_a, hash_t, 32) != 0);
}

TEST_CASE("exec: retained records die only by slot reuse and reset stays bounded", "[pool_exec]")
{
    /*
     * Retention is bounded by construction (POOL_EXEC_JOB_SLOTS static
     * records — no allocation anywhere). A prior-generation record stops
     * excluding once its slot is reused by DISTINCT work; gate_reset
     * clears everything.
     */
    PoolExecWorkFacts f  = mk_facts(970u);
    PoolExecWorkFacts f2 = mk_facts(971u);
    PoolExecWorkFacts f3 = mk_facts(972u);
    const uint8_t X = 56u, Y = 64u;
    PoolExecDeliveredView view;

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    deliver_tagged(X, &f, 0u);

    pool_session_execution_set_protocol_generation(2u);
    (void)pool_session_execution_begin_work_generation();
    /* Slot X is reused by DISTINCT work: the old header retires with it. */
    deliver_tagged(X, &f2, 0u);
    TEST_ASSERT_TRUE(pool_session_execution_delivered_view(X, &view));
    TEST_ASSERT_TRUE(view.header_unique);
    /* The retired header no longer excludes a NEW delivery elsewhere. */
    deliver_tagged(Y, &f, 1u);
    TEST_ASSERT_TRUE(pool_session_execution_delivered_view(Y, &view));
    TEST_ASSERT_TRUE(view.header_unique);

    /* Reset clears every record. */
    pool_session_execution_gate_reset();
    TEST_ASSERT_FALSE(pool_session_execution_delivered_view(X, &view));
    TEST_ASSERT_FALSE(pool_session_execution_delivered_view(Y, &view));
    (void)f3;
}

TEST_CASE("exec: the discriminator budget is finite, monotonic and never reissued", "[pool_exec]")
{
    /*
     * Blocker scenarios 1, 2, 3 and 9: the LAST available discriminator
     * works normally, the next request does NOT wrap, exhaustion fails
     * closed BEFORE work delivery, and no generation/extranonce2 domain is
     * ever reissued within one boot.
     */
    PoolExecWorkFacts f = mk_facts(2000u);
    uint64_t tagged = 0u;
    uint32_t prev = 0u;
    unsigned n;

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    pool_session_execution_set_work_domain_active(true);
    TEST_ASSERT_EQUAL_UINT32(POOL_EXEC_GENERATION_TAG_LIMIT,
                             pool_session_execution_generation_tags_remaining());

    /* Consume every tag but the last: strictly increasing = never reused. */
    for (n = 1u; n < POOL_EXEC_GENERATION_TAG_LIMIT; n++) {
        TEST_ASSERT_TRUE(pool_session_execution_allocate_generation_tag());
        TEST_ASSERT_TRUE(pool_session_execution_generation_tag_current() > prev);
        prev = pool_session_execution_generation_tag_current();
    }

    /* 1. The FINAL available discriminator works normally. */
    TEST_ASSERT_TRUE(pool_session_execution_allocate_generation_tag());
    TEST_ASSERT_EQUAL_UINT32(0xFFu, pool_session_execution_generation_tag_current());
    TEST_ASSERT_TRUE(pool_session_execution_extranonce2_tag(false, false, 8u,
                                                            3u, &tagged));
    TEST_ASSERT_EQUAL_UINT64(0xFF000003ull, tagged);
    pool_session_execution_note_work_delivered(24u, &f);
    TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                      pool_session_execution_resolve_asic_result(24u, true));

    /* 2. The next request does NOT wrap: it fails, permanently. */
    TEST_ASSERT_FALSE(pool_session_execution_allocate_generation_tag());
    TEST_ASSERT_TRUE(pool_session_execution_generation_exhausted());
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_generation_tags_remaining());
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_generation_tag_current());
    TEST_ASSERT_FALSE(pool_session_execution_allocate_generation_tag());

    /* 3. Exhaustion fails closed BEFORE work delivery: the embed query
     * declines, delivered work carries no domain, and a work-bound result
     * is refused. No verification work ever runs on a reused domain. */
    (void)pool_session_execution_begin_work_generation();
    TEST_ASSERT_FALSE(pool_session_execution_extranonce2_tag(false, false, 8u,
                                                             0u, &tagged));
    {
        PoolExecWorkFacts f2 = mk_facts(2001u);
        pool_session_execution_note_work_delivered(32u, &f2);
    }
    TEST_ASSERT_EQUAL(EXEC_RESULT_NO_DISCRIMINATOR,
                      pool_session_execution_resolve_asic_result(32u, true));
    TEST_ASSERT_EQUAL_UINT64(0u, pool_session_execution_asic_job_results());

    /* Only the BOOT reset returns the budget (deinit/rebind never do —
     * the executor tests cover that side). */
    pool_session_execution_gate_reset();
    TEST_ASSERT_TRUE(pool_session_execution_allocate_generation_tag());
    TEST_ASSERT_EQUAL_UINT32(0x81u, pool_session_execution_generation_tag_current());
}

TEST_CASE("exec: the rolling counter fails closed before touching the tag byte", "[pool_exec]")
{
    /*
     * Blocker scenarios 7 and 8: the final value of the 24-bit rolling
     * space works; one past it is REFUSED — the overflow can never
     * silently carry into the tag byte (a previously used domain), and
     * the refused work can never become verification evidence.
     */
    PoolExecWorkFacts f = mk_facts(2100u);
    uint64_t tagged = 0u;
    uint32_t tag;

    pool_session_execution_gate_reset();
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    pool_session_execution_set_work_domain_active(true);
    TEST_ASSERT_TRUE(pool_session_execution_allocate_generation_tag());
    tag = pool_session_execution_generation_tag_current();

    /* 8a. The FINAL rolling value works. */
    TEST_ASSERT_TRUE(pool_session_execution_extranonce2_tag(
        false, false, 8u, POOL_EXEC_GENERATION_COUNTER_MASK, &tagged));
    TEST_ASSERT_EQUAL_UINT64(((uint64_t)tag << 24) | POOL_EXEC_GENERATION_COUNTER_MASK,
                             tagged);
    pool_session_execution_note_work_delivered(40u, &f);
    TEST_ASSERT_EQUAL(EXEC_RESULT_ACCEPTED,
                      pool_session_execution_resolve_asic_result(40u, true));

    /* 7/8b. One past the space: refused, counter passes through UNTOUCHED
     * (no silent wrap), the delivery carries no domain, and the result is
     * not evidence. */
    TEST_ASSERT_FALSE(pool_session_execution_extranonce2_tag(
        false, false, 8u, POOL_EXEC_GENERATION_COUNTER_MASK + 1u, &tagged));
    TEST_ASSERT_EQUAL_UINT64(POOL_EXEC_GENERATION_COUNTER_MASK + 1u, tagged);
    TEST_ASSERT_EQUAL_UINT64(1u, pool_session_execution_rolling_declined());
    {
        PoolExecWorkFacts f2 = mk_facts(2101u);
        pool_session_execution_note_work_delivered(48u, &f2);
    }
    TEST_ASSERT_EQUAL(EXEC_RESULT_NO_DISCRIMINATOR,
                      pool_session_execution_resolve_asic_result(48u, true));

    /* A legitimate new-notify counter reset still tags: the guard closes
     * the overflow, not normal operation. */
    TEST_ASSERT_TRUE(pool_session_execution_extranonce2_tag(false, false, 8u,
                                                            0u, &tagged));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)tag << 24, tagged);
}

TEST_CASE("exec: V1 share submission preserves the exact tagged extranonce2", "[pool_exec]")
{
    /*
     * Blocker scenario 10. In production ONE string carries the tagged
     * extranonce2 end to end: extranonce_2_generate() writes it, the
     * coinbase is hashed from it, bm_job.extranonce2 strdup()s it, and
     * STRATUM_V1_submit_share() sends that very string. This test proves
     * the round trip: the pool, reconstructing the coinbase from the
     * SUBMITTED string, reproduces the delivered header byte-for-byte.
     */
    uint32_t tag = pool_exec_generation_tag(19u); /* 0x93 */
    uint64_t tagged = pool_exec_apply_generation_tag(5u, tag);
    char en2_str[17];
    uint8_t coinbase_hash[32];
    uint8_t merkle_from_submission[32];
    bm_job delivered;

    TEST_ASSERT_EQUAL_UINT32(0x93u, tag);
    extranonce_2_generate(tagged, 8u, en2_str);
    /* The tag byte is visible, unmangled, in the submitted hex (byte 3 of
     * the little-endian counter encoding = chars 6..7). */
    TEST_ASSERT_EQUAL_UINT8('9', en2_str[6]);
    TEST_ASSERT_EQUAL_UINT8('3', en2_str[7]);

    /* Delivered header (the real construction path). */
    build_v1_job_from_counter(tagged, &delivered);

    /* Pool-side reconstruction from the submitted string. */
    calculate_coinbase_tx_hash(NX_COINB1, NX_COINB2, NX_EXTRANONCE1,
                               en2_str, coinbase_hash);
    calculate_merkle_root_hash(coinbase_hash, NULL, 0, merkle_from_submission);
    {
        uint8_t merkle_delivered_order[32];
        reverse_32bit_words(merkle_from_submission, merkle_delivered_order);
        TEST_ASSERT_EQUAL_MEMORY(delivered.merkle_root, merkle_delivered_order, 32);
    }
}

TEST_CASE("exec: SV2 extended submission preserves the exact tagged extranonce bytes", "[pool_exec]")
{
    /*
     * Blocker scenario 11. The SV2 extended submit path stores the
     * delivered bytes as hex (bin2hex in generate_work_sv2_ext) and
     * decodes them back for submission (hex2bin in asic_result_task).
     * Prove the decode reproduces the delivered bytes exactly and that a
     * coinbase built from the submitted bytes equals the delivered one.
     */
    static const uint8_t prefix[] = {0x02, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t en_prefix[] = {0xA1, 0xB2};
    static const uint8_t suffix[] = {0x00, 0x00, 0x00, 0x00};
    uint8_t en2_delivered[8], en2_submitted[8];
    char en2_hex[17];
    uint8_t hash_delivered[32], hash_submitted[32];
    uint64_t v = pool_exec_apply_generation_tag(0x000042u, pool_exec_generation_tag(7u));
    int i;

    memset(en2_delivered, 0, sizeof(en2_delivered));
    for (i = 7; i >= 0 && v > 0u; i--) {
        en2_delivered[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
    bin2hex(en2_delivered, sizeof(en2_delivered), en2_hex, sizeof(en2_hex));
    memset(en2_submitted, 0xEE, sizeof(en2_submitted));
    hex2bin(en2_hex, en2_submitted, sizeof(en2_submitted));
    TEST_ASSERT_EQUAL_MEMORY(en2_delivered, en2_submitted, sizeof(en2_delivered));

    calculate_coinbase_tx_hash_bin(prefix, sizeof(prefix), en_prefix,
                                   sizeof(en_prefix), en2_delivered,
                                   sizeof(en2_delivered), suffix, sizeof(suffix),
                                   hash_delivered);
    calculate_coinbase_tx_hash_bin(prefix, sizeof(prefix), en_prefix,
                                   sizeof(en_prefix), en2_submitted,
                                   sizeof(en2_submitted), suffix, sizeof(suffix),
                                   hash_submitted);
    TEST_ASSERT_EQUAL_MEMORY(hash_delivered, hash_submitted, 32);
}

TEST_CASE("exec: every registry and identity operation returns at lock depth zero", "[pool_exec]")
{
    /*
     * The gate lock guards bounded CPU-only sections. Every public
     * operation must return with the lock fully released — the depth
     * counter is the machine-checkable proof the blocking-IO contract
     * builds on.
     */
    PoolExecWorkFacts f = mk_facts(2200u);
    PoolExecDeliveredView view;
    PoolExecIdentityCopy copy;
    uint64_t tagged = 0u;
    uint32_t pinned = 0u;

    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
    pool_session_execution_gate_reset();
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
    (void)pool_session_execution_begin_config_generation();
    pool_session_execution_set_protocol_generation(1u);
    (void)pool_session_execution_begin_work_generation();
    pool_session_execution_set_work_domain_active(true);
    (void)pool_session_execution_allocate_generation_tag();
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
    (void)pool_session_execution_extranonce2_tag(false, false, 8u, 0u, &tagged);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
    pool_session_execution_note_work_delivered(8u, &f);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
    (void)pool_session_execution_resolve_asic_result(8u, true);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
    (void)pool_session_execution_delivered_view(8u, &view);
    (void)pool_session_execution_jobs_forwarded();
    (void)pool_session_execution_asic_job_results();
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
    (void)publish_identity_fwd("lockhost.example", "lockuser");
    (void)pool_session_execution_identity_copy(&copy);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
    {
        uint32_t slot = 0u;
        const PoolExecIdentityStrings *b =
            pool_session_execution_identity_borrow(&slot);
        TEST_ASSERT_NOT_NULL(b);
        TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
        pool_session_execution_identity_release(slot);
    }
    (void)pool_session_execution_identity_reclaim_all(&pinned);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
}

TEST_CASE("exec: a copied identity outlives the lock and later publications", "[pool_exec]")
{
    /*
     * The blocking-write pattern, deterministically: the copy taken
     * before a (delayed) socket write stays byte-stable through a later
     * publication AND that later publication is never blocked by the
     * outstanding copy — copies pin nothing.
     */
    PoolExecIdentityCopy before, again, after;

    pool_session_execution_gate_reset();
    (void)publish_identity_fwd("first.example", "first-user");
    TEST_ASSERT_TRUE(pool_session_execution_identity_copy(&before));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_lock_depth());
    again = before; /* the caller-owned bytes a delayed write would use */

    /* A later, safe identity publication is NOT prevented by the
     * outstanding copy... */
    (void)publish_identity_fwd("second.example", "second-user");
    TEST_ASSERT_TRUE(pool_session_execution_identity_copy(&after));
    TEST_ASSERT_EQUAL_STRING("second.example", after.primary_host);

    /* ...and the old copy is still byte-for-byte what was taken. */
    TEST_ASSERT_EQUAL_MEMORY(&again, &before, sizeof(before));
    TEST_ASSERT_EQUAL_STRING("first.example", before.primary_host);
    TEST_ASSERT_EQUAL_STRING("first-user", before.primary_user);
}

/* ---------- identity slot pool: proven reader lifetime ---------- */

TEST_CASE("exec: an identity slot is never reused within an epoch", "[pool_exec]")
{
    const PoolExecIdentityStrings *seen[POOL_EXEC_IDENTITY_SLOTS];
    uint32_t slot;
    unsigned i, j, n = 0u;

    pool_session_execution_gate_reset();
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_identity_slots_used());

    for (i = 0u; i < POOL_EXEC_IDENTITY_SLOTS; i++) {
        PoolExecIdentityStrings *s = pool_session_execution_identity_acquire(&slot);
        TEST_ASSERT_NOT_NULL(s);
        strncpy(s->primary_host, "h.example", sizeof(s->primary_host) - 1);
        TEST_ASSERT_TRUE(pool_session_execution_identity_publish(slot));
        /* Every acquisition is a DISTINCT address: no in-place overwrite of
         * storage a reader may still be holding. */
        for (j = 0u; j < n; j++) {
            TEST_ASSERT_TRUE(seen[j] != s);
        }
        seen[n++] = s;
    }
    TEST_ASSERT_EQUAL_UINT32(POOL_EXEC_IDENTITY_SLOTS,
                             pool_session_execution_identity_slots_used());

    /* Exhaustion FAILS CLOSED — never by recycling a live slot. */
    TEST_ASSERT_NULL(pool_session_execution_identity_acquire(&slot));

    /* Only a quiescent reclaim frees the epoch — and only the slots that
     * no reader still references. */
    {
        uint32_t pinned = 0u;
        (void)pool_session_execution_identity_reclaim_all(&pinned);
        TEST_ASSERT_EQUAL_UINT32(0u, pinned);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_identity_slots_used());
    TEST_ASSERT_NOT_NULL(pool_session_execution_identity_acquire(&slot));
}

TEST_CASE("exec: publication transfers the reference and borrows pin a slot", "[pool_exec]")
{
    PoolExecIdentityStrings *a, *b;
    const PoolExecIdentityStrings *borrowed;
    uint32_t slot_a = 0u, slot_b = 0u, borrowed_slot = 0u;

    pool_session_execution_gate_reset();

    a = pool_session_execution_identity_acquire(&slot_a);
    TEST_ASSERT_NOT_NULL(a);
    strncpy(a->primary_host, "first.example", sizeof(a->primary_host) - 1);
    TEST_ASSERT_TRUE(pool_session_execution_identity_publish(slot_a));
    TEST_ASSERT_EQUAL_UINT32(slot_a, pool_session_execution_identity_published_slot());
    /* Publication does not pin storage; only reader borrows do. */
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_identity_refcount(slot_a));

    /* A controlled reader borrows the live slot. */
    borrowed = pool_session_execution_identity_borrow(&borrowed_slot);
    TEST_ASSERT_NOT_NULL(borrowed);
    TEST_ASSERT_EQUAL_UINT32(slot_a, borrowed_slot);
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_execution_identity_refcount(slot_a));

    /* Publishing a new slot drops the OLD publication reference but the
     * borrow keeps the old slot pinned and its bytes intact. */
    b = pool_session_execution_identity_acquire(&slot_b);
    TEST_ASSERT_NOT_NULL(b);
    strncpy(b->primary_host, "second.example", sizeof(b->primary_host) - 1);
    TEST_ASSERT_TRUE(pool_session_execution_identity_publish(slot_b));
    /* The borrow keeps slot A pinned; slot B is published but unborrowed. */
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_execution_identity_refcount(slot_a));
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_identity_refcount(slot_b));
    TEST_ASSERT_EQUAL_STRING("first.example", borrowed->primary_host);

    pool_session_execution_identity_release(slot_a);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_identity_refcount(slot_a));

    /* A slot can never be published twice. */
    TEST_ASSERT_FALSE(pool_session_execution_identity_publish(slot_b));
    TEST_ASSERT_FALSE(pool_session_execution_identity_publish(9999u));
}

TEST_CASE("exec: a published slot is immutable for a concurrent reader", "[pool_exec]")
{
    PoolExecIdentityStrings *s;
    const PoolExecIdentityStrings *reader;
    uint32_t slot = 0u, reader_slot = 0u;
    unsigned i;

    pool_session_execution_gate_reset();

    s = pool_session_execution_identity_acquire(&slot);
    TEST_ASSERT_NOT_NULL(s);
    strncpy(s->primary_host, "pinned.example", sizeof(s->primary_host) - 1);
    strncpy(s->primary_user, "pinned.acct", sizeof(s->primary_user) - 1);
    TEST_ASSERT_TRUE(pool_session_execution_identity_publish(slot));

    /* A long-lived reader holds the raw pointer across MANY further
     * publications — the classic case two rotating buffers would corrupt. */
    reader = pool_session_execution_identity_borrow(&reader_slot);
    TEST_ASSERT_NOT_NULL(reader);

    for (i = 0u; i < POOL_EXEC_IDENTITY_SLOTS - 1u; i++) {
        uint32_t other = 0u;
        PoolExecIdentityStrings *n = pool_session_execution_identity_acquire(&other);
        TEST_ASSERT_NOT_NULL(n);
        strncpy(n->primary_host, "rotating.example", sizeof(n->primary_host) - 1);
        strncpy(n->primary_user, "rotating.acct", sizeof(n->primary_user) - 1);
        TEST_ASSERT_TRUE(pool_session_execution_identity_publish(other));
        /* The reader's bytes are untouched every single time. */
        TEST_ASSERT_EQUAL_STRING("pinned.example", reader->primary_host);
        TEST_ASSERT_EQUAL_STRING("pinned.acct", reader->primary_user);
    }
    pool_session_execution_identity_release(reader_slot);
}

/* Fill a slot with a self-consistent identity and publish it. */
static uint32_t publish_identity(const char *host, const char *user);

static uint32_t publish_identity_fwd(const char *host, const char *user)
{
    return publish_identity(host, user);
}

static uint32_t publish_identity(const char *host, const char *user)
{
    uint32_t slot = 0u;
    PoolExecIdentityStrings *s = pool_session_execution_identity_acquire(&slot);

    TEST_ASSERT_NOT_NULL(s);
    strncpy(s->primary_host, host, sizeof(s->primary_host) - 1);
    strncpy(s->primary_user, user, sizeof(s->primary_user) - 1);
    strncpy(s->fallback_host, host, sizeof(s->fallback_host) - 1);
    strncpy(s->fallback_user, user, sizeof(s->fallback_user) - 1);
    TEST_ASSERT_TRUE(pool_session_execution_identity_publish(slot));
    return slot;
}

TEST_CASE("exec: an async reader sees complete A or complete B, never mixed", "[pool_exec]")
{
    PoolExecIdentityCopy early, late;
    unsigned i;

    pool_session_execution_gate_reset();
    publish_identity("alpha.example", "alpha.acct");

    /* 1. An HTTP/BAP-style reader begins on identity A: one copy-under-lock. */
    TEST_ASSERT_TRUE(pool_session_execution_identity_copy(&early));
    TEST_ASSERT_EQUAL_STRING("alpha.example", early.primary_host);
    TEST_ASSERT_EQUAL_STRING("alpha.acct", early.primary_user);

    /* 2. New epochs publish other identities while that reader still holds
     *    its copy. 3. The copy stays COMPLETE and self-consistent — every
     *    field from the same identity, never a mix. */
    for (i = 0u; i < 4u; i++) {
        publish_identity("beta.example", "beta.acct");
        TEST_ASSERT_EQUAL_STRING("alpha.example", early.primary_host);
        TEST_ASSERT_EQUAL_STRING("alpha.acct", early.primary_user);
        TEST_ASSERT_EQUAL_STRING("alpha.example", early.fallback_host);
        TEST_ASSERT_EQUAL_STRING("alpha.acct", early.fallback_user);
    }

    /* A later reader sees the complete NEW identity, equally consistent. */
    TEST_ASSERT_TRUE(pool_session_execution_identity_copy(&late));
    TEST_ASSERT_EQUAL_STRING("beta.example", late.primary_host);
    TEST_ASSERT_EQUAL_STRING("beta.acct", late.primary_user);
    TEST_ASSERT_EQUAL_STRING("beta.example", late.fallback_host);

    /* With nothing published a copy fails closed rather than inventing one. */
    pool_session_execution_gate_reset();
    TEST_ASSERT_FALSE(pool_session_execution_identity_copy(&late));
    TEST_ASSERT_FALSE(late.valid);
    TEST_ASSERT_FALSE(pool_session_execution_identity_copy(NULL));
}

TEST_CASE("exec: a slot with a live borrow cannot be reclaimed", "[pool_exec]")
{
    const PoolExecIdentityStrings *held;
    uint32_t slot = 0u, pinned = 0u, reclaimed;

    pool_session_execution_gate_reset();
    publish_identity("pinned.example", "pinned.acct");

    /* 4. A protocol-instance-style borrow pins the exact slot. */
    held = pool_session_execution_identity_borrow(&slot);
    TEST_ASSERT_NOT_NULL(held);
    TEST_ASSERT_TRUE(pool_session_execution_identity_refcount(slot) >= 1u);

    reclaimed = pool_session_execution_identity_reclaim_all(&pinned);
    TEST_ASSERT_EQUAL_UINT32(1u, pinned);       /* refused for the live one */
    TEST_ASSERT_EQUAL_UINT32(0u, reclaimed);
    /* The pinned storage is intact and still readable. */
    TEST_ASSERT_EQUAL_STRING("pinned.example", held->primary_host);

    /* 5. A delayed serializer that only now finishes still reads its own
     *    identity — the storage was never reused underneath it. */
    TEST_ASSERT_EQUAL_STRING("pinned.acct", held->primary_user);

    /* Once the instance exits and releases, the slot returns to the pool. */
    pool_session_execution_identity_release(slot);
    reclaimed = pool_session_execution_identity_reclaim_all(&pinned);
    TEST_ASSERT_EQUAL_UINT32(0u, pinned);
    TEST_ASSERT_TRUE(reclaimed >= 1u);
}

TEST_CASE("exec: a protocol instance keeps one stable identity for its life", "[pool_exec]")
{
    const PoolExecIdentityStrings *instance;
    uint32_t slot = 0u;
    unsigned i;

    pool_session_execution_gate_reset();
    publish_identity("instance.example", "instance.acct");

    /* 6. The instance borrows once at start and reads for its whole life. */
    instance = pool_session_execution_identity_borrow(&slot);
    TEST_ASSERT_NOT_NULL(instance);

    for (i = 0u; i < 5u; i++) {
        uint32_t pinned = 0u;
        publish_identity("churn.example", "churn.acct");
        (void)pool_session_execution_identity_reclaim_all(&pinned);
        TEST_ASSERT_TRUE(pinned >= 1u); /* the instance's slot is protected */
        TEST_ASSERT_EQUAL_STRING("instance.example", instance->primary_host);
        TEST_ASSERT_EQUAL_STRING("instance.acct", instance->primary_user);
    }
    pool_session_execution_identity_release(slot);
}

TEST_CASE("exec: slot exhaustion and reclamation fail closed", "[pool_exec]")
{
    uint32_t slot = 0u, pinned = 0u;
    unsigned i;

    /* 7. Exhaustion never recycles a live slot — acquire simply fails. */
    pool_session_execution_gate_reset();
    for (i = 0u; i < POOL_EXEC_IDENTITY_SLOTS; i++) {
        TEST_ASSERT_NOT_NULL(pool_session_execution_identity_acquire(&slot));
    }
    TEST_ASSERT_NULL(pool_session_execution_identity_acquire(&slot));
    TEST_ASSERT_EQUAL_UINT32(POOL_EXEC_IDENTITY_SLOTS,
                             pool_session_execution_identity_slots_used());

    /* Unreferenced slots reclaim; the pool is reusable across epochs. */
    (void)pool_session_execution_identity_reclaim_all(&pinned);
    TEST_ASSERT_EQUAL_UINT32(0u, pinned);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_identity_slots_used());
    TEST_ASSERT_NOT_NULL(pool_session_execution_identity_acquire(&slot));

    /* Out-of-range operations are inert. */
    pool_session_execution_identity_release(9999u);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_identity_refcount(9999u));
    TEST_ASSERT_FALSE(pool_session_execution_identity_publish(9999u));
}

TEST_CASE("exec: many epochs keep a constant bounded storage footprint", "[pool_exec]")
{
    unsigned epoch;

    /* 9. Twenty full epochs, each publishing several identities, with a
     *    reclaim at every quiescent boundary: the slots-in-use count returns
     *    to the same bound every time and never grows. */
    for (epoch = 0u; epoch < 20u; epoch++) {
        uint32_t pinned = 0u;
        unsigned k;

        pool_session_execution_gate_reset();
        TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_identity_slots_used());
        for (k = 0u; k < 3u; k++) {
            publish_identity("epoch.example", "epoch.acct");
        }
        TEST_ASSERT_EQUAL_UINT32(3u, pool_session_execution_identity_slots_used());
        (void)pool_session_execution_identity_reclaim_all(&pinned);
        TEST_ASSERT_EQUAL_UINT32(0u, pinned);
        TEST_ASSERT_EQUAL_UINT32(0u, pool_session_execution_identity_slots_used());
    }
}

TEST_CASE("exec: the identity pool bound matches the B1 retry budgets", "[pool_exec]")
{
    /* The pool is sized to the proven worst case: one publication per
     * bounded retry on each side, so a legitimate session can never be
     * starved and the memory bound is fixed. */
    TEST_ASSERT_EQUAL_UINT32((POOL_SESSION_MAX_TARGET_APPLY_RETRIES + 1u) +
                             (POOL_SESSION_MAX_TARGET_VERIFY_RETRIES + 1u) +
                             (POOL_SESSION_MAX_RESTORE_APPLY_RETRIES + 1u) +
                             (POOL_SESSION_MAX_RESTORE_VERIFY_RETRIES + 1u),
                             (uint32_t)POOL_EXEC_IDENTITY_SLOTS);
}

TEST_CASE("exec: a snapshot claiming ASIC evidence without a generation is invalid", "[pool_exec]")
{
    PoolExecutionSnapshot s;

    pool_exec_snapshot_init(&s);
    s.asic_evidence_seen = true;
    s.work_generation    = 0u;
    TEST_ASSERT_FALSE(pool_exec_snapshot_valid(&s));
    s.work_generation = 3u;
    TEST_ASSERT_TRUE(pool_exec_snapshot_valid(&s));
}
