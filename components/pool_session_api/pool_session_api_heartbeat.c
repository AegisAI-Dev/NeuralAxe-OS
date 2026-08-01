/*
 * NeuralAxe timed pool sessions — PURE bounded TARGET_ACTIVE heartbeat
 * scheduler and fail-safe policy (Phase 2M.1B, Gate B8).
 * See pool_session_api_heartbeat.h for the contract.
 */

#include <string.h>
#include "pool_session_api_heartbeat.h"
#include "pool_session_record.h"

#define US_PER_S 1000000ull

void pool_api_heartbeat_init(PoolApiHeartbeatState *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->initialized = true;
    s->status      = API_HB_NOT_APPLICABLE;
}

static uint64_t elapsed_since(uint64_t anchor_us, uint64_t now_us)
{
    /* A non-advancing or regressed monotonic clock never ages anything. */
    return (now_us <= anchor_us) ? 0u : ((now_us - anchor_us) / US_PER_S);
}

uint64_t pool_api_heartbeat_elapsed_s(const PoolApiHeartbeatState *s,
                                      uint64_t monotonic_us)
{
    if (s == NULL || !s->armed) {
        return 0u;
    }
    return elapsed_since(s->anchor_us, monotonic_us);
}

uint64_t pool_api_heartbeat_durable_age_s(const PoolApiHeartbeatState *s,
                                          uint64_t monotonic_us)
{
    if (s == NULL || !s->armed) {
        return 0u;
    }
    return elapsed_since(s->age_anchor_us, monotonic_us);
}

bool pool_api_heartbeat_failsafe_due(const PoolApiHeartbeatState *s,
                                     uint64_t monotonic_us)
{
    if (s == NULL || !s->initialized || !s->armed) {
        return false;
    }
    if (s->consecutive_failures >= (uint8_t)POOL_API_HEARTBEAT_MAX_CONSECUTIVE_FAILURES) {
        return true;
    }
    return pool_api_heartbeat_durable_age_s(s, monotonic_us) >=
           (uint64_t)POOL_API_HEARTBEAT_MAX_DURABLE_AGE_S;
}

bool pool_api_heartbeat_epoch_advanced(const PoolApiHeartbeatState *s,
                                       const PoolApiHeartbeatInput *in,
                                       uint64_t candidate)
{
    uint64_t base;

    if (s == NULL || in == NULL) {
        return false;
    }
    /*
     * The reference is the strictest durable fact available: the last
     * heartbeat committed in THIS boot, otherwise the PERSISTED B3 floor.
     * After a reboot the RAM counters are gone and the persisted floor is
     * therefore the only reference — exactly as intended.
     */
    if (s->have_committed_epoch) {
        base = s->last_committed_epoch_s;
        if (in->persisted_epoch_valid && in->persisted_epoch_floor_s > base) {
            base = in->persisted_epoch_floor_s;
        }
    } else if (in->persisted_epoch_valid) {
        base = in->persisted_epoch_floor_s;
    } else {
        return true;
    }
    if (candidate <= base) {
        return false; /* never lower, never equal: that would not be liveness */
    }
    return (candidate - base) >= (uint64_t)POOL_API_HEARTBEAT_MIN_EPOCH_ADVANCE_S;
}

PoolApiHeartbeatStatus pool_api_heartbeat_evaluate(const PoolApiHeartbeatState *s,
                                                   const PoolApiHeartbeatInput *in,
                                                   PoolApiHeartbeatDecision *out)
{
    PoolApiHeartbeatDecision d;

    memset(&d, 0, sizeof(d));
    d.status = API_HB_NOT_APPLICABLE;

    if (s == NULL || in == NULL || !s->initialized) {
        goto publish;
    }

    /* Fail-closed postures first: an uncertain or guarded device never
     * writes a liveness claim, and it must not keep mining the target. */
    if (in->recovery_guard) {
        d.status         = API_HB_RECOVERY_GUARD;
        d.guard_required = true;
        goto publish;
    }
    /*
     * The heartbeat exists ONLY for a durable TARGET_ACTIVE session that
     * currently holds the B7 target-mining grant. Without the grant there
     * is no target work to inhibit and nothing to fail safe.
     */
    if (!in->durable_target_active || !in->mining_grant_active) {
        d.status = API_HB_NOT_APPLICABLE;
        goto publish;
    }

    /*
     * The bounded budgets are checked BEFORE the cadence, so an exhausted
     * device orders the fail-safe on the very next tick rather than waiting
     * for another window. `failsafe_required` is raised only once — the
     * caller acknowledges it — so repeated ticks never duplicate a revoke
     * or a restore.
     */
    if (pool_api_heartbeat_failsafe_due(s, in->monotonic_us)) {
        d.status            = API_HB_PERSIST_FAILED;
        d.failsafe_required = !s->failsafe_engaged;
        goto publish;
    }

    if (!in->trusted_time_valid) {
        d.status = API_HB_TIME_UNTRUSTED;
        goto publish;
    }
    /* The candidate must be inside the committed B3 sanity band; anything
     * else would be refused by the store anyway. */
    if (in->trusted_epoch_s < (uint64_t)POOL_RECORD_EPOCH_MIN_S ||
        in->trusted_epoch_s > (uint64_t)POOL_RECORD_EPOCH_MAX_S) {
        d.status = API_HB_TIME_UNTRUSTED;
        goto publish;
    }
    /* An unarmed window never commits: the very first evaluation of a grant
     * only anchors the monotonic cadence and the durable-liveness age. */
    if (!s->armed) {
        d.status = API_HB_WAITING;
        goto publish;
    }
    if (pool_api_heartbeat_elapsed_s(s, in->monotonic_us) <
        (uint64_t)POOL_API_HEARTBEAT_PERIOD_S) {
        d.status = API_HB_WAITING;
        goto publish;
    }
    if (!pool_api_heartbeat_epoch_advanced(s, in, in->trusted_epoch_s)) {
        d.status = API_HB_WAITING;
        goto publish;
    }

    d.status     = API_HB_DUE;
    d.commit_now = true;
    d.epoch_s    = in->trusted_epoch_s;

publish:
    if (out != NULL) {
        *out = d;
    }
    return d.status;
}

void pool_api_heartbeat_arm(PoolApiHeartbeatState *s, uint64_t monotonic_us)
{
    if (s == NULL || !s->initialized) {
        return;
    }
    s->armed         = true;
    s->anchor_us     = monotonic_us;
    s->age_anchor_us = monotonic_us;
}

void pool_api_heartbeat_record_commit(PoolApiHeartbeatState *s,
                                      uint64_t monotonic_us, uint64_t epoch_s)
{
    if (s == NULL || !s->initialized) {
        return;
    }
    s->armed         = true;
    s->anchor_us     = monotonic_us;
    s->age_anchor_us = monotonic_us; /* durable liveness is now this fresh */
    s->have_success  = true;
    /* Ratchet only: a durable liveness epoch never moves backwards. */
    if (!s->have_committed_epoch || epoch_s > s->last_committed_epoch_s) {
        s->have_committed_epoch   = true;
        s->last_committed_epoch_s = epoch_s;
    }
    if (s->commits < UINT32_MAX) {
        s->commits++;
    }
    /* A success resets the bounded failure budget and the fail-safe latch. */
    s->consecutive_failures = 0u;
    s->failsafe_engaged     = false;
    s->status               = API_HB_COMMITTED;
}

void pool_api_heartbeat_record_failure(PoolApiHeartbeatState *s,
                                       uint64_t monotonic_us, bool uncertain)
{
    if (s == NULL || !s->initialized) {
        return;
    }
    /*
     * Re-arm the bounded RETRY boundary so a failing store is never
     * hammered, but NEVER re-anchor the durable-liveness age and NEVER
     * advance the durable epoch: liveness was not proven and must not be
     * reported as though it were.
     */
    s->armed     = true;
    s->anchor_us = monotonic_us;
    if (uncertain) {
        /* Unknowable outcome: the B5 recovery guard is already engaged by
         * the persistence engine; the target must not continue. */
        s->status = API_HB_RECOVERY_GUARD;
        return;
    }
    if (s->consecutive_failures < UINT8_MAX) {
        s->consecutive_failures++;
    }
    s->status = API_HB_PERSIST_FAILED;
}

void pool_api_heartbeat_record_failsafe(PoolApiHeartbeatState *s)
{
    if (s == NULL || !s->initialized) {
        return;
    }
    s->failsafe_engaged = true;
    s->status           = API_HB_PERSIST_FAILED;
}
