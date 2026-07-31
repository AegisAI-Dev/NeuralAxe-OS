/*
 * NeuralAxe Weather-Aware Tuning — pure Europe/Brussels time model
 * (Gate W3). PURE: no I/O, no heap, no clocks, no global TZ state.
 */

#include "brussels_time.h"

/* Civil-calendar day arithmetic (Howard Hinnant's algorithms; public
 * domain formulation, exact over the supported band). Day 0 = 1970-01-01. */
static int64_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= (m <= 2u) ? 1 : 0;
    {
        const int32_t era = (y >= 0 ? y : y - 399) / 400;
        const uint32_t yoe = (uint32_t)(y - era * 400);
        const uint32_t doy = (153u * (m + ((m > 2u) ? (uint32_t)-3 : 9u)) + 2u) / 5u + d - 1u;
        const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
        return (int64_t)era * 146097 + (int64_t)doe - 719468;
    }
}

static void civil_from_days(int64_t z, int32_t *out_y, uint32_t *out_m, uint32_t *out_d)
{
    z += 719468;
    {
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const uint32_t doe = (uint32_t)(z - era * 146097);
        const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
        const int64_t y = (int64_t)yoe + era * 400;
        const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
        const uint32_t mp = (5u * doy + 2u) / 153u;
        const uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
        const uint32_t m = mp + ((mp < 10u) ? 3u : (uint32_t)-9);
        *out_y = (int32_t)(y + ((m <= 2u) ? 1 : 0));
        *out_m = m;
        *out_d = d;
    }
}

/* ---------------- Gregorian helpers ---------------- */

bool brussels_is_leap_year(uint16_t year)
{
    if ((year % 4u) != 0u) {
        return false;
    }
    if ((year % 100u) != 0u) {
        return true;
    }
    return (year % 400u) == 0u;
}

uint8_t brussels_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1u || month > 12u) {
        return 0u;
    }
    if (month == 2u && brussels_is_leap_year(year)) {
        return 29u;
    }
    return days[month - 1u];
}

bool brussels_date_valid(const WeatherLocalDate *d)
{
    if (d == NULL) {
        return false;
    }
    if (d->year < BRUSSELS_YEAR_MIN || d->year > BRUSSELS_YEAR_MAX) {
        return false;
    }
    if (d->month < 1u || d->month > 12u) {
        return false;
    }
    if (d->day < 1u || d->day > brussels_days_in_month(d->year, d->month)) {
        return false;
    }
    return true;
}

uint8_t brussels_weekday(const WeatherLocalDate *d)
{
    int64_t z = days_from_civil((int32_t)d->year, d->month, d->day);
    /* Day 0 (1970-01-01) was a Thursday; with Sunday==0 that is weekday 4. */
    return (uint8_t)((z + 4) % 7);
}

uint8_t brussels_last_sunday(uint16_t year, uint8_t month)
{
    WeatherLocalDate d;
    uint8_t last = brussels_days_in_month(year, month);
    uint8_t wd;
    if (last == 0u) {
        return 0u;
    }
    d.year = year;
    d.month = month;
    d.day = last;
    wd = brussels_weekday(&d); /* 0 = Sunday */
    return (uint8_t)(last - wd);
}

int brussels_date_compare(const WeatherLocalDate *a, const WeatherLocalDate *b)
{
    if (a->year != b->year) {
        return (a->year < b->year) ? -1 : 1;
    }
    if (a->month != b->month) {
        return (a->month < b->month) ? -1 : 1;
    }
    if (a->day != b->day) {
        return (a->day < b->day) ? -1 : 1;
    }
    return 0;
}

/* ---------------- Brussels DST rule ---------------- */

static uint64_t transition_utc(uint16_t year, uint8_t month)
{
    uint8_t day;
    int64_t z;
    if (year < BRUSSELS_YEAR_MIN || year > BRUSSELS_YEAR_MAX) {
        return 0ull;
    }
    day = brussels_last_sunday(year, month);
    z = days_from_civil((int32_t)year, month, day);
    /* 01:00 UTC on the transition day. */
    return (uint64_t)z * 86400ull + 3600ull;
}

uint64_t brussels_spring_transition_utc(uint16_t year)
{
    return transition_utc(year, 3u);
}

uint64_t brussels_autumn_transition_utc(uint16_t year)
{
    return transition_utc(year, 10u);
}

bool brussels_utc_offset(uint64_t utc_s, int32_t *out_offset_s)
{
    int32_t y;
    uint32_t m, d;
    uint64_t spring, autumn;

    if (out_offset_s == NULL) {
        return false;
    }
    if (utc_s < BRUSSELS_EPOCH_MIN_S || utc_s > BRUSSELS_EPOCH_MAX_S) {
        return false;
    }
    civil_from_days((int64_t)(utc_s / 86400ull), &y, &m, &d);
    spring = brussels_spring_transition_utc((uint16_t)y);
    autumn = brussels_autumn_transition_utc((uint16_t)y);
    if (spring == 0ull || autumn == 0ull) {
        return false;
    }
    *out_offset_s = (utc_s >= spring && utc_s < autumn) ? BRUSSELS_OFFSET_CEST_S
                                                        : BRUSSELS_OFFSET_CET_S;
    return true;
}

bool brussels_local_from_utc(uint64_t utc_s, BrusselsLocalTime *out)
{
    int32_t offset;
    uint64_t local_s;
    int64_t days;
    uint32_t sod;
    int32_t y;
    uint32_t m, d;

    if (out == NULL) {
        return false;
    }
    if (!brussels_utc_offset(utc_s, &offset)) {
        return false;
    }
    local_s = utc_s + (uint64_t)(uint32_t)offset;
    days = (int64_t)(local_s / 86400ull);
    sod = (uint32_t)(local_s % 86400ull);
    civil_from_days(days, &y, &m, &d);

    out->date.year = (uint16_t)y;
    out->date.month = (uint8_t)m;
    out->date.day = (uint8_t)d;
    out->hour = (uint8_t)(sod / 3600u);
    out->minute = (uint8_t)((sod % 3600u) / 60u);
    out->second = (uint8_t)(sod % 60u);
    out->minute_of_day = (uint16_t)(sod / 60u);
    out->weekday = brussels_weekday(&out->date);
    out->utc_offset_s = offset;
    out->dst_active = (offset == BRUSSELS_OFFSET_CEST_S);
    return true;
}

bool brussels_utc_from_local(const WeatherLocalDate *date,
                             uint16_t minute_of_day,
                             BrusselsResolution *out)
{
    int64_t naive_s;
    uint64_t cet_candidate, cest_candidate;
    int32_t off;
    bool cet_valid = false, cest_valid = false;

    if (out == NULL) {
        return false;
    }
    out->utc_s = 0ull;
    out->valid = false;
    out->dst_adjusted = false;
    out->dst_ambiguous = false;
    if (!brussels_date_valid(date) || minute_of_day >= BRUSSELS_MINUTES_PER_DAY) {
        return false;
    }

    naive_s = days_from_civil((int32_t)date->year, date->month, date->day) * 86400
              + (int64_t)minute_of_day * 60;

    /* Candidate assuming CET (UTC+1) and CEST (UTC+2). A candidate is real
     * when the offset actually in force at that UTC instant matches the
     * assumption. */
    cet_candidate = (uint64_t)(naive_s - BRUSSELS_OFFSET_CET_S);
    cest_candidate = (uint64_t)(naive_s - BRUSSELS_OFFSET_CEST_S);
    if (brussels_utc_offset(cet_candidate, &off) && off == BRUSSELS_OFFSET_CET_S) {
        cet_valid = true;
    }
    if (brussels_utc_offset(cest_candidate, &off) && off == BRUSSELS_OFFSET_CEST_S) {
        cest_valid = true;
    }

    if (cet_valid && cest_valid) {
        /* Autumn overlap: the wall time occurs twice. Documented policy:
         * the FIRST occurrence (the earlier instant, still CEST) wins; the
         * repeated hour can never trigger the same wall time twice. */
        out->utc_s = cest_candidate;
        out->dst_ambiguous = true;
        out->valid = true;
        return true;
    }
    if (cet_valid) {
        out->utc_s = cet_candidate;
        out->valid = true;
        return true;
    }
    if (cest_valid) {
        out->utc_s = cest_candidate;
        out->valid = true;
        return true;
    }

    /* Spring gap: the wall time does not exist (clocks jumped 02:00->03:00
     * local). Documented policy: adjust to the FIRST VALID instant after
     * the gap — the transition itself. */
    out->utc_s = brussels_spring_transition_utc(date->year);
    out->dst_adjusted = true;
    out->valid = (out->utc_s != 0ull);
    return out->valid;
}
