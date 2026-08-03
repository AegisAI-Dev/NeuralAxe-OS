/*
 * Deterministic tests for the pure retry model and daily call budget (W3).
 * Monotonic values and dates are synthetic inputs; no clocks, no network.
 */

#include <string.h>
#include "unity.h"
#include "weather_retry.h"

static WeatherLocalDate d(uint16_t y, uint8_t m, uint8_t dd)
{
    WeatherLocalDate x = { y, m, dd };
    return x;
}

/* ---------------- retry ---------------- */

TEST_CASE("retry: first and final attempt with monotonic backoff", "[weather_retry]")
{
    WeatherRetryState st;
    uint8_t attempt = 0xFF;

    weather_retry_begin(&st, 41u);
    TEST_ASSERT_EQUAL_UINT8(0, st.attempts_done);
    TEST_ASSERT_FALSE(st.terminal_failure);

    /* attempt 0 (first attempt) */
    TEST_ASSERT_TRUE(weather_retry_attempt_start(&st, 41u, 1000ull, &attempt));
    TEST_ASSERT_EQUAL_UINT8(0, attempt);
    /* a second start while in flight is refused */
    TEST_ASSERT_FALSE(weather_retry_attempt_start(&st, 41u, 1000ull, &attempt));
    /* failure schedules the backoff: next eligible = now + 1*base */
    TEST_ASSERT_TRUE(weather_retry_note_result(&st, 41u, 0, false,
                                               WEATHER_PROVIDER_ERR_READ_TIMEOUT,
                                               1000ull));
    TEST_ASSERT_EQUAL_UINT8(1, st.attempts_done);
    TEST_ASSERT_EQUAL_UINT64(1000ull + WEATHER_RETRY_BACKOFF_BASE_US,
                             st.next_eligible_monotonic_us);

    /* exact boundary: one microsecond early refused, at boundary allowed */
    TEST_ASSERT_FALSE(weather_retry_attempt_start(
        &st, 41u, 1000ull + WEATHER_RETRY_BACKOFF_BASE_US - 1ull, &attempt));
    TEST_ASSERT_TRUE(weather_retry_attempt_start(
        &st, 41u, 1000ull + WEATHER_RETRY_BACKOFF_BASE_US, &attempt));
    TEST_ASSERT_EQUAL_UINT8(1, attempt); /* second and FINAL attempt */

    /* final failure exhausts the sequence */
    TEST_ASSERT_TRUE(weather_retry_note_result(&st, 41u, 1, false,
                                               WEATHER_PROVIDER_ERR_HTTP_STATUS,
                                               2000000ull));
    TEST_ASSERT_TRUE(st.terminal_failure);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_ERR_RETRY_EXHAUSTED, st.last_result);
    TEST_ASSERT_FALSE(weather_retry_attempt_start(&st, 41u, 9000000000ull, &attempt));
}

TEST_CASE("retry: success ends the sequence; new execution id resets", "[weather_retry]")
{
    WeatherRetryState st;
    uint8_t attempt;
    weather_retry_begin(&st, 7u);
    TEST_ASSERT_TRUE(weather_retry_attempt_start(&st, 7u, 10ull, &attempt));
    TEST_ASSERT_TRUE(weather_retry_note_result(&st, 7u, 0, true,
                                               WEATHER_PROVIDER_OK, 20ull));
    TEST_ASSERT_TRUE(st.succeeded);
    TEST_ASSERT_FALSE(weather_retry_attempt_start(&st, 7u, 30ull, &attempt));

    /* a new local-date/slot gets a fresh bounded sequence */
    weather_retry_begin(&st, 8u);
    TEST_ASSERT_TRUE(weather_retry_attempt_start(&st, 8u, 40ull, &attempt));
    TEST_ASSERT_EQUAL_UINT8(0, attempt);
}

TEST_CASE("retry: stale ids and duplicate events are idempotent", "[weather_retry]")
{
    WeatherRetryState st;
    uint8_t attempt;
    weather_retry_begin(&st, 100u);
    TEST_ASSERT_TRUE(weather_retry_attempt_start(&st, 100u, 0ull, &attempt));

    /* stale execution id: rejected without change */
    TEST_ASSERT_FALSE(weather_retry_note_result(&st, 99u, 0, false,
                                                WEATHER_PROVIDER_ERR_TLS_FAILURE,
                                                1ull));
    TEST_ASSERT_TRUE(st.in_flight);
    /* wrong attempt number: rejected */
    TEST_ASSERT_FALSE(weather_retry_note_result(&st, 100u, 1, false,
                                                WEATHER_PROVIDER_ERR_TLS_FAILURE,
                                                1ull));
    /* real failure accepted once; the duplicate is a no-op */
    TEST_ASSERT_TRUE(weather_retry_note_result(&st, 100u, 0, false,
                                               WEATHER_PROVIDER_ERR_TLS_FAILURE,
                                               1ull));
    TEST_ASSERT_FALSE(weather_retry_note_result(&st, 100u, 0, false,
                                                WEATHER_PROVIDER_ERR_TLS_FAILURE,
                                                2ull));
    TEST_ASSERT_EQUAL_UINT8(1, st.attempts_done);
    /* stale start attempts are refused */
    TEST_ASSERT_FALSE(weather_retry_attempt_start(&st, 99u, 9ull, &attempt));
    TEST_ASSERT_FALSE(weather_retry_attempt_start(NULL, 100u, 9ull, &attempt));
    TEST_ASSERT_FALSE(weather_retry_attempt_start(&st, 100u, 9ull, NULL));
}

TEST_CASE("retry: backoff overflow saturates instead of wrapping", "[weather_retry]")
{
    WeatherRetryState st;
    uint8_t attempt;
    weather_retry_begin(&st, 5u);
    TEST_ASSERT_TRUE(weather_retry_attempt_start(&st, 5u, UINT64_MAX - 10ull, &attempt));
    TEST_ASSERT_TRUE(weather_retry_note_result(&st, 5u, 0, false,
                                               WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT,
                                               UINT64_MAX - 10ull));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, st.next_eligible_monotonic_us);
    TEST_ASSERT_FALSE(st.terminal_failure); /* one attempt remains, gated */
}

/* ---------------- daily budget ---------------- */

TEST_CASE("budget: three scheduled calls plus retries fit the maximum", "[weather_retry]")
{
    WeatherDailyBudget b;
    WeatherLocalDate today = d(2026, 7, 15);
    uint8_t max = 8; /* synthetic bounded product budget */
    int i;

    weather_budget_init(&b, &today);
    for (i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(weather_budget_allows(&b, WEATHER_CALL_SCHEDULED, max));
        TEST_ASSERT_TRUE(weather_budget_note_call(&b, WEATHER_CALL_SCHEDULED));
    }
    for (i = 0; i < 3; i++) { /* one retry per slot at most (W2 bound 2) */
        TEST_ASSERT_TRUE(weather_budget_allows(&b, WEATHER_CALL_RETRY, max));
        TEST_ASSERT_TRUE(weather_budget_note_call(&b, WEATHER_CALL_RETRY));
    }
    TEST_ASSERT_EQUAL_UINT8(6, weather_budget_used(&b));
    TEST_ASSERT_TRUE(weather_budget_allows(&b, WEATHER_CALL_MANUAL, max));
}

TEST_CASE("budget: exhaustion, invalid maxima and saturation", "[weather_retry]")
{
    WeatherDailyBudget b;
    WeatherLocalDate today = d(2026, 7, 15);
    int i;
    weather_budget_init(&b, &today);
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(weather_budget_note_call(&b, WEATHER_CALL_SCHEDULED));
    }
    TEST_ASSERT_FALSE(weather_budget_allows(&b, WEATHER_CALL_SCHEDULED, 4));
    TEST_ASSERT_FALSE(weather_budget_allows(&b, WEATHER_CALL_RETRY, 4));
    TEST_ASSERT_TRUE(weather_budget_allows(&b, WEATHER_CALL_SCHEDULED, 5));

    /* invalid configuration fails closed */
    TEST_ASSERT_FALSE(weather_budget_allows(&b, WEATHER_CALL_SCHEDULED, 0));
    TEST_ASSERT_FALSE(weather_budget_allows(&b, WEATHER_CALL_SCHEDULED,
                                            WEATHER_BUDGET_ABSOLUTE_MAX + 1u));
    TEST_ASSERT_FALSE(weather_budget_allows(&b, (WeatherCallKind)9, 5));
    TEST_ASSERT_FALSE(weather_budget_note_call(&b, (WeatherCallKind)9));
    TEST_ASSERT_FALSE(weather_budget_note_call(NULL, WEATHER_CALL_RETRY));

    /* counters saturate; the sum never wraps */
    for (i = 0; i < 300; i++) {
        (void)weather_budget_note_call(&b, WEATHER_CALL_RETRY);
    }
    TEST_ASSERT_EQUAL_UINT8(255, b.retry_calls);
    TEST_ASSERT_EQUAL_UINT8(255, weather_budget_used(&b));
}

TEST_CASE("budget: rollover only on a trusted FORWARD local date", "[weather_retry]")
{
    WeatherDailyBudget b, fresh;
    WeatherLocalDate today = d(2026, 7, 15);
    WeatherLocalDate tomorrow = d(2026, 7, 16);
    WeatherLocalDate yesterday = d(2026, 7, 14);

    weather_budget_init(&b, &today);
    (void)weather_budget_note_call(&b, WEATHER_CALL_SCHEDULED);

    /* same date: no reset (idempotent evaluation) */
    TEST_ASSERT_FALSE(weather_budget_propose_rollover(&b, &today, true, &fresh));
    /* backward date: a correction can never reset (nor reset twice) */
    TEST_ASSERT_FALSE(weather_budget_propose_rollover(&b, &yesterday, true, &fresh));
    /* untrusted time can never reset */
    TEST_ASSERT_FALSE(weather_budget_propose_rollover(&b, &tomorrow, false, &fresh));
    /* forward + trusted: fresh budget for the new date */
    TEST_ASSERT_TRUE(weather_budget_propose_rollover(&b, &tomorrow, true, &fresh));
    TEST_ASSERT_EQUAL_UINT8(0, weather_budget_used(&fresh));
    TEST_ASSERT_EQUAL_UINT8(16, fresh.date.day);
    /* the original is untouched by the proposal */
    TEST_ASSERT_EQUAL_UINT8(1, weather_budget_used(&b));
}
