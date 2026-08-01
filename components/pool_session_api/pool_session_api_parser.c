/*
 * NeuralAxe timed pool sessions — PURE request parsing and validation
 * (Phase 2M.1B, Gate B8). See pool_session_api_parser.h for the contract.
 *
 * Everything here is deterministic and fail-closed. The only external
 * dependency is cJSON, the repository's committed JSON reader; there is no
 * ESP-IDF include, no HTTP-server dependency, no NVS, no lease, no store,
 * no pool and no logging. A rejected request never leaves a partially
 * parsed value in the output.
 */

#include <string.h>
#include <ctype.h>
#include <math.h>
#include "cJSON.h"
#include "pool_session_api_parser.h"

/* ------------------------------------------------------------------ */
/* Small bounded helpers                                               */
/* ------------------------------------------------------------------ */

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive substring test over bounded NUL-terminated names. */
static bool ci_contains(const char *hay, const char *needle)
{
    size_t hl;
    size_t nl;
    size_t i;
    size_t j;

    if (hay == NULL || needle == NULL) {
        return false;
    }
    hl = strlen(hay);
    nl = strlen(needle);
    if (nl == 0u || nl > hl) {
        return false;
    }
    for (i = 0; i + nl <= hl; i++) {
        for (j = 0; j < nl; j++) {
            if (lower_ascii(hay[i + j]) != lower_ascii(needle[j])) {
                break;
            }
        }
        if (j == nl) {
            return true;
        }
    }
    return false;
}

static bool ci_contains_any(const char *hay, const char *const *list, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (ci_contains(hay, list[i])) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Deny lists (checked BEFORE the unknown-field rule so the client gets */
/* the specific, actionable machine code)                              */
/* ------------------------------------------------------------------ */

bool pool_api_key_is_password_like(const char *key)
{
    static const char *const DENY[] = {
        "password", "passwd", "pass", "pwd", "secret",
        "token",    "key",    "credential", "auth",
    };
    return ci_contains_any(key, DENY, sizeof(DENY) / sizeof(DENY[0]));
}

bool pool_api_key_is_source_like(const char *key)
{
    static const char *const DENY[] = {
        "source", "current_pool", "currentpool", "from_pool", "frompool",
        "origin_pool", "previous",
    };
    return ci_contains_any(key, DENY, sizeof(DENY) / sizeof(DENY[0]));
}

bool pool_api_key_is_session_id_like(const char *key)
{
    static const char *const DENY[] = { "session" };
    return ci_contains_any(key, DENY, sizeof(DENY) / sizeof(DENY[0]));
}

bool pool_api_key_is_internal_state_like(const char *key)
{
    static const char *const DENY[] = {
        "restore_required", "restorerequired", "lease", "generation",
        "record",           "nvs",             "tombstone", "terminal",
        "grant",            "phase",           "owner",     "state",
        "certificate",      "cert",
    };
    return ci_contains_any(key, DENY, sizeof(DENY) / sizeof(DENY[0]));
}

/* One ordered classification of a rejected key. Returns API_VALID_OK when
 * the key is not on any deny list. */
static PoolApiValidation classify_denied_key(const char *key)
{
    if (pool_api_key_is_password_like(key)) {
        return API_ERR_PASSWORD_FIELD_REJECTED;
    }
    if (pool_api_key_is_source_like(key)) {
        return API_ERR_SOURCE_FIELD_REJECTED;
    }
    if (pool_api_key_is_session_id_like(key)) {
        return API_ERR_SESSION_ID_REJECTED;
    }
    if (pool_api_key_is_internal_state_like(key)) {
        return API_ERR_INTERNAL_FIELD_REJECTED;
    }
    return API_VALID_OK;
}

/* ------------------------------------------------------------------ */
/* Bounded token decoders (total; never normalize an unknown token)    */
/* ------------------------------------------------------------------ */

bool pool_api_protocol_from_token(const char *token, PoolSessionProtocol *out)
{
    if (token == NULL || out == NULL) {
        return false;
    }
    if (strcmp(token, "stratum_v1") == 0) {
        *out = POOL_PROTO_STRATUM_V1;
        return true;
    }
    if (strcmp(token, "stratum_v2") == 0) {
        *out = POOL_PROTO_STRATUM_V2;
        return true;
    }
    return false;
}

bool pool_api_tls_mode_from_token(const char *token, uint8_t *out_mode,
                                  bool *out_is_custom)
{
    if (out_is_custom != NULL) {
        *out_is_custom = false;
    }
    if (token == NULL || out_mode == NULL) {
        return false;
    }
    if (strcmp(token, "disabled") == 0) {
        *out_mode = POOL_API_TLS_DISABLED;
        return true;
    }
    if (strcmp(token, "bundled") == 0) {
        *out_mode = POOL_API_TLS_BUNDLED;
        return true;
    }
    if (strcmp(token, "custom") == 0) {
        /* Representable as a REQUEST, never as a session: a custom
         * certificate cannot be restored exactly by the committed B7
         * transaction, so it is refused before any command is built. */
        if (out_is_custom != NULL) {
            *out_is_custom = true;
        }
    }
    return false;
}

bool pool_api_chain_from_token(const char *token, PoolChainType *out)
{
    if (token == NULL || out == NULL) {
        return false;
    }
    if (strcmp(token, "bitcoin") == 0) {
        *out = POOL_CHAIN_BITCOIN;
        return true;
    }
    if (strcmp(token, "bitcoin_cash") == 0) {
        *out = POOL_CHAIN_BITCOIN_CASH;
        return true;
    }
    if (strcmp(token, "custom_unknown") == 0) {
        *out = POOL_CHAIN_CUSTOM_UNKNOWN;
        return true;
    }
    return false;
}

const char *pool_api_protocol_token(PoolSessionProtocol p)
{
    switch (p) {
    case POOL_PROTO_STRATUM_V1: return "stratum_v1";
    case POOL_PROTO_STRATUM_V2: return "stratum_v2";
    default:                    return "unknown";
    }
}

const char *pool_api_tls_mode_token(uint8_t mode)
{
    switch (mode) {
    case POOL_API_TLS_DISABLED: return "disabled";
    case POOL_API_TLS_BUNDLED:  return "bundled";
    default:                    return "unknown";
    }
}

const char *pool_api_chain_token(PoolChainType c)
{
    switch (c) {
    case POOL_CHAIN_BITCOIN:        return "bitcoin";
    case POOL_CHAIN_BITCOIN_CASH:   return "bitcoin_cash";
    case POOL_CHAIN_CUSTOM_UNKNOWN: return "custom_unknown";
    default:                        return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded body intake                                                 */
/* ------------------------------------------------------------------ */

/*
 * Copy a bounded body into `buf` (capacity POOL_API_MAX_BODY_BYTES + 1) and
 * NUL-terminate it. Rejects an oversized body and any embedded NUL (which
 * would silently truncate the document). `*out_empty` reports a body that
 * carries no non-whitespace byte.
 */
static PoolApiValidation intake_body(const char *body, size_t len, char *buf,
                                     bool *out_empty)
{
    size_t i;
    bool   any = false;

    *out_empty = true;
    if (len > (size_t)POOL_API_MAX_BODY_BYTES) {
        return API_ERR_BODY_TOO_LARGE;
    }
    if (len != 0u && body == NULL) {
        return API_ERR_BODY_MALFORMED;
    }
    for (i = 0; i < len; i++) {
        if (body[i] == '\0') {
            return API_ERR_BODY_MALFORMED; /* embedded NUL: never truncate */
        }
        if (!isspace((unsigned char)body[i])) {
            any = true;
        }
        buf[i] = body[i];
    }
    buf[len] = '\0';
    *out_empty = !any;
    return API_VALID_OK;
}

/* ------------------------------------------------------------------ */
/* Strict object walk                                                  */
/* ------------------------------------------------------------------ */

/*
 * Walk every child exactly once: reject a missing name, a duplicate name, a
 * denied name, an unknown name and an over-budget object. Known names are
 * marked in `seen` (index-aligned with `known`).
 */
static PoolApiValidation walk_object(const cJSON *root, const char *const *known,
                                     size_t known_n, bool *seen)
{
    const cJSON *child;
    size_t       count = 0u;
    size_t       i;

    for (i = 0; i < known_n; i++) {
        seen[i] = false;
    }
    for (child = root->child; child != NULL; child = child->next) {
        PoolApiValidation denied;
        bool              matched = false;

        if (++count > (size_t)POOL_API_MAX_JSON_KEYS) {
            return API_ERR_TOO_MANY_FIELDS;
        }
        if (child->string == NULL) {
            return API_ERR_BODY_NOT_OBJECT; /* array element inside an object */
        }
        denied = classify_denied_key(child->string);
        if (denied != API_VALID_OK) {
            return denied;
        }
        for (i = 0; i < known_n; i++) {
            if (strcmp(child->string, known[i]) == 0) {
                if (seen[i]) {
                    return API_ERR_DUPLICATE_FIELD;
                }
                seen[i] = true;
                matched = true;
                break;
            }
        }
        if (!matched) {
            return API_ERR_UNKNOWN_FIELD;
        }
    }
    return API_VALID_OK;
}

/*
 * Strict unsigned-integer extraction. A JSON null, boolean, string (even a
 * numeric string), object, array or fractional/negative/out-of-range number
 * is rejected — numbers are never coerced.
 */
static PoolApiValidation get_uint(const cJSON *root, const char *name,
                                  uint64_t max, uint64_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    double       v;

    if (item == NULL) {
        return API_ERR_MISSING_FIELD;
    }
    if (cJSON_IsNull(item)) {
        return API_ERR_NULL_FIELD;
    }
    if (!cJSON_IsNumber(item)) {
        return API_ERR_TYPE_MISMATCH;
    }
    v = item->valuedouble;
    if (!isfinite(v) || v < 0.0 || v != floor(v) || v > (double)max) {
        return API_ERR_TYPE_MISMATCH;
    }
    *out = (uint64_t)v;
    return API_VALID_OK;
}

/* Strict string extraction: returns the borrowed value; never a number. */
static PoolApiValidation get_string(const cJSON *root, const char *name,
                                    const char **out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

    if (item == NULL) {
        return API_ERR_MISSING_FIELD;
    }
    if (cJSON_IsNull(item)) {
        return API_ERR_NULL_FIELD;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return API_ERR_TYPE_MISMATCH;
    }
    *out = item->valuestring;
    return API_VALID_OK;
}

/* ------------------------------------------------------------------ */
/* Create-request parsing                                              */
/* ------------------------------------------------------------------ */

static const char *const CREATE_KEYS[] = {
    "duration_seconds",  /* 0 */
    "target_host",       /* 1 */
    "target_port",       /* 2 */
    "target_account",    /* 3 */
    "target_protocol",   /* 4 */
    "target_tls_mode",   /* 5 */
    "target_chain",      /* 6 */
    "client_request_id", /* 7 — optional */
};
#define CREATE_KEY_COUNT (sizeof(CREATE_KEYS) / sizeof(CREATE_KEYS[0]))
#define CREATE_KEY_REQUEST_ID 7u

PoolApiValidation pool_api_parse_create(const char *body, size_t len,
                                        PoolApiCreateRequest *out)
{
    char              buf[POOL_API_MAX_BODY_BYTES + 1u];
    bool              seen[CREATE_KEY_COUNT];
    bool              empty = true;
    cJSON            *root = NULL;
    PoolApiValidation rc;
    uint64_t          n;
    const char       *s;
    size_t            slen;
    PoolApiCreateRequest work;

    if (out == NULL) {
        return API_ERR_BODY_MALFORMED;
    }
    memset(out, 0, sizeof(*out));
    memset(&work, 0, sizeof(work));

    rc = intake_body(body, len, buf, &empty);
    if (rc != API_VALID_OK) {
        return rc;
    }
    if (empty) {
        return API_ERR_MISSING_FIELD; /* a create needs a target */
    }

    root = cJSON_Parse(buf);
    if (root == NULL) {
        return API_ERR_BODY_MALFORMED;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return API_ERR_BODY_NOT_OBJECT;
    }

    rc = walk_object(root, CREATE_KEYS, CREATE_KEY_COUNT, seen);
    if (rc != API_VALID_OK) {
        goto done;
    }
    /* Every key except the optional diagnostics id is required. */
    {
        size_t i;
        for (i = 0; i < CREATE_KEY_COUNT; i++) {
            if (i != (size_t)CREATE_KEY_REQUEST_ID && !seen[i]) {
                rc = API_ERR_MISSING_FIELD;
                goto done;
            }
        }
    }

    /* duration_seconds — the B1 bounds are the only authority. */
    rc = get_uint(root, "duration_seconds", 0xFFFFFFFFull, &n);
    if (rc != API_VALID_OK) {
        goto done;
    }
    if (n < (uint64_t)POOL_SESSION_MIN_DURATION_S ||
        n > (uint64_t)POOL_SESSION_MAX_DURATION_S) {
        rc = API_ERR_DURATION_OUT_OF_RANGE;
        goto done;
    }
    work.duration_s = (uint32_t)n;

    /* target_host — bounded, non-empty, never trimmed (trimming would
     * change the pool identity that must later be compared exactly). */
    rc = get_string(root, "target_host", &s);
    if (rc != API_VALID_OK) {
        goto done;
    }
    slen = strlen(s);
    if (slen == 0u) {
        rc = API_ERR_HOST_EMPTY;
        goto done;
    }
    if (slen >= (size_t)POOL_SESSION_HOST_MAX) {
        rc = API_ERR_HOST_TOO_LONG;
        goto done;
    }
    memcpy(work.target_host, s, slen);
    work.target_host[slen] = '\0';

    /* target_port */
    rc = get_uint(root, "target_port", 0xFFFFFFFFull, &n);
    if (rc != API_VALID_OK) {
        goto done;
    }
    if (n == 0u || n > 65535u) {
        rc = API_ERR_PORT_INVALID;
        goto done;
    }
    work.target_port = (uint16_t)n;

    /* target_account — bounded and (deliberately stricter than B1) never
     * empty: an unattended session with no account is a configuration
     * mistake, not a valid request. */
    rc = get_string(root, "target_account", &s);
    if (rc != API_VALID_OK) {
        goto done;
    }
    slen = strlen(s);
    if (slen == 0u) {
        rc = API_ERR_ACCOUNT_EMPTY;
        goto done;
    }
    if (slen >= (size_t)POOL_SESSION_USER_MAX) {
        rc = API_ERR_ACCOUNT_TOO_LONG;
        goto done;
    }
    memcpy(work.target_user, s, slen);
    work.target_user[slen] = '\0';

    /* target_protocol */
    rc = get_string(root, "target_protocol", &s);
    if (rc != API_VALID_OK) {
        goto done;
    }
    if (!pool_api_protocol_from_token(s, &work.target_protocol)) {
        rc = API_ERR_PROTOCOL_UNSUPPORTED;
        goto done;
    }

    /* target_tls_mode — custom certificates are refused explicitly. */
    rc = get_string(root, "target_tls_mode", &s);
    if (rc != API_VALID_OK) {
        goto done;
    }
    {
        bool is_custom = false;
        if (!pool_api_tls_mode_from_token(s, &work.target_tls_mode, &is_custom)) {
            rc = is_custom ? API_ERR_TLS_CUSTOM_REJECTED : API_ERR_TLS_MODE_UNSUPPORTED;
            goto done;
        }
    }

    /* target_chain — explicit; never inferred from the hostname. */
    rc = get_string(root, "target_chain", &s);
    if (rc != API_VALID_OK) {
        goto done;
    }
    if (!pool_api_chain_from_token(s, &work.target_chain)) {
        rc = API_ERR_CHAIN_UNSUPPORTED;
        goto done;
    }

    /* client_request_id — optional bounded diagnostics; 0 is reserved for
     * "absent" and is therefore not an acceptable explicit value. */
    if (seen[CREATE_KEY_REQUEST_ID]) {
        rc = get_uint(root, "client_request_id", 0xFFFFFFFFull, &n);
        if (rc != API_VALID_OK) {
            goto done;
        }
        if (n == 0u) {
            rc = API_ERR_REQUEST_ID_INVALID;
            goto done;
        }
        work.client_request_id = (uint32_t)n;
    }

    *out = work;
    rc   = API_VALID_OK;

done:
    cJSON_Delete(root);
    if (rc != API_VALID_OK) {
        memset(out, 0, sizeof(*out)); /* never leak a partial parse */
    }
    /* The bounded stack copy of the request body never outlives this call. */
    memset(buf, 0, sizeof(buf));
    return rc;
}

/* ------------------------------------------------------------------ */
/* Action-request parsing (Restore Now / acknowledgement)              */
/* ------------------------------------------------------------------ */

static const char *const ACTION_KEYS[] = {
    "client_request_id", /* 0 — optional */
};
#define ACTION_KEY_COUNT (sizeof(ACTION_KEYS) / sizeof(ACTION_KEYS[0]))

PoolApiValidation pool_api_parse_action(const char *body, size_t len,
                                        uint32_t *out_request_id)
{
    char              buf[POOL_API_MAX_BODY_BYTES + 1u];
    bool              seen[ACTION_KEY_COUNT];
    bool              empty = true;
    cJSON            *root = NULL;
    PoolApiValidation rc;
    uint64_t          n;

    if (out_request_id == NULL) {
        return API_ERR_BODY_MALFORMED;
    }
    *out_request_id = 0u;

    rc = intake_body(body, len, buf, &empty);
    if (rc != API_VALID_OK) {
        return rc;
    }
    if (empty) {
        return API_VALID_OK; /* the documented empty action body */
    }

    root = cJSON_Parse(buf);
    if (root == NULL) {
        return API_ERR_BODY_MALFORMED;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return API_ERR_BODY_NOT_OBJECT;
    }

    rc = walk_object(root, ACTION_KEYS, ACTION_KEY_COUNT, seen);
    if (rc != API_VALID_OK) {
        goto done;
    }
    if (seen[0]) {
        rc = get_uint(root, "client_request_id", 0xFFFFFFFFull, &n);
        if (rc != API_VALID_OK) {
            goto done;
        }
        if (n == 0u) {
            rc = API_ERR_REQUEST_ID_INVALID;
            goto done;
        }
        *out_request_id = (uint32_t)n;
    }
    rc = API_VALID_OK;

done:
    cJSON_Delete(root);
    if (rc != API_VALID_OK) {
        *out_request_id = 0u;
    }
    memset(buf, 0, sizeof(buf));
    return rc;
}
