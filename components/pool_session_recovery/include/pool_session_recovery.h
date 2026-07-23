#ifndef POOL_SESSION_RECOVERY_H_
#define POOL_SESSION_RECOVERY_H_

#include <stdint.h>
#include <stdbool.h>
#include "pool_session.h"
#include "pool_session_record.h"
#include "pool_session_store.h"
#include "pool_time.h"
#include "pool_session_reset.h"

/*
 * NeuralAxe timed pool sessions — pure boot-recovery decision engine
 * (Phase 2M.1B, Gate B4). Board 601 / BM1370 only.
 *
 * This layer combines the committed B3 store result + record, the B1
 * persistent state and restore obligation, the B2 trusted-time recovery
 * decision, a NeuralAxe reset classification and the bounded persisted
 * counters into ONE machine-readable boot-recovery plan. It decides what a
 * future runtime integrator (Gates B5-B7) is ALLOWED to do after boot — it
 * executes nothing: no NVS, no SNTP, no Wi-Fi, no Stratum, no pool
 * mutation, no restart, no clock read, no reset-reason read, no logging,
 * no heap, no global mutable state.
 *
 * NON-NEGOTIABLE PLAN SAFETY (B4 invariants):
 *  - Only STORE_OK exposes a record to state-based planning; every other
 *    store result yields a conservative plan with NO target resumption and
 *    NO pool-mutation recommendation.
 *  - Only a fully verified persisted TARGET_ACTIVE — with the restore
 *    obligation held, complete target evidence, valid identities, a valid
 *    persisted UTC deadline, un-exhausted budgets, a resume-permitting
 *    reset class AND a B2 RESUME decision — may produce the target
 *    ELIGIBILITY plan. Even that plan does NOT authorize target mining:
 *    B4 never emits ALLOW_TARGET_MINING, because it has no fresh live
 *    verification evidence. Mining authorization belongs to the future
 *    runtime layer after live re-verification.
 *  - While fresh trusted time is unavailable, target mining is inhibited
 *    and the wait is bounded; at the bound the plan fails safe to restore.
 *  - A missing persisted UTC deadline fails safe to restore immediately.
 *  - restore_required is NEVER discharged, no record is cleared, no
 *    tombstone is created, and no plan ever requests a device reboot.
 *  - The persisted latest_accepted_trusted_epoch is the anti-regression
 *    floor supplied to the B2 policy (required_min_epoch_s); the weaker
 *    verified_start_epoch is used only when no stronger floor exists (the
 *    committed audit §7 predicate explicitly permits that fallback).
 *
 * IDEMPOTENCY CONTRACT: the engine is a pure function — the same boot
 *  context always yields a byte-identical plan, and evaluating it twice
 *  never double-increments a counter (counters are PROPOSALS derived from
 *  the record's persisted values; the caller must persist a proposal via
 *  the B3 store before re-evaluating and before executing any associated
 *  external action — B4 invariant 21).
 */

/* ------------------------------------------------------------------ */
/* Machine-readable recovery codes                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    RECOVERY_OK = 0,
    RECOVERY_ERR_INVALID_ARGUMENT,
    RECOVERY_ERR_STORE_EMPTY,
    RECOVERY_ERR_STORE_CORRUPT,
    RECOVERY_ERR_STORE_UNSUPPORTED,
    RECOVERY_ERR_STORE_AMBIGUOUS,
    RECOVERY_ERR_STORE_UNCERTAIN,
    RECOVERY_ERR_RECORD_MISSING,
    RECOVERY_ERR_RECORD_INVALID,
    RECOVERY_ERR_SOURCE_MISSING,
    RECOVERY_ERR_TARGET_INVALID,
    RECOVERY_ERR_RESTORE_OBLIGATION_MISMATCH,
    RECOVERY_ERR_STATE_UNSUPPORTED,
    RECOVERY_ERR_TIME_UNTRUSTED,
    RECOVERY_ERR_TIME_TIMEOUT,
    RECOVERY_ERR_DEADLINE_MISSING,
    RECOVERY_ERR_DEADLINE_EXPIRED,
    RECOVERY_ERR_TIME_REGRESSION,
    RECOVERY_ERR_COUNTER_EXHAUSTED,
    RECOVERY_ERR_RESET_ABNORMAL,
    RECOVERY_ERR_MINING_INHIBITION_UNAVAILABLE,
    RECOVERY_ERR_OPERATOR_REQUIRED,
    POOL_RECOVERY_ERR__COUNT
} PoolRecoveryError;

/* ------------------------------------------------------------------ */
/* Plan enums                                                          */
/* ------------------------------------------------------------------ */

/* Primary decision. */
typedef enum {
    POOL_BOOT_DECISION_BOOT_NORMAL_SOURCE = 0, /* no active session facts   */
    POOL_BOOT_DECISION_RETAIN_TERMINAL_SOURCE, /* terminal kept, source runs */
    POOL_BOOT_DECISION_WAIT_FOR_TRUSTED_TIME,  /* bounded, mining inhibited  */
    /*
     * ELIGIBILITY, not authorization: the persisted session is eligible for
     * LIVE target verification and possible resumption. B4 operates only on
     * persisted facts — the target was verified before the reboot, which
     * does not prove the live post-boot configuration still matches the
     * persisted target identity. Mining is NOT yet authorized: this plan
     * always carries VERIFY_BEFORE_MINING + VERIFY_TARGET_CONFIGURATION,
     * and the future runtime owner must re-verify the live target identity
     * and the required mining evidence first.
     */
    POOL_BOOT_DECISION_RESUME_VERIFIED_TARGET,
    POOL_BOOT_DECISION_RESTORE_SOURCE_NOW,
    POOL_BOOT_DECISION_VERIFY_RESTORE,
    POOL_BOOT_DECISION_RECOVERY_REQUIRED,
    POOL_BOOT_DECISION__COUNT
} PoolBootDecision;

/* Which pool configuration the future runtime may run. */
typedef enum {
    POOL_BOOT_ALLOW_SOURCE_ONLY = 0,
    POOL_BOOT_ALLOW_TARGET_ONLY,
    POOL_BOOT_ALLOW_NO_POOL,
    POOL_BOOT_ALLOW_CURRENT_CONFIG_UNVERIFIED, /* committed truth unknowable */
    POOL_BOOT_ALLOW__COUNT
} PoolBootAllowedConfig;

/* Mining policy. ALLOW_TARGET_MINING is NEVER emitted by the pure B4
 * engine: B4 possesses no fresh live verification evidence, so target
 * mining cannot be authorized here. The value is RESERVED for the future
 * runtime-verification layer (Gates B6/B7), which may grant it only after
 * live target identity + mining evidence checks succeed. */
typedef enum {
    POOL_BOOT_MINING_ALLOW_SOURCE = 0,
    POOL_BOOT_MINING_ALLOW_TARGET, /* reserved; unreachable from B4 */
    POOL_BOOT_MINING_INHIBIT,
    POOL_BOOT_MINING_VERIFY_BEFORE_MINING,
    POOL_BOOT_MINING__COUNT
} PoolBootMiningPolicy;

/* Abstract persistence intent (descriptions only; B4 persists nothing). */
typedef enum {
    POOL_BOOT_PERSIST_NO_RECORD_UPDATE = 0,
    POOL_BOOT_PERSIST_PROPOSE_RECOVERY_STATE_UPDATE,
    POOL_BOOT_PERSIST_PROPOSE_COUNTER_UPDATE,
    POOL_BOOT_PERSIST_PROPOSE_RESTORE_DUE,
    POOL_BOOT_PERSIST_PROPOSE_RECOVERY_REQUIRED,
    POOL_BOOT_PERSIST_RETAIN_TERMINAL_RESULT,
    POOL_BOOT_PERSIST__COUNT
} PoolBootPersistIntent;

/* Abstract runtime intent (descriptions only; B4 executes nothing). */
typedef enum {
    POOL_BOOT_RUNTIME_NONE = 0,
    POOL_BOOT_RUNTIME_START_TRUSTED_TIME_WAIT, /* implies holding Stratum   */
    POOL_BOOT_RUNTIME_APPLY_SOURCE_CONFIGURATION,
    POOL_BOOT_RUNTIME_VERIFY_SOURCE_CONFIGURATION,
    POOL_BOOT_RUNTIME_START_SOURCE_MINING,
    POOL_BOOT_RUNTIME_RESUME_TARGET_CONFIGURATION,
    POOL_BOOT_RUNTIME_VERIFY_TARGET_CONFIGURATION,
    POOL_BOOT_RUNTIME_HOLD_STRATUM,
    POOL_BOOT_RUNTIME_SURFACE_OPERATOR_RECOVERY,
    POOL_BOOT_RUNTIME__COUNT
} PoolBootRuntimeIntent;

/* ------------------------------------------------------------------ */
/* Boot context (pure input; immutable)                                */
/* ------------------------------------------------------------------ */

/*
 * Everything the engine may know. No raw NVS bytes, no passwords, no
 * unrestricted strings, no task handles, no ESP-IDF errors. Facts that
 * live in the committed record are read from the record, not duplicated.
 */
typedef struct {
    /* B3 store outcome for this boot's load. */
    PoolStoreResult store_result;
    /* The validated committed record; REQUIRED exactly when store_result ==
     * STORE_OK. The engine never mutates it. */
    bool                      record_present;
    const PoolSessionRecord  *record;

    /* NeuralAxe reset classification (Gate B5+ reads the raw reason once). */
    PoolSessionResetClass reset_class;

    /* Bounded trusted-time synchronization wait (future monotonic boot
     * timer supplies elapsed; B4 only consumes the values). */
    uint32_t sync_wait_elapsed_s;
    uint32_t sync_wait_limit_s;

    /* The B2 clock view for this boot (produced by pool_time_snapshot()
     * under the B4-built policy; operational provenance, not proof). */
    bool             time_provider_initialized;
    PoolTimeSnapshot time_snapshot;

    /* Whether the future runtime can guarantee target-mining inhibition
     * while waiting for trusted time. When it cannot, WAIT plans are
     * forbidden and the engine fails safe to restore instead. */
    bool mining_inhibition_available;
} PoolSessionBootContext;

/* ------------------------------------------------------------------ */
/* Plan output                                                         */
/* ------------------------------------------------------------------ */

/* Deterministic counter PROPOSAL (values the caller must persist through
 * the B3 store BEFORE executing the plan's external action). */
typedef struct {
    uint8_t reboot_count;
    uint8_t recovery_attempt_count;
    uint8_t consecutive_recovery_failures;
    uint8_t reset_class_for_record;      /* PoolRecordResetClass value */
    bool    reboot_changed;
    bool    recovery_attempt_changed;
    bool    consecutive_changed;
    bool    any_exhausted;
    bool    must_persist_before_action;
} PoolBootCounterProposal;

/*
 * Bounded abstract record-update proposal. It never carries identities,
 * secrets, epochs or the obligation — those are PRESERVED by contract: the
 * future store owner copies the committed record, applies ONLY the fields
 * below plus the counter proposal, and validates + commits it through B3
 * before acting. It never clears a record, never creates a tombstone,
 * never touches generation/slot, never lowers the trusted-epoch floor and
 * never discharges restore_required.
 */
typedef struct {
    bool             update_needed;
    PoolSessionState proposed_state;        /* next persistent FSM state  */
    uint16_t         proposed_failure_code; /* PoolSessionError value     */
} PoolBootRecordProposal;

/* The complete machine-readable boot-recovery plan. */
typedef struct {
    PoolBootDecision      decision;
    PoolBootAllowedConfig allowed_config;
    PoolBootMiningPolicy  mining_policy;
    PoolBootPersistIntent persist_intent;
    PoolBootRuntimeIntent runtime_intent;

    PoolRecoveryError error;  /* primary machine status                    */
    PoolRecoveryError reason; /* secondary machine reason (never prose)    */

    bool     remaining_valid;       /* remaining_target_s carries a value  */
    uint64_t remaining_target_s;    /* non-zero on a resume plan           */
    bool     trusted_time_required; /* progress depends on fresh trust     */
    bool     restore_required;      /* echoed obligation (record present)  */
    bool     inhibit_target_stratum;/* runtime must hold target Stratum    */
    bool     terminal;              /* no further boot-time progression    */

    uint32_t plan_fingerprint;      /* deterministic FNV-1a of the fields  */

    PoolBootCounterProposal counters;
    PoolBootRecordProposal  record_proposal;
} PoolSessionRecoveryPlan;

/* ------------------------------------------------------------------ */
/* Pure API                                                            */
/* ------------------------------------------------------------------ */

/*
 * THE pure boot-recovery decision (Gate B4 Stage 14). Total over every
 * store result, persisted state, reset class and B2 time decision;
 * deterministic; inputs immutable; the output is fully written on every
 * path (a NULL/malformed context yields a conservative RECOVERY_REQUIRED
 * plan). Returns plan->error for convenience.
 */
PoolRecoveryError pool_session_recovery_plan(const PoolSessionBootContext *context,
                                             PoolSessionRecoveryPlan *out_plan);

/*
 * Build the B2 trust policy for this boot from the committed record:
 * defaults plus required_min_epoch_s = latest_accepted_trusted_epoch when
 * valid, else verified_start_epoch when valid (permitted fallback), else 0.
 * Exposed so the future integrator produces its PoolTimeSnapshot under the
 * SAME floor the engine enforces. Pure.
 */
void pool_session_recovery_build_time_policy(const PoolSessionRecord *record,
                                             PoolTimeTrustPolicy *out_policy);

/* Stable machine tokens (dot-free; no hostname/account/reset text). */
const char *pool_recovery_error_str(PoolRecoveryError e);
const char *pool_boot_decision_str(PoolBootDecision d);
const char *pool_boot_allowed_config_str(PoolBootAllowedConfig a);
const char *pool_boot_mining_policy_str(PoolBootMiningPolicy m);
const char *pool_boot_persist_intent_str(PoolBootPersistIntent p);
const char *pool_boot_runtime_intent_str(PoolBootRuntimeIntent r);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */
_Static_assert(POOL_RECOVERY_ERR__COUNT == 22, "recovery code count changed — review tokens/tests");
_Static_assert(POOL_BOOT_DECISION__COUNT == 7, "decision count changed — review tables/tests");
_Static_assert(POOL_BOOT_ALLOW__COUNT == 4 && POOL_BOOT_MINING__COUNT == 4,
               "policy enum count changed — review tables/tests");
_Static_assert(POOL_BOOT_PERSIST__COUNT == 6 && POOL_BOOT_RUNTIME__COUNT == 9,
               "intent enum count changed — review tables/tests");
/* A new B3 store result must be handled here explicitly, never fall through. */
_Static_assert(POOL_STORE_RESULT__COUNT == 16,
               "PoolStoreResult changed — extend the B4 store decision table");

#endif /* POOL_SESSION_RECOVERY_H_ */
