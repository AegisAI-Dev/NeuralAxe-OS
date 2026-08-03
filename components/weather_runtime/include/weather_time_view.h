#ifndef WEATHER_TIME_VIEW_H_
#define WEATHER_TIME_VIEW_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pool_time.h"

/*
 * NeuralAxe Weather-Aware Tuning — the ONE trusted-time seam for weather
 * (Gate W4).
 *
 * PURE: no clocks of its own, no SNTP, no NVS, no network, no logging, no
 * FreeRTOS, no heap. It is a projection of the committed Gate B2/B10
 * PoolTimeSnapshot into the bounded view the weather runtime consumes.
 *
 * WHY A PROJECTION AND NOT A PROVIDER
 * -----------------------------------
 * The trusted-time provider (components/pool_time/pool_time_sntp.c) has
 * exactly ONE production owner: pool_session_runtime. Weather must never
 * become a second one. This module therefore CONSTRUCTS NOTHING: it reads a
 * PoolTimeClock that the integrator hands over, through the already-committed
 * pure pool_time_snapshot(). There is no sntp symbol, no anchor writer, no
 * epoch floor and no re-anchoring anywhere in the weather component graph.
 *
 * THE NARROWING RULE (the safety property this module exists to guarantee)
 * -----------------------------------------------------------------------
 * This is NOT a second trust decision. Gate B2 decides trust; this view may
 * only ever REFUSE what B2 accepted, never grant what B2 refused:
 *
 *     trusted_time_available  =>  snapshot.trusted
 *
 * The single extra condition is a bounded anchor-age ceiling: B2 reports
 * anchor_age_us but its policy intentionally does not bound it, because a
 * timed session's own deadlines do. Weather schedules a whole local day
 * ahead, so it additionally refuses an anchor older than max_sync_age_s.
 * Refusing more is always safe; the implication above is exhaustively tested
 * and can never be inverted.
 *
 * RAW WALL CLOCK IS NOT A SOURCE HERE. time(), gettimeofday(),
 * settimeofday() and Stratum ntime are never read, never written and never
 * reachable: the only inputs are a PoolTimeClock ops table and a
 * PoolTimeTrustPolicy. Elapsed durations use monotonic microseconds only.
 * Timezone conversion lives entirely outside this file and outside pool_time
 * (components/local_schedule/brussels_time.c, pure UTC integer arithmetic).
 */

/*
 * Operational state. INTERNAL_ERROR is 0 so a memset-zero view fails closed:
 * an uninitialized view is never "trusted".
 */
typedef enum {
    WEATHER_TIME_INTERNAL_ERROR = 0,   /* fail-closed zero                 */
    WEATHER_TIME_PROVIDER_ABSENT,      /* no clock injected at all         */
    WEATHER_TIME_PROVIDER_UNINITIALIZED, /* clock present, no anchor state */
    WEATHER_TIME_SYNC_PENDING,         /* service started, no sync yet     */
    WEATHER_TIME_NOT_SYNCED,           /* no accepted sync                 */
    WEATHER_TIME_REJECTED,             /* B2 refused the anchor            */
    WEATHER_TIME_STALE,                /* B2 trusted, anchor too old       */
    WEATHER_TIME_TRUSTED,              /* the ONLY state that permits work */
    WEATHER_TIME_STATE__COUNT
} WeatherTimeState;

/*
 * Bounded anchor-age ceiling. The default is one day: weather scheduling
 * decides at most one local day ahead, so an anchor older than that can no
 * longer establish which local date it is. Zero is rejected (a zero ceiling
 * would make every anchor stale the microsecond after a sync) and values
 * above the hard limit are rejected rather than clamped.
 */
#define WEATHER_TIME_MAX_SYNC_AGE_S_DEFAULT 86400u
#define WEATHER_TIME_MAX_SYNC_AGE_S_LIMIT   604800u

/*
 * The complete weather time view. No pointers, no strings: nothing here can
 * carry a hostname, a coordinate or a server identity.
 */
typedef struct {
    bool     trusted_time_available; /* true iff state == TRUSTED          */
    uint64_t trusted_utc_s;          /* whole seconds; 0 unless trusted    */
    uint64_t monotonic_now_us;       /* elapsed/timeout/retry domain       */
    bool     sync_age_valid;         /* age is measurable AND within bound */
    uint64_t sync_age_s;             /* monotonic anchor age, whole seconds*/
    WeatherTimeState state;
    PoolTimeError provider_status;   /* ECHOED B2 verdict, never re-decided*/
    uint32_t sync_generation;        /* B2 anchor generation               */
} WeatherTimeView;

/* True when the ceiling is usable (0 and > LIMIT are refused). */
bool weather_time_max_sync_age_valid(uint32_t max_sync_age_s);

/*
 * Project an already-taken B2 snapshot. `snapshot_status` is the value
 * pool_time_snapshot() returned; it is echoed, never recomputed.
 * A NULL snapshot yields PROVIDER_ABSENT. Always writes *out (fail-closed
 * zero first), and returns out->state.
 */
WeatherTimeState weather_time_view_from_snapshot(const PoolTimeSnapshot *snapshot,
                                                 PoolTimeError snapshot_status,
                                                 uint32_t max_sync_age_s,
                                                 WeatherTimeView *out);

/*
 * Read through an injected clock. `clock` may be NULL (PROVIDER_ABSENT) —
 * that is the default W4 posture, in which weather simply waits. This
 * function calls pool_time_snapshot() and nothing else; it starts no
 * service, writes no anchor and touches no wall clock.
 */
WeatherTimeState weather_time_view_read(const PoolTimeClock *clock,
                                        const PoolTimeTrustPolicy *policy,
                                        uint32_t max_sync_age_s,
                                        WeatherTimeView *out);

/* Stable machine token; never a hostname or a numeric error value. */
const char *weather_time_state_str(WeatherTimeState s);

_Static_assert(WEATHER_TIME_INTERNAL_ERROR == 0,
               "a zeroed weather time view must fail closed");
_Static_assert(WEATHER_TIME_STATE__COUNT == 8,
               "weather time states changed — review tokens, tests and the "
               "narrowing proof");

#endif /* WEATHER_TIME_VIEW_H_ */
