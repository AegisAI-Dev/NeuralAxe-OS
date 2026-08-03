/*
 * Deterministic tests for the Open-Meteo adapter (W3): the fixed allowlisted
 * request contract, deterministic integer coordinate rendering and the strict
 * bounded response parser.
 *
 * NO NETWORK: every "response" is a synthetic in-memory byte buffer. The
 * adapter is a pure function of (bytes, context) — nothing here opens a
 * socket, resolves a name or reads a clock.
 */

#include <string.h>
#include <stdio.h>
#include "unity.h"
#include "weather_open_meteo.h"
#include "weather_transport.h"

/* UTF-8 degree sign + 'C' — split so the hex escape cannot swallow the C. */
#define DEG_C "\xC2\xB0" "C"

#define FETCH_EPOCH 1784000000ull /* synthetic trusted epoch */

static char s_body[WEATHER_RESPONSE_CAP + 64];

static WeatherLocalDate d(uint16_t y, uint8_t m, uint8_t dd)
{
    WeatherLocalDate x = { y, m, dd };
    return x;
}

static WeatherParseContext ctx_default(void)
{
    WeatherParseContext c;
    memset(&c, 0, sizeof(c));
    c.expected_date = d(2026, 7, 15);
    c.fetch_epoch_s = FETCH_EPOCH;
    c.fetch_epoch_trusted = true;
    c.source_generation = 11u;
    return c;
}

#define DEFAULT_OFFSET "7200"
#define DEFAULT_TZ "Europe/Brussels"
#define DEFAULT_ABBREV "\"timezone_abbreviation\":\"CEST\","
#define DEFAULT_DAILY_UNITS "\"temperature_2m_max\":\"" DEG_C "\""
#define DEFAULT_DAILY "\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[32.4]"
#define DEFAULT_CURRENT \
    ",\"current_units\":{\"temperature_2m\":\"" DEG_C "\"}," \
    "\"current\":{\"temperature_2m\":28.1}"

/* Compose a response body from independently mutable fragments. */
static size_t body_of(const char *offset, const char *timezone,
                      const char *abbrev_field, const char *daily_units,
                      const char *daily, const char *current)
{
    int n = snprintf(s_body, sizeof(s_body),
                     "{\"utc_offset_seconds\":%s,\"timezone\":\"%s\",%s"
                     "\"daily_units\":{%s},\"daily\":{%s}%s}",
                     offset, timezone, abbrev_field, daily_units, daily,
                     current);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(s_body));
    return (size_t)n;
}

static size_t body_default(void)
{
    return body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                   DEFAULT_DAILY_UNITS, DEFAULT_DAILY, DEFAULT_CURRENT);
}

static WeatherProviderResult parse_current(WeatherForecast *out)
{
    WeatherParseContext c = ctx_default();
    return weather_open_meteo_parse((const uint8_t *)s_body, strlen(s_body),
                                    &c, out);
}

/* Assert a rejection AND that nothing partially valid survived. */
static void expect_reject(WeatherProviderResult expected)
{
    WeatherForecast f;
    WeatherProviderResult r = parse_current(&f);
    TEST_ASSERT_EQUAL(expected, r);
    TEST_ASSERT_FALSE(f.validated);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_UNCONFIGURED, f.provider);
    TEST_ASSERT_EQUAL(WEATHER_TZ_UNSPECIFIED, f.timezone);
    TEST_ASSERT_EQUAL_INT16(0, f.forecast_max_dc);
    TEST_ASSERT_FALSE(f.current_valid);
}

/* ================================================================== */
/* Request contract                                                    */
/* ================================================================== */

TEST_CASE("open-meteo: the request is one fixed host, path and query",
          "[weather_open_meteo]")
{
    WeatherRequestParams p;
    WeatherRequest req;

    memset(&p, 0, sizeof(p));
    p.latitude_e4 = 508503;  /* 50.8503 */
    p.longitude_e4 = 43517;  /*  4.3517 */
    p.timezone = WEATHER_TZ_EUROPE_BRUSSELS;

    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&p, &req));
    /* The host and path are the compile-defined allowlist entries — the
     * struct has no field in which any other host or path could appear. */
    TEST_ASSERT_EQUAL_STRING("api.open-meteo.com", req.host);
    TEST_ASSERT_EQUAL_STRING("/v1/forecast", req.path);
    TEST_ASSERT_EQUAL_STRING(WEATHER_OPEN_METEO_HOST, req.host);
    TEST_ASSERT_EQUAL_STRING(WEATHER_OPEN_METEO_PATH, req.path);
    /* Byte-exact deterministic query, fixed parameter order. */
    TEST_ASSERT_EQUAL_STRING("latitude=50.8503&longitude=4.3517"
                             "&daily=temperature_2m_max"
                             "&current=temperature_2m"
                             "&timezone=Europe%2FBrussels"
                             "&forecast_days=1"
                             "&temperature_unit=celsius",
                             req.query);

    /* Building twice yields byte-identical requests (no nonce, no clock). */
    {
        WeatherRequest again;
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                          weather_open_meteo_build_request(&p, &again));
        TEST_ASSERT_EQUAL_STRING(req.query, again.query);
    }
}

TEST_CASE("open-meteo: the request carries no key, identity or telemetry",
          "[weather_open_meteo]")
{
    WeatherRequestParams p;
    WeatherRequest req;
    static const char *forbidden[] = {
        "key", "apikey", "api_key", "token", "auth", "secret", "password",
        "user", "worker", "wallet", "serial", "mac", "device", "uuid",
        "hostname", "session", "client", "hashrate", "asic", "firmware"
    };
    size_t i;

    memset(&p, 0, sizeof(p));
    p.latitude_e4 = 508503;
    p.longitude_e4 = 43517;
    p.timezone = WEATHER_TZ_EUROPE_BRUSSELS;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&p, &req));

    for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        TEST_ASSERT_NULL(strstr(req.query, forbidden[i]));
    }
    /* The whole outbound request is exactly host + path + query: there is
     * no body, no header field and no credential anywhere in the model. */
    TEST_ASSERT_TRUE(strlen(req.query) < WEATHER_QUERY_MAX);
}

TEST_CASE("open-meteo: coordinates render deterministically with four decimals",
          "[weather_open_meteo]")
{
    WeatherRequestParams p;
    WeatherRequest req;
    memset(&p, 0, sizeof(p));
    p.timezone = WEATHER_TZ_EUROPE_BRUSSELS;

    /* negative values keep the sign and the four-decimal shape */
    p.latitude_e4 = -336789;
    p.longitude_e4 = 1512093;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&p, &req));
    TEST_ASSERT_EQUAL(0, strncmp(req.query,
                                 "latitude=-33.6789&longitude=151.2093", 36));

    /* a zero component renders canonically (never "-0.0000") */
    p.latitude_e4 = 0;
    p.longitude_e4 = 43517;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&p, &req));
    TEST_ASSERT_EQUAL(0, strncmp(req.query, "latitude=0.0000&longitude=4.3517", 32));

    /* sub-degree magnitudes keep leading zeros in the fraction */
    p.latitude_e4 = -1;
    p.longitude_e4 = 1;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&p, &req));
    TEST_ASSERT_EQUAL(0, strncmp(req.query,
                                 "latitude=-0.0001&longitude=0.0001", 33));

    /* extremes inside the band are accepted */
    p.latitude_e4 = 900000;
    p.longitude_e4 = -1800000;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&p, &req));
    TEST_ASSERT_EQUAL(0, strncmp(req.query,
                                 "latitude=90.0000&longitude=-180.0000", 36));
}

TEST_CASE("open-meteo: invalid request parameters fail closed",
          "[weather_open_meteo]")
{
    WeatherRequestParams p;
    WeatherRequest req;
    memset(&p, 0, sizeof(p));
    p.latitude_e4 = 508503;
    p.longitude_e4 = 43517;
    p.timezone = WEATHER_TZ_EUROPE_BRUSSELS;

    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REQUEST_INVALID,
                      weather_open_meteo_build_request(NULL, &req));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REQUEST_INVALID,
                      weather_open_meteo_build_request(&p, NULL));

    /* the MVP is Brussels-framed: any other timezone id is rejected */
    {
        WeatherRequestParams q = p;
        q.timezone = WEATHER_TZ_UNSPECIFIED;
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REQUEST_INVALID,
                          weather_open_meteo_build_request(&q, &req));
        /* the output is cleared, never left half-built */
        TEST_ASSERT_NULL(req.host);
        TEST_ASSERT_EQUAL_STRING("", req.query);
    }
    /* unset (0/0) coordinates are rejected, not silently sent as null island */
    {
        WeatherRequestParams q = p;
        q.latitude_e4 = 0;
        q.longitude_e4 = 0;
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REQUEST_INVALID,
                          weather_open_meteo_build_request(&q, &req));
    }
    /* out-of-range coordinates */
    {
        WeatherRequestParams q = p;
        q.latitude_e4 = 900001;
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REQUEST_INVALID,
                          weather_open_meteo_build_request(&q, &req));
        q = p;
        q.latitude_e4 = -900001;
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REQUEST_INVALID,
                          weather_open_meteo_build_request(&q, &req));
        q = p;
        q.longitude_e4 = 1800001;
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REQUEST_INVALID,
                          weather_open_meteo_build_request(&q, &req));
        q = p;
        q.longitude_e4 = -1800001;
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_REQUEST_INVALID,
                          weather_open_meteo_build_request(&q, &req));
    }
}

/* ================================================================== */
/* Parser: accepted shapes                                             */
/* ================================================================== */

TEST_CASE("parse: a well-formed response yields only normalized facts",
          "[weather_open_meteo]")
{
    WeatherForecast f;
    size_t n = body_default();
    WeatherParseContext c = ctx_default();

    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_parse((const uint8_t *)s_body, n, &c, &f));
    TEST_ASSERT_TRUE(f.validated);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OPEN_METEO, f.provider);
    TEST_ASSERT_EQUAL(WEATHER_TZ_EUROPE_BRUSSELS, f.timezone);
    TEST_ASSERT_EQUAL_INT32(7200, f.utc_offset_s);
    TEST_ASSERT_EQUAL_UINT16(2026, f.local_date.year);
    TEST_ASSERT_EQUAL_UINT8(7, f.local_date.month);
    TEST_ASSERT_EQUAL_UINT8(15, f.local_date.day);
    TEST_ASSERT_EQUAL_INT16(324, f.forecast_max_dc); /* 32.4 C */
    TEST_ASSERT_TRUE(f.current_valid);
    TEST_ASSERT_EQUAL_INT16(281, f.current_dc);      /* 28.1 C, informational */
    TEST_ASSERT_EQUAL_UINT64(FETCH_EPOCH, f.fetch_epoch_s);
    TEST_ASSERT_TRUE(f.fetch_epoch_trusted);
    TEST_ASSERT_EQUAL_UINT32(11u, f.source_generation);
}

TEST_CASE("parse: the current block is optional and winter offsets are legal",
          "[weather_open_meteo]")
{
    WeatherForecast f;

    /* no current block at all */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS, DEFAULT_DAILY, "");
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, parse_current(&f));
    TEST_ASSERT_TRUE(f.validated);
    TEST_ASSERT_FALSE(f.current_valid);
    TEST_ASSERT_EQUAL_INT16(0, f.current_dc);
    TEST_ASSERT_EQUAL_INT16(324, f.forecast_max_dc);

    /* a present current object without the variable is still fine */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS, DEFAULT_DAILY, ",\"current\":{}");
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, parse_current(&f));
    TEST_ASSERT_FALSE(f.current_valid);

    /* CET (winter) offset with the matching abbreviation */
    (void)body_of("3600", DEFAULT_TZ, "\"timezone_abbreviation\":\"CET\",",
                  DEFAULT_DAILY_UNITS, DEFAULT_DAILY, DEFAULT_CURRENT);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, parse_current(&f));
    TEST_ASSERT_EQUAL_INT32(3600, f.utc_offset_s);

    /* the abbreviation field is optional */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, "", DEFAULT_DAILY_UNITS,
                  DEFAULT_DAILY, DEFAULT_CURRENT);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, parse_current(&f));
}

TEST_CASE("parse: deci-degree conversion and the sanity band",
          "[weather_open_meteo]")
{
    WeatherForecast f;

    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[32.44]",
                  "");
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, parse_current(&f));
    TEST_ASSERT_EQUAL_INT16(324, f.forecast_max_dc); /* rounds down */

    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[32.46]",
                  "");
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, parse_current(&f));
    TEST_ASSERT_EQUAL_INT16(325, f.forecast_max_dc); /* rounds up */

    /* negatives round away from zero */
    (void)body_of("3600", DEFAULT_TZ, "\"timezone_abbreviation\":\"CET\",",
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[-5.76]",
                  "");
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, parse_current(&f));
    TEST_ASSERT_EQUAL_INT16(-58, f.forecast_max_dc);

    /* band edges are inclusive */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[60]", "");
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, parse_current(&f));
    TEST_ASSERT_EQUAL_INT16(WEATHER_TEMP_SANITY_MAX_DC, f.forecast_max_dc);

    (void)body_of("3600", DEFAULT_TZ, "\"timezone_abbreviation\":\"CET\",",
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[-60]", "");
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, parse_current(&f));
    TEST_ASSERT_EQUAL_INT16(WEATHER_TEMP_SANITY_MIN_DC, f.forecast_max_dc);
}

/* ================================================================== */
/* Parser: rejection matrix                                            */
/* ================================================================== */

TEST_CASE("parse: argument, size and encoding rejections", "[weather_open_meteo]")
{
    WeatherForecast f;
    WeatherParseContext c = ctx_default();
    size_t n = body_default();

    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_JSON_INVALID,
                      weather_open_meteo_parse(NULL, n, &c, &f));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_JSON_INVALID,
                      weather_open_meteo_parse((const uint8_t *)s_body, 0u, &c, &f));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_JSON_INVALID,
                      weather_open_meteo_parse((const uint8_t *)s_body, n, NULL, &f));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_JSON_INVALID,
                      weather_open_meteo_parse((const uint8_t *)s_body, n, &c, NULL));

    /* anything beyond the transport cap is refused before parsing */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE,
                      weather_open_meteo_parse((const uint8_t *)s_body,
                                               WEATHER_RESPONSE_CAP + 1u, &c, &f));

    /* an embedded NUL is a malformed payload, never a silent truncation */
    s_body[10] = '\0';
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_JSON_INVALID,
                      weather_open_meteo_parse((const uint8_t *)s_body, n, &c, &f));

    /* an invalid expected date in the context fails closed */
    n = body_default();
    c.expected_date = d(2024, 7, 15); /* outside the supported band */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_DATE_INVALID,
                      weather_open_meteo_parse((const uint8_t *)s_body, n, &c, &f));
    TEST_ASSERT_FALSE(f.validated);
}

TEST_CASE("parse: malformed JSON and structural violations", "[weather_open_meteo]")
{
    /* not JSON at all */
    (void)snprintf(s_body, sizeof(s_body), "%s", "<html>nope</html>");
    expect_reject(WEATHER_PROVIDER_ERR_JSON_INVALID);

    /* truncated object */
    (void)snprintf(s_body, sizeof(s_body), "%s", "{\"timezone\":\"Europe/Bru");
    expect_reject(WEATHER_PROVIDER_ERR_JSON_INVALID);

    /* trailing non-whitespace garbage after a complete value */
    (void)body_default();
    {
        size_t n = strlen(s_body);
        s_body[n] = 'X';
        s_body[n + 1u] = '\0';
    }
    expect_reject(WEATHER_PROVIDER_ERR_JSON_INVALID);

    /* trailing WHITESPACE, by contrast, is accepted */
    {
        WeatherForecast f;
        (void)body_default();
        {
            size_t n = strlen(s_body);
            s_body[n] = '\n';
            s_body[n + 1u] = ' ';
            s_body[n + 2u] = '\0';
        }
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, parse_current(&f));
    }

    /* a non-object root */
    (void)snprintf(s_body, sizeof(s_body), "%s", "[1,2,3]");
    expect_reject(WEATHER_PROVIDER_ERR_JSON_INVALID);

    /* a provider error payload delivered with a 2xx */
    (void)snprintf(s_body, sizeof(s_body), "%s",
                   "{\"error\":true,\"reason\":\"bad request\"}");
    expect_reject(WEATHER_PROVIDER_ERR_SCHEMA_INVALID);
}

TEST_CASE("parse: timezone framing must be exactly Europe/Brussels",
          "[weather_open_meteo]")
{
    /* a different zone is never silently accepted */
    (void)body_of(DEFAULT_OFFSET, "Europe/Berlin", DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS, DEFAULT_DAILY, DEFAULT_CURRENT);
    expect_reject(WEATHER_PROVIDER_ERR_TIMEZONE_INVALID);

    /* GMT/auto responses (what the API returns when timezone is omitted) */
    (void)body_of("0", "GMT", "\"timezone_abbreviation\":\"GMT\",",
                  DEFAULT_DAILY_UNITS, DEFAULT_DAILY, DEFAULT_CURRENT);
    expect_reject(WEATHER_PROVIDER_ERR_TIMEZONE_INVALID);

    /* missing timezone field */
    (void)snprintf(s_body, sizeof(s_body), "%s",
                   "{\"utc_offset_seconds\":7200,\"daily_units\":{"
                   DEFAULT_DAILY_UNITS "},\"daily\":{" DEFAULT_DAILY "}}");
    expect_reject(WEATHER_PROVIDER_ERR_TIMEZONE_INVALID);

    /* an abbreviation outside the Brussels pair */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, "\"timezone_abbreviation\":\"EST\",",
                  DEFAULT_DAILY_UNITS, DEFAULT_DAILY, DEFAULT_CURRENT);
    expect_reject(WEATHER_PROVIDER_ERR_TIMEZONE_INVALID);

    /* an offset that is neither CET nor CEST */
    (void)body_of("0", DEFAULT_TZ, DEFAULT_ABBREV, DEFAULT_DAILY_UNITS,
                  DEFAULT_DAILY, DEFAULT_CURRENT);
    expect_reject(WEATHER_PROVIDER_ERR_TIMEZONE_INVALID);
    (void)body_of("10800", DEFAULT_TZ, DEFAULT_ABBREV, DEFAULT_DAILY_UNITS,
                  DEFAULT_DAILY, DEFAULT_CURRENT);
    expect_reject(WEATHER_PROVIDER_ERR_TIMEZONE_INVALID);

    /* a numeric-looking STRING offset is not a number */
    (void)body_of("\"7200\"", DEFAULT_TZ, DEFAULT_ABBREV, DEFAULT_DAILY_UNITS,
                  DEFAULT_DAILY, DEFAULT_CURRENT);
    expect_reject(WEATHER_PROVIDER_ERR_TIMEZONE_INVALID);

    /* a missing offset field */
    (void)snprintf(s_body, sizeof(s_body), "%s",
                   "{\"timezone\":\"Europe/Brussels\",\"daily_units\":{"
                   DEFAULT_DAILY_UNITS "},\"daily\":{" DEFAULT_DAILY "}}");
    expect_reject(WEATHER_PROVIDER_ERR_TIMEZONE_INVALID);
}

TEST_CASE("parse: units must be Celsius and the schema must be exact",
          "[weather_open_meteo]")
{
    /* Fahrenheit (or any other unit string) is rejected — never converted */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  "\"temperature_2m_max\":\"\xC2\xB0" "F\"",
                  DEFAULT_DAILY, "");
    expect_reject(WEATHER_PROVIDER_ERR_UNITS_INVALID);

    /* an empty daily_units object */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV, "",
                  DEFAULT_DAILY, "");
    expect_reject(WEATHER_PROVIDER_ERR_UNITS_INVALID);

    /* missing daily / daily_units blocks */
    (void)snprintf(s_body, sizeof(s_body), "%s",
                   "{\"utc_offset_seconds\":7200,\"timezone\":\"Europe/Brussels\","
                   "\"daily_units\":{" DEFAULT_DAILY_UNITS "}}");
    expect_reject(WEATHER_PROVIDER_ERR_SCHEMA_INVALID);
    (void)snprintf(s_body, sizeof(s_body), "%s",
                   "{\"utc_offset_seconds\":7200,\"timezone\":\"Europe/Brussels\","
                   "\"daily\":{" DEFAULT_DAILY "}}");
    expect_reject(WEATHER_PROVIDER_ERR_SCHEMA_INVALID);

    /* arrays that are not arrays */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":\"2026-07-15\",\"temperature_2m_max\":[32.4]", "");
    expect_reject(WEATHER_PROVIDER_ERR_SCHEMA_INVALID);
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-15\"],\"temperature_2m_max\":32.4", "");
    expect_reject(WEATHER_PROVIDER_ERR_SCHEMA_INVALID);

    /* mismatched lengths, zero rows and ambiguous multi-row payloads */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-15\",\"2026-07-16\"],"
                  "\"temperature_2m_max\":[32.4]", "");
    expect_reject(WEATHER_PROVIDER_ERR_SCHEMA_INVALID);
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[],\"temperature_2m_max\":[]", "");
    expect_reject(WEATHER_PROVIDER_ERR_SCHEMA_INVALID);
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-15\",\"2026-07-16\"],"
                  "\"temperature_2m_max\":[32.4,33.1]", "");
    expect_reject(WEATHER_PROVIDER_ERR_SCHEMA_INVALID);
}

TEST_CASE("parse: dates must be well formed AND be today's expected date",
          "[weather_open_meteo]")
{
    /* malformed shapes */
    static const char *bad_dates[] = {
        "2026-7-15", "26-07-15", "2026/07/15", "2026-07-15T00:00",
        "2026-13-01", "2026-02-30", "20260715", "", "abcd-ef-gh"
    };
    size_t i;
    char daily[160];

    for (i = 0; i < sizeof(bad_dates) / sizeof(bad_dates[0]); i++) {
        (void)snprintf(daily, sizeof(daily),
                       "\"time\":[\"%s\"],\"temperature_2m_max\":[32.4]",
                       bad_dates[i]);
        (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                      DEFAULT_DAILY_UNITS, daily, "");
        expect_reject(WEATHER_PROVIDER_ERR_DATE_INVALID);
    }

    /* a non-string date entry */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[20260715],\"temperature_2m_max\":[32.4]", "");
    expect_reject(WEATHER_PROVIDER_ERR_DATE_INVALID);

    /* yesterday's and tomorrow's rows are both refused: a wrong-date
     * forecast can never be accepted as today's */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-14\"],\"temperature_2m_max\":[32.4]", "");
    expect_reject(WEATHER_PROVIDER_ERR_DATE_INVALID);
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-16\"],\"temperature_2m_max\":[32.4]", "");
    expect_reject(WEATHER_PROVIDER_ERR_DATE_INVALID);
}

TEST_CASE("parse: values must be finite numbers inside the sanity band",
          "[weather_open_meteo]")
{
    static const char *bad_values[] = {
        "null", "\"32.4\"", "true", "{}", "[]", "700", "-700", "1e12", "-1e12"
    };
    size_t i;
    char daily[160];

    for (i = 0; i < sizeof(bad_values) / sizeof(bad_values[0]); i++) {
        (void)snprintf(daily, sizeof(daily),
                       "\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[%s]",
                       bad_values[i]);
        (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                      DEFAULT_DAILY_UNITS, daily, "");
        expect_reject(WEATHER_PROVIDER_ERR_VALUE_INVALID);
    }

    /* just outside the band on either side */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[60.1]", "");
    expect_reject(WEATHER_PROVIDER_ERR_VALUE_INVALID);
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS,
                  "\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[-60.1]", "");
    expect_reject(WEATHER_PROVIDER_ERR_VALUE_INVALID);
}

TEST_CASE("parse: a present-but-broken current block fails closed",
          "[weather_open_meteo]")
{
    /* current present without its units block */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS, DEFAULT_DAILY,
                  ",\"current\":{\"temperature_2m\":28.1}");
    expect_reject(WEATHER_PROVIDER_ERR_UNITS_INVALID);

    /* current present with the wrong unit */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS, DEFAULT_DAILY,
                  ",\"current_units\":{\"temperature_2m\":\"\xC2\xB0" "F\"},"
                  "\"current\":{\"temperature_2m\":28.1}");
    expect_reject(WEATHER_PROVIDER_ERR_UNITS_INVALID);

    /* current present with a non-numeric or out-of-band value */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS, DEFAULT_DAILY,
                  ",\"current_units\":{\"temperature_2m\":\"" DEG_C "\"},"
                  "\"current\":{\"temperature_2m\":\"28.1\"}");
    expect_reject(WEATHER_PROVIDER_ERR_VALUE_INVALID);
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS, DEFAULT_DAILY,
                  ",\"current_units\":{\"temperature_2m\":\"" DEG_C "\"},"
                  "\"current\":{\"temperature_2m\":900}");
    expect_reject(WEATHER_PROVIDER_ERR_VALUE_INVALID);

    /* current present but not an object */
    (void)body_of(DEFAULT_OFFSET, DEFAULT_TZ, DEFAULT_ABBREV,
                  DEFAULT_DAILY_UNITS, DEFAULT_DAILY, ",\"current\":42");
    expect_reject(WEATHER_PROVIDER_ERR_SCHEMA_INVALID);
}

TEST_CASE("parse: an untrusted fetch epoch is carried, never invented",
          "[weather_open_meteo]")
{
    WeatherForecast f;
    WeatherParseContext c = ctx_default();
    size_t n = body_default();

    c.fetch_epoch_trusted = false;
    c.fetch_epoch_s = 0ull;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_parse((const uint8_t *)s_body, n, &c, &f));
    /* Technically well-formed, but the untrusted epoch is preserved so the
     * shared usability rule (and the W1 bridge) can refuse it. */
    TEST_ASSERT_TRUE(f.validated);
    TEST_ASSERT_FALSE(f.fetch_epoch_trusted);
    TEST_ASSERT_EQUAL_UINT64(0ull, f.fetch_epoch_s);
    {
        WeatherLocalDate today = d(2026, 7, 15);
        TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                                  &today, FETCH_EPOCH, true, 21600));
    }
}
