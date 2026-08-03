#ifndef WEATHER_OPEN_METEO_H_
#define WEATHER_OPEN_METEO_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "weather_provider.h"

/*
 * NeuralAxe Weather-Aware Tuning — Open-Meteo adapter (Gate W3).
 *
 * ONE fixed allowlisted endpoint identity (compile constants — no
 * arbitrary host, path or URL is representable anywhere):
 *
 *   host: api.open-meteo.com
 *   path: /v1/forecast
 *
 * Query contract (verified against the official documentation on the
 * audit date; the `timezone` parameter is REQUIRED whenever `daily`
 * variables are requested; parameter order is fixed and deterministic):
 *
 *   latitude=<lat>&longitude=<lon>&daily=temperature_2m_max
 *   &current=temperature_2m&timezone=Europe%2FBrussels
 *   &forecast_days=1&temperature_unit=celsius
 *
 * Coordinates are scaled integers (degrees * 1e4) rendered with exactly
 * four decimals by pure integer arithmetic — no float formatting, no
 * locale dependency, no scientific notation; -0.0000 is canonicalized to
 * 0.0000. The request carries NO API key, device identifier, firmware
 * serial, hostname, worker/account, wallet or telemetry of any kind.
 *
 * LICENSING (official terms, audit date): the public endpoint's free tier
 * is NON-COMMERCIAL only (CC-BY 4.0 data licence; attribution "Weather
 * data by Open-Meteo.com" with a visible link). Eligibility is enforced
 * separately (weather_provider_eligible) and the public endpoint stays
 * ineligible under UNSPECIFIED/COMMERCIAL/SELF_HOSTED distribution.
 */

#define WEATHER_OPEN_METEO_HOST "api.open-meteo.com"
#define WEATHER_OPEN_METEO_PATH "/v1/forecast"

/* The provider interface (build_request / parse_response / attribution). */
const WeatherProviderIface *weather_provider_open_meteo(void);

/* Direct entry points (also reachable through the interface): */
WeatherProviderResult weather_open_meteo_build_request(const WeatherRequestParams *params,
                                                       WeatherRequest *out);
WeatherProviderResult weather_open_meteo_parse(const uint8_t *body, size_t body_len,
                                               const WeatherParseContext *ctx,
                                               WeatherForecast *out);

#endif /* WEATHER_OPEN_METEO_H_ */
