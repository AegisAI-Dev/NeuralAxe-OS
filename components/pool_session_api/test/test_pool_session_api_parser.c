/*
 * Deterministic tests for the Gate B8 PURE request parser.
 *
 * Nothing here touches a network, a store, a lease, a pool or hardware. All
 * identities are synthetic "*.example" fixtures.
 */

#include <string.h>
#include "unity.h"
#include "pool_session_api_parser.h"

#define VALID_CREATE                                                          \
    "{\"duration_seconds\":3600,\"target_host\":\"bch.example\","             \
    "\"target_port\":3334,\"target_account\":\"acct.worker\","                \
    "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","    \
    "\"target_chain\":\"bitcoin_cash\"}"

static PoolApiValidation parse(const char *body, PoolApiCreateRequest *out)
{
    return pool_api_parse_create(body, strlen(body), out);
}

/* Every rejection must leave the output entirely zeroed. */
static void assert_out_zeroed(const PoolApiCreateRequest *r)
{
    const uint8_t *raw = (const uint8_t *)r;
    size_t         i;

    for (i = 0; i < sizeof(*r); i++) {
        TEST_ASSERT_EQUAL_UINT8(0u, raw[i]);
    }
}

static void expect_reject(const char *body, PoolApiValidation expected)
{
    PoolApiCreateRequest r;
    memset(&r, 0xAA, sizeof(r));
    TEST_ASSERT_EQUAL(expected, parse(body, &r));
    assert_out_zeroed(&r);
}

/* ================================================================= */
/* Happy paths                                                        */
/* ================================================================= */

TEST_CASE("api parser: valid minimal create", "[pool_api_parser]")
{
    PoolApiCreateRequest r;

    TEST_ASSERT_EQUAL(API_VALID_OK, parse(VALID_CREATE, &r));
    TEST_ASSERT_EQUAL_UINT32(3600u, r.duration_s);
    TEST_ASSERT_EQUAL_STRING("bch.example", r.target_host);
    TEST_ASSERT_EQUAL_UINT16(3334u, r.target_port);
    TEST_ASSERT_EQUAL_STRING("acct.worker", r.target_user);
    TEST_ASSERT_EQUAL(POOL_PROTO_STRATUM_V1, r.target_protocol);
    TEST_ASSERT_EQUAL_UINT8(POOL_API_TLS_DISABLED, r.target_tls_mode);
    TEST_ASSERT_EQUAL(POOL_CHAIN_BITCOIN_CASH, r.target_chain);
    TEST_ASSERT_EQUAL_UINT32(0u, r.client_request_id);
}

TEST_CASE("api parser: duration bounds accept the exact minimum and maximum",
          "[pool_api_parser]")
{
    PoolApiCreateRequest r;

    TEST_ASSERT_EQUAL(API_VALID_OK,
                      parse("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                            "\"target_port\":1,\"target_account\":\"u\","
                            "\"target_protocol\":\"stratum_v2\","
                            "\"target_tls_mode\":\"bundled\","
                            "\"target_chain\":\"bitcoin\"}", &r));
    TEST_ASSERT_EQUAL_UINT32(900u, r.duration_s);
    TEST_ASSERT_EQUAL(POOL_PROTO_STRATUM_V2, r.target_protocol);
    TEST_ASSERT_EQUAL_UINT8(POOL_API_TLS_BUNDLED, r.target_tls_mode);
    TEST_ASSERT_EQUAL(POOL_CHAIN_BITCOIN, r.target_chain);

    TEST_ASSERT_EQUAL(API_VALID_OK,
                      parse("{\"duration_seconds\":86400,\"target_host\":\"a.example\","
                            "\"target_port\":65535,\"target_account\":\"u\","
                            "\"target_protocol\":\"stratum_v1\","
                            "\"target_tls_mode\":\"disabled\","
                            "\"target_chain\":\"custom_unknown\"}", &r));
    TEST_ASSERT_EQUAL_UINT32(86400u, r.duration_s);
    TEST_ASSERT_EQUAL_UINT16(65535u, r.target_port);
    TEST_ASSERT_EQUAL(POOL_CHAIN_CUSTOM_UNKNOWN, r.target_chain);
}

TEST_CASE("api parser: optional client_request_id round-trips", "[pool_api_parser]")
{
    PoolApiCreateRequest r;

    TEST_ASSERT_EQUAL(API_VALID_OK,
                      parse("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                            "\"target_port\":1,\"target_account\":\"u\","
                            "\"target_protocol\":\"stratum_v1\","
                            "\"target_tls_mode\":\"disabled\","
                            "\"target_chain\":\"bitcoin\","
                            "\"client_request_id\":4294967295}", &r));
    TEST_ASSERT_EQUAL_UINT32(4294967295u, r.client_request_id);
    expect_reject("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\",\"client_request_id\":0}",
                  API_ERR_REQUEST_ID_INVALID);
}

TEST_CASE("api parser: identical input gives byte-identical output", "[pool_api_parser]")
{
    PoolApiCreateRequest a, b;

    TEST_ASSERT_EQUAL(API_VALID_OK, parse(VALID_CREATE, &a));
    TEST_ASSERT_EQUAL(API_VALID_OK, parse(VALID_CREATE, &b));
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
}

/* ================================================================= */
/* Structural rejections                                              */
/* ================================================================= */

TEST_CASE("api parser: malformed, non-object and oversized bodies fail closed",
          "[pool_api_parser]")
{
    PoolApiCreateRequest r;
    static char          big[POOL_API_MAX_BODY_BYTES + 8u];

    expect_reject("{\"duration_seconds\":", API_ERR_BODY_MALFORMED);
    expect_reject("not json at all", API_ERR_BODY_MALFORMED);
    expect_reject("[1,2,3]", API_ERR_BODY_NOT_OBJECT);
    expect_reject("42", API_ERR_BODY_NOT_OBJECT);
    expect_reject("\"a string\"", API_ERR_BODY_NOT_OBJECT);
    expect_reject("", API_ERR_MISSING_FIELD);   /* a create needs a target */
    expect_reject("   \n\t ", API_ERR_MISSING_FIELD);

    memset(big, 'x', sizeof(big));
    memset(&r, 0xAA, sizeof(r));
    TEST_ASSERT_EQUAL(API_ERR_BODY_TOO_LARGE,
                      pool_api_parse_create(big, sizeof(big), &r));
    assert_out_zeroed(&r);

    /* An embedded NUL would silently truncate the document. */
    memset(&r, 0xAA, sizeof(r));
    TEST_ASSERT_EQUAL(API_ERR_BODY_MALFORMED,
                      pool_api_parse_create("{}\0{}", 5u, &r));
    assert_out_zeroed(&r);
}

TEST_CASE("api parser: unknown, duplicate and missing fields fail closed",
          "[pool_api_parser]")
{
    expect_reject("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\",\"extra\":1}",
                  API_ERR_UNKNOWN_FIELD);
    expect_reject("{\"duration_seconds\":900,\"duration_seconds\":901,"
                  "\"target_host\":\"a.example\",\"target_port\":1,"
                  "\"target_account\":\"u\",\"target_protocol\":\"stratum_v1\","
                  "\"target_tls_mode\":\"disabled\",\"target_chain\":\"bitcoin\"}",
                  API_ERR_DUPLICATE_FIELD);
    expect_reject("{\"duration_seconds\":900}", API_ERR_MISSING_FIELD);
    expect_reject("{}", API_ERR_MISSING_FIELD);
}

TEST_CASE("api parser: nulls and wrong types are never coerced", "[pool_api_parser]")
{
    expect_reject("{\"duration_seconds\":null,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_NULL_FIELD);
    /* A numeric STRING is never accepted for a numeric field. */
    expect_reject("{\"duration_seconds\":\"3600\",\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_TYPE_MISMATCH);
    expect_reject("{\"duration_seconds\":900,\"target_host\":123,"
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_TYPE_MISMATCH);
    expect_reject("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                  "\"target_port\":true,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_TYPE_MISMATCH);
    /* A fractional duration is not an integer. */
    expect_reject("{\"duration_seconds\":900.5,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_TYPE_MISMATCH);
}

/* ================================================================= */
/* Value-domain rejections                                            */
/* ================================================================= */

TEST_CASE("api parser: duration below and above the B1 bounds is rejected",
          "[pool_api_parser]")
{
    expect_reject("{\"duration_seconds\":899,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_DURATION_OUT_OF_RANGE);
    expect_reject("{\"duration_seconds\":86401,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_DURATION_OUT_OF_RANGE);
    expect_reject("{\"duration_seconds\":0,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_DURATION_OUT_OF_RANGE);
}

TEST_CASE("api parser: bounded host, port and account rules", "[pool_api_parser]")
{
    char body[POOL_API_MAX_BODY_BYTES];
    char host[POOL_SESSION_HOST_MAX + 8];
    char acct[POOL_SESSION_USER_MAX + 8];
    PoolApiCreateRequest r;
    size_t i;

    expect_reject("{\"duration_seconds\":900,\"target_host\":\"\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_HOST_EMPTY);
    expect_reject("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                  "\"target_port\":0,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_PORT_INVALID);
    expect_reject("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                  "\"target_port\":65536,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_PORT_INVALID);
    expect_reject("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_ACCOUNT_EMPTY);

    /* Exactly at the bound is accepted; one over is rejected. */
    for (i = 0; i < POOL_SESSION_HOST_MAX - 1u; i++) host[i] = 'h';
    host[POOL_SESSION_HOST_MAX - 1u] = '\0';
    snprintf(body, sizeof(body),
             "{\"duration_seconds\":900,\"target_host\":\"%s\",\"target_port\":1,"
             "\"target_account\":\"u\",\"target_protocol\":\"stratum_v1\","
             "\"target_tls_mode\":\"disabled\",\"target_chain\":\"bitcoin\"}", host);
    TEST_ASSERT_EQUAL(API_VALID_OK, parse(body, &r));

    host[POOL_SESSION_HOST_MAX - 1u] = 'h';
    host[POOL_SESSION_HOST_MAX] = '\0';
    snprintf(body, sizeof(body),
             "{\"duration_seconds\":900,\"target_host\":\"%s\",\"target_port\":1,"
             "\"target_account\":\"u\",\"target_protocol\":\"stratum_v1\","
             "\"target_tls_mode\":\"disabled\",\"target_chain\":\"bitcoin\"}", host);
    expect_reject(body, API_ERR_HOST_TOO_LONG);

    for (i = 0; i < POOL_SESSION_USER_MAX; i++) acct[i] = 'a';
    acct[POOL_SESSION_USER_MAX] = '\0';
    snprintf(body, sizeof(body),
             "{\"duration_seconds\":900,\"target_host\":\"a.example\",\"target_port\":1,"
             "\"target_account\":\"%s\",\"target_protocol\":\"stratum_v1\","
             "\"target_tls_mode\":\"disabled\",\"target_chain\":\"bitcoin\"}", acct);
    expect_reject(body, API_ERR_ACCOUNT_TOO_LONG);
}

TEST_CASE("api parser: unsupported protocol, TLS mode and chain fail closed",
          "[pool_api_parser]")
{
    expect_reject("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v3\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_PROTOCOL_UNSUPPORTED);
    expect_reject("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"whatever\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_TLS_MODE_UNSUPPORTED);
    expect_reject("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"custom\","
                  "\"target_chain\":\"bitcoin\"}",
                  API_ERR_TLS_CUSTOM_REJECTED);
    expect_reject("{\"duration_seconds\":900,\"target_host\":\"a.example\","
                  "\"target_port\":1,\"target_account\":\"u\","
                  "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                  "\"target_chain\":\"dogecoin\"}",
                  API_ERR_CHAIN_UNSUPPORTED);
}

/* ================================================================= */
/* Deny lists — the non-negotiable safety rules                        */
/* ================================================================= */

TEST_CASE("api parser: every password-like field is rejected", "[pool_api_parser]")
{
    static const char *const KEYS[] = {
        "password", "target_password", "Pass", "PWD", "poolSecret",
        "auth_token", "apiKey", "credential", "stratum_passwd",
    };
    char   body[POOL_API_MAX_BODY_BYTES];
    size_t i;

    for (i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(pool_api_key_is_password_like(KEYS[i]), KEYS[i]);
        snprintf(body, sizeof(body),
                 "{\"duration_seconds\":900,\"target_host\":\"a.example\","
                 "\"target_port\":1,\"target_account\":\"u\","
                 "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                 "\"target_chain\":\"bitcoin\",\"%s\":\"hunter2\"}", KEYS[i]);
        expect_reject(body, API_ERR_PASSWORD_FIELD_REJECTED);
    }
    /* No legitimate key may collide with the deny list. */
    TEST_ASSERT_FALSE(pool_api_key_is_password_like("duration_seconds"));
    TEST_ASSERT_FALSE(pool_api_key_is_password_like("target_host"));
    TEST_ASSERT_FALSE(pool_api_key_is_password_like("target_account"));
    TEST_ASSERT_FALSE(pool_api_key_is_password_like("target_tls_mode"));
    TEST_ASSERT_FALSE(pool_api_key_is_password_like("client_request_id"));
}

TEST_CASE("api parser: client-supplied source, session id and internal state rejected",
          "[pool_api_parser]")
{
    char   body[POOL_API_MAX_BODY_BYTES];
    size_t i;
    static const struct { const char *key; PoolApiValidation code; } CASES[] = {
        { "source_host",       API_ERR_SOURCE_FIELD_REJECTED },
        { "sourceAccount",     API_ERR_SOURCE_FIELD_REJECTED },
        { "current_pool",      API_ERR_SOURCE_FIELD_REJECTED },
        { "session_id",        API_ERR_SESSION_ID_REJECTED },
        { "sessionId",         API_ERR_SESSION_ID_REJECTED },
        { "restore_required",  API_ERR_INTERNAL_FIELD_REJECTED },
        { "lease_token",       API_ERR_PASSWORD_FIELD_REJECTED }, /* token deny wins */
        { "lease_generation",  API_ERR_INTERNAL_FIELD_REJECTED },
        { "record_generation", API_ERR_INTERNAL_FIELD_REJECTED },
        { "nvs_key",           API_ERR_PASSWORD_FIELD_REJECTED }, /* key deny wins  */
        { "durable_state",     API_ERR_INTERNAL_FIELD_REJECTED },
        { "target_certificate",API_ERR_INTERNAL_FIELD_REJECTED },
    };

    for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        snprintf(body, sizeof(body),
                 "{\"duration_seconds\":900,\"target_host\":\"a.example\","
                 "\"target_port\":1,\"target_account\":\"u\","
                 "\"target_protocol\":\"stratum_v1\",\"target_tls_mode\":\"disabled\","
                 "\"target_chain\":\"bitcoin\",\"%s\":1}", CASES[i].key);
        expect_reject(body, CASES[i].code);
    }
}

/* ================================================================= */
/* Action bodies                                                      */
/* ================================================================= */

TEST_CASE("api parser: action bodies accept empty and {} only", "[pool_api_parser]")
{
    uint32_t id = 0xDEADBEEFu;

    TEST_ASSERT_EQUAL(API_VALID_OK, pool_api_parse_action(NULL, 0u, &id));
    TEST_ASSERT_EQUAL_UINT32(0u, id);

    id = 0xDEADBEEFu;
    TEST_ASSERT_EQUAL(API_VALID_OK, pool_api_parse_action("{}", 2u, &id));
    TEST_ASSERT_EQUAL_UINT32(0u, id);

    id = 0u;
    TEST_ASSERT_EQUAL(API_VALID_OK,
                      pool_api_parse_action("{\"client_request_id\":7}", 23u, &id));
    TEST_ASSERT_EQUAL_UINT32(7u, id);
}

TEST_CASE("api parser: action bodies fail closed on anything else", "[pool_api_parser]")
{
    uint32_t id = 0u;
    static char big[POOL_API_MAX_BODY_BYTES + 4u];

    TEST_ASSERT_EQUAL(API_ERR_UNKNOWN_FIELD,
                      pool_api_parse_action("{\"force\":true}", 14u, &id));
    TEST_ASSERT_EQUAL_UINT32(0u, id);
    TEST_ASSERT_EQUAL(API_ERR_PASSWORD_FIELD_REJECTED,
                      pool_api_parse_action("{\"password\":\"x\"}", 16u, &id));
    TEST_ASSERT_EQUAL(API_ERR_SESSION_ID_REJECTED,
                      pool_api_parse_action("{\"session_id\":1}", 16u, &id));
    TEST_ASSERT_EQUAL(API_ERR_BODY_NOT_OBJECT, pool_api_parse_action("[]", 2u, &id));
    TEST_ASSERT_EQUAL(API_ERR_BODY_MALFORMED, pool_api_parse_action("{", 1u, &id));

    memset(big, 'x', sizeof(big));
    TEST_ASSERT_EQUAL(API_ERR_BODY_TOO_LARGE,
                      pool_api_parse_action(big, sizeof(big), &id));
    TEST_ASSERT_EQUAL_UINT32(0u, id);
}

/* ================================================================= */
/* Token decoders                                                     */
/* ================================================================= */

TEST_CASE("api parser: token decoders are total and never normalize",
          "[pool_api_parser]")
{
    PoolSessionProtocol p;
    PoolChainType       c;
    uint8_t             mode;
    bool                custom;

    TEST_ASSERT_TRUE(pool_api_protocol_from_token("stratum_v1", &p));
    TEST_ASSERT_FALSE(pool_api_protocol_from_token("STRATUM_V1", &p));
    TEST_ASSERT_FALSE(pool_api_protocol_from_token(" stratum_v1", &p));
    TEST_ASSERT_FALSE(pool_api_protocol_from_token("", &p));
    TEST_ASSERT_FALSE(pool_api_protocol_from_token(NULL, &p));

    TEST_ASSERT_TRUE(pool_api_tls_mode_from_token("bundled", &mode, &custom));
    TEST_ASSERT_FALSE(custom);
    TEST_ASSERT_FALSE(pool_api_tls_mode_from_token("custom", &mode, &custom));
    TEST_ASSERT_TRUE(custom);
    TEST_ASSERT_FALSE(pool_api_tls_mode_from_token("nonsense", &mode, &custom));
    TEST_ASSERT_FALSE(custom);

    TEST_ASSERT_TRUE(pool_api_chain_from_token("bitcoin_cash", &c));
    TEST_ASSERT_FALSE(pool_api_chain_from_token("btc", &c));

    TEST_ASSERT_EQUAL_STRING("stratum_v2", pool_api_protocol_token(POOL_PROTO_STRATUM_V2));
    TEST_ASSERT_EQUAL_STRING("unknown", pool_api_protocol_token((PoolSessionProtocol)99));
    TEST_ASSERT_EQUAL_STRING("bundled", pool_api_tls_mode_token(POOL_API_TLS_BUNDLED));
    TEST_ASSERT_EQUAL_STRING("unknown", pool_api_tls_mode_token(42u));
    TEST_ASSERT_EQUAL_STRING("custom_unknown",
                             pool_api_chain_token(POOL_CHAIN_CUSTOM_UNKNOWN));
    TEST_ASSERT_EQUAL_STRING("unknown", pool_api_chain_token((PoolChainType)77));
}

TEST_CASE("api parser: validation tokens are stable and dot-free", "[pool_api_parser]")
{
    unsigned v;

    for (v = 0; v < (unsigned)POOL_API_VALIDATION__COUNT; v++) {
        const char *s = pool_api_validation_str((PoolApiValidation)v);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_TRUE(strlen(s) > 0u);
        TEST_ASSERT_NULL(strchr(s, '.'));
        TEST_ASSERT_NULL(strchr(s, ' '));
    }
    TEST_ASSERT_EQUAL_STRING("UNKNOWN",
                             pool_api_validation_str((PoolApiValidation)999));
}
