#ifndef POOL_SESSION_API_HEARTBEAT_H_
#define POOL_SESSION_API_HEARTBEAT_H_

#include "pool_session_api_types.h"

/*
 * NeuralAxe timed pool sessions — PURE bounded TARGET_ACTIVE heartbeat
 * scheduler and its FAIL-SAFE policy (Phase 2M.1B, Gate B8).
 *
 * WHAT THE HEARTBEAT IS. While a durable session is TARGET_ACTIVE and the
 * Gate B7 target-mining grant is valid, the device periodically re-commits
 * the ALREADY COMMITTED B3 field `latest_trusted_epoch_s` — the
 * monotonically-advancing latest-accepted-trusted-epoch floor. That is an
 * honest use of the field, not a repurposing: the value written IS a
 * trusted epoch the device accepted at that moment, and the committed B3
 * acceptance helper (pool_session_record_propose_trusted_epoch) enforces
 * the sanity band and the never-lower rule.
 *
 * WHAT IT IS NOT. It is an ACCEPTED-TRUSTED-EPOCH DURABILITY SIGNAL and
 * nothing more. It is NOT a substitute for ASIC processing evidence, for
 * protocol health, for thermal health or for mining-grant validation, and
 * it never grants, extends or renews target mining. It NEVER touches
 * `deadline_epoch_s`, `deadline_valid`, `deadline_sync_generation` or
 * `duration_s`, so it is STRUCTURALLY incapable of extending a session.
 *
 * THE FAIL-SAFE. A heartbeat that keeps failing means the device can no
 * longer prove durable liveness for a session that is mining someone
 * else's pool. Continuing indefinitely with an ever-staler durable floor is
 * not acceptable, so the policy is bounded in BOTH dimensions:
 *
 *   - at most POOL_API_HEARTBEAT_MAX_CONSECUTIVE_FAILURES definite
 *     failures in a row;
 *   - at most POOL_API_HEARTBEAT_MAX_DURABLE_AGE_S of monotonic time
 *     without a durable heartbeat.
 *
 * Reaching either bound orders the fail-safe, and an UNCERTAIN commit
 * orders it immediately. The caller then performs, in this exact order:
 * inhibit ASIC target work -> revoke the target-mining grant -> restore the
 * source through the EXISTING session owner -> persist the restoration
 * intent before any source mutation -> retain restore_required.
 *
 * This module is PURE: no ESP-IDF, no NVS, no store, no lease, no clock
 * read of its own (monotonic microseconds are an input), no heap, no
 * globals. Every counter is bounded, RAM-only and reset by RAM loss; after
 * a reboot the PERSISTED B3 floor — never a RAM counter — is the reference.
 */

/* Committed Phase 2M.1A cadence: approximately 60 s while TARGET_ACTIVE. */
#define POOL_API_HEARTBEAT_PERIOD_S 60u
/* The trusted epoch must have advanced at least this much since the last
 * durable heartbeat (or the persisted floor) before flash is touched. */
#define POOL_API_HEARTBEAT_MIN_EPOCH_ADVANCE_S 60u
/* Bounded budget of DEFINITE consecutive failures before the fail-safe. */
#define POOL_API_HEARTBEAT_MAX_CONSECUTIVE_FAILURES 3u
/* Bounded monotonic age of the last DURABLE heartbeat before the fail-safe. */
#define POOL_API_HEARTBEAT_MAX_DURABLE_AGE_S 180u

/* Bounded RAM-only scheduler state. It holds no identity, no secret, no
 * wall clock and no persistent boot identifier. */
typedef struct {
    bool     initialized;
    bool     armed;                 /* a monotonic anchor exists this boot  */
    uint64_t anchor_us;             /* cadence / bounded-retry boundary     */
    uint64_t age_anchor_us;         /* last DURABLE liveness point          */
    bool     have_success;          /* a durable heartbeat happened         */
    bool     have_committed_epoch;
    uint64_t last_committed_epoch_s;
    uint8_t  consecutive_failures;  /* saturating, definite failures only   */
    bool     failsafe_engaged;      /* the fail-safe was already ordered    */
    uint32_t commits;               /* saturating audit counter             */
    PoolApiHeartbeatStatus status;  /* last published status                */
} PoolApiHeartbeatState;

/* Everything the pure scheduler may know about one evaluation. */
typedef struct {
    bool     durable_target_active;   /* committed record state == TARGET_ACTIVE */
    bool     mining_grant_active;     /* the B7 grant is currently valid         */
    bool     recovery_guard;          /* fail-closed posture: never write        */
    bool     trusted_time_valid;      /* B2 snapshot trusted AND TIME_OK         */
    uint64_t trusted_epoch_s;         /* meaningful only when trusted_time_valid */
    uint64_t monotonic_us;            /* monotonic microseconds this boot        */
    bool     persisted_epoch_valid;   /* record.latest_trusted_valid             */
    uint64_t persisted_epoch_floor_s; /* record.latest_trusted_epoch_s           */
} PoolApiHeartbeatInput;

typedef struct {
    PoolApiHeartbeatStatus status;
    bool                   commit_now;
    uint64_t               epoch_s; /* meaningful only when commit_now */
    /*
     * The bounded fail-safe order. True EXACTLY ONCE per exhausted budget
     * or age bound (the caller acknowledges with
     * pool_api_heartbeat_record_failsafe), so repeated owner-task ticks can
     * never duplicate a revoke or a restore.
     */
    bool failsafe_required;
    /* Uncertainty: guard immediately AND revoke; never a durable claim. */
    bool guard_required;
} PoolApiHeartbeatDecision;

/* Zero to the fail-closed NOT_APPLICABLE posture. */
void pool_api_heartbeat_init(PoolApiHeartbeatState *s);

/*
 * THE pure decision. Total, deterministic, input-immutable; *out is fully
 * written on every path. `commit_now` is true ONLY when every gate passes:
 * durable TARGET_ACTIVE, a valid B7 mining grant, no recovery guard, a
 * trusted in-band B2 epoch, an armed monotonic window that has reached
 * POOL_API_HEARTBEAT_PERIOD_S, and a trusted epoch at least
 * POOL_API_HEARTBEAT_MIN_EPOCH_ADVANCE_S above the last durable value.
 * A NULL argument yields NOT_APPLICABLE and never a commit.
 */
PoolApiHeartbeatStatus pool_api_heartbeat_evaluate(const PoolApiHeartbeatState *s,
                                                   const PoolApiHeartbeatInput *in,
                                                   PoolApiHeartbeatDecision *out);

/*
 * Arm (or re-arm) the monotonic window without writing anything. Called on
 * the first evaluation of a grant so the very first tick can never commit;
 * it also anchors the durable-liveness age.
 */
void pool_api_heartbeat_arm(PoolApiHeartbeatState *s, uint64_t monotonic_us);

/* Record a durable, independently read-back and B5-proven heartbeat commit:
 * resets the failure budget, re-anchors BOTH the cadence and the age, and
 * ratchets the last durable epoch (never backwards). */
void pool_api_heartbeat_record_commit(PoolApiHeartbeatState *s,
                                      uint64_t monotonic_us, uint64_t epoch_s);

/*
 * Record a heartbeat commit that did NOT durably prove liveness. The
 * bounded retry boundary is re-armed (so a failing store is never
 * hammered), the durable-liveness age is NOT re-anchored, and the durable
 * epoch is NOT advanced. A DEFINITE failure consumes one unit of the
 * bounded budget; an UNCERTAIN outcome publishes RECOVERY_GUARD and demands
 * the immediate fail-safe.
 */
void pool_api_heartbeat_record_failure(PoolApiHeartbeatState *s,
                                       uint64_t monotonic_us, bool uncertain);

/* Acknowledge that the caller has performed the ordered fail-safe (gate
 * inhibited, grant revoked, restoration started). Idempotent. */
void pool_api_heartbeat_record_failsafe(PoolApiHeartbeatState *s);

/* Bounded, overflow-safe monotonic elapsed seconds since the cadence
 * anchor and since the last DURABLE heartbeat. */
uint64_t pool_api_heartbeat_elapsed_s(const PoolApiHeartbeatState *s,
                                      uint64_t monotonic_us);
uint64_t pool_api_heartbeat_durable_age_s(const PoolApiHeartbeatState *s,
                                          uint64_t monotonic_us);

/* True when `candidate` is at least the configured minimum above the last
 * durable value (or above the persisted floor when nothing is durable yet). */
bool pool_api_heartbeat_epoch_advanced(const PoolApiHeartbeatState *s,
                                       const PoolApiHeartbeatInput *in,
                                       uint64_t candidate);

/* True when either bounded limit has been reached. */
bool pool_api_heartbeat_failsafe_due(const PoolApiHeartbeatState *s,
                                     uint64_t monotonic_us);

_Static_assert(POOL_API_HEARTBEAT_MAX_CONSECUTIVE_FAILURES >= 1u &&
                   POOL_API_HEARTBEAT_MAX_CONSECUTIVE_FAILURES <= 10u,
               "the heartbeat failure budget must stay small and bounded");
_Static_assert(POOL_API_HEARTBEAT_MAX_DURABLE_AGE_S >=
                   POOL_API_HEARTBEAT_PERIOD_S * 2u,
               "the durable-age bound must allow at least two cadence windows");

#endif /* POOL_SESSION_API_HEARTBEAT_H_ */
