/*
 * Deterministic tests for the Gate W5 private source policy.
 *
 * Everything here is pure: no NVS, no networking, no tasks, no clock, no
 * transport. The coordinates used are SYNTHETIC values chosen to exercise
 * boundaries (and deliberately not any real deployment site); no test in
 * this file or any other may contain a real private location.
 */

#include <string.h>
#include "unity.h"
#include "nx_weather_source.h"

/* Synthetic in-range coordinates. Not a real site: a round 1.0/2.0 degrees. */
#define SYN_LAT_E4  10000
#define SYN_LON_E4  20000

/* ---------------- fixtures ---------------- */

/* The complete, valid, recommendation-only configuration. */
static void make_ready(NxWeatherSourceConfig *c)
{
    nx_weather_source_defaults(c);
    c->enabled             = true;
    c->distribution        = NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL;
    c->provider            = WEATHER_PROVIDER_OPEN_METEO;
    c->latitude_e4         = SYN_LAT_E4;
    c->longitude_e4        = SYN_LON_E4;
    c->timezone            = WEATHER_TZ_EUROPE_BRUSSELS;
    c->recommendation_only = true;
    weather_schedule_config_defaults(&c->schedule);
}

/* ---------------- A. configuration validation ---------------- */

TEST_CASE("w5: the fixture itself is a valid ready configuration", "[nx_wx_src]")
{
    /* Guards against the whole suite passing for the wrong reason: if the
     * fixture were invalid, every "rejected" assertion below would hold
     * trivially. */
    NxWeatherSourceConfig c;
    make_ready(&c);
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                      nx_weather_source_validate(&c));
    TEST_ASSERT_TRUE(nx_weather_source_ready(nx_weather_source_validate(&c)));
}

TEST_CASE("w5: defaults are unconfigured and permit nothing", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    nx_weather_source_defaults(&c);
    TEST_ASSERT_FALSE(c.enabled);
    TEST_ASSERT_EQUAL(NX_WEATHER_DIST_UNSPECIFIED, c.distribution);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_UNCONFIGURED, c.provider);
    TEST_ASSERT_EQUAL_INT32(0, c.latitude_e4);
    TEST_ASSERT_EQUAL_INT32(0, c.longitude_e4);
    TEST_ASSERT_EQUAL(WEATHER_TZ_UNSPECIFIED, c.timezone);
    TEST_ASSERT_TRUE(c.recommendation_only);
    TEST_ASSERT_EQUAL(NX_WX_SRC_UNCONFIGURED, nx_weather_source_validate(&c));
    TEST_ASSERT_FALSE(nx_weather_source_ready(nx_weather_source_validate(&c)));
}

TEST_CASE("w5: a zeroed or NULL configuration fails closed", "[nx_wx_src]")
{
    NxWeatherSourceConfig zero;

    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL(NX_WX_SRC_UNCONFIGURED, nx_weather_source_validate(&zero));
    TEST_ASSERT_EQUAL(NX_WX_SRC_UNCONFIGURED, nx_weather_source_validate(NULL));
}

TEST_CASE("w5: the build-time binder defaults to unconfigured", "[nx_wx_src]")
{
    /* The repository's own defaults must never produce a usable source. In
     * the test build no W5 Kconfig symbol is set, so this is the shipped
     * posture. */
    NxWeatherSourceConfig c;

    nx_weather_source_from_build_config(&c);
    TEST_ASSERT_EQUAL(NX_WX_SRC_UNCONFIGURED, nx_weather_source_validate(&c));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_UNCONFIGURED, c.provider);
    TEST_ASSERT_EQUAL_INT32(0, c.latitude_e4);
    TEST_ASSERT_EQUAL_INT32(0, c.longitude_e4);
}

TEST_CASE("w5: only OWNER_MANAGED_EXTERNAL permits a provider", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    make_ready(&c);
    c.distribution = NX_WEATHER_DIST_UNSPECIFIED;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_DISTRIBUTION,
                      nx_weather_source_validate(&c));
    c.distribution = NX_WEATHER_DIST_DISABLED;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_DISTRIBUTION,
                      nx_weather_source_validate(&c));
    c.distribution = (NxWeatherDistribution)99;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_DISTRIBUTION,
                      nx_weather_source_validate(&c));

    /* The permission predicate agrees, for every mode. */
    TEST_ASSERT_TRUE(nx_weather_distribution_permits(
        NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL, WEATHER_PROVIDER_OPEN_METEO));
    TEST_ASSERT_FALSE(nx_weather_distribution_permits(
        NX_WEATHER_DIST_UNSPECIFIED, WEATHER_PROVIDER_OPEN_METEO));
    TEST_ASSERT_FALSE(nx_weather_distribution_permits(
        NX_WEATHER_DIST_DISABLED, WEATHER_PROVIDER_OPEN_METEO));
    TEST_ASSERT_FALSE(nx_weather_distribution_permits(
        (NxWeatherDistribution)99, WEATHER_PROVIDER_OPEN_METEO));
}

TEST_CASE("w5: the distribution mapping defers to the committed W3 gate", "[nx_wx_src]")
{
    /* W5 never widens W3: OWNER_MANAGED_EXTERNAL maps onto the one committed
     * licence class that permits the public endpoint, everything else onto
     * the committed UNSPECIFIED that W3 already refuses. */
    TEST_ASSERT_EQUAL(WEATHER_DISTRIBUTION_PERSONAL_NONCOMMERCIAL,
        nx_weather_distribution_to_w3(NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL));
    TEST_ASSERT_EQUAL(WEATHER_DISTRIBUTION_UNSPECIFIED,
        nx_weather_distribution_to_w3(NX_WEATHER_DIST_UNSPECIFIED));
    TEST_ASSERT_EQUAL(WEATHER_DISTRIBUTION_UNSPECIFIED,
        nx_weather_distribution_to_w3(NX_WEATHER_DIST_DISABLED));
    TEST_ASSERT_EQUAL(WEATHER_DISTRIBUTION_UNSPECIFIED,
        nx_weather_distribution_to_w3((NxWeatherDistribution)99));

    /* And the committed function is the authority that says yes/no. */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
        weather_provider_eligible(WEATHER_PROVIDER_OPEN_METEO,
            nx_weather_distribution_to_w3(NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL)));
    TEST_ASSERT_NOT_EQUAL(WEATHER_PROVIDER_OK,
        weather_provider_eligible(WEATHER_PROVIDER_OPEN_METEO,
            nx_weather_distribution_to_w3(NX_WEATHER_DIST_DISABLED)));
}

TEST_CASE("w5: a provider must be explicitly selected and supported", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    make_ready(&c);
    c.provider = WEATHER_PROVIDER_UNCONFIGURED;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_PROVIDER, nx_weather_source_validate(&c));
    c.provider = (WeatherProviderId)WEATHER_PROVIDER__COUNT;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_PROVIDER, nx_weather_source_validate(&c));
    c.provider = (WeatherProviderId)77;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_PROVIDER, nx_weather_source_validate(&c));

    /* W5 supports exactly one provider and adds none. */
    TEST_ASSERT_EQUAL(2, (int)WEATHER_PROVIDER__COUNT);
}

TEST_CASE("w5: latitude boundaries accept the extremes and reject beyond", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    make_ready(&c);
    c.latitude_e4 = NX_WEATHER_LAT_E4_MAX;
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                      nx_weather_source_validate(&c));
    c.latitude_e4 = -NX_WEATHER_LAT_E4_MAX;
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                      nx_weather_source_validate(&c));
    c.latitude_e4 = NX_WEATHER_LAT_E4_MAX + 1;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_LATITUDE, nx_weather_source_validate(&c));
    c.latitude_e4 = -NX_WEATHER_LAT_E4_MAX - 1;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_LATITUDE, nx_weather_source_validate(&c));
    c.latitude_e4 = INT32_MAX;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_LATITUDE, nx_weather_source_validate(&c));
    c.latitude_e4 = INT32_MIN;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_LATITUDE, nx_weather_source_validate(&c));
}

TEST_CASE("w5: longitude boundaries accept the extremes and reject beyond", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    make_ready(&c);
    c.longitude_e4 = NX_WEATHER_LON_E4_MAX;
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                      nx_weather_source_validate(&c));
    c.longitude_e4 = -NX_WEATHER_LON_E4_MAX;
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                      nx_weather_source_validate(&c));
    c.longitude_e4 = NX_WEATHER_LON_E4_MAX + 1;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_LONGITUDE, nx_weather_source_validate(&c));
    c.longitude_e4 = -NX_WEATHER_LON_E4_MAX - 1;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_LONGITUDE, nx_weather_source_validate(&c));
    c.longitude_e4 = INT32_MAX;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_LONGITUDE, nx_weather_source_validate(&c));
}

TEST_CASE("w5: the zero/zero unset sentinel is never a location", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    /* 0/0 is a real point at sea, but the repository reserves it as "unset",
     * so an unconfigured device can never request a location nobody chose. */
    make_ready(&c);
    c.latitude_e4  = 0;
    c.longitude_e4 = 0;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INCOMPLETE, nx_weather_source_validate(&c));
}

TEST_CASE("w5: a half-supplied coordinate pair is incomplete", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    make_ready(&c);
    c.latitude_e4 = 0;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INCOMPLETE, nx_weather_source_validate(&c));

    make_ready(&c);
    c.longitude_e4 = 0;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INCOMPLETE, nx_weather_source_validate(&c));
}

TEST_CASE("w5: a timezone must be explicitly selected", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    make_ready(&c);
    c.timezone = WEATHER_TZ_UNSPECIFIED;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_TIMEZONE, nx_weather_source_validate(&c));
    c.timezone = (WeatherTimezoneId)WEATHER_TZ__COUNT;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_TIMEZONE, nx_weather_source_validate(&c));
    c.timezone = (WeatherTimezoneId)55;
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_TIMEZONE, nx_weather_source_validate(&c));
}

TEST_CASE("w5: an invalid schedule is rejected", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    make_ready(&c);
    c.schedule.enabled    = true;
    c.schedule.slot_count = 0u;   /* enabled but no slot: invalid in W3 */
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_SCHEDULE, nx_weather_source_validate(&c));
}

TEST_CASE("w5: a configuration that is not recommendation-only is refused", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    make_ready(&c);
    c.recommendation_only = false;
    TEST_ASSERT_EQUAL(NX_WX_SRC_NOT_RECOMMENDATION_ONLY,
                      nx_weather_source_validate(&c));
    TEST_ASSERT_FALSE(nx_weather_source_ready(nx_weather_source_validate(&c)));
}

TEST_CASE("w5: a disabled feature short-circuits every other field", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;

    make_ready(&c);
    c.enabled = false;
    TEST_ASSERT_EQUAL(NX_WX_SRC_UNCONFIGURED, nx_weather_source_validate(&c));
}

/* ---------------- projection onto the W4 runtime ---------------- */

TEST_CASE("w5: a failed configuration yields the W4 safe posture", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;
    WeatherRuntimeConfig  rc;

    /* Every failure path must leave a runtime configuration that cannot
     * request anything, with no residue of the attempted values. */
    make_ready(&c);
    c.provider = WEATHER_PROVIDER_UNCONFIGURED;
    memset(&rc, 0xAA, sizeof(rc));
    TEST_ASSERT_EQUAL(NX_WX_SRC_INVALID_PROVIDER,
                      nx_weather_source_to_runtime(&c, &rc));
    TEST_ASSERT_FALSE(rc.enabled);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_UNCONFIGURED, rc.expected_provider);
    TEST_ASSERT_EQUAL_INT32(0, rc.location.latitude_e4);
    TEST_ASSERT_EQUAL_INT32(0, rc.location.longitude_e4);

    /* Unconfigured input: same safe posture. */
    nx_weather_source_defaults(&c);
    memset(&rc, 0x55, sizeof(rc));
    TEST_ASSERT_EQUAL(NX_WX_SRC_UNCONFIGURED,
                      nx_weather_source_to_runtime(&c, &rc));
    TEST_ASSERT_FALSE(rc.enabled);
    TEST_ASSERT_EQUAL_INT32(0, rc.location.latitude_e4);
    TEST_ASSERT_EQUAL_INT32(0, rc.location.longitude_e4);

    /* NULL output is refused without a crash. */
    TEST_ASSERT_EQUAL(NX_WX_SRC_UNCONFIGURED,
                      nx_weather_source_to_runtime(&c, NULL));
}

TEST_CASE("w5: a ready configuration projects exactly onto the W4 runtime", "[nx_wx_src]")
{
    NxWeatherSourceConfig c;
    WeatherRuntimeConfig  rc;
    WeatherRuntimeConfig  w4_defaults;

    weather_runtime_config_defaults(&w4_defaults);
    make_ready(&c);
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                      nx_weather_source_to_runtime(&c, &rc));

    TEST_ASSERT_TRUE(rc.enabled);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OPEN_METEO, rc.expected_provider);
    TEST_ASSERT_EQUAL_INT32(SYN_LAT_E4, rc.location.latitude_e4);
    TEST_ASSERT_EQUAL_INT32(SYN_LON_E4, rc.location.longitude_e4);
    TEST_ASSERT_EQUAL(WEATHER_TZ_EUROPE_BRUSSELS, rc.location.timezone);

    /* W5 decides WHETHER and WHERE only: the runtime's own freshness and
     * retry policy keep the committed W4 defaults. */
    TEST_ASSERT_EQUAL_UINT32(w4_defaults.max_sync_age_s, rc.max_sync_age_s);
    TEST_ASSERT_EQUAL_UINT32(w4_defaults.max_forecast_age_s, rc.max_forecast_age_s);
    TEST_ASSERT_EQUAL_UINT8(w4_defaults.max_attempts, rc.max_attempts);

    /* And the projected configuration is one the committed runtime accepts. */
    {
        WeatherRuntime rt;
        TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK,
                          weather_runtime_init(&rt, &rc, NULL));
    }
}

TEST_CASE("w5: status tokens are stable, unique and value-free", "[nx_wx_src]")
{
    int i, j;

    for (i = 0; i < (int)NX_WX_SRC__COUNT; i++) {
        const char *t = nx_weather_source_status_str((NxWeatherSourceStatus)i);
        TEST_ASSERT_NOT_NULL(t);
        /* A token may never carry a number: that is how a coordinate would
         * leak into a diagnostic. */
        for (const char *p = t; *p != '\0'; p++) {
            TEST_ASSERT_TRUE(*p < '0' || *p > '9');
        }
        for (j = 0; j < i; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(t,
                nx_weather_source_status_str((NxWeatherSourceStatus)j)));
        }
    }
    /* Out-of-range maps to the fail-closed token. */
    TEST_ASSERT_EQUAL_STRING("WX_SRC_UNCONFIGURED",
                             nx_weather_source_status_str((NxWeatherSourceStatus)99));
    TEST_ASSERT_EQUAL_STRING("WX_SRC_UNCONFIGURED",
                             nx_weather_source_status_str(NX_WX_SRC__COUNT));
}

TEST_CASE("w5: only READY reports ready", "[nx_wx_src]")
{
    int i;

    for (i = 0; i < (int)NX_WX_SRC__COUNT; i++) {
        bool ready = nx_weather_source_ready((NxWeatherSourceStatus)i);
        TEST_ASSERT_EQUAL(i == (int)NX_WX_SRC_READY_RECOMMENDATION_ONLY, ready);
    }
}

TEST_CASE("w5: validation is deterministic and side-effect free", "[nx_wx_src]")
{
    NxWeatherSourceConfig c, before;
    int i;

    make_ready(&c);
    before = c;
    for (i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                          nx_weather_source_validate(&c));
    }
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof(c));
}
