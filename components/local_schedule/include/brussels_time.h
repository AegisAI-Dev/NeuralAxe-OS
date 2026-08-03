#ifndef BRUSSELS_TIME_H_
#define BRUSSELS_TIME_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * NeuralAxe Weather-Aware Tuning — pure Europe/Brussels time model
 * (Gate W3).
 *
 * PURE: no NVS, no network, no clock reads, no heap, no logging, no
 * FreeRTOS. This module NEVER touches process-global timezone state:
 * no setenv("TZ"), no tzset(), no localtime(), no mktime(). The audited
 * Brussels rules are implemented directly in UTC with bounded integer
 * arithmetic (Gate W0 §21):
 *
 *   - standard time (CET):  UTC+1  (3600 s)
 *   - daylight time (CEST): UTC+2  (7200 s)
 *   - DST begins: last Sunday of March    at 01:00 UTC
 *   - DST ends:   last Sunday of October  at 01:00 UTC
 *
 * The EU seasonal-clock-change abolition was never enacted; this rule is
 * current law as of the audit. Product-facing name: "Europe/Brussels".
 *
 * TIME TRUST: every epoch consumed here must be the B2 operationally
 * trusted UTC epoch (SNTP-anchor + monotonic elapsed). Raw time(),
 * gettimeofday() and Stratum ntime are NEVER acceptable inputs — the
 * system wall clock on this firmware is pool-controlled (Gate W0 §20).
 *
 * Supported epoch band mirrors the B2/W2 sanity band (2025-01-01 ..
 * 2100-01-01); values outside it are rejected, never wrapped.
 */

/* Equal to the B2/W2 band by design (documented, independently defined so
 * this component stays dependency-free). */
#define BRUSSELS_EPOCH_MIN_S 1735689600ull /* 2025-01-01T00:00:00Z */
#define BRUSSELS_EPOCH_MAX_S 4102444800ull /* 2100-01-01T00:00:00Z */
#define BRUSSELS_YEAR_MIN 2025
#define BRUSSELS_YEAR_MAX 2099

#define BRUSSELS_OFFSET_CET_S 3600
#define BRUSSELS_OFFSET_CEST_S 7200
#define BRUSSELS_MINUTES_PER_DAY 1440u

/* Calendar date in the Europe/Brussels civil calendar (or plain Gregorian
 * for helper use). month 1..12, day 1..31. */
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
} WeatherLocalDate;

/* Full Brussels civil time decomposition of a trusted UTC epoch. */
typedef struct {
    WeatherLocalDate date;
    uint8_t hour;       /* 0..23  */
    uint8_t minute;     /* 0..59  */
    uint8_t second;     /* 0..59  */
    uint16_t minute_of_day; /* hour*60+minute, 0..1439 */
    uint8_t weekday;    /* 0 = Sunday .. 6 = Saturday */
    int32_t utc_offset_s; /* 3600 (CET) or 7200 (CEST) */
    bool dst_active;
} BrusselsLocalTime;

/*
 * Resolution of one Brussels local wall time (date + minute-of-day) to UTC.
 * DST edges make this non-injective:
 *  - spring gap (02:00..02:59 local on the transition date): the wall time
 *    DOES NOT EXIST -> resolved to the first valid instant after the gap
 *    (the transition itself, 01:00 UTC == 03:00 CEST), dst_adjusted=true;
 *  - autumn overlap (02:00..02:59 local): the wall time occurs TWICE ->
 *    the FIRST occurrence (the earlier, CEST instant) is chosen,
 *    dst_ambiguous=true. The second occurrence is never selected.
 */
typedef struct {
    uint64_t utc_s;      /* resolved UTC epoch                            */
    bool valid;          /* false only on range/argument errors           */
    bool dst_adjusted;   /* spring-gap adjustment applied                 */
    bool dst_ambiguous;  /* autumn overlap; first occurrence chosen       */
} BrusselsResolution;

/* ---------------- Gregorian helpers (pure) ---------------- */

bool brussels_is_leap_year(uint16_t year);
uint8_t brussels_days_in_month(uint16_t year, uint8_t month);
/* Strict validity: year within the supported band, month 1..12, day within
 * the month. */
bool brussels_date_valid(const WeatherLocalDate *d);
/* 0 = Sunday .. 6 = Saturday; date must be valid. */
uint8_t brussels_weekday(const WeatherLocalDate *d);
/* Day of month (25..31) of the last Sunday of the given month. */
uint8_t brussels_last_sunday(uint16_t year, uint8_t month);
/* Lexicographic date comparison: <0, 0, >0. */
int brussels_date_compare(const WeatherLocalDate *a, const WeatherLocalDate *b);

/* ---------------- Brussels DST rule ---------------- */

/* UTC epoch of the spring (CET->CEST) / autumn (CEST->CET) transition of
 * `year` (last Sunday of March / October at 01:00 UTC). 0 when the year is
 * outside the supported band. */
uint64_t brussels_spring_transition_utc(uint16_t year);
uint64_t brussels_autumn_transition_utc(uint16_t year);

/* UTC offset (3600 or 7200) applicable at a trusted UTC epoch; false when
 * the epoch is outside the supported band. */
bool brussels_utc_offset(uint64_t utc_s, int32_t *out_offset_s);

/* Decompose a trusted UTC epoch into Brussels civil time; false when the
 * epoch is outside the supported band. */
bool brussels_local_from_utc(uint64_t utc_s, BrusselsLocalTime *out);

/* Resolve a Brussels local wall time to UTC per the documented DST edge
 * policy. `minute_of_day` 0..1439. */
bool brussels_utc_from_local(const WeatherLocalDate *date,
                             uint16_t minute_of_day,
                             BrusselsResolution *out);

#endif /* BRUSSELS_TIME_H_ */
