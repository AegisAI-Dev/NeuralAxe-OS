#ifndef WEATHER_CACHE_H_
#define WEATHER_CACHE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "weather_forecast.h"
#include "weather_provider.h"

/*
 * NeuralAxe Weather-Aware Tuning — bounded provider-neutral forecast
 * cache (Gate W3).
 *
 * IN-MEMORY, VERSION-READY model: W3 does not persist it (the committed
 * W2 schema has no compatible field); the shape carries an explicit
 * cache_version for a later reviewed persistence contract. A cache may
 * serve status displays and SAFE policy re-evaluation within the same
 * valid date/age window. A cache from a previous local date can NEVER
 * authorize an upgrade, and untrusted time makes the age unverifiable and
 * therefore upgrade-ineligible (both enforced by the shared
 * weather_forecast_usable rule and re-proven by test).
 */

#define WEATHER_CACHE_VERSION 1u

typedef struct {
    uint16_t cache_version;   /* WEATHER_CACHE_VERSION                   */
    bool valid;               /* an entry exists                         */
    WeatherForecast forecast; /* normalized facts only — never raw JSON  */
    uint32_t invalidation_generation; /* bumped by explicit invalidation */
    WeatherProviderResult last_validation; /* result at store time       */
} WeatherForecastCache;

void weather_cache_init(WeatherForecastCache *c);

/* Store a forecast. Only a validated forecast with a trusted fetch epoch
 * is accepted; anything else clears nothing and returns false. */
bool weather_cache_store(WeatherForecastCache *c, const WeatherForecast *f);

/* Explicit invalidation (bumps invalidation_generation; entry removed). */
void weather_cache_invalidate(WeatherForecastCache *c);

/*
 * Usability check (delegates to weather_forecast_usable): OK only when an
 * entry exists AND the forecast passes provider/date/trust/age/band
 * checks for `today`/`now`. Every other condition — empty cache, provider
 * mismatch, previous-date entry, future fetch epoch, over-age, untrusted
 * now, invalid forecast — returns WEATHER_PROVIDER_ERR_CACHE_STALE.
 */
WeatherProviderResult weather_cache_usable(const WeatherForecastCache *c,
                                           WeatherProviderId expected_provider,
                                           const WeatherLocalDate *today,
                                           uint64_t now_epoch_s, bool now_trusted,
                                           uint32_t max_age_s);

#endif /* WEATHER_CACHE_H_ */
