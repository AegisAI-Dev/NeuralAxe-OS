/*
 * Exhaustive deterministic tests for the trusted-time SOURCE policy
 * (Phase 2M.1B, Gate B10).
 *
 * No hardware, no networking, no DNS, no real NTP, no pools, no secrets.
 * Every candidate below is a synthetic "*.example" / RFC-5737 style fixture
 * or a deliberately malformed string; nothing here can resolve or connect,
 * and the pure code under test performs no IO of any kind.
 */

#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "pool_time.h"
#include "pool_time_sntp.h"
#include "pool_time_source.h"

/* ================================================================= */
/* Helpers                                                            */
/* ================================================================= */

static PoolTimeSourceValidation validate(const char *host)
{
    PoolTimeSourceValidation v;
    memset(&v, 0xA5, sizeof(v)); /* prove every path fully writes *out */
    (void)pool_time_source_validate(host, &v);
    return v;
}

static void assert_rejected(const char *host)
{
    PoolTimeSourceValidation v = validate(host);
    TEST_ASSERT_EQUAL(TIME_SOURCE_INVALID, v.state);
    TEST_ASSERT_FALSE(v.usable);
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG, v.reason);
    /* Output is zeroed on failure: no partial shape survives. */
    TEST_ASSERT_EQUAL_UINT32(0u, v.length);
    TEST_ASSERT_EQUAL_UINT32(0u, v.label_count);
    TEST_ASSERT_FALSE(v.literal_ipv4);
}

static void assert_accepted(const char *host, uint32_t labels, bool ipv4)
{
    PoolTimeSourceValidation v = validate(host);
    TEST_ASSERT_EQUAL(TIME_SOURCE_CONFIGURED, v.state);
    TEST_ASSERT_TRUE(v.usable);
    TEST_ASSERT_EQUAL(TIME_OK, v.reason);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen(host), v.length);
    TEST_ASSERT_EQUAL_UINT32(labels, v.label_count);
    TEST_ASSERT_EQUAL(ipv4, v.literal_ipv4);
    /* An accepted source always fits the committed B2 config buffer. */
    TEST_ASSERT_TRUE(v.length < (uint32_t)POOL_TIME_SNTP_SERVER_HOST_MAX);
}

/* Build a hostname of exactly `total` characters ending in ".example". */
static void build_host(char *buf, size_t buflen, uint32_t total)
{
    const char *suffix = ".example";
    size_t      sl     = strlen(suffix);
    size_t      i;

    TEST_ASSERT_TRUE(total > sl && (size_t)total + 1u <= buflen);
    for (i = 0; i < (size_t)total - sl; i++) {
        buf[i] = 'a';
    }
    memcpy(buf + ((size_t)total - sl), suffix, sl);
    buf[total] = '\0';
}

/* ================================================================= */
/* A. Source validator                                                */
/* ================================================================= */

TEST_CASE("b10 src: NULL and empty are UNCONFIGURED, never an error",
          "[pool_time_source]")
{
    PoolTimeSourceValidation v = validate(NULL);
    TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, v.state);
    TEST_ASSERT_FALSE(v.usable);
    TEST_ASSERT_EQUAL(TIME_OK, v.reason); /* the shipped default is not a fault */
    TEST_ASSERT_EQUAL_UINT32(0u, v.length);

    v = validate("");
    TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, v.state);
    TEST_ASSERT_FALSE(v.usable);
    TEST_ASSERT_EQUAL(TIME_OK, v.reason);
    TEST_ASSERT_EQUAL_UINT32(0u, v.length);

    /* A NULL out pointer never crashes and never reports usable. */
    TEST_ASSERT_EQUAL(TIME_SOURCE_INVALID, pool_time_source_validate("a.example", NULL));
}

TEST_CASE("b10 src: ordinary bounded hostnames are accepted", "[pool_time_source]")
{
    assert_accepted("time.example", 2u, false);
    assert_accepted("ntp-a.time.example", 3u, false);
    assert_accepted("a.b", 2u, false);
    assert_accepted("NTP.Example", 2u, false);       /* case is preserved shape-wise */
    assert_accepted("ntp1.pool.time.example", 4u, false);
    assert_accepted("0ntp.example", 2u, false);      /* digits inside a label are fine */
}

TEST_CASE("b10 src: the maximum length is accepted and one more is rejected",
          "[pool_time_source]")
{
    char buf[POOL_TIME_SOURCE_HOST_MAX + 8u];

    build_host(buf, sizeof(buf), POOL_TIME_SOURCE_HOST_MAX);
    assert_accepted(buf, 2u, false);
    TEST_ASSERT_EQUAL_UINT32(POOL_TIME_SOURCE_HOST_MAX, (uint32_t)strlen(buf));

    build_host(buf, sizeof(buf), POOL_TIME_SOURCE_HOST_MAX + 1u);
    assert_rejected(buf);
}

TEST_CASE("b10 src: an unterminated candidate is rejected, not over-read",
          "[pool_time_source]")
{
    /* No NUL inside the bounded window: the validator must stop and reject. */
    char raw[POOL_TIME_SOURCE_HOST_MAX + 1u];
    memset(raw, 'a', sizeof(raw));
    raw[10] = '.';
    assert_rejected(raw);
}

TEST_CASE("b10 src: whitespace in any position is rejected", "[pool_time_source]")
{
    assert_rejected(" time.example");
    assert_rejected("time.example ");
    assert_rejected("time .example");
    assert_rejected("time. example");
    assert_rejected("time\texample.org");
    assert_rejected("time\nexample.org");
    assert_rejected("  ");
}

TEST_CASE("b10 src: control characters and non-ASCII are rejected",
          "[pool_time_source]")
{
    char probe[16];
    unsigned c;

    for (c = 1u; c <= 0x20u; c++) {
        snprintf(probe, sizeof(probe), "a%cb.example", (char)c);
        assert_rejected(probe);
    }
    snprintf(probe, sizeof(probe), "a%cb.example", (char)0x7F); /* DEL */
    assert_rejected(probe);
    snprintf(probe, sizeof(probe), "a%cb.example", (char)0x80); /* non-ASCII */
    assert_rejected(probe);
    snprintf(probe, sizeof(probe), "a%cb.example", (char)0xFF);
    assert_rejected(probe);
}

TEST_CASE("b10 src: URL schemes, paths, queries and fragments are rejected",
          "[pool_time_source]")
{
    assert_rejected("http://time.example");
    assert_rejected("https://time.example");
    assert_rejected("ntp://time.example");
    assert_rejected("sntp:time.example");
    assert_rejected("time.example/path");
    assert_rejected("/time.example");
    assert_rejected("time.example?q=1");
    assert_rejected("time.example#frag");
    assert_rejected("time.example:123");
    assert_rejected("time.example%2e");
}

TEST_CASE("b10 src: credentials and userinfo are rejected", "[pool_time_source]")
{
    assert_rejected("user@time.example");
    assert_rejected("user:pass@time.example");
    assert_rejected("@time.example");
    assert_rejected("time.example@");
}

TEST_CASE("b10 src: malformed labels and dot placement are rejected",
          "[pool_time_source]")
{
    assert_rejected(".time.example");   /* leading dot   */
    assert_rejected("time.example.");   /* trailing dot  */
    assert_rejected("time..example");   /* empty label   */
    assert_rejected(".");
    assert_rejected("..");
    assert_rejected("-time.example");   /* leading hyphen  */
    assert_rejected("time-.example");   /* trailing hyphen */
    assert_rejected("time.-example");
    assert_rejected("time.example-");
    assert_rejected("localhost");       /* single label: unaudited search domains */
    assert_rejected("ntp");
}

TEST_CASE("b10 src: an overlong label is rejected even inside the length bound",
          "[pool_time_source]")
{
    char buf[POOL_TIME_SOURCE_HOST_MAX + 8u];
    uint32_t i;

    /* 64-character first label (bound is 63) plus a short second label. */
    for (i = 0; i < POOL_TIME_SOURCE_LABEL_MAX + 1u; i++) {
        buf[i] = 'a';
    }
    buf[POOL_TIME_SOURCE_LABEL_MAX + 1u] = '.';
    buf[POOL_TIME_SOURCE_LABEL_MAX + 2u] = 'b';
    buf[POOL_TIME_SOURCE_LABEL_MAX + 3u] = '\0';
    assert_rejected(buf);

    /* Exactly 63 is a legal label — but the whole name still has to fit. */
    for (i = 0; i < POOL_TIME_SOURCE_LABEL_MAX; i++) {
        buf[i] = 'a';
    }
    buf[POOL_TIME_SOURCE_LABEL_MAX]      = '.';
    buf[POOL_TIME_SOURCE_LABEL_MAX + 1u] = 'b';
    buf[POOL_TIME_SOURCE_LABEL_MAX + 2u] = '\0';
    assert_rejected(buf); /* 65 characters total: over the hostname bound */
}

TEST_CASE("b10 src: bounded IPv4 literals are accepted", "[pool_time_source]")
{
    assert_accepted("192.0.2.1", 4u, true);      /* RFC 5737 documentation net */
    assert_accepted("198.51.100.42", 4u, true);
    assert_accepted("203.0.113.255", 4u, true);
    assert_accepted("0.0.0.0", 4u, true);
    assert_accepted("255.255.255.255", 4u, true);
}

TEST_CASE("b10 src: ambiguous or malformed numeric literals are rejected",
          "[pool_time_source]")
{
    assert_rejected("010.0.2.1");        /* leading zero: lwIP would read octal */
    assert_rejected("192.00.2.1");
    assert_rejected("192.0.2.256");      /* octet out of range */
    assert_rejected("192.0.2.1234");     /* too many digits    */
    assert_rejected("192.0.2");          /* not four octets    */
    assert_rejected("192.0.2.1.5");      /* five octets        */
    assert_rejected("1.2");
    assert_rejected("0x7f.0.0.1");       /* hex form: '.' aside, 'x' breaks it */
    assert_rejected("time.example.1");   /* numeric last label is not a host   */
}

TEST_CASE("b10 src: IPv6 literals are rejected in every form", "[pool_time_source]")
{
    /* ':' is outside the accepted character set; the bracketed URI form adds
     * '[' and ']'. IPv6 SNTP behaviour is not audited by this gate. */
    assert_rejected("2001:db8::1");
    assert_rejected("::1");
    assert_rejected("[2001:db8::1]");
    assert_rejected("fe80::1%25eth0");
}

TEST_CASE("b10 src: validation is pure — identical input, identical output",
          "[pool_time_source]")
{
    PoolTimeSourceValidation a = validate("ntp-a.time.example");
    PoolTimeSourceValidation b = validate("ntp-a.time.example");
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));

    a = validate("bad host");
    b = validate("bad host");
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
}

TEST_CASE("b10 src: the verdict carries no copy of the candidate",
          "[pool_time_source]")
{
    const char *host = "secret-ntp.example";
    PoolTimeSourceValidation v = validate(host);
    const uint8_t *h = (const uint8_t *)&v;
    size_t nl = strlen(host);
    size_t i;

    TEST_ASSERT_TRUE(v.usable);
    for (i = 0; i + nl <= sizeof(v); i++) {
        TEST_ASSERT_FALSE(memcmp(h + i, host, nl) == 0);
    }
    /* Not even the first label leaks. */
    for (i = 0; i + 6u <= sizeof(v); i++) {
        TEST_ASSERT_FALSE(memcmp(h + i, "secret", 6) == 0);
    }
}

/* ================================================================= */
/* B/C/E. Start decision — feature flags, lifecycle, observation       */
/* ================================================================= */

static PoolTimeSourceStartInput start_in_defaults(void)
{
    PoolTimeSourceStartInput in;
    memset(&in, 0, sizeof(in));
    in.runtime_enabled = true;
    in.source_present  = true;
    in.source_usable   = true;
    in.network_ready   = true;
    return in;
}

TEST_CASE("b10 start: nothing starts without the runtime feature",
          "[pool_time_source]")
{
    PoolTimeSourceStartInput in = start_in_defaults();
    PoolTimeSourceStartDecision d;

    in.runtime_enabled = false;
    in.observe_enabled = true;          /* observe alone can never enable it */
    in.trusted_time_required = true;
    d = pool_time_source_decide_start(&in);
    TEST_ASSERT_FALSE(d.start_provider);
    TEST_ASSERT_FALSE(d.observation_only);
    TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, d.state);
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_INITIALIZED, d.reason);

    /* A NULL input is fail-closed, never a start. */
    d = pool_time_source_decide_start(NULL);
    TEST_ASSERT_FALSE(d.start_provider);
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT, d.reason);
}

TEST_CASE("b10 start: runtime on with observe off starts only for recovery",
          "[pool_time_source]")
{
    PoolTimeSourceStartInput in = start_in_defaults();
    PoolTimeSourceStartDecision d;

    /* Nothing requires time: validated, idle, no networking. */
    d = pool_time_source_decide_start(&in);
    TEST_ASSERT_FALSE(d.start_provider);
    TEST_ASSERT_EQUAL(TIME_SOURCE_CONFIGURED, d.state);

    /* The B4 plan requires trusted time: start, and NOT as observation. */
    in.trusted_time_required = true;
    d = pool_time_source_decide_start(&in);
    TEST_ASSERT_TRUE(d.start_provider);
    TEST_ASSERT_FALSE(d.observation_only);
    TEST_ASSERT_EQUAL(TIME_OK, d.reason);
}

TEST_CASE("b10 start: observation starts only with no session owner",
          "[pool_time_source]")
{
    PoolTimeSourceStartInput in = start_in_defaults();
    PoolTimeSourceStartDecision d;

    in.observe_enabled = true;
    d = pool_time_source_decide_start(&in);
    TEST_ASSERT_TRUE(d.start_provider);
    TEST_ASSERT_TRUE(d.observation_only);

    /* A session owner disqualifies observation entirely. */
    in.session_owner_present = true;
    d = pool_time_source_decide_start(&in);
    TEST_ASSERT_FALSE(d.start_provider);
    TEST_ASSERT_EQUAL(TIME_SOURCE_CONFIGURED, d.state);
}

TEST_CASE("b10 start: an unconfigured or invalid source never starts anything",
          "[pool_time_source]")
{
    PoolTimeSourceStartInput in = start_in_defaults();
    PoolTimeSourceStartDecision d;
    int required, observe;

    /* Exhaustive over both demand paths: no combination reaches the network. */
    for (required = 0; required <= 1; required++) {
        for (observe = 0; observe <= 1; observe++) {
            in = start_in_defaults();
            in.trusted_time_required = (required != 0);
            in.observe_enabled       = (observe != 0);

            in.source_present = false;
            in.source_usable  = false;
            d = pool_time_source_decide_start(&in);
            TEST_ASSERT_FALSE(d.start_provider);
            TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, d.state);

            in.source_present = true;
            in.source_usable  = false;
            d = pool_time_source_decide_start(&in);
            TEST_ASSERT_FALSE(d.start_provider);
            TEST_ASSERT_EQUAL(TIME_SOURCE_INVALID, d.state);
            TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG, d.reason);
        }
    }
}

TEST_CASE("b10 start: no provider starts before network readiness",
          "[pool_time_source]")
{
    PoolTimeSourceStartInput in = start_in_defaults();
    PoolTimeSourceStartDecision d;

    in.network_ready         = false;
    in.trusted_time_required = true;
    d = pool_time_source_decide_start(&in);
    TEST_ASSERT_FALSE(d.start_provider);
    TEST_ASSERT_EQUAL(TIME_SOURCE_START_PENDING, d.state);
    TEST_ASSERT_EQUAL(TIME_ERR_SYNC_PENDING, d.reason);

    in.trusted_time_required = false;
    in.observe_enabled       = true;
    d = pool_time_source_decide_start(&in);
    TEST_ASSERT_FALSE(d.start_provider);
    TEST_ASSERT_EQUAL(TIME_SOURCE_START_PENDING, d.state);
}

TEST_CASE("b10 start: duplicate network-ready never creates a second client",
          "[pool_time_source]")
{
    PoolTimeSourceStartInput in = start_in_defaults();
    PoolTimeSourceStartDecision d;

    in.trusted_time_required = true;
    d = pool_time_source_decide_start(&in);
    TEST_ASSERT_TRUE(d.start_provider);

    /* Once running, the SAME facts must never ask for another start. */
    in.provider_started = true;
    d = pool_time_source_decide_start(&in);
    TEST_ASSERT_FALSE(d.start_provider);
    TEST_ASSERT_EQUAL(TIME_SOURCE_SYNCING, d.state);
    d = pool_time_source_decide_start(&in);
    TEST_ASSERT_FALSE(d.start_provider);
}

TEST_CASE("b10 start: the decision is total and pure over every input",
          "[pool_time_source]")
{
    uint32_t mask;

    for (mask = 0; mask < 256u; mask++) {
        PoolTimeSourceStartInput in;
        PoolTimeSourceStartDecision a, b;

        memset(&in, 0, sizeof(in));
        in.runtime_enabled       = (mask & 1u) != 0u;
        in.observe_enabled       = (mask & 2u) != 0u;
        in.network_ready         = (mask & 4u) != 0u;
        in.trusted_time_required = (mask & 8u) != 0u;
        in.source_present        = (mask & 16u) != 0u;
        in.source_usable         = (mask & 32u) != 0u;
        in.provider_started      = (mask & 64u) != 0u;
        in.session_owner_present = (mask & 128u) != 0u;

        a = pool_time_source_decide_start(&in);
        b = pool_time_source_decide_start(&in);
        TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a)); /* byte-identical output */

        TEST_ASSERT_TRUE((unsigned)a.state < (unsigned)POOL_TIME_SOURCE_STATE__COUNT);

        if (a.start_provider) {
            /* Every start is fully justified. */
            TEST_ASSERT_TRUE(in.runtime_enabled);
            TEST_ASSERT_TRUE(in.source_present);
            TEST_ASSERT_TRUE(in.source_usable);
            TEST_ASSERT_TRUE(in.network_ready);
            TEST_ASSERT_FALSE(in.provider_started);
            TEST_ASSERT_TRUE(in.trusted_time_required || in.observe_enabled);
            if (!in.trusted_time_required) {
                TEST_ASSERT_TRUE(in.observe_enabled);
                TEST_ASSERT_FALSE(in.session_owner_present);
                TEST_ASSERT_TRUE(a.observation_only);
            }
        }
        /* Observation is never claimed for a plan-driven start. */
        if (a.observation_only) {
            TEST_ASSERT_FALSE(in.trusted_time_required);
        }
    }
}

/* ================================================================= */
/* Bounded retry policy                                               */
/* ================================================================= */

TEST_CASE("b10 retry: defaults are bounded and structurally valid",
          "[pool_time_source]")
{
    PoolTimeSourceRetryPolicy p;
    pool_time_source_retry_defaults(&p);
    TEST_ASSERT_TRUE(pool_time_source_retry_policy_valid(&p));
    TEST_ASSERT_EQUAL_UINT32(POOL_TIME_SOURCE_ATTEMPTS_MAX, p.max_attempts);
    TEST_ASSERT_TRUE(p.base_backoff_s <= p.max_backoff_s);

    TEST_ASSERT_FALSE(pool_time_source_retry_policy_valid(NULL));
    p.max_attempts = 0u;
    TEST_ASSERT_FALSE(pool_time_source_retry_policy_valid(&p));
    pool_time_source_retry_defaults(&p);
    p.max_attempts = POOL_TIME_SOURCE_ATTEMPTS_MAX + 1u;
    TEST_ASSERT_FALSE(pool_time_source_retry_policy_valid(&p));
    pool_time_source_retry_defaults(&p);
    p.base_backoff_s = p.max_backoff_s + 1u;
    TEST_ASSERT_FALSE(pool_time_source_retry_policy_valid(&p));
}

TEST_CASE("b10 retry: the first attempt is immediate and later ones back off",
          "[pool_time_source]")
{
    PoolTimeSourceRetryPolicy p;
    PoolTimeSourceRetryDecision d;
    uint64_t t0 = 5ull * POOL_TIME_US_PER_S;

    pool_time_source_retry_defaults(&p);

    d = pool_time_source_retry_decide(&p, 0u, false, 0u, t0);
    TEST_ASSERT_TRUE(d.may_attempt);
    TEST_ASSERT_FALSE(d.exhausted);
    TEST_ASSERT_EQUAL_UINT32(1u, d.attempt_index);
    TEST_ASSERT_EQUAL_UINT32(0u, d.next_delay_s);

    /* Immediately after attempt 1 the backoff blocks a retry. */
    d = pool_time_source_retry_decide(&p, 1u, true, t0, t0);
    TEST_ASSERT_FALSE(d.may_attempt);
    TEST_ASSERT_FALSE(d.exhausted);
    TEST_ASSERT_EQUAL_UINT32(POOL_TIME_SOURCE_BACKOFF_BASE_S, d.next_delay_s);

    /* One second before the window closes it still blocks. */
    d = pool_time_source_retry_decide(&p, 1u, true, t0,
                                      t0 + (uint64_t)(POOL_TIME_SOURCE_BACKOFF_BASE_S - 1u) *
                                               POOL_TIME_US_PER_S);
    TEST_ASSERT_FALSE(d.may_attempt);
    TEST_ASSERT_EQUAL_UINT32(1u, d.next_delay_s);

    /* Exactly at the boundary it opens. */
    d = pool_time_source_retry_decide(&p, 1u, true, t0,
                                      t0 + (uint64_t)POOL_TIME_SOURCE_BACKOFF_BASE_S *
                                               POOL_TIME_US_PER_S);
    TEST_ASSERT_TRUE(d.may_attempt);
    TEST_ASSERT_EQUAL_UINT32(2u, d.attempt_index);
}

TEST_CASE("b10 retry: the backoff grows, caps and never exceeds the sync window",
          "[pool_time_source]")
{
    PoolTimeSourceRetryPolicy p;
    uint32_t n;
    uint32_t previous = 0u;

    pool_time_source_retry_defaults(&p);
    for (n = 1u; n < p.max_attempts; n++) {
        PoolTimeSourceRetryDecision d =
            pool_time_source_retry_decide(&p, n, true, 0u, 0u);
        TEST_ASSERT_FALSE(d.may_attempt);
        TEST_ASSERT_TRUE(d.next_delay_s >= previous);          /* monotonic  */
        TEST_ASSERT_TRUE(d.next_delay_s <= p.max_backoff_s);   /* capped     */
        TEST_ASSERT_TRUE(d.next_delay_s <= POOL_TIME_SYNC_WAIT_MAX_S);
        previous = d.next_delay_s;
    }
}

TEST_CASE("b10 retry: the attempt budget is hard bounded", "[pool_time_source]")
{
    PoolTimeSourceRetryPolicy p;
    PoolTimeSourceRetryDecision d;
    uint64_t far_future = 100000ull * POOL_TIME_US_PER_S;

    pool_time_source_retry_defaults(&p);

    /* Even with unlimited elapsed time the budget stops the retries. */
    d = pool_time_source_retry_decide(&p, p.max_attempts, true, 0u, far_future);
    TEST_ASSERT_TRUE(d.exhausted);
    TEST_ASSERT_FALSE(d.may_attempt);
    d = pool_time_source_retry_decide(&p, p.max_attempts + 7u, true, 0u, far_future);
    TEST_ASSERT_TRUE(d.exhausted);
    TEST_ASSERT_FALSE(d.may_attempt);

    /* An invalid policy fails closed rather than retrying forever. */
    d = pool_time_source_retry_decide(NULL, 0u, false, 0u, 0u);
    TEST_ASSERT_TRUE(d.exhausted);
    TEST_ASSERT_FALSE(d.may_attempt);
}

TEST_CASE("b10 retry: a monotonic regression waits instead of retrying early",
          "[pool_time_source]")
{
    PoolTimeSourceRetryPolicy p;
    PoolTimeSourceRetryDecision d;
    uint64_t late = 900ull * POOL_TIME_US_PER_S;

    pool_time_source_retry_defaults(&p);
    /* "now" earlier than the last attempt: fail safe to the full backoff. */
    d = pool_time_source_retry_decide(&p, 1u, true, late, late - POOL_TIME_US_PER_S);
    TEST_ASSERT_FALSE(d.may_attempt);
    TEST_ASSERT_FALSE(d.exhausted);
    TEST_ASSERT_EQUAL_UINT32(POOL_TIME_SOURCE_BACKOFF_BASE_S, d.next_delay_s);
}

/* ================================================================= */
/* H. Diagnostics and privacy                                         */
/* ================================================================= */

static PoolTimeSourceDiagnosticsInput diag_in_defaults(void)
{
    PoolTimeSourceDiagnosticsInput in;
    memset(&in, 0, sizeof(in));
    in.runtime_enabled  = true;
    in.source_present   = true;
    in.source_usable    = true;
    in.last_sync_result = TIME_ERR_NOT_INITIALIZED;
    in.wait_limit_s     = POOL_TIME_SYNC_WAIT_DEFAULT_S;
    return in;
}

static PoolTimeSourceDiagnostics diag_of(const PoolTimeSourceDiagnosticsInput *in)
{
    PoolTimeSourceDiagnostics d;
    memset(&d, 0xC3, sizeof(d));
    pool_time_source_diagnostics_build(in, &d);
    return d;
}

TEST_CASE("b10 diag: the initial view is fail-closed and structurally valid",
          "[pool_time_source]")
{
    PoolTimeSourceDiagnostics d;
    memset(&d, 0x5A, sizeof(d));
    pool_time_source_diagnostics_init(&d);

    TEST_ASSERT_EQUAL_UINT32(POOL_TIME_SOURCE_MODEL_VERSION, d.model_version);
    TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, d.state);
    TEST_ASSERT_FALSE(d.source_configured);
    TEST_ASSERT_FALSE(d.trusted_time_available);
    TEST_ASSERT_FALSE(d.trusted_time_operational);
    TEST_ASSERT_FALSE(d.observation_mode_enabled);
    TEST_ASSERT_TRUE(pool_time_source_diagnostics_valid(&d));

    /* A NULL input yields exactly the fail-closed view, never garbage. */
    pool_time_source_diagnostics_build(NULL, &d);
    TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, d.state);
    TEST_ASSERT_TRUE(pool_time_source_diagnostics_valid(&d));

    TEST_ASSERT_FALSE(pool_time_source_diagnostics_valid(NULL));
    pool_time_source_diagnostics_init(NULL); /* must not crash */
}

TEST_CASE("b10 diag: the state resolves by the documented priority",
          "[pool_time_source]")
{
    PoolTimeSourceDiagnosticsInput in;
    PoolTimeSourceDiagnostics      d;

    /* No runtime / no source -> UNCONFIGURED. */
    in = diag_in_defaults();
    in.runtime_enabled = false;
    TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, diag_of(&in).state);
    in = diag_in_defaults();
    in.source_present = false;
    TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, diag_of(&in).state);

    /* Present but unusable -> INVALID. */
    in = diag_in_defaults();
    in.source_usable = false;
    TEST_ASSERT_EQUAL(TIME_SOURCE_INVALID, diag_of(&in).state);

    /* Validated but idle -> CONFIGURED. */
    in = diag_in_defaults();
    TEST_ASSERT_EQUAL(TIME_SOURCE_CONFIGURED, diag_of(&in).state);

    /* Initialized -> START_PENDING; started -> SYNCING. */
    in = diag_in_defaults();
    in.provider_initialized = true;
    in.lifecycle            = (uint8_t)POOL_TIME_SNTP_INITIALIZED;
    TEST_ASSERT_EQUAL(TIME_SOURCE_START_PENDING, diag_of(&in).state);
    in.provider_started = true;
    in.lifecycle        = (uint8_t)POOL_TIME_SNTP_SYNC_PENDING;
    TEST_ASSERT_EQUAL(TIME_SOURCE_SYNCING, diag_of(&in).state);

    /* An accepted anchor -> TRUSTED and operational. */
    in.snapshot_trusted = true;
    in.lifecycle        = (uint8_t)POOL_TIME_SNTP_TRUSTED;
    d = diag_of(&in);
    TEST_ASSERT_EQUAL(TIME_SOURCE_TRUSTED, d.state);
    TEST_ASSERT_TRUE(d.trusted_time_available);
    TEST_ASSERT_TRUE(d.trusted_time_operational);
    TEST_ASSERT_TRUE(pool_time_source_diagnostics_valid(&d));

    /* Stopped service with no anchor -> STOPPED. */
    in = diag_in_defaults();
    in.provider_initialized = true;
    in.lifecycle            = (uint8_t)POOL_TIME_SNTP_STOPPED;
    TEST_ASSERT_EQUAL(TIME_SOURCE_STOPPED, diag_of(&in).state);

    /* A refused candidate -> REJECTED (and never trusted). */
    in = diag_in_defaults();
    in.provider_started  = true;
    in.lifecycle         = (uint8_t)POOL_TIME_SNTP_SYNC_PENDING;
    in.last_sync_result  = TIME_ERR_EPOCH_BELOW_MIN;
    d = diag_of(&in);
    TEST_ASSERT_EQUAL(TIME_SOURCE_REJECTED, d.state);
    TEST_ASSERT_FALSE(d.trusted_time_available);
    TEST_ASSERT_FALSE(d.trusted_time_operational);

    /* An expired wait or a spent budget -> TIMEOUT. */
    in = diag_in_defaults();
    in.provider_started = true;
    in.wait_expired     = true;
    TEST_ASSERT_EQUAL(TIME_SOURCE_TIMEOUT, diag_of(&in).state);
    in = diag_in_defaults();
    in.provider_started    = true;
    in.attempts_exhausted  = true;
    TEST_ASSERT_EQUAL(TIME_SOURCE_TIMEOUT, diag_of(&in).state);

    /* A platform failure -> ERROR, and trust from an existing anchor is
     * reported as available but NOT operational. */
    in = diag_in_defaults();
    in.provider_started = true;
    in.lifecycle        = (uint8_t)POOL_TIME_SNTP_ERROR;
    in.snapshot_trusted = true;
    d = diag_of(&in);
    TEST_ASSERT_EQUAL(TIME_SOURCE_ERROR, d.state);
    TEST_ASSERT_TRUE(d.trusted_time_available);
    TEST_ASSERT_FALSE(d.trusted_time_operational);
    TEST_ASSERT_TRUE(pool_time_source_diagnostics_valid(&d));
}

TEST_CASE("b10 diag: trust is impossible without a configured source",
          "[pool_time_source]")
{
    PoolTimeSourceDiagnosticsInput in = diag_in_defaults();
    PoolTimeSourceDiagnostics      d;

    /* Even a "trusted" snapshot cannot make an unconfigured source trusted. */
    in.source_present   = false;
    in.snapshot_trusted = true;
    in.provider_started = true;
    d = diag_of(&in);
    TEST_ASSERT_EQUAL(TIME_SOURCE_UNCONFIGURED, d.state);
    TEST_ASSERT_FALSE(d.trusted_time_available);
    TEST_ASSERT_FALSE(d.trusted_time_operational);
    TEST_ASSERT_TRUE(pool_time_source_diagnostics_valid(&d));

    in = diag_in_defaults();
    in.source_usable    = false;
    in.snapshot_trusted = true;
    d = diag_of(&in);
    TEST_ASSERT_EQUAL(TIME_SOURCE_INVALID, d.state);
    TEST_ASSERT_FALSE(d.trusted_time_available);
}

TEST_CASE("b10 diag: the sync age is a saturating monotonic duration",
          "[pool_time_source]")
{
    PoolTimeSourceDiagnosticsInput in = diag_in_defaults();
    PoolTimeSourceDiagnostics      d;

    in.provider_started = true;
    in.snapshot_trusted = true;
    in.lifecycle        = (uint8_t)POOL_TIME_SNTP_TRUSTED;

    /* Untrusted: no age is published at all. */
    in.anchor_age_valid = false;
    in.anchor_age_us    = 42ull * POOL_TIME_US_PER_S;
    d = diag_of(&in);
    TEST_ASSERT_FALSE(d.sync_age_valid);
    TEST_ASSERT_EQUAL_UINT32(0u, d.sync_age_s);

    in.anchor_age_valid = true;
    d = diag_of(&in);
    TEST_ASSERT_TRUE(d.sync_age_valid);
    TEST_ASSERT_EQUAL_UINT32(42u, d.sync_age_s);

    /* Sub-second ages truncate to 0 but stay valid. */
    in.anchor_age_us = POOL_TIME_US_PER_S - 1ull;
    d = diag_of(&in);
    TEST_ASSERT_TRUE(d.sync_age_valid);
    TEST_ASSERT_EQUAL_UINT32(0u, d.sync_age_s);

    /* An absurd age saturates instead of wrapping. */
    in.anchor_age_us = UINT64_MAX;
    d = diag_of(&in);
    TEST_ASSERT_TRUE(d.sync_age_valid);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, d.sync_age_s);
}

TEST_CASE("b10 diag: an out-of-range enum falls back safely",
          "[pool_time_source]")
{
    PoolTimeSourceDiagnosticsInput in = diag_in_defaults();
    PoolTimeSourceDiagnostics      d;

    in.last_sync_result = (PoolTimeError)(POOL_TIME_ERR__COUNT + 9);
    in.lifecycle        = (uint8_t)0xEE;
    d = diag_of(&in);
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_INITIALIZED, d.last_sync_result);
    TEST_ASSERT_TRUE(pool_time_source_diagnostics_valid(&d));
    TEST_ASSERT_TRUE((unsigned)d.state < (unsigned)POOL_TIME_SOURCE_STATE__COUNT);

    /* Structural validation rejects a hand-corrupted view. */
    d.state = (PoolTimeSourceState)POOL_TIME_SOURCE_STATE__COUNT;
    TEST_ASSERT_FALSE(pool_time_source_diagnostics_valid(&d));
    pool_time_source_diagnostics_build(&in, &d);
    d.trusted_time_operational = true;
    d.trusted_time_available   = false;
    TEST_ASSERT_FALSE(pool_time_source_diagnostics_valid(&d));
    pool_time_source_diagnostics_build(&in, &d);
    d.model_version = POOL_TIME_SOURCE_MODEL_VERSION + 1u;
    TEST_ASSERT_FALSE(pool_time_source_diagnostics_valid(&d));
}

TEST_CASE("b10 diag: the published view carries no hostname, address or epoch",
          "[pool_time_source]")
{
    const char *host = "ntp-secret.example";
    const char *addr = "198.51.100.42";
    PoolTimeSourceDiagnosticsInput in = diag_in_defaults();
    PoolTimeSourceDiagnostics      d;
    const uint8_t *h;
    uint64_t epoch_us = 1750000000ull * POOL_TIME_US_PER_S;
    size_t i;

    in.provider_started = true;
    in.snapshot_trusted = true;
    in.anchor_age_valid = true;
    in.anchor_age_us    = epoch_us; /* even a huge age never stores an epoch */
    in.lifecycle        = (uint8_t)POOL_TIME_SNTP_TRUSTED;
    d = diag_of(&in);
    h = (const uint8_t *)&d;

    for (i = 0; i + strlen(host) <= sizeof(d); i++) {
        TEST_ASSERT_FALSE(memcmp(h + i, host, strlen(host)) == 0);
    }
    for (i = 0; i + strlen(addr) <= sizeof(d); i++) {
        TEST_ASSERT_FALSE(memcmp(h + i, addr, strlen(addr)) == 0);
    }
    for (i = 0; i + sizeof(epoch_us) <= sizeof(d); i++) {
        TEST_ASSERT_FALSE(memcmp(h + i, &epoch_us, sizeof(epoch_us)) == 0);
    }
}

TEST_CASE("b10 diag: every state token is stable, distinct and dot-free",
          "[pool_time_source]")
{
    int i, j;

    for (i = 0; i < (int)POOL_TIME_SOURCE_STATE__COUNT; i++) {
        const char *a = pool_time_source_state_str((PoolTimeSourceState)i);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_TRUE(strlen(a) > 0);
        TEST_ASSERT_NULL(strchr(a, '.'));
        TEST_ASSERT_NULL(strchr(a, ' '));
        TEST_ASSERT_NOT_EQUAL(0, strcmp(a, "TIME_SOURCE_UNKNOWN"));
        for (j = 0; j < i; j++) {
            TEST_ASSERT_NOT_EQUAL(
                0, strcmp(a, pool_time_source_state_str((PoolTimeSourceState)j)));
        }
    }
    TEST_ASSERT_EQUAL_STRING(
        "TIME_SOURCE_UNKNOWN",
        pool_time_source_state_str((PoolTimeSourceState)POOL_TIME_SOURCE_STATE__COUNT));
    TEST_ASSERT_EQUAL_STRING("TIME_SOURCE_UNKNOWN",
                             pool_time_source_state_str((PoolTimeSourceState)9999));
}

/* ================================================================= */
/* The B2 sync observer contract                                      */
/* ================================================================= */

static uint64_t g_obs_mono_us;
static int      g_obs_calls;
static PoolTimeError g_obs_last;
static void    *g_obs_ctx;

static uint64_t obs_mono(void) { return g_obs_mono_us; }
static int obs_init(const PoolTimeSntpConfig *cfg) { (void)cfg; return 0; }
static int obs_start(void) { return 0; }
static int obs_stop(void) { return 0; }
static int obs_deinit(void) { return 0; }

static const PoolTimeSntpPlatformOps OBS_OPS = {
    .monotonic_us = obs_mono,
    .sntp_init    = obs_init,
    .sntp_start   = obs_start,
    .sntp_stop    = obs_stop,
    .sntp_deinit  = obs_deinit,
};

static void obs_cb(void *ctx, PoolTimeError verdict)
{
    g_obs_calls++;
    g_obs_last = verdict;
    g_obs_ctx  = ctx;
}

static void obs_provider_up(PoolTimeSntpProvider *p, void *ctx)
{
    PoolTimeSntpConfig  cfg;
    PoolTimeTrustPolicy pol;

    g_obs_mono_us = 1000000ull;
    g_obs_calls   = 0;
    g_obs_last    = TIME_OK;
    g_obs_ctx     = NULL;

    pool_time_trust_policy_defaults(&pol);
    pool_time_sntp_config_defaults(&cfg);
    cfg.server_count = 1u;
    strncpy(cfg.servers[0], "ntp-obs.example", sizeof(cfg.servers[0]) - 1u);

    memset(p, 0, sizeof(*p));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_init(p, &OBS_OPS, &cfg, &pol));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_set_observer(p, obs_cb, ctx));
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_start(p));
}

TEST_CASE("b10 obs: the observer fires for acceptance and for rejection",
          "[pool_time_source]")
{
    PoolTimeSntpProvider p;
    int marker = 7;

    obs_provider_up(&p, &marker);

    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, 1750000000ull, 0u));
    TEST_ASSERT_EQUAL(1, g_obs_calls);
    TEST_ASSERT_EQUAL(TIME_OK, g_obs_last);
    TEST_ASSERT_EQUAL_PTR(&marker, g_obs_ctx);

    /* A candidate below the sanity band is refused — and still observed. */
    g_obs_mono_us += POOL_TIME_US_PER_S;
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BELOW_MIN,
                      pool_time_sntp_handle_sync(&p, 1000ull, 0u));
    TEST_ASSERT_EQUAL(2, g_obs_calls);
    TEST_ASSERT_EQUAL(TIME_ERR_EPOCH_BELOW_MIN, g_obs_last);

    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_deinit(&p));
}

TEST_CASE("b10 obs: no observer runs after stop or deinit", "[pool_time_source]")
{
    PoolTimeSntpProvider p;

    obs_provider_up(&p, NULL);
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_stop(&p));

    /* The service is not running: the sync is ignored and nothing is called. */
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_SYNCED,
                      pool_time_sntp_handle_sync(&p, 1750000000ull, 0u));
    TEST_ASSERT_EQUAL(0, g_obs_calls);

    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_deinit(&p));
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_INITIALIZED,
                      pool_time_sntp_handle_sync(&p, 1750000000ull, 0u));
    TEST_ASSERT_EQUAL(0, g_obs_calls);

    /* An observer cannot even be registered on a torn-down provider. */
    TEST_ASSERT_EQUAL(TIME_ERR_NOT_INITIALIZED,
                      pool_time_sntp_set_observer(&p, obs_cb, NULL));
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_ARGUMENT,
                      pool_time_sntp_set_observer(NULL, obs_cb, NULL));
}

TEST_CASE("b10 obs: clearing the observer stops notifications but not trust",
          "[pool_time_source]")
{
    PoolTimeSntpProvider p;
    PoolTimeClock        clock;
    PoolTimeTrustPolicy  pol;
    PoolTimeSnapshot     snap;

    obs_provider_up(&p, NULL);
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_set_observer(&p, NULL, NULL));

    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, 1750000000ull, 0u));
    TEST_ASSERT_EQUAL(0, g_obs_calls); /* silent... */

    /* ...but the anchor was still accepted and the clock is trusted. */
    pool_time_trust_policy_defaults(&pol);
    pool_time_sntp_get_clock(&p, &clock);
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_snapshot(&clock, &pol, &snap));
    TEST_ASSERT_TRUE(snap.trusted);

    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_deinit(&p));
}

TEST_CASE("b10 obs: a duplicate identical sync is idempotent for the anchor",
          "[pool_time_source]")
{
    PoolTimeSntpProvider p;
    PoolTimeAnchor       before, after;
    PoolTimeClock        clock;

    obs_provider_up(&p, NULL);
    pool_time_sntp_get_clock(&p, &clock);

    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, 1750000000ull, 0u));
    TEST_ASSERT_TRUE(clock.ops->read_anchor(clock.ctx, &before));

    /* The SAME epoch replayed at the SAME monotonic instant changes nothing
     * about the accepted anchor — the observer still reports the verdict. */
    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_handle_sync(&p, 1750000000ull, 0u));
    TEST_ASSERT_TRUE(clock.ops->read_anchor(clock.ctx, &after));
    TEST_ASSERT_EQUAL_UINT64(before.epoch_us_at_sync, after.epoch_us_at_sync);
    TEST_ASSERT_EQUAL_UINT64(before.monotonic_us_at_sync, after.monotonic_us_at_sync);
    TEST_ASSERT_EQUAL(2, g_obs_calls);

    TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_deinit(&p));
}

/* ================================================================= */
/* Product-policy guards                                              */
/* ================================================================= */

TEST_CASE("b10 policy: no implicit public NTP server is compiled in",
          "[pool_time_source]")
{
    PoolTimeSntpConfig cfg;

    /* The B2 defaults still refuse to invent a server list. */
    pool_time_sntp_config_defaults(&cfg);
    TEST_ASSERT_EQUAL_UINT32(0u, cfg.server_count);
    TEST_ASSERT_EQUAL(TIME_ERR_INVALID_SERVER_CONFIG,
                      pool_time_sntp_validate_config(&cfg));
    TEST_ASSERT_FALSE(cfg.accept_dhcp_ntp); /* DHCP NTP is never auto-trusted */

    /* The compiled product source is empty unless an administrator set one. */
#ifdef CONFIG_NX_TIMED_SESSIONS_NTP_SERVER
    {
        PoolTimeSourceValidation v = validate(CONFIG_NX_TIMED_SESSIONS_NTP_SERVER);
        /* Whatever the build carries, it is either absent or VALID — a
         * malformed compiled source can never be silently used. */
        TEST_ASSERT_TRUE(v.state == TIME_SOURCE_UNCONFIGURED ||
                         v.state == TIME_SOURCE_CONFIGURED);
    }
#endif

    /* Every well-known public NTP hostname must be absent from the binary's
     * configured source. This is the anti-"implicit server" guard. */
    {
        static const char *forbidden[] = { "pool.ntp.org", "time.google.com",
                                           "time.windows.com", "time.apple.com",
                                           "time.cloudflare.com", "time.nist.gov" };
        size_t i;
        for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
#ifdef CONFIG_NX_TIMED_SESSIONS_NTP_SERVER
            TEST_ASSERT_NOT_EQUAL(0, strcmp(CONFIG_NX_TIMED_SESSIONS_NTP_SERVER,
                                            forbidden[i]));
#else
            (void)forbidden[i];
#endif
        }
    }
}

TEST_CASE("b10 policy: an accepted source always fits the B2 config buffer",
          "[pool_time_source]")
{
    char buf[POOL_TIME_SOURCE_HOST_MAX + 8u];
    PoolTimeSourceValidation v;
    PoolTimeSntpConfig       cfg;
    uint32_t                 n;

    for (n = (uint32_t)strlen(".example") + 1u; n <= POOL_TIME_SOURCE_HOST_MAX; n++) {
        build_host(buf, sizeof(buf), n);
        v = validate(buf);
        if (v.state != TIME_SOURCE_CONFIGURED) {
            continue;
        }
        /* The exact production copy path: strncpy into the B2 buffer must
         * never truncate an accepted source. */
        pool_time_sntp_config_defaults(&cfg);
        cfg.server_count = 1u;
        strncpy(cfg.servers[0], buf, sizeof(cfg.servers[0]) - 1u);
        cfg.servers[0][sizeof(cfg.servers[0]) - 1u] = '\0';
        TEST_ASSERT_EQUAL_STRING(buf, cfg.servers[0]);
        TEST_ASSERT_EQUAL(TIME_OK, pool_time_sntp_validate_config(&cfg));
    }
}
