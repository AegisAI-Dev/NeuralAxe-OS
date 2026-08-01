/*
 * NeuralAxe timed pool sessions — HTTP adapter for the Gate B8 control
 * plane (Phase 2M.1B). See pool_session_api.h for the contract.
 *
 * THE HANDLERS ONLY VALIDATE AND SUBMIT. There is no pool write, no
 * Stratum call, no ASIC grant, no esp_restart() and no session-store write
 * anywhere in this file — grep-checkable and asserted by the QEMU suite.
 */

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "cJSON.h"

#include "pool_session_api.h"
#include "http_server.h"

/* Repository convention (see axe-os/api/system/asic_settings.c). */
extern esp_err_t set_cors_headers(httpd_req_t *req);

#ifdef CONFIG_NX_TIMED_SESSIONS
#include "pool_operation_http_policy.h"
#include "pool_operation_types.h"
#endif

#ifdef CONFIG_NX_TIMED_SESSIONS_API
#include "pool_session.h"
#include "pool_session_api_parser.h"
#include "pool_session_api_status.h"
#include "pool_session_command.h"
#include "pool_session_runtime_core.h"
#include "pool_session_execution_core.h"

static const char *TAG = "nx_api_http";
static int         s_api_prebuffer_len = 512;
#endif

/* ------------------------------------------------------------------ */
/* Standardized sanitized 409 conflict body                            */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_NX_TIMED_SESSIONS
esp_err_t nx_pool_session_api_send_conflict(httpd_req_t *req,
                                            NxAdmissionVerdict verdict,
                                            const PoolOperationHttpConflict *conflict)
{
#ifdef CONFIG_NX_TIMED_SESSIONS_API
    cJSON    *root;
    esp_err_t res;
    PoolOperationHttpConflict fallback;

    if (conflict == NULL) {
        memset(&fallback, 0, sizeof(fallback));
        fallback.http_status  = 409;
        fallback.code         = OP_HTTP_OPERATION_INVALID_REQUEST;
        fallback.active_owner = OP_OWNER_NONE;
        conflict              = &fallback;
    }

    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");

    root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Memory allocation failed");
    }
    /* ONLY the committed B5 sanitized surface. No token, no generation, no
     * session id and no pool identity can appear here. */
    cJSON_AddStringToObject(root, "code", pool_operation_http_code_str(conflict->code));
    cJSON_AddBoolToObject(root, "retryable", conflict->retryable);
    cJSON_AddStringToObject(root, "activeOwner",
                            pool_operation_owner_str(conflict->active_owner));
    cJSON_AddBoolToObject(root, "restoreRequired", conflict->restore_required);
    cJSON_AddBoolToObject(root, "terminalAckRequired", conflict->terminal_ack_required);

    res = HTTP_send_json(req, root, &s_api_prebuffer_len);
    cJSON_Delete(root);
    (void)verdict;
    return res;
#else
    /* API flag OFF: the committed Gate B7 bare-token body, byte-for-byte,
     * so build postures B and C are unchanged. */
    (void)conflict;
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, nx_admission_verdict_str(verdict));
#endif
}
#endif /* CONFIG_NX_TIMED_SESSIONS */

/* ------------------------------------------------------------------ */
/* Routes (API flag only)                                              */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_NX_TIMED_SESSIONS_API

/* Bounded body intake. Returns false when the request exceeded the strict
 * bound or the socket read failed; the caller has already answered. */
static bool api_read_body(httpd_req_t *req, char *buf, size_t cap, size_t *out_len)
{
    int total = req->content_len;
    int cur   = 0;
    int got;

    *out_len = 0;
    if (total < 0 || (size_t)total > cap) {
        cJSON *root = cJSON_CreateObject();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        if (root != NULL) {
            cJSON_AddStringToObject(root, "code",
                                    pool_api_validation_str(API_ERR_BODY_TOO_LARGE));
            (void)HTTP_send_json(req, root, &s_api_prebuffer_len);
            cJSON_Delete(root);
        } else {
            (void)httpd_resp_sendstr(req, "{}");
        }
        return false;
    }
    while (cur < total) {
        got = httpd_req_recv(req, buf + cur, total - cur);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            (void)httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body read failed");
            return false;
        }
        cur += got;
    }
    *out_len = (size_t)cur;
    return true;
}

static esp_err_t api_send_validation_error(httpd_req_t *req, PoolApiValidation v)
{
    cJSON    *root = cJSON_CreateObject();
    esp_err_t res;

    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Memory allocation failed");
    }
    /* A stable machine code only: the rejected VALUE is never echoed, so a
     * password-like field can never be reflected back to a client. */
    cJSON_AddStringToObject(root, "code", pool_api_validation_str(v));
    res = HTTP_send_json(req, root, &s_api_prebuffer_len);
    cJSON_Delete(root);
    return res;
}

static esp_err_t api_send_submit_result(httpd_req_t *req, PoolApiSubmitStatus st,
                                        uint32_t sequence, uint32_t client_request_id)
{
    cJSON    *root;
    esp_err_t res;

    if (st == API_SUBMIT_INVALID) {
        return api_send_validation_error(req, API_ERR_BODY_MALFORMED);
    }
    root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Memory allocation failed");
    }
    if (st == API_SUBMIT_ACCEPTED) {
        /* 202: submission is NOT completion. The authoritative outcome
         * becomes visible through the status route. */
        httpd_resp_set_status(req, "202 Accepted");
        cJSON_AddBoolToObject(root, "accepted", true);
        cJSON_AddNumberToObject(root, "requestSequence", (double)sequence);
        cJSON_AddNumberToObject(root, "clientRequestId", (double)client_request_id);
    } else {
        httpd_resp_set_status(req, "503 Service Unavailable");
        cJSON_AddBoolToObject(root, "accepted", false);
        cJSON_AddStringToObject(root, "code", pool_api_submit_str(st));
        cJSON_AddBoolToObject(root, "retryable", st == API_SUBMIT_QUEUE_FULL);
    }
    httpd_resp_set_type(req, "application/json");
    res = HTTP_send_json(req, root, &s_api_prebuffer_len);
    cJSON_Delete(root);
    return res;
}

/* Every mutating route consults the SAME committed admission fence before
 * reading or applying anything, and answers with the standardized body. */
static bool api_admitted(httpd_req_t *req, esp_err_t *out_res)
{
    NxAdmissionVerdict        verdict = NX_ADMIT_ALLOW;
    PoolOperationHttpConflict conflict;

    memset(&conflict, 0, sizeof(conflict));
    /*
     * A timed-session control request is itself a session-store mutation
     * class, so the pool-configuration fence is the correct gate: a manual
     * PATCH, an OTA or a recovery guard must all block it.
     */
    if (nx_timed_sessions_mutation_conflict(NX_MUTATION_POOL_CONFIG, &verdict, &conflict)) {
        return true;
    }
    *out_res = nx_pool_session_api_send_conflict(req, verdict, &conflict);
    return false;
}

static esp_err_t api_submit_action(httpd_req_t *req, PoolApiCommandKind kind)
{
    char                     body[POOL_API_MAX_BODY_BYTES];
    size_t                   len = 0;
    uint32_t                 request_id = 0u;
    uint32_t                 sequence = 0u;
    PoolApiValidation        v;
    PoolApiCommand           cmd;
    PoolApiSubmitStatus      st;
    esp_err_t                res = ESP_OK;

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    if (!api_admitted(req, &res)) {
        return res;
    }
    if (!api_read_body(req, body, sizeof(body), &len)) {
        return ESP_OK;
    }
    v = pool_api_parse_action(body, len, &request_id);
    memset(body, 0, sizeof(body));
    if (v != API_VALID_OK) {
        return api_send_validation_error(req, v);
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind              = kind;
    cmd.actor             = POOL_API_ACTOR_LOCAL_HTTP;
    cmd.client_request_id = request_id;

    /* Submission only: the owner task performs every state change. */
    st = pool_api_processor_submit(pool_api_default_processor(), &cmd, &sequence);
    memset(&cmd, 0, sizeof(cmd));
    return api_send_submit_result(req, st, sequence, request_id);
}

static esp_err_t POST_timed_session_create(httpd_req_t *req)
{
    char                 body[POOL_API_MAX_BODY_BYTES];
    size_t               len = 0;
    PoolApiCreateRequest parsed;
    PoolApiValidation    v;
    PoolApiCommand       cmd;
    PoolApiSubmitStatus  st;
    uint32_t             sequence = 0u;
    esp_err_t            res = ESP_OK;

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    if (!api_admitted(req, &res)) {
        return res;
    }
    if (!api_read_body(req, body, sizeof(body), &len)) {
        return ESP_OK;
    }

    v = pool_api_parse_create(body, len, &parsed);
    /* The request bytes never outlive the parse. */
    memset(body, 0, sizeof(body));
    if (v != API_VALID_OK) {
        memset(&parsed, 0, sizeof(parsed));
        return api_send_validation_error(req, v);
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind              = POOL_API_CMD_CREATE_SESSION;
    cmd.actor             = POOL_API_ACTOR_LOCAL_HTTP;
    cmd.client_request_id = parsed.client_request_id;
    cmd.create            = parsed;

    st = pool_api_processor_submit(pool_api_default_processor(), &cmd, &sequence);
    res = api_send_submit_result(req, st, sequence, parsed.client_request_id);
    memset(&cmd, 0, sizeof(cmd));
    memset(&parsed, 0, sizeof(parsed));
    return res;
}

static esp_err_t POST_timed_session_restore(httpd_req_t *req)
{
    return api_submit_action(req, POOL_API_CMD_RESTORE_NOW);
}

static esp_err_t POST_timed_session_acknowledge(httpd_req_t *req)
{
    return api_submit_action(req, POOL_API_CMD_ACKNOWLEDGE_TERMINAL);
}

/* Read-only sanitized status. It calls NO external operation: it copies the
 * snapshot the owner task published. */
static esp_err_t GET_timed_session(httpd_req_t *req)
{
    PoolApiStatus s;
    cJSON        *root;
    esp_err_t     res;

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    pool_api_processor_status(pool_api_default_processor(), &s);

    root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Memory allocation failed");
    }
    httpd_resp_set_type(req, "application/json");

    /* Stable machine tokens, booleans and bounded numbers ONLY. No pool
     * host, port, account, worker, wallet, password, session id, lease
     * token or generation exists in the model this is built from. */
    cJSON_AddBoolToObject(root, "apiEnabled", s.api_enabled);
    cJSON_AddBoolToObject(root, "runtimeInitialized", s.runtime_initialized);
    cJSON_AddBoolToObject(root, "executionEnabled", s.execution_enabled);
    cJSON_AddBoolToObject(root, "sessionPresent", s.session_present);
    cJSON_AddBoolToObject(root, "terminalResultPending", s.terminal_result_pending);
    cJSON_AddStringToObject(root, "durableState", pool_session_state_str(s.durable_state));
    cJSON_AddStringToObject(root, "durableFailure",
                            pool_session_error_str((PoolSessionError)s.durable_failure));
    cJSON_AddStringToObject(root, "runtimeState",
                            pool_runtime_state_str((PoolRuntimeState)s.runtime_state));
    cJSON_AddStringToObject(root, "executionState",
                            pool_exec_state_str((PoolExecState)s.execution_state));
    cJSON_AddStringToObject(root, "executionReason",
                            pool_exec_reason_str((PoolExecReason)s.execution_reason));
    cJSON_AddStringToObject(root, "leaseOwner",
                            pool_operation_owner_str((PoolOperationOwner)s.lease_owner));
    cJSON_AddStringToObject(root, "leasePhase",
                            pool_operation_phase_str((PoolOperationLeasePhase)s.lease_phase));
    cJSON_AddBoolToObject(root, "protocolStartPermitted", s.protocol_start_permitted);
    cJSON_AddStringToObject(root, "asicGate",
                            pool_exec_gate_str((PoolExecGatePosture)s.asic_gate));
    cJSON_AddBoolToObject(root, "targetMiningGrantActive", s.target_mining_grant_active);
    cJSON_AddBoolToObject(root, "restoreRequired", s.restore_required);
    cJSON_AddBoolToObject(root, "operatorRecoveryRequired", s.operator_recovery_required);
    cJSON_AddBoolToObject(root, "trustedTimeRequired", s.trusted_time_required);
    cJSON_AddBoolToObject(root, "trustedTimeAvailable", s.trusted_time_available);
    cJSON_AddStringToObject(root, "deadlineStatus", pool_api_deadline_str(s.deadline_status));
    cJSON_AddBoolToObject(root, "remainingSecondsValid", s.remaining_seconds_valid);
    cJSON_AddNumberToObject(root, "remainingSeconds", (double)s.remaining_seconds);
    cJSON_AddBoolToObject(root, "commandPending", s.command_pending);
    cJSON_AddStringToObject(root, "pendingCommand",
                            pool_api_command_kind_str(s.pending_command));
    cJSON_AddStringToObject(root, "lastCommand", pool_api_command_kind_str(s.last_command));
    cJSON_AddStringToObject(root, "lastCommandResult",
                            pool_api_command_result_str(s.last_command_result));
    cJSON_AddNumberToObject(root, "lastClientRequestId", (double)s.last_client_request_id);
    cJSON_AddStringToObject(root, "heartbeatStatus",
                            pool_api_heartbeat_str(s.heartbeat_status));
    cJSON_AddNumberToObject(root, "heartbeatCommits", (double)s.heartbeat_commits);
    cJSON_AddStringToObject(root, "apiConflictCode",
                            pool_operation_http_code_str(
                                (PoolOperationHttpCode)s.api_conflict_code));
    cJSON_AddNumberToObject(root, "statusSequence", (double)s.status_sequence);

    res = HTTP_send_json(req, root, &s_api_prebuffer_len);
    cJSON_Delete(root);
    return res;
}

esp_err_t nx_pool_session_api_register_routes(httpd_handle_t server, void *rest_context)
{
    httpd_uri_t status_get = {
        .uri = "/api/system/timed-session",
        .method = HTTP_GET,
        .handler = GET_timed_session,
        .user_ctx = rest_context,
    };
    httpd_uri_t create_post = {
        .uri = "/api/system/timed-session",
        .method = HTTP_POST,
        .handler = POST_timed_session_create,
        .user_ctx = rest_context,
    };
    httpd_uri_t restore_post = {
        .uri = "/api/system/timed-session/restore",
        .method = HTTP_POST,
        .handler = POST_timed_session_restore,
        .user_ctx = rest_context,
    };
    httpd_uri_t acknowledge_post = {
        .uri = "/api/system/timed-session/acknowledge",
        .method = HTTP_POST,
        .handler = POST_timed_session_acknowledge,
        .user_ctx = rest_context,
    };

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &create_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &restore_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &acknowledge_post));
    ESP_LOGI(TAG, "Gate B8 timed-session routes registered (API flag enabled)");
    return ESP_OK;
}

#endif /* CONFIG_NX_TIMED_SESSIONS_API — with the flag off this file emits
        * no route, no handler, no command queue and no singleton; the
        * registration entry point is a static inline no-op in the header. */
