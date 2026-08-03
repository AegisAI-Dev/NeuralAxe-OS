/*
 * NeuralAxe Weather-Aware Tuning — PRIVATE SOURCE POLICY (Gate W5).
 * See include/nx_weather_source.h for the contract.
 *
 * Everything in this file is pure: no I/O, no allocation, no clock, no
 * logging, no ESP-IDF dependency beyond the committed W1-W4 headers. It
 * cannot emit a coordinate because it never formats one.
 */

#include <string.h>

#include "nx_weather_source.h"

/* ------------------------------------------------------------------ */
/* Distribution policy                                                 */
/* ------------------------------------------------------------------ */

WeatherDistributionMode nx_weather_distribution_to_w3(NxWeatherDistribution d)
{
    switch (d) {
    case NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL:
        /*
         * The owner supplies and owns the external source on a privately
         * operated device. That is exactly the committed W3 licence class
         * that permits the public endpoint; every other W5 mode maps to the
         * committed UNSPECIFIED, which W3 already refuses.
         */
        return WEATHER_DISTRIBUTION_PERSONAL_NONCOMMERCIAL;
    case NX_WEATHER_DIST_UNSPECIFIED:
    case NX_WEATHER_DIST_DISABLED:
    default:
        return WEATHER_DISTRIBUTION_UNSPECIFIED;
    }
}

bool nx_weather_distribution_permits(NxWeatherDistribution d,
                                     WeatherProviderId provider)
{
    if ((unsigned)d >= (unsigned)NX_WEATHER_DIST__COUNT) {
        return false;               /* out of range: fail closed */
    }
    if (d != NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL) {
        return false;               /* only one mode ever permits a source */
    }
    if (provider == WEATHER_PROVIDER_UNCONFIGURED ||
        (unsigned)provider >= (unsigned)WEATHER_PROVIDER__COUNT) {
        return false;               /* nothing selected / unknown */
    }
    /* The committed W3 eligibility function stays the single authority. */
    return weather_provider_eligible(provider,
                                     nx_weather_distribution_to_w3(d)) ==
           WEATHER_PROVIDER_OK;
}

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */

void nx_weather_source_defaults(NxWeatherSourceConfig *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->enabled      = false;
    out->distribution = NX_WEATHER_DIST_UNSPECIFIED;
    out->provider     = WEATHER_PROVIDER_UNCONFIGURED;
    out->latitude_e4  = 0;
    out->longitude_e4 = 0;
    out->timezone     = WEATHER_TZ_UNSPECIFIED;
    /* The committed W3 schedule defaults; still inert while disabled. */
    weather_schedule_config_defaults(&out->schedule);
    /* W5 exists to permit a recommendation, never an execution. */
    out->recommendation_only = true;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static bool latitude_in_range(int32_t lat_e4)
{
    return lat_e4 >= -NX_WEATHER_LAT_E4_MAX && lat_e4 <= NX_WEATHER_LAT_E4_MAX;
}

static bool longitude_in_range(int32_t lon_e4)
{
    return lon_e4 >= -NX_WEATHER_LON_E4_MAX && lon_e4 <= NX_WEATHER_LON_E4_MAX;
}

NxWeatherSourceStatus nx_weather_source_validate(const NxWeatherSourceConfig *cfg)
{
    if (cfg == NULL || !cfg->enabled) {
        return NX_WX_SRC_UNCONFIGURED;
    }

    /* An entirely blank-but-enabled configuration is "nothing supplied"
     * rather than a specific field error — report it as unconfigured. */
    if (cfg->distribution == NX_WEATHER_DIST_UNSPECIFIED &&
        cfg->provider == WEATHER_PROVIDER_UNCONFIGURED &&
        cfg->latitude_e4 == 0 && cfg->longitude_e4 == 0 &&
        cfg->timezone == WEATHER_TZ_UNSPECIFIED) {
        return NX_WX_SRC_UNCONFIGURED;
    }

    if ((unsigned)cfg->distribution >= (unsigned)NX_WEATHER_DIST__COUNT ||
        cfg->distribution != NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL) {
        /* UNSPECIFIED and DISABLED both refuse to permit any source. */
        return NX_WX_SRC_INVALID_DISTRIBUTION;
    }

    if (cfg->provider == WEATHER_PROVIDER_UNCONFIGURED ||
        (unsigned)cfg->provider >= (unsigned)WEATHER_PROVIDER__COUNT ||
        !nx_weather_distribution_permits(cfg->distribution, cfg->provider)) {
        return NX_WX_SRC_INVALID_PROVIDER;
    }

    if (!latitude_in_range(cfg->latitude_e4)) {
        return NX_WX_SRC_INVALID_LATITUDE;
    }
    if (!longitude_in_range(cfg->longitude_e4)) {
        return NX_WX_SRC_INVALID_LONGITUDE;
    }

    /*
     * The committed W3 "unset" sentinel. 0/0 is a real point at sea, but the
     * repository already reserves it to mean "no location supplied", so
     * accepting it would let an unconfigured device issue a request for a
     * location nobody chose. Exactly one of the two being zero is a
     * partially supplied pair, which is INCOMPLETE rather than invalid.
     */
    if (cfg->latitude_e4 == 0 && cfg->longitude_e4 == 0) {
        return NX_WX_SRC_INCOMPLETE;
    }
    if (cfg->latitude_e4 == 0 || cfg->longitude_e4 == 0) {
        return NX_WX_SRC_INCOMPLETE;
    }

    if (cfg->timezone == WEATHER_TZ_UNSPECIFIED ||
        (unsigned)cfg->timezone >= (unsigned)WEATHER_TZ__COUNT) {
        return NX_WX_SRC_INVALID_TIMEZONE;
    }

    if (!weather_schedule_config_valid(&cfg->schedule)) {
        return NX_WX_SRC_INVALID_SCHEDULE;
    }

    if (!cfg->recommendation_only) {
        /* W5 can authorize a recommendation and nothing else. */
        return NX_WX_SRC_NOT_RECOMMENDATION_ONLY;
    }

    return NX_WX_SRC_READY_RECOMMENDATION_ONLY;
}

const char *nx_weather_source_status_str(NxWeatherSourceStatus s)
{
    switch (s) {
    case NX_WX_SRC_UNCONFIGURED:              return "WX_SRC_UNCONFIGURED";
    case NX_WX_SRC_INVALID_DISTRIBUTION:      return "WX_SRC_INVALID_DISTRIBUTION";
    case NX_WX_SRC_INVALID_PROVIDER:          return "WX_SRC_INVALID_PROVIDER";
    case NX_WX_SRC_INVALID_LATITUDE:          return "WX_SRC_INVALID_LATITUDE";
    case NX_WX_SRC_INVALID_LONGITUDE:         return "WX_SRC_INVALID_LONGITUDE";
    case NX_WX_SRC_INVALID_TIMEZONE:          return "WX_SRC_INVALID_TIMEZONE";
    case NX_WX_SRC_INVALID_SCHEDULE:          return "WX_SRC_INVALID_SCHEDULE";
    case NX_WX_SRC_INCOMPLETE:                return "WX_SRC_INCOMPLETE";
    case NX_WX_SRC_NOT_RECOMMENDATION_ONLY:   return "WX_SRC_NOT_RECOMMENDATION_ONLY";
    case NX_WX_SRC_READY_RECOMMENDATION_ONLY: return "WX_SRC_READY_RECOMMENDATION_ONLY";
    case NX_WX_SRC__COUNT:
    default:                                  return "WX_SRC_UNCONFIGURED";
    }
}

bool nx_weather_source_ready(NxWeatherSourceStatus s)
{
    return s == NX_WX_SRC_READY_RECOMMENDATION_ONLY;
}

/* ------------------------------------------------------------------ */
/* Projection onto the committed Gate W4 runtime configuration         */
/* ------------------------------------------------------------------ */

NxWeatherSourceStatus nx_weather_source_to_runtime(const NxWeatherSourceConfig *cfg,
                                                   WeatherRuntimeConfig *out)
{
    NxWeatherSourceStatus status;

    if (out == NULL) {
        return NX_WX_SRC_UNCONFIGURED;
    }
    /* The W4 safe posture first, so every failure path leaves a runtime
     * configuration that cannot request anything. */
    weather_runtime_config_defaults(out);

    status = nx_weather_source_validate(cfg);
    if (!nx_weather_source_ready(status)) {
        return status;
    }

    /* The ONLY place a private location reaches the runtime. */
    out->enabled                = true;
    out->expected_provider      = cfg->provider;
    out->location.latitude_e4   = cfg->latitude_e4;
    out->location.longitude_e4  = cfg->longitude_e4;
    out->location.timezone      = cfg->timezone;
    out->schedule               = cfg->schedule;
    /* max_sync_age_s, max_forecast_age_s and max_attempts keep the committed
     * W4 defaults: W5 decides WHETHER and WHERE, never the runtime's own
     * freshness or retry policy. */
    return status;
}
