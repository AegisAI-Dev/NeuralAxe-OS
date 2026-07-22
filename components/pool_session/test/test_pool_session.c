/*
 * Exhaustive deterministic tests for the pure timed-pool-session FSM (B1).
 * No hardware, no network, no secrets. All hosts/accounts below are synthetic
 * fixtures ("*.example", "acct.worker") — never real pools or wallets.
 */

#include <string.h>
#include "unity.h"
#include "pool_session.h"

/* ---------------- fixture builders ---------------- */

static PoolEndpoint ep(const char *host, uint16_t port, const char *user)
{
    PoolEndpoint e;
    memset(&e, 0, sizeof(e));
    strncpy(e.host, host, sizeof(e.host) - 1);
    e.port = port;
    strncpy(e.user, user, sizeof(e.user) - 1);
    e.protocol = POOL_PROTO_STRATUM_V1;
    e.tls = false;
    return e;
}

static PoolConfigIdentity cfg(PoolChainType chain, const char *host, uint16_t port, const char *user)
{
    PoolConfigIdentity c;
    memset(&c, 0, sizeof(c));
    c.primary = ep(host, port, user);
    c.fallback_enabled = false;
    c.chain = chain;
    c.profile_id[0] = '\0';
    return c;
}

static PoolSessionRequest valid_req(void)
{
    PoolSessionRequest r;
    memset(&r, 0, sizeof(r));
    r.model_version = POOL_SESSION_MODEL_VERSION;
    r.session_id = 42u;
    r.duration_s = 3600u;
    r.password_policy = POOL_SESSION_PW_KEEP_CURRENT;
    r.source = cfg(POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    r.target = cfg(POOL_CHAIN_BITCOIN_CASH, "bch.example", 3333, "acct.worker");
    strncpy(r.board_version, POOL_SESSION_SUPPORTED_BOARD, sizeof(r.board_version) - 1);
    strncpy(r.asic_model, POOL_SESSION_SUPPORTED_ASIC, sizeof(r.asic_model) - 1);
    return r;
}

/* Transition in place: out_next may safely alias current (transition copies the
 * input into a local first), which avoids a second ~1 KB PoolSession on the
 * (small, 3.5 KB) test task stack. */
static PoolSessionOutcome step_evt(PoolSession *s, PoolSessionEventType t)
{
    PoolSessionEvent e = { .type = t, .session_id = s->session_id, .request = NULL };
    return pool_session_transition(s, &e, s);
}

static PoolSessionOutcome do_create(PoolSession *s, const PoolSessionRequest *req)
{
    PoolSessionEvent e = { .type = POOL_EVT_CREATE_REQUESTED, .session_id = req->session_id, .request = req };
    return pool_session_transition(s, &e, s);
}

/* Drive the FSM to TARGET_ACTIVE via the happy path.
 * The request is file-static (not a ~1KB stack local): QEMU tests run
 * sequentially in one task with a bounded (8 KB) stack, and these driver
 * helpers nest, so keeping the large PoolSessionRequest off-stack avoids
 * overflowing the task stack. */
static PoolSessionRequest g_scratch_req;

static PoolSession session_target_active(void)
{
    PoolSession s;
    pool_session_init(&s);
    g_scratch_req = valid_req();
    do_create(&s, &g_scratch_req);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    step_evt(&s, POOL_EVT_TARGET_APPLY_REQUESTED);
    step_evt(&s, POOL_EVT_TARGET_RESTART_STARTED);
    step_evt(&s, POOL_EVT_TARGET_RESTART_COMPLETE);
    step_evt(&s, POOL_EVT_TARGET_CONNECTION_OBSERVED);
    step_evt(&s, POOL_EVT_TARGET_MINING_OBSERVED);
    step_evt(&s, POOL_EVT_TARGET_HOST_VERIFIED);
    return s;
}

/* A plausible session forced into an arbitrary state for classification/property tests. */
static PoolSession base_in(PoolSessionState st)
{
    PoolSession s;
    pool_session_init(&s);
    s.session_id = 42u;
    s.duration_s = 3600u;
    s.source = cfg(POOL_CHAIN_BITCOIN, "src.example", 3333, "srcuser");
    s.target = cfg(POOL_CHAIN_BITCOIN_CASH, "tgt.example", 3334, "tgtuser");
    s.state = st;
    return s;
}

/* ================================================================= */
/* A. Model tests                                                     */
/* ================================================================= */

TEST_CASE("model: init produces a clean IDLE session", "[pool_session]")
{
    PoolSession s;
    memset(&s, 0xAA, sizeof(s));
    pool_session_init(&s);
    TEST_ASSERT_EQUAL(POOL_STATE_IDLE, s.state);
    TEST_ASSERT_EQUAL_UINT32(POOL_SESSION_MODEL_VERSION, s.model_version);
    TEST_ASSERT_EQUAL_UINT32(0u, s.session_id);
    TEST_ASSERT_EQUAL_UINT32(0u, s.generation);
    TEST_ASSERT_EQUAL(POOL_SESSION_PW_KEEP_CURRENT, s.password_policy);
    TEST_ASSERT_EQUAL(ERR_NONE, s.last_error);
    TEST_ASSERT_EQUAL(POOL_STATE_IDLE, s.terminal_result);
    TEST_ASSERT_FALSE(s.cancel_requested);
    TEST_ASSERT_FALSE(s.restore_requested);
}

TEST_CASE("model: identity buffers hold max-length strings, NUL-bounded", "[pool_session]")
{
    PoolEndpoint e;
    memset(&e, 0, sizeof(e));
    memset(e.host, 'h', sizeof(e.host) - 1);
    memset(e.user, 'u', sizeof(e.user) - 1);
    e.port = 65535;
    e.protocol = POOL_PROTO_STRATUM_V2;
    /* strings are NUL terminated within their buffers */
    TEST_ASSERT_EQUAL(sizeof(e.host) - 1, strlen(e.host));
    TEST_ASSERT_EQUAL(sizeof(e.user) - 1, strlen(e.user));
    PoolEndpoint b = e;
    TEST_ASSERT_TRUE(pool_endpoint_equal(&e, &b));
}

TEST_CASE("model: boundary ports validate correctly", "[pool_session]")
{
    PoolSessionRequest r = valid_req();
    r.target.primary.port = 0;                 /* invalid */
    TEST_ASSERT_EQUAL(ERR_TARGET_IDENTITY_INVALID, pool_session_validate_request(&r));
    r = valid_req();
    r.target.primary.port = 65535;             /* valid */
    TEST_ASSERT_EQUAL(ERR_NONE, pool_session_validate_request(&r));
    r.target.primary.port = 1;                 /* valid */
    TEST_ASSERT_EQUAL(ERR_NONE, pool_session_validate_request(&r));
}

TEST_CASE("model: explicit chains are all accepted, invalid chain rejected", "[pool_session]")
{
    PoolSessionRequest r = valid_req();
    r.source.chain = POOL_CHAIN_CUSTOM_UNKNOWN;
    r.target.chain = POOL_CHAIN_BITCOIN_CASH;
    TEST_ASSERT_EQUAL(ERR_NONE, pool_session_validate_request(&r));
    r.target.chain = (PoolChainType)99;        /* invalid enum */
    TEST_ASSERT_EQUAL(ERR_TARGET_IDENTITY_INVALID, pool_session_validate_request(&r));
}

TEST_CASE("model: schema version mismatch rejected", "[pool_session]")
{
    PoolSessionRequest r = valid_req();
    r.model_version = POOL_SESSION_MODEL_VERSION + 1u;
    TEST_ASSERT_EQUAL(ERR_SCHEMA_UNSUPPORTED, pool_session_validate_request(&r));
}

/* ================================================================= */
/* B. Request validation                                              */
/* ================================================================= */

TEST_CASE("validate: 14 minutes rejected, 15 min and 24 h accepted", "[pool_session]")
{
    PoolSessionRequest r = valid_req();
    r.duration_s = 14u * 60u;                  /* 840 s */
    TEST_ASSERT_EQUAL(ERR_INVALID_DURATION, pool_session_validate_request(&r));
    r.duration_s = POOL_SESSION_MIN_DURATION_S; /* 900 s */
    TEST_ASSERT_EQUAL(ERR_NONE, pool_session_validate_request(&r));
    r.duration_s = POOL_SESSION_MAX_DURATION_S; /* 86400 s */
    TEST_ASSERT_EQUAL(ERR_NONE, pool_session_validate_request(&r));
}

TEST_CASE("validate: greater than 24 h rejected", "[pool_session]")
{
    PoolSessionRequest r = valid_req();
    r.duration_s = POOL_SESSION_MAX_DURATION_S + 1u;
    TEST_ASSERT_EQUAL(ERR_INVALID_DURATION, pool_session_validate_request(&r));
}

TEST_CASE("validate: minutes->seconds is overflow safe", "[pool_session]")
{
    uint32_t out = 0;
    TEST_ASSERT_TRUE(pool_session_minutes_to_seconds(15u, &out));
    TEST_ASSERT_EQUAL_UINT32(900u, out);
    TEST_ASSERT_TRUE(pool_session_minutes_to_seconds(1440u, &out)); /* 24 h */
    TEST_ASSERT_EQUAL_UINT32(86400u, out);
    TEST_ASSERT_FALSE(pool_session_minutes_to_seconds(1441u, &out));      /* > max   */
    TEST_ASSERT_FALSE(pool_session_minutes_to_seconds(0xFFFFFFFFu, &out));/* overflow */
    TEST_ASSERT_FALSE(pool_session_minutes_to_seconds(100000000u, &out));
    TEST_ASSERT_FALSE(pool_session_minutes_to_seconds(15u, NULL));
}

TEST_CASE("validate: unsupported board and non-BM1370 rejected", "[pool_session]")
{
    PoolSessionRequest r = valid_req();
    strncpy(r.board_version, "401", sizeof(r.board_version) - 1);
    TEST_ASSERT_EQUAL(ERR_UNSUPPORTED_BOARD, pool_session_validate_request(&r));
    r = valid_req();
    strncpy(r.asic_model, "BM1368", sizeof(r.asic_model) - 1);
    TEST_ASSERT_EQUAL(ERR_UNSUPPORTED_BOARD, pool_session_validate_request(&r));
}

TEST_CASE("validate: replacement password mode rejected", "[pool_session]")
{
    PoolSessionRequest r = valid_req();
    r.password_policy = POOL_SESSION_PW_REPLACE;
    TEST_ASSERT_EQUAL(ERR_PW_MODE_UNSUPPORTED, pool_session_validate_request(&r));
}

TEST_CASE("validate: empty host and invalid fallback rejected", "[pool_session]")
{
    PoolSessionRequest r = valid_req();
    r.target.primary.host[0] = '\0';
    TEST_ASSERT_EQUAL(ERR_TARGET_IDENTITY_INVALID, pool_session_validate_request(&r));
    r = valid_req();
    r.target.fallback_enabled = true;          /* fallback host empty -> invalid */
    TEST_ASSERT_EQUAL(ERR_TARGET_IDENTITY_INVALID, pool_session_validate_request(&r));
    r.target.fallback = ep("bch-fb.example", 3333, "acct.worker");
    TEST_ASSERT_EQUAL(ERR_NONE, pool_session_validate_request(&r));
}

TEST_CASE("validate: exact source==target rejected as no-op", "[pool_session]")
{
    PoolSessionRequest r = valid_req();
    r.target = r.source;                       /* identical operational identity */
    TEST_ASSERT_EQUAL(ERR_TARGET_EQUALS_SOURCE, pool_session_validate_request(&r));
}

TEST_CASE("validate: same-chain different pool accepted", "[pool_session]")
{
    PoolSessionRequest r = valid_req();
    r.source = cfg(POOL_CHAIN_BITCOIN, "btc-a.example", 3333, "acct.worker");
    r.target = cfg(POOL_CHAIN_BITCOIN, "btc-b.example", 3333, "acct.worker");
    TEST_ASSERT_EQUAL(ERR_NONE, pool_session_validate_request(&r));
}

TEST_CASE("validate: Custom/Unknown transitions accepted both directions", "[pool_session]")
{
    PoolSessionRequest r = valid_req();
    r.source = cfg(POOL_CHAIN_CUSTOM_UNKNOWN, "custom.example", 3333, "acct.worker");
    r.target = cfg(POOL_CHAIN_BITCOIN_CASH, "bch.example", 3333, "acct.worker");
    TEST_ASSERT_EQUAL(ERR_NONE, pool_session_validate_request(&r));
    r.source = cfg(POOL_CHAIN_BITCOIN_CASH, "bch.example", 3333, "acct.worker");
    r.target = cfg(POOL_CHAIN_CUSTOM_UNKNOWN, "custom.example", 3333, "acct.worker");
    TEST_ASSERT_EQUAL(ERR_NONE, pool_session_validate_request(&r));
}

TEST_CASE("validate: NULL request and zero session id rejected", "[pool_session]")
{
    TEST_ASSERT_EQUAL(ERR_INVALID_REQUEST, pool_session_validate_request(NULL));
    PoolSessionRequest r = valid_req();
    r.session_id = 0u;
    TEST_ASSERT_EQUAL(ERR_INVALID_REQUEST, pool_session_validate_request(&r));
}

/* ================================================================= */
/* C. State classification (every state)                              */
/* ================================================================= */

TEST_CASE("classify: persistent/terminal/active over all states", "[pool_session]")
{
    for (int i = 0; i < POOL_STATE__COUNT; i++) {
        PoolSessionState st = (PoolSessionState)i;
        bool term = pool_state_is_terminal(st);
        bool active = pool_state_is_active_session(st);
        if (term) TEST_ASSERT_FALSE(active);           /* terminal is never active */
        if (st == POOL_STATE_IDLE) TEST_ASSERT_FALSE(active);
    }
    /* spot checks */
    TEST_ASSERT_TRUE(pool_state_is_persistent(POOL_STATE_TARGET_ACTIVE));
    TEST_ASSERT_FALSE(pool_state_is_persistent(POOL_STATE_VERIFYING_TARGET));
    TEST_ASSERT_FALSE(pool_state_is_persistent(POOL_STATE_PREPARING));
    TEST_ASSERT_TRUE(pool_state_is_terminal(POOL_STATE_COMPLETE));
    TEST_ASSERT_TRUE(pool_state_is_terminal(POOL_STATE_CANCELLED));
    TEST_ASSERT_TRUE(pool_state_is_terminal(POOL_STATE_RESTORE_FAILED));
    TEST_ASSERT_TRUE(pool_state_is_terminal(POOL_STATE_RECOVERY_REQUIRED));
    TEST_ASSERT_FALSE(pool_state_is_terminal(POOL_STATE_TARGET_FAILED));
    TEST_ASSERT_FALSE(pool_state_is_terminal(POOL_STATE_INTERRUPTED));
}

TEST_CASE("classify: OTA and pool-change blocking are obligation-aware", "[pool_session]")
{
    PoolSession s = base_in(POOL_STATE_IDLE);
    TEST_ASSERT_FALSE(pool_session_blocks_ota(&s));
    TEST_ASSERT_FALSE(pool_session_blocks_manual_pool_change(&s));

    s = base_in(POOL_STATE_TARGET_ACTIVE);
    s.restore_required = true;
    TEST_ASSERT_TRUE(pool_session_blocks_ota(&s));
    TEST_ASSERT_TRUE(pool_session_blocks_manual_pool_change(&s));

    /* COMPLETE with the obligation discharged -> unblocked + acknowledgeable */
    s = base_in(POOL_STATE_COMPLETE);
    s.restore_required = false;
    TEST_ASSERT_FALSE(pool_session_blocks_ota(&s));
    TEST_ASSERT_TRUE(pool_session_is_acknowledgeable_terminal(&s));

    /* RESTORE_FAILED still owing a restore -> BLOCKED + NOT acknowledgeable */
    s = base_in(POOL_STATE_RESTORE_FAILED);
    s.restore_required = true;
    TEST_ASSERT_TRUE(pool_session_blocks_ota(&s));
    TEST_ASSERT_TRUE(pool_session_blocks_manual_pool_change(&s));
    TEST_ASSERT_FALSE(pool_session_is_acknowledgeable_terminal(&s));

    /* CANCELLED pre-mutation (no obligation) -> unblocked + acknowledgeable */
    s = base_in(POOL_STATE_CANCELLED);
    s.restore_required = false;
    TEST_ASSERT_FALSE(pool_session_blocks_ota(&s));
    TEST_ASSERT_TRUE(pool_session_is_acknowledgeable_terminal(&s));
}

TEST_CASE("classify: cancel / restore-now / source-snapshot / target-id", "[pool_session]")
{
    TEST_ASSERT_TRUE(pool_state_allows_cancel(POOL_STATE_PREPARING));
    TEST_ASSERT_TRUE(pool_state_allows_cancel(POOL_STATE_TARGET_ACTIVE));
    TEST_ASSERT_FALSE(pool_state_allows_cancel(POOL_STATE_RESTORE_DUE));
    TEST_ASSERT_FALSE(pool_state_allows_cancel(POOL_STATE_COMPLETE));

    TEST_ASSERT_TRUE(pool_state_allows_restore_now(POOL_STATE_TARGET_ACTIVE));
    TEST_ASSERT_TRUE(pool_state_allows_restore_now(POOL_STATE_TARGET_FAILED));
    TEST_ASSERT_FALSE(pool_state_allows_restore_now(POOL_STATE_IDLE));
    TEST_ASSERT_FALSE(pool_state_allows_restore_now(POOL_STATE_PREPARING));
    TEST_ASSERT_FALSE(pool_state_allows_restore_now(POOL_STATE_COMPLETE));

    TEST_ASSERT_FALSE(pool_state_requires_source_snapshot(POOL_STATE_IDLE));
    TEST_ASSERT_FALSE(pool_state_requires_source_snapshot(POOL_STATE_PREPARING));
    TEST_ASSERT_TRUE(pool_state_requires_source_snapshot(POOL_STATE_TARGET_SNAPSHOT_COMMITTED));

    TEST_ASSERT_FALSE(pool_state_requires_target_identity(POOL_STATE_IDLE));
    TEST_ASSERT_TRUE(pool_state_requires_target_identity(POOL_STATE_PREPARING));
}

TEST_CASE("classify: helpers are safe for out-of-range enum values", "[pool_session]")
{
    PoolSessionState bad = (PoolSessionState)999;
    TEST_ASSERT_FALSE(pool_state_is_persistent(bad));
    TEST_ASSERT_FALSE(pool_state_is_terminal(bad));
    TEST_ASSERT_FALSE(pool_state_is_active_session(bad));
    TEST_ASSERT_FALSE(pool_state_is_target_side(bad));
    TEST_ASSERT_FALSE(pool_state_is_restore_side(bad));
    TEST_ASSERT_FALSE(pool_state_allows_cancel(bad));
    TEST_ASSERT_FALSE(pool_state_allows_restore_now(bad));
    TEST_ASSERT_FALSE(pool_state_requires_source_snapshot(bad));
    TEST_ASSERT_FALSE(pool_state_requires_target_identity(bad));
}

/* ================================================================= */
/* D. Happy path                                                      */
/* ================================================================= */

TEST_CASE("happy: full target->active->restore->complete->idle", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();

    TEST_ASSERT_EQUAL(ERR_NONE, do_create(&s, &r).error);
    TEST_ASSERT_EQUAL(POOL_STATE_PREPARING, s.state);

    PoolSessionOutcome o = step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_SNAPSHOT_COMMITTED, s.state);
    TEST_ASSERT_EQUAL(POOL_SIDE_COMMIT_SOURCE_SNAPSHOT, o.side_effect);

    step_evt(&s, POOL_EVT_TARGET_APPLY_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_TARGET, s.state);
    step_evt(&s, POOL_EVT_TARGET_RESTART_STARTED);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTARTING_FOR_TARGET, s.state);
    step_evt(&s, POOL_EVT_TARGET_RESTART_COMPLETE);
    TEST_ASSERT_EQUAL(POOL_STATE_VERIFYING_TARGET, s.state);

    step_evt(&s, POOL_EVT_TARGET_CONNECTION_OBSERVED);
    TEST_ASSERT_EQUAL(POOL_STATE_VERIFYING_TARGET, s.state); /* not yet active */
    step_evt(&s, POOL_EVT_TARGET_MINING_OBSERVED);
    TEST_ASSERT_EQUAL(POOL_STATE_VERIFYING_TARGET, s.state);
    o = step_evt(&s, POOL_EVT_TARGET_HOST_VERIFIED);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_ACTIVE, s.state);    /* all three -> active */
    TEST_ASSERT_EQUAL(POOL_SIDE_ARM_MONOTONIC_DEADLINE, o.side_effect);
    TEST_ASSERT_TRUE(pool_session_restore_required(&s));     /* obligation stands */

    step_evt(&s, POOL_EVT_DEADLINE_REACHED);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, s.state);
    step_evt(&s, POOL_EVT_RESTORE_APPLY_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_RESTORE, s.state);
    step_evt(&s, POOL_EVT_RESTORE_RESTART_STARTED);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTARTING_FOR_RESTORE, s.state);
    step_evt(&s, POOL_EVT_RESTORE_RESTART_COMPLETE);
    TEST_ASSERT_EQUAL(POOL_STATE_VERIFYING_RESTORE, s.state);

    step_evt(&s, POOL_EVT_RESTORE_CONNECTION_OBSERVED);
    step_evt(&s, POOL_EVT_RESTORE_MINING_OBSERVED);
    o = step_evt(&s, POOL_EVT_RESTORE_IDENTITY_VERIFIED);
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, s.state);
    TEST_ASSERT_TRUE(o.terminal);
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, s.terminal_result);
    TEST_ASSERT_FALSE(pool_session_restore_required(&s));    /* obligation discharged */
    TEST_ASSERT_TRUE(pool_session_is_acknowledgeable_terminal(&s));

    o = step_evt(&s, POOL_EVT_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(POOL_STATE_IDLE, s.state);
    TEST_ASSERT_EQUAL(POOL_SIDE_CLEAR_SESSION_RECORD, o.side_effect);
}

TEST_CASE("happy: TARGET_ACTIVE requires connection + mining + identity (not share)", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    step_evt(&s, POOL_EVT_TARGET_APPLY_REQUESTED);
    step_evt(&s, POOL_EVT_TARGET_RESTART_STARTED);
    step_evt(&s, POOL_EVT_TARGET_RESTART_COMPLETE);
    /* only two of three -> still verifying */
    step_evt(&s, POOL_EVT_TARGET_CONNECTION_OBSERVED);
    step_evt(&s, POOL_EVT_TARGET_HOST_VERIFIED);
    TEST_ASSERT_EQUAL(POOL_STATE_VERIFYING_TARGET, s.state);
    step_evt(&s, POOL_EVT_TARGET_MINING_OBSERVED);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_ACTIVE, s.state);
}

/* ================================================================= */
/* E. Target failure paths                                            */
/* ================================================================= */

TEST_CASE("target: apply failures retry (bounded) then TARGET_FAILED", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    step_evt(&s, POOL_EVT_TARGET_APPLY_REQUESTED);
    for (int i = 0; i < POOL_SESSION_MAX_TARGET_APPLY_RETRIES; i++) {
        PoolSessionOutcome o = step_evt(&s, POOL_EVT_TARGET_APPLY_FAILED);
        TEST_ASSERT_TRUE(o.retry);
        TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_TARGET, s.state);
    }
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_TARGET_APPLY_FAILED); /* exhausted */
    TEST_ASSERT_FALSE(o.retry);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_FAILED, s.state);
    TEST_ASSERT_EQUAL(ERR_TARGET_APPLY, o.error);
    TEST_ASSERT_TRUE(s.retries.target_apply <= POOL_SESSION_MAX_TARGET_APPLY_RETRIES);
}

TEST_CASE("target: verify timeout retries then TARGET_FAILED", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    step_evt(&s, POOL_EVT_TARGET_APPLY_REQUESTED);
    step_evt(&s, POOL_EVT_TARGET_RESTART_STARTED);
    step_evt(&s, POOL_EVT_TARGET_RESTART_COMPLETE);
    for (int i = 0; i < POOL_SESSION_MAX_TARGET_VERIFY_RETRIES; i++) {
        PoolSessionOutcome o = step_evt(&s, POOL_EVT_TARGET_VERIFY_TIMEOUT);
        TEST_ASSERT_TRUE(o.retry);
        TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_TARGET, s.state); /* re-apply */
        step_evt(&s, POOL_EVT_TARGET_RESTART_STARTED);
        step_evt(&s, POOL_EVT_TARGET_RESTART_COMPLETE);
    }
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_TARGET_VERIFY_TIMEOUT);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_FAILED, s.state);
    TEST_ASSERT_EQUAL(ERR_TARGET_VERIFY_TIMEOUT, o.error);
}

TEST_CASE("target: restart retry exhaustion -> TARGET_FAILED", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    step_evt(&s, POOL_EVT_TARGET_APPLY_REQUESTED);
    step_evt(&s, POOL_EVT_TARGET_RESTART_STARTED);
    for (int i = 0; i < POOL_SESSION_MAX_TARGET_RESTART_RETRIES; i++) {
        PoolSessionOutcome o = step_evt(&s, POOL_EVT_TARGET_RETRY_REQUESTED);
        TEST_ASSERT_TRUE(o.retry);
        TEST_ASSERT_EQUAL(POOL_STATE_RESTARTING_FOR_TARGET, s.state);
    }
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_TARGET_RETRY_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_FAILED, s.state);
    TEST_ASSERT_EQUAL(ERR_TARGET_RESTART, o.error);
}

TEST_CASE("target: TARGET_FAILED auto-restores the source", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    step_evt(&s, POOL_EVT_TARGET_APPLY_REQUESTED);
    for (int i = 0; i <= POOL_SESSION_MAX_TARGET_APPLY_RETRIES; i++) {
        step_evt(&s, POOL_EVT_TARGET_APPLY_FAILED);
    }
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_FAILED, s.state);
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_RESTORE_APPLY_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_RESTORE, s.state);
    TEST_ASSERT_EQUAL(POOL_SIDE_APPLY_SOURCE_CONFIGURATION, o.side_effect);
}

/* ================================================================= */
/* F. Restore failure paths                                           */
/* ================================================================= */

TEST_CASE("restore: apply failures retry then RESTORE_FAILED", "[pool_session]")
{
    PoolSession s = session_target_active();
    step_evt(&s, POOL_EVT_DEADLINE_REACHED);
    step_evt(&s, POOL_EVT_RESTORE_APPLY_REQUESTED);
    for (int i = 0; i < POOL_SESSION_MAX_RESTORE_APPLY_RETRIES; i++) {
        PoolSessionOutcome o = step_evt(&s, POOL_EVT_RESTORE_APPLY_FAILED);
        TEST_ASSERT_TRUE(o.retry);
        TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_RESTORE, s.state);
    }
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_RESTORE_APPLY_FAILED);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_FAILED, s.state);
    TEST_ASSERT_EQUAL(ERR_RESTORE_APPLY, o.error);
    TEST_ASSERT_TRUE(pool_state_is_terminal(s.state));
}

TEST_CASE("restore: verify timeout retries then RESTORE_FAILED", "[pool_session]")
{
    PoolSession s = session_target_active();
    step_evt(&s, POOL_EVT_DEADLINE_REACHED);
    step_evt(&s, POOL_EVT_RESTORE_APPLY_REQUESTED);
    step_evt(&s, POOL_EVT_RESTORE_RESTART_STARTED);
    step_evt(&s, POOL_EVT_RESTORE_RESTART_COMPLETE);
    for (int i = 0; i < POOL_SESSION_MAX_RESTORE_VERIFY_RETRIES; i++) {
        PoolSessionOutcome o = step_evt(&s, POOL_EVT_RESTORE_VERIFY_TIMEOUT);
        TEST_ASSERT_TRUE(o.retry);
        TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_RESTORE, s.state);
        step_evt(&s, POOL_EVT_RESTORE_RESTART_STARTED);
        step_evt(&s, POOL_EVT_RESTORE_RESTART_COMPLETE);
    }
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_RESTORE_VERIFY_TIMEOUT);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_FAILED, s.state);
    TEST_ASSERT_EQUAL(ERR_RESTORE_VERIFY_TIMEOUT, o.error);
}

TEST_CASE("restore: RESTORE_FAILED can be manually re-attempted with reset budget", "[pool_session]")
{
    PoolSession s = session_target_active();
    step_evt(&s, POOL_EVT_DEADLINE_REACHED);
    step_evt(&s, POOL_EVT_RESTORE_APPLY_REQUESTED);
    for (int i = 0; i <= POOL_SESSION_MAX_RESTORE_APPLY_RETRIES; i++) {
        step_evt(&s, POOL_EVT_RESTORE_APPLY_FAILED);
    }
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_FAILED, s.state);
    step_evt(&s, POOL_EVT_RESTORE_NOW_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, s.state);
    TEST_ASSERT_EQUAL_UINT8(0, s.retries.restore_apply); /* budget reset */
}

/* ================================================================= */
/* G. Cancel                                                          */
/* ================================================================= */

TEST_CASE("cancel: before target mutation -> CANCELLED (no restore)", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_SNAPSHOT_COMMITTED, s.state);
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_CANCEL_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_CANCELLED, s.state);
    TEST_ASSERT_TRUE(o.terminal);
}

TEST_CASE("cancel: during target apply -> drives restore", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    step_evt(&s, POOL_EVT_TARGET_APPLY_REQUESTED);
    step_evt(&s, POOL_EVT_CANCEL_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, s.state);
    TEST_ASSERT_TRUE(s.cancel_requested);
}

TEST_CASE("cancel: while target active -> drives restore", "[pool_session]")
{
    PoolSession a = session_target_active();
    step_evt(&a, POOL_EVT_CANCEL_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, a.state);
    TEST_ASSERT_TRUE(a.cancel_requested);
}

TEST_CASE("cancel: duplicate cancel and cancel during restore do not abandon restore", "[pool_session]")
{
    PoolSession s = session_target_active();
    step_evt(&s, POOL_EVT_CANCEL_REQUESTED);       /* -> RESTORE_DUE */
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_CANCEL_REQUESTED); /* duplicate */
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, s.state);
    TEST_ASSERT_FALSE(o.changed);
    step_evt(&s, POOL_EVT_RESTORE_APPLY_REQUESTED);
    o = step_evt(&s, POOL_EVT_CANCEL_REQUESTED);   /* cancel during restore */
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_RESTORE, s.state);
    TEST_ASSERT_FALSE(o.changed);
}

TEST_CASE("cancel: in terminal state is a deterministic conflict", "[pool_session]")
{
    PoolSession s = base_in(POOL_STATE_COMPLETE);
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_CANCEL_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, s.state);
    TEST_ASSERT_EQUAL(ERR_STATE_CONFLICT, o.error);
    TEST_ASSERT_FALSE(o.changed);
}

/* ================================================================= */
/* H. Restore Now                                                     */
/* ================================================================= */

TEST_CASE("restore-now: idempotent and never abandons source", "[pool_session]")
{
    PoolSession s = session_target_active();
    PoolSession before = s;
    step_evt(&s, POOL_EVT_RESTORE_NOW_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, s.state);
    TEST_ASSERT_TRUE(s.restore_requested);
    /* duplicate -> no-op */
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_RESTORE_NOW_REQUESTED);
    TEST_ASSERT_FALSE(o.changed);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, s.state);
    /* source snapshot never cleared */
    TEST_ASSERT_EQUAL_MEMORY(&before.source, &s.source, sizeof(before.source));
}

TEST_CASE("restore-now: invalid in IDLE, no-op after COMPLETE", "[pool_session]")
{
    PoolSession idle;
    pool_session_init(&idle);
    PoolSessionOutcome o = step_evt(&idle, POOL_EVT_RESTORE_NOW_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_IDLE, idle.state);
    TEST_ASSERT_EQUAL(ERR_NO_ACTIVE_SESSION, o.error);

    PoolSession done = base_in(POOL_STATE_COMPLETE);
    o = step_evt(&done, POOL_EVT_RESTORE_NOW_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, done.state);
    TEST_ASSERT_EQUAL(ERR_NONE, o.error);
    TEST_ASSERT_FALSE(o.changed);
}

TEST_CASE("restore-now: pre-apply behaves as cancel", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    step_evt(&s, POOL_EVT_RESTORE_NOW_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_CANCELLED, s.state);
}

/* ================================================================= */
/* I. Idempotency                                                     */
/* ================================================================= */

TEST_CASE("idempotency: duplicate create same id returns existing", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    TEST_ASSERT_EQUAL(POOL_STATE_PREPARING, s.state);
    PoolSession before = s;
    PoolSessionOutcome o = do_create(&s, &r); /* same session_id 42 */
    TEST_ASSERT_FALSE(o.changed);
    TEST_ASSERT_EQUAL(ERR_NONE, o.error);
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(before));
}

TEST_CASE("idempotency: duplicate create different id -> conflict", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    PoolSessionRequest r2 = valid_req();
    r2.session_id = 77u;
    PoolSessionOutcome o = do_create(&s, &r2);
    TEST_ASSERT_EQUAL(ERR_SESSION_ALREADY_ACTIVE, o.error);
    TEST_ASSERT_FALSE(o.changed);
    TEST_ASSERT_EQUAL_UINT32(42u, s.session_id); /* unchanged */
}

TEST_CASE("idempotency: repeated verify observe is a no-op", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    step_evt(&s, POOL_EVT_TARGET_APPLY_REQUESTED);
    step_evt(&s, POOL_EVT_TARGET_RESTART_STARTED);
    step_evt(&s, POOL_EVT_TARGET_RESTART_COMPLETE);
    step_evt(&s, POOL_EVT_TARGET_CONNECTION_OBSERVED);
    PoolSession before = s;
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_TARGET_CONNECTION_OBSERVED); /* dup */
    TEST_ASSERT_FALSE(o.changed);
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(before));
}

TEST_CASE("idempotency: ack only from terminal; second ack has no session", "[pool_session]")
{
    PoolSession s = base_in(POOL_STATE_CANCELLED);
    s.terminal_result = POOL_STATE_CANCELLED;
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(POOL_STATE_IDLE, s.state);
    TEST_ASSERT_TRUE(o.changed);
    o = step_evt(&s, POOL_EVT_ACKNOWLEDGE_TERMINAL); /* now IDLE */
    TEST_ASSERT_EQUAL(ERR_STATE_CONFLICT, o.error);
    TEST_ASSERT_FALSE(o.changed);
}

/* ================================================================= */
/* J. Recovery safety                                                 */
/* ================================================================= */

static bool is_pool_mutation_side_effect(PoolSessionSideEffect se)
{
    switch (se) {
        case POOL_SIDE_COMMIT_SOURCE_SNAPSHOT:
        case POOL_SIDE_APPLY_TARGET_CONFIGURATION:
        case POOL_SIDE_RESTART_FOR_TARGET:
        case POOL_SIDE_APPLY_SOURCE_CONFIGURATION:
        case POOL_SIDE_RESTART_FOR_RESTORE:
        case POOL_SIDE_CLEAR_SESSION_RECORD:
            return true;
        default:
            return false;
    }
}

TEST_CASE("recovery: corrupt/unsupported record -> RECOVERY_REQUIRED, no mutation", "[pool_session]")
{
    for (int i = 0; i < POOL_STATE__COUNT; i++) {
        PoolSession s = base_in((PoolSessionState)i);
        PoolSession src_before = s;
        PoolSessionOutcome o = step_evt(&s, POOL_EVT_RECORD_CORRUPT);
        TEST_ASSERT_FALSE(is_pool_mutation_side_effect(o.side_effect));
        if (i != POOL_STATE_IDLE) {
            TEST_ASSERT_EQUAL(POOL_STATE_RECOVERY_REQUIRED, s.state);
            /* source/target preserved for diagnostics */
            TEST_ASSERT_EQUAL_MEMORY(&src_before.source, &s.source, sizeof(s.source));
            TEST_ASSERT_EQUAL_MEMORY(&src_before.target, &s.target, sizeof(s.target));
        }
        s = base_in((PoolSessionState)i);
        o = step_evt(&s, POOL_EVT_RECORD_SCHEMA_UNSUPPORTED);
        TEST_ASSERT_FALSE(is_pool_mutation_side_effect(o.side_effect));
    }
}

TEST_CASE("recovery: device restart in any state emits no pool mutation", "[pool_session]")
{
    for (int i = 0; i < POOL_STATE__COUNT; i++) {
        PoolSession s = base_in((PoolSessionState)i);
        PoolSession src_before = s;
        PoolSessionOutcome o = step_evt(&s, POOL_EVT_DEVICE_RESTART_OBSERVED);
        TEST_ASSERT_FALSE(is_pool_mutation_side_effect(o.side_effect));
        /* source identity is never overwritten by a restart */
        TEST_ASSERT_EQUAL_MEMORY(&src_before.source, &s.source, sizeof(s.source));
        TEST_ASSERT_TRUE(o.next_state < POOL_STATE__COUNT);
    }
}

/* (Source/target immutability across the full state x event matrix is folded
 * into the section-L property test below, which sweeps the same matrix once.) */

/* ================================================================= */
/* K. Privacy                                                         */
/* ================================================================= */

TEST_CASE("privacy: diagnostic strings never leak a planted secret/account", "[pool_session]")
{
    /* Plant a distinctive fake secret in account/host fields. */
    PoolSession s = base_in(POOL_STATE_APPLYING_TARGET);
    strncpy(s.target.primary.user, "PLANTEDSECRET.worker", sizeof(s.target.primary.user) - 1);
    strncpy(s.source.primary.user, "PLANTEDSECRET.worker", sizeof(s.source.primary.user) - 1);

    /* Drive an error transition and check its diagnostic strings carry no data. */
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_RESTORE_APPLY_REQUESTED); /* illegal here */
    const char *estr = pool_session_error_str(o.error);
    const char *sstr = pool_session_state_str(o.next_state);
    const char *fstr = pool_session_side_effect_str(o.side_effect);
    TEST_ASSERT_NULL(strstr(estr, "PLANTED"));
    TEST_ASSERT_NULL(strstr(sstr, "PLANTED"));
    TEST_ASSERT_NULL(strstr(fstr, "PLANTED"));
    /* every error code maps to a token that contains no account/host material */
    for (int i = 0; i < POOL_SESSION_ERR__COUNT; i++) {
        const char *t = pool_session_error_str((PoolSessionError)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_NULL(strstr(t, "PLANTED"));
        TEST_ASSERT_NULL(strstr(t, "."));      /* tokens are ALL_CAPS, no host dots */
    }
}

/* ================================================================= */
/* L. Property-style exhaustive transition test                       */
/* ================================================================= */

TEST_CASE("property: every (state,event) is safe, bounded and source-immutable", "[pool_session]")
{
    for (int st = 0; st < POOL_STATE__COUNT; st++) {
        PoolSession base = base_in((PoolSessionState)st);
        for (int ev = 0; ev < POOL_EVT__COUNT; ev++) {
            PoolSessionEvent e = { .type = (PoolSessionEventType)ev, .session_id = base.session_id, .request = NULL };
            PoolSession out;
            PoolSessionOutcome o = pool_session_transition(&base, &e, &out);

            /* valid enum outputs */
            TEST_ASSERT_TRUE(o.next_state < POOL_STATE__COUNT);
            TEST_ASSERT_TRUE(o.side_effect < POOL_SIDE__COUNT);
            TEST_ASSERT_TRUE(o.error < POOL_SESSION_ERR__COUNT);
            TEST_ASSERT_EQUAL(o.next_state, out.state);

            /* illegal transition never claims a change */
            if (o.error == ERR_ILLEGAL_TRANSITION) TEST_ASSERT_FALSE(o.changed);
            /* no-op leaves the session byte-identical */
            if (!o.changed) TEST_ASSERT_EQUAL_MEMORY(&base, &out, sizeof(base));

            /* retry counters never wrap past their maxima */
            TEST_ASSERT_TRUE(out.retries.target_apply    <= POOL_SESSION_MAX_TARGET_APPLY_RETRIES);
            TEST_ASSERT_TRUE(out.retries.target_restart  <= POOL_SESSION_MAX_TARGET_RESTART_RETRIES);
            TEST_ASSERT_TRUE(out.retries.target_verify   <= POOL_SESSION_MAX_TARGET_VERIFY_RETRIES);
            TEST_ASSERT_TRUE(out.retries.restore_apply   <= POOL_SESSION_MAX_RESTORE_APPLY_RETRIES);
            TEST_ASSERT_TRUE(out.retries.restore_restart <= POOL_SESSION_MAX_RESTORE_RESTART_RETRIES);
            TEST_ASSERT_TRUE(out.retries.restore_verify  <= POOL_SESSION_MAX_RESTORE_VERIFY_RETRIES);

            /* restore_required can only be established by the apply-target intent */
            if (!base.restore_required && out.restore_required) {
                TEST_ASSERT_EQUAL(POOL_SIDE_APPLY_TARGET_CONFIGURATION, o.side_effect);
            }

            /* source/target identity never overwritten (target never leaks into
               source). CREATE builds a session and ACK clears it, so skip those. */
            if (ev != POOL_EVT_CREATE_REQUESTED && ev != POOL_EVT_ACKNOWLEDGE_TERMINAL) {
                TEST_ASSERT_EQUAL_MEMORY(&base.source, &out.source, sizeof(base.source));
                TEST_ASSERT_EQUAL_MEMORY(&base.target, &out.target, sizeof(base.target));
            }
        }
        /* the input session is never mutated by any event (once per state) */
        PoolSession fresh = base_in((PoolSessionState)st);
        TEST_ASSERT_EQUAL_MEMORY(&fresh, &base, sizeof(fresh));
    }
}

TEST_CASE("property: transitions are deterministic (representative pairs)", "[pool_session]")
{
    /* Determinism over the interesting transitions. Running the full matrix
       twice is unnecessarily expensive under QEMU emulation. */
    const struct { PoolSessionState st; PoolSessionEventType ev; } pairs[] = {
        { POOL_STATE_IDLE,                      POOL_EVT_CREATE_REQUESTED },
        { POOL_STATE_PREPARING,                 POOL_EVT_SOURCE_SNAPSHOT_COMMITTED },
        { POOL_STATE_TARGET_SNAPSHOT_COMMITTED, POOL_EVT_TARGET_APPLY_REQUESTED },
        { POOL_STATE_APPLYING_TARGET,           POOL_EVT_TARGET_APPLY_FAILED },
        { POOL_STATE_VERIFYING_TARGET,          POOL_EVT_TARGET_CONNECTION_OBSERVED },
        { POOL_STATE_TARGET_ACTIVE,             POOL_EVT_DEADLINE_REACHED },
        { POOL_STATE_RESTORE_DUE,               POOL_EVT_RESTORE_APPLY_REQUESTED },
        { POOL_STATE_VERIFYING_RESTORE,         POOL_EVT_RESTORE_IDENTITY_VERIFIED },
        { POOL_STATE_TARGET_FAILED,             POOL_EVT_RESTORE_APPLY_REQUESTED },
        { POOL_STATE_RESTORE_FAILED,            POOL_EVT_ACKNOWLEDGE_TERMINAL },
        { POOL_STATE_COMPLETE,                  POOL_EVT_ACKNOWLEDGE_TERMINAL },
        { POOL_STATE_APPLYING_TARGET,           POOL_EVT_DEVICE_RESTART_OBSERVED },
        { POOL_STATE_TARGET_ACTIVE,             POOL_EVT_RECORD_CORRUPT },
    };
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        PoolSession base = base_in(pairs[i].st);
        PoolSessionEvent e = { .type = pairs[i].ev, .session_id = base.session_id, .request = NULL };
        PoolSession a, b;
        PoolSessionOutcome oa = pool_session_transition(&base, &e, &a);
        PoolSessionOutcome ob = pool_session_transition(&base, &e, &b);
        TEST_ASSERT_EQUAL_MEMORY(&oa, &ob, sizeof(oa));
        TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
    }
}

TEST_CASE("property: terminal states only leave via acknowledge", "[pool_session]")
{
    const PoolSessionState terminals[] = {
        POOL_STATE_COMPLETE, POOL_STATE_CANCELLED,
        POOL_STATE_RESTORE_FAILED, POOL_STATE_RECOVERY_REQUIRED
    };
    for (unsigned t = 0; t < sizeof(terminals) / sizeof(terminals[0]); t++) {
        for (int ev = 0; ev < POOL_EVT__COUNT; ev++) {
            if (ev == POOL_EVT_ACKNOWLEDGE_TERMINAL) continue;
            /* RESTORE_FAILED / RECOVERY_REQUIRED accept a manual restore re-attempt */
            if ((terminals[t] == POOL_STATE_RESTORE_FAILED ||
                 terminals[t] == POOL_STATE_RECOVERY_REQUIRED) &&
                ev == POOL_EVT_RESTORE_NOW_REQUESTED) continue;
            /* corrupt/schema/restart/interrupt are cross-cutting and handled elsewhere */
            if (ev == POOL_EVT_RECORD_CORRUPT || ev == POOL_EVT_RECORD_SCHEMA_UNSUPPORTED ||
                ev == POOL_EVT_DEVICE_RESTART_OBSERVED || ev == POOL_EVT_INTERRUPT_OBSERVED ||
                ev == POOL_EVT_CREATE_REQUESTED) continue;
            PoolSession s = base_in(terminals[t]);
            PoolSessionOutcome o = step_evt(&s, (PoolSessionEventType)ev);
            TEST_ASSERT_EQUAL(terminals[t], s.state); /* stays terminal */
            TEST_ASSERT_FALSE(o.changed);
        }
    }
}

/* ================================================================= */
/* M. Restore obligation — post-mutation safety                       */
/* ================================================================= */

/* Drive to APPLYING_TARGET: the first APPLY_TARGET_CONFIGURATION intent.
 * Uses the file-static request (see session_target_active) to stay stack-light. */
static PoolSession session_applying_target(void)
{
    PoolSession s;
    pool_session_init(&s);
    g_scratch_req = valid_req();
    do_create(&s, &g_scratch_req);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    step_evt(&s, POOL_EVT_TARGET_APPLY_REQUESTED);
    return s;
}

/* Drive to a post-mutation TARGET_FAILED via apply-retry exhaustion. */
static PoolSession session_target_failed(void)
{
    PoolSession s = session_applying_target();
    for (int i = 0; i <= POOL_SESSION_MAX_TARGET_APPLY_RETRIES; i++) {
        step_evt(&s, POOL_EVT_TARGET_APPLY_FAILED);
    }
    return s;
}

/* Drive to RESTORE_FAILED via restore-apply-retry exhaustion. */
static PoolSession session_restore_failed(void)
{
    PoolSession s = session_target_active();
    step_evt(&s, POOL_EVT_DEADLINE_REACHED);
    step_evt(&s, POOL_EVT_RESTORE_APPLY_REQUESTED);
    for (int i = 0; i <= POOL_SESSION_MAX_RESTORE_APPLY_RETRIES; i++) {
        step_evt(&s, POOL_EVT_RESTORE_APPLY_FAILED);
    }
    return s;
}

TEST_CASE("obligation: APPLY_TARGET_CONFIGURATION establishes restore_required", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest r = valid_req();
    do_create(&s, &r);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    TEST_ASSERT_FALSE(pool_session_restore_required(&s)); /* not applied yet */
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_TARGET_APPLY_REQUESTED);
    /* obligation is present in the returned session, before the intent runs */
    TEST_ASSERT_EQUAL(POOL_SIDE_APPLY_TARGET_CONFIGURATION, o.side_effect);
    TEST_ASSERT_TRUE(pool_session_restore_required(&s));
}

TEST_CASE("obligation: post-mutation apply failure -> restore, not clearable", "[pool_session]")
{
    PoolSession s = session_target_failed();
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_FAILED, s.state);
    TEST_ASSERT_TRUE(pool_session_restore_required(&s));
    TEST_ASSERT_FALSE(pool_session_is_acknowledgeable_terminal(&s));
    /* ACK refused */
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(ERR_STATE_CONFLICT, o.error);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_FAILED, s.state);
    /* drives toward restore */
    o = step_evt(&s, POOL_EVT_RESTORE_APPLY_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_APPLYING_RESTORE, s.state);
    TEST_ASSERT_TRUE(pool_session_restore_required(&s));
}

TEST_CASE("obligation: target restart failure -> restore, obligation kept", "[pool_session]")
{
    PoolSession s = session_applying_target();
    step_evt(&s, POOL_EVT_TARGET_RESTART_STARTED);
    for (int i = 0; i <= POOL_SESSION_MAX_TARGET_RESTART_RETRIES; i++) {
        step_evt(&s, POOL_EVT_TARGET_RETRY_REQUESTED);
    }
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_FAILED, s.state);
    TEST_ASSERT_TRUE(pool_session_restore_required(&s));
    TEST_ASSERT_FALSE(pool_session_is_acknowledgeable_terminal(&s));
}

TEST_CASE("obligation: verify timeout (incl host mismatch) -> restore, kept", "[pool_session]")
{
    /* A host that never verifies (identity mismatch) shows up as verify timeout. */
    PoolSession s = session_applying_target();
    step_evt(&s, POOL_EVT_TARGET_RESTART_STARTED);
    step_evt(&s, POOL_EVT_TARGET_RESTART_COMPLETE);
    for (int i = 0; i < POOL_SESSION_MAX_TARGET_VERIFY_RETRIES; i++) {
        step_evt(&s, POOL_EVT_TARGET_CONNECTION_OBSERVED);
        step_evt(&s, POOL_EVT_TARGET_MINING_OBSERVED);
        /* host identity never verified -> timeout re-applies */
        step_evt(&s, POOL_EVT_TARGET_VERIFY_TIMEOUT);
        step_evt(&s, POOL_EVT_TARGET_RESTART_STARTED);
        step_evt(&s, POOL_EVT_TARGET_RESTART_COMPLETE);
    }
    step_evt(&s, POOL_EVT_TARGET_VERIFY_TIMEOUT); /* exhausted */
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_FAILED, s.state);
    TEST_ASSERT_TRUE(pool_session_restore_required(&s));
    TEST_ASSERT_FALSE(pool_session_is_acknowledgeable_terminal(&s));
}

TEST_CASE("obligation: target retry exhaustion is not an acknowledgeable terminal", "[pool_session]")
{
    PoolSession s = session_target_failed(); /* exhausted apply retries */
    TEST_ASSERT_TRUE(s.retries.target_apply <= POOL_SESSION_MAX_TARGET_APPLY_RETRIES);
    TEST_ASSERT_FALSE(pool_session_is_acknowledgeable_terminal(&s));
    TEST_ASSERT_TRUE(pool_session_blocks_ota(&s));
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(ERR_STATE_CONFLICT, o.error);
}

TEST_CASE("obligation: restore retry exhaustion retains source + obligation", "[pool_session]")
{
    PoolConfigIdentity expect = cfg(POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    PoolSession s = session_restore_failed();
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_FAILED, s.state);
    TEST_ASSERT_TRUE(pool_session_restore_required(&s));
    TEST_ASSERT_EQUAL_MEMORY(&expect, &s.source, sizeof(expect)); /* source intact */
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(ERR_STATE_CONFLICT, o.error);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_FAILED, s.state);
}

TEST_CASE("obligation: ACK rejected from RESTORE_FAILED and RECOVERY_REQUIRED", "[pool_session]")
{
    PoolSession s = session_restore_failed();
    PoolSessionOutcome o = step_evt(&s, POOL_EVT_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(ERR_STATE_CONFLICT, o.error);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_FAILED, s.state);

    /* Post-mutation RECOVERY_REQUIRED (corrupt record during target apply). */
    PoolSession r = session_applying_target();
    step_evt(&r, POOL_EVT_RECORD_CORRUPT);
    TEST_ASSERT_EQUAL(POOL_STATE_RECOVERY_REQUIRED, r.state);
    TEST_ASSERT_TRUE(pool_session_restore_required(&r));
    o = step_evt(&r, POOL_EVT_ACKNOWLEDGE_TERMINAL);
    TEST_ASSERT_EQUAL(ERR_STATE_CONFLICT, o.error);
    TEST_ASSERT_EQUAL(POOL_STATE_RECOVERY_REQUIRED, r.state);
    /* a manual Restore Now may still retry restoration (bounded) */
    o = step_evt(&r, POOL_EVT_RESTORE_NOW_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, r.state);
    TEST_ASSERT_TRUE(pool_session_restore_required(&r));
}

TEST_CASE("obligation: CANCELLED reachable only before target mutation", "[pool_session]")
{
    PoolSession s;
    pool_session_init(&s);
    PoolSessionRequest rq = valid_req();
    do_create(&s, &rq);
    step_evt(&s, POOL_EVT_SOURCE_SNAPSHOT_COMMITTED);
    TEST_ASSERT_FALSE(pool_session_restore_required(&s));
    step_evt(&s, POOL_EVT_CANCEL_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_CANCELLED, s.state);
    TEST_ASSERT_FALSE(pool_session_restore_required(&s));
    TEST_ASSERT_TRUE(pool_session_is_acknowledgeable_terminal(&s));

    /* After mutation, cancel drives restore and never yields CANCELLED. */
    PoolSession a = session_applying_target();
    step_evt(&a, POOL_EVT_CANCEL_REQUESTED);
    TEST_ASSERT_EQUAL(POOL_STATE_RESTORE_DUE, a.state);
    TEST_ASSERT_TRUE(pool_session_restore_required(&a));
}

TEST_CASE("obligation: cleared only at COMPLETE (verified restore + mining)", "[pool_session]")
{
    PoolSession s = session_target_active();
    step_evt(&s, POOL_EVT_DEADLINE_REACHED);
    step_evt(&s, POOL_EVT_RESTORE_APPLY_REQUESTED);
    step_evt(&s, POOL_EVT_RESTORE_RESTART_STARTED);
    step_evt(&s, POOL_EVT_RESTORE_RESTART_COMPLETE);
    step_evt(&s, POOL_EVT_RESTORE_CONNECTION_OBSERVED);
    step_evt(&s, POOL_EVT_RESTORE_MINING_OBSERVED);
    TEST_ASSERT_EQUAL(POOL_STATE_VERIFYING_RESTORE, s.state);
    TEST_ASSERT_TRUE(pool_session_restore_required(&s)); /* identity not yet verified */
    step_evt(&s, POOL_EVT_RESTORE_IDENTITY_VERIFIED);
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, s.state);
    TEST_ASSERT_FALSE(pool_session_restore_required(&s)); /* cleared exactly here */
    TEST_ASSERT_TRUE(pool_session_is_acknowledgeable_terminal(&s));
}

TEST_CASE("obligation property: while owed, no event reaches IDLE/CANCELLED/ack; clears only at COMPLETE", "[pool_session]")
{
    const PoolSessionState post[] = {
        POOL_STATE_APPLYING_TARGET, POOL_STATE_RESTARTING_FOR_TARGET,
        POOL_STATE_VERIFYING_TARGET, POOL_STATE_TARGET_ACTIVE,
        POOL_STATE_TARGET_FAILED, POOL_STATE_RESTORE_DUE,
        POOL_STATE_APPLYING_RESTORE, POOL_STATE_RESTARTING_FOR_RESTORE,
        POOL_STATE_VERIFYING_RESTORE, POOL_STATE_RESTORE_FAILED,
        POOL_STATE_INTERRUPTED, POOL_STATE_RECOVERY_REQUIRED,
    };
    for (unsigned p = 0; p < sizeof(post) / sizeof(post[0]); p++) {
        for (int ev = 0; ev < POOL_EVT__COUNT; ev++) {
            PoolSession s = base_in(post[p]);
            s.restore_required = true;
            PoolSession out;
            PoolSessionEvent e = { .type = (PoolSessionEventType)ev, .session_id = s.session_id, .request = NULL };
            pool_session_transition(&s, &e, &out);
            if (!out.restore_required) {
                /* the obligation may clear ONLY by reaching COMPLETE */
                TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, out.state);
            } else {
                TEST_ASSERT_NOT_EQUAL(POOL_STATE_IDLE, out.state);
                TEST_ASSERT_NOT_EQUAL(POOL_STATE_CANCELLED, out.state);
                TEST_ASSERT_FALSE(pool_session_is_acknowledgeable_terminal(&out));
            }
        }
    }
}

TEST_CASE("obligation: source identity byte-identical through failure + recovery", "[pool_session]")
{
    PoolConfigIdentity expect = cfg(POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    PoolSession s = session_target_failed();
    TEST_ASSERT_EQUAL_MEMORY(&expect, &s.source, sizeof(expect));
    step_evt(&s, POOL_EVT_RESTORE_APPLY_REQUESTED);
    step_evt(&s, POOL_EVT_RESTORE_APPLY_FAILED);
    TEST_ASSERT_EQUAL_MEMORY(&expect, &s.source, sizeof(expect));
    step_evt(&s, POOL_EVT_RESTORE_RESTART_STARTED);
    step_evt(&s, POOL_EVT_DEVICE_RESTART_OBSERVED);
    TEST_ASSERT_EQUAL_MEMORY(&expect, &s.source, sizeof(expect));
    TEST_ASSERT_TRUE(pool_session_restore_required(&s));
}
