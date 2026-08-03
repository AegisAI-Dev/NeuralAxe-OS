/*
 * NeuralAxe Weather-Aware Tuning — bounded forecast cache (Gate W3).
 * PURE: no I/O, no heap, no clocks, no persistence (in-memory,
 * version-ready model).
 */

#include <string.h>
#include "weather_cache.h"

void weather_cache_init(WeatherForecastCache *c)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->cache_version = (uint16_t)WEATHER_CACHE_VERSION;
    weather_forecast_init(&c->forecast);
}

bool weather_cache_store(WeatherForecastCache *c, const WeatherForecast *f)
{
    if (c == NULL || f == NULL) {
        return false;
    }
    if (c->cache_version != (uint16_t)WEATHER_CACHE_VERSION) {
        return false;
    }
    /* Only a fully validated forecast with a trusted fetch epoch may be
     * cached; nothing else mutates the entry. */
    if (!f->validated || !f->fetch_epoch_trusted) {
        return false;
    }
    if (f->provider == WEATHER_PROVIDER_UNCONFIGURED) {
        return false;
    }
    c->forecast = *f;
    c->valid = true;
    c->last_validation = WEATHER_PROVIDER_OK;
    return true;
}

void weather_cache_invalidate(WeatherForecastCache *c)
{
    if (c == NULL) {
        return;
    }
    c->valid = false;
    weather_forecast_init(&c->forecast);
    if (c->invalidation_generation != UINT32_MAX) {
        c->invalidation_generation++;
    }
    c->last_validation = WEATHER_PROVIDER_ERR_CACHE_STALE;
}

WeatherProviderResult weather_cache_usable(const WeatherForecastCache *c,
                                           WeatherProviderId expected_provider,
                                           const WeatherLocalDate *today,
                                           uint64_t now_epoch_s, bool now_trusted,
                                           uint32_t max_age_s)
{
    if (c == NULL || today == NULL) {
        return WEATHER_PROVIDER_ERR_CACHE_STALE;
    }
    if (c->cache_version != (uint16_t)WEATHER_CACHE_VERSION || !c->valid) {
        return WEATHER_PROVIDER_ERR_CACHE_STALE;
    }
    /* The shared usability rule enforces provider match, Brussels
     * timezone, TODAY's local date (a previous-date cache can never
     * authorize anything today), trusted+fresh fetch and the sanity band.
     * Untrusted `now` makes the age unverifiable -> unusable. */
    if (!weather_forecast_usable(&c->forecast, expected_provider, today,
                                 now_epoch_s, now_trusted, max_age_s)) {
        return WEATHER_PROVIDER_ERR_CACHE_STALE;
    }
    return WEATHER_PROVIDER_OK;
}
