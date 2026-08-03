/*
 * Deterministic tests for the provider-neutral forecast model, the shared
 * usability rule, the cache and the instrumentation contract (W3).
 * Synthetic values only; no clocks, no network.
 */

#include <string.h>
#include "unity.h"
#include "weather_forecast.h"
#include "weather_cache.h"

#define EPOCH_S 1785000000ull /* synthetic trusted epoch (2026-07) */

static WeatherLocalDate d(uint16_t y, uint8_t m, uint8_t dd)
{
    WeatherLocalDate x = { y, m, dd };
    return x;
}

static WeatherForecast valid_forecast(void)
{
    WeatherForecast f;
    weather_forecast_init(&f);
    f.provider = WEATHER_PROVIDER_OPEN_METEO;
    f.local_date = d(2026, 7, 15);
    f.forecast_max_dc = 324;
    f.current_valid = true;
    f.current_dc = 281;
    f.timezone = WEATHER_TZ_EUROPE_BRUSSELS;
    f.utc_offset_s = 7200;
    f.fetch_epoch_s = EPOCH_S;
    f.fetch_epoch_trusted = true;
    f.source_generation = 3;
    f.validated = true;
    return f;
}

TEST_CASE("forecast: init is zero, unconfigured and unvalidated", "[weather_forecast]")
{
    WeatherForecast f;
    memset(&f, 0xFF, sizeof(f));
    weather_forecast_init(&f);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_UNCONFIGURED, f.provider);
    TEST_ASSERT_EQUAL(WEATHER_TZ_UNSPECIFIED, f.timezone);
    TEST_ASSERT_FALSE(f.validated);
    TEST_ASSERT_FALSE(f.current_valid);
    TEST_ASSERT_FALSE(f.fetch_epoch_trusted);
    weather_forecast_init(NULL); /* must not crash */
}

TEST_CASE("forecast: usability rule — every condition individually gates", "[weather_forecast]")
{
    WeatherLocalDate today = d(2026, 7, 15);
    WeatherForecast f = valid_forecast();
    TEST_ASSERT_TRUE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                             &today, EPOCH_S + 100, true, 21600));

    TEST_ASSERT_FALSE(weather_forecast_usable(NULL, WEATHER_PROVIDER_OPEN_METEO,
                                              &today, EPOCH_S, true, 21600));
    TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                              NULL, EPOCH_S, true, 21600));

    f = valid_forecast();
    f.validated = false;
    TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                              &today, EPOCH_S, true, 21600));
    /* expected provider unconfigured or mismatched */
    f = valid_forecast();
    TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_UNCONFIGURED,
                                              &today, EPOCH_S, true, 21600));
    f.provider = WEATHER_PROVIDER_UNCONFIGURED;
    TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                              &today, EPOCH_S, true, 21600));
    /* wrong timezone */
    f = valid_forecast();
    f.timezone = WEATHER_TZ_UNSPECIFIED;
    TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                              &today, EPOCH_S, true, 21600));
    /* yesterday's forecast can NEVER inform today */
    f = valid_forecast();
    f.local_date = d(2026, 7, 14);
    TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                              &today, EPOCH_S, true, 21600));
    /* untrusted fetch or untrusted now: age unverifiable */
    f = valid_forecast();
    f.fetch_epoch_trusted = false;
    TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                              &today, EPOCH_S, true, 21600));
    f = valid_forecast();
    TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                              &today, EPOCH_S, false, 21600));
    /* future fetch epoch */
    f = valid_forecast();
    TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                              &today, EPOCH_S - 1, true, 21600));
    /* age boundary: exactly max age passes; one second more fails */
    f = valid_forecast();
    TEST_ASSERT_TRUE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                             &today, EPOCH_S + 21600, true, 21600));
    TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                              &today, EPOCH_S + 21601, true, 21600));
    /* sanity band */
    f = valid_forecast();
    f.forecast_max_dc = WEATHER_TEMP_SANITY_MAX_DC + 1;
    TEST_ASSERT_FALSE(weather_forecast_usable(&f, WEATHER_PROVIDER_OPEN_METEO,
                                              &today, EPOCH_S, true, 21600));
}

TEST_CASE("cache: store/usable/invalidate life cycle", "[weather_forecast]")
{
    WeatherForecastCache c;
    WeatherLocalDate today = d(2026, 7, 15);
    WeatherForecast f = valid_forecast();

    weather_cache_init(&c);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CACHE_STALE,
                      weather_cache_usable(&c, WEATHER_PROVIDER_OPEN_METEO,
                                           &today, EPOCH_S, true, 21600));

    /* only validated + trusted-fetch forecasts are storable */
    f.validated = false;
    TEST_ASSERT_FALSE(weather_cache_store(&c, &f));
    f = valid_forecast();
    f.fetch_epoch_trusted = false;
    TEST_ASSERT_FALSE(weather_cache_store(&c, &f));
    f = valid_forecast();
    TEST_ASSERT_TRUE(weather_cache_store(&c, &f));

    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_cache_usable(&c, WEATHER_PROVIDER_OPEN_METEO,
                                           &today, EPOCH_S + 60, true, 21600));
    /* same-day, exact max age */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_cache_usable(&c, WEATHER_PROVIDER_OPEN_METEO,
                                           &today, EPOCH_S + 21600, true, 21600));
    /* over age */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CACHE_STALE,
                      weather_cache_usable(&c, WEATHER_PROVIDER_OPEN_METEO,
                                           &today, EPOCH_S + 21601, true, 21600));
    /* previous local date can never authorize anything the next day */
    {
        WeatherLocalDate tomorrow = d(2026, 7, 16);
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CACHE_STALE,
                          weather_cache_usable(&c, WEATHER_PROVIDER_OPEN_METEO,
                                               &tomorrow, EPOCH_S + 60, true, 21600));
    }
    /* untrusted now: unverifiable age */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CACHE_STALE,
                      weather_cache_usable(&c, WEATHER_PROVIDER_OPEN_METEO,
                                           &today, EPOCH_S + 60, false, 21600));
    /* provider mismatch */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CACHE_STALE,
                      weather_cache_usable(&c, WEATHER_PROVIDER_UNCONFIGURED,
                                           &today, EPOCH_S + 60, true, 21600));
    /* future fetch epoch */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CACHE_STALE,
                      weather_cache_usable(&c, WEATHER_PROVIDER_OPEN_METEO,
                                           &today, EPOCH_S - 1, true, 21600));

    /* explicit invalidation removes the entry and bumps the generation */
    {
        uint32_t g = c.invalidation_generation;
        weather_cache_invalidate(&c);
        TEST_ASSERT_FALSE(c.valid);
        TEST_ASSERT_EQUAL_UINT32(g + 1u, c.invalidation_generation);
        TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_CACHE_STALE,
                          weather_cache_usable(&c, WEATHER_PROVIDER_OPEN_METEO,
                                               &today, EPOCH_S + 60, true, 21600));
    }
}

TEST_CASE("stats: pure bookkeeping with monotonic-regression guard", "[weather_forecast]")
{
    WeatherFetchStats s;
    memset(&s, 0xFF, sizeof(s));
    weather_stats_reset(&s);
    TEST_ASSERT_FALSE(s.valid);
    TEST_ASSERT_EQUAL_UINT32(0, s.response_len);

    weather_stats_begin(&s, 150000u);
    TEST_ASSERT_EQUAL_UINT32(150000u, s.free_internal_before);
    TEST_ASSERT_FALSE(s.valid);

    weather_stats_end(&s, 120000u, 1000ull, 251000ull, 812u, 812u, 2048u);
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_EQUAL_UINT32(250000u, s.duration_us);
    TEST_ASSERT_EQUAL_UINT32(120000u, s.free_internal_after);
    TEST_ASSERT_EQUAL_UINT32(812u, s.response_len);
    TEST_ASSERT_EQUAL_UINT32(2048u, s.stack_high_water);

    /* monotonic regression invalidates the measurement */
    weather_stats_begin(&s, 1u);
    weather_stats_end(&s, 1u, 5000ull, 4000ull, 1u, 1u, 1u);
    TEST_ASSERT_FALSE(s.valid);

    weather_stats_reset(NULL);
    weather_stats_begin(NULL, 0u);
    weather_stats_end(NULL, 0u, 0ull, 0ull, 0u, 0u, 0u); /* no crash */
}
