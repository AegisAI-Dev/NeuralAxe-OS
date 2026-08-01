/*
 * NeuralAxe timed pool sessions — bounded command transport and the
 * owner-task command processor (Phase 2M.1B, Gate B8).
 * See pool_session_command.h for the contract.
 */

#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include "pool_session_command.h"
#include "pool_session_api_status.h"
#include "pool_operation_coordinator.h"
#include "pool_operation_http_policy.h"
#include "pool_session_execution_core.h"
#include "pool_time.h"

static const char *TAG = "nx_pool_api";

/* Forward declarations for the create -> adoption -> cancel chain. */
static PoolApiCommandResult api_adopt_executor(PoolSessionApiProcessor *p);
static PoolApiCommandResult api_commit_transition(PoolSessionApiProcessor *p,
                                                  PoolSessionEventType type);

/*
 * ONE bounded static critical section guarding the mailbox and the
 * published status. It is taken ONLY around bounded pure operations —
 * never around an NVS operation, a coordinator call, an adapter call or a
 * log write — so no blocking work ever runs inside it.
 */
static portMUX_TYPE s_api_lock = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------ */
/* Bounded command validation                                          */
/* ------------------------------------------------------------------ */

static bool str_bounded_nonempty(const char *s, size_t cap)
{
    size_t i;

    if (s == NULL || s[0] == '\0') {
        return false;
    }
    for (i = 0; i < cap; i++) {
        if (s[i] == '\0') {
            return true;
        }
    }
    return false;
}

bool pool_api_command_valid(const PoolApiCommand *cmd)
{
    if (cmd == NULL) {
        return false;
    }
    if ((unsigned)cmd->kind == 0u || (unsigned)cmd->kind >= (unsigned)POOL_API_CMD__COUNT) {
        return false;
    }
    if ((unsigned)cmd->actor == 0u || (unsigned)cmd->actor >= (unsigned)POOL_API_ACTOR__COUNT) {
        return false;
    }
    if (cmd->kind != POOL_API_CMD_CREATE_SESSION) {
        return true;
    }
    if (cmd->create.duration_s < POOL_SESSION_MIN_DURATION_S ||
        cmd->create.duration_s > POOL_SESSION_MAX_DURATION_S) {
        return false;
    }
    if (!str_bounded_nonempty(cmd->create.target_host, POOL_SESSION_HOST_MAX)) {
        return false;
    }
    if (cmd->create.target_port == 0u) {
        return false;
    }
    if (!str_bounded_nonempty(cmd->create.target_user, POOL_SESSION_USER_MAX)) {
        return false;
    }
    if ((unsigned)cmd->create.target_protocol >= (unsigned)POOL_PROTO__COUNT) {
        return false;
    }
    if (cmd->create.target_tls_mode != POOL_API_TLS_DISABLED &&
        cmd->create.target_tls_mode != POOL_API_TLS_BUNDLED) {
        return false;
    }
    if ((unsigned)cmd->create.target_chain >= (unsigned)POOL_CHAIN__COUNT) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Bounded static mailbox                                              */
/* ------------------------------------------------------------------ */

void pool_api_queue_init(PoolApiCommandQueue *q)
{
    if (q == NULL) {
        return;
    }
    memset(q, 0, sizeof(*q));
    q->initialized   = true;
    q->next_sequence = 1u; /* sequence 0 is never issued */
}

PoolApiSubmitStatus pool_api_queue_submit(PoolApiCommandQueue *q,
                                          const PoolApiCommand *cmd,
                                          uint32_t *out_sequence)
{
    PoolApiSubmitStatus rc;
    uint32_t            seq = 0u;

    if (out_sequence != NULL) {
        *out_sequence = 0u;
    }
    if (q == NULL || !pool_api_command_valid(cmd)) {
        if (q != NULL) {
            portENTER_CRITICAL(&s_api_lock);
            if (q->rejected_invalid < UINT32_MAX) {
                q->rejected_invalid++;
            }
            portEXIT_CRITICAL(&s_api_lock);
        }
        return API_SUBMIT_INVALID;
    }

    portENTER_CRITICAL(&s_api_lock);
    if (!q->initialized) {
        rc = API_SUBMIT_NOT_READY;
    } else if (q->count >= (uint8_t)POOL_API_COMMAND_QUEUE_DEPTH) {
        /* Bounded depth exhausted: nothing is mutated, nothing is dropped. */
        if (q->rejected_full < UINT32_MAX) {
            q->rejected_full++;
        }
        rc = API_SUBMIT_QUEUE_FULL;
    } else {
        seq = q->next_sequence;
        q->next_sequence = (q->next_sequence == UINT32_MAX) ? 1u : (q->next_sequence + 1u);
        q->slots[q->tail] = *cmd;
        q->slots[q->tail].submission_sequence = seq;
        q->tail = (uint8_t)((q->tail + 1u) % POOL_API_COMMAND_QUEUE_DEPTH);
        q->count++;
        if (q->submitted < UINT32_MAX) {
            q->submitted++;
        }
        rc = API_SUBMIT_ACCEPTED;
    }
    portEXIT_CRITICAL(&s_api_lock);

    if (rc == API_SUBMIT_ACCEPTED && out_sequence != NULL) {
        *out_sequence = seq;
    }
    return rc;
}

bool pool_api_queue_take(PoolApiCommandQueue *q, PoolApiCommand *out)
{
    bool got = false;

    if (q == NULL || out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_api_lock);
    if (q->initialized && q->count > 0u) {
        *out = q->slots[q->head];
        /* No command bytes remain in the mailbox after consumption. */
        memset(&q->slots[q->head], 0, sizeof(q->slots[q->head]));
        q->head = (uint8_t)((q->head + 1u) % POOL_API_COMMAND_QUEUE_DEPTH);
        q->count--;
        if (q->consumed < UINT32_MAX) {
            q->consumed++;
        }
        got = true;
    }
    portEXIT_CRITICAL(&s_api_lock);
    if (!got) {
        memset(out, 0, sizeof(*out));
    }
    return got;
}

uint8_t pool_api_queue_depth(const PoolApiCommandQueue *q)
{
    uint8_t d;

    if (q == NULL) {
        return 0u;
    }
    portENTER_CRITICAL(&s_api_lock);
    d = q->count;
    portEXIT_CRITICAL(&s_api_lock);
    return d;
}

bool pool_api_queue_slot_is_zero(const PoolApiCommandQueue *q, uint8_t index)
{
    const uint8_t *raw;
    size_t         i;

    if (q == NULL || index >= (uint8_t)POOL_API_COMMAND_QUEUE_DEPTH) {
        return false;
    }
    raw = (const uint8_t *)&q->slots[index];
    for (i = 0; i < sizeof(q->slots[index]); i++) {
        if (raw[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Pure helpers                                                        */
/* ------------------------------------------------------------------ */

uint32_t pool_api_derive_session_id(uint32_t pre_generation, uint32_t create_sequence)
{
    uint32_t id;

    /* Deterministic and clock-free: mixes only the committed generation the
     * create was planned FROM and a bounded per-boot create sequence. The
     * value never leaves the firmware (it is not published anywhere). */
    id = (pre_generation * 2654435761u) ^ (create_sequence + 0x9E3779B9u);
    return (id == 0u) ? 1u : id;
}

/* Build the bounded B1 source identity from an independently read-back
 * effective configuration. The chain is CUSTOM_UNKNOWN by construction: the
 * device stores no chain fact, and B1 forbids inferring one from a
 * hostname. No password is read, copied or represented. */
static void identity_from_effective(const PoolExecEffectiveConfig *cfg,
                                    PoolConfigIdentity *out)
{
    memset(out, 0, sizeof(*out));
    out->primary          = cfg->primary;
    out->fallback_enabled = (cfg->fallback.host[0] != '\0');
    if (out->fallback_enabled) {
        out->fallback = cfg->fallback;
    }
    out->chain = POOL_CHAIN_CUSTOM_UNKNOWN;
    /* profile_id stays "" — the firmware holds no backend profile registry. */
}

static void identity_from_command(const PoolApiCreateRequest *req,
                                  PoolConfigIdentity *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->primary.host, req->target_host, sizeof(out->primary.host));
    out->primary.host[sizeof(out->primary.host) - 1u] = '\0';
    out->primary.port = req->target_port;
    memcpy(out->primary.user, req->target_user, sizeof(out->primary.user));
    out->primary.user[sizeof(out->primary.user) - 1u] = '\0';
    out->primary.protocol = req->target_protocol;
    out->primary.tls      = (req->target_tls_mode == POOL_API_TLS_BUNDLED);
    /* B8 accepts no target fallback: the committed B7 transaction pins
     * use_fallback=false and verifies the PRIMARY endpoint only. */
    out->fallback_enabled = false;
    out->chain            = req->target_chain;
}

/* ------------------------------------------------------------------ */
/* Bounded coordinator helpers (no blocking work inside the API lock)  */
/* ------------------------------------------------------------------ */

static void api_refresh_lease(PoolSessionApiProcessor *p)
{
    (void)pool_operation_coordinator_snapshot(&p->rt->coord, &p->rt->lease);
}

static uint8_t api_conflict_code_for(PoolSessionApiProcessor *p,
                                     PoolOperationRequestKind kind)
{
    PoolOperationRequest      req;
    PoolOperationDecision     dec;
    PoolOperationHttpConflict cf;

    memset(&req, 0, sizeof(req));
    req.kind = kind;
    dec      = pool_operation_coordinator_evaluate(&p->rt->coord, &req);
    api_refresh_lease(p);
    pool_operation_http_map(&dec, &p->rt->lease, &cf);
    return (uint8_t)cf.code;
}

static bool api_durable_session(const PoolSessionApiProcessor *p)
{
    return p->rt->record_present && p->rt->store_result == STORE_OK &&
           p->rt->record.kind == (uint8_t)POOL_RECORD_KIND_SESSION &&
           p->rt->record.session_id != 0u;
}

/* ------------------------------------------------------------------ */
/* Audited no-mutation releases                                        */
/* ------------------------------------------------------------------ */

/*
 * Independently prove that a failed operation mutated NOTHING, then release
 * the lease through the audited B5 abort path. `expected` is the committed
 * store result the operation started from.
 *
 * The proof is a fresh, independent reload: the committed state must still
 * be EXACTLY what it was. `pool_config_untouched` and `protocol_untouched`
 * are structurally true for every path that can reach here — this component
 * contains no pool write and no protocol call at all (grep-checkable, and
 * asserted by test) — and the obligation is read back from the reloaded
 * truth rather than assumed.
 *
 * Returns the sanitized command result: ACCEPTED means the lease was
 * released and ownership is FREE again; RECOVERY_GUARD means the outcome
 * could not be proven and the guard was entered instead.
 */
static PoolApiCommandResult api_release_no_mutation(PoolSessionApiProcessor *p,
                                                    PoolStoreResult expected,
                                                    bool acknowledge_lease)
{
    PoolSessionRuntime               *rt = p->rt;
    PoolOperationNoMutationEvidence   ev;
    PoolOperationStatus               os;
    PoolStoreResult                   sr;

    memset(&ev, 0, sizeof(ev));
    memset(&p->reloaded, 0, sizeof(p->reloaded));
    sr = pool_session_store_load(&rt->store, &p->reloaded, &rt->load_info);
    ev.store_result = sr;

    /* The committed state must be EXACTLY the pre-operation state. */
    if (sr == expected) {
        if (sr == STORE_OK) {
            /* The acknowledgement case: the ORIGINAL terminal record must
             * still be there, unchanged and still safely acknowledgeable. */
            ev.store_unchanged =
                p->reloaded.kind == (uint8_t)POOL_RECORD_KIND_SESSION &&
                p->reloaded.session_id == rt->record.session_id &&
                p->reloaded.state == rt->record.state &&
                p->reloaded.generation == rt->record.generation &&
                !p->reloaded.restore_required;
        } else {
            /* The create case: EMPTY or CLEARED, so there is no record
             * content that could have changed. */
            ev.store_unchanged = true;
        }
    }
    ev.pool_config_untouched      = true;
    ev.protocol_untouched         = true;
    ev.restore_required_never_set = !p->reloaded.restore_required;

    os = acknowledge_lease
             ? pool_operation_coordinator_abort_acknowledge(&rt->coord, &rt->token, &ev)
             : pool_operation_coordinator_abort_reservation(&rt->coord, &rt->token, &ev);
    if (os == OP_OK) {
        /* The lease is gone: invalidate our copy of the token so nothing
         * can present a stale one, and adopt the proven committed truth. */
        memset(&rt->token, 0, sizeof(rt->token));
        api_refresh_lease(p);
        if (sr == STORE_OK) {
            rt->record         = p->reloaded;
            rt->record_present = true;
        }
        rt->store_result   = sr;
        if (sr != STORE_OK) {
            rt->record_present = false;
        }
        p->pending_session_id = 0u;
        ESP_LOGW(TAG, "lease released after a proven no-mutation failure");
        return API_CMD_RESULT_ACCEPTED;
    }
    api_refresh_lease(p);
    /*
     * The abort refused (uncertain, or the proof was incomplete). B5 has
     * already entered the recovery guard for the uncertain case; for an
     * unprovable one the lease is deliberately retained rather than
     * released on a guess.
     */
    ESP_LOGW(TAG, "no-mutation release refused (%s)", pool_operation_status_str(os));
    return (os == OP_ERR_PERSISTENCE_UNCERTAIN) ? API_CMD_RESULT_RECOVERY_GUARD
                                                : API_CMD_RESULT_FAILED_PERSIST;
}

/* ------------------------------------------------------------------ */
/* Create transaction                                                  */
/* ------------------------------------------------------------------ */

/*
 * Capture the source internally, validate BOTH identities and stage the
 * durable record. Nothing here mutates anything: no NVS write, no lease,
 * no protocol call. Returns API_CMD_RESULT_ACCEPTED when p->staged holds a
 * validated record ready to commit.
 */
static PoolApiCommandResult api_build_create_transaction(PoolSessionApiProcessor *p,
                                                         const PoolApiCommand *cmd,
                                                         uint32_t session_id)
{
    char               board[POOL_SESSION_BOARD_MAX];
    char               asic[POOL_SESSION_ASIC_MAX];
    PoolSessionEvent   ev;
    PoolSessionOutcome oc;
    PoolSessionError   verr;

    memset(board, 0, sizeof(board));
    memset(asic, 0, sizeof(asic));
    memset(&p->req, 0, sizeof(p->req));
    memset(&p->session, 0, sizeof(p->session));
    memset(&p->staged, 0, sizeof(p->staged));
    memset(&p->effective, 0, sizeof(p->effective));

    /* --- Hardware eligibility (fail closed) --- */
    if (!p->src_ops->device_identity(p->src_ctx, board, sizeof(board), asic, sizeof(asic))) {
        return API_CMD_RESULT_REJECTED_HARDWARE;
    }
    if (strncmp(board, POOL_SESSION_SUPPORTED_BOARD, sizeof(board)) != 0 ||
        strncmp(asic, POOL_SESSION_SUPPORTED_ASIC, sizeof(asic)) != 0) {
        return API_CMD_RESULT_REJECTED_HARDWARE;
    }

    /* --- Source capture: INTERNAL ONLY. The client never supplies it. --- */
    p->src_ops->read_effective(p->src_ctx, &p->effective);
    if (!p->effective.valid) {
        return API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED;
    }
    /* Exact restorability of the source is a precondition, not a hope. */
    if (pool_exec_tls_representable(&p->effective) != EXEC_TLS_OK) {
        return API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED;
    }
    if (p->effective.use_fallback) {
        /* The committed B7 configuration transaction pins use_fallback
         * false and verifies the PRIMARY endpoint, so a device currently
         * mining on its FALLBACK endpoint could not be restored to exactly
         * the configuration it had. Refuse before anything is reserved. */
        return API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED;
    }
    /* Keep-current-password only: the stored password must remain readable
     * and untouched. This probe returns a BOOLEAN — never any bytes. */
    if (!p->src_ops->source_password_retained(p->src_ctx)) {
        return API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED;
    }

    /* --- Bounded request assembly (NO password field exists) --- */
    p->req.model_version   = POOL_SESSION_MODEL_VERSION;
    p->req.session_id      = session_id;
    p->req.duration_s      = cmd->create.duration_s;
    p->req.password_policy = POOL_SESSION_PW_KEEP_CURRENT;
    identity_from_effective(&p->effective, &p->req.source);
    identity_from_command(&cmd->create, &p->req.target);
    memcpy(p->req.board_version, POOL_SESSION_SUPPORTED_BOARD,
           sizeof(POOL_SESSION_SUPPORTED_BOARD));
    memcpy(p->req.asic_model, POOL_SESSION_SUPPORTED_ASIC,
           sizeof(POOL_SESSION_SUPPORTED_ASIC));

    /* A target that IS the current pool is a no-op session, not a request. */
    if (pool_endpoint_equal(&p->req.source.primary, &p->req.target.primary)) {
        return API_CMD_RESULT_REJECTED_TARGET_UNSUPPORTED;
    }

    verr = pool_session_validate_request(&p->req);
    if (verr != ERR_NONE) {
        if (verr == ERR_SOURCE_IDENTITY_INVALID) {
            return API_CMD_RESULT_REJECTED_SOURCE_UNSUPPORTED;
        }
        if (verr == ERR_UNSUPPORTED_BOARD) {
            return API_CMD_RESULT_REJECTED_HARDWARE;
        }
        if (verr == ERR_TARGET_IDENTITY_INVALID || verr == ERR_TARGET_EQUALS_SOURCE) {
            return API_CMD_RESULT_REJECTED_TARGET_UNSUPPORTED;
        }
        return API_CMD_RESULT_REJECTED_VALIDATION;
    }

    /* --- Drive the committed B1 FSM to the durable snapshot state --- */
    pool_session_init(&p->session);
    memset(&ev, 0, sizeof(ev));
    ev.type       = POOL_EVT_CREATE_REQUESTED;
    ev.session_id = p->req.session_id;
    ev.request    = &p->req;
    oc            = pool_session_transition(&p->session, &ev, &p->session);
    if (oc.error != ERR_NONE || p->session.state != POOL_STATE_PREPARING) {
        return API_CMD_RESULT_REJECTED_VALIDATION;
    }

    memset(&ev, 0, sizeof(ev));
    ev.type       = POOL_EVT_SOURCE_SNAPSHOT_COMMITTED;
    ev.session_id = p->req.session_id;
    oc            = pool_session_transition(&p->session, &ev, &p->session);
    if (oc.error != ERR_NONE || p->session.state != POOL_STATE_TARGET_SNAPSHOT_COMMITTED) {
        return API_CMD_RESULT_REJECTED_VALIDATION;
    }
    /* The snapshot state is pre-mutation by the committed B1 contract. */
    if (p->session.restore_required) {
        return API_CMD_RESULT_REJECTED_VALIDATION;
    }

    if (pool_session_record_from_session(&p->session, &p->staged) != RECORD_OK) {
        return API_CMD_RESULT_REJECTED_VALIDATION;
    }
    if (p->staged.password_policy != POOL_SESSION_PW_KEEP_CURRENT ||
        p->staged.restore_required || p->staged.duration_s != cmd->create.duration_s) {
        return API_CMD_RESULT_REJECTED_VALIDATION;
    }
    return API_CMD_RESULT_ACCEPTED;
}

/* Independent, EXACT verification of the committed create record. */
static bool api_verify_create_readback(const PoolSessionRecord *staged,
                                       const PoolSessionRecord *reloaded,
                                       uint32_t pre_generation,
                                       PoolStoreResult reload_result)
{
    if (reload_result != STORE_OK) {
        return false;
    }
    if (reloaded->kind != (uint8_t)POOL_RECORD_KIND_SESSION) {
        return false;
    }
    if (reloaded->session_id != staged->session_id) {
        return false;
    }
    if (reloaded->b1_model_version != POOL_SESSION_MODEL_VERSION) {
        return false;
    }
    if (reloaded->state != POOL_STATE_TARGET_SNAPSHOT_COMMITTED) {
        return false;
    }
    if (!pool_config_identity_equal(&reloaded->source, &staged->source)) {
        return false;
    }
    if (!pool_config_identity_equal(&reloaded->target, &staged->target)) {
        return false;
    }
    if (reloaded->duration_s != staged->duration_s) {
        return false;
    }
    if (reloaded->password_policy != POOL_SESSION_PW_KEEP_CURRENT) {
        return false;
    }
    if (reloaded->restore_required) {
        return false; /* a pre-mutation snapshot owes nothing yet */
    }
    if (reloaded->generation == 0u || reloaded->generation <= pre_generation) {
        return false;
    }
    return true;
}

static PoolApiCommandResult api_process_create(PoolSessionApiProcessor *p,
                                               const PoolApiCommand *cmd)
{
    PoolSessionRuntime           *rt = p->rt;
    PoolOperationRequest          req;
    PoolOperationDecision         dec;
    PoolOperationStatus           os;
    PoolOperationPersistenceProof proof;
    PoolApiCommandResult          rc;
    PoolStoreResult               sr;
    PoolStoreResult               pre_store;
    uint32_t                      pre_generation;
    bool                          reuse_reservation;

    if (!rt->initialized || !rt->booted) {
        return API_CMD_RESULT_REJECTED_STATE;
    }
    api_refresh_lease(p);

    /* A durable session, or one the executor already owns, blocks a new
     * one: exactly one session can exist. */
    if (api_durable_session(p) || pool_session_executor_owns_flow(p->ex)) {
        p->last_conflict_code = api_conflict_code_for(p, OP_REQUEST_TIMED_SESSION_START);
        return API_CMD_RESULT_REJECTED_CONFLICT;
    }
    /*
     * A session that could never be executed must never be created. With no
     * executor bound the create is refused BEFORE anything is reserved (the
     * HTTP layer refuses it even earlier, at submission).
     */
    if (p->ex == NULL) {
        return API_CMD_RESULT_REJECTED_STATE;
    }

    /*
     * A reservation retained by an earlier refused abort is REUSED — a
     * second competing lease is never acquired.
     */
    reuse_reservation = rt->token.valid && p->pending_session_id != 0u &&
                        rt->lease.owner == OP_OWNER_TIMED_SESSION &&
                        rt->lease.phase == OP_PHASE_RESERVED_PENDING_PERSISTENCE;

    pre_generation = rt->committed_generation;
    pre_store      = rt->store_result;

    if (!reuse_reservation) {
        /*
         * The session id is derived ONCE per reservation from the committed
         * generation the create was planned FROM plus a bounded per-boot
         * sequence, and is then FIXED: the pre-flight build, the B5
         * reservation, the under-lease build and the persistence proof all
         * use the same value, so they can never disagree.
         */
        p->create_sequence = (p->create_sequence == UINT32_MAX)
                                 ? 1u : (p->create_sequence + 1u);
        p->pending_session_id = pool_api_derive_session_id(pre_generation,
                                                           p->create_sequence);

        /*
         * PRE-FLIGHT: every fallible validation runs with NO lease held, so
         * a rejected create can never strand one.
         */
        rc = api_build_create_transaction(p, cmd, p->pending_session_id);
        if (rc != API_CMD_RESULT_ACCEPTED) {
            p->pending_session_id = 0u;
            return rc;
        }

        memset(&req, 0, sizeof(req));
        req.kind       = OP_REQUEST_TIMED_SESSION_START;
        req.session_id = p->pending_session_id;
        memset(&dec, 0, sizeof(dec));
        os = pool_operation_coordinator_try_acquire(&rt->coord, &req, &rt->token, &dec);
        api_refresh_lease(p);
        if (os != OP_OK) {
            PoolOperationHttpConflict cf;
            pool_operation_http_map(&dec, &rt->lease, &cf);
            p->last_conflict_code = (uint8_t)cf.code;
            p->pending_session_id = 0u;
            return API_CMD_RESULT_REJECTED_CONFLICT;
        }
        /* The lease must be exactly what B5 promises for a new session. */
        if (!rt->token.valid || rt->token.owner != OP_OWNER_TIMED_SESSION ||
            rt->token.lease_generation == 0u ||
            rt->lease.owner != OP_OWNER_TIMED_SESSION ||
            rt->lease.phase != OP_PHASE_RESERVED_PENDING_PERSISTENCE ||
            !rt->lease.persistence_required_before_action) {
            return API_CMD_RESULT_FAILED_OWNERSHIP;
        }
    }

    /*
     * UNDER THE LEASE: re-capture and re-validate. Once the lease is held
     * the committed B7 mutation fence denies every foreign pool PATCH, so
     * THIS capture is the authoritative, race-free source snapshot that
     * becomes immutable. (Re-running it also rebuilds the staged record
     * when a reservation is being reused.)
     */
    rc = api_build_create_transaction(p, cmd, p->pending_session_id);
    if (rc != API_CMD_RESULT_ACCEPTED) {
        /*
         * A DEFINITE no-mutation failure: nothing was staged, written or
         * started. Release the reservation through the audited B5
         * no-mutation abort so ownership returns to FREE without any client
         * retry, then report the original reason.
         */
        (void)api_release_no_mutation(p, pre_store, /*acknowledge_lease=*/false);
        return rc;
    }

    /* ---- Durable commit ---- */
    sr = pool_session_store_commit_record(&rt->store, &p->staged);
    if (sr == STORE_COMMIT_UNCERTAIN) {
        memset(&proof, 0, sizeof(proof));
        proof.kind         = OP_PROOF_SESSION_COMMITTED;
        proof.store_result = STORE_COMMIT_UNCERTAIN;
        proof.session_id   = p->staged.session_id;
        (void)pool_operation_coordinator_apply_persistence_proof(&rt->coord, &rt->token,
                                                                 &proof, &rt->token);
        api_refresh_lease(p);
        return API_CMD_RESULT_RECOVERY_GUARD;
    }
    if (sr != STORE_OK) {
        /*
         * A DEFINITE commit failure. The committed B3 contract keeps the OLD
         * pointer authoritative until the new one verifies, so an
         * independent reload can prove the store is exactly as it was — and
         * only then is the reservation released. If that proof fails the
         * abort refuses and the guard/retained posture stands.
         */
        (void)api_release_no_mutation(p, pre_store, /*acknowledge_lease=*/false);
        return API_CMD_RESULT_FAILED_PERSIST;
    }

    /* ---- Independent reload + EXACT verification ---- */
    memset(&p->reloaded, 0, sizeof(p->reloaded));
    sr = pool_session_store_load(&rt->store, &p->reloaded, &rt->load_info);
    if (!api_verify_create_readback(&p->staged, &p->reloaded, pre_generation, sr)) {
        /* Flash changed but does not match: the durable truth is ambiguous. */
        memset(&proof, 0, sizeof(proof));
        proof.kind         = OP_PROOF_SESSION_COMMITTED;
        proof.store_result = STORE_COMMIT_UNCERTAIN;
        proof.session_id   = p->staged.session_id;
        (void)pool_operation_coordinator_apply_persistence_proof(&rt->coord, &rt->token,
                                                                 &proof, &rt->token);
        api_refresh_lease(p);
        return API_CMD_RESULT_FAILED_READBACK;
    }

    /* ---- B5 SESSION_COMMITTED proof (activates the reservation) ---- */
    memset(&proof, 0, sizeof(proof));
    proof.kind                        = OP_PROOF_SESSION_COMMITTED;
    proof.store_result                = STORE_OK;
    proof.committed_record_generation = p->reloaded.generation;
    proof.session_id                  = p->reloaded.session_id;
    proof.persisted_state             = (PoolSessionState)p->reloaded.state;
    proof.restore_required            = p->reloaded.restore_required;
    os = pool_operation_coordinator_apply_persistence_proof(&rt->coord, &rt->token,
                                                            &proof, &rt->token);
    api_refresh_lease(p);
    if (os != OP_OK) {
        return API_CMD_RESULT_FAILED_OWNERSHIP;
    }

    /* ---- Adopt the reloaded record as THE committed truth ---- */
    rt->record         = p->reloaded;
    rt->record_present = true;
    rt->store_result   = STORE_OK;
    (void)pool_session_store_committed_generation(&rt->store, &rt->committed_generation);

    p->pending_session_id = 0u; /* the reservation is resolved */
    ESP_LOGI(TAG, "timed session created (durable snapshot committed)");

    /*
     * ---- Same-boot handoff to the committed Gate B7 executor ----
     *
     * A durable TARGET_SNAPSHOT_COMMITTED record must NEVER be left behind
     * by a completed command: either the executor owns it from here on, or
     * it is safely cancelled below. Nothing in Gate B8 touches a pool key,
     * Stratum or the ASIC gate — the executor does, and only after IT has
     * durably committed the restore obligation.
     */
    return api_adopt_executor(p);
}

/* ------------------------------------------------------------------ */
/* Executor handoff and the pre-mutation cancellation fallback         */
/* ------------------------------------------------------------------ */

/*
 * Adoption failed BEFORE any target mutation. The committed B1 pre-mutation
 * CANCELLED path is the correct answer: the source was never touched, so
 * NO restoration is owed, and the safe terminal result is retained for
 * acknowledgement. A TARGET_SNAPSHOT_COMMITTED record is never left behind.
 */
static PoolApiCommandResult api_cancel_pre_mutation(PoolSessionApiProcessor *p,
                                                    PoolExecReason why)
{
    PoolApiCommandResult rc;

    ESP_LOGW(TAG, "executor adoption refused (%s) — cancelling before mutation",
             pool_exec_reason_str(why));
    if (p->rt->record.restore_required) {
        /* Should be unreachable: adoption never proceeds past the obligation
         * boundary on a failure path. Guard rather than guess. */
        return API_CMD_RESULT_RECOVERY_GUARD;
    }
    rc = api_commit_transition(p, POOL_EVT_CANCEL_REQUESTED);
    if (rc != API_CMD_RESULT_ACCEPTED) {
        return rc; /* persist/readback/ownership failure reported as-is */
    }
    return API_CMD_RESULT_ADOPTION_FAILED;
}

static PoolApiCommandResult api_adopt_executor(PoolSessionApiProcessor *p)
{
    PoolExecReason r;

    if (p->ex == NULL) {
        return api_cancel_pre_mutation(p, EXEC_REASON_NOT_BOUND);
    }
    /* Verified inside the executor under the CURRENT token: the lease owner
     * and phase, the durable state, the obligation, the hardware and the
     * TLS representability — then the B1 APPLY_TARGET boundary is committed,
     * independently reloaded, verified and proven BEFORE the configuration
     * transaction is armed. */
    r = pool_session_executor_adopt_created_session(p->ex);
    if (r != EXEC_REASON_NONE) {
        return api_cancel_pre_mutation(p, r);
    }
    /* Ownership has transferred: from here the executor is authoritative. */
    ESP_LOGI(TAG, "timed session adopted by the execution layer");
    return API_CMD_RESULT_ACCEPTED;
}

/* ------------------------------------------------------------------ */
/* Restore Now                                                         */
/* ------------------------------------------------------------------ */

/*
 * Commit ONE B1 transition of the durable session and prove it to B5 under
 * the CURRENT token. Mirrors the committed Gate B7 boundary sequence:
 * stage -> commit -> independent reload -> exact comparison -> proof.
 */
static PoolApiCommandResult api_commit_transition(PoolSessionApiProcessor *p,
                                                  PoolSessionEventType type)
{
    PoolSessionRuntime           *rt = p->rt;
    PoolSessionEvent              ev;
    PoolSessionOutcome            oc;
    PoolStoreResult               sr;
    PoolOperationPersistenceProof proof;
    PoolOperationStatus           os;
    uint32_t                      pre_generation = rt->committed_generation;
    bool                          pre_obligation = rt->record.restore_required;

    if (pool_session_record_to_session(&rt->record, &p->session) != RECORD_OK) {
        return API_CMD_RESULT_REJECTED_STATE;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type       = type;
    ev.session_id = p->session.session_id;
    oc            = pool_session_transition(&p->session, &ev, &p->session);
    if (oc.error != ERR_NONE && !oc.changed) {
        return API_CMD_RESULT_REJECTED_STATE;
    }
    if (!oc.changed) {
        return API_CMD_RESULT_ACCEPTED; /* idempotent no-op */
    }
    if (!pool_state_is_persistent(p->session.state)) {
        return API_CMD_RESULT_REJECTED_STATE;
    }
    if (pool_session_record_from_session(&p->session, &p->staged) != RECORD_OK) {
        return API_CMD_RESULT_REJECTED_STATE;
    }
    /* The restore obligation is monotone: it may only clear at COMPLETE. */
    if (pre_obligation && !p->staged.restore_required &&
        p->staged.state != POOL_STATE_COMPLETE) {
        return API_CMD_RESULT_REJECTED_STATE;
    }
    /* Preserve the durable time facts the API never owns. */
    p->staged.verified_start_valid   = rt->record.verified_start_valid;
    p->staged.verified_start_epoch_s = rt->record.verified_start_epoch_s;
    p->staged.deadline_valid         = rt->record.deadline_valid;
    p->staged.deadline_epoch_s       = rt->record.deadline_epoch_s;
    p->staged.deadline_sync_generation = rt->record.deadline_sync_generation;
    p->staged.latest_trusted_valid   = rt->record.latest_trusted_valid;
    p->staged.latest_trusted_epoch_s = rt->record.latest_trusted_epoch_s;
    p->staged.reboot_count           = rt->record.reboot_count;
    p->staged.recovery_attempt_count = rt->record.recovery_attempt_count;
    p->staged.consecutive_recovery_failures = rt->record.consecutive_recovery_failures;
    p->staged.last_reset_class       = rt->record.last_reset_class;

    sr = pool_session_store_commit_record(&rt->store, &p->staged);
    if (sr == STORE_COMMIT_UNCERTAIN) {
        memset(&proof, 0, sizeof(proof));
        proof.kind         = OP_PROOF_RECOVERY_UPDATE_COMMITTED;
        proof.store_result = STORE_COMMIT_UNCERTAIN;
        proof.session_id   = p->staged.session_id;
        (void)pool_operation_coordinator_apply_persistence_proof(&rt->coord, &rt->token,
                                                                 &proof, &rt->token);
        api_refresh_lease(p);
        return API_CMD_RESULT_RECOVERY_GUARD;
    }
    if (sr != STORE_OK) {
        return API_CMD_RESULT_FAILED_PERSIST;
    }

    memset(&p->reloaded, 0, sizeof(p->reloaded));
    sr = pool_session_store_load(&rt->store, &p->reloaded, &rt->load_info);
    if (pool_exec_verify_transition_readback(&p->staged, &p->reloaded, pre_generation,
                                             pre_obligation, sr) != EXEC_REASON_NONE) {
        memset(&proof, 0, sizeof(proof));
        proof.kind         = OP_PROOF_RECOVERY_UPDATE_COMMITTED;
        proof.store_result = STORE_COMMIT_UNCERTAIN;
        proof.session_id   = p->staged.session_id;
        (void)pool_operation_coordinator_apply_persistence_proof(&rt->coord, &rt->token,
                                                                 &proof, &rt->token);
        api_refresh_lease(p);
        return API_CMD_RESULT_FAILED_READBACK;
    }

    rt->record         = p->reloaded;
    rt->record_present = true;
    rt->store_result   = STORE_OK;
    (void)pool_session_store_committed_generation(&rt->store, &rt->committed_generation);

    memset(&proof, 0, sizeof(proof));
    proof.kind = (p->staged.state == POOL_STATE_COMPLETE ||
                  p->staged.state == POOL_STATE_CANCELLED)
                     ? OP_PROOF_TERMINAL_COMMITTED
                     : OP_PROOF_RECOVERY_UPDATE_COMMITTED;
    proof.store_result                = STORE_OK;
    proof.committed_record_generation = rt->record.generation;
    proof.session_id                  = rt->record.session_id;
    proof.persisted_state             = (PoolSessionState)p->staged.state;
    proof.restore_required            = p->staged.restore_required;
    os = pool_operation_coordinator_apply_persistence_proof(&rt->coord, &rt->token,
                                                            &proof, &rt->token);
    api_refresh_lease(p);
    if (os != OP_OK) {
        return API_CMD_RESULT_FAILED_OWNERSHIP;
    }

    /* A durable terminal releases session ownership; the RESULT stays
     * retained until it is acknowledged. */
    if (rt->lease.phase == OP_PHASE_TERMINAL_ACK_PENDING) {
        (void)pool_operation_coordinator_release_session(&rt->coord, &rt->token);
        memset(&rt->token, 0, sizeof(rt->token));
        api_refresh_lease(p);
    }
    return API_CMD_RESULT_ACCEPTED;
}

static PoolApiCommandResult api_process_restore(PoolSessionApiProcessor *p)
{
    PoolSessionRuntime  *rt = p->rt;
    PoolOperationStatus  os;
    PoolApiCommandResult rc;
    PoolSessionState     st;

    if (!rt->initialized || !rt->booted) {
        return API_CMD_RESULT_REJECTED_STATE;
    }
    api_refresh_lease(p);

    if (!api_durable_session(p)) {
        p->last_conflict_code = (uint8_t)OP_HTTP_OPERATION_NO_ACTIVE_SESSION;
        return API_CMD_RESULT_REJECTED_STATE;
    }
    st = (PoolSessionState)rt->record.state;
    if (pool_state_is_terminal(st)) {
        p->last_conflict_code = (uint8_t)OP_HTTP_OPERATION_NO_ACTIVE_SESSION;
        return API_CMD_RESULT_REJECTED_STATE;
    }
    /* A session-class owner must already exist: Restore Now is a CONTROL
     * request to the existing owner, never a new acquisition. */
    if (rt->lease.owner != OP_OWNER_TIMED_SESSION &&
        rt->lease.owner != OP_OWNER_BOOT_RECOVERY &&
        rt->lease.owner != OP_OWNER_SOURCE_RESTORE) {
        p->last_conflict_code = api_conflict_code_for(p, OP_REQUEST_RESTORE_NOW);
        return API_CMD_RESULT_REJECTED_CONFLICT;
    }
    /* Already restoring: idempotently accepted, nothing changes. */
    if (rt->lease.phase == OP_PHASE_RESTORING_SOURCE && pool_state_is_restore_side(st)) {
        return API_CMD_RESULT_ACCEPTED;
    }
    /*
     * The committed B1 FSM is the ONLY authority on which active states
     * accept Restore Now: a post-mutation state drives a real restoration
     * (pool_state_allows_restore_now), and a PRE-mutation snapshot state
     * cancels safely because the source was never touched. A state that
     * accepts neither is rejected by the transition itself, so no second
     * policy table is duplicated here.
     */
    if (!pool_state_is_active_session(st)) {
        p->last_conflict_code = (uint8_t)OP_HTTP_OPERATION_NO_ACTIVE_SESSION;
        return API_CMD_RESULT_REJECTED_STATE;
    }

    /* B5 Restore Now: a control transition of the EXISTING lease. It
     * rotates the generation in place; no second lease is created. */
    os = pool_operation_coordinator_restore_now(&rt->coord, rt->record.session_id,
                                                &rt->token);
    api_refresh_lease(p);
    if (os != OP_OK) {
        p->last_conflict_code = (os == OP_ERR_STALE_LEASE)
                                    ? (uint8_t)OP_HTTP_OPERATION_STALE_LEASE
                                    : (uint8_t)OP_HTTP_OPERATION_NOT_OWNER;
        return API_CMD_RESULT_REJECTED_CONFLICT;
    }

    /* Persist the restoration INTENT before any Gate B7 source mutation. */
    rc = api_commit_transition(p, POOL_EVT_RESTORE_NOW_REQUESTED);
    if (rc != API_CMD_RESULT_ACCEPTED) {
        return rc;
    }

    /*
     * Notify the existing B6/B7 owner flow through the committed bounded
     * event path — strictly AFTER the durable proof. Gate B8 itself never
     * touches the pool, Stratum or the ASIC gate.
     */
    p->api_owns_flow = false;
    (void)pool_session_runtime_notify(rt, RUNTIME_EVENT_STORE_RELOAD_REQUIRED);
    return API_CMD_RESULT_ACCEPTED;
}

/* ------------------------------------------------------------------ */
/* Terminal acknowledgement                                            */
/* ------------------------------------------------------------------ */

static PoolApiCommandResult api_process_acknowledge(PoolSessionApiProcessor *p)
{
    PoolSessionRuntime           *rt = p->rt;
    PoolOperationRequest          req;
    PoolOperationDecision         dec;
    PoolOperationStatus           os;
    PoolOperationPersistenceProof proof;
    PoolStoreResult               sr;
    PoolSessionState              st;
    bool                          reuse_ack_lease;

    if (!rt->initialized || !rt->booted) {
        return API_CMD_RESULT_REJECTED_STATE;
    }
    api_refresh_lease(p);

    reuse_ack_lease = rt->token.valid && rt->lease.owner == OP_OWNER_SESSION_ACKNOWLEDGE;

    if (!reuse_ack_lease) {
        /* No execution owner may exist, and a terminal result must be
         * retained. Both are read-only checks with no lease held. */
        if (rt->lease.owner != OP_OWNER_NONE || rt->lease.phase != OP_PHASE_FREE ||
            !rt->lease.terminal_pending) {
            p->last_conflict_code = api_conflict_code_for(p, OP_REQUEST_SESSION_ACKNOWLEDGE);
            return API_CMD_RESULT_REJECTED_CONFLICT;
        }
        if (!api_durable_session(p)) {
            p->last_conflict_code = (uint8_t)OP_HTTP_OPERATION_NO_ACTIVE_SESSION;
            return API_CMD_RESULT_REJECTED_STATE;
        }
        st = (PoolSessionState)rt->record.state;
        /*
         * ONLY a COMPLETE or a safe pre-mutation CANCELLED record with no
         * restore obligation may be acknowledged. TARGET_FAILED,
         * RESTORE_FAILED, RECOVERY_REQUIRED, every active state and every
         * record that still owes a restoration are denied.
         */
        if ((st != POOL_STATE_COMPLETE && st != POOL_STATE_CANCELLED) ||
            rt->record.restore_required) {
            p->last_conflict_code = (uint8_t)OP_HTTP_OPERATION_UNSAFE_RELEASE;
            return API_CMD_RESULT_REJECTED_STATE;
        }

        memset(&req, 0, sizeof(req));
        req.kind       = OP_REQUEST_SESSION_ACKNOWLEDGE;
        req.session_id = rt->record.session_id;
        memset(&dec, 0, sizeof(dec));
        os = pool_operation_coordinator_try_acquire(&rt->coord, &req, &rt->token, &dec);
        api_refresh_lease(p);
        if (os != OP_OK) {
            PoolOperationHttpConflict cf;
            pool_operation_http_map(&dec, &rt->lease, &cf);
            p->last_conflict_code = (uint8_t)cf.code;
            return API_CMD_RESULT_REJECTED_CONFLICT;
        }
        if (!rt->token.valid || rt->token.owner != OP_OWNER_SESSION_ACKNOWLEDGE ||
            rt->lease.owner != OP_OWNER_SESSION_ACKNOWLEDGE) {
            return API_CMD_RESULT_FAILED_OWNERSHIP;
        }
    }

    /* ---- Commit the tombstone through the committed B3 clear path ---- */
    sr = pool_session_store_commit_clear(&rt->store);
    if (sr == STORE_COMMIT_UNCERTAIN) {
        memset(&proof, 0, sizeof(proof));
        proof.kind         = OP_PROOF_CLEARED;
        proof.store_result = STORE_COMMIT_UNCERTAIN;
        (void)pool_operation_coordinator_release_ack(&rt->coord, &rt->token, &proof);
        api_refresh_lease(p);
        /* The terminal result REMAINS pending and a new session stays
         * blocked: an uncertain clear never becomes FREE. */
        return API_CMD_RESULT_RECOVERY_GUARD;
    }
    if (sr != STORE_OK && sr != STORE_CLEARED) {
        /*
         * A DEFINITE tombstone-commit failure. Prove independently that the
         * ORIGINAL safe terminal record is still the committed state, then
         * release the short-lived acknowledgement lease through the audited
         * B5 abort. `terminal_pending` deliberately STAYS TRUE, so the
         * retained result survives and a later acknowledgement may retry —
         * without the original HTTP request having to stay alive.
         */
        (void)api_release_no_mutation(p, STORE_OK, /*acknowledge_lease=*/true);
        return API_CMD_RESULT_FAILED_PERSIST;
    }

    /* ---- Independent reload: STORE_CLEARED is mandatory ---- */
    memset(&p->reloaded, 0, sizeof(p->reloaded));
    sr = pool_session_store_load(&rt->store, &p->reloaded, &rt->load_info);
    if (sr != STORE_CLEARED) {
        /*
         * The clear reported success but the committed state is not a
         * tombstone. When an independent reload proves the ORIGINAL terminal
         * record is still there, this is a definite no-mutation failure and
         * the acknowledgement lease is released with the terminal result
         * retained; anything else is ambiguous and the abort refuses.
         */
        (void)api_release_no_mutation(p, STORE_OK, /*acknowledge_lease=*/true);
        return API_CMD_RESULT_FAILED_READBACK; /* terminal state stays retained */
    }

    memset(&proof, 0, sizeof(proof));
    proof.kind         = OP_PROOF_CLEARED;
    proof.store_result = STORE_CLEARED;
    os = pool_operation_coordinator_release_ack(&rt->coord, &rt->token, &proof);
    memset(&rt->token, 0, sizeof(rt->token));
    api_refresh_lease(p);
    if (os != OP_OK) {
        return API_CMD_RESULT_FAILED_OWNERSHIP;
    }

    /* The committed truth is now a tombstone: no session exists. */
    rt->record_present = false;
    rt->store_result   = STORE_CLEARED;
    memset(&rt->record, 0, sizeof(rt->record));
    p->api_owns_flow = false;

    /* Acknowledgement NEVER starts another session; it only asks the
     * runtime to re-derive its posture from the cleared store. */
    (void)pool_session_runtime_notify(rt, RUNTIME_EVENT_STORE_RELOAD_REQUIRED);
    ESP_LOGI(TAG, "terminal result acknowledged (tombstone committed)");
    return API_CMD_RESULT_ACCEPTED;
}

/* ------------------------------------------------------------------ */
/* Heartbeat                                                           */
/* ------------------------------------------------------------------ */

static uint64_t api_monotonic_us(const PoolSessionApiProcessor *p)
{
    if (p->rt->deps.monotonic_us == NULL) {
        return 0u;
    }
    return p->rt->deps.monotonic_us();
}

/*
 * THE bounded heartbeat fail-safe, in exactly the required order:
 *   1. inhibit ASIC target work,
 *   2. revoke the Gate B7 target-mining grant,
 *   3. restore the source through the EXISTING session owner,
 *   4. persist the restoration intent before any source mutation,
 *   5. retain restore_required.
 *
 * Steps 1 and 2 are the first two statements of the committed
 * exec_begin_restore (gate closed BEFORE the revoke so no job can slip
 * through), and steps 3-5 are the committed bounded restore path. Gate B8
 * orders it; the executor performs it. Idempotent: the latch means repeated
 * task ticks can never issue a second revoke or a second restore.
 */
static void api_engage_failsafe(PoolSessionApiProcessor *p, PoolExecReason cause)
{
    PoolExecReason r;

    if (p->hb.failsafe_engaged) {
        return;
    }
    if (p->ex == NULL) {
        /* No executor: no grant can exist and no ASIC target work is
         * possible, so the fail-safe is already satisfied. */
        pool_api_heartbeat_record_failsafe(&p->hb);
        return;
    }
    r = pool_session_executor_request_restore(p->ex, cause);
    pool_api_heartbeat_record_failsafe(&p->hb);
    ESP_LOGW(TAG, "heartbeat fail-safe engaged (%s -> %s)",
             pool_exec_reason_str(cause), pool_exec_reason_str(r));
}

static void api_step_heartbeat(PoolSessionApiProcessor *p)
{
    PoolSessionRuntime      *rt = p->rt;
    PoolApiHeartbeatInput    in;
    PoolApiHeartbeatDecision d;
    PoolExecutionSnapshot    es;
    PoolRuntimeStatus        st;
    bool                     grant = false;

    memset(&in, 0, sizeof(in));
    memset(&es, 0, sizeof(es));
    if (p->ex != NULL && pool_session_executor_snapshot(p->ex, &es) == EXEC_REASON_NONE) {
        grant = es.mining_grant_active && es.gate == EXEC_GATE_OPEN_TARGET;
    }

    in.durable_target_active = api_durable_session(p) &&
                               rt->record.state == POOL_STATE_TARGET_ACTIVE;
    in.mining_grant_active   = grant;
    in.recovery_guard        = (rt->lease.phase == OP_PHASE_RECOVERY_GUARD) ||
                        (rt->lease.owner == OP_OWNER_RECOVERY_GUARD) ||
                        (rt->lease.owner == OP_OWNER_OPERATOR_RECOVERY);
    in.trusted_time_valid = rt->time_snapshot.trusted && rt->time_snapshot.status == TIME_OK;
    in.trusted_epoch_s    = in.trusted_time_valid ? rt->time_snapshot.trusted_epoch_s : 0u;
    in.monotonic_us       = api_monotonic_us(p);
    in.persisted_epoch_valid   = in.durable_target_active && rt->record.latest_trusted_valid;
    in.persisted_epoch_floor_s = in.durable_target_active ? rt->record.latest_trusted_epoch_s : 0u;

    (void)pool_api_heartbeat_evaluate(&p->hb, &in, &d);

    /*
     * UNCERTAINTY: the B5 recovery guard is already engaged by the
     * persistence engine. The target must not keep mining on an unknowable
     * durable state — inhibit and revoke immediately.
     */
    if (d.guard_required) {
        api_engage_failsafe(p, EXEC_REASON_PERSIST_UNCERTAIN);
        p->hb.status = API_HB_RECOVERY_GUARD;
        return;
    }
    /*
     * BUDGET OR AGE EXHAUSTED: the device can no longer prove durable
     * liveness for a session mining someone else's pool. Order the bounded
     * fail-safe exactly once (the acknowledgement latch makes repeated task
     * ticks idempotent).
     */
    if (d.failsafe_required) {
        api_engage_failsafe(p, EXEC_REASON_PERSIST_FAILED);
        return;
    }

    if (!p->hb.armed) {
        /* The first evaluation of an applicable grant only anchors the
         * monotonic cadence and the durable-liveness age — never a write. */
        if (d.status == API_HB_WAITING) {
            pool_api_heartbeat_arm(&p->hb, in.monotonic_us);
        }
        p->hb.status = d.status;
        return;
    }
    if (!d.commit_now) {
        p->hb.status = d.status;
        return;
    }

    /*
     * Drive the COMMITTED generation-aware Gate B6 persistence engine:
     * build the epoch-only proposal, dedupe it, commit it, reload it
     * independently, verify it exactly and prove it to B5. It touches only
     * the trusted-epoch floor, so it can never extend a deadline.
     */
    st = pool_session_runtime_commit_epoch_heartbeat(rt, d.epoch_s);
    if (st == RUNTIME_OK || st == RUNTIME_ERR_PERSIST_DUPLICATE) {
        pool_api_heartbeat_record_commit(&p->hb, in.monotonic_us, d.epoch_s);
        return;
    }
    if (st == RUNTIME_ERR_STORE_UNCERTAIN) {
        pool_api_heartbeat_record_failure(&p->hb, in.monotonic_us, true);
        api_engage_failsafe(p, EXEC_REASON_PERSIST_UNCERTAIN);
        return;
    }
    /* A DEFINITE failure: liveness was NOT durably proven. Nothing here
     * claims otherwise, no durable epoch advances, and one unit of the
     * bounded budget is consumed. */
    pool_api_heartbeat_record_failure(&p->hb, in.monotonic_us, false);
    ESP_LOGW(TAG, "heartbeat not durably proven (%s)", pool_runtime_status_str(st));
    /* Re-evaluate immediately so an exhausted budget fails safe on THIS
     * tick rather than one cadence window later. */
    if (pool_api_heartbeat_failsafe_due(&p->hb, in.monotonic_us) &&
        !p->hb.failsafe_engaged) {
        api_engage_failsafe(p, EXEC_REASON_PERSIST_FAILED);
    }
}

/* ------------------------------------------------------------------ */
/* Status publication                                                  */
/* ------------------------------------------------------------------ */

static void api_publish_status(PoolSessionApiProcessor *p)
{
    PoolSessionRuntime   *rt = p->rt;
    PoolApiStatusInput    in;
    PoolApiStatus         built;
    PoolExecutionSnapshot es;
    PoolRuntimeSnapshot   rs;
    bool                  have_session;

    memset(&in, 0, sizeof(in));
    memset(&es, 0, sizeof(es));
    pool_runtime_snapshot_init(&rs);
    (void)pool_session_runtime_snapshot(rt, &rs);

    in.api_enabled         = true;
    in.runtime_initialized = rt->initialized && rt->booted;
#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
    in.execution_enabled = true;
#else
    in.execution_enabled = false;
#endif

    have_session        = api_durable_session(p);
    in.session_present  = have_session;
    in.durable_state    = have_session ? (uint8_t)rt->record.state : (uint8_t)POOL_STATE_IDLE;
    in.durable_failure  = have_session ? rt->record.last_failure_code : 0u;
    in.durable_restore_required = have_session && rt->record.restore_required;

    in.runtime_state            = (uint8_t)rs.state;
    in.protocol_start_permitted = (rs.protocol == POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE);

    if (p->ex != NULL && pool_session_executor_snapshot(p->ex, &es) == EXEC_REASON_NONE) {
        in.execution_state     = (uint8_t)es.state;
        in.execution_reason    = (uint8_t)es.reason;
        in.asic_gate           = (uint8_t)es.gate;
        in.mining_grant_active = es.mining_grant_active;
    }

    in.lease_owner      = (uint8_t)rt->lease.owner;
    in.lease_phase      = (uint8_t)rt->lease.phase;
    in.terminal_pending = rt->lease.terminal_pending;

    in.trusted_time_required  = rt->plan.trusted_time_required;
    in.trusted_time_available = rt->time_snapshot.trusted &&
                                rt->time_snapshot.status == TIME_OK;
    in.remaining_valid = rt->plan.remaining_valid;
    in.remaining_s     = rt->plan.remaining_target_s;

    in.command_pending = (pool_api_queue_depth(&p->queue) > 0u);
    if (in.command_pending) {
        /* The bounded head kind only — never a target identity. */
        portENTER_CRITICAL(&s_api_lock);
        in.pending_command = p->queue.slots[p->queue.head].kind;
        portEXIT_CRITICAL(&s_api_lock);
    }
    in.last_command            = p->last_kind;
    in.last_command_result     = p->last_result;
    in.last_client_request_id  = p->last_client_request_id;

    in.heartbeat_status  = p->hb.status;
    in.heartbeat_commits = p->hb.commits;
    in.api_conflict_code = p->last_conflict_code;

    p->status_sequence = (p->status_sequence == UINT32_MAX) ? 1u : (p->status_sequence + 1u);
    in.status_sequence = p->status_sequence;

    pool_api_status_build(&in, &built);

    portENTER_CRITICAL(&s_api_lock);
    p->status = built;
    portEXIT_CRITICAL(&s_api_lock);
}

/* ------------------------------------------------------------------ */
/* Lifecycle and the bounded owner-task step                           */
/* ------------------------------------------------------------------ */

void pool_api_processor_init(PoolSessionApiProcessor *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    pool_api_queue_init(&p->queue);
    pool_api_heartbeat_init(&p->hb);
    pool_api_status_init(&p->status);
    p->initialized  = true;
    p->last_kind    = POOL_API_CMD_NONE;
    p->last_result  = API_CMD_RESULT_NONE;
}

PoolApiSubmitStatus pool_api_processor_bind(PoolSessionApiProcessor *p,
                                            PoolSessionRuntime *rt,
                                            const PoolApiSourceOps *ops, void *ctx,
                                            PoolSessionExecutor *ex)
{
    if (p == NULL || !p->initialized) {
        return API_SUBMIT_NOT_READY;
    }
    if (rt == NULL || !rt->initialized || !rt->booted) {
        return API_SUBMIT_NOT_READY;
    }
    if (ops == NULL || ops->device_identity == NULL || ops->read_effective == NULL ||
        ops->source_password_retained == NULL) {
        return API_SUBMIT_INVALID; /* fail closed on an incomplete adapter */
    }
    p->rt      = rt;
    p->src_ops = ops;
    p->src_ctx = ctx;
    p->ex      = ex;
    p->bound   = true;
    api_publish_status(p);
    return API_SUBMIT_ACCEPTED;
}

void pool_api_processor_deinit(PoolSessionApiProcessor *p)
{
    if (p == NULL) {
        return;
    }
    /* Never releases ownership, never clears a record, never reopens a gate. */
    memset(p, 0, sizeof(*p));
}

PoolApiSubmitStatus pool_api_processor_submit(PoolSessionApiProcessor *p,
                                              const PoolApiCommand *cmd,
                                              uint32_t *out_sequence)
{
    PoolApiSubmitStatus rc;

    if (out_sequence != NULL) {
        *out_sequence = 0u;
    }
    if (p == NULL || !p->initialized || !p->bound) {
        return API_SUBMIT_NOT_READY;
    }
    /*
     * Requirement: the create route must never accept a session that has no
     * viable executor-adoption path. With no executor bound the submission
     * is refused here, so the client gets 503 instead of a 202 for a
     * session that could never run.
     */
    if (cmd != NULL && cmd->kind == POOL_API_CMD_CREATE_SESSION && p->ex == NULL) {
        return API_SUBMIT_NOT_READY;
    }
    rc = pool_api_queue_submit(&p->queue, cmd, out_sequence);
    if (rc == API_SUBMIT_ACCEPTED) {
        portENTER_CRITICAL(&s_api_lock);
        p->status.command_pending = true;
        p->status.pending_command = cmd->kind;
        portEXIT_CRITICAL(&s_api_lock);
    }
    return rc;
}

static void api_process_command(PoolSessionApiProcessor *p, const PoolApiCommand *cmd)
{
    PoolApiCommandResult rc;

    p->last_kind              = cmd->kind;
    p->last_client_request_id = cmd->client_request_id;
    p->last_conflict_code     = (uint8_t)OP_HTTP_NONE;

    if (!pool_api_command_valid(cmd)) {
        p->last_result = API_CMD_RESULT_REJECTED_VALIDATION;
        return;
    }
    /*
     * api_owns_flow is a BOUNDED owner-task critical-workflow flag: it is
     * set for the duration of THIS command transaction and cleared on every
     * return path below. It never persists while waiting for another HTTP
     * request or for future code — a created session is handed to the
     * executor, or safely cancelled, before this function returns.
     */
    p->api_owns_flow = true;
    switch (cmd->kind) {
    case POOL_API_CMD_CREATE_SESSION:
        rc = api_process_create(p, cmd);
        break;
    case POOL_API_CMD_RESTORE_NOW:
        rc = api_process_restore(p);
        break;
    case POOL_API_CMD_ACKNOWLEDGE_TERMINAL:
        rc = api_process_acknowledge(p);
        break;
    default:
        rc = API_CMD_RESULT_REJECTED_VALIDATION;
        break;
    }
    p->api_owns_flow = false; /* every completed path clears it */

    p->last_result = rc;
    if (p->processed_count < UINT32_MAX) {
        p->processed_count++;
    }
    if (rc != API_CMD_RESULT_ACCEPTED) {
        ESP_LOGW(TAG, "command %s -> %s", pool_api_command_kind_str(cmd->kind),
                 pool_api_command_result_str(rc));
    }
}

bool pool_api_processor_step(PoolSessionApiProcessor *p)
{
    PoolApiCommand cmd;

    if (p == NULL || !p->initialized || !p->bound || p->rt == NULL) {
        return false;
    }
    api_refresh_lease(p);

    if (pool_api_queue_take(&p->queue, &cmd)) {
        api_process_command(p, &cmd);
        /* No command bytes outlive the step, and no pointer is retained. */
        memset(&cmd, 0, sizeof(cmd));
    }

    api_step_heartbeat(p);
    api_publish_status(p);

    /*
     * The flow is owned while EITHER a command transaction is in progress
     * (bounded, this task, this call) OR the executor holds the session.
     * Reporting the union closes the handoff window with no gap: the B6
     * re-planner can never observe an in-boot creation as an "interrupted"
     * boot record, and there is never an interval with no owner while a
     * durable session is active.
     */
    return p->api_owns_flow || pool_session_executor_owns_flow(p->ex);
}

void pool_api_processor_status(const PoolSessionApiProcessor *p, PoolApiStatus *out)
{
    if (out == NULL) {
        return;
    }
    if (p == NULL || !p->initialized) {
        pool_api_status_init(out);
        return;
    }
    portENTER_CRITICAL(&s_api_lock);
    *out = p->status;
    portEXIT_CRITICAL(&s_api_lock);
}

uint32_t pool_api_processor_processed(const PoolSessionApiProcessor *p)
{
    return (p == NULL) ? 0u : p->processed_count;
}

/* ------------------------------------------------------------------ */
/* Production singleton (feature-gated)                                */
/* ------------------------------------------------------------------ */

bool pool_api_feature_enabled(void)
{
#ifdef CONFIG_NX_TIMED_SESSIONS_API
    return true;
#else
    return false;
#endif
}

#ifdef CONFIG_NX_TIMED_SESSIONS_API
static PoolSessionApiProcessor s_default_processor;

PoolSessionApiProcessor *pool_api_default_processor(void)
{
    return &s_default_processor;
}
#else
PoolSessionApiProcessor *pool_api_default_processor(void)
{
    /* No processor storage, no command queue and no route exists. */
    return NULL;
}
#endif
