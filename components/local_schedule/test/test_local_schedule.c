/*
 * Exhaustive deterministic tests for the pure daily scheduler (W3).
 * All epochs are built through the independently tested Brussels model;
 * no clocks, no network, no tasks.
 */

#include <string.h>
#include "unity.h"
#include "local_schedule.h"

static WeatherLocalDate d(uint16_t y, uint8_t m, uint8_t dd)
{
    WeatherLocalDate x = { y, m, dd };
    return x;
}

/* Trusted UTC instant for (date, minute-of-day) via the Brussels model. */
static uint64_t at_local(WeatherLocalDate date, uint16_t minute)
{
    BrusselsResolution r;
    TEST_ASSERT_TRUE(brussels_utc_from_local(&date, minute, &r));
    TEST_ASSERT_TRUE(r.valid);
    return r.utc_s;
}

static WeatherScheduleConfig cfg_enabled(void)
{
    WeatherScheduleConfig c;
    weather_schedule_config_defaults(&c);
    c.enabled = true;
    return c;
}

static WeatherScheduleProgress prog_for(WeatherLocalDate date)
{
    WeatherScheduleProgress p;
    weather_schedule_progress_init(&p, &date);
    return p;
}

#define SUMMER_DAY d(2026, 7, 15)
#define WINTER_DAY d(2026, 1, 15)

TEST_CASE("schedule: defaults are disabled with the three product slots", "[local_schedule]")
{
    WeatherScheduleConfig c;
    weather_schedule_config_defaults(&c);
    TEST_ASSERT_FALSE(c.enabled);
    TEST_ASSERT_EQUAL_UINT8(3, c.slot_count);
    TEST_ASSERT_EQUAL_UINT16(300, c.slots_min[0]); /* 05:00 */
    TEST_ASSERT_EQUAL_UINT16(660, c.slots_min[1]); /* 11:00 */
    TEST_ASSERT_EQUAL_UINT16(900, c.slots_min[2]); /* 15:00 */
    TEST_ASSERT_EQUAL_UINT32(0, c.catch_up_window_s); /* conservative */
    TEST_ASSERT_TRUE(weather_schedule_config_valid(&c));
}

TEST_CASE("schedule: config validation matrix", "[local_schedule]")
{
    WeatherScheduleConfig c;
    TEST_ASSERT_FALSE(weather_schedule_config_valid(NULL));
    c = cfg_enabled();
    c.slot_count = 0;
    TEST_ASSERT_FALSE(weather_schedule_config_valid(&c));
    c = cfg_enabled();
    c.slot_count = WEATHER_SCHEDULE_MAX_SLOTS + 1u;
    TEST_ASSERT_FALSE(weather_schedule_config_valid(&c));
    c = cfg_enabled();
    c.slots_min[1] = 300; /* duplicate */
    TEST_ASSERT_FALSE(weather_schedule_config_valid(&c));
    c = cfg_enabled();
    c.slots_min[1] = 200; /* not ascending */
    TEST_ASSERT_FALSE(weather_schedule_config_valid(&c));
    c = cfg_enabled();
    c.slots_min[2] = 1440; /* out of day */
    TEST_ASSERT_FALSE(weather_schedule_config_valid(&c));
    c = cfg_enabled();
    c.slots_min[4] = 5; /* beyond count: canonical zero violated */
    TEST_ASSERT_FALSE(weather_schedule_config_valid(&c));
    c = cfg_enabled();
    c.catch_up_window_s = WEATHER_SCHEDULE_CATCH_UP_MAX_S + 1u;
    TEST_ASSERT_FALSE(weather_schedule_config_valid(&c));
}

TEST_CASE("schedule: progress validation rejects unknown bits", "[local_schedule]")
{
    WeatherScheduleConfig c = cfg_enabled();
    WeatherScheduleProgress p = prog_for(SUMMER_DAY);
    TEST_ASSERT_TRUE(weather_schedule_progress_valid(&p, &c));
    p.executed_mask = 0x07u;
    TEST_ASSERT_TRUE(weather_schedule_progress_valid(&p, &c));
    p.executed_mask = 0x08u; /* bit 3 with slot_count 3 */
    TEST_ASSERT_FALSE(weather_schedule_progress_valid(&p, &c));
    p = prog_for(SUMMER_DAY);
    p.version = 9;
    TEST_ASSERT_FALSE(weather_schedule_progress_valid(&p, &c));
    p = prog_for(d(2024, 1, 1)); /* invalid date */
    TEST_ASSERT_FALSE(weather_schedule_progress_valid(&p, &c));
}

TEST_CASE("schedule: disabled / untrusted / invalid inputs fail closed", "[local_schedule]")
{
    WeatherScheduleConfig c;
    WeatherScheduleProgress p = prog_for(SUMMER_DAY);
    WeatherSchedulePlan plan;
    uint64_t now = at_local(SUMMER_DAY, 600);

    weather_schedule_config_defaults(&c); /* disabled */
    weather_schedule_evaluate(&c, &p, now, true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DISABLED, plan.decision);

    c = cfg_enabled();
    weather_schedule_evaluate(&c, &p, now, false, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_TIME_UNTRUSTED, plan.decision);
    TEST_ASSERT_FALSE(plan.proposal_present);

    c.slots_min[1] = 100; /* invalid config */
    weather_schedule_evaluate(&c, &p, now, true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_CONFIG_INVALID, plan.decision);

    c = cfg_enabled();
    p.executed_mask = 0x40u; /* invalid progress */
    weather_schedule_evaluate(&c, &p, now, true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_CONFIG_INVALID, plan.decision);
    TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_PROGRESS_INVALID, plan.reason);

    /* epoch outside the band */
    p = prog_for(SUMMER_DAY);
    weather_schedule_evaluate(&c, &p, 100ull, true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_TIME_UNTRUSTED, plan.decision);
    TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_EPOCH_RANGE, plan.reason);

    weather_schedule_evaluate(NULL, &p, now, true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_CONFIG_INVALID, plan.decision);
    weather_schedule_evaluate(&c, NULL, now, true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_CONFIG_INVALID, plan.decision);
    weather_schedule_evaluate(&c, &p, now, true, NULL); /* must not crash */
}

TEST_CASE("schedule: before / at / between / after slots (summer day)", "[local_schedule]")
{
    WeatherScheduleConfig c = cfg_enabled();
    WeatherScheduleProgress p = prog_for(SUMMER_DAY);
    WeatherSchedulePlan plan;

    /* 04:00 — before the first slot */
    weather_schedule_evaluate(&c, &p, at_local(SUMMER_DAY, 240), true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, plan.decision);
    TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_BEFORE_FIRST_SLOT, plan.reason);
    TEST_ASSERT_TRUE(plan.next_due_valid);
    TEST_ASSERT_EQUAL_UINT64(at_local(SUMMER_DAY, 300), plan.next_due_utc_s);

    /* exactly 05:00 — DUE for slot 0 with the executed-bit proposal */
    weather_schedule_evaluate(&c, &p, at_local(SUMMER_DAY, 300), true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DUE, plan.decision);
    TEST_ASSERT_EQUAL_INT8(0, plan.slot_index);
    TEST_ASSERT_EQUAL_UINT64(at_local(SUMMER_DAY, 300), plan.slot_utc_s);
    TEST_ASSERT_TRUE(plan.proposal_present);
    TEST_ASSERT_EQUAL_UINT8(0x01u, plan.proposed_progress.executed_mask);

    /* 05:09:59 -> still on time; 05:10:01 -> beyond the on-time window */
    weather_schedule_evaluate(&c, &p,
                              at_local(SUMMER_DAY, 300) +
                                  (uint64_t)WEATHER_SCHEDULE_ON_TIME_WINDOW_S,
                              true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DUE, plan.decision);
    weather_schedule_evaluate(&c, &p,
                              at_local(SUMMER_DAY, 300) +
                                  (uint64_t)WEATHER_SCHEDULE_ON_TIME_WINDOW_S + 1ull,
                              true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, plan.decision);
    TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_CATCH_UP_DISABLED, plan.reason);

    /* between slots with slot 0 executed */
    p.executed_mask = 0x01u;
    weather_schedule_evaluate(&c, &p, at_local(SUMMER_DAY, 500), true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, plan.decision);
    TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_BETWEEN_SLOTS, plan.reason);
    TEST_ASSERT_EQUAL_UINT64(at_local(SUMMER_DAY, 660), plan.next_due_utc_s);

    /* after the last slot, everything executed: next due = tomorrow 05:00 */
    p.executed_mask = 0x07u;
    weather_schedule_evaluate(&c, &p, at_local(SUMMER_DAY, 1000), true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, plan.decision);
    TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_ALL_EXECUTED, plan.reason);
    TEST_ASSERT_TRUE(plan.next_due_valid);
    TEST_ASSERT_EQUAL_UINT64(at_local(d(2026, 7, 16), 300), plan.next_due_utc_s);
}

TEST_CASE("schedule: exactly-once — duplicate wakeups and executed bits", "[local_schedule]")
{
    WeatherScheduleConfig c = cfg_enabled();
    WeatherScheduleProgress p = prog_for(WINTER_DAY);
    WeatherSchedulePlan a, b;
    uint64_t now = at_local(WINTER_DAY, 300);

    weather_schedule_evaluate(&c, &p, now, true, &a);
    weather_schedule_evaluate(&c, &p, now, true, &b);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DUE, a.decision);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a)); /* pure: identical plans */

    /* applying the proposal makes the slot non-due forever today */
    p = a.proposed_progress;
    weather_schedule_evaluate(&c, &p, now, true, &b);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, b.decision);
    TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_BETWEEN_SLOTS, b.reason);

    /* a backward time correction never re-runs the executed slot */
    weather_schedule_evaluate(&c, &p, at_local(WINTER_DAY, 299), true, &b);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, b.decision);
}

TEST_CASE("schedule: day rollover proposes fresh progress; regression refuses", "[local_schedule]")
{
    WeatherScheduleConfig c = cfg_enabled();
    WeatherScheduleProgress p = prog_for(SUMMER_DAY);
    WeatherSchedulePlan plan;
    p.executed_mask = 0x07u;

    /* forward rollover */
    weather_schedule_evaluate(&c, &p, at_local(d(2026, 7, 16), 100), true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DAY_ROLLOVER, plan.decision);
    TEST_ASSERT_TRUE(plan.proposal_present);
    TEST_ASSERT_EQUAL_UINT8(0, plan.proposed_progress.executed_mask);
    TEST_ASSERT_EQUAL_UINT16(2026, plan.proposed_progress.date.year);
    TEST_ASSERT_EQUAL_UINT8(16, plan.proposed_progress.date.day);

    /* backward local date: refuse execution, keep progress untouched */
    weather_schedule_evaluate(&c, &p, at_local(d(2026, 7, 14), 600), true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, plan.decision);
    TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_DATE_REGRESSION, plan.reason);
    TEST_ASSERT_FALSE(plan.proposal_present);
}

TEST_CASE("schedule: bounded catch-up selects only the LATEST missed slot", "[local_schedule]")
{
    WeatherScheduleConfig c = cfg_enabled();
    WeatherScheduleProgress p = prog_for(SUMMER_DAY);
    WeatherSchedulePlan plan;
    c.catch_up_window_s = 7200u; /* synthetic bounded test value */

    /* 12:30 local: slots 05:00 and 11:00 both missed -> only 11:00 (the
     * latest) is offered, never a burst. */
    weather_schedule_evaluate(&c, &p, at_local(SUMMER_DAY, 750), true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_CATCH_UP_DUE, plan.decision);
    TEST_ASSERT_EQUAL_INT8(1, plan.slot_index);
    TEST_ASSERT_EQUAL_UINT8(0x02u, plan.proposed_progress.executed_mask);

    /* outside the window: 11:00 missed by 2h1s */
    weather_schedule_evaluate(&c, &p,
                              at_local(SUMMER_DAY, 660) + 7201ull, true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, plan.decision);
    TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_MISSED_OUTSIDE_WINDOW, plan.reason);

    /* exact window boundary is still catch-up */
    weather_schedule_evaluate(&c, &p,
                              at_local(SUMMER_DAY, 660) + 7200ull, true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_CATCH_UP_DUE, plan.decision);

    /* already executed: no catch-up */
    p.executed_mask = 0x02u;
    weather_schedule_evaluate(&c, &p, at_local(SUMMER_DAY, 750), true, &plan);
    TEST_ASSERT_NOT_EQUAL(WEATHER_SCHEDULE_CATCH_UP_DUE, plan.decision);

    /* catch-up disabled (default 0): plain NOT_DUE */
    c.catch_up_window_s = 0u;
    p.executed_mask = 0u;
    weather_schedule_evaluate(&c, &p, at_local(SUMMER_DAY, 750), true, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, plan.decision);
    TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_CATCH_UP_DISABLED, plan.reason);

    /* untrusted time: no catch-up of any kind */
    c.catch_up_window_s = 7200u;
    weather_schedule_evaluate(&c, &p, at_local(SUMMER_DAY, 750), false, &plan);
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_TIME_UNTRUSTED, plan.decision);
}

TEST_CASE("schedule: DST edges — gap-adjusted slot and no autumn double run", "[local_schedule]")
{
    WeatherScheduleConfig c = cfg_enabled();
    WeatherSchedulePlan plan;
    WeatherLocalDate gap_day = d(2025, 3, 30);
    WeatherLocalDate rep_day = d(2025, 10, 26);

    /* A configured 02:30 slot on the spring-gap day resolves to the first
     * valid instant (01:00Z) and is marked DST-adjusted; it runs once. */
    c.slot_count = 1;
    memset(c.slots_min, 0, sizeof(c.slots_min));
    c.slots_min[0] = 150; /* 02:30 local */
    {
        WeatherScheduleProgress p = prog_for(gap_day);
        uint64_t spring = brussels_spring_transition_utc(2025);
        weather_schedule_evaluate(&c, &p, spring, true, &plan);
        TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DUE, plan.decision);
        TEST_ASSERT_TRUE(plan.slot_dst_adjusted);
        TEST_ASSERT_EQUAL_UINT64(spring, plan.slot_utc_s);
        p = plan.proposed_progress;
        weather_schedule_evaluate(&c, &p, spring + 60ull, true, &plan);
        TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, plan.decision);
    }

    /* A 02:30 slot on the autumn-overlap day uses the FIRST occurrence;
     * during the SECOND occurrence of 02:30 local the executed bit blocks
     * any re-run. */
    {
        WeatherScheduleProgress p = prog_for(rep_day);
        uint64_t autumn = brussels_autumn_transition_utc(2025);
        uint64_t first = autumn - 1800ull;  /* 02:30 CEST */
        uint64_t second = autumn + 1800ull; /* 02:30 CET  */
        weather_schedule_evaluate(&c, &p, first, true, &plan);
        TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DUE, plan.decision);
        TEST_ASSERT_FALSE(plan.slot_dst_adjusted);
        TEST_ASSERT_EQUAL_UINT64(first, plan.slot_utc_s);
        p = plan.proposed_progress;
        weather_schedule_evaluate(&c, &p, second, true, &plan);
        TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, plan.decision);
        TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_ALL_EXECUTED, plan.reason);
    }

    /* Default product slots resolve unambiguously on both edge days. */
    {
        WeatherScheduleConfig def = cfg_enabled();
        WeatherScheduleProgress p = prog_for(gap_day);
        weather_schedule_evaluate(&def, &p, at_local(gap_day, 300), true, &plan);
        TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DUE, plan.decision);
        TEST_ASSERT_FALSE(plan.slot_dst_adjusted);
        p = prog_for(rep_day);
        weather_schedule_evaluate(&def, &p, at_local(rep_day, 300), true, &plan);
        TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DUE, plan.decision);
        TEST_ASSERT_FALSE(plan.slot_dst_adjusted);
    }
}

TEST_CASE("schedule: next-due is always in the future (no busy polling)", "[local_schedule]")
{
    WeatherScheduleConfig c = cfg_enabled();
    WeatherScheduleProgress p = prog_for(SUMMER_DAY);
    WeatherSchedulePlan plan;
    static const uint16_t probes[] = { 0, 240, 299, 400, 659, 800, 899, 1100, 1439 };
    size_t i;
    for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        uint64_t now = at_local(SUMMER_DAY, probes[i]);
        weather_schedule_evaluate(&c, &p, now, true, &plan);
        if (plan.next_due_valid) {
            TEST_ASSERT_TRUE(plan.next_due_utc_s > now);
        }
    }
}

TEST_CASE("schedule: token strings are total", "[local_schedule]")
{
    int i;
    for (i = 0; i < WEATHER_SCHEDULE__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN",
            weather_schedule_decision_str((WeatherScheduleDecision)i)));
    }
    for (i = 0; i < WEATHER_SCHED_REASON__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN",
            weather_schedule_reason_str((WeatherScheduleReason)i)));
    }
    TEST_ASSERT_EQUAL_STRING("UNKNOWN",
        weather_schedule_decision_str((WeatherScheduleDecision)99));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN",
        weather_schedule_reason_str((WeatherScheduleReason)99));
}
