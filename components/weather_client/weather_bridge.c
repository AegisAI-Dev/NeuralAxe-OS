/*
 * NeuralAxe Weather-Aware Tuning — pure bridge into the W1 climate-policy
 * input (Gate W3). PURE: no I/O, no clocks, no tuning application, no
 * profile knowledge — the output is exactly one TuningForecastInput.
 */

#include <string.h>
#include "weather_bridge.h"

void weather_bridge_to_policy(const WeatherForecast *forecast,
                              WeatherProviderResult provider_result,
                              WeatherProviderId expected_provider,
                              const WeatherLocalDate *today,
                              uint64_t now_epoch_s, bool now_trusted,
                              uint32_t max_age_s,
                              TuningForecastInput *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->status = TUNING_FORECAST_UNAVAILABLE; /* fail-safe default */
    out->forecast_max_dc = 0;

    /* Untrusted time: the forecast's age/date framing is unverifiable —
     * always the fail-safe stance, never an upgrade-capable input. */
    if (!now_trusted) {
        out->status = TUNING_FORECAST_STALE;
        return;
    }

    if (provider_result != WEATHER_PROVIDER_OK || forecast == NULL) {
        switch (provider_result) {
        case WEATHER_PROVIDER_ERR_JSON_INVALID:
        case WEATHER_PROVIDER_ERR_SCHEMA_INVALID:
        case WEATHER_PROVIDER_ERR_UNITS_INVALID:
        case WEATHER_PROVIDER_ERR_TIMEZONE_INVALID:
        case WEATHER_PROVIDER_ERR_VALUE_INVALID:
            out->status = TUNING_FORECAST_INVALID;
            return;
        case WEATHER_PROVIDER_ERR_DATE_INVALID:
        case WEATHER_PROVIDER_ERR_CACHE_STALE:
            out->status = TUNING_FORECAST_STALE;
            return;
        default:
            /* Transport, eligibility, rate/retry and internal failures:
             * weather unavailable. Never an upgrade direction — W1 maps
             * every non-OK status to the fail-safe hot stance. */
            out->status = TUNING_FORECAST_UNAVAILABLE;
            return;
        }
    }

    /* Defense in depth: even an OK result must pass the full usability
     * rule (validated, expected provider, Brussels tz, TODAY's date,
     * trusted + fresh fetch, sanity band). */
    if (!weather_forecast_usable(forecast, expected_provider, today,
                                 now_epoch_s, now_trusted, max_age_s)) {
        out->status = TUNING_FORECAST_STALE;
        return;
    }

    /* The forecast MAXIMUM is the only weather fact policy consumes; the
     * current outdoor temperature is deliberately ignored here. */
    out->status = TUNING_FORECAST_OK;
    out->forecast_max_dc = forecast->forecast_max_dc;
}
