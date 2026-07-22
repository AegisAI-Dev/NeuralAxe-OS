/*
 * NeuralAxe timed pool sessions — crash-safe dual-slot store (Gate B3).
 *
 * All persistence flows through the injected backend ops. The algorithms
 * here never guess: a corrupt or missing active pointer, an ambiguous slot
 * layout or a generation mismatch is a recovery result, never a
 * highest-generation heuristic. The active-pointer write is the logical
 * commit point; nothing is ever erased during a commit.
 *
 * No task, no queue, no logging, no heap, no networking, no pool mutation.
 */

#include <string.h>
#include "pool_session_store.h"

/* Bounded static scratch. A PoolSessionRecord is ~1 KB — too large to place
 * repeatedly on an 8 KB task stack (Gate B1 §5.1 precedent). The store's
 * public contract is synchronous and single-threaded (one future scheduler
 * owner, Gate B5), so two static-duration scratch records are safe and are
 * NOT runtime state: their contents are meaningless between calls. */
static PoolSessionRecord s_decode_scratch;    /* committed-state decode target */
static PoolSessionRecord s_tombstone_scratch; /* tombstone build target        */

static const char *slot_key(uint8_t slot)
{
    return (slot == POOL_RECORD_SLOT_A) ? POOL_STORE_KEY_SLOT_A : POOL_STORE_KEY_SLOT_B;
}

static uint8_t other_slot(uint8_t slot)
{
    return (slot == POOL_RECORD_SLOT_A) ? POOL_RECORD_SLOT_B : POOL_RECORD_SLOT_A;
}

/* ------------------------------------------------------------------ */
/* Result classification                                               */
/* ------------------------------------------------------------------ */

bool pool_store_result_requires_recovery(PoolStoreResult r)
{
    switch (r) {
    case STORE_CORRUPT:
    case STORE_UNSUPPORTED_SCHEMA:
    case STORE_INVALID_RECORD:
    case STORE_ACTIVE_POINTER_INVALID:
    case STORE_ACTIVE_SLOT_INVALID:
    case STORE_RECOVERY_REQUIRED:
    case STORE_GENERATION_EXHAUSTED:
        return true;
    default:
        return false;
    }
}

bool pool_store_result_permits_session_load(PoolStoreResult r)
{
    /* Everything except STORE_OK yields no active session and never a
     * pool-mutation recommendation from this layer. */
    return r == STORE_OK;
}

/* ------------------------------------------------------------------ */
/* Backend access helpers                                              */
/* ------------------------------------------------------------------ */

/* Pointer read: OK with *found=false when absent; decode failures are
 * STORE_ACTIVE_POINTER_INVALID (never guessed around). */
static PoolStoreResult read_pointer(PoolSessionStore *st, PoolRecordPointer *ptr,
                                    bool *found)
{
    uint8_t pbuf[POOL_RECORD_POINTER_LEN * 2u];
    size_t len = 0;
    int rc;

    *found = false;
    rc = st->ops->read_blob(st->ctx, POOL_STORE_KEY_ACTIVE, pbuf, sizeof(pbuf), &len);
    if (rc == POOL_STORE_BACKEND_NOT_FOUND) {
        return STORE_OK;
    }
    if (rc != POOL_STORE_BACKEND_OK) {
        return STORE_IO_ERROR;
    }
    if (len > sizeof(pbuf)) {
        return STORE_ACTIVE_POINTER_INVALID; /* oversized stored pointer */
    }
    if (pool_record_pointer_decode(pbuf, len, ptr) != RECORD_OK) {
        return STORE_ACTIVE_POINTER_INVALID;
    }
    *found = true;
    return STORE_OK;
}

/* Slot presence probe (no decode). 0 present, 1 absent, -1 io. */
static int probe_slot(PoolSessionStore *st, uint8_t slot)
{
    size_t len = 0;
    int rc = st->ops->read_blob(st->ctx, slot_key(slot), st->work,
                                sizeof(st->work), &len);
    if (rc == POOL_STORE_BACKEND_NOT_FOUND) {
        return 1;
    }
    if (rc != POOL_STORE_BACKEND_OK) {
        return -1;
    }
    return 0;
}

/* Map codec errors from a slot decode onto store results. */
static PoolStoreResult map_slot_decode_error(PoolRecordCodecError err)
{
    switch (err) {
    case RECORD_ERR_UNSUPPORTED_SCHEMA:
        return STORE_UNSUPPORTED_SCHEMA;
    case RECORD_ERR_TRUNCATED:
    case RECORD_ERR_TRAILING_BYTES:
    case RECORD_ERR_BAD_MAGIC:
    case RECORD_ERR_BAD_LENGTH:
    case RECORD_ERR_UNKNOWN_FLAGS:
    case RECORD_ERR_CRC_MISMATCH:
    case RECORD_ERR_BAD_GENERATION:
        return STORE_CORRUPT;
    default:
        return STORE_INVALID_RECORD; /* decoded bytes, invalid meaning */
    }
}

/* Read + decode one slot into *out. OK with *found=false when absent. */
static PoolStoreResult read_slot_record(PoolSessionStore *st, uint8_t slot,
                                        PoolSessionRecord *out, bool *found)
{
    size_t len = 0;
    int rc;
    PoolRecordCodecError err;

    *found = false;
    rc = st->ops->read_blob(st->ctx, slot_key(slot), st->work, sizeof(st->work), &len);
    if (rc == POOL_STORE_BACKEND_NOT_FOUND) {
        return STORE_OK;
    }
    if (rc != POOL_STORE_BACKEND_OK) {
        return STORE_IO_ERROR;
    }
    *found = true;
    if (len > sizeof(st->work)) {
        return STORE_CORRUPT; /* stored blob exceeds any valid v1 record */
    }
    err = pool_session_record_decode(st->work, len, out);
    if (err != RECORD_OK) {
        return map_slot_decode_error(err);
    }
    return STORE_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

PoolStoreResult pool_session_store_init(PoolSessionStore *st,
                                        const PoolStoreBackendOps *ops, void *ctx)
{
    if (st == NULL) {
        return STORE_INVALID_ARGUMENT;
    }
    if (ops == NULL || ops->open == NULL || ops->read_blob == NULL ||
        ops->write_blob == NULL || ops->commit == NULL || ops->close == NULL) {
        return STORE_INVALID_ARGUMENT;
    }
    memset(st, 0, sizeof(*st));
    st->ops = ops;
    st->ctx = ctx;
    if (ops->open(ctx) != POOL_STORE_BACKEND_OK) {
        st->ops = NULL;
        return STORE_IO_ERROR;
    }
    st->initialized = true;
    return STORE_OK;
}

PoolStoreResult pool_session_store_deinit(PoolSessionStore *st)
{
    if (st == NULL) {
        return STORE_INVALID_ARGUMENT;
    }
    if (st->initialized && st->ops != NULL && st->ops->close != NULL) {
        (void)st->ops->close(st->ctx);
    }
    memset(st, 0, sizeof(*st));
    return STORE_OK;
}

bool pool_session_store_committed_generation(const PoolSessionStore *st,
                                             uint32_t *out_generation)
{
    if (st == NULL || !st->initialized || !st->have_committed) {
        return false;
    }
    if (out_generation != NULL) {
        *out_generation = st->committed_generation;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Load (Stage-10 case table; never guess)                             */
/* ------------------------------------------------------------------ */

PoolStoreResult pool_session_store_load(PoolSessionStore *st,
                                        PoolSessionRecord *out,
                                        PoolStoreLoadInfo *info)
{
    PoolRecordPointer ptr;
    bool ptr_found = false;
    bool slot_found = false;
    PoolStoreResult res;
    PoolSessionRecord *rec;

    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
    if (out != NULL) {
        pool_session_record_init(out);
    }
    if (st == NULL) {
        return STORE_INVALID_ARGUMENT;
    }
    if (!st->initialized) {
        return STORE_NOT_INITIALIZED;
    }
    st->have_committed = false; /* recomputed from flash truth below */

    res = read_pointer(st, &ptr, &ptr_found);
    if (res != STORE_OK) {
        return res;
    }

    if (!ptr_found) {
        /* Case A/H: no pointer. Slots may only be uncommitted staged data —
         * never guess a record out of them. */
        int a = probe_slot(st, POOL_RECORD_SLOT_A);
        int b = probe_slot(st, POOL_RECORD_SLOT_B);
        if (a < 0 || b < 0) {
            return STORE_IO_ERROR;
        }
        if (a == 1 && b == 1) {
            return STORE_EMPTY;
        }
        return STORE_RECOVERY_REQUIRED;
    }

    rec = (out != NULL) ? out : &s_decode_scratch;
    res = read_slot_record(st, ptr.slot, rec, &slot_found);
    if (res != STORE_OK) {
        return res; /* IO / CORRUPT / UNSUPPORTED_SCHEMA / INVALID_RECORD */
    }
    if (!slot_found) {
        return STORE_ACTIVE_SLOT_INVALID; /* pointer names a missing slot */
    }
    if (rec->generation != ptr.generation) {
        return STORE_RECOVERY_REQUIRED; /* pointer/slot generation mismatch */
    }

    st->have_committed = true;
    st->committed_slot = ptr.slot;
    st->committed_generation = ptr.generation;
    {
        /* The diagnostics decode below may reuse the shared scratch record
         * when the caller passed out == NULL — capture the committed kind
         * first so the result classification cannot be clobbered. */
        uint8_t committed_kind = rec->kind;

        if (info != NULL) {
            info->active_slot = ptr.slot;
            info->committed_generation = ptr.generation;

            /* Best-effort diagnostics on the other slot; its errors are not
             * load errors and it is NEVER promoted. */
            {
                bool ofound = false;
                PoolStoreResult ores =
                    read_slot_record(st, other_slot(ptr.slot), &s_decode_scratch, &ofound);
                if (ores == STORE_OK && ofound) {
                    if (s_decode_scratch.generation == ptr.generation) {
                        info->duplicate_generation = true;
                    } else if (s_decode_scratch.generation > ptr.generation) {
                        info->staged_newer_ignored = true;
                    }
                }
            }
        }

        if (committed_kind == (uint8_t)POOL_RECORD_KIND_TOMBSTONE) {
            return STORE_CLEARED;
        }
    }
    return STORE_OK;
}

/* ------------------------------------------------------------------ */
/* Commit (Stage-9 sequence; pointer write last)                       */
/* ------------------------------------------------------------------ */

PoolStoreResult pool_session_store_commit_record(PoolSessionStore *st,
                                                 PoolSessionRecord *rec)
{
    PoolRecordPointer ptr;
    bool ptr_found = false;
    uint8_t next_slot;
    uint32_t cur_gen;
    size_t enc_len = 0;
    PoolStoreResult res;
    PoolRecordCodecError cerr;

    if (st == NULL || rec == NULL) {
        return STORE_INVALID_ARGUMENT;
    }
    if (!st->initialized) {
        return STORE_NOT_INITIALIZED;
    }

    /* 1-2. Load and validate the current committed base. A store in any
     * recovery condition refuses to commit over the evidence. */
    res = read_pointer(st, &ptr, &ptr_found);
    if (res != STORE_OK) {
        return res;
    }
    if (!ptr_found) {
        int a = probe_slot(st, POOL_RECORD_SLOT_A);
        int b = probe_slot(st, POOL_RECORD_SLOT_B);
        if (a < 0 || b < 0) {
            return STORE_IO_ERROR;
        }
        if (a == 0 || b == 0) {
            return STORE_RECOVERY_REQUIRED; /* staged slots without a pointer */
        }
        next_slot = POOL_RECORD_SLOT_A; /* pristine store: first commit */
        cur_gen = 0u;
    } else {
        bool slot_found = false;
        res = read_slot_record(st, ptr.slot, &s_decode_scratch, &slot_found);
        if (res != STORE_OK) {
            return res;
        }
        if (!slot_found) {
            return STORE_ACTIVE_SLOT_INVALID;
        }
        if (s_decode_scratch.generation != ptr.generation) {
            return STORE_RECOVERY_REQUIRED;
        }
        next_slot = other_slot(ptr.slot); /* 3. write the INACTIVE slot */
        cur_gen = ptr.generation;
    }

    /* 4. Next generation, overflow-checked (never wraps). */
    if (cur_gen == UINT32_MAX) {
        return STORE_GENERATION_EXHAUSTED;
    }
    rec->generation = cur_gen + 1u;

    /* 5-6. Validate and encode (encode validates semantically first). */
    cerr = pool_session_record_encode(rec, st->work, sizeof(st->work), &enc_len);
    if (cerr != RECORD_OK) {
        return STORE_INVALID_RECORD;
    }

    /* 7-8. Stage the inactive slot and commit it. */
    if (st->ops->write_blob(st->ctx, slot_key(next_slot), st->work, enc_len) !=
        POOL_STORE_BACKEND_OK) {
        return STORE_IO_ERROR; /* old committed record remains authoritative */
    }
    if (st->ops->commit(st->ctx) != POOL_STORE_BACKEND_OK) {
        return STORE_IO_ERROR;
    }

    /* 9-10. Read back and verify byte-exactly AND semantically. */
    {
        size_t vlen = 0;
        int rc = st->ops->read_blob(st->ctx, slot_key(next_slot), st->verify,
                                    sizeof(st->verify), &vlen);
        if (rc != POOL_STORE_BACKEND_OK || vlen != enc_len ||
            memcmp(st->verify, st->work, enc_len) != 0) {
            return STORE_READBACK_MISMATCH; /* pointer untouched; old record intact */
        }
        if (pool_session_record_decode(st->verify, vlen, &s_decode_scratch) != RECORD_OK ||
            s_decode_scratch.generation != rec->generation ||
            s_decode_scratch.kind != rec->kind) {
            return STORE_READBACK_MISMATCH;
        }
    }

    /* 11-15. Pointer phase — THE logical commit point. Any failure from
     * here on leaves the on-flash truth possibly either version: report
     * COMMIT_UNCERTAIN and force a reload to learn it. */
    {
        uint8_t pbuf[POOL_RECORD_POINTER_LEN];
        uint8_t rbuf[POOL_RECORD_POINTER_LEN * 2u];
        size_t plen = 0, rlen = 0;
        PoolRecordPointer nptr, chk;

        nptr.slot = next_slot;
        nptr.generation = rec->generation;
        if (pool_record_pointer_encode(&nptr, pbuf, sizeof(pbuf), &plen) != RECORD_OK) {
            return STORE_INVALID_ARGUMENT; /* unreachable for valid inputs */
        }
        st->have_committed = false;
        if (st->ops->write_blob(st->ctx, POOL_STORE_KEY_ACTIVE, pbuf, plen) !=
            POOL_STORE_BACKEND_OK) {
            return STORE_COMMIT_UNCERTAIN;
        }
        if (st->ops->commit(st->ctx) != POOL_STORE_BACKEND_OK) {
            return STORE_COMMIT_UNCERTAIN;
        }
        if (st->ops->read_blob(st->ctx, POOL_STORE_KEY_ACTIVE, rbuf, sizeof(rbuf), &rlen) !=
                POOL_STORE_BACKEND_OK ||
            rlen > sizeof(rbuf) ||
            pool_record_pointer_decode(rbuf, rlen, &chk) != RECORD_OK ||
            chk.slot != nptr.slot || chk.generation != nptr.generation) {
            return STORE_COMMIT_UNCERTAIN;
        }
    }

    /* The old slot is intentionally NOT erased: crash-recovery evidence. */
    st->have_committed = true;
    st->committed_slot = next_slot;
    st->committed_generation = rec->generation;
    return STORE_OK;
}

/* ------------------------------------------------------------------ */
/* Crash-safe acknowledgement/clear (committed tombstone)              */
/* ------------------------------------------------------------------ */

PoolStoreResult pool_session_store_commit_clear(PoolSessionStore *st)
{
    PoolStoreResult res;

    if (st == NULL) {
        return STORE_INVALID_ARGUMENT;
    }
    if (!st->initialized) {
        return STORE_NOT_INITIALIZED;
    }

    res = pool_session_store_load(st, &s_decode_scratch, NULL);
    if (res == STORE_EMPTY || res == STORE_CLEARED) {
        return STORE_STATE_CONFLICT; /* nothing to acknowledge */
    }
    if (res != STORE_OK) {
        return res; /* corrupt/unsupported/recovery states cannot be cleared */
    }

    /* Only an acknowledgeable terminal session may be cleared: COMPLETE, or
     * a pre-mutation CANCELLED / RECOVERY_REQUIRED — and NEVER while the
     * restore obligation is unresolved. (RESTORE_FAILED always retains the
     * obligation, so it is structurally excluded.) */
    if (s_decode_scratch.restore_required) {
        return STORE_STATE_CONFLICT;
    }
    if (s_decode_scratch.state != POOL_STATE_COMPLETE &&
        s_decode_scratch.state != POOL_STATE_CANCELLED &&
        s_decode_scratch.state != POOL_STATE_RECOVERY_REQUIRED) {
        return STORE_STATE_CONFLICT;
    }

    /* Commit the tombstone through the exact same crash-safe path. The old
     * record is not erased before the tombstone pointer commit succeeds —
     * a crash anywhere before that leaves the old session committed. */
    pool_session_record_init_tombstone(&s_tombstone_scratch);
    return pool_session_store_commit_record(st, &s_tombstone_scratch);
}

/* ------------------------------------------------------------------ */
/* Stable machine tokens                                               */
/* ------------------------------------------------------------------ */

const char *pool_store_result_str(PoolStoreResult r)
{
    switch (r) {
    case STORE_OK:                     return "STORE_OK";
    case STORE_EMPTY:                  return "STORE_EMPTY";
    case STORE_CLEARED:                return "STORE_CLEARED";
    case STORE_NOT_INITIALIZED:        return "STORE_NOT_INITIALIZED";
    case STORE_INVALID_ARGUMENT:       return "STORE_INVALID_ARGUMENT";
    case STORE_IO_ERROR:               return "STORE_IO_ERROR";
    case STORE_CORRUPT:                return "STORE_CORRUPT";
    case STORE_UNSUPPORTED_SCHEMA:     return "STORE_UNSUPPORTED_SCHEMA";
    case STORE_INVALID_RECORD:         return "STORE_INVALID_RECORD";
    case STORE_ACTIVE_POINTER_INVALID: return "STORE_ACTIVE_POINTER_INVALID";
    case STORE_ACTIVE_SLOT_INVALID:    return "STORE_ACTIVE_SLOT_INVALID";
    case STORE_RECOVERY_REQUIRED:      return "STORE_RECOVERY_REQUIRED";
    case STORE_GENERATION_EXHAUSTED:   return "STORE_GENERATION_EXHAUSTED";
    case STORE_READBACK_MISMATCH:      return "STORE_READBACK_MISMATCH";
    case STORE_COMMIT_UNCERTAIN:       return "STORE_COMMIT_UNCERTAIN";
    case STORE_STATE_CONFLICT:         return "STORE_STATE_CONFLICT";
    default:                           return "STORE_RESULT_UNKNOWN";
    }
}
