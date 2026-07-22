#ifndef POOL_SESSION_STORE_H_
#define POOL_SESSION_STORE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pool_session_record.h"

/*
 * NeuralAxe timed pool sessions — crash-safe dual-slot persistent store
 * (Phase 2M.1B, Gate B3).
 *
 * Caller-owned, synchronous, single-threaded contract: no task, no queue,
 * no networking, no pool mutation, no restart. All persistence flows
 * through an injected backend-operations table; the real ESP-IDF NVS
 * backend lives in pool_session_store_nvs.c and is NEVER instantiated by
 * any production runtime path in Gate B3 (Gate B5 owns wiring/ownership).
 *
 * CRASH-CONSISTENCY MODEL (Phase 2M.1A §11):
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
 *    missing — those cases are recovery results, not guesses.
 *  - The previously active slot is never erased by a commit: it remains as
 *    crash-recovery evidence.
 *  - Acknowledgement/clear is itself a crash-safe commit of a TOMBSTONE
 *    record through the exact same path; the old record is not erased
 *    before the tombstone pointer commit succeeds.
 *
 * These generations/slots/CRCs provide CRASH CONSISTENCY ONLY. They are
 * not authentication, anti-tamper, anti-rollback or anti-forgery: malicious
 * offline rollback or forgery of records remains a documented residual on
 * this build (see pool_session_record.h).
 *
 * FLASH-WEAR NOTE for Gates B6/B7 (B3 decides no cadence): do NOT commit on
 * every telemetry tick. Choose state-transition-only persistence plus a
 * bounded heartbeat (Phase 2M.1A proposes 60 s while TARGET_ACTIVE) and
 * evaluate NVS wear before runtime wiring; the 24 KB "nvs" partition is
 * shared with the whole pool configuration.
 */

/* Dedicated namespace and bounded keys (ESP-IDF limit: 15 chars + NUL). */
#define POOL_STORE_NVS_NAMESPACE "nx_tps"
#define POOL_STORE_KEY_SLOT_A    "rec_a"
#define POOL_STORE_KEY_SLOT_B    "rec_b"
#define POOL_STORE_KEY_ACTIVE    "active"

/* ------------------------------------------------------------------ */
/* Stable store results                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    STORE_OK = 0,                 /* a committed SESSION record was loaded/committed */
    STORE_EMPTY,                  /* no committed state exists at all               */
    STORE_CLEARED,                /* the committed state is a tombstone             */
    STORE_NOT_INITIALIZED,
    STORE_INVALID_ARGUMENT,
    STORE_IO_ERROR,
    STORE_CORRUPT,                /* pointed slot fails CRC/byte validation         */
    STORE_UNSUPPORTED_SCHEMA,
    STORE_INVALID_RECORD,         /* decoded but semantically invalid               */
    STORE_ACTIVE_POINTER_INVALID, /* pointer corrupt/unsupported — never guess      */
    STORE_ACTIVE_SLOT_INVALID,    /* pointer valid but pointed slot missing         */
    STORE_RECOVERY_REQUIRED,      /* ambiguous/mismatched committed state           */
    STORE_GENERATION_EXHAUSTED,   /* generation would wrap — refuse                 */
    STORE_READBACK_MISMATCH,      /* staged slot did not verify; old record intact  */
    STORE_COMMIT_UNCERTAIN,       /* pointer phase failed; reload to learn truth    */
    STORE_STATE_CONFLICT,         /* operation not allowed for the committed state  */
    POOL_STORE_RESULT__COUNT
} PoolStoreResult;

/*
 * Classification helpers (total; safe for any value):
 *  - requires_recovery: the committed state cannot be trusted as-is and
 *    Gate B4 must run its recovery table.
 *  - permits_session_load: exactly STORE_OK — every other result (including
 *    EMPTY and CLEARED) yields NO active session and therefore NO
 *    pool-mutation recommendation from this layer, ever.
 */
bool pool_store_result_requires_recovery(PoolStoreResult r);
bool pool_store_result_permits_session_load(PoolStoreResult r);

/* ------------------------------------------------------------------ */
/* Abstract backend operations                                         */
/* ------------------------------------------------------------------ */

/* Backend status codes returned by the ops below. */
#define POOL_STORE_BACKEND_OK        0
#define POOL_STORE_BACKEND_NOT_FOUND 1
#define POOL_STORE_BACKEND_IO        2

/*
 * Injected persistence operations. Contract:
 *  - read_blob: on success writes the STORED length to *out_len; copies the
 *    value into buf only when it fits cap (a larger stored value returns
 *    OK with *out_len > cap and buf untouched — the store treats that as a
 *    corrupt record, not an I/O error). Missing key => NOT_FOUND.
 *  - write_blob stages a value; commit makes staged values durable. The
 *    fake backend models power loss as "staged values are lost, committed
 *    values survive", which is the conservative reading of the documented
 *    ESP-IDF NVS guarantee (an interrupted single-pair write preserves the
 *    old value).
 *  - All ops are synchronous and must not block indefinitely.
 */
typedef struct {
    int (*open)(void *ctx);
    int (*read_blob)(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len);
    int (*write_blob)(void *ctx, const char *key, const uint8_t *buf, size_t len);
    int (*commit)(void *ctx);
    int (*close)(void *ctx);
} PoolStoreBackendOps;

/* ------------------------------------------------------------------ */
/* Store instance                                                      */
/* ------------------------------------------------------------------ */

/* Extra load diagnostics (never required for correctness). */
typedef struct {
    bool     duplicate_generation; /* both slots valid with the same generation */
    bool     staged_newer_ignored; /* a staged newer slot exists and was ignored */
    uint8_t  active_slot;
    uint32_t committed_generation;
} PoolStoreLoadInfo;

/*
 * Caller-owned instance. Transparent for tests; treat as opaque otherwise.
 * The work/verify buffers are inside the instance so no heap is needed —
 * keep instances file-static or long-lived, not on small task stacks.
 */
typedef struct {
    const PoolStoreBackendOps *ops;
    void                      *ctx;
    bool                       initialized;
    bool                       have_committed;      /* cached after load/commit */
    uint8_t                    committed_slot;
    uint32_t                   committed_generation;
    uint8_t                    work[POOL_RECORD_MAX_ENCODED];
    uint8_t                    verify[POOL_RECORD_MAX_ENCODED];
} PoolSessionStore;

/* ------------------------------------------------------------------ */
/* API (synchronous; no tasks, no queues, no pool mutation)            */
/* ------------------------------------------------------------------ */

/* Bind the backend and open it. NVS itself must already be initialized by
 * the future caller — this layer never calls nvs_flash_init or erases. */
PoolStoreResult pool_session_store_init(PoolSessionStore *st,
                                        const PoolStoreBackendOps *ops, void *ctx);

/* Close the backend and reset the instance. Idempotent. */
PoolStoreResult pool_session_store_deinit(PoolSessionStore *st);

/*
 * Deterministic load of the committed state (Stage-10 case table):
 *  STORE_OK       -> *out is the committed SESSION record
 *  STORE_CLEARED  -> committed tombstone; no active session
 *  STORE_EMPTY    -> nothing committed at all
 *  every other result: no session, no guessing, no mutation recommendation.
 * `out` and `info` may be NULL when not needed.
 */
PoolStoreResult pool_session_store_load(PoolSessionStore *st,
                                        PoolSessionRecord *out,
                                        PoolStoreLoadInfo *info);

/*
 * Crash-safe dual-slot commit (Stage-9 sequence). On STORE_OK the record's
 * `generation` field has been assigned the newly committed generation.
 * Requires a healthy committed state (empty, cleared, or a valid session);
 * a store in any recovery condition refuses to commit over the evidence.
 */
PoolStoreResult pool_session_store_commit_record(PoolSessionStore *st,
                                                 PoolSessionRecord *rec);

/*
 * Crash-safe acknowledgement/clear: commits a TOMBSTONE through the same
 * dual-slot path. Allowed ONLY when the committed record is an
 * acknowledgeable terminal session (COMPLETE, or pre-mutation CANCELLED /
 * RECOVERY_REQUIRED) with restore_required == false. An unresolved restore
 * obligation can never be cleared (STORE_STATE_CONFLICT).
 */
PoolStoreResult pool_session_store_commit_clear(PoolSessionStore *st);

/* Committed generation of the cached committed state (valid after a
 * successful load/commit in this instance). Returns false when unknown. */
bool pool_session_store_committed_generation(const PoolSessionStore *st,
                                             uint32_t *out_generation);

/* Stable machine token (dot-free; never carries key names, identities,
 * account values or raw NVS error text). */
const char *pool_store_result_str(PoolStoreResult r);

/* ------------------------------------------------------------------ */
/* Real ESP-IDF NVS backend (compiled; never wired in B3)              */
/* ------------------------------------------------------------------ */

/* Opaque-ish context for the real backend: holds the NVS handle for the
 * dedicated namespace. sizeof(handle storage) is static-asserted against
 * nvs_handle_t in the adapter. */
typedef struct {
    uint32_t nvs_handle;
    bool     open;
} PoolStoreNvsBackend;

/* The actual ESP-IDF NVS operations table. Opens ONLY the dedicated
 * "nx_tps" namespace; never calls nvs_flash_init; never erases anything;
 * never logs payloads, identities or account values. */
const PoolStoreBackendOps *pool_store_nvs_ops(void);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */
_Static_assert(sizeof(POOL_STORE_NVS_NAMESPACE) <= 16, "namespace exceeds NVS limit");
_Static_assert(sizeof(POOL_STORE_KEY_SLOT_A) <= 16, "key exceeds NVS limit");
_Static_assert(sizeof(POOL_STORE_KEY_SLOT_B) <= 16, "key exceeds NVS limit");
_Static_assert(sizeof(POOL_STORE_KEY_ACTIVE) <= 16, "key exceeds NVS limit");
_Static_assert(POOL_STORE_RESULT__COUNT == 16, "store result count changed — review tokens/tests");

#endif /* POOL_SESSION_STORE_H_ */
