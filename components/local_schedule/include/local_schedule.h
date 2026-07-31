#ifndef LOCAL_SCHEDULE_H_
#define LOCAL_SCHEDULE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "brussels_time.h"

/*
 * NeuralAxe Weather-Aware Tuning — pure bounded daily check scheduler
 * (Gate W3).
 *
 * PURE: no tasks, no timers, no clocks, no NVS, no network, no busy
 * polling. Given a bounded configuration, a reconstructable progress
 * model and ONE trusted UTC epoch input, this module decides — exactly
 * once per local Brussels date per slot — whether a weather check is due,
 * and computes the next due UTC instant so a future runtime can sleep on
 * monotonic time instead of polling. No profile decision exists here.
 *
 * TIME TRUST: `now_utc_s` must be the B2 operationally trusted epoch;
 * raw time()/gettimeofday() and Stratum ntime are never acceptable.
 * With untrusted time the scheduler refuses every execution decision.
 *
 * EXACTLY-ONCE CONTRACT: executed slots are represented in a bounded
 * bitmask inside WeatherScheduleProgress, keyed to one local date. The
 * evaluation is a pure function: duplicate task wakeups over the same
 * (config, progress, now) yield the identical plan; a slot becomes
 * non-due only when the caller applies the plan's PROPOSED progress
 * (persist-before-or-immediately-after-network per a later reviewed
 * runtime contract — W3 only models the decision). Backward trusted-time
 * corrections never re-run a completed slot (the executed bit stays) and
 * a backward LOCAL-DATE regression refuses execution entirely instead of
 * resetting progress. The autumn repeated hour cannot run a slot twice:
 * slot instants resolve through the first-occurrence rule and the
 * executed bit covers the local date, not the wall instant.
 */

#define WEATHER_SCHEDULE_MAX_SLOTS 6u
#define WEATHER_SCHEDULE_PROGRESS_VERSION 1u

/* Default product slots (task brief): 05:00, 11:00, 15:00 local. */
#define WEATHER_SCHEDULE_DEFAULT_SLOT_1_MIN 300u
#define WEATHER_SCHEDULE_DEFAULT_SLOT_2_MIN 660u
#define WEATHER_SCHEDULE_DEFAULT_SLOT_3_MIN 900u

/*
 * ON-TIME WINDOW (model constant, not a product policy): a slot reached
 * within this many seconds of its boundary is a normal SCHEDULE_DUE; a
 * larger delay is a MISSED slot and falls under the separately configured
 * catch-up policy. The value only absorbs task-scheduling/retry latency;
 * boundary behavior is pinned by test.
 */
#define WEATHER_SCHEDULE_ON_TIME_WINDOW_S 600u

/* Catch-up window bound. Catch-up never crosses a local date, so larger
 * windows are meaningless; 0 disables catch-up (the conservative default
 * until a product decision is committed). */
#define WEATHER_SCHEDULE_CATCH_UP_MAX_S 43200u

typedef struct {
    bool enabled;
    uint8_t slot_count;                            /* 1..6 when enabled  */
    uint16_t slots_min[WEATHER_SCHEDULE_MAX_SLOTS]; /* minutes since local
        midnight; strictly ascending; canonical zeros past slot_count    */
    uint32_t catch_up_window_s; /* 0 = catch-up disabled                 */
} WeatherScheduleConfig;

/* Version-ready progress model (W3 does NOT persist it; the shape is
 * suitable for a later persistence/runtime gate). */
typedef struct {
    uint16_t version;        /* WEATHER_SCHEDULE_PROGRESS_VERSION        */
    WeatherLocalDate date;   /* local Brussels date the mask belongs to  */
    uint8_t executed_mask;   /* bit i => slot i executed on `date`;
                                bits >= slot_count must be zero          */
} WeatherScheduleProgress;

typedef enum {
    WEATHER_SCHEDULE_CONFIG_INVALID = 0, /* fail-safe zero               */
    WEATHER_SCHEDULE_DISABLED = 1,
    WEATHER_SCHEDULE_TIME_UNTRUSTED = 2,
    WEATHER_SCHEDULE_NOT_DUE = 3,
    WEATHER_SCHEDULE_DUE = 4,
    WEATHER_SCHEDULE_CATCH_UP_DUE = 5,
    WEATHER_SCHEDULE_DAY_ROLLOVER = 6,
    WEATHER_SCHEDULE__COUNT
} WeatherScheduleDecision;

typedef enum {
    WEATHER_SCHED_REASON_NONE = 0,
    WEATHER_SCHED_REASON_DISABLED,
    WEATHER_SCHED_REASON_TIME_UNTRUSTED,
    WEATHER_SCHED_REASON_CONFIG_INVALID,
    WEATHER_SCHED_REASON_PROGRESS_INVALID,
    WEATHER_SCHED_REASON_EPOCH_RANGE,
    WEATHER_SCHED_REASON_BEFORE_FIRST_SLOT,
    WEATHER_SCHED_REASON_BETWEEN_SLOTS,
    WEATHER_SCHED_REASON_ALL_EXECUTED,
    WEATHER_SCHED_REASON_ON_TIME,
    WEATHER_SCHED_REASON_CATCH_UP,
    WEATHER_SCHED_REASON_MISSED_OUTSIDE_WINDOW,
    WEATHER_SCHED_REASON_CATCH_UP_DISABLED,
    WEATHER_SCHED_REASON_NEW_LOCAL_DATE,
    WEATHER_SCHED_REASON_DATE_REGRESSION,
    WEATHER_SCHED_REASON__COUNT
} WeatherScheduleReason;

typedef struct {
    WeatherScheduleDecision decision;
    int8_t slot_index;         /* >= 0 iff DUE / CATCH_UP_DUE            */
    uint64_t slot_utc_s;       /* resolved slot instant (first-occurrence
                                  rule; spring gap adjusted)             */
    bool slot_dst_adjusted;    /* spring-gap adjustment applied          */
    uint64_t next_due_utc_s;   /* earliest future unexecuted slot        */
    bool next_due_valid;
    /* Proposal the caller must apply (and, in a later gate, persist) to
     * make the decision sticky: DAY_ROLLOVER proposes a fresh progress
     * for the new local date; DUE/CATCH_UP_DUE propose the executed bit. */
    WeatherScheduleProgress proposed_progress;
    bool proposal_present;
    WeatherScheduleReason reason;
} WeatherSchedulePlan;

/* Defaults: disabled, the three product slots, catch-up disabled. */
void weather_schedule_config_defaults(WeatherScheduleConfig *out);

/* Structural config validation (count/order/range/canonical zeros/window
 * bound). Mirrors the W2 check-time rules. */
bool weather_schedule_config_valid(const WeatherScheduleConfig *c);

/* Initialize progress for a local date (mask cleared). */
void weather_schedule_progress_init(WeatherScheduleProgress *out,
                                    const WeatherLocalDate *date);

/* Structural progress validation against a config (version, date, no
 * unknown mask bits). */
bool weather_schedule_progress_valid(const WeatherScheduleProgress *p,
                                     const WeatherScheduleConfig *c);

/*
 * Evaluate the schedule. Deterministic; memset-zero plan output means
 * CONFIG_INVALID (fail safe). The caller supplies ONLY trusted time; when
 * `now_trusted` is false no execution decision is ever produced.
 */
void weather_schedule_evaluate(const WeatherScheduleConfig *cfg,
                               const WeatherScheduleProgress *progress,
                               uint64_t now_utc_s, bool now_trusted,
                               WeatherSchedulePlan *out);

/* Stable machine tokens. */
const char *weather_schedule_decision_str(WeatherScheduleDecision d);
const char *weather_schedule_reason_str(WeatherScheduleReason r);

/* Compile-time guards. */
_Static_assert(WEATHER_SCHEDULE_CONFIG_INVALID == 0,
               "zeroed plan must mean CONFIG_INVALID (fail safe)");
_Static_assert(WEATHER_SCHEDULE__COUNT == 7 &&
               WEATHER_SCHED_REASON__COUNT == 15,
               "schedule enums changed — review tokens/tests");
_Static_assert(WEATHER_SCHEDULE_MAX_SLOTS <= 8u,
               "executed bitmask is u8");

#endif /* LOCAL_SCHEDULE_H_ */
