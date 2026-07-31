/*
 * NeuralAxe Weather-Aware Tuning — provider registry, distribution
 * eligibility, attribution and transport-layer validation (Gate W3).
 * PURE: no I/O, no heap, no clocks, no logging.
 */

#include <string.h>
#include <stdio.h>
#include "weather_provider.h"
#include "weather_transport.h"
#include "weather_open_meteo.h"

WeatherProviderResult weather_provider_eligible(WeatherProviderId provider,
                                                WeatherDistributionMode mode)
{
    if ((unsigned)mode >= WEATHER_DISTRIBUTION__COUNT) {
        return WEATHER_PROVIDER_ERR_INELIGIBLE_DISTRIBUTION; /* fail closed */
    }
    switch (provider) {
    case WEATHER_PROVIDER_UNCONFIGURED:
        return WEATHER_PROVIDER_ERR_UNCONFIGURED;
    case WEATHER_PROVIDER_OPEN_METEO:
        /* The PUBLIC endpoint's free tier is non-commercial only
         * (official terms). Anything but an explicit personal/
         * non-commercial distribution keeps it ineligible — commercial
         * and self-hosted modes require their own future adapters, and
         * an unspecified distribution never silently uses it. */
        return (mode == WEATHER_DISTRIBUTION_PERSONAL_NONCOMMERCIAL)
                   ? WEATHER_PROVIDER_OK
                   : WEATHER_PROVIDER_ERR_INELIGIBLE_DISTRIBUTION;
    default:
        return WEATHER_PROVIDER_ERR_UNCONFIGURED;
    }
}

const WeatherAttribution *weather_provider_attribution(WeatherProviderId id)
{
    const WeatherProviderIface *iface = weather_provider_get(id);
    if (iface == NULL || iface->attribution == NULL) {
        return NULL;
    }
    return iface->attribution();
}

const WeatherProviderIface *weather_provider_get(WeatherProviderId id)
{
    switch (id) {
    case WEATHER_PROVIDER_OPEN_METEO:
        return weather_provider_open_meteo();
    default:
        return NULL; /* UNCONFIGURED/unknown: no interface, never invoked */
    }
}

/* ---------------- transport-layer validation ---------------- */

WeatherProviderResult weather_transport_validate(const WeatherHttpResponse *r)
{
    if (r == NULL) {
        return WEATHER_PROVIDER_ERR_INTERNAL;
    }
    if (r->oversized || r->body_len > WEATHER_RESPONSE_CAP) {
        return WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE;
    }
    if (!r->complete) {
        return WEATHER_PROVIDER_ERR_TRUNCATED;
    }
    if (r->http_status >= 300 && r->http_status < 400) {
        return WEATHER_PROVIDER_ERR_REDIRECT_REJECTED; /* never followed */
    }
    if (r->http_status < 200 || r->http_status >= 300) {
        return WEATHER_PROVIDER_ERR_HTTP_STATUS;
    }
    /* Explicit conservative policy: a missing Content-Type fails closed
     * exactly like a wrong one — unavailability degrades toward the safe
     * hot stance, never toward acceptance. */
    if (!r->content_type_present || !r->content_type_json) {
        return WEATHER_PROVIDER_ERR_CONTENT_TYPE;
    }
    if (r->body_len == 0u) {
        return WEATHER_PROVIDER_ERR_TRUNCATED;
    }
    return WEATHER_PROVIDER_OK;
}

bool weather_request_url(const WeatherRequest *req, char *buf, size_t cap)
{
    int n;
    if (req == NULL || buf == NULL || cap == 0u ||
        req->host == NULL || req->path == NULL) {
        return false;
    }
    n = snprintf(buf, cap, "https://%s%s?%s", req->host, req->path, req->query);
    if (n <= 0 || (size_t)n >= cap || (size_t)n >= WEATHER_REQUEST_URL_MAX) {
        if (cap > 0u) {
            buf[0] = '\0';
        }
        return false;
    }
    return true;
}

/* ---------------- stable sanitized tokens ---------------- */

const char *weather_provider_result_str(WeatherProviderResult r)
{
    switch (r) {
    case WEATHER_PROVIDER_OK: return "OK";
    case WEATHER_PROVIDER_ERR_UNCONFIGURED: return "ERR_UNCONFIGURED";
    case WEATHER_PROVIDER_ERR_INELIGIBLE_DISTRIBUTION: return "ERR_INELIGIBLE_DISTRIBUTION";
    case WEATHER_PROVIDER_ERR_REQUEST_INVALID: return "ERR_REQUEST_INVALID";
    case WEATHER_PROVIDER_ERR_TLS_FAILURE: return "ERR_TLS_FAILURE";
    case WEATHER_PROVIDER_ERR_DNS_FAILURE: return "ERR_DNS_FAILURE";
    case WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT: return "ERR_CONNECT_TIMEOUT";
    case WEATHER_PROVIDER_ERR_READ_TIMEOUT: return "ERR_READ_TIMEOUT";
    case WEATHER_PROVIDER_ERR_HTTP_STATUS: return "ERR_HTTP_STATUS";
    case WEATHER_PROVIDER_ERR_REDIRECT_REJECTED: return "ERR_REDIRECT_REJECTED";
    case WEATHER_PROVIDER_ERR_CONTENT_TYPE: return "ERR_CONTENT_TYPE";
    case WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE: return "ERR_RESPONSE_TOO_LARGE";
    case WEATHER_PROVIDER_ERR_TRUNCATED: return "ERR_TRUNCATED";
    case WEATHER_PROVIDER_ERR_JSON_INVALID: return "ERR_JSON_INVALID";
    case WEATHER_PROVIDER_ERR_SCHEMA_INVALID: return "ERR_SCHEMA_INVALID";
    case WEATHER_PROVIDER_ERR_UNITS_INVALID: return "ERR_UNITS_INVALID";
    case WEATHER_PROVIDER_ERR_TIMEZONE_INVALID: return "ERR_TIMEZONE_INVALID";
    case WEATHER_PROVIDER_ERR_DATE_INVALID: return "ERR_DATE_INVALID";
    case WEATHER_PROVIDER_ERR_VALUE_INVALID: return "ERR_VALUE_INVALID";
    case WEATHER_PROVIDER_ERR_CACHE_STALE: return "ERR_CACHE_STALE";
    case WEATHER_PROVIDER_ERR_RATE_LIMITED: return "ERR_RATE_LIMITED";
    case WEATHER_PROVIDER_ERR_RETRY_EXHAUSTED: return "ERR_RETRY_EXHAUSTED";
    case WEATHER_PROVIDER_ERR_INTERNAL: return "ERR_INTERNAL";
    default: return "ERR_UNKNOWN";
    }
}

const char *weather_provider_id_str(WeatherProviderId id)
{
    switch (id) {
    case WEATHER_PROVIDER_UNCONFIGURED: return "UNCONFIGURED";
    case WEATHER_PROVIDER_OPEN_METEO: return "OPEN_METEO";
    default: return "UNKNOWN";
    }
}

const char *weather_distribution_str(WeatherDistributionMode m)
{
    switch (m) {
    case WEATHER_DISTRIBUTION_UNSPECIFIED: return "UNSPECIFIED";
    case WEATHER_DISTRIBUTION_PERSONAL_NONCOMMERCIAL: return "PERSONAL_NONCOMMERCIAL";
    case WEATHER_DISTRIBUTION_COMMERCIAL: return "COMMERCIAL";
    case WEATHER_DISTRIBUTION_SELF_HOSTED: return "SELF_HOSTED";
    default: return "UNKNOWN";
    }
}
