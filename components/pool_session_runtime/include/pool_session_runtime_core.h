#ifndef POOL_SESSION_RUNTIME_CORE_H_
#define POOL_SESSION_RUNTIME_CORE_H_

#include <stdint.h>
#include <stdbool.h>
#include "pool_session.h"
#include "pool_session_record.h"
#include "pool_session_store.h"
#include "pool_session_recovery.h"
#include "pool_session_reset.h"
#include "pool_operation_types.h"

/*
 * NeuralAxe timed pool sessions — PURE runtime controller
 * (Phase 2M.1B, Gate B6). Board 601 / BM1370 only.
 *
 * This header is the pure decision domain of the session runtime: it turns
 * the committed B3 store outcome, the pure B4 boot-recovery plan and the
 * reconstructed B5 ownership into ONE runtime state, ONE protocol-start
 * permission and ONE sanitized snapshot. It executes NOTHING: no ESP-IDF,
 * no IO, no NVS, no SNTP, no FreeRTOS, no heap, no logging, no global
 * mutable state, no clock read, no reset-reason read.
 *
 * GATE B6 SCOPE — runtime skeleton and safety-hold gate:
 *  - NO runtime state ever authorizes an external action. Target mining is
 *    never granted, pool configuration is never mutated, Stratum is never
 *    stopped/started/reconnected for a timed session, no restore executes,
 *    no restart or OTA is requested. Gate B7 owns controlled apply, restore
 *    and live verification.
 *  - The protocol-start permission is a ONE-WAY, boot-time decision. ALLOW
 *    is emitted only for proven-safe postures; every unresolved, uncertain,
 *    corrupt, unsupported, ambiguous, active or obligation-bearing posture
 *    HOLDS. Nothing in B6 releases a hold afterwards.
 *  - A failed store open or a failed store load is NEVER interpreted as
 *    STORE_EMPTY: it reconstructs the recovery guard and holds.
 *  - Invalid, unknown or internally inconsistent inputs fail CLOSED
 *    (recovery guard or error — never FREE, never ALLOW).
 *
 * B1-B5 POLICY IS NOT DUPLICATED HERE. This layer consumes their verdicts:
 * the B4 plan decides recovery, the B5 phase is the authoritative ownership
 * reconstruction, and this layer only cross-checks that the two agree and
 * maps the agreed posture onto the runtime/protocol surface.
 *
 * PRIVACY: no pool hostname, account, worker, wallet, password, session
 * identifier, raw record, raw NVS byte, raw reset value or unrestricted
 * string exists anywhere in this model. The snapshot has NO string fields
 * by construction.
 */

#define POOL_RUNTIME_MODEL_VERSION 1u

/* ------------------------------------------------------------------ */
/* Runtime states                                                      */
/* ------------------------------------------------------------------ */

/*
 * A runtime state is a POSTURE, never an authorization. Any value outside
 * this enum is treated as RUNTIME_ERROR by every total helper below.
 */
typedef enum {
    RUNTIME_UNINITIALIZED = 0,      /* nothing has run yet                     */
    RUNTIME_BOOTSTRAPPING,          /* boot sequence in progress               */
    RUNTIME_FREE,                   /* proven no session facts; source may run */
    RUNTIME_TERMINAL_PENDING,       /* safe retained terminal; awaiting ack    */
    RUNTIME_PERSISTENCE_PENDING,    /* a B4 proposal must commit first         */
    RUNTIME_WAITING_FOR_TRUSTED_TIME, /* bounded wait; mining inhibited        */
    RUNTIME_VERIFY_TARGET_PENDING,  /* eligibility only; NO mining grant       */
    RUNTIME_RESTORE_SOURCE_PENDING, /* restore owed; B6 executes nothing       */
    RUNTIME_OPERATOR_RECOVERY,      /* an operator-recovery lease is out       */
    RUNTIME_RECOVERY_GUARD,         /* fail-closed guard; operator required    */
    RUNTIME_STOPPED,                /* task stopped (test/shutdown only)       */
    RUNTIME_ERROR,                  /* fail-closed initialization failure      */
    POOL_RUNTIME_STATE__COUNT
} PoolRuntimeState;

/* ------------------------------------------------------------------ */
/* Machine-readable runtime status codes                               */
/* ------------------------------------------------------------------ */

/* Stable public tokens only — never a hostname, account, NVS key, raw reset
 * value or ESP-IDF error string. */
typedef enum {
    RUNTIME_OK = 0,
    RUNTIME_ERR_INVALID_ARGUMENT,
    RUNTIME_ERR_FEATURE_DISABLED,
    RUNTIME_ERR_ALREADY_INITIALIZED,
    RUNTIME_ERR_NOT_INITIALIZED,
    RUNTIME_ERR_STORE_OPEN_FAILED,
    RUNTIME_ERR_STORE_LOAD_FAILED,
    RUNTIME_ERR_STORE_UNCERTAIN,
    RUNTIME_ERR_PLAN_INVALID,
    RUNTIME_ERR_BOOTSTRAP_FAILED,
    RUNTIME_ERR_OWNERSHIP_MISMATCH,
    RUNTIME_ERR_PERSIST_REQUIRED,
    RUNTIME_ERR_PERSIST_FAILED,
    RUNTIME_ERR_PERSIST_READBACK,
    RUNTIME_ERR_PERSIST_DUPLICATE,
    RUNTIME_ERR_TIME_SOURCE_UNCONFIGURED,
    RUNTIME_ERR_TIME_WAIT_PENDING,
    RUNTIME_ERR_TIME_WAIT_EXPIRED,
    RUNTIME_ERR_TASK_CREATE_FAILED,
    RUNTIME_ERR_TASK_ALREADY_RUNNING,
    RUNTIME_ERR_UNSUPPORTED_EVENT,
    RUNTIME_ERR_INTERNAL_CONSISTENCY,
    POOL_RUNTIME_STATUS__COUNT
} PoolRuntimeStatus;

/* ------------------------------------------------------------------ */
/* Protocol-start permission                                           */
/* ------------------------------------------------------------------ */

/*
 * The single boot-time answer handed to main(). HOLD is value 0 so that any
 * zeroed / uninitialized / partially-written decision fails closed.
 *
 * ALLOW_SOURCE means EXACTLY: the existing, unchanged source/default
 * protocol startup may proceed. It never means target mining, and it never
 * means a timed session may act.
 */
typedef enum {
    POOL_RUNTIME_PROTOCOL_HOLD = 0,
    POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
    POOL_RUNTIME_PROTOCOL__COUNT
} PoolRuntimeProtocolPermission;

/* ------------------------------------------------------------------ */
/* Bounded runtime events (bit mask; no payload, no heap)              */
/* ------------------------------------------------------------------ */

#define RUNTIME_EVENT_BOOTSTRAP_COMPLETE    (1u << 0)
#define RUNTIME_EVENT_NETWORK_READY         (1u << 1)
#define RUNTIME_EVENT_TIME_SYNC_CHANGED     (1u << 2)
#define RUNTIME_EVENT_MONOTONIC_BOUNDARY    (1u << 3)
#define RUNTIME_EVENT_STORE_RELOAD_REQUIRED (1u << 4)
#define RUNTIME_EVENT_SHUTDOWN_FOR_TEST     (1u << 5)
#define RUNTIME_EVENT__ALL_VALID            0x3Fu

/* Drop every unknown bit. Unknown events never reach any handler, never
 * change state and never release a hold. */
uint32_t pool_runtime_event_sanitize(uint32_t raw_events);
/* True only for exactly one known event bit. */
bool pool_runtime_event_is_known(uint32_t event_bit);
/* Stable machine token for exactly one known bit ("unknown" otherwise). */
const char *pool_runtime_event_str(uint32_t event_bit);

/* ------------------------------------------------------------------ */
/* Feature-flag boot action (pure; testable for BOTH flag values)      */
/* ------------------------------------------------------------------ */

typedef struct {
    bool bootstrap_required; /* run the B6 boot sequence at all           */
    bool protocol_allowed;   /* pre-decision protocol answer (fail-closed) */
} PoolRuntimeBootAction;

/*
 * The exact rule both the adapter and main() obey:
 *  - feature DISABLED -> no bootstrap, protocol startup UNCHANGED (allowed);
 *    no instance, no task, no store open, no SNTP, nothing reachable.
 *  - feature ENABLED  -> bootstrap required and protocol NOT yet allowed;
 *    the real answer comes from the runtime decision below.
 */
PoolRuntimeBootAction pool_runtime_boot_action_for_feature(bool feature_enabled);

/* ------------------------------------------------------------------ */
/* Classification input / decision                                     */
/* ------------------------------------------------------------------ */

/*
 * Everything the pure classifier may know about one boot. No raw NVS bytes,
 * no secrets, no strings, no handles, no ESP-IDF errors, no raw reset value.
 */
typedef struct {
    /* B3 lifecycle facts. store_opened=false or store_loaded=false must
     * NEVER be confused with an empty store (non-negotiable contract 7). */
    bool            store_opened;
    bool            store_loaded;
    PoolStoreResult store_result;

    bool                     record_present;
    const PoolSessionRecord *record; /* required exactly when STORE_OK */

    /* B4 output (required: the engine always runs). */
    const PoolSessionRecoveryPlan *plan;

    /* B5 reconstruction — authoritative ownership for this boot. */
    PoolOperationStatus     bootstrap_status;
    PoolOperationOwner      lease_owner;
    PoolOperationLeasePhase lease_phase;
    uint32_t                lease_generation;
    bool                    terminal_pending;
    bool                    lease_restore_required;
    bool                    lease_persistence_required;

    /* Persistence-before-action evidence for the CURRENT evaluation. */
    bool            persist_required;  /* this evaluation's normalized proposal
                                        * is non-empty (runtime-known demand;
                                        * ORed with the plan's own demand)    */
    bool            persist_attempted;
    bool            persist_verified;  /* committed AND read-back verified */
    PoolStoreResult persist_result;    /* meaningful when persist_attempted */
} PoolRuntimeClassifyInput;

/*
 * The complete runtime decision. `target_mining_authorized` and
 * `pool_mutation_permitted` are structurally always false in Gate B6 and
 * are asserted false by the test suite on EVERY produced decision.
 */
typedef struct {
    PoolRuntimeState              state;
    PoolRuntimeProtocolPermission protocol;
    PoolRuntimeStatus             status;

    bool trusted_time_required;
    bool persistence_pending;
    bool restore_required;
    bool target_mining_authorized; /* always false in B6 */
    bool pool_mutation_permitted;  /* always false in B6 */
} PoolRuntimeDecision;

/*
 * THE pure runtime classification. Total over every store result, plan
 * decision, lease phase, owner and persistence outcome; deterministic;
 * inputs immutable; *out is fully written on every path. A NULL or
 * malformed input yields a fail-closed HOLD decision, never ALLOW.
 * Returns out->status for convenience.
 */
PoolRuntimeStatus pool_runtime_classify(const PoolRuntimeClassifyInput *in,
                                        PoolRuntimeDecision *out);

/* Total, fail-closed: ALLOW only for RUNTIME_FREE / RUNTIME_TERMINAL_PENDING.
 * Every other value — including out-of-range ones — HOLDS. */
PoolRuntimeProtocolPermission pool_runtime_protocol_for_state(PoolRuntimeState s);

/* Convenience predicate over the same total rule. */
bool pool_runtime_state_holds_protocol(PoolRuntimeState s);

/* ------------------------------------------------------------------ */
/* Sanitized runtime snapshot                                          */
/* ------------------------------------------------------------------ */

/*
 * The published runtime view. Machine facts only: NO string field exists in
 * this struct by construction, so no hostname, account, worker, wallet,
 * password, session identifier, profile, raw record or raw NVS byte can be
 * carried. The reset CLASS is published; the raw esp_reset_reason() value
 * never is.
 */
typedef struct {
    uint32_t                      model_version;
    PoolRuntimeState              state;
    PoolRuntimeProtocolPermission protocol;
    PoolRuntimeStatus             status;

    PoolStoreResult         store_result;
    PoolBootDecision        boot_decision;
    PoolBootAllowedConfig   allowed_config;
    PoolBootMiningPolicy    mining_policy;
    PoolRecoveryError       recovery_error;
    PoolOperationOwner      lease_owner;
    PoolOperationLeasePhase lease_phase;
    uint32_t                lease_generation;
    PoolSessionResetClass   reset_class;
    uint32_t                plan_fingerprint;
    uint32_t                committed_generation;

    bool session_present;
    bool restore_required;
    bool trusted_time_required;
    bool persistence_pending;
    bool target_mining_authorized; /* always false in B6 */
    bool pool_mutation_permitted;  /* always false in B6 */
    bool task_running;

    uint8_t  reboot_count;
    uint8_t  recovery_attempt_count;
    uint8_t  consecutive_recovery_failures;
    uint32_t trusted_time_wait_elapsed_s;
    uint32_t trusted_time_wait_limit_s;
    uint32_t proposal_commits;
} PoolRuntimeSnapshot;

/* Zero a snapshot to the fail-closed UNINITIALIZED/HOLD view. */
void pool_runtime_snapshot_init(PoolRuntimeSnapshot *snap);

/*
 * Build the sanitized snapshot from the same immutable inputs plus the
 * decision. Never copies an identity, a session id or any string. *snap is
 * fully written on every path. Returns RUNTIME_ERR_INVALID_ARGUMENT (and a
 * zeroed snapshot) for NULL arguments.
 */
PoolRuntimeStatus pool_runtime_snapshot_build(const PoolRuntimeClassifyInput *in,
                                              const PoolRuntimeDecision *dec,
                                              PoolSessionResetClass reset_class,
                                              uint32_t committed_generation,
                                              uint32_t wait_elapsed_s,
                                              uint32_t wait_limit_s,
                                              uint32_t proposal_commits,
                                              bool task_running,
                                              PoolRuntimeSnapshot *snap);

/*
 * Structural consistency of a published snapshot: model version, every enum
 * in range, protocol matching the state's total rule, and the two Gate B6
 * prohibitions (no mining grant, no pool-mutation permission) intact.
 */
bool pool_runtime_snapshot_valid(const PoolRuntimeSnapshot *snap);

/* ------------------------------------------------------------------ */
/* Bounded runtime control (pure event/state bookkeeping)              */
/* ------------------------------------------------------------------ */

/*
 * The task's pure working state. Holds no pointers, no handles and no
 * identities. Persistence-proposal accounting lives in the separate
 * PoolRuntimeProposalTracker below, not here.
 */
typedef struct {
    PoolRuntimeState state;
    uint32_t         pending_events;      /* sanitized bit mask               */
    bool             bootstrap_complete;
    bool             network_ready;
    bool             time_sync_seen;
    bool             shutdown_requested;
    uint32_t         wait_elapsed_s;
    uint32_t         wait_limit_s;
} PoolRuntimeControl;

/* What the adapter must do after applying a batch of events. */
typedef struct {
    bool             start_time_provider;
    bool             reevaluate_plan;
    bool             reload_store;
    bool             stop_task;
    bool             state_changed;
    uint32_t         applied_events;  /* sanitized bits actually handled */
    uint32_t         ignored_events;  /* unknown bits, dropped           */
    PoolRuntimeState state;
} PoolRuntimeEventOutcome;

/* Initialize the control block to the fail-closed UNINITIALIZED posture. */
void pool_runtime_control_init(PoolRuntimeControl *c, uint32_t wait_limit_s);

/* Record the boot decision into the control block (state only; never a
 * protocol release). */
PoolRuntimeStatus pool_runtime_control_adopt(PoolRuntimeControl *c,
                                             const PoolRuntimeDecision *dec);

/*
 * Apply a batch of raw event bits. Unknown bits are dropped (reported in
 * `ignored_events`) and never fail the call. Repeating an already-applied
 * event is idempotent: no counter moves, no duplicate work is requested.
 * NO event, and no absence of an event, ever releases a protocol hold or
 * changes ownership — the outcome only asks the adapter to start the time
 * provider, re-evaluate B4, reload the store or stop the task.
 */
PoolRuntimeStatus pool_runtime_control_apply(PoolRuntimeControl *c,
                                             uint32_t raw_events,
                                             PoolRuntimeEventOutcome *out);

/*
 * Advance the bounded trusted-time wait by `delta_s` (saturating at the
 * limit). Returns true when the bound has been reached — the caller must
 * then re-evaluate the B4 plan, which fails safe toward restore. Reaching
 * the bound NEVER releases the protocol hold and never frees ownership.
 */
bool pool_runtime_control_advance_wait(PoolRuntimeControl *c, uint32_t delta_s);

/* True when the bounded wait has reached its limit. */
bool pool_runtime_wait_expired(uint32_t elapsed_s, uint32_t limit_s);

/* ------------------------------------------------------------------ */
/* Persistence-proposal tracking (pure; generation-aware)              */
/* ------------------------------------------------------------------ */

/*
 * SEMANTIC persistence proposals — the corrected duplicate-commit contract.
 *
 * The committed B4 counter model derives every proposal from the record's
 * PERSISTED values (reboot_count = increment(record.reboot_count)), so
 * re-planning over a record persisted earlier in the SAME physical boot
 * re-proposes the per-boot increments a second time. The correction is NOT
 * "one proposal per boot": it is
 *
 *  - the SAME semantic proposal derived from the SAME committed source
 *    generation is committed at most once;
 *  - a DIFFERENT later persist-before-action proposal (new state, failure
 *    code, recovery-attempt increment or raised trusted-epoch floor) is
 *    still committed and independently read back during the same boot;
 *  - a proposal derived from a NEWER committed generation is never
 *    suppressed by an earlier one;
 *  - the per-PHYSICAL-BOOT facts — the reboot_count increment and the
 *    reset-class-driven consecutive_recovery_failures increment (B4 §2.4:
 *    "reboot +1 per boot, consecutive +1 on abnormal classes") — are
 *    durably accounted at most once per physical boot and are NORMALIZED
 *    out of later raw plans without discarding their other field changes.
 *    recovery_attempt_count is per-planned-restore-action and is NEVER
 *    normalized.
 *
 * The tracker is RAM-only, bounded, and reset naturally by RAM loss on a
 * real reboot. It deliberately contains NO wall-clock time, NO Stratum
 * ntime, NO raw reset value, NO persistent boot identifier, NO hostname,
 * account, worker, wallet, password, session identity, raw record or raw
 * NVS bytes. It is an internal consistency mechanism, not a cryptographic
 * capability.
 */

typedef enum {
    RUNTIME_PROPOSAL_NONE = 0,       /* nothing (after normalization) to persist */
    RUNTIME_PROPOSAL_PLAN,           /* B4 plan-driven record/counter update      */
    RUNTIME_PROPOSAL_EPOCH_FLOOR,    /* raised latest-accepted trusted epoch only */
    RUNTIME_PROPOSAL_PLAN_AND_EPOCH, /* both parts in one staged commit           */
    POOL_RUNTIME_PROPOSAL_KIND__COUNT
} PoolRuntimeProposalKind;

/*
 * One complete, NORMALIZED semantic proposal: every safety-relevant field a
 * commit may change in the persisted record, plus the committed source
 * generation it was derived from. No identity, secret or string exists here.
 */
typedef struct {
    PoolRuntimeProposalKind kind;              /* NONE = nothing to persist  */
    uint32_t source_generation;                /* committed gen planned FROM */

    bool     state_update;                     /* apply proposed state/code  */
    uint8_t  proposed_state;                   /* PoolSessionState value     */
    uint16_t proposed_failure_code;            /* PoolSessionError value     */

    uint8_t  reboot_count;                     /* absolute, NORMALIZED       */
    uint8_t  recovery_attempt_count;           /* absolute (never normalized)*/
    uint8_t  consecutive_recovery_failures;    /* absolute, NORMALIZED       */
    uint8_t  reset_class_for_record;           /* PoolRecordResetClass value */

    bool     raise_epoch_floor;                /* ratchet the trusted floor  */
    uint64_t proposed_epoch_floor_s;           /* valid when raising         */

    bool     expected_restore_required;        /* preserved exactly by B6    */
} PoolRuntimeProposal;

/*
 * Bounded RAM-only tracker of durable persistence work this PHYSICAL boot.
 * `proven` refers to the LAST proposal and is set only after commit +
 * independent reload + complete readback verification (+ B5 proof
 * acceptance whenever an owned session-class lease exists — B5 proofs are
 * token-gated by design, so unowned postures have no proof to apply).
 *
 * DEDUPLICATION AUTHORITY: `last_proposal` is the EXACT normalized proposal
 * key — fixed-width bounded fields only, no identity, secret, raw record or
 * unrestricted string — and explicit field-by-field equality over it is the
 * ONLY authoritative "already proven" proof. The 32-bit fingerprint below
 * is a diagnostic token and an inexpensive preliminary INEQUALITY check; a
 * hash match is NEVER sufficient to treat two proposals as equal, so a
 * fingerprint collision can never suppress a distinct proposal.
 */
typedef struct {
    bool     initialized;
    /* Once-per-physical-boot accounting (cleared only by RAM loss). */
    bool     reboot_increment_committed;
    bool     consecutive_increment_committed;
    /* The last PROVEN proposal (bounded; replans always derive from the
     * current committed record, so older proposals cannot recur). */
    bool     proven;
    PoolRuntimeProposal last_proposal; /* THE exact normalized proposal key */
    /* Diagnostic tokens only — never equality authority. */
    PoolRuntimeProposalKind last_kind;
    uint32_t last_fingerprint;      /* diagnostic / preliminary inequality  */
    uint32_t source_generation;     /* generation the proposal derived FROM */
    uint32_t committed_generation;  /* generation the commit produced       */
    /* Saturating audit counters. */
    uint32_t commit_count;          /* durable proposal commits this boot   */
    uint32_t proof_count;           /* accepted B5 persistence proofs       */
} PoolRuntimeProposalTracker;

/* Zero the tracker to the fresh-physical-boot posture. */
void pool_runtime_tracker_init(PoolRuntimeProposalTracker *t);

/* True when a B4 plan carries a proposal that MUST be committed through B3
 * and read-back verified before the runtime state may advance. */
bool pool_runtime_plan_requires_persistence(const PoolSessionRecoveryPlan *plan);

/*
 * Build the normalized semantic proposal for ONE evaluation from the raw B4
 * plan, the CURRENT committed record, the tracker's per-boot accounting and
 * an optional trusted-epoch floor candidate. Deterministic and total:
 *  - no committed record (or NULL plan/record) => kind NONE;
 *  - per-boot increments already durably accounted are normalized back to
 *    the committed values while every OTHER changed field is preserved;
 *  - the epoch part exists only for a candidate strictly above the persisted
 *    floor, not below verified_start, and inside the B3 sanity band;
 *  - kind NONE exactly when nothing would change in the committed record.
 * *out is fully written on every path. Returns out->kind for convenience.
 */
PoolRuntimeProposalKind pool_runtime_proposal_build(
    const PoolSessionRecoveryPlan *plan, const PoolSessionRecord *record,
    const PoolRuntimeProposalTracker *t, uint32_t source_generation,
    bool epoch_candidate_valid, uint64_t epoch_candidate_s,
    PoolRuntimeProposal *out);

/*
 * Deterministic field-by-field fingerprint (FNV-1a 32) of a semantic
 * proposal, covering the kind, the source generation and every persisted
 * field above. Explicit serialization — never a raw struct hash. ROLE: a
 * diagnostic token, an inexpensive preliminary INEQUALITY check and test
 * instrumentation ONLY. Because it is a pure function of the fields, a
 * differing fingerprint proves inequality; a MATCHING fingerprint proves
 * nothing (32-bit hashes collide) and never suffices for deduplication.
 */
uint32_t pool_runtime_proposal_fingerprint(const PoolRuntimeProposal *p);

/*
 * EXACT semantic-proposal equality: explicit field-by-field comparison of
 * every normalized proposal-key field (kind, source generation, state
 * update flag + proposed state + failure code, all three counters, reset
 * class, epoch flag + proposed floor, expected restore_required). Never a
 * memcmp over the padded struct. Total: NULL inputs are never equal.
 */
bool pool_runtime_proposal_equal(const PoolRuntimeProposal *a,
                                 const PoolRuntimeProposal *b);

/*
 * True only when the tracker's last proposal is PROVEN (commit + independent
 * readback verified + applicable B5 proof accepted) AND matches the
 * candidate by EXACT field-by-field equality — same kind, same source
 * generation, every normalized proposal-key field identical. The stored
 * fingerprint is consulted only as a preliminary inequality check; a
 * matching hash with any differing exact field is a DISTINCT proposal.
 */
bool pool_runtime_proposal_already_proven(const PoolRuntimeProposalTracker *t,
                                          const PoolRuntimeProposal *p);

/*
 * Record one durable, independently-read-back proposal COMMIT. Marks the
 * per-physical-boot reboot/consecutive accounting exactly when the proposal
 * carried those increments (proposal value != pre-commit committed value)
 * and advances the audit counter — those are facts about FLASH and must
 * survive a later proof-step failure. Does NOT mark the proposal complete.
 */
PoolRuntimeStatus pool_runtime_tracker_record_commit(
    PoolRuntimeProposalTracker *t, const PoolRuntimeProposal *p,
    const PoolSessionRecord *pre_commit_record, uint32_t committed_generation);

/*
 * Mark the committed proposal COMPLETE — call only after commit, independent
 * reload, full readback verification AND B5 proof acceptance (whenever an
 * owned lease exists) all succeeded. Completing the identical already-proven
 * proposal again returns RUNTIME_ERR_PERSIST_DUPLICATE and changes NOTHING.
 */
PoolRuntimeStatus pool_runtime_tracker_record_proven(
    PoolRuntimeProposalTracker *t, const PoolRuntimeProposal *p);

/*
 * Independent read-back proof of a committed proposal against the NORMALIZED
 * semantic proposal: the reloaded record must be a SESSION record with the
 * exact proposed state, failure code, counters and reset class, must
 * preserve the session identity and the restore obligation exactly, must
 * carry a strictly newer committed generation, and must never lower the
 * persisted trusted-epoch floor (a raised floor must land exactly).
 * Returns RUNTIME_OK only when every check passes.
 */
PoolRuntimeStatus pool_runtime_verify_proposal_readback(
    const PoolSessionRecord *before, const PoolSessionRecord *reloaded,
    const PoolRuntimeProposal *proposal, PoolStoreResult reload_result);

/* ------------------------------------------------------------------ */
/* Stable machine tokens (dot-free; never carry identities or prose)   */
/* ------------------------------------------------------------------ */

const char *pool_runtime_state_str(PoolRuntimeState s);
const char *pool_runtime_status_str(PoolRuntimeStatus st);
const char *pool_runtime_protocol_str(PoolRuntimeProtocolPermission p);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */

_Static_assert(POOL_RUNTIME_STATE__COUNT == 12,
               "runtime state count changed — review tables/tokens/tests");
_Static_assert(POOL_RUNTIME_STATUS__COUNT == 22,
               "runtime status count changed — review tokens/tests");
_Static_assert(POOL_RUNTIME_PROTOCOL__COUNT == 2,
               "protocol permission count changed — review the barrier");
_Static_assert(POOL_RUNTIME_PROTOCOL_HOLD == 0,
               "HOLD must be zero so a zeroed decision fails closed");
_Static_assert(RUNTIME_UNINITIALIZED == 0,
               "UNINITIALIZED must be zero so a zeroed control fails closed");
_Static_assert(RUNTIME_EVENT__ALL_VALID == 0x3Fu,
               "event mask changed — review sanitization/tests");
_Static_assert(POOL_RUNTIME_PROPOSAL_KIND__COUNT == 4,
               "proposal kind count changed — review fingerprint/tests");
_Static_assert(RUNTIME_PROPOSAL_NONE == 0,
               "NONE must be zero so a zeroed proposal persists nothing");
/* A new B3 result, B4 decision, B5 phase or B5 owner must be handled here
 * explicitly and must never fall through to an ALLOW. */
_Static_assert(POOL_STORE_RESULT__COUNT == 16,
               "PoolStoreResult changed — extend the B6 runtime table");
_Static_assert(POOL_BOOT_DECISION__COUNT == 7,
               "PoolBootDecision changed — extend the B6 runtime table");
_Static_assert(OP_PHASE__COUNT == 10 && OP_OWNER__COUNT == 9,
               "B5 ownership model changed — extend the B6 runtime table");

#endif /* POOL_SESSION_RUNTIME_CORE_H_ */
