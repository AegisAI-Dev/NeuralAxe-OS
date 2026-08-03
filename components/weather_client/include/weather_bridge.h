#ifndef WEATHER_BRIDGE_H_
#define WEATHER_BRIDGE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "weather_forecast.h"
#include "weather_provider.h"
#include "tuning_policy.h"

/*
 * NeuralAxe Weather-Aware Tuning — pure bridge from validated weather
 * facts into the committed W1 climate-policy input (Gate W3).
 *
 * The bridge outputs ONLY a TuningForecastInput. It never inserts a
 * tuning payload, never marks a profile validated, never bypasses W1
 * profile eligibility, and cannot make the production registry produce an
 * automatic selection (all production profiles remain UNVALIDATED and
 * payload-free — re-proven by test through the full W1 chain).
 *
 * Mapping (fail-safe by construction — every failure direction feeds the
 * W1 fail-safe/hot stance, NEVER an upgrade):
 *  - fully usable forecast (validated, expected provider, Brussels tz,
 *    today's trusted local date, trusted+fresh fetch)  -> TUNING_FORECAST_OK
 *    with forecast_max_dc — the ONLY weather fact policy consumes;
 *  - provider fetch failure                            -> UNAVAILABLE;
 *  - technically invalid response                      -> INVALID;
 *  - stale / wrong-date / future-fetch / untrusted-time -> STALE.
 * The optional current outdoor temperature is NEVER consulted (proven by
 * test: varying it cannot change any selection intent).
 */

/* Classify one fetch/cache outcome into the W1 climate input. `forecast`
 * may be NULL (e.g. transport failure); `provider_result` is the fetch or
 * cache result; date/trust inputs mirror weather_forecast_usable. */
void weather_bridge_to_policy(const WeatherForecast *forecast,
                              WeatherProviderResult provider_result,
                              WeatherProviderId expected_provider,
                              const WeatherLocalDate *today,
                              uint64_t now_epoch_s, bool now_trusted,
                              uint32_t max_age_s,
                              TuningForecastInput *out);

#endif /* WEATHER_BRIDGE_H_ */
