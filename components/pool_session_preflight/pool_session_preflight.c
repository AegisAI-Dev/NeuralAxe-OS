/*
 * NeuralAxe timed pool sessions — PURE read-only store-preflight classifier
 * (Phase 2M.1B, Gate B10.2).
 *
 * No ESP-IDF, no IO, no NVS, no FreeRTOS, no networking, no heap, no logging,
 * no global mutable state and no clock read. Every function is a total
 * function of its arguments, so identical inputs always produce byte-identical
 * output.
 *
 * The classifier never sees a record: the adapter extracts a handful of
 * scalars through the COMMITTED Gate B1/B3 predicates and passes those.
 */

#include <string.h>
#include <stdio.h>

#include "pool_session_preflight.h"
#include "pool_session.h"
#include "pool_session_record.h"

/* ------------------------------------------------------------------ */
/* Outcome vocabulary                                                  */
/* ------------------------------------------------------------------ */

bool nx_tps_preflight_permits_pilot(NxTpsPreflightOutcome o)
{
    /* Exactly two outcomes, listed positively. Anything else — including a
     * value outside the enum — blocks. */
    return o == NX_TPS_PREFLIGHT_EMPTY || o == NX_TPS_PREFLIGHT_CLEARED;
}

const char *nx_tps_preflight_outcome_token(NxTpsPreflightOutcome o)
{
    switch (o) {
    case NX_TPS_PREFLIGHT_EMPTY:              return "TPS_PREFLIGHT_EMPTY";
    case NX_TPS_PREFLIGHT_CLEARED:            return "TPS_PREFLIGHT_CLEARED";
    case NX_TPS_PREFLIGHT_RECORD_PRESENT:     return "TPS_PREFLIGHT_BLOCKED_RECORD";
    case NX_TPS_PREFLIGHT_TERMINAL_PENDING:   return "TPS_PREFLIGHT_BLOCKED_TERMINAL";
    case NX_TPS_PREFLIGHT_COMMIT_UNCERTAIN:   return "TPS_PREFLIGHT_BLOCKED_UNCERTAIN";
    case NX_TPS_PREFLIGHT_CORRUPT:            return "TPS_PREFLIGHT_BLOCKED_CORRUPT";
    case NX_TPS_PREFLIGHT_UNSUPPORTED_SCHEMA: return "TPS_PREFLIGHT_BLOCKED_SCHEMA";
    case NX_TPS_PREFLIGHT_IO_ERROR:           return "TPS_PREFLIGHT_BLOCKED_IO";
    case NX_TPS_PREFLIGHT_NVS_INIT_FAILED:    return "TPS_PREFLIGHT_BLOCKED_NVS_INIT";
    case NX_TPS_PREFLIGHT_INTERNAL_ERROR:
    case NX_TPS_PREFLIGHT__COUNT:
    default:                                  return "TPS_PREFLIGHT_INTERNAL_ERROR";
    }
}

/* ------------------------------------------------------------------ */
/* Classification                                                      */
/* ------------------------------------------------------------------ */

void nx_tps_preflight_result_init(NxTpsPreflightResult *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->model_version = NX_TPS_PREFLIGHT_MODEL_VERSION;
    out->outcome       = NX_TPS_PREFLIGHT_INTERNAL_ERROR; /* fail closed */
    out->permits_pilot = false;
}

/*
 * Map a committed Gate B3 PoolStoreResult onto a preflight outcome. Every
 * enumerator is handled explicitly — no default that could silently absorb a
 * future store result into a permitting outcome.
 */
static NxTpsPreflightOutcome outcome_for_store_result(PoolStoreResult r)
{
    switch (r) {
    case STORE_EMPTY:
        return NX_TPS_PREFLIGHT_EMPTY;
    case STORE_CLEARED:
        /* Derived, not invented: the committed loader returns STORE_CLEARED
         * exactly when the pointer-selected committed record decodes with
         * kind == POOL_RECORD_KIND_TOMBSTONE. */
        return NX_TPS_PREFLIGHT_CLEARED;
    case STORE_OK:
        /* Refined by the caller from the record's own scalars. */
        return NX_TPS_PREFLIGHT_RECORD_PRESENT;

    /* Every "the committed state cannot be trusted as-is" result. None of
     * these may ever become EMPTY: an unreadable store is not an empty one. */
    case STORE_CORRUPT:
    case STORE_INVALID_RECORD:
    case STORE_ACTIVE_POINTER_INVALID:
    case STORE_ACTIVE_SLOT_INVALID:
    case STORE_RECOVERY_REQUIRED:
    case STORE_READBACK_MISMATCH:
        return NX_TPS_PREFLIGHT_CORRUPT;

    case STORE_UNSUPPORTED_SCHEMA:
        return NX_TPS_PREFLIGHT_UNSUPPORTED_SCHEMA;
    case STORE_COMMIT_UNCERTAIN:
        return NX_TPS_PREFLIGHT_COMMIT_UNCERTAIN;

    case STORE_IO_ERROR:
    case STORE_NOT_INITIALIZED:
        return NX_TPS_PREFLIGHT_IO_ERROR;

    /* Results that cannot arise from a pure load: reaching one means the
     * preflight itself is wrong, so it fails closed rather than guessing. */
    case STORE_INVALID_ARGUMENT:
    case STORE_GENERATION_EXHAUSTED:
    case STORE_STATE_CONFLICT:
    case POOL_STORE_RESULT__COUNT:
    default:
        return NX_TPS_PREFLIGHT_INTERNAL_ERROR;
    }
}

void nx_tps_preflight_classify(const NxTpsPreflightInput *in,
                               NxTpsPreflightResult *out)
{
    NxTpsPreflightOutcome outcome;

    if (out == NULL) {
        return;
    }
    nx_tps_preflight_result_init(out);
    if (in == NULL || in->model_version != NX_TPS_PREFLIGHT_MODEL_VERSION) {
        return; /* INTERNAL_ERROR */
    }

    out->namespace_present = in->namespace_present;
    out->nvs_init_failed   = in->nvs_init_failed;
    out->write_attempts    = in->write_attempts;
    out->erase_attempts    = in->erase_attempts;
    out->commit_attempts   = in->commit_attempts;

    /*
     * A preflight that attempted ANY mutation is a broken preflight. Its
     * reading of the store is not trustworthy either, so the verdict is
     * INTERNAL_ERROR — never a permitting outcome — regardless of what the
     * store said. Checked FIRST: "the inspector is broken" is a more severe
     * finding than "the evidence was destroyed".
     */
    if (in->write_attempts != 0u || in->erase_attempts != 0u ||
        in->commit_attempts != 0u) {
        return; /* INTERNAL_ERROR */
    }

    /*
     * The NVS subsystem could not be initialized. The preflight boot gate never
     * erases to recover, so nothing was destroyed trying — but nothing can be
     * read either, and a store that cannot be read is never EMPTY.
     */
    if (in->nvs_init_failed) {
        out->outcome       = NX_TPS_PREFLIGHT_NVS_INIT_FAILED;
        out->permits_pilot = false;
        return;
    }

    /*
     * The namespace could not be READ. This is checked BEFORE absence on
     * purpose: "unreadable" and "absent" look identical through a failed open,
     * and reporting the first as the second would classify EMPTY and PERMIT a
     * pilot on a store nobody actually looked at. An unreadable store is not an
     * empty one.
     */
    if (in->namespace_open_failed) {
        out->outcome       = NX_TPS_PREFLIGHT_IO_ERROR;
        out->permits_pilot = false;
        return;
    }

    /*
     * Namespace absent. With NVS_READONLY this is a pure query that creates
     * nothing, and the committed contract is explicit: no namespace means no
     * committed state has ever existed here.
     */
    if (!in->namespace_present) {
        out->outcome       = NX_TPS_PREFLIGHT_EMPTY;
        out->permits_pilot = true;
        return;
    }

    if (!in->store_loaded) {
        return; /* the loader never ran: INTERNAL_ERROR */
    }
    if ((uint32_t)in->store_result >= (uint32_t)POOL_STORE_RESULT__COUNT) {
        return; /* out-of-range store result: INTERNAL_ERROR */
    }

    outcome = outcome_for_store_result((PoolStoreResult)in->store_result);

    /*
     * Refine a loaded record. The committed loader already returns
     * STORE_CLEARED for a tombstone, so reaching RECORD_PRESENT means a
     * SESSION record — but each condition is re-checked here so a future
     * loader change cannot quietly widen what counts as safe.
     */
    if (outcome == NX_TPS_PREFLIGHT_RECORD_PRESENT) {
        if (!in->record_valid) {
            outcome = NX_TPS_PREFLIGHT_CORRUPT;
        } else if (in->record_kind == (uint8_t)POOL_RECORD_KIND_TOMBSTONE) {
            outcome = NX_TPS_PREFLIGHT_CLEARED;
        } else if (in->record_kind != (uint8_t)POOL_RECORD_KIND_SESSION) {
            outcome = NX_TPS_PREFLIGHT_CORRUPT;
        } else if ((uint32_t)in->session_state >= (uint32_t)POOL_STATE__COUNT) {
            outcome = NX_TPS_PREFLIGHT_CORRUPT;
        } else if (pool_state_is_terminal((PoolSessionState)in->session_state)) {
            /* A terminal result rests awaiting operator acknowledgement. It
             * is still an owner, so it still blocks. */
            outcome = NX_TPS_PREFLIGHT_TERMINAL_PENDING;
        }
        /* else: a live, non-terminal session record — RECORD_PRESENT. */
    }

    /*
     * An unresolved restore obligation always blocks, whatever else the
     * record says: the device owes the source pool a restoration.
     */
    if (in->restore_required && nx_tps_preflight_permits_pilot(outcome)) {
        outcome = NX_TPS_PREFLIGHT_RECORD_PRESENT;
    }

    out->outcome       = outcome;
    out->permits_pilot = nx_tps_preflight_permits_pilot(outcome);
}

/* ------------------------------------------------------------------ */
/* Bounded reporting                                                   */
/* ------------------------------------------------------------------ */

uint32_t nx_tps_preflight_format(const NxTpsPreflightResult *r, uint32_t uptime_s,
                                 char *buf, uint32_t cap)
{
    int n;

    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    buf[0] = '\0';
    if (r == NULL) {
        return 0u;
    }

    n = snprintf(buf, (size_t)cap,
                 "TPS_PREFLIGHT_COMPLETE outcome=%s pilot=%u ns=%u nvsinit=%u "
                 "reads=%u w=%u e=%u c=%u up=%u",
                 nx_tps_preflight_outcome_token(r->outcome),
                 (unsigned)(r->permits_pilot ? 1u : 0u),
                 (unsigned)(r->namespace_present ? 1u : 0u),
                 (unsigned)(r->nvs_init_failed ? 0u : 1u),
                 (unsigned)r->read_attempts,
                 (unsigned)r->write_attempts,
                 (unsigned)r->erase_attempts,
                 (unsigned)r->commit_attempts,
                 (unsigned)uptime_s);
    if (n < 0 || (uint32_t)n >= cap) {
        buf[0] = '\0';
        return 0u;
    }
    return (uint32_t)n;
}
