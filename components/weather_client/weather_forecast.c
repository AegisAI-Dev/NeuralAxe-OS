/*
 * NeuralAxe Weather-Aware Tuning — provider-neutral forecast model
 * (Gate W3). PURE: no I/O, no heap, no clocks, no logging.
 */

#include <string.h>
#include "weather_forecast.h"

void weather_forecast_init(WeatherForecast *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->provider = WEATHER_PROVIDER_UNCONFIGURED;
    out->timezone = WEATHER_TZ_UNSPECIFIED;
    out->validated = false;
}

bool weather_forecast_usable(const WeatherForecast *f,
                             WeatherProviderId expected_provider,
                             const WeatherLocalDate *today,
                             uint64_t now_epoch_s, bool now_trusted,
                             uint32_t max_age_s)
{
    if (f == NULL || today == NULL) {
        return false;
    }
    if (!f->validated) {
        return false;
    }
    if (expected_provider == WEATHER_PROVIDER_UNCONFIGURED ||
        f->provider != expected_provider) {
        return false;
    }
    if (f->timezone != WEATHER_TZ_EUROPE_BRUSSELS) {
        return false;
    }
    if (!brussels_date_valid(&f->local_date) || !brussels_date_valid(today)) {
        return false;
    }
    if (brussels_date_compare(&f->local_date, today) != 0) {
        /* Yesterday's (or any wrong-date) forecast never informs today. */
        return false;
    }
    if (!f->fetch_epoch_trusted || !now_trusted) {
        /* Untrusted time makes the age unverifiable — unusable. */
        return false;
    }
    if (now_epoch_s < f->fetch_epoch_s) {
        return false; /* future fetch epoch is invalid */
    }
    if (now_epoch_s - f->fetch_epoch_s > (uint64_t)max_age_s) {
        return false;
    }
    if (f->forecast_max_dc < WEATHER_TEMP_SANITY_MIN_DC ||
        f->forecast_max_dc > WEATHER_TEMP_SANITY_MAX_DC) {
        return false;
    }
    return true;
}

/* ---------------- fetch instrumentation ---------------- */

void weather_stats_reset(WeatherFetchStats *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

void weather_stats_begin(WeatherFetchStats *s, uint32_t free_internal_before)
{
    if (s == NULL) {
        return;
    }
    weather_stats_reset(s);
    s->free_internal_before = free_internal_before;
}

void weather_stats_end(WeatherFetchStats *s, uint32_t free_internal_after,
                       uint64_t mono_start_us, uint64_t mono_end_us,
                       uint32_t response_len, uint32_t parser_input_len,
                       uint32_t stack_high_water)
{
    if (s == NULL) {
        return;
    }
    if (mono_end_us < mono_start_us) {
        s->valid = false; /* monotonic regression: measurement invalid */
        return;
    }
    {
        uint64_t d = mono_end_us - mono_start_us;
        s->duration_us = (d > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)d;
    }
    s->free_internal_after = free_internal_after;
    s->response_len = response_len;
    s->parser_input_len = parser_input_len;
    s->stack_high_water = stack_high_water;
    s->valid = true;
}
