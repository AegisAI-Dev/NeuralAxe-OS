/*
 * NeuralAxe — crash-safe schedule-window deduplication decision (Gate W6.4).
 *
 * Pure and total. See nx_weather_window.h for the at-most-once contract and
 * the ordering that makes it true.
 */

#include <string.h>

#include "nx_weather_window.h"

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */

const char *nx_weather_window_decision_str(NxWeatherWindowDecision d)
{
    switch (d) {
    case NX_WX_WINDOW_UNAVAILABLE:     return "WIN_UNAVAILABLE";
    case NX_WX_WINDOW_NOT_DUE:         return "WIN_NOT_DUE";
    case NX_WX_WINDOW_ALREADY_CLAIMED: return "WIN_ALREADY_CLAIMED";
    case NX_WX_WINDOW_DAY_REGRESSION:  return "WIN_DAY_REGRESSION";
    case NX_WX_WINDOW_CLAIM_REQUIRED:  return "WIN_CLAIM_REQUIRED";
    default:                           return "WIN_UNAVAILABLE";
    }
}

bool nx_weather_window_requires_claim(NxWeatherWindowDecision d)
{
    return d == NX_WX_WINDOW_CLAIM_REQUIRED;
}

/* ------------------------------------------------------------------ */
/* Identity conversion                                                 */
/* ------------------------------------------------------------------ */

bool nx_weather_window_date_from_state(const TuningScheduleWindowState *st,
                                       WeatherLocalDate *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (st == NULL || !st->present) {
        return false;
    }
    out->year  = st->year;
    out->month = st->month;
    out->day   = st->day;
    /* A persisted date that is not a valid calendar date is refused rather
     * than repaired: the caller must fail closed, never guess a day. */
    return brussels_date_valid(out);
}

void nx_weather_window_state_from_date(const WeatherLocalDate *date,
                                       uint8_t served_mask,
                                       TuningScheduleWindowState *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (date == NULL) {
        return;    /* stays the canonical absent state */
    }
    out->present     = true;
    out->year        = date->year;
    out->month       = date->month;
    out->day         = date->day;
    out->served_mask = served_mask;
}

/* ------------------------------------------------------------------ */
/* THE decision                                                        */
/* ------------------------------------------------------------------ */

NxWeatherWindowDecision nx_weather_window_decide(
    const WeatherSchedulePlan *plan,
    const TuningScheduleWindowState *persisted,
    uint8_t slot_count,
    TuningScheduleWindowState *out_next)
{
    WeatherLocalDate candidate;
    WeatherLocalDate claimed;
    uint8_t          slot_bit;
    uint8_t          base_mask = 0u;

    /* Fail closed FIRST: an ignored return value must never leave a
     * commitable claim behind. */
    if (out_next != NULL) {
        memset(out_next, 0, sizeof(*out_next));
    }
    if (plan == NULL || persisted == NULL || out_next == NULL) {
        return NX_WX_WINDOW_UNAVAILABLE;
    }
    if (slot_count == 0u || slot_count > WEATHER_SCHEDULE_MAX_SLOTS) {
        return NX_WX_WINDOW_UNAVAILABLE;
    }

    /* Only an actually open window is a candidate. Every other decision the
     * committed schedule can reach — DISABLED, TIME_UNTRUSTED, NOT_DUE,
     * DAY_ROLLOVER, CONFIG_INVALID — opens nothing and must never claim. */
    if (plan->decision != WEATHER_SCHEDULE_DUE &&
        plan->decision != WEATHER_SCHEDULE_CATCH_UP_DUE) {
        return NX_WX_WINDOW_NOT_DUE;
    }
    if (plan->slot_index < 0 ||
        (uint8_t)plan->slot_index >= slot_count) {
        return NX_WX_WINDOW_UNAVAILABLE;
    }
    if (!plan->proposal_present) {
        /* A due plan without a proposal cannot name the window it opened. */
        return NX_WX_WINDOW_UNAVAILABLE;
    }
    slot_bit = (uint8_t)(1u << (uint8_t)plan->slot_index);

    candidate = plan->proposed_progress.date;
    if (!brussels_date_valid(&candidate)) {
        return NX_WX_WINDOW_UNAVAILABLE;
    }

    if (persisted->present) {
        int cmp;

        if (!nx_weather_window_date_from_state(persisted, &claimed)) {
            /* Present but undecodable: refuse rather than treat as a fresh
             * day, which would re-open every window of the claimed day. */
            return NX_WX_WINDOW_UNAVAILABLE;
        }
        cmp = brussels_date_compare(&candidate, &claimed);
        if (cmp < 0) {
            /*
             * The candidate service day is OLDER than the durably claimed one.
             * Trusted time has moved backwards across a reboot. Re-opening a
             * day whose windows were already spent is exactly the duplicate
             * this gate exists to prevent, so refuse. The committed schedule
             * makes the same refusal against its RAM progress; this makes it
             * survive a reboot.
             */
            return NX_WX_WINDOW_DAY_REGRESSION;
        }
        if (cmp == 0) {
            if ((persisted->served_mask & slot_bit) != 0u) {
                return NX_WX_WINDOW_ALREADY_CLAIMED;
            }
            /* Same day, a slot not yet claimed: extend the persisted mask.
             * The base is the DURABLE mask, never the plan's RAM mask. */
            base_mask = persisted->served_mask;
        }
        /* cmp > 0: a newer service day. base_mask stays 0 so the single
         * commit below atomically replaces the old day AND its mask — this is
         * why a day rollover needs no separate write. */
    }

    nx_weather_window_state_from_date(&candidate,
                                      (uint8_t)(base_mask | slot_bit),
                                      out_next);
    return NX_WX_WINDOW_CLAIM_REQUIRED;
}

/* ------------------------------------------------------------------ */
/* Projection back onto the committed RAM progress model               */
/* ------------------------------------------------------------------ */

bool nx_weather_window_progress_from_state(const TuningScheduleWindowState *st,
                                           const WeatherLocalDate *today,
                                           WeatherScheduleProgress *out)
{
    WeatherLocalDate claimed;

    if (out == NULL || today == NULL) {
        return false;
    }
    /* Always a valid, cleared progress for today first: a caller that ignores
     * the result still gets a safe starting point rather than stale bits. */
    weather_schedule_progress_init(out, today);

    if (st == NULL || !st->present) {
        return false;
    }
    if (!nx_weather_window_date_from_state(st, &claimed)) {
        return false;
    }
    if (brussels_date_compare(&claimed, today) != 0) {
        /*
         * The durable claim belongs to a different service day. Today starts
         * with an empty mask — and note this is NOT the place that refuses a
         * backward day: that refusal belongs to the claim decision, which sees
         * the plan. Here we only project.
         */
        return false;
    }
    out->executed_mask = st->served_mask;
    return true;
}
