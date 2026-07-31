/*
 * Deterministic tests for the provider registry, distribution eligibility,
 * attribution metadata, the HTTP-layer validation rules and the bounded URL
 * composer (W3). No network of any kind: every response is a synthetic
 * in-memory struct.
 */

#include <string.h>
#include "unity.h"
#include "weather_provider.h"
#include "weather_transport.h"
#include "weather_open_meteo.h"

/* The response struct embeds the full 4 KiB bounded body; keep it out of the
 * Unity task stack. */
static WeatherHttpResponse s_resp;

static void resp_ok_json(size_t body_len)
{
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.http_status = 200;
    s_resp.content_type_present = true;
    s_resp.content_type_json = true;
    s_resp.complete = true;
    s_resp.body_len = body_len;
    if (body_len > 0u) {
        memset(s_resp.body, '{', body_len);
    }
}

/* ---------------- provider default and registry ---------------- */

TEST_CASE("provider: UNCONFIGURED is the default and has no interface", "[weather_provider]")
{
    /* The zero value of the provider id is UNCONFIGURED, so any zeroed
     * configuration is unconfigured — Open-Meteo is never an implicit
     * operational default. */
    WeatherProviderId zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_UNCONFIGURED, zeroed);

    /* No interface exists for it, so no request can ever be built or
     * parsed through the registry without explicit configuration. */
    TEST_ASSERT_NULL(weather_provider_get(WEATHER_PROVIDER_UNCONFIGURED));
    TEST_ASSERT_NULL(weather_provider_get((WeatherProviderId)99));
    TEST_ASSERT_NULL(weather_provider_attribution(WEATHER_PROVIDER_UNCONFIGURED));
}

TEST_CASE("provider: the Open-Meteo interface is complete and self-consistent", "[weather_provider]")
{
    const WeatherProviderIface *iface =
        weather_provider_get(WEATHER_PROVIDER_OPEN_METEO);
    TEST_ASSERT_NOT_NULL(iface);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OPEN_METEO, iface->id);
    TEST_ASSERT_NOT_NULL(iface->build_request);
    TEST_ASSERT_NOT_NULL(iface->parse_response);
    TEST_ASSERT_NOT_NULL(iface->attribution);
    /* Same adapter reachable through the interface and directly. */
    TEST_ASSERT_TRUE(iface->build_request == weather_open_meteo_build_request);
    TEST_ASSERT_TRUE(iface->parse_response == weather_open_meteo_parse);
    TEST_ASSERT_EQUAL_PTR(weather_provider_open_meteo(), iface);
}

TEST_CASE("provider: attribution metadata is present and licence-complete", "[weather_provider]")
{
    const WeatherAttribution *a =
        weather_provider_attribution(WEATHER_PROVIDER_OPEN_METEO);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("Open-Meteo", a->label);
    TEST_ASSERT_EQUAL_STRING("Weather data by Open-Meteo.com", a->attribution_text);
    TEST_ASSERT_NOT_NULL(a->attribution_url);
    TEST_ASSERT_EQUAL_STRING("CC BY 4.0", a->licence);
    /* The attribution URL is DISPLAY metadata: it is not the request host
     * and is never used as an endpoint. */
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a->attribution_url, WEATHER_OPEN_METEO_HOST));
}

/* ---------------- distribution eligibility ---------------- */

TEST_CASE("eligibility: the public endpoint requires explicit non-commercial use",
          "[weather_provider]")
{
    /* Only an explicit personal/non-commercial distribution may use the
     * public free tier. */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_provider_eligible(WEATHER_PROVIDER_OPEN_METEO,
                                                WEATHER_DISTRIBUTION_PERSONAL_NONCOMMERCIAL));
    /* Unspecified (the zero default) never silently qualifies. */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_INELIGIBLE_DISTRIBUTION,
                      weather_provider_eligible(WEATHER_PROVIDER_OPEN_METEO,
                                                WEATHER_DISTRIBUTION_UNSPECIFIED));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_INELIGIBLE_DISTRIBUTION,
                      weather_provider_eligible(WEATHER_PROVIDER_OPEN_METEO,
                                                WEATHER_DISTRIBUTION_COMMERCIAL));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_INELIGIBLE_DISTRIBUTION,
                      weather_provider_eligible(WEATHER_PROVIDER_OPEN_METEO,
                                                WEATHER_DISTRIBUTION_SELF_HOSTED));
    /* Out-of-range distribution values fail closed. */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_INELIGIBLE_DISTRIBUTION,
                      weather_provider_eligible(WEATHER_PROVIDER_OPEN_METEO,
                                                (WeatherDistributionMode)77));
}

TEST_CASE("eligibility: an unconfigured provider is ineligible under every mode",
          "[weather_provider]")
{
    int m;
    for (m = 0; m < WEATHER_DISTRIBUTION__COUNT; m++) {
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_UNCONFIGURED,
                          weather_provider_eligible(WEATHER_PROVIDER_UNCONFIGURED,
                                                    (WeatherDistributionMode)m));
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_UNCONFIGURED,
                          weather_provider_eligible((WeatherProviderId)42,
                                                    (WeatherDistributionMode)m));
    }
}

/* ---------------- HTTP-layer validation ---------------- */

TEST_CASE("transport: 2xx JSON with a complete bounded body is accepted",
          "[weather_provider]")
{
    resp_ok_json(600u);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, weather_transport_validate(&s_resp));
    /* Exactly at the cap is still accepted. */
    resp_ok_json(WEATHER_RESPONSE_CAP);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, weather_transport_validate(&s_resp));
    /* Media-type parameters are permitted by the prefix contract; the flag
     * is what this layer judges. */
    resp_ok_json(10u);
    s_resp.http_status = 204;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, weather_transport_validate(&s_resp));
}

TEST_CASE("transport: every failure class maps to its sanitized code",
          "[weather_provider]")
{
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_INTERNAL, weather_transport_validate(NULL));

    /* oversized dominates everything else */
    resp_ok_json(100u);
    s_resp.oversized = true;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE,
                      weather_transport_validate(&s_resp));
    resp_ok_json(100u);
    s_resp.body_len = WEATHER_RESPONSE_CAP + 1u;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE,
                      weather_transport_validate(&s_resp));

    /* incomplete body */
    resp_ok_json(100u);
    s_resp.complete = false;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_TRUNCATED,
                      weather_transport_validate(&s_resp));

    /* every 3xx is rejected, never followed */
    {
        static const int redirects[] = { 300, 301, 302, 303, 307, 308, 399 };
        size_t i;
        for (i = 0; i < sizeof(redirects) / sizeof(redirects[0]); i++) {
            resp_ok_json(100u);
            s_resp.http_status = redirects[i];
            TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REDIRECT_REJECTED,
                              weather_transport_validate(&s_resp));
        }
    }

    /* other non-2xx */
    {
        static const int bad[] = { 0, 100, 400, 401, 403, 404, 429, 500, 503 };
        size_t i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            resp_ok_json(100u);
            s_resp.http_status = bad[i];
            TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_HTTP_STATUS,
                              weather_transport_validate(&s_resp));
        }
    }

    /* wrong content type, and a MISSING content type, both fail closed */
    resp_ok_json(100u);
    s_resp.content_type_json = false;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CONTENT_TYPE,
                      weather_transport_validate(&s_resp));
    resp_ok_json(100u);
    s_resp.content_type_present = false;
    s_resp.content_type_json = false;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CONTENT_TYPE,
                      weather_transport_validate(&s_resp));

    /* empty body on an otherwise fine 2xx */
    resp_ok_json(0u);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_TRUNCATED,
                      weather_transport_validate(&s_resp));
}

/* ---------------- bounded URL composition ---------------- */

TEST_CASE("url: composition is https-only, allowlisted and length-bounded",
          "[weather_provider]")
{
    WeatherRequest req;
    char url[WEATHER_REQUEST_URL_MAX];

    memset(&req, 0, sizeof(req));
    req.host = WEATHER_OPEN_METEO_HOST;
    req.path = WEATHER_OPEN_METEO_PATH;
    strcpy(req.query, "latitude=50.8503&longitude=4.3517");

    TEST_ASSERT_TRUE(weather_request_url(&req, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://" WEATHER_OPEN_METEO_HOST
                             WEATHER_OPEN_METEO_PATH
                             "?latitude=50.8503&longitude=4.3517",
                             url);
    /* The scheme is a compile constant: no http:// URL is representable. */
    TEST_ASSERT_EQUAL(0, strncmp(url, "https://", 8));

    /* argument rejections */
    TEST_ASSERT_FALSE(weather_request_url(NULL, url, sizeof(url)));
    TEST_ASSERT_FALSE(weather_request_url(&req, NULL, sizeof(url)));
    TEST_ASSERT_FALSE(weather_request_url(&req, url, 0u));
    {
        WeatherRequest bad = req;
        bad.host = NULL;
        TEST_ASSERT_FALSE(weather_request_url(&bad, url, sizeof(url)));
        bad = req;
        bad.path = NULL;
        TEST_ASSERT_FALSE(weather_request_url(&bad, url, sizeof(url)));
    }

    /* a too-small destination fails and leaves an empty string */
    {
        char small[16];
        TEST_ASSERT_FALSE(weather_request_url(&req, small, sizeof(small)));
        TEST_ASSERT_EQUAL_STRING("", small);
    }

    /* The bound is sufficient BY CONSTRUCTION: even a maximal query (the
     * largest string the bounded WeatherRequest can hold) still composes
     * inside WEATHER_REQUEST_URL_MAX, so the allowlisted request can never
     * be silently dropped for length. */
    memset(req.query, 'q', sizeof(req.query) - 1u);
    req.query[sizeof(req.query) - 1u] = '\0';
    TEST_ASSERT_TRUE(weather_request_url(&req, url, sizeof(url)));
    TEST_ASSERT_TRUE(strlen(url) < WEATHER_REQUEST_URL_MAX);
    TEST_ASSERT_EQUAL(0, strncmp(url, "https://", 8));

    /* A destination smaller than the composed URL still fails closed. */
    {
        char tight[64];
        TEST_ASSERT_FALSE(weather_request_url(&req, tight, sizeof(tight)));
        TEST_ASSERT_EQUAL_STRING("", tight);
    }
}

/* ---------------- sanitized tokens ---------------- */

TEST_CASE("tokens: result, provider and distribution strings are total",
          "[weather_provider]")
{
    int i;
    for (i = 0; i < WEATHER_PROVIDER_RESULT__COUNT; i++) {
        const char *s = weather_provider_result_str((WeatherProviderResult)i);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_NOT_EQUAL(0, strcmp("ERR_UNKNOWN", s));
        /* Tokens are bounded machine codes, never host names, coordinates,
         * status text or response fragments. */
        TEST_ASSERT_TRUE(strlen(s) < 40u);
        TEST_ASSERT_NULL(strstr(s, "."));
        TEST_ASSERT_NULL(strstr(s, "/"));
    }
    TEST_ASSERT_EQUAL_STRING("ERR_UNKNOWN",
                             weather_provider_result_str((WeatherProviderResult)999));

    for (i = 0; i < WEATHER_PROVIDER__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN",
            weather_provider_id_str((WeatherProviderId)i)));
    }
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", weather_provider_id_str((WeatherProviderId)9));

    for (i = 0; i < WEATHER_DISTRIBUTION__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN",
            weather_distribution_str((WeatherDistributionMode)i)));
    }
    TEST_ASSERT_EQUAL_STRING("UNKNOWN",
                             weather_distribution_str((WeatherDistributionMode)9));
}
