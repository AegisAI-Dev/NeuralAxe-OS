/*
 * NeuralAxe Weather-Aware Tuning — trusted-time projection (Gate W4).
 *
 * PURE. The only include beyond its own header is pool_time.h, which is
 * itself pure (components/pool_time/pool_time.c includes nothing else).
 * There is deliberately no <time.h>, no esp_timer, no SNTP header and no
 * logging header in this translation unit.
 */

#include "weather_time_view.h"

#define WEATHER_TIME_US_PER_S 1000000ull

bool weather_time_max_sync_age_valid(uint32_t max_sync_age_s)
{
    return max_sync_age_s > 0u && max_sync_age_s <= WEATHER_TIME_MAX_SYNC_AGE_S_LIMIT;
}

const char *weather_time_state_str(WeatherTimeState s)
{
    switch (s) {
    case WEATHER_TIME_INTERNAL_ERROR:        return "TIME_INTERNAL_ERROR";
    case WEATHER_TIME_PROVIDER_ABSENT:       return "TIME_PROVIDER_ABSENT";
    case WEATHER_TIME_PROVIDER_UNINITIALIZED:return "TIME_PROVIDER_UNINITIALIZED";
    case WEATHER_TIME_SYNC_PENDING:          return "TIME_SYNC_PENDING";
    case WEATHER_TIME_NOT_SYNCED:            return "TIME_NOT_SYNCED";
    case WEATHER_TIME_REJECTED:              return "TIME_REJECTED";
    case WEATHER_TIME_STALE:                 return "TIME_STALE";
    case WEATHER_TIME_TRUSTED:               return "TIME_TRUSTED";
    case WEATHER_TIME_STATE__COUNT:
    default:                                 return "TIME_INTERNAL_ERROR";
    }
}

/*
 * Map the B2 verdict for an anchor that did NOT pass the trust predicate.
 * The verdict is classified, never overridden: every branch is a refusal.
 */
static WeatherTimeState untrusted_state(PoolTimeError status)
{
    switch (status) {
    case TIME_ERR_NOT_INITIALIZED:
        return WEATHER_TIME_PROVIDER_UNINITIALIZED;
    case TIME_ERR_SYNC_PENDING:
        return WEATHER_TIME_SYNC_PENDING;
    case TIME_ERR_NOT_SYNCED:
        return WEATHER_TIME_NOT_SYNCED;
    default:
        /* Anything else is an explicit B2 rejection (epoch bounds, the
         * anti-regression floor, monotonic regression, recovery required,
         * ...). Weather does not interpret it further and never retries it
         * with different rules — that decision belongs to B2 alone. */
        return WEATHER_TIME_REJECTED;
    }
}

WeatherTimeState weather_time_view_from_snapshot(const PoolTimeSnapshot *snapshot,
                                                 PoolTimeError snapshot_status,
                                                 uint32_t max_sync_age_s,
                                                 WeatherTimeView *out)
{
    WeatherTimeView v;
    uint64_t age_s;

    if (out == NULL) {
        return WEATHER_TIME_INTERNAL_ERROR;
    }

    /* Fail closed first: every early return below leaves a refused view. */
    v.trusted_time_available = false;
    v.trusted_utc_s          = 0u;
    v.monotonic_now_us       = 0u;
    v.sync_age_valid         = false;
    v.sync_age_s             = 0u;
    v.state                  = WEATHER_TIME_INTERNAL_ERROR;
    v.provider_status        = TIME_ERR_NOT_INITIALIZED;
    v.sync_generation        = 0u;
    *out = v;

    if (snapshot == NULL) {
        out->state = WEATHER_TIME_PROVIDER_ABSENT;
        return out->state;
    }
    if (!weather_time_max_sync_age_valid(max_sync_age_s)) {
        /* A misconfigured ceiling must never widen trust. */
        out->state = WEATHER_TIME_INTERNAL_ERROR;
        return out->state;
    }

    /* Facts that are safe to carry regardless of the verdict. */
    v.monotonic_now_us = snapshot->monotonic_now_us;
    v.sync_generation  = snapshot->sync_generation;
    v.provider_status  = snapshot_status;
    age_s              = snapshot->anchor_age_us / WEATHER_TIME_US_PER_S;
    v.sync_age_s       = age_s;

    if (!snapshot->trusted) {
        /* B2 refused. Weather can only agree. */
        v.state = untrusted_state(snapshot_status);
        *out = v;
        return out->state;
    }

    /*
     * B2 trusted. The ONLY additional condition is the bounded anchor age,
     * and it can only subtract. `trusted_time_available` is assigned in
     * exactly one place in this file, inside this branch.
     */
    if (age_s > (uint64_t)max_sync_age_s) {
        v.sync_age_valid = false;
        v.state          = WEATHER_TIME_STALE;
        *out = v;
        return out->state;
    }

    v.sync_age_valid         = true;
    v.trusted_utc_s          = snapshot->trusted_epoch_s;
    v.trusted_time_available = true;
    v.state                  = WEATHER_TIME_TRUSTED;
    *out = v;
    return out->state;
}

WeatherTimeState weather_time_view_read(const PoolTimeClock *clock,
                                        const PoolTimeTrustPolicy *policy,
                                        uint32_t max_sync_age_s,
                                        WeatherTimeView *out)
{
    PoolTimeSnapshot snap;
    PoolTimeError    status;

    if (out == NULL) {
        return WEATHER_TIME_INTERNAL_ERROR;
    }
    if (clock == NULL || policy == NULL) {
        /* The default W4 posture: nothing injected, so nothing is trusted.
         * Weather waits; it never falls back to a wall clock. */
        return weather_time_view_from_snapshot(NULL, TIME_ERR_NOT_INITIALIZED,
                                               max_sync_age_s, out);
    }

    /* The committed B2 entry point — the single trust decision in the tree. */
    status = pool_time_snapshot(clock, policy, &snap);
    return weather_time_view_from_snapshot(&snap, status, max_sync_age_s, out);
}
