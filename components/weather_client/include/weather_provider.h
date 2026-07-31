#ifndef WEATHER_PROVIDER_H_
#define WEATHER_PROVIDER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "weather_forecast.h"

/*
 * NeuralAxe Weather-Aware Tuning — replaceable provider interface,
 * distribution eligibility and attribution metadata (Gate W3).
 *
 * Provider logic never contains profile ids or tuning decisions; policy
 * logic never contains provider-specific JSON field names. The provider
 * default is UNCONFIGURED and Open-Meteo is NEVER an implicit operational
 * default: even a configured Open-Meteo adapter stays INELIGIBLE until an
 * explicit product/distribution policy permits it (the Gate W0 commercial
 * decision is unresolved). No API key exists anywhere in W3.
 */

/* ------------------------------------------------------------------ */
/* Stable provider results (bounded machine codes; sanitized tokens)   */
/* ------------------------------------------------------------------ */

typedef enum {
    WEATHER_PROVIDER_OK = 0,
    WEATHER_PROVIDER_ERR_UNCONFIGURED,
    WEATHER_PROVIDER_ERR_INELIGIBLE_DISTRIBUTION,
    WEATHER_PROVIDER_ERR_REQUEST_INVALID,
    WEATHER_PROVIDER_ERR_TLS_FAILURE,
    WEATHER_PROVIDER_ERR_DNS_FAILURE,
    WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT,
    WEATHER_PROVIDER_ERR_READ_TIMEOUT,
    WEATHER_PROVIDER_ERR_HTTP_STATUS,
    WEATHER_PROVIDER_ERR_REDIRECT_REJECTED,
    WEATHER_PROVIDER_ERR_CONTENT_TYPE,
    WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE,
    WEATHER_PROVIDER_ERR_TRUNCATED,
    WEATHER_PROVIDER_ERR_JSON_INVALID,
    WEATHER_PROVIDER_ERR_SCHEMA_INVALID,
    WEATHER_PROVIDER_ERR_UNITS_INVALID,
    WEATHER_PROVIDER_ERR_TIMEZONE_INVALID,
    WEATHER_PROVIDER_ERR_DATE_INVALID,
    WEATHER_PROVIDER_ERR_VALUE_INVALID,
    WEATHER_PROVIDER_ERR_CACHE_STALE,
    WEATHER_PROVIDER_ERR_RATE_LIMITED,
    WEATHER_PROVIDER_ERR_RETRY_EXHAUSTED,
    WEATHER_PROVIDER_ERR_INTERNAL,
    WEATHER_PROVIDER_RESULT__COUNT
} WeatherProviderResult;

/* ------------------------------------------------------------------ */
/* Distribution eligibility (Stage 4)                                  */
/* ------------------------------------------------------------------ */

/*
 * The unresolved product/distribution decision, modeled explicitly. W3
 * does not persist it (the committed W2 schema has no such field); it is
 * a future product/runtime configuration input — wiring it is a W4/W5
 * prerequisite. UNSPECIFIED (0) is the default and blocks the public
 * Open-Meteo endpoint: a commercial or unspecified distribution can never
 * silently use the non-commercial public endpoint.
 */
typedef enum {
    WEATHER_DISTRIBUTION_UNSPECIFIED = 0,
    WEATHER_DISTRIBUTION_PERSONAL_NONCOMMERCIAL = 1,
    WEATHER_DISTRIBUTION_COMMERCIAL = 2,
    WEATHER_DISTRIBUTION_SELF_HOSTED = 3,
    WEATHER_DISTRIBUTION__COUNT
} WeatherDistributionMode;

/*
 * Provider eligibility under a distribution mode (separate from technical
 * parser validity):
 *  - UNCONFIGURED provider        -> ERR_UNCONFIGURED always;
 *  - OPEN_METEO (public endpoint) -> OK only under PERSONAL_NONCOMMERCIAL
 *    (official terms: free tier is non-commercial); UNSPECIFIED,
 *    COMMERCIAL and SELF_HOSTED all -> ERR_INELIGIBLE_DISTRIBUTION
 *    (commercial requires a future customer/self-hosted adapter; a
 *    self-hosted deployment requires its own adapter).
 */
WeatherProviderResult weather_provider_eligible(WeatherProviderId provider,
                                                WeatherDistributionMode mode);

/* ------------------------------------------------------------------ */
/* Attribution metadata (Stage 4)                                      */
/* ------------------------------------------------------------------ */

/* Static bounded presentation metadata (no rendering in W3). The URL here
 * is DISPLAY attribution required by the data licence — it is not an
 * endpoint and is never fetched by this component. */
typedef struct {
    const char *label;            /* e.g. "Open-Meteo"                   */
    const char *attribution_text; /* e.g. "Weather data by Open-Meteo.com" */
    const char *attribution_url;  /* display link required by CC-BY      */
    const char *licence;          /* e.g. "CC BY 4.0"                    */
} WeatherAttribution;

/* NULL for UNCONFIGURED/unknown providers. */
const WeatherAttribution *weather_provider_attribution(WeatherProviderId id);

/* ------------------------------------------------------------------ */
/* Replaceable provider interface                                      */
/* ------------------------------------------------------------------ */

/* Bounded request parameters. Coordinates are scaled integers (degrees *
 * 1e4); 0/0 means UNSET and is rejected. No label, no identity, no URL. */
typedef struct {
    int32_t latitude_e4;   /* |lat| <= 900000                            */
    int32_t longitude_e4;  /* |lon| <= 1800000                           */
    WeatherTimezoneId timezone; /* must be EUROPE_BRUSSELS in the MVP    */
} WeatherRequestParams;

/* A built request: compile-constant allowlisted host/path plus a bounded
 * deterministic query. No arbitrary URL is representable. */
#define WEATHER_QUERY_MAX 224
typedef struct {
    const char *host;  /* compile-defined allowlisted host               */
    const char *path;  /* compile-defined path                           */
    char query[WEATHER_QUERY_MAX]; /* deterministic, bounded             */
} WeatherRequest;

/* Provider-validation context for parsing (all caller-supplied). */
typedef struct {
    WeatherLocalDate expected_date; /* today's trusted Brussels date     */
    uint64_t fetch_epoch_s;         /* trusted epoch of the fetch        */
    bool fetch_epoch_trusted;
    uint32_t source_generation;
} WeatherParseContext;

typedef struct {
    WeatherProviderId id;
    WeatherProviderResult (*build_request)(const WeatherRequestParams *params,
                                           WeatherRequest *out);
    WeatherProviderResult (*parse_response)(const uint8_t *body, size_t body_len,
                                            const WeatherParseContext *ctx,
                                            WeatherForecast *out);
    const WeatherAttribution *(*attribution)(void);
} WeatherProviderIface;

/* Registry lookup. NULL for UNCONFIGURED and unknown ids — an
 * unconfigured provider has no interface and can never be invoked. */
const WeatherProviderIface *weather_provider_get(WeatherProviderId id);

/* Stable sanitized tokens (never host, coordinates, status text, TLS
 * internals or response fragments). */
const char *weather_provider_result_str(WeatherProviderResult r);
const char *weather_provider_id_str(WeatherProviderId id);
const char *weather_distribution_str(WeatherDistributionMode m);

_Static_assert(WEATHER_PROVIDER_RESULT__COUNT == 23,
               "provider result count changed — review tokens/tests");
_Static_assert(WEATHER_DISTRIBUTION_UNSPECIFIED == 0,
               "unspecified distribution must be the zero default");

#endif /* WEATHER_PROVIDER_H_ */
