/*
 * NeuralAxe Weather-Aware Tuning — Open-Meteo adapter: deterministic
 * request builder + strict bounded response parser (Gate W3).
 *
 * PURE except for cJSON parsing of a caller-bounded buffer (parsed once,
 * freed immediately; no raw JSON retained). No I/O, no clocks, no
 * logging, no heap ownership beyond the transient cJSON tree.
 */

#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "weather_open_meteo.h"
#include "weather_transport.h"

/* Fixed attribution metadata (display-only; the URL is licence-required
 * attribution, never an endpoint fetched by this component). */
static const WeatherAttribution s_open_meteo_attribution = {
    .label = "Open-Meteo",
    .attribution_text = "Weather data by Open-Meteo.com",
    .attribution_url = "https://open-meteo.com/",
    .licence = "CC BY 4.0",
};

static const WeatherAttribution *open_meteo_attribution(void)
{
    return &s_open_meteo_attribution;
}

/* ------------------------------------------------------------------ */
/* Deterministic coordinate rendering                                  */
/* ------------------------------------------------------------------ */

/*
 * degrees*1e4 -> "[-]D.DDDD" with exactly four decimals via pure integer
 * arithmetic: no float formatting, no locale, no scientific notation.
 * value 0 renders "0.0000" (canonical — a negative zero cannot exist in
 * two's-complement scaled integers).
 */
static bool render_coord_e4(int32_t e4, char *buf, size_t cap)
{
    uint32_t mag;
    int n;
    if (buf == NULL) {
        return false;
    }
    mag = (e4 < 0) ? (uint32_t)(-(int64_t)e4) : (uint32_t)e4;
    /* Both parts are bounded (<= 180 and <= 9999), so the narrowing casts
     * are exact; uint32_t is `long unsigned` on xtensa, hence the explicit
     * conversion rather than a %lu format. */
    n = snprintf(buf, cap, "%s%u.%04u",
                 (e4 < 0) ? "-" : "", (unsigned)(mag / 10000u),
                 (unsigned)(mag % 10000u));
    return n > 0 && (size_t)n < cap;
}

WeatherProviderResult weather_open_meteo_build_request(const WeatherRequestParams *params,
                                                       WeatherRequest *out)
{
    char lat[16];
    char lon[16];
    int n;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (params == NULL || out == NULL) {
        return WEATHER_PROVIDER_ERR_REQUEST_INVALID;
    }
    if (params->timezone != WEATHER_TZ_EUROPE_BRUSSELS) {
        return WEATHER_PROVIDER_ERR_REQUEST_INVALID;
    }
    if (params->latitude_e4 == 0 && params->longitude_e4 == 0) {
        return WEATHER_PROVIDER_ERR_REQUEST_INVALID; /* unset coordinates */
    }
    if (params->latitude_e4 < -900000 || params->latitude_e4 > 900000 ||
        params->longitude_e4 < -1800000 || params->longitude_e4 > 1800000) {
        return WEATHER_PROVIDER_ERR_REQUEST_INVALID;
    }
    if (!render_coord_e4(params->latitude_e4, lat, sizeof(lat)) ||
        !render_coord_e4(params->longitude_e4, lon, sizeof(lon))) {
        return WEATHER_PROVIDER_ERR_REQUEST_INVALID;
    }

    out->host = WEATHER_OPEN_METEO_HOST;
    out->path = WEATHER_OPEN_METEO_PATH;
    /* Fixed, deterministic parameter order; the timezone is a fixed
     * percent-encoded constant (Europe%2FBrussels); `timezone` is REQUIRED
     * by the API whenever daily variables are requested. */
    n = snprintf(out->query, sizeof(out->query),
                 "latitude=%s&longitude=%s"
                 "&daily=temperature_2m_max"
                 "&current=temperature_2m"
                 "&timezone=Europe%%2FBrussels"
                 "&forecast_days=1"
                 "&temperature_unit=celsius",
                 lat, lon);
    if (n <= 0 || (size_t)n >= sizeof(out->query)) {
        memset(out, 0, sizeof(*out));
        return WEATHER_PROVIDER_ERR_REQUEST_INVALID;
    }
    return WEATHER_PROVIDER_OK;
}

/* ------------------------------------------------------------------ */
/* Strict response parsing (Stage 8)                                   */
/* ------------------------------------------------------------------ */

#define OM_UNIT_CELSIUS "\xC2\xB0" "C" /* UTF-8 degree sign + 'C' */

static bool json_is_finite_number(const cJSON *item, double *out)
{
    double v;
    if (!cJSON_IsNumber(item)) {
        return false; /* numeric strings are rejected */
    }
    v = item->valuedouble;
    if (v != v) {
        return false; /* NaN */
    }
    if (v > 1.0e9 || v < -1.0e9) {
        return false; /* infinities / absurd magnitudes */
    }
    *out = v;
    return true;
}

/* Deterministic deci-degree conversion: round half away from zero. */
static bool to_deci_celsius(double v, int16_t *out_dc)
{
    double scaled = v * 10.0;
    int32_t dc = (int32_t)(scaled + ((scaled >= 0.0) ? 0.5 : -0.5));
    if (dc < WEATHER_TEMP_SANITY_MIN_DC || dc > WEATHER_TEMP_SANITY_MAX_DC) {
        return false;
    }
    *out_dc = (int16_t)dc;
    return true;
}

/* Strict "YYYY-MM-DD" -> valid Gregorian date in the supported band. */
static bool parse_iso_date(const char *s, WeatherLocalDate *out)
{
    uint32_t y = 0, m = 0, d = 0;
    size_t i;
    if (s == NULL || strlen(s) != 10u || s[4] != '-' || s[7] != '-') {
        return false;
    }
    for (i = 0; i < 10u; i++) {
        if (i == 4u || i == 7u) {
            continue;
        }
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    y = (uint32_t)(s[0] - '0') * 1000u + (uint32_t)(s[1] - '0') * 100u +
        (uint32_t)(s[2] - '0') * 10u + (uint32_t)(s[3] - '0');
    m = (uint32_t)(s[5] - '0') * 10u + (uint32_t)(s[6] - '0');
    d = (uint32_t)(s[8] - '0') * 10u + (uint32_t)(s[9] - '0');
    out->year = (uint16_t)y;
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
    return brussels_date_valid(out);
}

static bool unit_is_celsius(const cJSON *units, const char *field)
{
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(units, field);
    return cJSON_IsString(u) && u->valuestring != NULL &&
           strcmp(u->valuestring, OM_UNIT_CELSIUS) == 0;
}

WeatherProviderResult weather_open_meteo_parse(const uint8_t *body, size_t body_len,
                                               const WeatherParseContext *ctx,
                                               WeatherForecast *out)
{
    cJSON *root = NULL;
    WeatherProviderResult res = WEATHER_PROVIDER_ERR_SCHEMA_INVALID;
    const char *parse_end = NULL;
    int32_t utc_offset = 0;
    WeatherLocalDate date;
    int16_t max_dc = 0;
    bool current_valid = false;
    int16_t current_dc = 0;

    if (out != NULL) {
        weather_forecast_init(out);
    }
    if (body == NULL || body_len == 0u || ctx == NULL || out == NULL) {
        return WEATHER_PROVIDER_ERR_JSON_INVALID;
    }
    if (body_len > WEATHER_RESPONSE_CAP) {
        return WEATHER_PROVIDER_ERR_RESPONSE_TOO_LARGE;
    }
    if (memchr(body, 0, body_len) != NULL) {
        return WEATHER_PROVIDER_ERR_JSON_INVALID; /* embedded NUL */
    }
    if (!brussels_date_valid(&ctx->expected_date)) {
        return WEATHER_PROVIDER_ERR_DATE_INVALID;
    }

    root = cJSON_ParseWithLengthOpts((const char *)body, body_len, &parse_end, 0);
    if (root == NULL) {
        return WEATHER_PROVIDER_ERR_JSON_INVALID;
    }
    /* Reject trailing non-whitespace garbage after the JSON value. */
    if (parse_end != NULL) {
        const char *p = parse_end;
        const char *end = (const char *)body + body_len;
        while (p < end) {
            char c = *p;
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                cJSON_Delete(root);
                return WEATHER_PROVIDER_ERR_JSON_INVALID;
            }
            p++;
        }
    }

    do {
        const cJSON *item, *daily, *daily_units, *times, *maxima, *current;

        if (!cJSON_IsObject(root)) {
            res = WEATHER_PROVIDER_ERR_JSON_INVALID;
            break;
        }
        item = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (item != NULL && cJSON_IsTrue(item)) {
            /* Provider error payloads normally arrive with HTTP 400 (the
             * HTTP layer already rejects those); a 200+error=true is a
             * schema violation. */
            res = WEATHER_PROVIDER_ERR_SCHEMA_INVALID;
            break;
        }

        item = cJSON_GetObjectItemCaseSensitive(root, "timezone");
        if (!cJSON_IsString(item) || item->valuestring == NULL ||
            strcmp(item->valuestring, "Europe/Brussels") != 0) {
            res = WEATHER_PROVIDER_ERR_TIMEZONE_INVALID;
            break;
        }
        item = cJSON_GetObjectItemCaseSensitive(root, "timezone_abbreviation");
        if (item != NULL) {
            if (!cJSON_IsString(item) || item->valuestring == NULL ||
                (strcmp(item->valuestring, "CET") != 0 &&
                 strcmp(item->valuestring, "CEST") != 0)) {
                res = WEATHER_PROVIDER_ERR_TIMEZONE_INVALID;
                break;
            }
        }
        item = cJSON_GetObjectItemCaseSensitive(root, "utc_offset_seconds");
        {
            double v;
            if (item == NULL || !json_is_finite_number(item, &v) ||
                (v != 3600.0 && v != 7200.0)) {
                res = WEATHER_PROVIDER_ERR_TIMEZONE_INVALID;
                break;
            }
            utc_offset = (int32_t)v;
        }

        daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
        daily_units = cJSON_GetObjectItemCaseSensitive(root, "daily_units");
        if (!cJSON_IsObject(daily) || !cJSON_IsObject(daily_units)) {
            res = WEATHER_PROVIDER_ERR_SCHEMA_INVALID;
            break;
        }
        if (!unit_is_celsius(daily_units, "temperature_2m_max")) {
            res = WEATHER_PROVIDER_ERR_UNITS_INVALID;
            break;
        }
        times = cJSON_GetObjectItemCaseSensitive(daily, "time");
        maxima = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
        if (!cJSON_IsArray(times) || !cJSON_IsArray(maxima)) {
            res = WEATHER_PROVIDER_ERR_SCHEMA_INVALID;
            break;
        }
        if (cJSON_GetArraySize(times) != cJSON_GetArraySize(maxima)) {
            res = WEATHER_PROVIDER_ERR_SCHEMA_INVALID;
            break;
        }
        if (cJSON_GetArraySize(times) != 1) {
            /* Exactly one usable forecast entry in the MVP; zero or
             * multiple/ambiguous rows are rejected. */
            res = WEATHER_PROVIDER_ERR_SCHEMA_INVALID;
            break;
        }
        item = cJSON_GetArrayItem(times, 0);
        if (!cJSON_IsString(item) || item->valuestring == NULL ||
            !parse_iso_date(item->valuestring, &date)) {
            res = WEATHER_PROVIDER_ERR_DATE_INVALID;
            break;
        }
        if (brussels_date_compare(&date, &ctx->expected_date) != 0) {
            res = WEATHER_PROVIDER_ERR_DATE_INVALID; /* wrong/stale date */
            break;
        }
        item = cJSON_GetArrayItem(maxima, 0);
        {
            double v;
            if (item == NULL || cJSON_IsNull(item) ||
                !json_is_finite_number(item, &v) || !to_deci_celsius(v, &max_dc)) {
                res = WEATHER_PROVIDER_ERR_VALUE_INVALID;
                break;
            }
        }

        /* Optional current block: absence never invalidates the forecast;
         * a PRESENT but invalid block fails closed. */
        current = cJSON_GetObjectItemCaseSensitive(root, "current");
        if (current != NULL) {
            const cJSON *cur_units =
                cJSON_GetObjectItemCaseSensitive(root, "current_units");
            const cJSON *cur_t;
            if (!cJSON_IsObject(current)) {
                res = WEATHER_PROVIDER_ERR_SCHEMA_INVALID;
                break;
            }
            cur_t = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
            if (cur_t != NULL) {
                double v;
                if (!cJSON_IsObject(cur_units) ||
                    !unit_is_celsius(cur_units, "temperature_2m")) {
                    res = WEATHER_PROVIDER_ERR_UNITS_INVALID;
                    break;
                }
                if (!json_is_finite_number(cur_t, &v) ||
                    !to_deci_celsius(v, &current_dc)) {
                    res = WEATHER_PROVIDER_ERR_VALUE_INVALID;
                    break;
                }
                current_valid = true;
            }
        }

        /* Success: fill only bounded normalized facts. */
        out->provider = WEATHER_PROVIDER_OPEN_METEO;
        out->timezone = WEATHER_TZ_EUROPE_BRUSSELS;
        out->utc_offset_s = utc_offset;
        out->local_date = date;
        out->forecast_max_dc = max_dc;
        out->current_valid = current_valid;
        out->current_dc = current_dc;
        out->fetch_epoch_s = ctx->fetch_epoch_s;
        out->fetch_epoch_trusted = ctx->fetch_epoch_trusted;
        out->source_generation = ctx->source_generation;
        out->validated = true;
        res = WEATHER_PROVIDER_OK;
    } while (0);

    cJSON_Delete(root); /* freed immediately; no raw JSON retained */
    if (res != WEATHER_PROVIDER_OK && out != NULL) {
        weather_forecast_init(out); /* never a partially valid forecast */
    }
    return res;
}

/* ------------------------------------------------------------------ */
/* Provider interface                                                  */
/* ------------------------------------------------------------------ */

static const WeatherProviderIface s_open_meteo_iface = {
    .id = WEATHER_PROVIDER_OPEN_METEO,
    .build_request = weather_open_meteo_build_request,
    .parse_response = weather_open_meteo_parse,
    .attribution = open_meteo_attribution,
};

const WeatherProviderIface *weather_provider_open_meteo(void)
{
    return &s_open_meteo_iface;
}
