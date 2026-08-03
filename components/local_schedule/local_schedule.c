/*
 * NeuralAxe Weather-Aware Tuning — pure bounded daily scheduler (Gate W3).
 * PURE: no tasks, no timers, no clocks, no NVS, no network, no logging.
 */

#include <string.h>
#include "local_schedule.h"

void weather_schedule_config_defaults(WeatherScheduleConfig *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->enabled = false; /* the feature ships disabled */
    out->slot_count = 3u;
    out->slots_min[0] = (uint16_t)WEATHER_SCHEDULE_DEFAULT_SLOT_1_MIN;
    out->slots_min[1] = (uint16_t)WEATHER_SCHEDULE_DEFAULT_SLOT_2_MIN;
    out->slots_min[2] = (uint16_t)WEATHER_SCHEDULE_DEFAULT_SLOT_3_MIN;
    out->catch_up_window_s = 0u; /* conservative: catch-up disabled until a
                                    committed product decision */
}

bool weather_schedule_config_valid(const WeatherScheduleConfig *c)
{
    uint8_t i;
    if (c == NULL) {
        return false;
    }
    if (c->slot_count == 0u || c->slot_count > WEATHER_SCHEDULE_MAX_SLOTS) {
        return false;
    }
    for (i = 0; i < WEATHER_SCHEDULE_MAX_SLOTS; i++) {
        if (i < c->slot_count) {
            if (c->slots_min[i] >= BRUSSELS_MINUTES_PER_DAY) {
                return false;
            }
            if (i > 0u && c->slots_min[i] <= c->slots_min[i - 1u]) {
                return false; /* strictly ascending, no duplicates */
            }
        } else if (c->slots_min[i] != 0u) {
            return false; /* canonical zeros past slot_count */
        }
    }
    if (c->catch_up_window_s > WEATHER_SCHEDULE_CATCH_UP_MAX_S) {
        return false;
    }
    return true;
}

void weather_schedule_progress_init(WeatherScheduleProgress *out,
                                    const WeatherLocalDate *date)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->version = (uint16_t)WEATHER_SCHEDULE_PROGRESS_VERSION;
    if (date != NULL) {
        out->date = *date;
    }
}

bool weather_schedule_progress_valid(const WeatherScheduleProgress *p,
                                     const WeatherScheduleConfig *c)
{
    if (p == NULL || c == NULL) {
        return false;
    }
    if (p->version != (uint16_t)WEATHER_SCHEDULE_PROGRESS_VERSION) {
        return false;
    }
    if (!brussels_date_valid(&p->date)) {
        return false;
    }
    /* Unknown mask bits (>= slot_count) are rejected, never ignored. */
    if (c->slot_count < 8u &&
        (p->executed_mask & (uint8_t)~((1u << c->slot_count) - 1u)) != 0u) {
        return false;
    }
    return true;
}

/* Resolve slot i of `date` to UTC per the DST edge policy. */
static bool slot_utc(const WeatherScheduleConfig *cfg,
                     const WeatherLocalDate *date, uint8_t i,
                     uint64_t *out_utc, bool *out_adjusted)
{
    BrusselsResolution res;
    if (!brussels_utc_from_local(date, cfg->slots_min[i], &res) || !res.valid) {
        return false;
    }
    *out_utc = res.utc_s;
    *out_adjusted = res.dst_adjusted;
    return true;
}

/* Next unexecuted slot strictly after `now`, today or (mask-free)
 * tomorrow. Returns false when none resolves. */
static bool next_due(const WeatherScheduleConfig *cfg,
                     const WeatherScheduleProgress *progress,
                     const BrusselsLocalTime *now_local, uint64_t now_utc,
                     uint64_t *out_utc)
{
    uint8_t i;
    uint64_t cand;
    bool adj;

    for (i = 0; i < cfg->slot_count; i++) {
        if ((progress->executed_mask & (uint8_t)(1u << i)) != 0u) {
            continue;
        }
        if (!slot_utc(cfg, &now_local->date, i, &cand, &adj)) {
            return false;
        }
        if (cand > now_utc) {
            *out_utc = cand;
            return true;
        }
    }
    /* First slot of the next local day. */
    {
        WeatherLocalDate next = now_local->date;
        uint8_t dim = brussels_days_in_month(next.year, next.month);
        if (next.day < dim) {
            next.day++;
        } else {
            next.day = 1u;
            if (next.month < 12u) {
                next.month++;
            } else {
                next.month = 1u;
                next.year++;
            }
        }
        if (!brussels_date_valid(&next)) {
            return false; /* band edge */
        }
        if (!slot_utc(cfg, &next, 0u, &cand, &adj)) {
            return false;
        }
        *out_utc = cand;
        return true;
    }
}

void weather_schedule_evaluate(const WeatherScheduleConfig *cfg,
                               const WeatherScheduleProgress *progress,
                               uint64_t now_utc_s, bool now_trusted,
                               WeatherSchedulePlan *out)
{
    BrusselsLocalTime now_local;
    int8_t latest = -1;
    uint8_t i;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->slot_index = -1;
    out->decision = WEATHER_SCHEDULE_CONFIG_INVALID; /* == 0, explicit */
    out->reason = WEATHER_SCHED_REASON_CONFIG_INVALID;

    if (!weather_schedule_config_valid(cfg)) {
        return;
    }
    if (!cfg->enabled) {
        out->decision = WEATHER_SCHEDULE_DISABLED;
        out->reason = WEATHER_SCHED_REASON_DISABLED;
        return;
    }
    if (!now_trusted) {
        /* No execution decision without operationally trusted time. */
        out->decision = WEATHER_SCHEDULE_TIME_UNTRUSTED;
        out->reason = WEATHER_SCHED_REASON_TIME_UNTRUSTED;
        return;
    }
    if (!brussels_local_from_utc(now_utc_s, &now_local)) {
        out->decision = WEATHER_SCHEDULE_TIME_UNTRUSTED;
        out->reason = WEATHER_SCHED_REASON_EPOCH_RANGE;
        return;
    }
    if (!weather_schedule_progress_valid(progress, cfg)) {
        out->reason = WEATHER_SCHED_REASON_PROGRESS_INVALID;
        return; /* CONFIG_INVALID posture: fail closed */
    }

    /* Local-date bookkeeping. Forward rollover proposes a fresh mask; a
     * BACKWARD local date refuses execution and keeps progress untouched
     * (a completed slot is never re-run after a backward correction). */
    {
        int cmp = brussels_date_compare(&now_local.date, &progress->date);
        if (cmp > 0) {
            out->decision = WEATHER_SCHEDULE_DAY_ROLLOVER;
            out->reason = WEATHER_SCHED_REASON_NEW_LOCAL_DATE;
            weather_schedule_progress_init(&out->proposed_progress,
                                           &now_local.date);
            out->proposal_present = true;
            return;
        }
        if (cmp < 0) {
            out->decision = WEATHER_SCHEDULE_NOT_DUE;
            out->reason = WEATHER_SCHED_REASON_DATE_REGRESSION;
            return;
        }
    }

    /* Latest slot whose boundary has been reached and is unexecuted. */
    for (i = 0; i < cfg->slot_count; i++) {
        if (cfg->slots_min[i] <= now_local.minute_of_day &&
            (progress->executed_mask & (uint8_t)(1u << i)) == 0u) {
            latest = (int8_t)i;
        }
    }

    if (latest < 0) {
        bool all_exec = true;
        for (i = 0; i < cfg->slot_count; i++) {
            if ((progress->executed_mask & (uint8_t)(1u << i)) == 0u) {
                all_exec = false;
                break;
            }
        }
        out->decision = WEATHER_SCHEDULE_NOT_DUE;
        out->reason = all_exec ? WEATHER_SCHED_REASON_ALL_EXECUTED
                     : (now_local.minute_of_day < cfg->slots_min[0])
                           ? WEATHER_SCHED_REASON_BEFORE_FIRST_SLOT
                           : WEATHER_SCHED_REASON_BETWEEN_SLOTS;
        out->next_due_valid =
            next_due(cfg, progress, &now_local, now_utc_s, &out->next_due_utc_s);
        return;
    }

    /* Resolve the candidate slot instant (first-occurrence / gap policy). */
    {
        uint64_t sutc = 0ull;
        bool adjusted = false;
        uint64_t delay_s;

        if (!slot_utc(cfg, &now_local.date, (uint8_t)latest, &sutc, &adjusted)) {
            out->reason = WEATHER_SCHED_REASON_EPOCH_RANGE;
            return; /* CONFIG_INVALID posture */
        }
        /* The autumn repeated hour cannot double-run a slot: the slot
         * instant uses the FIRST occurrence, so during the second
         * occurrence `now >= sutc` still names the same instant and the
         * executed bit (per local date) already blocks re-execution. */
        delay_s = (now_utc_s > sutc) ? (now_utc_s - sutc) : 0ull;

        if (delay_s <= (uint64_t)WEATHER_SCHEDULE_ON_TIME_WINDOW_S) {
            out->decision = WEATHER_SCHEDULE_DUE;
            out->reason = WEATHER_SCHED_REASON_ON_TIME;
        } else if (cfg->catch_up_window_s == 0u) {
            out->decision = WEATHER_SCHEDULE_NOT_DUE;
            out->reason = WEATHER_SCHED_REASON_CATCH_UP_DISABLED;
        } else if (delay_s <= (uint64_t)cfg->catch_up_window_s) {
            /* Bounded catch-up: only this LATEST missed slot; earlier
             * missed slots are never burst-executed. */
            out->decision = WEATHER_SCHEDULE_CATCH_UP_DUE;
            out->reason = WEATHER_SCHED_REASON_CATCH_UP;
        } else {
            out->decision = WEATHER_SCHEDULE_NOT_DUE;
            out->reason = WEATHER_SCHED_REASON_MISSED_OUTSIDE_WINDOW;
        }

        if (out->decision == WEATHER_SCHEDULE_DUE ||
            out->decision == WEATHER_SCHEDULE_CATCH_UP_DUE) {
            out->slot_index = latest;
            out->slot_utc_s = sutc;
            out->slot_dst_adjusted = adjusted;
            out->proposed_progress = *progress;
            out->proposed_progress.executed_mask |= (uint8_t)(1u << latest);
            out->proposal_present = true;
        }
        out->next_due_valid =
            next_due(cfg, progress, &now_local, now_utc_s, &out->next_due_utc_s);
    }
}

const char *weather_schedule_decision_str(WeatherScheduleDecision d)
{
    switch (d) {
    case WEATHER_SCHEDULE_CONFIG_INVALID: return "CONFIG_INVALID";
    case WEATHER_SCHEDULE_DISABLED: return "DISABLED";
    case WEATHER_SCHEDULE_TIME_UNTRUSTED: return "TIME_UNTRUSTED";
    case WEATHER_SCHEDULE_NOT_DUE: return "NOT_DUE";
    case WEATHER_SCHEDULE_DUE: return "DUE";
    case WEATHER_SCHEDULE_CATCH_UP_DUE: return "CATCH_UP_DUE";
    case WEATHER_SCHEDULE_DAY_ROLLOVER: return "DAY_ROLLOVER";
    default: return "UNKNOWN";
    }
}

const char *weather_schedule_reason_str(WeatherScheduleReason r)
{
    switch (r) {
    case WEATHER_SCHED_REASON_NONE: return "NONE";
    case WEATHER_SCHED_REASON_DISABLED: return "DISABLED";
    case WEATHER_SCHED_REASON_TIME_UNTRUSTED: return "TIME_UNTRUSTED";
    case WEATHER_SCHED_REASON_CONFIG_INVALID: return "CONFIG_INVALID";
    case WEATHER_SCHED_REASON_PROGRESS_INVALID: return "PROGRESS_INVALID";
    case WEATHER_SCHED_REASON_EPOCH_RANGE: return "EPOCH_RANGE";
    case WEATHER_SCHED_REASON_BEFORE_FIRST_SLOT: return "BEFORE_FIRST_SLOT";
    case WEATHER_SCHED_REASON_BETWEEN_SLOTS: return "BETWEEN_SLOTS";
    case WEATHER_SCHED_REASON_ALL_EXECUTED: return "ALL_EXECUTED";
    case WEATHER_SCHED_REASON_ON_TIME: return "ON_TIME";
    case WEATHER_SCHED_REASON_CATCH_UP: return "CATCH_UP";
    case WEATHER_SCHED_REASON_MISSED_OUTSIDE_WINDOW: return "MISSED_OUTSIDE_WINDOW";
    case WEATHER_SCHED_REASON_CATCH_UP_DISABLED: return "CATCH_UP_DISABLED";
    case WEATHER_SCHED_REASON_NEW_LOCAL_DATE: return "NEW_LOCAL_DATE";
    case WEATHER_SCHED_REASON_DATE_REGRESSION: return "DATE_REGRESSION";
    default: return "UNKNOWN";
    }
}
