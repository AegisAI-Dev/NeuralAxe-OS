#ifndef TUNING_RECOVERY_H_
#define TUNING_RECOVERY_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tuning_record.h"
#include "tuning_store.h"

/*
 * NeuralAxe Weather-Aware Tuning — pure boot-recovery decision (Gate W2).
 *
 * PURE: no NVS, no ESP-IDF, no clock reads, no logging, no heap, no tuning
 * application. Given the store's load result and (when present) the
 * committed record, this module decides — deterministically — what the
 * boot integrator (Gate W4) must do BEFORE any weather activity:
 *
 *  - a reboot during a profile transaction NEVER assumes the requested
 *    profile succeeded: the plan is rollback-to-last-known-safe (or
 *    retain-current-unverified when no last-known-safe exists), automatic
 *    upgrades are inhibited and the evidence is preserved;
 *  - a bounded boot-attempt budget prevents reboot loops: when exhausted
 *    the plan is RECOVERY_REQUIRED with operator surfacing, never another
 *    automatic attempt;
 *  - any store failure yields RECOVERY_REQUIRED with NO record mutation
 *    (the integrator must not write over recovery evidence);
 *  - the plan carries a persist-before-action PROPOSAL (next transaction
 *    state + boot-attempt counter) exactly like the B4 pattern: the
 *    integrator commits the proposal via the store BEFORE executing any
 *    action, and only when the store result was TUNING_STORE_OK.
 *
 * This module never selects, validates or applies profiles — rollback
 * target ids pass through as recorded; the Gate W1 eligibility rules run
 * at execution time in W4 (an ineligible rollback target surfaces
 * operator recovery there). Weather scheduling, networking and time trust
 * are entirely out of scope here.
 */

/* Policy threshold (storage bound is TUNING_TX_BOOT_ATTEMPT_STORE_MAX;
 * the decision threshold is deliberately lower — B3/B4 split). */
#define TUNING_BOOT_MAX_TX_BOOT_ATTEMPTS 3u

/* Decision values. 0 is the fail-safe default: a zeroed plan means
 * RECOVERY_REQUIRED, never a normal start. */
typedef enum {
    TUNING_BOOT_RECOVERY_REQUIRED = 0,
    TUNING_BOOT_NORMAL_START = 1,       /* no persisted policy state       */
    TUNING_BOOT_NORMAL_RESUME = 2,      /* state restored; tx safe         */
    TUNING_BOOT_ROLLBACK_TO_LAST_KNOWN_SAFE = 3,
    TUNING_BOOT_RETAIN_CURRENT_UNVERIFIED = 4,
    TUNING_BOOT__COUNT
} TuningBootDecision;

/* Bounded machine codes explaining the decision. */
typedef enum {
    TUNING_BOOT_CODE_NONE = 0,
    TUNING_BOOT_CODE_INVALID_INPUT,
    TUNING_BOOT_CODE_STORE_FAILED,
    TUNING_BOOT_CODE_TX_INTERRUPTED,
    TUNING_BOOT_CODE_ROLLBACK_IN_PROGRESS,
    TUNING_BOOT_CODE_NO_LAST_KNOWN_SAFE,
    TUNING_BOOT_CODE_BOOT_BUDGET_EXHAUSTED,
    TUNING_BOOT_CODE_TX_RECOVERY_STATE,
    TUNING_BOOT_CODE_TX_FINALIZE,   /* COMMITTED found durable at boot     */
    TUNING_BOOT_CODE__COUNT
} TuningBootCode;

/*
 * BOOT-RECOVERY CONTEXT (Option A — bounded, NON-PERSISTED). Distinguishes
 * a same-boot re-evaluation from a real new boot:
 *  - it lives in RAM only and is NEVER persisted, so it starts CLEAR on
 *    every real boot by construction — it cannot suppress an increment
 *    after a reboot, and no wall-clock, Stratum ntime or reset-reason
 *    value is used to identify a boot (the context's RAM lifetime IS the
 *    boot identity);
 *  - the integrator initializes it with tuning_boot_context_init exactly
 *    once at boot, and marks it with tuning_boot_context_note_committed
 *    ONLY after the attempt-increment proposal was committed through the
 *    store with result TUNING_STORE_OK (whose commit path performs the
 *    byte-exact + semantic read-back). STORE_COMMIT_UNCERTAIN,
 *    READBACK_MISMATCH or any other non-OK result must NOT be noted — an
 *    uncertain commit never authorizes rollback execution;
 *  - while the noted (generation, attempt-count) pair matches the
 *    committed record, re-planning resumes the ALREADY-RESERVED attempt:
 *    no further increment, rollback_action_eligible = true. Task wakeups
 *    in the same boot therefore consume exactly one durable unit;
 *  - during an unresolved rollback obligation the integrator must not
 *    interleave unrelated record commits before executing the reserved
 *    attempt (a generation mismatch conservatively demands a fresh
 *    reservation).
 */
typedef struct {
    bool attempt_committed_this_boot;     /* durable reservation exists    */
    uint32_t committed_record_generation; /* generation of that record     */
    uint8_t committed_attempt_count;      /* durable count after commit    */
} TuningBootContext;

/* Clear the context (call exactly once per real boot). */
void tuning_boot_context_init(TuningBootContext *ctx);

/* Note a durably committed + read-back-verified attempt reservation. */
void tuning_boot_context_note_committed(TuningBootContext *ctx,
                                        uint32_t record_generation,
                                        uint8_t attempt_count);

typedef struct {
    TuningStoreResult store_result;
    bool record_present;              /* true iff record points at a
                                         decoded committed STATE record   */
    const TuningPolicyRecord *record; /* required when store_result == OK */
    TuningBootContext context;        /* zeroed == fresh boot              */
} TuningBootInput;

typedef struct {
    TuningBootDecision decision;
    bool upgrade_inhibit;    /* no automatic upgrade until cleared by W4  */
    bool operator_surface;   /* surface operator recovery in status/UI    */
    /* True ONLY for a durable COMMITTED transaction: the integrator runs
     * tuning_record_finalize_transaction (tx subrecord -> canonical IDLE,
     * everything else preserved) and commits the combined record. NEVER a
     * tombstone, NEVER a re-apply of tuning in W2. */
    bool finalize_transaction;
    char rollback_profile_id[TUNING_PROFILE_ID_MAX]; /* set iff ROLLBACK  */
    /* Persist-before-action proposal (meaningful ONLY when the store
     * loaded OK; never write after a store failure). For a ROLLBACK plan
     * the integrator applies it as: tx.state = proposed_tx_state,
     * tx.boot_attempt_count = proposed_boot_attempt_count,
     * tx.rollback_profile_id = rollback_profile_id. */
    TuningTxState proposed_tx_state;
    uint8_t proposed_boot_attempt_count;
    /* True ONLY when the durable attempt reservation for THIS boot already
     * exists (boot context matches the committed record). Rollback
     * EXECUTION is permitted only when true; when false, the integrator
     * must first commit the increment proposal (read-back verified) and
     * note it in the boot context. */
    bool rollback_action_eligible;
    TuningBootCode primary_code;
    TuningBootCode secondary_code;
} TuningBootPlan;

/*
 * Deterministic plan (memset-first; identical inputs produce memcmp-equal
 * plans). NULL/invalid inputs yield the conservative RECOVERY plan.
 *
 * TOTAL DECISION TABLE — every TuningTxState is handled explicitly and the
 * compile pin below forces this table to be re-audited whenever a state is
 * added; an unknown/future state fails closed to RECOVERY_REQUIRED with
 * evidence preserved:
 *   IDLE               -> NORMAL_RESUME; no rollback, no apply, no counter
 *                         increment; durable policy state preserved.
 *   COMMITTED          -> NORMAL_RESUME + finalize_transaction: the
 *                         selected profile is already verified & durable;
 *                         last-known-safe preserved; no rollback; only the
 *                         canonicalize-to-IDLE proposal; never re-apply.
 *   INTENT_PERSISTED / APPLY_PENDING / APPLYING / RESTART_PENDING /
 *   VERIFYING          -> apply success UNKNOWN; never assumed. With a
 *                         valid last-known-safe: ROLLBACK proposal +
 *                         upgrade inhibit + persist-before-action.
 *                         Without one: RETAIN_CURRENT_UNVERIFIED +
 *                         operator recovery; no invented safe profile.
 *   ROLLBACK_PENDING / ROLLING_BACK
 *                      -> rollback obligation preserved and RESUMED (the
 *                         recorded target, else last-known-safe); never a
 *                         second independent rollback transaction; the
 *                         attempt counter is never reset, and each REAL
 *                         boot must reserve the next attempt before action
 *                         (see the budget contract below) so repeated
 *                         reboots can never bypass the limit.
 *   RECOVERY_REQUIRED  -> operator recovery only; no automatic apply, no
 *                         automatic rollback retry, no tombstone, no
 *                         transition to IDLE.
 *
 * BOOT-ATTEMPT BUDGET (durable + monotonic; owner final-blocker contract):
 * every boot that observes an unresolved automatic rollback obligation
 * consumes AT MOST ONE durable rollback-attempt unit before rollback
 * action is permitted, and repeated reboots CANNOT resume forever:
 *  - FIRST qualifying evaluation in a boot (context clear): the plan
 *    proposes exactly one increment, rollback_action_eligible = false —
 *    the integrator persists the proposal (read-back verified by the
 *    store) and notes it in the boot context BEFORE any action;
 *  - SAME-BOOT re-evaluation (context matches the committed record):
 *    the already-reserved attempt is resumed — no further increment,
 *    rollback_action_eligible = true;
 *  - REAL REBOOT: the context is RAM-only and starts clear, so the next
 *    boot reserves the NEXT attempt (one more durable unit). This holds
 *    for ROLLBACK_PENDING (attempt reserved covers only the boot that
 *    reserved it: A NEW BOOT MUST RESERVE THE NEXT ATTEMPT BEFORE ACTION)
 *    and for ROLLING_BACK (a reboot means the previous attempt did not
 *    reach durable completion: it is treated as interrupted and the next
 *    attempt must be reserved). Three crash/reboot cycles therefore
 *    deterministically reach the limit;
 *  - reaching TUNING_BOOT_MAX_TX_BOOT_ATTEMPTS (stored count, new
 *    reservations only) yields RECOVERY_REQUIRED — never another
 *    automatic attempt; corrupted large values fail closed the same way;
 *    the saturating increment never wraps; invalid encodings are rejected
 *    at decode (store INVALID_RECORD -> recovery);
 *  - an UNCERTAIN commit or readback mismatch is never noted in the
 *    context, so rollback execution never becomes eligible from it;
 *  - IDLE/COMMITTED boots never consume budget; a missing last-known-safe
 *    consumes at most one increment before the terminal RECOVERY_REQUIRED
 *    proposal stops further automatic attempts.
 */
void tuning_boot_plan(const TuningBootInput *in, TuningBootPlan *out);

/* Stable machine tokens. */
const char *tuning_boot_decision_str(TuningBootDecision d);
const char *tuning_boot_code_str(TuningBootCode c);

/* Compile-time guards. */
_Static_assert(TUNING_BOOT_RECOVERY_REQUIRED == 0,
               "zeroed plan must mean RECOVERY_REQUIRED (fail safe)");
_Static_assert(TUNING_BOOT__COUNT == 5 && TUNING_BOOT_CODE__COUNT == 9,
               "boot plan enums changed — review tokens/tests");
_Static_assert(TUNING_BOOT_MAX_TX_BOOT_ATTEMPTS < TUNING_TX_BOOT_ATTEMPT_STORE_MAX,
               "policy threshold must stay below the storage bound");
/* The decision table above is TOTAL over the transaction states: adding a
 * state must break the build here until the table (and its tests) are
 * re-audited. */
_Static_assert(TUNING_TX__COUNT == 10,
               "boot-recovery decision table must be re-audited for new states");

#endif /* TUNING_RECOVERY_H_ */
