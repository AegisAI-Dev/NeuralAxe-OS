#ifndef WEATHER_FORECAST_H_
#define WEATHER_FORECAST_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "brussels_time.h"

/*
 * NeuralAxe Weather-Aware Tuning — provider-neutral forecast model
 * (Gate W3).
 *
 * PURE bounded value model. Contains ONLY normalized weather facts —
 * never raw JSON, provider error text, URLs, hostnames, device identity,
 * pool/tuning/telemetry facts, profile ids or tuning intents. Weather
 * data is ADVISORY ONLY: nothing in this component applies tuning or
 * writes frequency/voltage/fan/thermal/NVS values.
 *
 * Temperatures are deci-degrees Celsius (int16). The FORECAST MAXIMUM is
 * the only weather fact the W1 climate hysteresis policy consumes; the
 * optional current outdoor/modelled temperature is INFORMATIONAL ONLY and
 * never influences profile selection (proven by test). Outdoor/modelled
 * temperatures are never ASIC, VRM or inlet temperatures.
 */

/* Provider identity. UNCONFIGURED (0) is the operational default — the
 * numbering mirrors the committed W2 TuningWeatherProvider wire values
 * (asserted in weather_provider.h documentation; W2 is not included here
 * to keep this component free of persistence dependencies). */
typedef enum {
    WEATHER_PROVIDER_UNCONFIGURED = 0,
    WEATHER_PROVIDER_OPEN_METEO = 1,
    WEATHER_PROVIDER__COUNT
} WeatherProviderId;

/* Bounded timezone identity of the forecast's local-day framing (mirrors
 * the W2 TuningTimezoneId values). Never free text. */
typedef enum {
    WEATHER_TZ_UNSPECIFIED = 0,
    WEATHER_TZ_EUROPE_BRUSSELS = 1,
    WEATHER_TZ__COUNT
} WeatherTimezoneId;

/* Temperature sanity band, deci-C (equals the W1 forecast sanity band). */
#define WEATHER_TEMP_SANITY_MIN_DC (-600)
#define WEATHER_TEMP_SANITY_MAX_DC 600

/*
 * Normalized forecast facts. Fixed-width fields, explicit valid flags, no
 * heap, no pointers. `validated` is true ONLY when the full provider
 * validation chain succeeded; a forecast with validated == false must be
 * treated as unusable regardless of other fields.
 */
typedef struct {
    WeatherProviderId provider;
    WeatherLocalDate local_date;   /* provider-local forecast date        */
    int16_t forecast_max_dc;       /* the ONLY policy-relevant fact       */
    bool current_valid;            /* informational only                  */
    int16_t current_dc;            /* informational only                  */
    WeatherTimezoneId timezone;
    int32_t utc_offset_s;          /* provider-reported (3600/7200)       */
    uint64_t fetch_epoch_s;        /* trusted epoch at fetch              */
    bool fetch_epoch_trusted;
    uint32_t source_generation;    /* caller-scoped fetch counter         */
    bool validated;                /* full validation chain passed        */
} WeatherForecast;

/* Zero-initialize (provider UNCONFIGURED, nothing valid). */
void weather_forecast_init(WeatherForecast *out);

/*
 * Central usability rule (shared by the cache and the W1 bridge —
 * defense in depth): a forecast may inform policy ONLY when ALL hold:
 *  - validated == true and provider == expected (configured) provider;
 *  - timezone == Europe/Brussels;
 *  - local_date == today's trusted Brussels local date (yesterday's or a
 *    wrong-date forecast can NEVER authorize anything today);
 *  - fetch_epoch_trusted and now_trusted (untrusted time makes age
 *    unverifiable -> unusable);
 *  - now_epoch_s >= fetch_epoch_s (a future fetch epoch is invalid);
 *  - age (now - fetch) <= max_age_s;
 *  - temperature inside the sanity band.
 */
bool weather_forecast_usable(const WeatherForecast *f,
                             WeatherProviderId expected_provider,
                             const WeatherLocalDate *today,
                             uint64_t now_epoch_s, bool now_trusted,
                             uint32_t max_age_s);

/* ------------------------------------------------------------------ */
/* Fetch instrumentation contract (Stage 20)                           */
/* ------------------------------------------------------------------ */

/*
 * Pure bookkeeping for the FUTURE W4/W6 runtime: the caller captures the
 * platform numbers (free internal heap, stack high-water, monotonic
 * microseconds) and this struct only carries them. W3 fabricates nothing:
 * all fields stay zero until a real runtime fills them. No field contains
 * device identity, and none of this is ever sent to a provider.
 */
typedef struct {
    uint32_t free_internal_before;   /* bytes, caller-captured            */
    uint32_t free_internal_after;    /* bytes, caller-captured            */
    uint32_t response_len;           /* bytes received (<= response cap)  */
    uint32_t parser_input_len;       /* bytes handed to the parser        */
    uint32_t duration_us;            /* monotonic elapsed                 */
    uint32_t stack_high_water;       /* words/bytes per caller convention */
    bool valid;                      /* a completed measurement exists    */
} WeatherFetchStats;

void weather_stats_reset(WeatherFetchStats *s);
/* begin: record pre-request facts (monotonic start is caller-held). */
void weather_stats_begin(WeatherFetchStats *s, uint32_t free_internal_before);
/* end: complete the measurement; duration from caller-held monotonic
 * values (end >= start required, else the stats stay invalid). */
void weather_stats_end(WeatherFetchStats *s, uint32_t free_internal_after,
                       uint64_t mono_start_us, uint64_t mono_end_us,
                       uint32_t response_len, uint32_t parser_input_len,
                       uint32_t stack_high_water);

_Static_assert(WEATHER_PROVIDER_UNCONFIGURED == 0 &&
               WEATHER_PROVIDER_OPEN_METEO == 1 && WEATHER_PROVIDER__COUNT == 2,
               "provider ids mirror the W2 wire values");
_Static_assert(WEATHER_TZ_UNSPECIFIED == 0 && WEATHER_TZ_EUROPE_BRUSSELS == 1 &&
               WEATHER_TZ__COUNT == 2, "timezone ids mirror the W2 wire values");

#endif /* WEATHER_FORECAST_H_ */
