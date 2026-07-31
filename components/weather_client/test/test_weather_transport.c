/*
 * End-to-end W3 tests over an INJECTED FAKE transport (W3).
 *
 * NO NETWORK IS EVER TOUCHED. The only transport used here is the local
 * `fake_ops` below, which copies a byte array from the test into the
 * response struct. The real ESP-IDF adapter is compiled and linked (its ops
 * pointer is inspected) but is NEVER invoked — there is no call site for
 * `weather_open_meteo_http_ops()->fetch` in this file or anywhere else in
 * the firmware.
 */

#include <string.h>
#include <stdio.h>
#include "unity.h"
#include "weather_open_meteo.h"
#include "weather_transport.h"
#include "weather_cache.h"
#include "weather_retry.h"
#include "weather_bridge.h"
#include "local_schedule.h"

#define DEG_C "\xC2\xB0" "C"
#define NOW_S 1784000000ull

/* The response buffer embeds the full bounded body: keep it off the stack. */
static WeatherHttpResponse s_resp;

/* ---------------- the fake transport ---------------- */

typedef struct {
    WeatherProviderResult result; /* what the transport layer returns      */
    int http_status;
    bool content_type_present;
    bool content_type_json;
    bool complete;
    bool oversized;
    const char *body;
    size_t body_len;              /* 0 => strlen(body)                     */
    unsigned calls;               /* observed by the tests                 */
    char seen_host[64];
    char seen_query[WEATHER_QUERY_MAX];
} FakeTransport;

static FakeTransport s_fake;

static WeatherProviderResult fake_fetch(void *ctx, const WeatherRequest *req,
                                        WeatherHttpResponse *out)
{
    FakeTransport *f = (FakeTransport *)ctx;
    size_t n;

    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_NOT_NULL(out);
    f->calls++;

    /* Record what the caller would have put on the wire so the tests can
     * audit it (allowlist + privacy). */
    snprintf(f->seen_host, sizeof(f->seen_host), "%s", req->host);
    snprintf(f->seen_query, sizeof(f->seen_query), "%s", req->query);

    memset(out, 0, sizeof(*out));
    if (f->result != WEATHER_PROVIDER_OK) {
        return f->result;
    }
    n = (f->body_len != 0u) ? f->body_len
                            : ((f->body != NULL) ? strlen(f->body) : 0u);
    if (n > WEATHER_RESPONSE_CAP) {
        n = WEATHER_RESPONSE_CAP;
        out->oversized = true;
    }
    if (n > 0u && f->body != NULL) {
        memcpy(out->body, f->body, n);
    }
    out->body_len = n;
    out->http_status = f->http_status;
    out->content_type_present = f->content_type_present;
    out->content_type_json = f->content_type_json;
    out->complete = f->complete;
    out->oversized = out->oversized || f->oversized;
    return WEATHER_PROVIDER_OK;
}

static const WeatherTransportOps fake_ops = { .fetch = fake_fetch };

static const char *k_good_body =
    "{\"utc_offset_seconds\":7200,\"timezone\":\"Europe/Brussels\","
    "\"timezone_abbreviation\":\"CEST\","
    "\"current_units\":{\"temperature_2m\":\"" DEG_C "\"},"
    "\"current\":{\"temperature_2m\":27.5},"
    "\"daily_units\":{\"temperature_2m_max\":\"" DEG_C "\"},"
    "\"daily\":{\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[31.7]}}";

static void fake_reset_ok(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.result = WEATHER_PROVIDER_OK;
    s_fake.http_status = 200;
    s_fake.content_type_present = true;
    s_fake.content_type_json = true;
    s_fake.complete = true;
    s_fake.body = k_good_body;
}

static WeatherLocalDate d(uint16_t y, uint8_t m, uint8_t dd)
{
    WeatherLocalDate x = { y, m, dd };
    return x;
}

/*
 * One complete offline "check": build the allowlisted request, run it
 * through the INJECTED transport, apply the HTTP rules, then the strict
 * parser. Mirrors what a future runtime would do — minus every side effect.
 */
static WeatherProviderResult run_check(const WeatherTransportOps *ops, void *ctx,
                                       WeatherForecast *out)
{
    WeatherRequestParams params;
    WeatherRequest req;
    WeatherParseContext pctx;
    WeatherProviderResult r;

    memset(&params, 0, sizeof(params));
    params.latitude_e4 = 508503;
    params.longitude_e4 = 43517;
    params.timezone = WEATHER_TZ_EUROPE_BRUSSELS;

    r = weather_open_meteo_build_request(&params, &req);
    if (r != WEATHER_PROVIDER_OK) {
        return r;
    }
    r = ops->fetch(ctx, &req, &s_resp);
    if (r != WEATHER_PROVIDER_OK) {
        return r;
    }
    r = weather_transport_validate(&s_resp);
    if (r != WEATHER_PROVIDER_OK) {
        return r;
    }
    memset(&pctx, 0, sizeof(pctx));
    pctx.expected_date = d(2026, 7, 15);
    pctx.fetch_epoch_s = NOW_S;
    pctx.fetch_epoch_trusted = true;
    pctx.source_generation = 1u;
    return weather_open_meteo_parse(s_resp.body, s_resp.body_len, &pctx, out);
}

/* ================================================================== */

TEST_CASE("transport: the real ESP-IDF adapter links but is never invoked",
          "[weather_transport]")
{
    const WeatherTransportOps *real = weather_open_meteo_http_ops();
    /* The adapter compiles into the component graph and exposes its ops —
     * this is the compile/link proof required by the gate. */
    TEST_ASSERT_NOT_NULL(real);
    TEST_ASSERT_NOT_NULL(real->fetch);
    /* It is a DIFFERENT function from the fake used by every test here, and
     * no test calls it: nothing in this suite can open a socket. */
    TEST_ASSERT_TRUE(real->fetch != fake_fetch);
}

TEST_CASE("transport: a good fake response flows through to a usable forecast",
          "[weather_transport]")
{
    WeatherForecast f;
    WeatherLocalDate today = d(2026, 7, 15);

    fake_reset_ok();
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, run_check(&fake_ops, &s_fake, &f));
    TEST_ASSERT_EQUAL_UINT32(1u, s_fake.calls);
    TEST_ASSERT_TRUE(f.validated);
    TEST_ASSERT_EQUAL_INT16(317, f.forecast_max_dc);
    TEST_ASSERT_TRUE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                             &today, NOW_S + 60ull, true, 21600));

    /* Audit what the caller handed to the transport: exactly the
     * allowlisted host and the deterministic query — no credential, no
     * identity, no free-form URL. */
    TEST_ASSERT_EQUAL_STRING(WEATHER_OPEN_METEO_HOST, s_fake.seen_host);
    TEST_ASSERT_EQUAL_STRING("latitude=50.8503&longitude=4.3517"
                             "&daily=temperature_2m_max"
                             "&current=temperature_2m"
                             "&timezone=Europe%2FBrussels"
                             "&forecast_days=1"
                             "&temperature_unit=celsius",
                             s_fake.seen_query);

    /* The forecast is cacheable and bridges into the policy input. */
    {
        static WeatherForecastCache cache;
        TuningForecastInput in;
        weather_cache_init(&cache);
        TEST_ASSERT_TRUE(weather_cache_store(&cache, &f));
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                          weather_cache_usable(&cache, WEATHER_PROVIDER_OPEN_METEO,
                                               &today, NOW_S + 60ull, true, 21600));
        weather_bridge_to_policy(&cache.forecast, WEATHER_PROVIDER_OK,
                                 WEATHER_PROVIDER_OPEN_METEO, &today,
                                 NOW_S + 60ull, true, 21600, &in);
        TEST_ASSERT_EQUAL(TUNING_FORECAST_OK, in.status);
        TEST_ASSERT_EQUAL_INT16(317, in.forecast_max_dc);
    }
}

TEST_CASE("transport: fake failures surface as sanitized codes and no forecast",
          "[weather_transport]")
{
    WeatherForecast f;

    /* transport-level failures (TLS/DNS/timeout) never reach the parser */
    {
        static const WeatherProviderResult failures[] = {
            WEATHER_PROVIDER_ERR_TLS_FAILURE, WEATHER_PROVIDER_ERR_DNS_FAILURE,
            WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT,
            WEATHER_PROVIDER_ERR_READ_TIMEOUT
        };
        size_t i;
        for (i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
            fake_reset_ok();
            s_fake.result = failures[i];
            weather_forecast_init(&f);
            TEST_ASSERT_EQUAL(failures[i], run_check(&fake_ops, &s_fake, &f));
            TEST_ASSERT_FALSE(f.validated);
        }
    }

    /* a redirect is rejected, never followed */
    fake_reset_ok();
    s_fake.http_status = 302;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REDIRECT_REJECTED,
                      run_check(&fake_ops, &s_fake, &f));

    /* a server error */
    fake_reset_ok();
    s_fake.http_status = 503;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_HTTP_STATUS,
                      run_check(&fake_ops, &s_fake, &f));

    /* an HTML error page served as text/html */
    fake_reset_ok();
    s_fake.content_type_json = false;
    s_fake.body = "<html>error</html>";
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CONTENT_TYPE,
                      run_check(&fake_ops, &s_fake, &f));

    /* a truncated body */
    fake_reset_ok();
    s_fake.complete = false;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_TRUNCATED,
                      run_check(&fake_ops, &s_fake, &f));

    /* an oversized source is refused without keeping the bytes */
    fake_reset_ok();
    s_fake.oversized = true;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE,
                      run_check(&fake_ops, &s_fake, &f));

    /* a 200 carrying garbage */
    fake_reset_ok();
    s_fake.body = "{\"unexpected\":true}";
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_TIMEZONE_INVALID,
                      run_check(&fake_ops, &s_fake, &f));
    TEST_ASSERT_FALSE(f.validated);
}

TEST_CASE("transport: a body at the cap is read; beyond the cap is refused",
          "[weather_transport]")
{
    static char big[WEATHER_RESPONSE_CAP * 2];
    WeatherForecast f;

    /* Pad the good payload with trailing whitespace up to exactly the cap:
     * still parseable. */
    memset(big, ' ', sizeof(big));
    memcpy(big, k_good_body, strlen(k_good_body));
    fake_reset_ok();
    s_fake.body = big;
    s_fake.body_len = WEATHER_RESPONSE_CAP;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, run_check(&fake_ops, &s_fake, &f));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)WEATHER_RESPONSE_CAP,
                             (uint32_t)s_resp.body_len);

    /* One byte more and the transport marks it oversized; nothing parses. */
    fake_reset_ok();
    s_fake.body = big;
    s_fake.body_len = WEATHER_RESPONSE_CAP + 1u;
    weather_forecast_init(&f);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE,
                      run_check(&fake_ops, &s_fake, &f));
    TEST_ASSERT_FALSE(f.validated);
}

TEST_CASE("transport: schedule, retry and budget bound the offline check loop",
          "[weather_transport]")
{
    WeatherScheduleConfig cfg;
    WeatherScheduleProgress prog;
    WeatherSchedulePlan plan;
    WeatherRetryState retry;
    WeatherDailyBudget budget;
    WeatherLocalDate today = d(2026, 7, 15);
    WeatherForecast f;
    BrusselsResolution slot;
    uint8_t attempt = 0;

    weather_schedule_config_defaults(&cfg);
    cfg.enabled = true;
    weather_schedule_progress_init(&prog, &today);
    weather_budget_init(&budget, &today);

    /* 05:00 local is due exactly once. */
    TEST_ASSERT_TRUE(brussels_utc_from_local(&today, 300u, &slot));
    weather_schedule_evaluate(&cfg, &prog, slot.utc_s, true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DUE, plan.decision);
    TEST_ASSERT_EQUAL_INT8(0, plan.slot_index);

    weather_retry_begin(&retry, 1u);

    /* Attempt 0 fails at the transport; the budget records the call. */
    TEST_ASSERT_TRUE(weather_budget_allows(&budget, WEATHER_CALL_SCHEDULED, 8u));
    TEST_ASSERT_TRUE(weather_retry_attempt_start(&retry, 1u, 0ull, &attempt));
    fake_reset_ok();
    s_fake.result = WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT,
                      run_check(&fake_ops, &s_fake, &f));
    TEST_ASSERT_TRUE(weather_budget_note_call(&budget, WEATHER_CALL_SCHEDULED));
    TEST_ASSERT_TRUE(weather_retry_note_result(&retry, 1u, attempt, false,
                                               WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT,
                                               0ull));

    /* The retry is gated by the monotonic backoff, not by a busy loop. */
    TEST_ASSERT_FALSE(weather_retry_attempt_start(&retry, 1u, 1ull, &attempt));
    TEST_ASSERT_TRUE(weather_retry_attempt_start(
        &retry, 1u, WEATHER_RETRY_BACKOFF_BASE_US, &attempt));
    TEST_ASSERT_EQUAL_UINT8(1, attempt);

    /* Attempt 1 succeeds. */
    fake_reset_ok();
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, run_check(&fake_ops, &s_fake, &f));
    TEST_ASSERT_TRUE(weather_budget_note_call(&budget, WEATHER_CALL_RETRY));
    TEST_ASSERT_TRUE(weather_retry_note_result(&retry, 1u, attempt, true,
                                               WEATHER_PROVIDER_OK,
                                               WEATHER_RETRY_BACKOFF_BASE_US));
    TEST_ASSERT_TRUE(retry.succeeded);
    TEST_ASSERT_EQUAL_UINT8(2, weather_budget_used(&budget));

    /* The whole execution consumed exactly two transport calls, and the
     * slot is now done for this local date. */
    TEST_ASSERT_EQUAL_UINT32(1u, s_fake.calls); /* the last fake was reset */
    prog = plan.proposed_progress;
    weather_schedule_evaluate(&cfg, &prog, slot.utc_s, true, &plan);
    TEST_ASSERT_NOT_EQUAL(WEATHER_SCHEDULE_DUE, plan.decision);

    /* An exhausted daily budget produces no further call intent. */
    {
        WeatherDailyBudget tight;
        weather_budget_init(&tight, &today);
        TEST_ASSERT_TRUE(weather_budget_note_call(&tight, WEATHER_CALL_SCHEDULED));
        TEST_ASSERT_FALSE(weather_budget_allows(&tight, WEATHER_CALL_SCHEDULED, 1u));
        TEST_ASSERT_FALSE(weather_budget_allows(&tight, WEATHER_CALL_RETRY, 1u));
    }
}
