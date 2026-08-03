/*
 * Exhaustive deterministic tests for the pure Europe/Brussels time model
 * (W3). No clocks, no TZ state, no network. External anchors: the EU 2025
 * transitions (2025-03-30 and 2025-10-26, both 01:00 UTC) are publicly
 * documented instants.
 */

#include <string.h>
#include "unity.h"
#include "brussels_time.h"

/* Externally verifiable anchors. */
#define EPOCH_2025_01_01 1735689600ull /* Wed 2025-01-01T00:00:00Z */
#define SPRING_2025 1743296400ull      /* 2025-03-30T01:00:00Z     */
#define AUTUMN_2025 1761440400ull      /* 2025-10-26T01:00:00Z     */

static WeatherLocalDate d(uint16_t y, uint8_t m, uint8_t dd)
{
    WeatherLocalDate x = { y, m, dd };
    return x;
}

TEST_CASE("gregorian: leap years and month lengths", "[brussels_time]")
{
    TEST_ASSERT_FALSE(brussels_is_leap_year(2025));
    TEST_ASSERT_TRUE(brussels_is_leap_year(2028));
    TEST_ASSERT_TRUE(brussels_is_leap_year(2048));
    TEST_ASSERT_FALSE(brussels_is_leap_year(2100)); /* century, not /400 */
    TEST_ASSERT_TRUE(brussels_is_leap_year(2000 + 96)); /* 2096 */
    TEST_ASSERT_EQUAL_UINT8(28, brussels_days_in_month(2025, 2));
    TEST_ASSERT_EQUAL_UINT8(29, brussels_days_in_month(2028, 2));
    TEST_ASSERT_EQUAL_UINT8(31, brussels_days_in_month(2025, 1));
    TEST_ASSERT_EQUAL_UINT8(30, brussels_days_in_month(2025, 4));
    TEST_ASSERT_EQUAL_UINT8(31, brussels_days_in_month(2025, 12));
    TEST_ASSERT_EQUAL_UINT8(0, brussels_days_in_month(2025, 0));
    TEST_ASSERT_EQUAL_UINT8(0, brussels_days_in_month(2025, 13));
}

TEST_CASE("gregorian: date validity and comparison", "[brussels_time]")
{
    WeatherLocalDate a;
    a = d(2026, 2, 29);
    TEST_ASSERT_FALSE(brussels_date_valid(&a)); /* 2026 not leap */
    a = d(2028, 2, 29);
    TEST_ASSERT_TRUE(brussels_date_valid(&a));
    a = d(2024, 6, 1);
    TEST_ASSERT_FALSE(brussels_date_valid(&a)); /* below band */
    a = d(2100, 1, 1);
    TEST_ASSERT_FALSE(brussels_date_valid(&a)); /* above band */
    a = d(2026, 0, 1);
    TEST_ASSERT_FALSE(brussels_date_valid(&a));
    a = d(2026, 4, 31);
    TEST_ASSERT_FALSE(brussels_date_valid(&a));
    TEST_ASSERT_FALSE(brussels_date_valid(NULL));

    {
        WeatherLocalDate x = d(2026, 7, 15), y = d(2026, 7, 16);
        TEST_ASSERT_TRUE(brussels_date_compare(&x, &y) < 0);
        TEST_ASSERT_TRUE(brussels_date_compare(&y, &x) > 0);
        TEST_ASSERT_EQUAL(0, brussels_date_compare(&x, &x));
        y = d(2027, 1, 1);
        TEST_ASSERT_TRUE(brussels_date_compare(&x, &y) < 0);
    }
}

TEST_CASE("gregorian: weekday and last Sundays match known calendars", "[brussels_time]")
{
    WeatherLocalDate a = d(2025, 1, 1);
    TEST_ASSERT_EQUAL_UINT8(3, brussels_weekday(&a)); /* Wednesday */
    a = d(2025, 3, 30);
    TEST_ASSERT_EQUAL_UINT8(0, brussels_weekday(&a)); /* Sunday */

    /* Known EU DST calendar: last Sundays of March/October. */
    TEST_ASSERT_EQUAL_UINT8(30, brussels_last_sunday(2025, 3));
    TEST_ASSERT_EQUAL_UINT8(26, brussels_last_sunday(2025, 10));
    TEST_ASSERT_EQUAL_UINT8(29, brussels_last_sunday(2026, 3));
    TEST_ASSERT_EQUAL_UINT8(25, brussels_last_sunday(2026, 10));
    TEST_ASSERT_EQUAL_UINT8(28, brussels_last_sunday(2027, 3));
    TEST_ASSERT_EQUAL_UINT8(31, brussels_last_sunday(2027, 10));
    TEST_ASSERT_EQUAL_UINT8(26, brussels_last_sunday(2028, 3));
    TEST_ASSERT_EQUAL_UINT8(29, brussels_last_sunday(2028, 10));
}

TEST_CASE("dst: transition instants match the public 2025 anchors", "[brussels_time]")
{
    TEST_ASSERT_EQUAL_UINT64(SPRING_2025, brussels_spring_transition_utc(2025));
    TEST_ASSERT_EQUAL_UINT64(AUTUMN_2025, brussels_autumn_transition_utc(2025));
    TEST_ASSERT_EQUAL_UINT64(0ull, brussels_spring_transition_utc(2024));
    TEST_ASSERT_EQUAL_UINT64(0ull, brussels_autumn_transition_utc(2100));
}

TEST_CASE("dst: offset around both transitions, second-exact", "[brussels_time]")
{
    int32_t off = 0;
    /* one second before / at / after spring */
    TEST_ASSERT_TRUE(brussels_utc_offset(SPRING_2025 - 1ull, &off));
    TEST_ASSERT_EQUAL_INT32(BRUSSELS_OFFSET_CET_S, off);
    TEST_ASSERT_TRUE(brussels_utc_offset(SPRING_2025, &off));
    TEST_ASSERT_EQUAL_INT32(BRUSSELS_OFFSET_CEST_S, off);
    TEST_ASSERT_TRUE(brussels_utc_offset(SPRING_2025 + 1ull, &off));
    TEST_ASSERT_EQUAL_INT32(BRUSSELS_OFFSET_CEST_S, off);
    /* one second before / at / after autumn */
    TEST_ASSERT_TRUE(brussels_utc_offset(AUTUMN_2025 - 1ull, &off));
    TEST_ASSERT_EQUAL_INT32(BRUSSELS_OFFSET_CEST_S, off);
    TEST_ASSERT_TRUE(brussels_utc_offset(AUTUMN_2025, &off));
    TEST_ASSERT_EQUAL_INT32(BRUSSELS_OFFSET_CET_S, off);
    TEST_ASSERT_TRUE(brussels_utc_offset(AUTUMN_2025 + 1ull, &off));
    TEST_ASSERT_EQUAL_INT32(BRUSSELS_OFFSET_CET_S, off);
    /* winter + summer */
    TEST_ASSERT_TRUE(brussels_utc_offset(EPOCH_2025_01_01, &off));
    TEST_ASSERT_EQUAL_INT32(BRUSSELS_OFFSET_CET_S, off);
    TEST_ASSERT_TRUE(brussels_utc_offset(SPRING_2025 + 86400ull * 30u, &off));
    TEST_ASSERT_EQUAL_INT32(BRUSSELS_OFFSET_CEST_S, off);
    /* band edges fail closed */
    TEST_ASSERT_FALSE(brussels_utc_offset(BRUSSELS_EPOCH_MIN_S - 1ull, &off));
    TEST_ASSERT_FALSE(brussels_utc_offset(BRUSSELS_EPOCH_MAX_S, &off));
    TEST_ASSERT_FALSE(brussels_utc_offset(EPOCH_2025_01_01, NULL));
}

TEST_CASE("utc->local: winter, summer and transition decomposition", "[brussels_time]")
{
    BrusselsLocalTime lt;
    /* 2025-01-01T00:00Z -> 01:00 CET Wednesday */
    TEST_ASSERT_TRUE(brussels_local_from_utc(EPOCH_2025_01_01, &lt));
    TEST_ASSERT_EQUAL_UINT16(2025, lt.date.year);
    TEST_ASSERT_EQUAL_UINT8(1, lt.date.month);
    TEST_ASSERT_EQUAL_UINT8(1, lt.date.day);
    TEST_ASSERT_EQUAL_UINT8(1, lt.hour);
    TEST_ASSERT_EQUAL_UINT8(0, lt.minute);
    TEST_ASSERT_EQUAL_UINT16(60, lt.minute_of_day);
    TEST_ASSERT_EQUAL_UINT8(3, lt.weekday);
    TEST_ASSERT_FALSE(lt.dst_active);
    TEST_ASSERT_EQUAL_INT32(BRUSSELS_OFFSET_CET_S, lt.utc_offset_s);

    /* spring instant: 01:00Z -> 03:00 CEST (02:xx never appears) */
    TEST_ASSERT_TRUE(brussels_local_from_utc(SPRING_2025, &lt));
    TEST_ASSERT_EQUAL_UINT8(3, lt.hour);
    TEST_ASSERT_TRUE(lt.dst_active);
    TEST_ASSERT_TRUE(brussels_local_from_utc(SPRING_2025 - 1ull, &lt));
    TEST_ASSERT_EQUAL_UINT8(1, lt.hour);
    TEST_ASSERT_EQUAL_UINT8(59, lt.minute);
    TEST_ASSERT_EQUAL_UINT8(59, lt.second);
    TEST_ASSERT_FALSE(lt.dst_active);

    /* autumn instant: 01:00Z -> 02:00 CET; one second earlier 02:59:59 CEST */
    TEST_ASSERT_TRUE(brussels_local_from_utc(AUTUMN_2025, &lt));
    TEST_ASSERT_EQUAL_UINT8(2, lt.hour);
    TEST_ASSERT_EQUAL_UINT8(0, lt.minute);
    TEST_ASSERT_FALSE(lt.dst_active);
    TEST_ASSERT_TRUE(brussels_local_from_utc(AUTUMN_2025 - 1ull, &lt));
    TEST_ASSERT_EQUAL_UINT8(2, lt.hour);
    TEST_ASSERT_EQUAL_UINT8(59, lt.minute);
    TEST_ASSERT_TRUE(lt.dst_active);

    TEST_ASSERT_FALSE(brussels_local_from_utc(BRUSSELS_EPOCH_MIN_S - 1ull, &lt));
    TEST_ASSERT_FALSE(brussels_local_from_utc(EPOCH_2025_01_01, NULL));
}

TEST_CASE("local->utc: unique resolutions round-trip", "[brussels_time]")
{
    BrusselsResolution r;
    BrusselsLocalTime lt;
    WeatherLocalDate winter = d(2025, 1, 10);
    WeatherLocalDate summer = d(2025, 7, 10);

    TEST_ASSERT_TRUE(brussels_utc_from_local(&winter, 12u * 60u, &r));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FALSE(r.dst_adjusted);
    TEST_ASSERT_FALSE(r.dst_ambiguous);
    TEST_ASSERT_TRUE(brussels_local_from_utc(r.utc_s, &lt));
    TEST_ASSERT_EQUAL(0, brussels_date_compare(&lt.date, &winter));
    TEST_ASSERT_EQUAL_UINT16(12u * 60u, lt.minute_of_day);
    TEST_ASSERT_FALSE(lt.dst_active);

    TEST_ASSERT_TRUE(brussels_utc_from_local(&summer, 12u * 60u, &r));
    TEST_ASSERT_TRUE(r.valid && !r.dst_adjusted && !r.dst_ambiguous);
    TEST_ASSERT_TRUE(brussels_local_from_utc(r.utc_s, &lt));
    TEST_ASSERT_EQUAL_UINT16(12u * 60u, lt.minute_of_day);
    TEST_ASSERT_TRUE(lt.dst_active);
}

TEST_CASE("local->utc: spring gap adjusts to the first valid instant", "[brussels_time]")
{
    BrusselsResolution r;
    WeatherLocalDate gap_day = d(2025, 3, 30);

    /* 02:30 local does not exist -> adjusted to the transition (01:00Z),
     * which reads 03:00 CEST. */
    TEST_ASSERT_TRUE(brussels_utc_from_local(&gap_day, 150u, &r));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_TRUE(r.dst_adjusted);
    TEST_ASSERT_FALSE(r.dst_ambiguous);
    TEST_ASSERT_EQUAL_UINT64(SPRING_2025, r.utc_s);

    /* 02:00 exactly also does not exist. */
    TEST_ASSERT_TRUE(brussels_utc_from_local(&gap_day, 120u, &r));
    TEST_ASSERT_TRUE(r.dst_adjusted);
    TEST_ASSERT_EQUAL_UINT64(SPRING_2025, r.utc_s);

    /* 01:59 exists (CET) and 03:00 exists (CEST, the transition itself). */
    TEST_ASSERT_TRUE(brussels_utc_from_local(&gap_day, 119u, &r));
    TEST_ASSERT_TRUE(r.valid && !r.dst_adjusted && !r.dst_ambiguous);
    TEST_ASSERT_EQUAL_UINT64(SPRING_2025 - 60ull, r.utc_s);
    TEST_ASSERT_TRUE(brussels_utc_from_local(&gap_day, 180u, &r));
    TEST_ASSERT_TRUE(r.valid && !r.dst_adjusted && !r.dst_ambiguous);
    TEST_ASSERT_EQUAL_UINT64(SPRING_2025, r.utc_s);
}

TEST_CASE("local->utc: autumn overlap chooses the FIRST occurrence", "[brussels_time]")
{
    BrusselsResolution r;
    WeatherLocalDate rep_day = d(2025, 10, 26);

    /* 02:30 local occurs twice; the first (CEST) occurrence wins. */
    TEST_ASSERT_TRUE(brussels_utc_from_local(&rep_day, 150u, &r));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_TRUE(r.dst_ambiguous);
    TEST_ASSERT_FALSE(r.dst_adjusted);
    TEST_ASSERT_EQUAL_UINT64(AUTUMN_2025 - 1800ull, r.utc_s); /* 00:30Z */

    /* 01:59 is unique CEST; 03:00 is unique CET. */
    TEST_ASSERT_TRUE(brussels_utc_from_local(&rep_day, 119u, &r));
    TEST_ASSERT_TRUE(r.valid && !r.dst_ambiguous);
    TEST_ASSERT_TRUE(brussels_utc_from_local(&rep_day, 180u, &r));
    TEST_ASSERT_TRUE(r.valid && !r.dst_ambiguous);
    TEST_ASSERT_EQUAL_UINT64(AUTUMN_2025 + 3600ull, r.utc_s); /* 02:00Z */
}

TEST_CASE("local->utc: argument and range rejections", "[brussels_time]")
{
    BrusselsResolution r;
    WeatherLocalDate ok = d(2026, 7, 15);
    WeatherLocalDate bad = d(2024, 7, 15);
    TEST_ASSERT_FALSE(brussels_utc_from_local(NULL, 100u, &r));
    TEST_ASSERT_FALSE(brussels_utc_from_local(&ok, 1440u, &r));
    TEST_ASSERT_FALSE(brussels_utc_from_local(&bad, 100u, &r));
    TEST_ASSERT_FALSE(brussels_utc_from_local(&ok, 100u, NULL));
}
