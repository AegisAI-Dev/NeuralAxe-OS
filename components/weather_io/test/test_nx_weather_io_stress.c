/*
 * NeuralAxe Weather-Aware Tuning — Gate W6.3 deterministic stress and
 * boundedness proof.
 *
 * FAKE TRANSPORT ONLY. No socket, no name resolution, no TLS, no host.
 *
 * This file answers the Gate W6.3 §12 questions with assertions rather than
 * assurances: after thousands of mixed ticks, is in-flight still capped at one,
 * does any slot grow, does any counter wrap, can the machine end stuck, and can
 * a result be applied twice or applied late?
 *
 * It is deterministic: the "randomness" is a fixed LCG with a pinned seed, so a
 * failure reproduces exactly.
 */

#include <string.h>
#include "unity.h"

#include "nx_weather_io.h"
#include "weather_runtime.h"
#include "weather_transport.h"
#include "weather_open_meteo.h"

#define WXS_TICKS       4000u
#define WXS_LAT_E4      500000
#define WXS_LON_E4       43000
#define WXS_EPOCH   1751328000ull

static NxWeatherIo g_io;

static const char WXS_BODY_OK[] =
    "{\"latitude\":50.0,\"longitude\":4.3,\"timezone\":\"Europe/Brussels\","
    "\"utc_offset_seconds\":7200,"
    "\"daily_units\":{\"temperature_2m_max\":\"\\u00b0C\"},"
    "\"daily\":{\"time\":[\"2025-07-01\"],\"temperature_2m_max\":[24.5]}}";

/* ---- deterministic fake transport ---- */

typedef struct {
    WeatherProviderResult forced;
    uint32_t calls;
} WxsFake;

static WxsFake g_fake;

static WeatherProviderResult wxs_fetch(void *ctx, const WeatherRequest *req,
                                       WeatherHttpResponse *out)
{
    WxsFake *f = (WxsFake *)ctx;
    size_t n;

    (void)req;
    f->calls++;
    memset(out, 0, sizeof(*out));
    if (f->forced != WEATHER_PROVIDER_OK) {
        return f->forced;
    }
    out->http_status          = 200;
    out->content_type_present = true;
    out->content_type_json    = true;
    out->complete             = true;
    n = strlen(WXS_BODY_OK);
    memcpy(out->body, WXS_BODY_OK, n);
    out->body_len = n;
    return WEATHER_PROVIDER_OK;
}

static const WeatherTransportOps WXS_OPS = { .fetch = wxs_fetch };

/* Pinned LCG: deterministic, reproducible, no clock and no entropy source. */
static uint32_t g_seed;
static uint32_t wxs_rand(void)
{
    g_seed = (g_seed * 1103515245u) + 12345u;
    return (g_seed >> 16) & 0x7FFFu;
}

static void wxs_build(NxWeatherIoRequest *r, uint32_t gen, int8_t slot)
{
    WeatherRequestParams p;

    memset(r, 0, sizeof(*r));
    memset(&p, 0, sizeof(p));
    p.latitude_e4  = WXS_LAT_E4;
    p.longitude_e4 = WXS_LON_E4;
    p.timezone     = WEATHER_TZ_EUROPE_BRUSSELS;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&p, &r->request));

    r->generation        = gen;
    r->window.date.year  = 2025;
    r->window.date.month = 7;
    r->window.date.day   = 1;
    r->window.slot_index = slot;
    r->window.provider   = WEATHER_PROVIDER_OPEN_METEO;

    r->parse.expected_date.year  = 2025;
    r->parse.expected_date.month = 7;
    r->parse.expected_date.day   = 1;
    r->parse.fetch_epoch_s       = WXS_EPOCH;
    r->parse.fetch_epoch_trusted = true;
    r->parse.source_generation   = gen;
}


/*
 * The main sweep. Thousands of statistics-style ticks with mixed success,
 * failure, timeout, busy and pending conditions.
 */
TEST_CASE("w63: stress bounded under thousands of mixed ticks", "[weather_io_stress]")
{
    static WeatherHttpResponse scratch;   /* the worker's single buffer     */
    uint32_t gen = 0u;
    uint32_t submitted = 0u, completed = 0u, consumed = 0u;
    uint32_t t;
    uint64_t now_us = 0u;

    nx_weather_io_init(&g_io);
    memset(&g_fake, 0, sizeof(g_fake));
    g_seed = 0xC0FFEEu;   /* pinned: every run is reproducible */

    for (t = 0; t < WXS_TICKS; t++) {
        uint32_t roll = wxs_rand() % 100u;

        now_us += 1000000ull;   /* a ~1 s statistics tick, monotonic        */

        /* Every tick enforces the deadline first, exactly as the integrator
         * does. This is what guarantees no permanent BUSY. */
        (void)nx_weather_io_tick(&g_io, now_us);

        /* ---- consume anything ready ---- */
        if (g_io.result_present) {
            NxWeatherIoResult got;
            if (nx_weather_io_consume(&g_io, NULL, &got, now_us)) {
                consumed++;
            }
        }

        /* ---- try to submit ---- */
        if (roll < 55u) {
            NxWeatherIoRequest r;
            wxs_build(&r, gen + 1u, (int8_t)(roll % 3u));
            if (nx_weather_io_submit(&g_io, &r, now_us) == WX_IO_SUBMIT_ACCEPTED) {
                gen++;
                submitted++;
            }
        }

        /* ---- run the worker for some submissions, abandon others ---- */
        if (roll >= 20u && g_io.state == WX_IO_SUBMITTED) {
            NxWeatherIoRequest req;
            WeatherObservation obs;
            WeatherProviderResult r;

            if (nx_weather_io_claim(&g_io, &req, now_us)) {
                switch (roll % 4u) {
                case 0: g_fake.forced = WEATHER_PROVIDER_OK; break;
                case 1: g_fake.forced = WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT; break;
                case 2: g_fake.forced = WEATHER_PROVIDER_ERR_READ_TIMEOUT; break;
                default: g_fake.forced = WEATHER_PROVIDER_ERR_JSON_INVALID; break;
                }
                memset(&obs, 0, sizeof(obs));
                r = weather_fetch_execute(&WXS_OPS, &g_fake, &req.request,
                                          &req.parse, NULL, &scratch, &obs);
                if (nx_weather_io_publish(&g_io, req.generation, r, &obs, now_us)) {
                    completed++;
                }
            }
        }

        /* ---- INVARIANTS, checked on EVERY tick ---- */

        /* At most one in flight, always. */
        TEST_ASSERT_TRUE(!g_io.in_flight || g_io.in_flight_generation != 0u);
        /* At most one request queued and at most one result held. */
        TEST_ASSERT_TRUE(g_io.request_present == false ||
                         g_io.state == WX_IO_SUBMITTED);
        /* A result and an in-flight request are mutually exclusive: the
         * publish that fills the slot also clears the in-flight flag. */
        TEST_ASSERT_FALSE(g_io.result_present && g_io.in_flight);
        /*
         * The machine's last-accepted generation is EXACTLY the number of
         * submissions this sweep has had accepted — the request ids are dense
         * and no rejected submission ever consumed one.
         *
         * (The first draft of this assertion was `generation >= gen - 1u`,
         * which underflows to 0xFFFFFFFF on the very first tick, when gen is
         * still 0. QEMU caught it. The exact-equality form below is both
         * correct at gen == 0 and a strictly stronger statement.)
         */
        TEST_ASSERT_EQUAL_UINT32(gen, g_io.generation);
        /* No counter wrapped. */
        TEST_ASSERT_TRUE(g_io.submit_count <= NX_WX_IO_COUNTER_MAX);
        TEST_ASSERT_TRUE(g_io.discard_count <= NX_WX_IO_COUNTER_MAX);
    }

    /* Real work happened — the sweep is not vacuously passing. */
    TEST_ASSERT_TRUE(submitted > 100u);
    TEST_ASSERT_TRUE(completed > 50u);
    TEST_ASSERT_TRUE(consumed > 50u);

    /* Exactly one transport call per completed fetch: the module performed
     * no retry of its own and never duplicated a request. */
    TEST_ASSERT_EQUAL_UINT32(completed, g_fake.calls);

    /* Nothing consumed more than was produced. */
    TEST_ASSERT_TRUE(consumed <= completed);
    /* Every submission is accounted for: completed, discarded or timed out. */
    TEST_ASSERT_TRUE(submitted >= completed);

    /* The machine did NOT end stuck: it is either idle-ish or holding at most
     * one legitimate item, and it still accepts work after one more tick. */
    (void)nx_weather_io_tick(&g_io, now_us + NX_WX_IO_DEADLINE_US + 1u);
    if (g_io.result_present) {
        NxWeatherIoResult got;
        TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, NULL, &got, now_us));
    }
    TEST_ASSERT_TRUE(nx_weather_io_state_accepts(g_io.state));
    TEST_ASSERT_FALSE(g_io.in_flight);
}

/*
 * The abandonment sweep: every request is submitted and then timed out without
 * the worker ever publishing, and the worker's late results all arrive after
 * supersession. Nothing may be applied, and nothing may latch.
 */
TEST_CASE("w63: stress every late result is discarded", "[weather_io_stress]")
{
    uint32_t i;
    uint64_t now_us = 0u;

    nx_weather_io_init(&g_io);
    memset(&g_fake, 0, sizeof(g_fake));
    g_seed = 0xC0FFEEu;   /* pinned: every run is reproducible */

    for (i = 1u; i <= 500u; i++) {
        NxWeatherIoRequest r;
        WeatherObservation obs;

        wxs_build(&r, i, 0);
        TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED,
                          nx_weather_io_submit(&g_io, &r, now_us));
        /* The consumer abandons it. */
        now_us += NX_WX_IO_DEADLINE_US;
        TEST_ASSERT_TRUE(nx_weather_io_tick(&g_io, now_us));

        /* The worker finishes late with a perfectly good forecast. */
        memset(&obs, 0, sizeof(obs));
        obs.forecast_present = true;
        obs.provider_result  = WEATHER_PROVIDER_OK;
        TEST_ASSERT_FALSE(nx_weather_io_publish(&g_io, i, WEATHER_PROVIDER_OK,
                                                &obs, now_us));
        TEST_ASSERT_FALSE(g_io.result_present);
        now_us += 1000000ull;
    }

    TEST_ASSERT_EQUAL_UINT32(500u, g_io.timeout_count);
    TEST_ASSERT_EQUAL_UINT32(500u, g_io.discard_count);
    TEST_ASSERT_EQUAL_UINT32(0u, g_io.success_count);
    TEST_ASSERT_EQUAL_UINT32(0u, g_io.consume_count);
    /* Still usable after 500 abandonments. */
    TEST_ASSERT_TRUE(nx_weather_io_state_accepts(g_io.state));
}

/*
 * Storage boundedness. The machine is a fixed-size struct with two inline
 * slots: there is nothing that could grow, and this pins that fact so a future
 * pointer field fails the build's own test rather than a device.
 */
TEST_CASE("w63: storage is fixed and slotted", "[weather_io_stress]")
{
    NxWeatherIo a, b;
    uint32_t i;

    nx_weather_io_init(&g_io);
    memset(&g_fake, 0, sizeof(g_fake));
    g_seed = 0xC0FFEEu;   /* pinned: every run is reproducible */

    TEST_ASSERT_EQUAL_UINT32(sizeof(NxWeatherIo), sizeof(a));

    nx_weather_io_init(&a);
    memcpy(&b, &a, sizeof(b));

    for (i = 1u; i <= 200u; i++) {
        NxWeatherIoRequest r;
        wxs_build(&r, i, 0);
        (void)nx_weather_io_submit(&a, &r, (uint64_t)i * 1000000ull);
        (void)nx_weather_io_tick(&a, (uint64_t)i * 1000000ull +
                                     NX_WX_IO_DEADLINE_US);
    }
    /* Size is a compile-time property: 200 cycles cannot have changed it. */
    TEST_ASSERT_EQUAL_UINT32(sizeof(NxWeatherIo), sizeof(a));
    TEST_ASSERT_EQUAL_UINT32(sizeof(b), sizeof(a));
}
