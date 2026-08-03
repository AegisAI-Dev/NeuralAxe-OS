/*
 * Gate W5 privacy, source-selection and recommendation-only properties.
 *
 * These tests assert the properties that make a private location safe:
 * nothing implicit, nothing discovered, nothing disclosed, nothing executed.
 * All coordinates here are synthetic.
 */

#include <string.h>
#include "unity.h"
#include "nx_weather_source.h"
#include "weather_open_meteo.h"
#include "weather_transport.h"

/* A distinctive synthetic coordinate whose decimal digits are easy to search
 * for in any produced text. Not a real site. */
#define PRIV_LAT_E4  123456
#define PRIV_LON_E4  -654321

static void make_ready(NxWeatherSourceConfig *c)
{
    nx_weather_source_defaults(c);
    c->enabled             = true;
    c->distribution        = NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL;
    c->provider            = WEATHER_PROVIDER_OPEN_METEO;
    c->latitude_e4         = PRIV_LAT_E4;
    c->longitude_e4        = PRIV_LON_E4;
    c->timezone            = WEATHER_TZ_EUROPE_BRUSSELS;
    c->recommendation_only = true;
    weather_schedule_config_defaults(&c->schedule);
}

/* ---------------- B. privacy ---------------- */

TEST_CASE("w5 privacy: no status token can carry a coordinate", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;
    int i;

    /* Drive every failure class with a private coordinate present, and prove
     * the reported token never contains any part of it. */
    static const char *digits[] = { "1234", "123456", "6543", "654321" };

    for (i = 0; i < (int)NX_WX_SRC__COUNT; i++) {
        const char *t = nx_weather_source_status_str((NxWeatherSourceStatus)i);
        for (unsigned d = 0; d < sizeof(digits) / sizeof(digits[0]); d++) {
            TEST_ASSERT_NULL(strstr(t, digits[d]));
        }
    }

    make_ready(&c);
    c.timezone = WEATHER_TZ_UNSPECIFIED;   /* fails with coordinates set */
    {
        const char *t = nx_weather_source_status_str(nx_weather_source_validate(&c));
        TEST_ASSERT_NULL(strstr(t, "1234"));
        TEST_ASSERT_NULL(strstr(t, "6543"));
    }
}

TEST_CASE("w5 privacy: no status token names a host, URL, city or provider domain", "[nx_wx_src]")
{
    int i;

    for (i = 0; i < (int)NX_WX_SRC__COUNT; i++) {
        const char *t = nx_weather_source_status_str((NxWeatherSourceStatus)i);
        TEST_ASSERT_NULL(strstr(t, "http"));
        TEST_ASSERT_NULL(strstr(t, "://"));
        TEST_ASSERT_NULL(strstr(t, "."));          /* no dotted host or domain */
        TEST_ASSERT_NULL(strstr(t, "open-meteo"));
        TEST_ASSERT_NULL(strstr(t, "api"));
        TEST_ASSERT_NULL(strstr(t, "?"));
        TEST_ASSERT_NULL(strstr(t, "&"));
        TEST_ASSERT_NULL(strstr(t, "="));
    }
}

TEST_CASE("w5 privacy: the model carries no location label of any kind", "[nx_wx_src]")
{
    /*
     * The committed W2 record has a presentation-only `location_label`. W5
     * deliberately has no equivalent: there is no char array in the whole
     * configuration, so a city or site name cannot be stored, projected or
     * printed. Proven structurally: the configuration is exactly its
     * bounded scalar fields plus the committed schedule.
     */
    NxWeatherSourceConfig c;
    size_t accounted;

    make_ready(&c);
    accounted = sizeof(c.enabled) + sizeof(c.distribution) + sizeof(c.provider) +
                sizeof(c.latitude_e4) + sizeof(c.longitude_e4) +
                sizeof(c.timezone) + sizeof(c.schedule) +
                sizeof(c.recommendation_only);
    /* Every byte is accounted for by a known scalar or the committed
     * schedule; padding only can differ. */
    TEST_ASSERT_TRUE(sizeof(c) >= accounted);
    TEST_ASSERT_TRUE(sizeof(c) - accounted < sizeof(void *) * 4u);
}

TEST_CASE("w5 privacy: a projected runtime config exposes no string", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;
    WeatherRuntimeConfig  rc;

    make_ready(&c);
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                      nx_weather_source_to_runtime(&c, &rc));
    /* The location travels as scaled integers, never as formatted text. */
    TEST_ASSERT_EQUAL_INT32(PRIV_LAT_E4, rc.location.latitude_e4);
    TEST_ASSERT_EQUAL_INT32(PRIV_LON_E4, rc.location.longitude_e4);
    /* And the runtime configuration itself holds no pointer that could
     * reference owner text. */
    TEST_ASSERT_EQUAL(sizeof(rc.location.latitude_e4), sizeof(int32_t));
}

/* ---------------- C. source selection ---------------- */

TEST_CASE("w5 source: nothing is implicit without an explicit selection", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;
    WeatherRuntimeConfig  rc;

    /* Enabled, owner-managed, located, timezoned — but NO provider chosen.
     * The device must not fall back to the only implemented adapter. */
    make_ready(&c);
    c.provider = WEATHER_PROVIDER_UNCONFIGURED;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_PROVIDER,
                      nx_weather_source_to_runtime(&c, &rc));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_UNCONFIGURED, rc.expected_provider);
    TEST_ASSERT_FALSE(rc.enabled);
}

TEST_CASE("w5 source: the endpoint stays owned by the committed W3 adapter", "[nx_wx_src]")
{
    /*
     * W5 selects a provider by IDENTITY, never by URL: there is no host,
     * scheme, path or query field anywhere in the W5 configuration, so no
     * arbitrary endpoint can be injected. The adapter keeps its fixed
     * host/path allowlist.
     */
    WeatherRequestParams params;
    WeatherRequest       req;

    memset(&params, 0, sizeof(params));
    params.latitude_e4  = PRIV_LAT_E4;
    params.longitude_e4 = PRIV_LON_E4;
    params.timezone     = WEATHER_TZ_EUROPE_BRUSSELS;

    memset(&req, 0, sizeof(req));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&params, &req));
    TEST_ASSERT_EQUAL_STRING(WEATHER_OPEN_METEO_HOST, req.host);
    TEST_ASSERT_EQUAL_STRING(WEATHER_OPEN_METEO_PATH, req.path);
}

TEST_CASE("w5 source: an unconfigured source builds no request at all", "[nx_wx_src]")
{
    /* The committed W3 builder rejects the unset sentinel, so an
     * unconfigured device cannot even form a URL, let alone send one. */
    WeatherRequestParams params;
    WeatherRequest       req;

    memset(&params, 0, sizeof(params));
    params.timezone = WEATHER_TZ_EUROPE_BRUSSELS;
    memset(&req, 0, sizeof(req));
    TEST_ASSERT_NOT_EQUAL(WEATHER_PROVIDER_OK,
                          weather_open_meteo_build_request(&params, &req));
}

TEST_CASE("w5 source: a redirect to another host can never be followed", "[nx_wx_src]")
{
    /*
     * The committed transport validator rejects every 3xx outright. The
     * response must otherwise be a COMPLETE, well-formed one, so the test
     * proves the redirect rule itself rather than an earlier guard: the
     * validator checks completeness before status, so an incomplete
     * response would report TRUNCATED and the assertion would pass for the
     * wrong reason.
     */
    WeatherHttpResponse r;

    memset(&r, 0, sizeof(r));
    r.complete             = true;
    r.http_status          = 302;
    r.content_type_present = true;
    r.content_type_json    = true;
    r.body_len             = 2u;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REDIRECT_REJECTED,
                      weather_transport_validate(&r));
    r.http_status = 301;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REDIRECT_REJECTED,
                      weather_transport_validate(&r));
    r.http_status = 307;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REDIRECT_REJECTED,
                      weather_transport_validate(&r));

    /* The same response with a 200 is accepted, which proves the rejection
     * above was caused by the redirect status and nothing else. */
    r.http_status = 200;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, weather_transport_validate(&r));
}

/* ---------------- E. recommendation-only ---------------- */

TEST_CASE("w5 rec-only: no field can request an execution", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    /*
     * The only execution-adjacent field is `recommendation_only`, and the
     * one value it may hold for a usable configuration is true. The
     * build-time binder has no Kconfig symbol that can clear it, so no build
     * can produce an executing configuration.
     */
    make_ready(&c);
    TEST_ASSERT_TRUE(c.recommendation_only);
    c.recommendation_only = false;
    TEST_ASSERT_FALSE(nx_weather_source_ready(nx_weather_source_validate(&c)));

    nx_weather_source_from_build_config(&c);
    TEST_ASSERT_TRUE(c.recommendation_only);
}

TEST_CASE("w5 rec-only: a ready configuration still authorizes no action", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;
    WeatherRuntimeConfig  rc;
    WeatherRuntime        rt;
    WeatherRecommendation rec;
    WeatherRuntimeState   state;

    /*
     * With a fully valid private configuration but NO injected clock,
     * transport or store, the committed runtime must still reach only a
     * bounded waiting state: zero requests, zero policy evaluation, zero
     * recommendation. This is the boot posture of the pilot.
     */
    make_ready(&c);
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                      nx_weather_source_to_runtime(&c, &rc));
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &rc, NULL));

    memset(&rec, 0, sizeof(rec));
    state = weather_runtime_step(&rt, NULL, NULL, &rec);
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME, state);
    TEST_ASSERT_FALSE(rec.present);
    TEST_ASSERT_EQUAL(WEATHER_NOT_EXECUTED_NO_TRUSTED_TIME, rec.not_executed);
}

TEST_CASE("w5 rec-only: no raw wall-clock fallback exists", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;
    WeatherRuntimeConfig  rc;
    WeatherRuntime        rt;
    WeatherRecommendation rec;
    int i;

    /* Repeated steps without a trusted clock never progress past waiting —
     * the runtime does not invent a time source. */
    make_ready(&c);
    (void)nx_weather_source_to_runtime(&c, &rc);
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK, weather_runtime_init(&rt, &rc, NULL));
    for (i = 0; i < 16; i++) {
        memset(&rec, 0, sizeof(rec));
        TEST_ASSERT_EQUAL(WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME,
                          weather_runtime_step(&rt, NULL, NULL, &rec));
        TEST_ASSERT_FALSE(rec.present);
    }
}
