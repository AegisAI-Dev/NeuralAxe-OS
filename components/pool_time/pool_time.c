/*
 * NeuralAxe trusted-time foundation — pure time-domain logic (Gate B2).
 *
 * PURE: no ESP-IDF calls, no networking, no NVS, no FreeRTOS, no heap, no
 * logging, no global mutable state. Every function is deterministic in its
 * inputs and never mutates them.
 */

#include "pool_time.h"

/* ------------------------------------------------------------------ */
/* Policy                                                              */
/* ------------------------------------------------------------------ */

void pool_time_trust_policy_defaults(PoolTimeTrustPolicy *p)
{
    if (p == (void *)0) {
        return;
    }
    p->min_epoch_s          = POOL_TIME_EPOCH_MIN_S;
    p->max_epoch_s          = POOL_TIME_EPOCH_MAX_S;
    p->required_min_epoch_s = 0u; /* cross-reboot floor disabled by default */
    p->sync_wait_default_s  = POOL_TIME_SYNC_WAIT_DEFAULT_S;
    p->sync_wait_max_s      = POOL_TIME_SYNC_WAIT_MAX_S;
}

bool pool_time_trust_policy_valid(const PoolTimeTrustPolicy *p)
{
    if (p == (void *)0) {
        return false;
    }
    if (p->min_epoch_s == 0u || p->min_epoch_s >= p->max_epoch_s) {
        return false;
    }
    if (p->sync_wait_default_s == 0u || p->sync_wait_max_s == 0u) {
        return false;
    }
    if (p->sync_wait_default_s > p->sync_wait_max_s) {
        return false;
    }
    if (p->sync_wait_max_s > POOL_TIME_SYNC_WAIT_MAX_S) {
        return false;
    }
    /* A non-zero cross-reboot floor must itself be a sane epoch. */
    if (p->required_min_epoch_s != 0u && p->required_min_epoch_s > p->max_epoch_s) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Anchor validation                                                   */
/* ------------------------------------------------------------------ */

PoolTimeError pool_time_validate_anchor(const PoolTimeAnchor *candidate,
                                        const PoolTimeTrustPolicy *policy)
{
    if (candidate == (void *)0 || !pool_time_trust_policy_valid(policy)) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    if (!candidate->valid || !candidate->sync_completed_this_boot) {
        return TIME_ERR_NOT_SYNCED;
    }
    {
        uint64_t epoch_s = candidate->epoch_us_at_sync / POOL_TIME_US_PER_S;
        if (epoch_s < policy->min_epoch_s) {
            return TIME_ERR_EPOCH_BELOW_MIN;
        }
        if (epoch_s > policy->max_epoch_s) {
            return TIME_ERR_EPOCH_ABOVE_MAX;
        }
    }
    return TIME_OK;
}

PoolTimeError pool_time_validate_reanchor(const PoolTimeAnchor *current,
                                          const PoolTimeAnchor *candidate,
                                          const PoolTimeTrustPolicy *policy)
{
    PoolTimeError base = pool_time_validate_anchor(candidate, policy);
    if (base != TIME_OK) {
        return base;
    }
    if (current == (void *)0 || !current->valid) {
        return TIME_OK; /* first acceptance */
    }
    if (candidate->monotonic_us_at_sync < current->monotonic_us_at_sync) {
        return TIME_ERR_MONOTONIC_REGRESSION;
    }
    {
        /* Implied trusted estimate of the CURRENT anchor at the candidate's
         * monotonic instant; the candidate may never fall below it. */
        uint64_t delta = candidate->monotonic_us_at_sync - current->monotonic_us_at_sync;
        uint64_t implied;
        if (current->epoch_us_at_sync > UINT64_MAX - delta) {
            return TIME_ERR_OVERFLOW;
        }
        implied = current->epoch_us_at_sync + delta;
        if (candidate->epoch_us_at_sync < implied) {
            return TIME_ERR_TRUST_REGRESSION;
        }
    }
    return TIME_OK;
}

/* ------------------------------------------------------------------ */
/* Trusted-time predicate                                              */
/* ------------------------------------------------------------------ */

PoolTimeError pool_time_evaluate_trust(const PoolTimeAnchor *anchor,
                                       uint64_t monotonic_now_us,
                                       const PoolTimeTrustPolicy *policy,
                                       uint64_t *out_trusted_utc_us)
{
    uint64_t elapsed_us;
    uint64_t derived_us;
    uint64_t derived_s;

    if (out_trusted_utc_us != (void *)0) {
        *out_trusted_utc_us = 0u;
    }
    if (anchor == (void *)0 || !pool_time_trust_policy_valid(policy)) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    if (!anchor->valid || !anchor->sync_completed_this_boot) {
        return TIME_ERR_NOT_SYNCED;
    }
    {
        uint64_t anchor_epoch_s = anchor->epoch_us_at_sync / POOL_TIME_US_PER_S;
        if (anchor_epoch_s < policy->min_epoch_s) {
            return TIME_ERR_EPOCH_BELOW_MIN;
        }
        if (anchor_epoch_s > policy->max_epoch_s) {
            return TIME_ERR_EPOCH_ABOVE_MAX;
        }
    }
    if (monotonic_now_us < anchor->monotonic_us_at_sync) {
        return TIME_ERR_MONOTONIC_REGRESSION;
    }
    elapsed_us = monotonic_now_us - anchor->monotonic_us_at_sync;
    if (anchor->epoch_us_at_sync > UINT64_MAX - elapsed_us) {
        return TIME_ERR_OVERFLOW;
    }
    derived_us = anchor->epoch_us_at_sync + elapsed_us;
    derived_s  = derived_us / POOL_TIME_US_PER_S;
    if (derived_s > policy->max_epoch_s) {
        return TIME_ERR_EPOCH_ABOVE_MAX;
    }
    if (policy->required_min_epoch_s != 0u && derived_s < policy->required_min_epoch_s) {
        return TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN;
    }
    if (out_trusted_utc_us != (void *)0) {
        *out_trusted_utc_us = derived_us;
    }
    return TIME_OK;
}

/* ------------------------------------------------------------------ */
/* Snapshot                                                            */
/* ------------------------------------------------------------------ */

PoolTimeError pool_time_snapshot(const PoolTimeClock *clock,
                                 const PoolTimeTrustPolicy *policy,
                                 PoolTimeSnapshot *out)
{
    PoolTimeSnapshot snap = {0};
    PoolTimeAnchor   anchor = {0};

    if (out == (void *)0) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    *out = snap;
    if (clock == (void *)0 || clock->ops == (void *)0 ||
        clock->ops->monotonic_us == (void *)0 || clock->ops->read_anchor == (void *)0) {
        out->status = TIME_ERR_NOT_INITIALIZED;
        return out->status;
    }
    if (!pool_time_trust_policy_valid(policy)) {
        out->status = TIME_ERR_INVALID_ARGUMENT;
        return out->status;
    }

    snap.monotonic_now_us = clock->ops->monotonic_us(clock->ctx);
    if (!clock->ops->read_anchor(clock->ctx, &anchor)) {
        snap.status = TIME_ERR_NOT_INITIALIZED;
        *out = snap;
        return out->status;
    }

    snap.sync_generation = anchor.generation;
    if (anchor.valid && snap.monotonic_now_us >= anchor.monotonic_us_at_sync) {
        snap.anchor_age_us = snap.monotonic_now_us - anchor.monotonic_us_at_sync;
    }

    if (!anchor.valid) {
        snap.status = (anchor.sync_status == POOL_TIME_SYNC_STATUS_PENDING)
                          ? TIME_ERR_SYNC_PENDING
                          : TIME_ERR_NOT_SYNCED;
        *out = snap;
        return out->status;
    }

    {
        uint64_t trusted_us = 0u;
        PoolTimeError verdict = pool_time_evaluate_trust(&anchor, snap.monotonic_now_us,
                                                         policy, &trusted_us);
        snap.status = verdict;
        if (verdict == TIME_OK) {
            snap.trusted         = true;
            snap.trusted_utc_us  = trusted_us;
            snap.trusted_epoch_s = trusted_us / POOL_TIME_US_PER_S;
        }
    }
    *out = snap;
    return out->status;
}

/* ------------------------------------------------------------------ */
/* Monotonic deadline helpers                                          */
/* ------------------------------------------------------------------ */

PoolTimeError pool_time_arm_monotonic_deadline(uint64_t monotonic_now_us,
                                               uint32_t duration_s,
                                               PoolTimeMonotonicDeadline *out)
{
    PoolTimeMonotonicDeadline d = {0};
    uint64_t duration_us;

    if (out == (void *)0) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    *out = d;
    if (duration_s < POOL_TIME_MIN_DURATION_S || duration_s > POOL_TIME_MAX_DURATION_S) {
        return TIME_ERR_DURATION_INVALID;
    }
    duration_us = (uint64_t)duration_s * POOL_TIME_US_PER_S;
    if (monotonic_now_us > UINT64_MAX - duration_us) {
        return TIME_ERR_OVERFLOW;
    }
    d.valid                 = true;
    d.armed_at_monotonic_us = monotonic_now_us;
    d.deadline_monotonic_us = monotonic_now_us + duration_us;
    d.requested_duration_s  = duration_s;
    *out = d;
    return TIME_OK;
}

PoolTimeError pool_time_check_monotonic_deadline(const PoolTimeMonotonicDeadline *d,
                                                 uint64_t monotonic_now_us,
                                                 PoolTimeDeadlineCheck *out)
{
    PoolTimeDeadlineCheck c = {0};

    if (out == (void *)0) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    *out = c;
    if (d == (void *)0 || !d->valid || d->deadline_monotonic_us < d->armed_at_monotonic_us) {
        out->status = TIME_ERR_INVALID_ARGUMENT;
        return out->status;
    }
    if (monotonic_now_us < d->armed_at_monotonic_us) {
        /* Monotonic time can never legitimately run backwards inside one
         * boot. FAIL SAFE: report the deadline as due so callers drive
         * toward restore, never toward extra target time. */
        c.expired = true;
        c.status  = TIME_ERR_MONOTONIC_REGRESSION;
        *out = c;
        return out->status;
    }
    if (monotonic_now_us >= d->deadline_monotonic_us) {
        c.expired = true;
        c.status  = TIME_OK;
        *out = c;
        return out->status;
    }
    c.remaining_us = d->deadline_monotonic_us - monotonic_now_us;
    /* Ceiling rounding: 1 µs remaining reports 1 s; 0 s only when expired.
     * remaining_us <= 24h in µs, so the addition cannot overflow. */
    c.remaining_s  = (uint32_t)((c.remaining_us + (POOL_TIME_US_PER_S - 1u)) / POOL_TIME_US_PER_S);
    c.status = TIME_OK;
    *out = c;
    return out->status;
}

/* ------------------------------------------------------------------ */
/* UTC deadline helpers                                                */
/* ------------------------------------------------------------------ */

PoolTimeError pool_time_create_utc_deadline(const PoolTimeSnapshot *snap,
                                            const PoolTimeTrustPolicy *policy,
                                            uint32_t duration_s,
                                            PoolTimeUtcDeadline *out)
{
    PoolTimeUtcDeadline d = {0};

    if (out == (void *)0) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    *out = d;
    if (snap == (void *)0 || !pool_time_trust_policy_valid(policy)) {
        return TIME_ERR_INVALID_ARGUMENT;
    }

    /* CONSISTENCY CHECKING, not authentication (see the header contract):
     * every field available to pure code is revalidated; a fully
     * self-consistent fabricated snapshot is undetectable here by design. */

    if (!snap->trusted || snap->status != TIME_OK) {
        /* An untrusted snapshot (no SNTP sync this boot, previous-boot
         * value, raw system clock, Stratum ntime, ...) can never create a
         * cross-reboot deadline. */
        return TIME_ERR_NOT_SYNCED;
    }
    if (snap->sync_generation == 0u) {
        /* An accepted synchronization always publishes generation >= 1. */
        return TIME_ERR_NOT_SYNCED;
    }
    if (snap->trusted_utc_us / POOL_TIME_US_PER_S != snap->trusted_epoch_s) {
        /* The second and microsecond views must agree. */
        return TIME_ERR_INVALID_ARGUMENT;
    }
    if (snap->anchor_age_us > snap->monotonic_now_us) {
        /* The anchor cannot be older than the boot itself. */
        return TIME_ERR_INVALID_ARGUMENT;
    }
    if (snap->trusted_epoch_s < policy->min_epoch_s) {
        return TIME_ERR_EPOCH_BELOW_MIN;
    }
    if (snap->trusted_epoch_s > policy->max_epoch_s) {
        return TIME_ERR_EPOCH_ABOVE_MAX;
    }
    if (policy->required_min_epoch_s != 0u &&
        snap->trusted_epoch_s < policy->required_min_epoch_s) {
        return TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN;
    }
    if (duration_s < POOL_TIME_MIN_DURATION_S || duration_s > POOL_TIME_MAX_DURATION_S) {
        return TIME_ERR_DURATION_INVALID;
    }
    /* With the band and field-agreement checks above, this addition cannot
     * overflow for any snapshot that passed them; kept total on purpose. */
    if (snap->trusted_epoch_s > UINT64_MAX - (uint64_t)duration_s) {
        return TIME_ERR_OVERFLOW;
    }
    d.valid                = true;
    d.deadline_epoch_s     = snap->trusted_epoch_s + (uint64_t)duration_s;
    d.requested_duration_s = duration_s;
    d.created_generation   = snap->sync_generation;
    *out = d;
    return TIME_OK;
}

PoolTimeError pool_time_create_utc_deadline_from_clock(const PoolTimeClock *clock,
                                                       const PoolTimeTrustPolicy *policy,
                                                       uint32_t duration_s,
                                                       PoolTimeSnapshot *out_snap,
                                                       PoolTimeUtcDeadline *out)
{
    PoolTimeSnapshot snap = {0};
    PoolTimeError status;

    if (out == (void *)0) {
        return TIME_ERR_INVALID_ARGUMENT;
    }
    out->valid                = false;
    out->deadline_epoch_s     = 0u;
    out->requested_duration_s = 0u;
    out->created_generation   = 0u;

    /* Obtain the snapshot directly from the provider-owned accepted anchor —
     * no caller-supplied snapshot is involved on this path. */
    status = pool_time_snapshot(clock, policy, &snap);
    if (out_snap != (void *)0) {
        *out_snap = snap;
    }
    if (status != TIME_OK) {
        return status;
    }
    return pool_time_create_utc_deadline(&snap, policy, duration_s, out);
}

/* ------------------------------------------------------------------ */
/* Reboot/recovery time decision                                       */
/* ------------------------------------------------------------------ */

static PoolTimeRecoveryResult recovery_result(PoolTimeRecoveryDecision decision,
                                              PoolTimeError status,
                                              bool requires_trusted_time,
                                              bool terminal_for_time)
{
    PoolTimeRecoveryResult r = {0};
    r.decision              = decision;
    r.status                = status;
    r.requires_trusted_time = requires_trusted_time;
    r.terminal_for_time     = terminal_for_time;
    return r;
}

PoolTimeRecoveryResult pool_time_decide_recovery(const PoolTimeRecoveryInput *in,
                                                 const PoolTimeTrustPolicy *policy)
{
    bool trusted;
    PoolTimeError untrusted_reason = TIME_ERR_NOT_SYNCED;

    /* Invalid call: never resume a target on unverifiable timing input. */
    if (in == (void *)0 || !pool_time_trust_policy_valid(policy)) {
        return recovery_result(POOL_TIME_DECISION_RECOVERY_REQUIRED,
                               TIME_ERR_INVALID_ARGUMENT, false, true);
    }

    /* A. No valid source snapshot: nothing safe can be restored or resumed. */
    if (!in->source_snapshot_valid) {
        return recovery_result(POOL_TIME_DECISION_RECOVERY_REQUIRED,
                               TIME_ERR_NO_SOURCE_SNAPSHOT, false, true);
    }

    /* G. Model corruption in the bounded-window fields: the source snapshot
     * is trustworthy, so fail safe toward restore (never resume, never wait
     * on a broken window). */
    if (in->sync_wait_limit_s == 0u || in->sync_wait_limit_s > POOL_TIME_SYNC_WAIT_MAX_S) {
        return recovery_result(POOL_TIME_DECISION_FAIL_SAFE_RESTORE,
                               TIME_ERR_INVALID_WAIT_WINDOW, false, true);
    }

    /* B. No valid persisted UTC deadline: the previous monotonic deadline
     * died with the reboot. Do not wait for time at all — restore. */
    if (!in->utc_deadline_valid) {
        return recovery_result(POOL_TIME_DECISION_FAIL_SAFE_RESTORE,
                               TIME_ERR_NO_PERSISTED_DEADLINE, false, true);
    }

    /* G. Corrupt persisted epochs (flagged valid but outside the sanity
     * band, or internally inconsistent): fail safe toward restore. */
    if (in->deadline_epoch_s < policy->min_epoch_s) {
        return recovery_result(POOL_TIME_DECISION_FAIL_SAFE_RESTORE,
                               TIME_ERR_EPOCH_BELOW_MIN, false, true);
    }
    /* The deadline may legitimately exceed max_epoch by up to one max
     * duration only if start sat at the ceiling; anything above that is
     * corrupt. Keep the strict band on the persisted value. */
    if (in->deadline_epoch_s > policy->max_epoch_s + (uint64_t)POOL_TIME_MAX_DURATION_S) {
        return recovery_result(POOL_TIME_DECISION_FAIL_SAFE_RESTORE,
                               TIME_ERR_EPOCH_ABOVE_MAX, false, true);
    }
    if (in->verified_start_epoch_valid) {
        if (in->verified_start_epoch_s < policy->min_epoch_s ||
            in->verified_start_epoch_s > policy->max_epoch_s) {
            return recovery_result(POOL_TIME_DECISION_FAIL_SAFE_RESTORE,
                                   TIME_ERR_INVALID_ARGUMENT, false, true);
        }
        if (in->verified_start_epoch_s > in->deadline_epoch_s) {
            return recovery_result(POOL_TIME_DECISION_FAIL_SAFE_RESTORE,
                                   TIME_ERR_INVALID_ARGUMENT, false, true);
        }
    }

    /* F. Trusted time earlier than the persisted verified-start epoch is a
     * rejected trust claim: treat as untrusted and keep the bounded wait. */
    trusted = in->time_trusted;
    if (trusted && in->verified_start_epoch_valid &&
        in->trusted_epoch_s < in->verified_start_epoch_s) {
        trusted = false;
        untrusted_reason = TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN;
    }

    if (trusted) {
        /* C. Fresh SNTP trust this boot. */
        if (in->trusted_epoch_s >= in->deadline_epoch_s) {
            PoolTimeRecoveryResult r =
                recovery_result(POOL_TIME_DECISION_RESTORE_DUE, TIME_OK, false, true);
            r.remaining_valid = true;
            r.remaining_s     = 0u;
            return r;
        }
        {
            PoolTimeRecoveryResult r =
                recovery_result(POOL_TIME_DECISION_RESUME_TARGET_WITH_REMAINING_TIME,
                                TIME_OK, true, true);
            r.remaining_valid = true;
            r.remaining_s     = in->deadline_epoch_s - in->trusted_epoch_s;
            return r;
        }
    }

    /* D/E. Untrusted (DNS failure, NTP unreachable, sync pending, rejected
     * trust): wait only within the bounded window, then fail safe. */
    if (in->sync_wait_elapsed_s < in->sync_wait_limit_s) {
        return recovery_result(POOL_TIME_DECISION_WAIT_FOR_TRUSTED_TIME,
                               untrusted_reason, true, false);
    }
    return recovery_result(POOL_TIME_DECISION_FAIL_SAFE_RESTORE,
                           (untrusted_reason == TIME_ERR_NOT_SYNCED)
                               ? TIME_ERR_SYNC_TIMEOUT
                               : untrusted_reason,
                           false, true);
}

/* ------------------------------------------------------------------ */
/* Stable machine tokens                                               */
/* ------------------------------------------------------------------ */

const char *pool_time_error_str(PoolTimeError e)
{
    switch (e) {
    case TIME_OK:                           return "TIME_OK";
    case TIME_ERR_INVALID_ARGUMENT:         return "TIME_ERR_INVALID_ARGUMENT";
    case TIME_ERR_NOT_INITIALIZED:          return "TIME_ERR_NOT_INITIALIZED";
    case TIME_ERR_NOT_SYNCED:               return "TIME_ERR_NOT_SYNCED";
    case TIME_ERR_SYNC_PENDING:             return "TIME_ERR_SYNC_PENDING";
    case TIME_ERR_SYNC_TIMEOUT:             return "TIME_ERR_SYNC_TIMEOUT";
    case TIME_ERR_SNTP_INIT:                return "TIME_ERR_SNTP_INIT";
    case TIME_ERR_SNTP_START:               return "TIME_ERR_SNTP_START";
    case TIME_ERR_SNTP_STOP:                return "TIME_ERR_SNTP_STOP";
    case TIME_ERR_INVALID_SERVER_CONFIG:    return "TIME_ERR_INVALID_SERVER_CONFIG";
    case TIME_ERR_INVALID_WAIT_WINDOW:      return "TIME_ERR_INVALID_WAIT_WINDOW";
    case TIME_ERR_EPOCH_BELOW_MIN:          return "TIME_ERR_EPOCH_BELOW_MIN";
    case TIME_ERR_EPOCH_ABOVE_MAX:          return "TIME_ERR_EPOCH_ABOVE_MAX";
    case TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN:return "TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN";
    case TIME_ERR_MONOTONIC_REGRESSION:     return "TIME_ERR_MONOTONIC_REGRESSION";
    case TIME_ERR_TRUST_REGRESSION:         return "TIME_ERR_TRUST_REGRESSION";
    case TIME_ERR_DURATION_INVALID:         return "TIME_ERR_DURATION_INVALID";
    case TIME_ERR_OVERFLOW:                 return "TIME_ERR_OVERFLOW";
    case TIME_ERR_NO_SOURCE_SNAPSHOT:       return "TIME_ERR_NO_SOURCE_SNAPSHOT";
    case TIME_ERR_NO_PERSISTED_DEADLINE:    return "TIME_ERR_NO_PERSISTED_DEADLINE";
    case TIME_ERR_RECOVERY_REQUIRED:        return "TIME_ERR_RECOVERY_REQUIRED";
    default:                                return "TIME_ERR_UNKNOWN";
    }
}

const char *pool_time_decision_str(PoolTimeRecoveryDecision d)
{
    switch (d) {
    case POOL_TIME_DECISION_WAIT_FOR_TRUSTED_TIME:
        return "WAIT_FOR_TRUSTED_TIME";
    case POOL_TIME_DECISION_RESUME_TARGET_WITH_REMAINING_TIME:
        return "RESUME_TARGET_WITH_REMAINING_TIME";
    case POOL_TIME_DECISION_RESTORE_DUE:
        return "RESTORE_DUE";
    case POOL_TIME_DECISION_FAIL_SAFE_RESTORE:
        return "FAIL_SAFE_RESTORE";
    case POOL_TIME_DECISION_RECOVERY_REQUIRED:
        return "RECOVERY_REQUIRED";
    default:
        return "DECISION_UNKNOWN";
    }
}

const char *pool_time_sync_status_str(PoolTimeSyncStatus s)
{
    switch (s) {
    case POOL_TIME_SYNC_STATUS_NONE:      return "SYNC_STATUS_NONE";
    case POOL_TIME_SYNC_STATUS_PENDING:   return "SYNC_STATUS_PENDING";
    case POOL_TIME_SYNC_STATUS_COMPLETED: return "SYNC_STATUS_COMPLETED";
    default:                              return "SYNC_STATUS_UNKNOWN";
    }
}
