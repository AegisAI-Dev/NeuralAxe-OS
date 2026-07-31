#ifndef WEATHER_RETRY_H_
#define WEATHER_RETRY_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "weather_provider.h"

/*
 * NeuralAxe Weather-Aware Tuning — pure bounded retry model and daily
 * call budget (Gate W3). No task, no timer, no clock reads: monotonic
 * microseconds and trusted local dates are caller-supplied inputs.
 */

/* ------------------------------------------------------------------ */
/* Retry model (Stage 14)                                              */
/* ------------------------------------------------------------------ */

/* Maximum attempts per scheduled check. Equals the committed W2
 * TUNING_MAX_RETRY_COUNT (2) by design — one initial attempt plus one
 * retry; attempt numbering is 0-based (attempt 0 = first attempt). */
#define WEATHER_RETRY_ATTEMPTS_MAX 2u

/* Monotonic backoff after a failed attempt N (0-based): (N+1) * base.
 * Bounded, linear, no wall clock, no callback-driven retry. */
#define WEATHER_RETRY_BACKOFF_BASE_US 60000000ull /* 60 s */

typedef struct {
    uint32_t execution_id;   /* one schedule execution (date+slot scoped) */
    uint8_t attempts_done;   /* completed (failed) attempts so far        */
    bool in_flight;          /* an attempt is currently outstanding       */
    bool terminal_failure;   /* budget exhausted for this execution       */
    bool succeeded;          /* success ends the sequence                 */
    uint64_t next_eligible_monotonic_us; /* earliest next attempt          */
    WeatherProviderResult last_result;
} WeatherRetryState;

/* Begin a fresh bounded sequence for one schedule execution id (a new
 * local-date/slot gets a new id and therefore a fresh sequence). */
void weather_retry_begin(WeatherRetryState *st, uint32_t execution_id);

/* May an attempt start now? True only when: matching execution id, not
 * terminal, not succeeded, not in flight, attempts remain, and
 * now >= next_eligible. Marks the attempt in flight on true. */
bool weather_retry_attempt_start(WeatherRetryState *st, uint32_t execution_id,
                                 uint64_t now_monotonic_us,
                                 uint8_t *out_attempt_no);

/* Report the outcome of attempt `attempt_no`. Stale execution ids and
 * duplicate/out-of-order attempt numbers are rejected (idempotent —
 * nothing changes). Success ends the sequence; a failure schedules the
 * monotonic backoff or marks terminal exhaustion. */
bool weather_retry_note_result(WeatherRetryState *st, uint32_t execution_id,
                               uint8_t attempt_no, bool success,
                               WeatherProviderResult result,
                               uint64_t now_monotonic_us);

/* ------------------------------------------------------------------ */
/* Daily call budget (Stage 15)                                        */
/* ------------------------------------------------------------------ */

/* Product-side request-budget bound (NOT a provider rate limit — provider
 * commercial limits are never encoded as firmware safety limits). The
 * hard daily maximum is a bounded policy input <= this model bound. */
#define WEATHER_BUDGET_ABSOLUTE_MAX 24u

typedef struct {
    WeatherLocalDate date;    /* trusted Brussels local date              */
    uint8_t scheduled_calls;
    uint8_t retry_calls;
    uint8_t manual_calls;     /* reserved for a future W5 surface         */
} WeatherDailyBudget;

typedef enum {
    WEATHER_CALL_SCHEDULED = 0,
    WEATHER_CALL_RETRY = 1,
    WEATHER_CALL_MANUAL = 2,
    WEATHER_CALL__COUNT
} WeatherCallKind;

void weather_budget_init(WeatherDailyBudget *b, const WeatherLocalDate *date);

/* Total calls consumed today (saturating sum). */
uint8_t weather_budget_used(const WeatherDailyBudget *b);

/* Idempotent evaluation: would one more call of `kind` fit under
 * `daily_max` (which must be 1..WEATHER_BUDGET_ABSOLUTE_MAX)? Exhausted
 * budgets produce no provider-call intent. */
bool weather_budget_allows(const WeatherDailyBudget *b, WeatherCallKind kind,
                           uint8_t daily_max);

/* Note one actually-performed call (saturating; never wraps). False when
 * the kind is invalid. */
bool weather_budget_note_call(WeatherDailyBudget *b, WeatherCallKind kind);

/*
 * Local-date rollover proposal: resets the counters ONLY for a trusted,
 * strictly FORWARD date change. A backward date never resets (the budget
 * cannot be reset twice by a correction) and untrusted time never resets.
 * Returns true when *out contains a fresh budget for `today`.
 */
bool weather_budget_propose_rollover(const WeatherDailyBudget *b,
                                     const WeatherLocalDate *today,
                                     bool today_trusted,
                                     WeatherDailyBudget *out);

#endif /* WEATHER_RETRY_H_ */
