/*
 * NeuralAxe Weather-Aware Tuning — pure bounded retry model + daily call
 * budget (Gate W3). PURE: no tasks, no timers, no clocks, no logging.
 */

#include <string.h>
#include "weather_retry.h"

/* ---------------- retry (Stage 14) ---------------- */

void weather_retry_begin(WeatherRetryState *st, uint32_t execution_id)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
    st->execution_id = execution_id;
    st->last_result = WEATHER_PROVIDER_OK;
}

bool weather_retry_attempt_start(WeatherRetryState *st, uint32_t execution_id,
                                 uint64_t now_monotonic_us,
                                 uint8_t *out_attempt_no)
{
    if (st == NULL || out_attempt_no == NULL) {
        return false;
    }
    if (st->execution_id != execution_id) {
        return false; /* stale/foreign execution */
    }
    if (st->terminal_failure || st->succeeded || st->in_flight) {
        return false;
    }
    if (st->attempts_done >= WEATHER_RETRY_ATTEMPTS_MAX) {
        st->terminal_failure = true; /* defensive; note_result sets this */
        return false;
    }
    if (now_monotonic_us < st->next_eligible_monotonic_us) {
        return false; /* monotonic backoff not yet elapsed */
    }
    st->in_flight = true;
    *out_attempt_no = st->attempts_done;
    return true;
}

bool weather_retry_note_result(WeatherRetryState *st, uint32_t execution_id,
                               uint8_t attempt_no, bool success,
                               WeatherProviderResult result,
                               uint64_t now_monotonic_us)
{
    if (st == NULL) {
        return false;
    }
    if (st->execution_id != execution_id) {
        return false; /* stale execution id: rejected, nothing changes */
    }
    if (!st->in_flight || attempt_no != st->attempts_done) {
        return false; /* duplicate/out-of-order event: idempotent no-op */
    }
    if (st->terminal_failure || st->succeeded) {
        return false;
    }

    st->in_flight = false;
    st->last_result = result;
    if (success) {
        st->succeeded = true; /* success ends the sequence */
        return true;
    }
    st->attempts_done++;
    if (st->attempts_done >= WEATHER_RETRY_ATTEMPTS_MAX) {
        st->terminal_failure = true;
        st->last_result = WEATHER_PROVIDER_ERR_RETRY_EXHAUSTED;
        return true;
    }
    /* Monotonic linear backoff: attempt N failed -> next eligible at
     * now + (N+1)*base. Never wall clock; overflow saturates. */
    {
        uint64_t delay =
            (uint64_t)st->attempts_done * WEATHER_RETRY_BACKOFF_BASE_US;
        uint64_t next = now_monotonic_us + delay;
        st->next_eligible_monotonic_us =
            (next < now_monotonic_us) ? UINT64_MAX : next;
    }
    return true;
}

/* ---------------- daily budget (Stage 15) ---------------- */

void weather_budget_init(WeatherDailyBudget *b, const WeatherLocalDate *date)
{
    if (b == NULL) {
        return;
    }
    memset(b, 0, sizeof(*b));
    if (date != NULL) {
        b->date = *date;
    }
}

uint8_t weather_budget_used(const WeatherDailyBudget *b)
{
    uint32_t sum;
    if (b == NULL) {
        return 0u;
    }
    sum = (uint32_t)b->scheduled_calls + (uint32_t)b->retry_calls +
          (uint32_t)b->manual_calls;
    return (sum > 255u) ? 255u : (uint8_t)sum;
}

bool weather_budget_allows(const WeatherDailyBudget *b, WeatherCallKind kind,
                           uint8_t daily_max)
{
    if (b == NULL || (unsigned)kind >= WEATHER_CALL__COUNT) {
        return false;
    }
    if (daily_max == 0u || daily_max > WEATHER_BUDGET_ABSOLUTE_MAX) {
        return false; /* invalid budget configuration fails closed */
    }
    return weather_budget_used(b) < daily_max;
}

bool weather_budget_note_call(WeatherDailyBudget *b, WeatherCallKind kind)
{
    uint8_t *slot;
    if (b == NULL) {
        return false;
    }
    switch (kind) {
    case WEATHER_CALL_SCHEDULED: slot = &b->scheduled_calls; break;
    case WEATHER_CALL_RETRY:     slot = &b->retry_calls;     break;
    case WEATHER_CALL_MANUAL:    slot = &b->manual_calls;    break;
    default:
        return false;
    }
    if (*slot != 255u) {
        (*slot)++; /* saturating; never wraps */
    }
    return true;
}

bool weather_budget_propose_rollover(const WeatherDailyBudget *b,
                                     const WeatherLocalDate *today,
                                     bool today_trusted,
                                     WeatherDailyBudget *out)
{
    if (b == NULL || today == NULL || out == NULL) {
        return false;
    }
    if (!today_trusted) {
        return false; /* untrusted time can never reset the budget */
    }
    if (!brussels_date_valid(today) || !brussels_date_valid(&b->date)) {
        return false;
    }
    if (brussels_date_compare(today, &b->date) <= 0) {
        /* Same date: no reset. Backward date: a correction must never
         * reset the budget (and can never reset it twice). */
        return false;
    }
    weather_budget_init(out, today);
    return true;
}
