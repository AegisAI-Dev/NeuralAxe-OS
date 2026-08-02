#ifndef POOL_SESSION_PREFLIGHT_H_
#define POOL_SESSION_PREFLIGHT_H_

#include <stdint.h>
#include <stdbool.h>
#include "pool_session_store.h"

/*
 * NeuralAxe timed pool sessions — READ-ONLY store preflight
 * (Phase 2M.1B, Gate B10.2). Board 601 / BM1370 only.
 *
 * WHY THIS EXISTS. Gate B10.1 found, and recorded, that the timed-session
 * store could not be classified before flashing an observation-pilot image:
 * the only sanitized read surface is the Gate B8 API that a pilot build must
 * keep disabled, and the only alternative was dumping the whole NVS
 * partition — which carries the Wi-Fi PSK and pool credentials. This gate
 * closes that gap with a dedicated, default-disabled, strictly read-only
 * posture that classifies ONE namespace and reports ONE bounded token.
 *
 * WHAT IT MAY DO: open the single "nx_tps" namespace NVS_READONLY, read the
 * three committed keys through the COMMITTED Gate B3 loader, and classify.
 *
 * WHAT IT MAY NEVER DO — enforced by construction, asserted by tests:
 *  - no nvs_set_*, no nvs_erase_*, no nvs_commit, on ANY path;
 *  - no namespace creation when the namespace is absent;
 *  - no enumeration of unrelated namespaces, no full-partition read;
 *  - no record repair, migration, normalization, promotion or rewrite;
 *  - no B3 mutation API, no B4 recovery transition, no B5 lease, no B7
 *    execution, no B8 route, no SNTP, no protocol/ASIC/tuning change, no
 *    restart.
 *
 * PRIVACY BY CONSTRUCTION: no model here has a string field and no function
 * returns record contents. A pool host, port, account, worker, password, TLS
 * setting, session id, record generation, trusted epoch, lease token, CRC,
 * raw payload byte, key name, namespace name or NVS error string cannot be
 * carried, formatted or logged through this interface.
 *
 * A PREFLIGHT RESULT AUTHORIZES NOTHING. It is an input to an owner's
 * go/no-go decision and never grants target mining, execution or API access.
 */

#define NX_TPS_PREFLIGHT_MODEL_VERSION 1u

/* ------------------------------------------------------------------ */
/* Stable outcomes                                                     */
/* ------------------------------------------------------------------ */

/*
 * FAIL-CLOSED NUMBERING. INTERNAL_ERROR is deliberately 0 so that a zeroed
 * struct, a memset failure path or an uninitialized read reports the
 * BLOCKING outcome. Making EMPTY zero would mean "forgot to classify" and
 * "the store is safe" were the same value — the one mistake this gate exists
 * to make impossible.
 */
typedef enum {
    NX_TPS_PREFLIGHT_INTERNAL_ERROR = 0,
    NX_TPS_PREFLIGHT_EMPTY,
    NX_TPS_PREFLIGHT_CLEARED,
    NX_TPS_PREFLIGHT_RECORD_PRESENT,
    NX_TPS_PREFLIGHT_TERMINAL_PENDING,
    NX_TPS_PREFLIGHT_COMMIT_UNCERTAIN,
    NX_TPS_PREFLIGHT_CORRUPT,
    NX_TPS_PREFLIGHT_UNSUPPORTED_SCHEMA,
    NX_TPS_PREFLIGHT_IO_ERROR,
    NX_TPS_PREFLIGHT_NVS_INIT_FAILED,
    NX_TPS_PREFLIGHT__COUNT
} NxTpsPreflightOutcome;

/*
 * The ONLY two outcomes that permit a later trusted-time observation pilot.
 * Total: any value outside the enum returns false.
 */
bool nx_tps_preflight_permits_pilot(NxTpsPreflightOutcome o);

/* Stable machine token (dot-free; never a namespace, key, identity or error
 * string). Unknown values resolve to a blocking token, never a permitting
 * one. */
const char *nx_tps_preflight_outcome_token(NxTpsPreflightOutcome o);

/* ------------------------------------------------------------------ */
/* Pure classification                                                 */
/* ------------------------------------------------------------------ */

/*
 * Everything the pure classifier may know. Fixed-width scalars and flags
 * only — no record, no pointer to mutable state, no string. The adapter
 * reads the record into its own buffer, extracts these few scalars through
 * the committed pure predicates, and zeroes the buffer; the classifier never
 * sees a record.
 */
typedef struct {
    uint32_t model_version;

    /* False when the read-only open reported "namespace does not exist".
     * With NVS_READONLY that is a pure query: nothing is created. Meaningful
     * ONLY when `namespace_open_failed` is false. */
    bool     namespace_present;

    /*
     * True when the read-only open failed for any reason other than "does not
     * exist" — the namespace could not be READ. Checked BEFORE absence, because
     * "unreadable" reported as "absent" would classify EMPTY and permit a pilot
     * on a store nobody actually looked at.
     */
    bool     namespace_open_failed;

    /*
     * True when the NVS subsystem itself could not be initialized without
     * destructive recovery. The preflight boot gate NEVER erases to recover, so
     * this is the honest "the store cannot be read at all, and nothing was
     * destroyed trying" case. It is always blocking.
     */
    bool     nvs_init_failed;

    /* True when the committed Gate B3 loader actually ran to completion. */
    bool     store_loaded;

    uint8_t  store_result;   /* PoolStoreResult, as a scalar */
    uint8_t  record_kind;    /* POOL_RECORD_KIND_*; 0 when no record   */
    uint8_t  session_state;  /* PoolSessionState;   0 when no record   */
    bool     record_valid;   /* the committed validator accepted it    */
    bool     restore_required;

    /* Audit counters from the read-only backend. Any non-zero value means
     * the preflight attempted a mutation, which is a broken preflight: the
     * classifier fails closed rather than trusting its own reading. */
    uint32_t write_attempts;
    uint32_t erase_attempts;
    uint32_t commit_attempts;
} NxTpsPreflightInput;

/* The bounded verdict. No string field, by construction. */
typedef struct {
    uint32_t              model_version;
    NxTpsPreflightOutcome outcome;
    bool                  permits_pilot;
    bool                  namespace_present;
    bool                  nvs_init_failed;
    uint32_t              read_attempts;   /* audit only */
    uint32_t              write_attempts;  /* MUST be 0  */
    uint32_t              erase_attempts;  /* MUST be 0  */
    uint32_t              commit_attempts; /* MUST be 0  */
} NxTpsPreflightResult;

/* Zero a result to the fail-closed INTERNAL_ERROR posture. */
void nx_tps_preflight_result_init(NxTpsPreflightResult *out);

/*
 * THE pure classification. Total, deterministic, input-immutable; *out is
 * fully written on every path. A NULL input, a model mismatch, an
 * out-of-range enum or ANY attempted mutation yields INTERNAL_ERROR.
 *
 * Unknown, truncated, corrupt or unreadable data NEVER becomes EMPTY or
 * CLEARED — the two outcomes that would let a pilot proceed.
 */
void nx_tps_preflight_classify(const NxTpsPreflightInput *in,
                               NxTpsPreflightResult *out);

/* ------------------------------------------------------------------ */
/* Bounded reporting                                                   */
/* ------------------------------------------------------------------ */

#define NX_TPS_PREFLIGHT_LINE_MAX 160u

/*
 * Format the one bounded summary line. Returns characters written, or 0 when
 * the arguments are invalid or the buffer is too small — in which case `buf`
 * is left empty rather than truncated, because a half-written classification
 * line is worse than none. Carries no namespace, key, size, generation,
 * epoch, identity or error text.
 */
uint32_t nx_tps_preflight_format(const NxTpsPreflightResult *r, uint32_t uptime_s,
                                 char *buf, uint32_t cap);

/* ------------------------------------------------------------------ */
/* Read-only ESP-IDF backend (adapter; see pool_session_preflight_nvs.c) */
/* ------------------------------------------------------------------ */

/*
 * Context for the read-only backend. `write_attempts` and `commit_attempts`
 * make "this preflight performed no mutation" a measured runtime fact rather
 * than a claim: those ops exist only because the committed Gate B3 store
 * contract requires a complete ops table, and they REFUSE unconditionally
 * while counting the attempt.
 *
 * `erase_attempts` is different and deliberately so: PoolStoreBackendOps has
 * NO erase entry, so nothing in production can increment it. It is a
 * structural placeholder that keeps the fail-closed check total if an erase op
 * is ever added, and the tests set it synthetically. Do not read it as
 * evidence that an erase path was exercised and refused — there is no erase
 * path to exercise.
 */
typedef struct {
    uint32_t nvs_handle;
    bool     open;
    bool     namespace_present;
    /*
     * Set when nvs_open() failed for any reason OTHER than "namespace does not
     * exist". "Absent" and "unreadable" must never share a representation: an
     * unreadable store reported as absent would classify EMPTY and permit a
     * pilot. Kept separate from `namespace_present` for exactly that reason.
     */
    bool     open_failed;
    uint32_t read_attempts;
    uint32_t write_attempts;
    uint32_t erase_attempts;
    uint32_t commit_attempts;
} NxTpsPreflightNvsBackend;

/* Reset a backend context to the closed, nothing-observed posture. */
void nx_tps_preflight_nvs_init(NxTpsPreflightNvsBackend *b);

/*
 * The READ-ONLY operations table. `open` uses NVS_READONLY, so a missing
 * namespace is reported rather than created; `write_blob`, `commit` and any
 * erase path refuse unconditionally and count the attempt. Never calls
 * nvs_flash_init and never touches another namespace.
 */
const PoolStoreBackendOps *nx_tps_preflight_nvs_ops(void);

/* True when the last open found the namespace absent (classified EMPTY). */
bool nx_tps_preflight_nvs_namespace_absent(const NxTpsPreflightNvsBackend *b);

/* ------------------------------------------------------------------ */
/* One-shot boot inspection (see pool_session_preflight_boot.c)        */
/* ------------------------------------------------------------------ */

/*
 * Run EXACTLY ONE read-only classification and emit the bounded tokens.
 * Idempotent: a second call returns the cached result and re-reads nothing,
 * so no polling loop can exist. Safe to call only after the existing NVS
 * subsystem is initialized. A no-op returning the fail-closed result when
 * CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT is disabled.
 */
void nx_tps_preflight_run_once(NxTpsPreflightResult *out);

/*
 * THE preflight boot gate. Initializes NVS **without destructive recovery**,
 * then runs the one-shot inspection.
 *
 *   nvs_flash_init()  — and NOTHING else on failure. ESP_ERR_NVS_NO_FREE_PAGES,
 *   ESP_ERR_NVS_NEW_VERSION_FOUND and every other initialization error fail
 *   CLOSED: no nvs_flash_erase(), no retry, no partition wipe.
 *
 * Returns true when NVS initialized cleanly and the inspection ran, meaning the
 * caller may continue its normal boot. Returns false when NVS could not be
 * initialized: the caller MUST then stop — no configuration initialization, no
 * Wi-Fi, no pool, no protocol, no mining — because configuration integrity is
 * unknown and the only safe posture is inert.
 *
 * A no-op returning true when the preflight flag is disabled, so the shipped
 * default boot order is unchanged.
 */
bool nx_tps_preflight_boot_gate(void);

/* The compile-time value of CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT. */
bool nx_tps_preflight_enabled(void);

/* Number of classifications performed this boot. The gate contract requires
 * at most 1. */
uint32_t nx_tps_preflight_run_count(void);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */
_Static_assert(NX_TPS_PREFLIGHT_INTERNAL_ERROR == 0,
               "a zeroed verdict must BLOCK, never permit a pilot");
_Static_assert(NX_TPS_PREFLIGHT__COUNT == 10,
               "preflight outcome count changed — review tokens/tests/plan");

#endif /* POOL_SESSION_PREFLIGHT_H_ */
