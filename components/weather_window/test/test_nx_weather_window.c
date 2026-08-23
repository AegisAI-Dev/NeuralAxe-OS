/*
 * Gate W6.4 — schedule-window deduplication decision tests.
 *
 * Pure: no NVS, no storage backend, no clock, no network, no hardware. These
 * pin the DECISION half of the at-most-once contract; the persistence half
 * (crash-safe claim commit, power-loss injection, reboot recovery) is proven
 * in the store and production-wiring suites.
 */

#include <string.h>

#include "unity.h"

#include "nx_weather_window.h"

/* A committed-shape schedule: the three product slots, catch-up disabled. */
#define W64_SLOTS 3u

static WeatherLocalDate mkdate(uint16_t y, uint8_t m, uint8_t d)
{
    WeatherLocalDate x;
    x.year = y; x.month = m; x.day = d;
    return x;
}

/* A DUE plan for `slot` on `date`, shaped exactly as the committed schedule
 * emits one (proposal present, executed bit proposed). */
static WeatherSchedulePlan due_plan(WeatherLocalDate date, int8_t slot,
                                    uint8_t ram_mask)
{
    WeatherSchedulePlan p;

    memset(&p, 0, sizeof(p));
    p.decision   = WEATHER_SCHEDULE_DUE;
    p.slot_index = slot;
    p.reason     = WEATHER_SCHED_REASON_ON_TIME;
    weather_schedule_progress_init(&p.proposed_progress, &date);
    p.proposed_progress.executed_mask =
        (uint8_t)(ram_mask | (uint8_t)(1u << (uint8_t)slot));
    p.proposal_present = true;
    return p;
}

static TuningScheduleWindowState claimed(WeatherLocalDate d, uint8_t mask)
{
    TuningScheduleWindowState s;
    nx_weather_window_state_from_date(&d, mask, &s);
    return s;
}

static TuningScheduleWindowState absent(void)
{
    TuningScheduleWindowState s;
    memset(&s, 0, sizeof(s));
    return s;
}

/* ================================================================== */
/* A — the fail-closed surface                                         */
/* ================================================================== */

TEST_CASE("w64: NULL and out-of-range inputs fail closed", "[weather_window]")
{
    WeatherSchedulePlan       p = due_plan(mkdate(2026, 7, 15), 0, 0u);
    TuningScheduleWindowState st = absent();
    TuningScheduleWindowState next;

    TEST_ASSERT_EQUAL(NX_WX_WINDOW_UNAVAILABLE,
                      nx_weather_window_decide(NULL, &st, W64_SLOTS, &next));
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_UNAVAILABLE,
                      nx_weather_window_decide(&p, NULL, W64_SLOTS, &next));
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_UNAVAILABLE,
                      nx_weather_window_decide(&p, &st, W64_SLOTS, NULL));
    /* slot_count of 0 or beyond the committed maximum is not a schedule. */
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_UNAVAILABLE,
                      nx_weather_window_decide(&p, &st, 0u, &next));
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_UNAVAILABLE,
                      nx_weather_window_decide(&p, &st,
                                               WEATHER_SCHEDULE_MAX_SLOTS + 1u,
                                               &next));

    /* A slot beyond the configured count cannot be claimed. */
    p = due_plan(mkdate(2026, 7, 15), 4, 0u);
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_UNAVAILABLE,
                      nx_weather_window_decide(&p, &st, W64_SLOTS, &next));

    /* An out-of-range decision token still maps to the fail-closed string. */
    TEST_ASSERT_NOT_NULL(nx_weather_window_decision_str(
                             (NxWeatherWindowDecision)(NX_WX_WINDOW__COUNT + 3)));
    TEST_ASSERT_FALSE(nx_weather_window_requires_claim(NX_WX_WINDOW_UNAVAILABLE));
    TEST_ASSERT_TRUE(nx_weather_window_requires_claim(NX_WX_WINDOW_CLAIM_REQUIRED));
}

TEST_CASE("w64: a refused decision never leaves a commitable claim",
          "[weather_window]")
{
    WeatherSchedulePlan       p = due_plan(mkdate(2026, 7, 15), 0, 0u);
    TuningScheduleWindowState st = absent();
    TuningScheduleWindowState next;

    /* Pre-dirty the output so a missing memset would be visible. */
    memset(&next, 0xAA, sizeof(next));
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_UNAVAILABLE,
                      nx_weather_window_decide(&p, &st, 0u, &next));
    TEST_ASSERT_FALSE(next.present);
    TEST_ASSERT_EQUAL_UINT8(0u, next.served_mask);
    TEST_ASSERT_EQUAL_UINT16(0u, next.year);
}

TEST_CASE("w64: every non-open schedule decision claims nothing",
          "[weather_window]")
{
    TuningScheduleWindowState st = absent();
    TuningScheduleWindowState next;
    WeatherScheduleDecision   d[] = {
        WEATHER_SCHEDULE_CONFIG_INVALID, WEATHER_SCHEDULE_DISABLED,
        WEATHER_SCHEDULE_TIME_UNTRUSTED, WEATHER_SCHEDULE_NOT_DUE,
        WEATHER_SCHEDULE_DAY_ROLLOVER
    };
    size_t i;

    for (i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
        WeatherSchedulePlan p = due_plan(mkdate(2026, 7, 15), 0, 0u);
        p.decision = d[i];
        TEST_ASSERT_EQUAL(NX_WX_WINDOW_NOT_DUE,
                          nx_weather_window_decide(&p, &st, W64_SLOTS, &next));
        TEST_ASSERT_FALSE(next.present);
    }
}

/* ================================================================== */
/* B — virgin store, first claim, suppression                          */
/* ================================================================== */

TEST_CASE("w64: a virgin store claims the window exactly once",
          "[weather_window]")
{
    WeatherLocalDate          day = mkdate(2026, 7, 15);
    WeatherSchedulePlan       p   = due_plan(day, 0, 0u);
    TuningScheduleWindowState st  = absent();
    TuningScheduleWindowState next;

    /* Virgin is NOT corruption and NOT "already served". */
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_CLAIM_REQUIRED,
                      nx_weather_window_decide(&p, &st, W64_SLOTS, &next));
    TEST_ASSERT_TRUE(next.present);
    TEST_ASSERT_EQUAL_UINT16(2026u, next.year);
    TEST_ASSERT_EQUAL_UINT8(7u, next.month);
    TEST_ASSERT_EQUAL_UINT8(15u, next.day);
    TEST_ASSERT_EQUAL_UINT8(0x01u, next.served_mask);

    /* Once that state is durable, the SAME window is suppressed — this is the
     * reboot case: the persisted state is all that survived. */
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_ALREADY_CLAIMED,
                      nx_weather_window_decide(&p, &next, W64_SLOTS, &st));
    TEST_ASSERT_FALSE(st.present);   /* nothing to commit */
}

TEST_CASE("w64: a later slot on the same service day stays eligible once",
          "[weather_window]")
{
    WeatherLocalDate          day = mkdate(2026, 7, 15);
    TuningScheduleWindowState st  = claimed(day, 0x01u);   /* slot 0 spent */
    TuningScheduleWindowState next;
    WeatherSchedulePlan       p1  = due_plan(day, 1, 0x01u);

    TEST_ASSERT_EQUAL(NX_WX_WINDOW_CLAIM_REQUIRED,
                      nx_weather_window_decide(&p1, &st, W64_SLOTS, &next));
    /* The durable mask is EXTENDED, never replaced: slot 0 stays spent. */
    TEST_ASSERT_EQUAL_UINT8(0x03u, next.served_mask);
    TEST_ASSERT_EQUAL_UINT8(15u, next.day);

    /* And it is then suppressed in turn. */
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_ALREADY_CLAIMED,
                      nx_weather_window_decide(&p1, &next, W64_SLOTS, &st));

    /* Slot 2 remains independently eligible. */
    {
        WeatherSchedulePlan p2 = due_plan(day, 2, 0x03u);
        TuningScheduleWindowState n2;
        TEST_ASSERT_EQUAL(NX_WX_WINDOW_CLAIM_REQUIRED,
                          nx_weather_window_decide(&p2, &next, W64_SLOTS, &n2));
        TEST_ASSERT_EQUAL_UINT8(0x07u, n2.served_mask);
    }
}

TEST_CASE("w64: the next mask comes from DURABLE state, never the RAM plan",
          "[weather_window]")
{
    WeatherLocalDate          day = mkdate(2026, 7, 15);
    TuningScheduleWindowState st  = claimed(day, 0x01u);
    TuningScheduleWindowState next;
    /*
     * The plan's RAM mask falsely claims slots 0,1,2 were executed this boot
     * while only slot 0 was ever durably committed. If the decision trusted
     * the plan it would persist 0x07 and silently lose slot 1 and 2 forever.
     */
    WeatherSchedulePlan p = due_plan(day, 1, 0x07u);

    TEST_ASSERT_EQUAL(NX_WX_WINDOW_CLAIM_REQUIRED,
                      nx_weather_window_decide(&p, &st, W64_SLOTS, &next));
    TEST_ASSERT_EQUAL_UINT8(0x03u, next.served_mask);  /* not 0x07 */
}

/* ================================================================== */
/* C — day rollover and day regression                                 */
/* ================================================================== */

TEST_CASE("w64: a new service day replaces the old day and mask in ONE claim",
          "[weather_window]")
{
    TuningScheduleWindowState st = claimed(mkdate(2026, 7, 15), 0x07u);
    TuningScheduleWindowState next;
    WeatherSchedulePlan       p  = due_plan(mkdate(2026, 7, 16), 0, 0u);

    TEST_ASSERT_EQUAL(NX_WX_WINDOW_CLAIM_REQUIRED,
                      nx_weather_window_decide(&p, &st, W64_SLOTS, &next));
    /* Yesterday's full mask does NOT suppress today, and no separate
     * rollover write was needed to clear it. */
    TEST_ASSERT_EQUAL_UINT8(16u, next.day);
    TEST_ASSERT_EQUAL_UINT8(0x01u, next.served_mask);
}

TEST_CASE("w64: an OLDER service day cannot reopen a spent window",
          "[weather_window]")
{
    TuningScheduleWindowState st = claimed(mkdate(2026, 7, 16), 0x01u);
    TuningScheduleWindowState next;
    /* Trusted time moved backwards across a reboot. */
    WeatherSchedulePlan       p  = due_plan(mkdate(2026, 7, 15), 0, 0u);

    TEST_ASSERT_EQUAL(NX_WX_WINDOW_DAY_REGRESSION,
                      nx_weather_window_decide(&p, &st, W64_SLOTS, &next));
    TEST_ASSERT_FALSE(next.present);
}

TEST_CASE("w64: the same local time on a different day is a distinct window",
          "[weather_window]")
{
    TuningScheduleWindowState st = claimed(mkdate(2026, 7, 15), 0x01u);
    TuningScheduleWindowState next;
    WeatherSchedulePlan       p  = due_plan(mkdate(2026, 8, 15), 0, 0u);

    /* Same slot index, same wall-clock minute, different service day. */
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_CLAIM_REQUIRED,
                      nx_weather_window_decide(&p, &st, W64_SLOTS, &next));
    TEST_ASSERT_EQUAL_UINT8(8u, next.month);
    TEST_ASSERT_EQUAL_UINT8(0x01u, next.served_mask);
}

TEST_CASE("w64: a present-but-undecodable claim refuses rather than resets",
          "[weather_window]")
{
    TuningScheduleWindowState st;
    TuningScheduleWindowState next;
    WeatherSchedulePlan       p = due_plan(mkdate(2026, 7, 15), 0, 0u);

    memset(&st, 0, sizeof(st));
    st.present = true;
    st.year = 2026u; st.month = 13u; st.day = 40u;   /* impossible date */

    /* Treating this as "fresh day" would re-open every window of the claimed
     * day, so it must fail closed instead. */
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_UNAVAILABLE,
                      nx_weather_window_decide(&p, &st, W64_SLOTS, &next));
    TEST_ASSERT_FALSE(next.present);
}

TEST_CASE("w64: CATCH_UP_DUE claims exactly like DUE", "[weather_window]")
{
    WeatherLocalDate          day = mkdate(2026, 7, 15);
    TuningScheduleWindowState st  = absent();
    TuningScheduleWindowState next;
    WeatherSchedulePlan       p   = due_plan(day, 1, 0u);

    p.decision = WEATHER_SCHEDULE_CATCH_UP_DUE;
    p.reason   = WEATHER_SCHED_REASON_CATCH_UP;
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_CLAIM_REQUIRED,
                      nx_weather_window_decide(&p, &st, W64_SLOTS, &next));
    TEST_ASSERT_EQUAL_UINT8(0x02u, next.served_mask);
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_ALREADY_CLAIMED,
                      nx_weather_window_decide(&p, &next, W64_SLOTS, &st));
}

/* ================================================================== */
/* D — identity conversion and RAM projection                          */
/* ================================================================== */

TEST_CASE("w64: identity conversion round-trips and rejects absent state",
          "[weather_window]")
{
    WeatherLocalDate          day = mkdate(2026, 12, 31);
    TuningScheduleWindowState st;
    WeatherLocalDate          back;

    nx_weather_window_state_from_date(&day, 0x05u, &st);
    TEST_ASSERT_TRUE(st.present);
    TEST_ASSERT_TRUE(nx_weather_window_date_from_state(&st, &back));
    TEST_ASSERT_EQUAL_UINT16(2026u, back.year);
    TEST_ASSERT_EQUAL_UINT8(12u, back.month);
    TEST_ASSERT_EQUAL_UINT8(31u, back.day);

    /* Absent yields no date, and the output is cleared rather than stale. */
    {
        TuningScheduleWindowState none = absent();
        memset(&back, 0xEE, sizeof(back));
        TEST_ASSERT_FALSE(nx_weather_window_date_from_state(&none, &back));
        TEST_ASSERT_EQUAL_UINT16(0u, back.year);
    }
    /* A NULL date produces the canonical absent state, never a partial one. */
    {
        TuningScheduleWindowState s2;
        nx_weather_window_state_from_date(NULL, 0xFFu, &s2);
        TEST_ASSERT_FALSE(s2.present);
        TEST_ASSERT_EQUAL_UINT8(0u, s2.served_mask);
    }
}

TEST_CASE("w64: RAM progress is reconstructed from the durable authority",
          "[weather_window]")
{
    WeatherLocalDate          today = mkdate(2026, 7, 15);
    TuningScheduleWindowState st    = claimed(today, 0x03u);
    WeatherScheduleProgress   prog;

    /* Same day: the durable mask becomes the RAM mask, so the schedule
     * evaluates against what actually survived the reboot. */
    TEST_ASSERT_TRUE(nx_weather_window_progress_from_state(&st, &today, &prog));
    TEST_ASSERT_EQUAL_UINT8(0x03u, prog.executed_mask);
    TEST_ASSERT_EQUAL_UINT16(2026u, prog.date.year);
    TEST_ASSERT_EQUAL_UINT16(WEATHER_SCHEDULE_PROGRESS_VERSION, prog.version);

    /* A different day starts empty — and still returns a VALID progress. */
    {
        WeatherLocalDate tomorrow = mkdate(2026, 7, 16);
        TEST_ASSERT_FALSE(nx_weather_window_progress_from_state(&st, &tomorrow,
                                                                &prog));
        TEST_ASSERT_EQUAL_UINT8(0u, prog.executed_mask);
        TEST_ASSERT_EQUAL_UINT8(16u, prog.date.day);
    }

    /* No durable claim at all: today, empty, valid. */
    {
        TuningScheduleWindowState none = absent();
        TEST_ASSERT_FALSE(nx_weather_window_progress_from_state(&none, &today,
                                                                &prog));
        TEST_ASSERT_EQUAL_UINT8(0u, prog.executed_mask);
        TEST_ASSERT_EQUAL_UINT8(15u, prog.date.day);
    }
}

/* ================================================================== */
/* E — bounded write budget                                            */
/* ================================================================== */

TEST_CASE("w64: repeated ticks in a claimed window require no further claim",
          "[weather_window]")
{
    WeatherLocalDate          day = mkdate(2026, 7, 15);
    TuningScheduleWindowState st  = absent();
    TuningScheduleWindowState next;
    WeatherSchedulePlan       p   = due_plan(day, 0, 0u);
    unsigned                  claims = 0u;
    unsigned                  i;

    /* Thousands of statistics ticks inside one open window. */
    for (i = 0; i < 5000u; i++) {
        if (nx_weather_window_decide(&p, &st, W64_SLOTS, &next) ==
            NX_WX_WINDOW_CLAIM_REQUIRED) {
            claims++;
            st = next;          /* the durable commit the integrator performs */
        }
    }
    /* Exactly one durable claim for one window, regardless of tick count. */
    TEST_ASSERT_EQUAL_UINT(1u, claims);
    TEST_ASSERT_EQUAL_UINT8(0x01u, st.served_mask);
}

TEST_CASE("w64: a full service day costs at most one claim per slot",
          "[weather_window]")
{
    WeatherLocalDate          day = mkdate(2026, 7, 15);
    TuningScheduleWindowState st  = absent();
    unsigned                  claims = 0u;
    uint8_t                   slot;
    unsigned                  rep;

    for (slot = 0u; slot < W64_SLOTS; slot++) {
        for (rep = 0u; rep < 50u; rep++) {
            TuningScheduleWindowState next;
            WeatherSchedulePlan p = due_plan(day, (int8_t)slot, st.served_mask);
            if (nx_weather_window_decide(&p, &st, W64_SLOTS, &next) ==
                NX_WX_WINDOW_CLAIM_REQUIRED) {
                claims++;
                st = next;
            }
        }
    }
    /* The theoretical maximum is the committed slot count, not the tick or
     * retry count. */
    TEST_ASSERT_EQUAL_UINT(W64_SLOTS, claims);
    TEST_ASSERT_EQUAL_UINT8(0x07u, st.served_mask);
}
