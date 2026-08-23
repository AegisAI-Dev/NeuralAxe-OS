#ifndef NX_WEATHER_WINDOW_H_
#define NX_WEATHER_WINDOW_H_

#include <stdint.h>
#include <stdbool.h>

#include "local_schedule.h"   /* WeatherSchedulePlan, WeatherScheduleProgress */
#include "brussels_time.h"    /* WeatherLocalDate                             */
#include "tuning_record.h"    /* TuningScheduleWindowState (the persisted form) */

/*
 * NeuralAxe — CRASH-SAFE SCHEDULE-WINDOW DEDUPLICATION DECISION
 * (Phase 2W, Gate W6.4).
 *
 * THE PROBLEM. Until now the served-window mask lived only in RAM. A device
 * that served the 05:00 window and rebooted at 05:03 reacquired trusted time,
 * found itself still inside the same logical window, and could serve it again.
 * Nothing durable recorded that the window had been spent.
 *
 * THE CONTRACT, STATED EXACTLY. This gate provides
 *
 *     AT MOST ONE OUTBOUND SUBMISSION PER PERSISTED SCHEDULE WINDOW ACROSS
 *     REBOOTS, subject to successful durable claim persistence.
 *
 * It is deliberately NOT "exactly once". Exactly-once external side effects
 * cannot be guaranteed across a local persistence transaction and a remote
 * HTTP request — there is no distributed transaction between flash and a
 * weather provider. It is also deliberately NOT at-least-once: the ordering
 * below can LOSE a window, and that is the accepted trade.
 *
 * THE ORDERING THAT MAKES IT TRUE.
 *
 *     trusted schedule says DUE
 *       -> determine canonical window identity
 *         -> consult persistent dedup state
 *           -> durably CLAIM the window (dual-slot commit, verified)
 *             -> only then permit the W6.3 async submission
 *
 * If power is lost after the durable claim but before the HTTP request, the
 * window is simply missed after reboot. For a recommendation-only pilot that
 * is strictly safer than issuing the same authorized window twice after an
 * ambiguous crash: a missed recommendation changes nothing, a duplicate is an
 * unaccounted outbound request against the owner's provider.
 *
 * PURE. No NVS, no ESP-IDF, no FreeRTOS, no heap, no logging, no clock read,
 * no network. It decides; the integrator persists and submits. It cannot
 * submit anything and cannot write anything.
 *
 * NOT A CLOCK. The persisted service day is scheduling state, never a time
 * authority. It can never make time trusted, and this module never consults
 * a clock: the day it reasons about arrives only inside a plan that the
 * committed schedule already produced from trusted B2/B10 time.
 */

/* ------------------------------------------------------------------ */
/* Decision vocabulary                                                 */
/* ------------------------------------------------------------------ */

/*
 * Every value is a bounded token; the zero is the fail-closed one, so a
 * zeroed decision never authorizes a submission.
 */
typedef enum {
    NX_WX_WINDOW_UNAVAILABLE = 0,  /* fail-closed zero: no usable decision  */
    NX_WX_WINDOW_NOT_DUE,          /* the plan opens no window right now    */
    NX_WX_WINDOW_ALREADY_CLAIMED,  /* durably claimed already -> suppress   */
    NX_WX_WINDOW_DAY_REGRESSION,   /* candidate day older than persisted    */
    NX_WX_WINDOW_CLAIM_REQUIRED,   /* claim durably, THEN submit            */
    NX_WX_WINDOW__COUNT
} NxWeatherWindowDecision;

const char *nx_weather_window_decision_str(NxWeatherWindowDecision d);

/* True only for the one decision that permits an outbound submission — and
 * only after the caller has durably committed the proposed state. */
bool nx_weather_window_requires_claim(NxWeatherWindowDecision d);

/* ------------------------------------------------------------------ */
/* Identity conversion                                                 */
/* ------------------------------------------------------------------ */

/*
 * The persisted form carries date primitives rather than a WeatherLocalDate
 * so the W2 store keeps its existing dependency set. These two functions are
 * the ONLY mapping between the two representations, so the conversion cannot
 * drift apart in two places. Both are total and NULL-safe.
 */
bool nx_weather_window_date_from_state(const TuningScheduleWindowState *st,
                                       WeatherLocalDate *out);
void nx_weather_window_state_from_date(const WeatherLocalDate *date,
                                       uint8_t served_mask,
                                       TuningScheduleWindowState *out);

/* ------------------------------------------------------------------ */
/* THE decision                                                        */
/* ------------------------------------------------------------------ */

/*
 * Decide what must happen for the window the plan has opened, given the
 * PERSISTED dedup state (the authority) and the committed slot count.
 *
 * `persisted` is the durable truth. The plan's own `proposed_progress` mask is
 * deliberately NOT used as the next mask: it is RAM state that may contain
 * bits from the current boot that were never durably committed. The next mask
 * is always derived from the PERSISTED mask, so a claim can only ever add the
 * one bit it is claiming.
 *
 * On NX_WX_WINDOW_CLAIM_REQUIRED, `out_next` receives the exact state the
 * caller must durably commit BEFORE submitting. A new service day atomically
 * replaces the old day and its mask in that single commit, which is why day
 * rollover needs no separate write.
 *
 * Fails closed (UNAVAILABLE) on any NULL, on an out-of-range slot index, on a
 * slot index beyond the configured count, and on a plan whose proposed date is
 * not a valid calendar date. `out_next` is always zeroed first, so a caller
 * that ignores the return value cannot commit a stale or partial claim.
 */
NxWeatherWindowDecision nx_weather_window_decide(
    const WeatherSchedulePlan *plan,
    const TuningScheduleWindowState *persisted,
    uint8_t slot_count,
    TuningScheduleWindowState *out_next);

/*
 * Project the persisted authority back onto the committed W3 RAM progress
 * model, so that after boot the schedule evaluates against durable truth
 * rather than a fresh empty mask. Returns false (writing a cleared progress
 * for `today`) when there is no persisted claim for that date — which is the
 * correct starting point, not an error.
 */
bool nx_weather_window_progress_from_state(const TuningScheduleWindowState *st,
                                           const WeatherLocalDate *today,
                                           WeatherScheduleProgress *out);

_Static_assert(NX_WX_WINDOW_UNAVAILABLE == 0,
               "a zeroed window decision must be the fail-closed token");
_Static_assert(NX_WX_WINDOW__COUNT == 5,
               "W6.4 decision tokens changed — review strings and tests");

#endif /* NX_WEATHER_WINDOW_H_ */
