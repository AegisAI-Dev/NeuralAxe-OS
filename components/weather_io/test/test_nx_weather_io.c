/*
 * NeuralAxe Weather-Aware Tuning — Gate W6.3 bounded async I/O tests.
 *
 * FAKE TRANSPORT ONLY. Nothing in this file opens a socket, resolves a name,
 * performs a TLS handshake or contacts any host. The committed real adapter
 * (weather_open_meteo_http.c) is never referenced here, and the fake below
 * returns caller-programmed bytes from static storage.
 *
 * Every test drives the pure state machine directly and single-threaded, which
 * is why each entry in the Gate W6.3 failure matrix is deterministically
 * reachable with no scheduler and no network.
 */

#include <string.h>
#include "unity.h"

#include "nx_weather_io.h"
#include "weather_runtime.h"
#include "weather_transport.h"
#include "weather_open_meteo.h"
#include "tuning_profile.h"

/* ------------------------------------------------------------------ */
/* Synthetic fixtures — no private value, no real coordinate           */
/* ------------------------------------------------------------------ */

#define WXIO_LAT_E4   500000    /* synthetic, not an owner location        */
#define WXIO_LON_E4    43000
#define WXIO_EPOCH  1751328000ull   /* fixed synthetic trusted epoch       */

static NxWeatherIo g_io;

/* One valid minimal Open-Meteo daily payload the committed parser accepts. */
static const char WXIO_BODY_OK[] =
    "{\"latitude\":50.0,\"longitude\":4.3,\"timezone\":\"Europe/Brussels\","
    "\"utc_offset_seconds\":7200,"
    "\"daily_units\":{\"temperature_2m_max\":\"\\u00b0C\"},"
    "\"daily\":{\"time\":[\"2025-07-01\"],\"temperature_2m_max\":[24.5]}}";

static const char WXIO_BODY_GARBAGE[] = "{not json at all";

/* ---- the fake transport ---- */

typedef struct {
    WeatherProviderResult transport_result; /* what ops->fetch returns      */
    int      http_status;
    bool     content_type_present;
    bool     content_type_json;
    bool     oversized;
    bool     complete;
    const char *body;
    uint32_t calls;                          /* how often it was invoked    */
} WxIoFake;

static WxIoFake g_fake;

static WeatherProviderResult wxio_fake_fetch(void *ctx, const WeatherRequest *req,
                                             WeatherHttpResponse *out)
{
    WxIoFake *f = (WxIoFake *)ctx;
    size_t n;

    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_NOT_NULL(out);
    /* The allowlisted host/path must have come from the committed builder. */
    TEST_ASSERT_EQUAL_STRING(WEATHER_OPEN_METEO_HOST, req->host);
    TEST_ASSERT_EQUAL_STRING(WEATHER_OPEN_METEO_PATH, req->path);

    f->calls++;
    memset(out, 0, sizeof(*out));
    if (f->transport_result != WEATHER_PROVIDER_OK) {
        return f->transport_result;   /* connect/TLS/read failure           */
    }
    out->http_status          = f->http_status;
    out->content_type_present = f->content_type_present;
    out->content_type_json    = f->content_type_json;
    out->oversized            = f->oversized;
    out->complete             = f->complete;
    if (f->body != NULL) {
        n = strlen(f->body);
        if (n > WEATHER_RESPONSE_CAP) {
            n = WEATHER_RESPONSE_CAP;
        }
        memcpy(out->body, f->body, n);
        out->body_len = n;
    }
    return WEATHER_PROVIDER_OK;
}

static const WeatherTransportOps WXIO_FAKE_OPS = { .fetch = wxio_fake_fetch };

static void fake_reset_ok(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.transport_result     = WEATHER_PROVIDER_OK;
    g_fake.http_status          = 200;
    g_fake.content_type_present = true;
    g_fake.content_type_json    = true;
    g_fake.complete             = true;
    g_fake.body                 = WXIO_BODY_OK;
}

/* ---- request construction ---- */

static WeatherLocalDate wxio_date(uint16_t y, uint8_t m, uint8_t d)
{
    WeatherLocalDate x;
    x.year = y; x.month = m; x.day = d;
    return x;
}

static void wxio_request(NxWeatherIoRequest *r, uint32_t gen, int8_t slot,
                         WeatherLocalDate date)
{
    WeatherRequestParams p;

    memset(r, 0, sizeof(*r));
    memset(&p, 0, sizeof(p));
    p.latitude_e4  = WXIO_LAT_E4;
    p.longitude_e4 = WXIO_LON_E4;
    p.timezone     = WEATHER_TZ_EUROPE_BRUSSELS;

    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&p, &r->request));

    r->generation         = gen;
    r->window.date        = date;
    r->window.slot_index  = slot;
    r->window.provider    = WEATHER_PROVIDER_OPEN_METEO;

    r->parse.expected_date       = wxio_date(2025, 7, 1);
    r->parse.fetch_epoch_s       = WXIO_EPOCH;
    r->parse.fetch_epoch_trusted = true;
    r->parse.source_generation   = gen;
}

/* Run the worker half synchronously: claim, execute the fake, publish. */
static WeatherProviderResult wxio_run_worker(uint64_t now_us)
{
    NxWeatherIoRequest  req;
    WeatherObservation  obs;
    WeatherProviderResult r;
    /*
     * STATIC, not on the stack. WeatherHttpResponse is ~4 KB and this repo has
     * already been bitten once by a test task quietly overflowing its stack and
     * corrupting a LATER suite. The real worker stages the response in a module
     * static for the same reason.
     */
    static WeatherHttpResponse scratch;

    if (!nx_weather_io_claim(&g_io, &req, now_us)) {
        return WEATHER_PROVIDER_ERR_INTERNAL;
    }
    memset(&obs, 0, sizeof(obs));
    r = weather_fetch_execute(&WXIO_FAKE_OPS, &g_fake, &req.request, &req.parse,
                              NULL, &scratch, &obs);
    (void)nx_weather_io_publish(&g_io, req.generation, r, &obs, now_us);
    return r;
}


/* ================================================================== */
/* 1 — trusted time absent => no submission                            */
/* ================================================================== */
TEST_CASE("w63: untrusted epoch is never submitted", "[weather_io]")
{
    NxWeatherIoRequest r;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    r.parse.fetch_epoch_trusted = false;    /* the ONLY change             */

    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_REJECTED_INVALID,
                      nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL(WX_IO_IDLE, g_io.state);
    TEST_ASSERT_FALSE(g_io.in_flight);
    TEST_ASSERT_EQUAL_UINT32(0u, g_io.submit_count);
    TEST_ASSERT_EQUAL_UINT32(0u, g_fake.calls);   /* no transport touch     */
}

/* ================================================================== */
/* 2 — schedule not due (no slot) => no submission                     */
/* ================================================================== */
TEST_CASE("w63: no due slot is never submitted", "[weather_io]")
{
    NxWeatherIoRequest r;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, -1, wxio_date(2025, 7, 1));   /* slot_index < 0    */

    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_REJECTED_INVALID,
                      nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL_UINT32(0u, g_io.submit_count);
    TEST_ASSERT_EQUAL_UINT32(0u, g_fake.calls);
}

/* An unconfigured provider is equally unsubmittable. */
TEST_CASE("w63: unconfigured provider is never submitted", "[weather_io]")
{
    NxWeatherIoRequest r;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    r.window.provider = WEATHER_PROVIDER_UNCONFIGURED;

    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_REJECTED_INVALID,
                      nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL_UINT32(0u, g_fake.calls);
}

/* ================================================================== */
/* 3 — due schedule => exactly one submission, caller does not block   */
/* ================================================================== */
TEST_CASE("w63: due schedule submits exactly once", "[weather_io]")
{
    NxWeatherIoRequest r;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));

    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED,
                      nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL(WX_IO_SUBMITTED, g_io.state);
    TEST_ASSERT_TRUE(g_io.in_flight);
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.submit_count);
    /*
     * The submitter has returned and the transport has NOT run: the fetch
     * happens later, on the worker. This is the whole point of the gate.
     */
    TEST_ASSERT_EQUAL_UINT32(0u, g_fake.calls);
}

/* ================================================================== */
/* 4 — worker busy => a duplicate tick does not submit again           */
/* ================================================================== */
TEST_CASE("w63: duplicate tick while busy does not resubmit", "[weather_io]")
{
    NxWeatherIoRequest a, b;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&a, 1u, 0, wxio_date(2025, 7, 1));
    wxio_request(&b, 2u, 0, wxio_date(2025, 7, 1));

    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &a, 1000u));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_REJECTED_BUSY,
                      nx_weather_io_submit(&g_io, &b, 2000u));
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.submit_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.reject_busy_count);
    /* Still exactly one in flight, and it is still the FIRST one. */
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.in_flight_generation);
}

/* ================================================================== */
/* 5 — successful fake fetch => published and consumed exactly once    */
/* ================================================================== */
TEST_CASE("w63: success publishes and consumes exactly once", "[weather_io]")
{
    NxWeatherIoRequest r;
    NxWeatherIoResult  got;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, wxio_run_worker(2000u));

    TEST_ASSERT_EQUAL(WX_IO_RESULT_READY, g_io.state);
    TEST_ASSERT_TRUE(g_io.result_present);
    TEST_ASSERT_EQUAL_UINT32(1u, g_fake.calls);

    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &r.window, &got, 3000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, got.result);
    TEST_ASSERT_TRUE(got.observation.forecast_present);
    TEST_ASSERT_EQUAL_UINT32(1u, got.generation);
    TEST_ASSERT_EQUAL(WX_IO_IDLE, g_io.state);

    /* Exactly once: a second consume finds nothing. */
    TEST_ASSERT_FALSE(nx_weather_io_consume(&g_io, &r.window, &got, 4000u));
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.consume_count);
}

/* ================================================================== */
/* 6 — duplicate tick while a result is pending => no duplicate fetch  */
/* ================================================================== */
TEST_CASE("w63: duplicate tick while result pending does not fetch", "[weather_io]")
{
    NxWeatherIoRequest a, b;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&a, 1u, 0, wxio_date(2025, 7, 1));
    wxio_request(&b, 2u, 1, wxio_date(2025, 7, 1));

    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &a, 1000u));
    (void)wxio_run_worker(2000u);
    TEST_ASSERT_TRUE(g_io.result_present);

    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_REJECTED_RESULT_PENDING,
                      nx_weather_io_submit(&g_io, &b, 3000u));
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.reject_pending_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_fake.calls);   /* still exactly one      */
}

/* ================================================================== */
/* 7 — timeout => worker returns idle, runtime remains safe            */
/* ================================================================== */
TEST_CASE("w63: deadline returns to idle without touching the worker", "[weather_io]")
{
    NxWeatherIoRequest r;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));

    /* One microsecond short of the deadline: still in flight. */
    TEST_ASSERT_FALSE(nx_weather_io_tick(&g_io, 1000u + NX_WX_IO_DEADLINE_US - 1u));
    TEST_ASSERT_TRUE(g_io.in_flight);

    /* At the deadline: released, reported, and back to accepting work. */
    TEST_ASSERT_TRUE(nx_weather_io_tick(&g_io, 1000u + NX_WX_IO_DEADLINE_US));
    TEST_ASSERT_EQUAL(WX_IO_TIMEOUT, g_io.state);
    TEST_ASSERT_FALSE(g_io.in_flight);
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.timeout_count);
    TEST_ASSERT_TRUE(nx_weather_io_state_accepts(g_io.state));
    /* The transport was never invoked and nothing was fabricated. */
    TEST_ASSERT_EQUAL_UINT32(0u, g_fake.calls);
    TEST_ASSERT_FALSE(g_io.result_present);
}

/* ================================================================== */
/* 8 — TLS / connect failure                                           */
/* ================================================================== */
TEST_CASE("w63: connect failure is a bounded failure result", "[weather_io]")
{
    NxWeatherIoRequest r;
    NxWeatherIoResult  got;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    g_fake.transport_result = WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT;
    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT, wxio_run_worker(2000u));

    TEST_ASSERT_EQUAL_UINT32(1u, g_io.failure_count);
    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &r.window, &got, 3000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT, got.result);
    TEST_ASSERT_FALSE(got.observation.forecast_present);
    TEST_ASSERT_EQUAL(WX_IO_IDLE, g_io.state);
}

/* ================================================================== */
/* 9 — parser rejection                                                */
/* ================================================================== */
TEST_CASE("w63: parser rejection yields no forecast", "[weather_io]")
{
    NxWeatherIoRequest r;
    NxWeatherIoResult  got;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    g_fake.body = WXIO_BODY_GARBAGE;
    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_NOT_EQUAL(WEATHER_PROVIDER_OK, wxio_run_worker(2000u));

    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &r.window, &got, 3000u));
    TEST_ASSERT_FALSE(got.observation.forecast_present);
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.failure_count);
}

/* ================================================================== */
/* 10 — stale forecast (payload dated for another day)                 */
/* ================================================================== */
TEST_CASE("w63: wrong date payload is rejected by the committed parser", "[weather_io]")
{
    NxWeatherIoRequest r;
    NxWeatherIoResult  got;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 2));
    /* The payload says 2025-07-01; tell the parser we expect 2025-07-02. */
    r.parse.expected_date = wxio_date(2025, 7, 2);

    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_NOT_EQUAL(WEATHER_PROVIDER_OK, wxio_run_worker(2000u));

    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &r.window, &got, 3000u));
    TEST_ASSERT_FALSE(got.observation.forecast_present);
}

/* ================================================================== */
/* 11 — oversized response                                             */
/* ================================================================== */
TEST_CASE("w63: oversized response is rejected before parsing", "[weather_io]")
{
    NxWeatherIoRequest r;
    NxWeatherIoResult  got;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    g_fake.oversized = true;
    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE,
                      wxio_run_worker(2000u));

    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &r.window, &got, 3000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE, got.result);
    TEST_ASSERT_FALSE(got.observation.forecast_present);
}

/* Partial/truncated body is the neighbouring transport-layer refusal. */
TEST_CASE("w63: truncated response is rejected", "[weather_io]")
{
    NxWeatherIoRequest r;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    g_fake.complete = false;
    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_TRUNCATED, wxio_run_worker(2000u));
}

/* A 3xx must never be followed. */
TEST_CASE("w63: redirect is rejected", "[weather_io]")
{
    NxWeatherIoRequest r;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    g_fake.http_status = 302;
    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REDIRECT_REJECTED,
                      wxio_run_worker(2000u));
}

/* ================================================================== */
/* 12 — network lost during the request                                */
/* ================================================================== */
TEST_CASE("w63: network lost mid request is a read timeout", "[weather_io]")
{
    NxWeatherIoRequest r;
    NxWeatherIoResult  got;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    /* The link drops after the claim: the transport reports a read timeout. */
    g_fake.transport_result = WEATHER_PROVIDER_ERR_READ_TIMEOUT;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_READ_TIMEOUT, wxio_run_worker(2000u));

    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &r.window, &got, 3000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_READ_TIMEOUT, got.result);
    TEST_ASSERT_EQUAL(WX_IO_IDLE, g_io.state);   /* returned to idle        */
}

/* ================================================================== */
/* 13 — stale generation result is discarded                           */
/* ================================================================== */
TEST_CASE("w63: publish after timeout is discarded", "[weather_io]")
{
    NxWeatherIoRequest r;
    WeatherObservation obs;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));

    /* The consumer gives up on it. */
    TEST_ASSERT_TRUE(nx_weather_io_tick(&g_io, 1000u + NX_WX_IO_DEADLINE_US));

    /* The worker finishes anyway and tries to publish generation 1. */
    memset(&obs, 0, sizeof(obs));
    obs.provider_result = WEATHER_PROVIDER_OK;
    obs.forecast_present = true;
    TEST_ASSERT_FALSE(nx_weather_io_publish(&g_io, 1u, WEATHER_PROVIDER_OK,
                                            &obs, 2000u));

    TEST_ASSERT_FALSE(g_io.result_present);   /* nothing was stored          */
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.discard_count);
    TEST_ASSERT_EQUAL(WX_IO_RESULT_DISCARDED, g_io.last_event);
}

/* ================================================================== */
/* 14 — a newer generation supersedes; the old result cannot apply     */
/* ================================================================== */
TEST_CASE("w63: newer generation supersedes the old attempt", "[weather_io]")
{
    NxWeatherIoRequest a, b;
    NxWeatherIoResult  got;
    WeatherObservation stale;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&a, 1u, 0, wxio_date(2025, 7, 1));
    wxio_request(&b, 2u, 1, wxio_date(2025, 7, 1));

    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &a, 1000u));
    TEST_ASSERT_TRUE(nx_weather_io_tick(&g_io, 1000u + NX_WX_IO_DEADLINE_US));

    /* A new window opens and is submitted. */
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED,
                      nx_weather_io_submit(&g_io, &b,
                                           2000u + NX_WX_IO_DEADLINE_US));
    /* The ABANDONED first attempt now returns. It must not be stored. */
    memset(&stale, 0, sizeof(stale));
    stale.forecast_present = true;
    TEST_ASSERT_FALSE(nx_weather_io_publish(&g_io, 1u, WEATHER_PROVIDER_OK,
                                            &stale, 3000u + NX_WX_IO_DEADLINE_US));
    TEST_ASSERT_FALSE(g_io.result_present);

    /* The second attempt completes normally and is the one that lands. */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      wxio_run_worker(4000u + NX_WX_IO_DEADLINE_US));
    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &b.window, &got,
                                           5000u + NX_WX_IO_DEADLINE_US));
    TEST_ASSERT_EQUAL_UINT32(2u, got.generation);
    TEST_ASSERT_EQUAL_INT8(1, got.window.slot_index);
}

/* A result whose WINDOW no longer matches is discarded, not returned. */
TEST_CASE("w63: result for a superseded window is discarded on consume", "[weather_io]")
{
    NxWeatherIoRequest r;
    NxWeatherIoResult  got;
    NxWeatherIoWindow  other;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, wxio_run_worker(2000u));

    other = r.window;
    other.date = wxio_date(2025, 7, 2);      /* the day rolled over         */

    TEST_ASSERT_FALSE(nx_weather_io_consume(&g_io, &other, &got, 3000u));
    TEST_ASSERT_FALSE(g_io.result_present);
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.discard_count);
    TEST_ASSERT_EQUAL(WX_IO_IDLE, g_io.state);   /* usable again            */
}

/* ================================================================== */
/* 15 — result slot already occupied => bounded safe behaviour         */
/* ================================================================== */
TEST_CASE("w63: publish into an occupied slot never overwrites", "[weather_io]")
{
    NxWeatherIoRequest r;
    NxWeatherIoResult  got;
    WeatherObservation obs;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, wxio_run_worker(2000u));
    TEST_ASSERT_TRUE(g_io.result_present);

    /* Force a second publish for the same generation: refused, and the
     * UNREAD result survives untouched. */
    memset(&obs, 0, sizeof(obs));
    TEST_ASSERT_FALSE(nx_weather_io_publish(&g_io, 1u, WEATHER_PROVIDER_ERR_INTERNAL,
                                            &obs, 2500u));
    TEST_ASSERT_TRUE(g_io.result_present);
    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &r.window, &got, 3000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, got.result);   /* the FIRST one   */
}

/* ================================================================== */
/* 16 — repeated failures: no unbounded retry, no permanent BUSY       */
/* ================================================================== */
TEST_CASE("w63: repeated failures never latch busy", "[weather_io]")
{
    NxWeatherIoResult got;
    uint32_t i;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    g_fake.transport_result = WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT;

    for (i = 1u; i <= 50u; i++) {
        NxWeatherIoRequest r;

        wxio_request(&r, i, 0, wxio_date(2025, 7, 1));
        TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED,
                          nx_weather_io_submit(&g_io, &r, (uint64_t)i * 1000u));
        (void)wxio_run_worker((uint64_t)i * 1000u + 10u);
        TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &r.window, &got,
                                               (uint64_t)i * 1000u + 20u));
        /* Every cycle ends IDLE and accepting: the machine never latches. */
        TEST_ASSERT_EQUAL(WX_IO_IDLE, g_io.state);
        TEST_ASSERT_TRUE(nx_weather_io_state_accepts(g_io.state));
        TEST_ASSERT_FALSE(g_io.in_flight);
    }
    TEST_ASSERT_EQUAL_UINT32(50u, g_io.failure_count);
    /* The module itself performed no retry of its own: one fetch per submit. */
    TEST_ASSERT_EQUAL_UINT32(50u, g_fake.calls);
    TEST_ASSERT_EQUAL_UINT32(50u, g_io.submit_count);
}

/* ================================================================== */
/* 17 + 18 — a successful observation reaches the committed W1 policy, */
/*           and an UNVALIDATED production profile stays non-actionable */
/* ================================================================== */
TEST_CASE("w63: observation reaches w1 policy and stays non actionable", "[weather_io]")
{
    NxWeatherIoRequest r;
    NxWeatherIoResult  got;
    WeatherRuntime     rt;
    WeatherRuntimeConfig cfg;
    WeatherRecommendation rec;
    TuningPolicyEnvironment penv;
    TuningPolicyInput       pin;
    WeatherRuntimeStepEnv   env;
    size_t n = 0;

    /* Produce a real observation through the async path. */

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, wxio_run_worker(2000u));
    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &r.window, &got, 3000u));
    TEST_ASSERT_TRUE(got.observation.forecast_present);

    /* Feed it to the committed W4 runtime against the PRODUCTION registry. */
    weather_runtime_config_defaults(&cfg);
    cfg.enabled           = true;
    cfg.expected_provider = WEATHER_PROVIDER_OPEN_METEO;
    cfg.location.latitude_e4  = WXIO_LAT_E4;
    cfg.location.longitude_e4 = WXIO_LON_E4;
    cfg.location.timezone     = WEATHER_TZ_EUROPE_BRUSSELS;
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &cfg, NULL));

    memset(&penv, 0, sizeof(penv));
    memset(&pin, 0, sizeof(pin));
    penv.profiles = tuning_registry_gamma601(&n);
    penv.profile_count = n;
    env.policy_env = &penv;
    env.policy_in  = &pin;

    memset(&rec, 0, sizeof(rec));
    (void)weather_runtime_step(&rt, &env, &got.observation, &rec);

    /*
     * The load-bearing assertions of this whole gate: whatever the policy
     * decided, nothing is executable and nothing is auto-eligible, because
     * every production Gamma 601 profile ships UNVALIDATED.
     */
    TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
    TEST_ASSERT_TRUE(n > 0u);
}

/* ================================================================== */
/* 19 — executed=false on EVERY path (the diagnostic constant)         */
/* ================================================================== */
TEST_CASE("w63: every io outcome leaves the recommendation unexecuted", "[weather_io]")
{
    /*
     * Drive EVERY distinct I/O outcome into the committed W4 runtime against
     * the production registry and assert the same three things each time:
     * nothing is auto-eligible, the intent carries no tuning payload (the W1
     * contract), and the reported reason is always a NOT-executed reason.
     *
     * The recommendation type has no "executed" field to set — that is the
     * structural half of the guarantee. This test covers the behavioural
     * half: no combination of provider result and forecast presence produces
     * an actionable outcome on a production artifact.
     */

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    static const WeatherProviderResult OUTCOMES[] = {
        WEATHER_PROVIDER_OK,
        WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT,
        WEATHER_PROVIDER_ERR_READ_TIMEOUT,
        WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE,
        WEATHER_PROVIDER_ERR_TRUNCATED,
        WEATHER_PROVIDER_ERR_REDIRECT_REJECTED,
        WEATHER_PROVIDER_ERR_JSON_INVALID,
        WEATHER_PROVIDER_ERR_CACHE_STALE,
    };
    size_t i;

    for (i = 0; i < sizeof(OUTCOMES) / sizeof(OUTCOMES[0]); i++) {
        NxWeatherIoRequest    r;
        NxWeatherIoResult     got;
        WeatherRuntime        rt;
        WeatherRuntimeConfig  cfg;
        WeatherRecommendation rec;
        TuningPolicyEnvironment penv;
        TuningPolicyInput       pin;
        WeatherRuntimeStepEnv   env;
        size_t n = 0;

        nx_weather_io_init(&g_io);
        fake_reset_ok();
        if (OUTCOMES[i] != WEATHER_PROVIDER_OK) {
            g_fake.transport_result = OUTCOMES[i];
        }

        wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
        TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED,
                          nx_weather_io_submit(&g_io, &r, 1000u));
        (void)wxio_run_worker(2000u);
        TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &r.window, &got, 3000u));

        weather_runtime_config_defaults(&cfg);
        cfg.enabled               = true;
        cfg.expected_provider     = WEATHER_PROVIDER_OPEN_METEO;
        cfg.location.latitude_e4  = WXIO_LAT_E4;
        cfg.location.longitude_e4 = WXIO_LON_E4;
        cfg.location.timezone     = WEATHER_TZ_EUROPE_BRUSSELS;
        TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK,
                          weather_runtime_init(&rt, &cfg, NULL));

        memset(&penv, 0, sizeof(penv));
        memset(&pin, 0, sizeof(pin));
        penv.profiles      = tuning_registry_gamma601(&n);
        penv.profile_count = n;
        env.policy_env = &penv;
        env.policy_in  = &pin;

        memset(&rec, 0, sizeof(rec));
        (void)weather_runtime_step(&rt, &env, &got.observation, &rec);

        TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
        /* The W1 intent is an intent: no payload of any kind travels here. */
        TEST_ASSERT_TRUE(rec.intent.action == TUNING_SELECT_NONE ||
                         rec.intent.action == TUNING_SELECT_PROFILE ||
                         rec.intent.action == TUNING_SELECT_RETAIN_INHIBIT ||
                         rec.intent.action == TUNING_SELECT_OPERATOR_RECOVERY);
        TEST_ASSERT_TRUE(rec.not_executed < WEATHER_NOT_EXECUTED__COUNT);
    }
}

/* ================================================================== */
/* 20 — diagnostics are bounded, saturating and privacy-safe           */
/* ================================================================== */
TEST_CASE("w63: diagnostics are bounded and fail closed", "[weather_io]")
{
    NxWeatherIo     fresh;
    NxWeatherIoDiag d;
    NxWeatherIoRequest r;

    /* An uninitialised machine may not be quoted. */

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    memset(&fresh, 0, sizeof(fresh));
    nx_weather_io_observe(&fresh, 1000u, &d);
    TEST_ASSERT_FALSE(d.present);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)WX_IO_UNINITIALIZED, d.state);

    nx_weather_io_observe(NULL, 1000u, &d);
    TEST_ASSERT_FALSE(d.present);

    /* A live machine reports bounded facts and a monotonic age. */
    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    nx_weather_io_observe(&g_io, 1000u + 5000000u, &d);
    TEST_ASSERT_TRUE(d.present);
    TEST_ASSERT_TRUE(d.in_flight);
    TEST_ASSERT_FALSE(d.result_pending);
    TEST_ASSERT_EQUAL_UINT32(1u, d.submit_count);
    TEST_ASSERT_EQUAL_UINT32(5u, d.request_age_s);   /* seconds, monotonic  */
    TEST_ASSERT_EQUAL_UINT32(1u, d.generation);
}

/* Counters saturate rather than wrap. */
TEST_CASE("w63: counters saturate", "[weather_io]")
{
    NxWeatherIoRequest r;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    g_io.submit_count = NX_WX_IO_COUNTER_MAX;
    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_EQUAL_UINT32(NX_WX_IO_COUNTER_MAX, g_io.submit_count);
}

/* Every token is total and never NULL, including out-of-range input. */
TEST_CASE("w63: tokens are total", "[weather_io]")
{
    int i;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    for (i = 0; i < WX_IO_STATE__COUNT + 4; i++) {
        TEST_ASSERT_NOT_NULL(nx_weather_io_state_str((NxWeatherIoState)i));
    }
    for (i = 0; i < WX_IO_SUBMIT__COUNT + 4; i++) {
        TEST_ASSERT_NOT_NULL(nx_weather_io_submit_str((NxWeatherIoSubmitResult)i));
    }
}

/* NULL is never a crash and never a success. */
TEST_CASE("w63: null is total", "[weather_io]")
{
    NxWeatherIoRequest r;
    NxWeatherIoResult  got;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    nx_weather_io_init(NULL);
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_REJECTED_UNINITIALIZED,
                      nx_weather_io_submit(NULL, &r, 0u));
    TEST_ASSERT_FALSE(nx_weather_io_claim(NULL, &r, 0u));
    TEST_ASSERT_FALSE(nx_weather_io_publish(NULL, 1u, WEATHER_PROVIDER_OK, NULL, 0u));
    TEST_ASSERT_FALSE(nx_weather_io_consume(NULL, NULL, &got, 0u));
    TEST_ASSERT_FALSE(nx_weather_io_tick(NULL, 0u));
    TEST_ASSERT_FALSE(nx_weather_io_window_equal(NULL, NULL));
}

/* A generation must strictly advance; replay is refused. */
TEST_CASE("w63: generation must strictly advance", "[weather_io]")
{
    NxWeatherIoRequest a, b;
    NxWeatherIoResult  got;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&a, 5u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &a, 1000u));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, wxio_run_worker(2000u));
    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &a.window, &got, 3000u));

    /* Replaying the same id, and any lower id, is invalid. */
    wxio_request(&b, 5u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_REJECTED_INVALID,
                      nx_weather_io_submit(&g_io, &b, 4000u));
    wxio_request(&b, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_REJECTED_INVALID,
                      nx_weather_io_submit(&g_io, &b, 5000u));
}

/* A claim can happen at most once per submission. */
TEST_CASE("w63: claim is single shot", "[weather_io]")
{
    NxWeatherIoRequest r, c;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_TRUE(nx_weather_io_claim(&g_io, &c, 1500u));
    TEST_ASSERT_FALSE(nx_weather_io_claim(&g_io, &c, 1600u));
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.claim_count);
    TEST_ASSERT_EQUAL(WX_IO_RUNNING, g_io.state);
}

/* ================================================================== */
/* Gate W6.3 §11 — LOGICAL timeout vs PHYSICAL worker execution        */
/* ================================================================== */

/*
 * The adversarial question: when the consumer abandons a request at the
 * deadline while the worker is still PHYSICALLY blocked inside HTTP, can a
 * second fetch begin and reuse the worker-owned response buffer concurrently?
 *
 * The answer must be no, and the reason must be structural rather than lucky.
 * This test drives exactly that interleaving.
 */
TEST_CASE("w63 §11: a logical timeout never authorises a second physical fetch",
          "[weather_io]")
{
    NxWeatherIoRequest a, b, claimed;
    WeatherObservation late;
    NxWeatherIoResult  got;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    /* 1 — submit and let the worker CLAIM it. The worker is now notionally
     *     inside the blocking transport call; nothing has been published. */
    wxio_request(&a, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &a, 1000u));
    TEST_ASSERT_TRUE(nx_weather_io_claim(&g_io, &claimed, 1100u));
    TEST_ASSERT_EQUAL(WX_IO_RUNNING, g_io.state);

    /* 2 — the consumer abandons it at the deadline. */
    TEST_ASSERT_TRUE(nx_weather_io_tick(&g_io, 1000u + NX_WX_IO_DEADLINE_US));
    TEST_ASSERT_EQUAL(WX_IO_TIMEOUT, g_io.state);
    TEST_ASSERT_FALSE(g_io.in_flight);

    /* 3 — a new window opens and IS accepted: the logical slot is free. */
    wxio_request(&b, 2u, 1, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED,
                      nx_weather_io_submit(&g_io, &b, 2000u + NX_WX_IO_DEADLINE_US));

    /*
     * 4 — THE LOAD-BEARING ASSERTION. The worker is still physically executing
     *     request 1. Only ONE claim may be outstanding at a time, and the
     *     single worker task is the thing that enforces it: there is exactly
     *     one task, and it cannot re-enter claim() until its current fetch
     *     returns. A claim here is legal ONLY because this test is standing in
     *     for that one task having finished; what must never happen is TWO
     *     live claims, which the machine makes impossible by emptying the
     *     request slot on the first claim.
     *
     *     Concretely: the second claim yields request 2 and NOT request 1
     *     again, so request 1's buffer is never handed to a second executor.
     */
    TEST_ASSERT_TRUE(nx_weather_io_claim(&g_io, &claimed, 3000u + NX_WX_IO_DEADLINE_US));
    TEST_ASSERT_EQUAL_UINT32(2u, claimed.generation);
    /* And no third claim exists: the slot is empty again. */
    TEST_ASSERT_FALSE(nx_weather_io_claim(&g_io, &claimed, 3100u + NX_WX_IO_DEADLINE_US));

    /* 5 — request 1 finally returns. It must not land, and must not disturb
     *     the in-flight identity of request 2. */
    memset(&late, 0, sizeof(late));
    late.forecast_present = true;
    late.provider_result  = WEATHER_PROVIDER_OK;
    TEST_ASSERT_FALSE(nx_weather_io_publish(&g_io, 1u, WEATHER_PROVIDER_OK,
                                            &late, 4000u + NX_WX_IO_DEADLINE_US));
    TEST_ASSERT_FALSE(g_io.result_present);
    TEST_ASSERT_TRUE(g_io.in_flight);
    TEST_ASSERT_EQUAL_UINT32(2u, g_io.in_flight_generation);

    /* 6 — request 2 publishes normally and is the only one that can be read. */
    TEST_ASSERT_TRUE(nx_weather_io_publish(&g_io, 2u, WEATHER_PROVIDER_OK,
                                           &late, 5000u + NX_WX_IO_DEADLINE_US));
    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &b.window, &got,
                                           6000u + NX_WX_IO_DEADLINE_US));
    TEST_ASSERT_EQUAL_UINT32(2u, got.generation);
    TEST_ASSERT_EQUAL_INT8(1, got.window.slot_index);
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.discard_count);
}

/*
 * A timed-out request must never leave a claimable orphan behind: if the
 * deadline fires BEFORE the worker ever claimed it, the request is gone.
 */
TEST_CASE("w63 §11: a request abandoned before the claim leaves no orphan",
          "[weather_io]")
{
    NxWeatherIoRequest r, claimed;

    nx_weather_io_init(&g_io);
    fake_reset_ok();

    wxio_request(&r, 1u, 0, wxio_date(2025, 7, 1));
    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED, nx_weather_io_submit(&g_io, &r, 1000u));
    TEST_ASSERT_TRUE(nx_weather_io_tick(&g_io, 1000u + NX_WX_IO_DEADLINE_US));

    /* The worker wakes late and finds nothing to do. */
    TEST_ASSERT_FALSE(nx_weather_io_claim(&g_io, &claimed, 2000u + NX_WX_IO_DEADLINE_US));
    TEST_ASSERT_EQUAL_UINT32(0u, g_io.claim_count);
    TEST_ASSERT_EQUAL_UINT32(0u, g_fake.calls);
}
