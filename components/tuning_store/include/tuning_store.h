#ifndef TUNING_STORE_H_
#define TUNING_STORE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tuning_record.h"

/*
 * NeuralAxe Weather-Aware Tuning — crash-safe dual-slot persistent store
 * (Gate W2). Mirrors the PROVEN Gate B3 pool-session store algorithm in a
 * NEW namespace with a NEW record family (the B3 namespace/schema are
 * deliberately not reused — Gate W0 rule).
 *
 * Caller-owned, synchronous, single-owner contract: no task, no queue, no
 * networking, no tuning application, no restart. All persistence flows
 * through an injected backend-operations table; the real ESP-IDF NVS
 * backend lives in tuning_store_nvs.c and is NEVER instantiated by any
 * production runtime path in Gate W2 (Gate W4 owns wiring under the B5
 * operation-ownership lease).
 *
 * CRASH-CONSISTENCY MODEL (identical to B3):
 *  - Two record slots (keys "rec_a"/"rec_b") + a separately encoded,
 *    versioned active-slot pointer (key "active").
 *  - A commit writes the INACTIVE slot, commits it, reads it back and
 *    verifies it byte-exactly and semantically, and only then writes,
 *    commits, reads back and verifies the pointer. THE POINTER WRITE IS
 *    THE LOGICAL COMMIT POINT.
 *  - Until the new pointer verifies, the OLD pointer-selected record stays
 *    authoritative; a staged newer inactive slot is IGNORED and never
 *    auto-promoted. The loader never guesses between ambiguous slots and
 *    never selects "highest generation" when the pointer is corrupt or
 *    missing — those are recovery results, not guesses.
 *  - The previously active slot is never erased by a commit: it remains as
 *    crash-recovery evidence.
 *  - NORMAL TRANSACTION COMPLETION NEVER CLEARS THE POLICY RECORD: after a
 *    durably verified COMMITTED transaction the integrator finalizes via
 *    tuning_record_finalize_transaction (canonicalizing ONLY the
 *    transaction subrecord to IDLE, preserving settings, climate state,
 *    last-known-safe, override, cooldown, epoch floor and counters) and
 *    commits the updated combined record through this same dual-slot path.
 *  - A full policy-store tombstone exists ONLY as an EXPLICIT COMPLETE
 *    POLICY RESET (tuning_store_admin_reset below) — an administrative
 *    operation with NO production caller in W2 and none permitted from
 *    transaction completion, boot recovery, rollback completion, automatic
 *    error recovery, cooldown/override expiration, profile verification or
 *    routine policy disablement. Even that reset refuses to destroy
 *    pending/rollback/recovery evidence (TUNING_STORE_STATE_CONFLICT
 *    outside the safe IDLE/COMMITTED transaction states).
 *
 * Generations/slots/CRCs provide CRASH CONSISTENCY ONLY — not
 * authentication, anti-tamper, anti-rollback or anti-forgery (documented
 * residual; see tuning_record.h).
 *
 * FLASH-WEAR NOTE for Gate W4 (W2 decides no cadence): commit ONLY on
 * state transitions (settings change, tx transition, override change,
 * climate stance change, bounded floor advance) — never per telemetry
 * sample or scheduler wakeup. The 24 KB "nvs" partition is shared with the
 * entire device configuration and hits a DESTRUCTIVE full-erase failure
 * mode when full (Gate W0 §10) — keep this store's footprint small.
 */

/* Dedicated namespace and bounded keys (ESP-IDF limit: 15 chars + NUL).
 * Namespace is NEW — never the B3 "nx_tps". */
#define TUNING_STORE_NVS_NAMESPACE "nx_wtp"
#define TUNING_STORE_KEY_SLOT_A    "rec_a"
#define TUNING_STORE_KEY_SLOT_B    "rec_b"
#define TUNING_STORE_KEY_ACTIVE    "active"

/* ------------------------------------------------------------------ */
/* Stable store results                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    TUNING_STORE_OK = 0,               /* committed STATE record loaded/committed */
    TUNING_STORE_EMPTY,                /* no committed state exists at all        */
    TUNING_STORE_CLEARED,              /* the committed state is a tombstone      */
    TUNING_STORE_NOT_INITIALIZED,
    TUNING_STORE_INVALID_ARGUMENT,
    TUNING_STORE_IO_ERROR,
    TUNING_STORE_CORRUPT,              /* pointed slot fails CRC/byte validation  */
    TUNING_STORE_UNSUPPORTED_SCHEMA,   /* incl. W1 profile-model version gate     */
    TUNING_STORE_INVALID_RECORD,       /* decoded but semantically invalid        */
    TUNING_STORE_ACTIVE_POINTER_INVALID, /* pointer corrupt — never guess         */
    TUNING_STORE_ACTIVE_SLOT_INVALID,  /* pointer valid, pointed slot missing     */
    TUNING_STORE_RECOVERY_REQUIRED,    /* ambiguous/mismatched committed state    */
    TUNING_STORE_GENERATION_EXHAUSTED, /* generation would wrap — refuse          */
    TUNING_STORE_READBACK_MISMATCH,    /* staged slot failed verify; old intact   */
    TUNING_STORE_COMMIT_UNCERTAIN,     /* pointer phase failed; reload for truth  */
    TUNING_STORE_STATE_CONFLICT,       /* operation not allowed for this state    */
    TUNING_STORE_RESULT__COUNT
} TuningStoreResult;

/* Classification helpers (total; safe for any value). permits_state_load
 * is exactly TUNING_STORE_OK — every other result (including EMPTY and
 * CLEARED) yields NO policy state and therefore NO tuning recommendation
 * from this layer, ever. */
bool tuning_store_result_requires_recovery(TuningStoreResult r);
bool tuning_store_result_permits_state_load(TuningStoreResult r);

/* ------------------------------------------------------------------ */
/* Abstract backend operations                                         */
/* ------------------------------------------------------------------ */

#define TUNING_STORE_BACKEND_OK        0
#define TUNING_STORE_BACKEND_NOT_FOUND 1
#define TUNING_STORE_BACKEND_IO        2

/*
 * Injected persistence operations (contract identical to B3):
 *  - read_blob: on success writes the STORED length to *out_len; copies
 *    into buf only when it fits cap (a larger stored value returns OK with
 *    *out_len > cap and buf untouched — treated as corrupt, not I/O).
 *    Missing key => NOT_FOUND.
 *  - write_blob stages a value; commit makes staged values durable. Power
 *    loss loses staged values and preserves committed ones (conservative
 *    reading of the documented ESP-IDF NVS per-pair guarantee).
 *  - All ops are synchronous and must not block indefinitely.
 */
typedef struct {
    int (*open)(void *ctx);
    int (*read_blob)(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len);
    int (*write_blob)(void *ctx, const char *key, const uint8_t *buf, size_t len);
    int (*commit)(void *ctx);
    int (*close)(void *ctx);
} TuningStoreBackendOps;

/* ------------------------------------------------------------------ */
/* Store instance                                                      */
/* ------------------------------------------------------------------ */

/* Extra load diagnostics (never required for correctness). */
typedef struct {
    bool     duplicate_generation; /* both slots valid, same generation   */
    bool     staged_newer_ignored; /* staged newer slot exists — ignored  */
    uint8_t  active_slot;
    uint32_t committed_generation;
} TuningStoreLoadInfo;

/* Caller-owned instance. Work/verify buffers live inside the instance so
 * no heap is needed — keep instances file-static or long-lived, not on
 * small task stacks. */
typedef struct {
    const TuningStoreBackendOps *ops;
    void                        *ctx;
    bool                         initialized;
    bool                         have_committed;
    uint8_t                      committed_slot;
    uint32_t                     committed_generation;
    uint8_t                      work[TUNING_RECORD_MAX_ENCODED];
    uint8_t                      verify[TUNING_RECORD_MAX_ENCODED];
} TuningStore;

/* ------------------------------------------------------------------ */
/* API (synchronous; no tasks, no queues, no tuning mutation)          */
/* ------------------------------------------------------------------ */

/* Bind the backend and open it. NVS itself must already be initialized by
 * the future caller — this layer never calls nvs_flash_init or erases. */
TuningStoreResult tuning_store_init(TuningStore *st,
                                    const TuningStoreBackendOps *ops, void *ctx);

/* Close the backend and reset the instance. Idempotent. */
TuningStoreResult tuning_store_deinit(TuningStore *st);

/* Deterministic load of the committed state:
 *  TUNING_STORE_OK      -> *out is the committed STATE record
 *  TUNING_STORE_CLEARED -> committed tombstone; no policy state
 *  TUNING_STORE_EMPTY   -> nothing committed at all
 *  everything else: no state, no guessing, no mutation recommendation.
 * `out` and `info` may be NULL when not needed. */
TuningStoreResult tuning_store_load(TuningStore *st,
                                    TuningPolicyRecord *out,
                                    TuningStoreLoadInfo *info);

/* Crash-safe dual-slot commit. On TUNING_STORE_OK the record's
 * `generation` has been assigned the newly committed generation. A store
 * in any recovery condition refuses to commit over the evidence. */
TuningStoreResult tuning_store_commit_record(TuningStore *st,
                                             TuningPolicyRecord *rec);

/*
 * EXPLICIT COMPLETE POLICY RESET (administrative operation ONLY).
 * Commits a TOMBSTONE through the same crash-safe dual-slot path, erasing
 * the entire persisted weather-policy state. NO automatic code path may
 * call this — it is not reachable from transaction completion, boot
 * recovery, rollback completion, automatic error recovery, cooldown or
 * override expiration, profile verification or routine policy disablement
 * (normal completion uses tuning_record_finalize_transaction + a regular
 * commit instead). W2 ships NO production caller; a future gate may wire
 * it behind an explicit owner-facing administrative action only.
 * Allowed ONLY when the committed record's transaction is in a safe state
 * (IDLE or COMMITTED); every pending/rollback/recovery transaction is
 * preserved evidence (TUNING_STORE_STATE_CONFLICT).
 */
TuningStoreResult tuning_store_admin_reset(TuningStore *st);

/* Committed generation of the cached committed state (valid after a
 * successful load/commit in this instance). False when unknown. */
bool tuning_store_committed_generation(const TuningStore *st,
                                       uint32_t *out_generation);

/* Stable machine token (dot-free; never carries key names or values). */
const char *tuning_store_result_str(TuningStoreResult r);

/* ------------------------------------------------------------------ */
/* Real ESP-IDF NVS backend (compiled; never wired in W2)              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t nvs_handle;
    bool     open;
} TuningStoreNvsBackend;

/* The actual ESP-IDF NVS operations table. Opens ONLY the dedicated
 * "nx_wtp" namespace; never calls nvs_flash_init; never erases anything;
 * never logs payloads or NVS error text. */
const TuningStoreBackendOps *tuning_store_nvs_ops(void);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */
_Static_assert(sizeof(TUNING_STORE_NVS_NAMESPACE) <= 16, "namespace exceeds NVS limit");
_Static_assert(sizeof(TUNING_STORE_KEY_SLOT_A) <= 16, "key exceeds NVS limit");
_Static_assert(sizeof(TUNING_STORE_KEY_SLOT_B) <= 16, "key exceeds NVS limit");
_Static_assert(sizeof(TUNING_STORE_KEY_ACTIVE) <= 16, "key exceeds NVS limit");
_Static_assert(TUNING_STORE_RESULT__COUNT == 16,
               "store result count changed — review tokens/tests");

#endif /* TUNING_STORE_H_ */
