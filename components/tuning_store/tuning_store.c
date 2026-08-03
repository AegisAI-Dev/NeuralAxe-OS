/*
 * NeuralAxe Weather-Aware Tuning — crash-safe dual-slot store (Gate W2).
 *
 * Mirrors the proven Gate B3 algorithm: all persistence flows through the
 * injected backend ops; a corrupt or missing active pointer, an ambiguous
 * slot layout or a generation mismatch is a recovery result, never a
 * highest-generation heuristic. The active-pointer write is the logical
 * commit point; nothing is ever erased during a commit.
 *
 * No task, no queue, no logging, no heap, no networking, no tuning
 * mutation.
 */

#include <string.h>
#include "tuning_store.h"

/* Bounded static scratch (single-owner synchronous contract, Gate W4 owns
 * wiring under the B5 lease); contents are meaningless between calls. */
static TuningPolicyRecord s_decode_scratch;
static TuningPolicyRecord s_tombstone_scratch;

static const char *slot_key(uint8_t slot)
{
    return (slot == TUNING_RECORD_SLOT_A) ? TUNING_STORE_KEY_SLOT_A
                                          : TUNING_STORE_KEY_SLOT_B;
}

static uint8_t other_slot(uint8_t slot)
{
    return (slot == TUNING_RECORD_SLOT_A) ? TUNING_RECORD_SLOT_B
                                          : TUNING_RECORD_SLOT_A;
}

/* ------------------------------------------------------------------ */
/* Result classification                                               */
/* ------------------------------------------------------------------ */

bool tuning_store_result_requires_recovery(TuningStoreResult r)
{
    switch (r) {
    case TUNING_STORE_CORRUPT:
    case TUNING_STORE_UNSUPPORTED_SCHEMA:
    case TUNING_STORE_INVALID_RECORD:
    case TUNING_STORE_ACTIVE_POINTER_INVALID:
    case TUNING_STORE_ACTIVE_SLOT_INVALID:
    case TUNING_STORE_RECOVERY_REQUIRED:
    case TUNING_STORE_GENERATION_EXHAUSTED:
        return true;
    default:
        return false;
    }
}

bool tuning_store_result_permits_state_load(TuningStoreResult r)
{
    return r == TUNING_STORE_OK;
}

/* ------------------------------------------------------------------ */
/* Backend access helpers                                              */
/* ------------------------------------------------------------------ */

static TuningStoreResult read_pointer(TuningStore *st, TuningRecordPointer *ptr,
                                      bool *found)
{
    uint8_t pbuf[TUNING_RECORD_POINTER_LEN * 2u];
    size_t len = 0;
    int rc;

    *found = false;
    rc = st->ops->read_blob(st->ctx, TUNING_STORE_KEY_ACTIVE, pbuf, sizeof(pbuf), &len);
    if (rc == TUNING_STORE_BACKEND_NOT_FOUND) {
        return TUNING_STORE_OK;
    }
    if (rc != TUNING_STORE_BACKEND_OK) {
        return TUNING_STORE_IO_ERROR;
    }
    if (len > sizeof(pbuf)) {
        return TUNING_STORE_ACTIVE_POINTER_INVALID; /* oversized pointer */
    }
    if (tuning_record_pointer_decode(pbuf, len, ptr) != TUNING_RECORD_OK) {
        return TUNING_STORE_ACTIVE_POINTER_INVALID;
    }
    *found = true;
    return TUNING_STORE_OK;
}

/* Slot presence probe (no decode). 0 present, 1 absent, -1 io. */
static int probe_slot(TuningStore *st, uint8_t slot)
{
    size_t len = 0;
    int rc = st->ops->read_blob(st->ctx, slot_key(slot), st->work,
                                sizeof(st->work), &len);
    if (rc == TUNING_STORE_BACKEND_NOT_FOUND) {
        return 1;
    }
    if (rc != TUNING_STORE_BACKEND_OK) {
        return -1;
    }
    return 0;
}

static TuningStoreResult map_slot_decode_error(TuningRecordError err)
{
    switch (err) {
    case TUNING_RECORD_ERR_UNSUPPORTED_SCHEMA:
    case TUNING_RECORD_ERR_UNSUPPORTED_MODEL:
        return TUNING_STORE_UNSUPPORTED_SCHEMA;
    case TUNING_RECORD_ERR_TRUNCATED:
    case TUNING_RECORD_ERR_TRAILING_BYTES:
    case TUNING_RECORD_ERR_BAD_MAGIC:
    case TUNING_RECORD_ERR_BAD_LENGTH:
    case TUNING_RECORD_ERR_UNKNOWN_FLAGS:
    case TUNING_RECORD_ERR_CRC_MISMATCH:
    case TUNING_RECORD_ERR_BAD_GENERATION:
        return TUNING_STORE_CORRUPT;
    default:
        return TUNING_STORE_INVALID_RECORD; /* decoded bytes, invalid meaning */
    }
}

static TuningStoreResult read_slot_record(TuningStore *st, uint8_t slot,
                                          TuningPolicyRecord *out, bool *found)
{
    size_t len = 0;
    int rc;
    TuningRecordError err;

    *found = false;
    rc = st->ops->read_blob(st->ctx, slot_key(slot), st->work, sizeof(st->work), &len);
    if (rc == TUNING_STORE_BACKEND_NOT_FOUND) {
        return TUNING_STORE_OK;
    }
    if (rc != TUNING_STORE_BACKEND_OK) {
        return TUNING_STORE_IO_ERROR;
    }
    *found = true;
    if (len > sizeof(st->work)) {
        return TUNING_STORE_CORRUPT; /* stored blob exceeds any valid v1 record */
    }
    err = tuning_record_decode(st->work, len, out);
    if (err != TUNING_RECORD_OK) {
        return map_slot_decode_error(err);
    }
    return TUNING_STORE_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

TuningStoreResult tuning_store_init(TuningStore *st,
                                    const TuningStoreBackendOps *ops, void *ctx)
{
    if (st == NULL) {
        return TUNING_STORE_INVALID_ARGUMENT;
    }
    if (ops == NULL || ops->open == NULL || ops->read_blob == NULL ||
        ops->write_blob == NULL || ops->commit == NULL || ops->close == NULL) {
        return TUNING_STORE_INVALID_ARGUMENT;
    }
    memset(st, 0, sizeof(*st));
    st->ops = ops;
    st->ctx = ctx;
    if (ops->open(ctx) != TUNING_STORE_BACKEND_OK) {
        st->ops = NULL;
        return TUNING_STORE_IO_ERROR;
    }
    st->initialized = true;
    return TUNING_STORE_OK;
}

TuningStoreResult tuning_store_deinit(TuningStore *st)
{
    if (st == NULL) {
        return TUNING_STORE_INVALID_ARGUMENT;
    }
    if (st->initialized && st->ops != NULL && st->ops->close != NULL) {
        (void)st->ops->close(st->ctx);
    }
    memset(st, 0, sizeof(*st));
    return TUNING_STORE_OK;
}

bool tuning_store_committed_generation(const TuningStore *st,
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
/* Load (never guess)                                                  */
/* ------------------------------------------------------------------ */

TuningStoreResult tuning_store_load(TuningStore *st,
                                    TuningPolicyRecord *out,
                                    TuningStoreLoadInfo *info)
{
    TuningRecordPointer ptr;
    bool ptr_found = false;
    bool slot_found = false;
    TuningStoreResult res;
    TuningPolicyRecord *rec;

    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (st == NULL) {
        return TUNING_STORE_INVALID_ARGUMENT;
    }
    if (!st->initialized) {
        return TUNING_STORE_NOT_INITIALIZED;
    }
    st->have_committed = false; /* recomputed from flash truth below */

    res = read_pointer(st, &ptr, &ptr_found);
    if (res != TUNING_STORE_OK) {
        return res;
    }

    if (!ptr_found) {
        /* No pointer: slots may only be uncommitted staged data — never
         * guess a record out of them. */
        int a = probe_slot(st, TUNING_RECORD_SLOT_A);
        int b = probe_slot(st, TUNING_RECORD_SLOT_B);
        if (a < 0 || b < 0) {
            return TUNING_STORE_IO_ERROR;
        }
        if (a == 1 && b == 1) {
            return TUNING_STORE_EMPTY;
        }
        return TUNING_STORE_RECOVERY_REQUIRED;
    }

    rec = (out != NULL) ? out : &s_decode_scratch;
    res = read_slot_record(st, ptr.slot, rec, &slot_found);
    if (res != TUNING_STORE_OK) {
        return res;
    }
    if (!slot_found) {
        return TUNING_STORE_ACTIVE_SLOT_INVALID; /* pointer names missing slot */
    }
    if (rec->generation != ptr.generation) {
        return TUNING_STORE_RECOVERY_REQUIRED; /* pointer/slot mismatch */
    }

    st->have_committed = true;
    st->committed_slot = ptr.slot;
    st->committed_generation = ptr.generation;
    {
        /* Capture the committed kind before diagnostics may reuse the
         * shared scratch record (out == NULL case). */
        uint8_t committed_kind = rec->kind;

        if (info != NULL) {
            info->active_slot = ptr.slot;
            info->committed_generation = ptr.generation;
            {
                bool ofound = false;
                TuningStoreResult ores = read_slot_record(
                    st, other_slot(ptr.slot), &s_decode_scratch, &ofound);
                if (ores == TUNING_STORE_OK && ofound) {
                    if (s_decode_scratch.generation == ptr.generation) {
                        info->duplicate_generation = true;
                    } else if (s_decode_scratch.generation > ptr.generation) {
                        info->staged_newer_ignored = true;
                    }
                }
            }
        }

        if (committed_kind == (uint8_t)TUNING_RECORD_KIND_TOMBSTONE) {
            return TUNING_STORE_CLEARED;
        }
    }
    return TUNING_STORE_OK;
}

/* ------------------------------------------------------------------ */
/* Commit (pointer write last)                                         */
/* ------------------------------------------------------------------ */

TuningStoreResult tuning_store_commit_record(TuningStore *st,
                                             TuningPolicyRecord *rec)
{
    TuningRecordPointer ptr;
    bool ptr_found = false;
    uint8_t next_slot;
    uint32_t cur_gen;
    size_t enc_len = 0;
    TuningStoreResult res;
    TuningRecordError cerr;

    if (st == NULL || rec == NULL) {
        return TUNING_STORE_INVALID_ARGUMENT;
    }
    if (!st->initialized) {
        return TUNING_STORE_NOT_INITIALIZED;
    }

    /* Load and validate the current committed base. A store in any
     * recovery condition refuses to commit over the evidence. */
    res = read_pointer(st, &ptr, &ptr_found);
    if (res != TUNING_STORE_OK) {
        return res;
    }
    if (!ptr_found) {
        int a = probe_slot(st, TUNING_RECORD_SLOT_A);
        int b = probe_slot(st, TUNING_RECORD_SLOT_B);
        if (a < 0 || b < 0) {
            return TUNING_STORE_IO_ERROR;
        }
        if (a == 0 || b == 0) {
            return TUNING_STORE_RECOVERY_REQUIRED; /* staged w/o pointer */
        }
        next_slot = TUNING_RECORD_SLOT_A; /* pristine store: first commit */
        cur_gen = 0u;
    } else {
        bool slot_found = false;
        res = read_slot_record(st, ptr.slot, &s_decode_scratch, &slot_found);
        if (res != TUNING_STORE_OK) {
            return res;
        }
        if (!slot_found) {
            return TUNING_STORE_ACTIVE_SLOT_INVALID;
        }
        if (s_decode_scratch.generation != ptr.generation) {
            return TUNING_STORE_RECOVERY_REQUIRED;
        }
        next_slot = other_slot(ptr.slot); /* write the INACTIVE slot */
        cur_gen = ptr.generation;
    }

    /* Next generation, overflow-checked (never wraps). */
    if (cur_gen == UINT32_MAX) {
        return TUNING_STORE_GENERATION_EXHAUSTED;
    }
    rec->generation = cur_gen + 1u;

    /* Validate + encode (encode validates semantically first). */
    cerr = tuning_record_encode(rec, st->work, sizeof(st->work), &enc_len);
    if (cerr != TUNING_RECORD_OK) {
        return TUNING_STORE_INVALID_RECORD;
    }

    /* Stage the inactive slot and commit it. */
    if (st->ops->write_blob(st->ctx, slot_key(next_slot), st->work, enc_len) !=
        TUNING_STORE_BACKEND_OK) {
        return TUNING_STORE_IO_ERROR; /* old committed record authoritative */
    }
    if (st->ops->commit(st->ctx) != TUNING_STORE_BACKEND_OK) {
        return TUNING_STORE_IO_ERROR;
    }

    /* Read back and verify byte-exactly AND semantically. */
    {
        size_t vlen = 0;
        int rc = st->ops->read_blob(st->ctx, slot_key(next_slot), st->verify,
                                    sizeof(st->verify), &vlen);
        if (rc != TUNING_STORE_BACKEND_OK || vlen != enc_len ||
            memcmp(st->verify, st->work, enc_len) != 0) {
            return TUNING_STORE_READBACK_MISMATCH; /* pointer untouched */
        }
        if (tuning_record_decode(st->verify, vlen, &s_decode_scratch) !=
                TUNING_RECORD_OK ||
            s_decode_scratch.generation != rec->generation ||
            s_decode_scratch.kind != rec->kind) {
            return TUNING_STORE_READBACK_MISMATCH;
        }
    }

    /* Pointer phase — THE logical commit point. Any failure from here on
     * leaves the on-flash truth possibly either version: report
     * COMMIT_UNCERTAIN and force a reload to learn it. */
    {
        uint8_t pbuf[TUNING_RECORD_POINTER_LEN];
        uint8_t rbuf[TUNING_RECORD_POINTER_LEN * 2u];
        size_t plen = 0, rlen = 0;
        TuningRecordPointer nptr, chk;

        nptr.slot = next_slot;
        nptr.generation = rec->generation;
        if (tuning_record_pointer_encode(&nptr, pbuf, sizeof(pbuf), &plen) !=
            TUNING_RECORD_OK) {
            return TUNING_STORE_INVALID_ARGUMENT; /* unreachable for valid inputs */
        }
        st->have_committed = false;
        if (st->ops->write_blob(st->ctx, TUNING_STORE_KEY_ACTIVE, pbuf, plen) !=
            TUNING_STORE_BACKEND_OK) {
            return TUNING_STORE_COMMIT_UNCERTAIN;
        }
        if (st->ops->commit(st->ctx) != TUNING_STORE_BACKEND_OK) {
            return TUNING_STORE_COMMIT_UNCERTAIN;
        }
        if (st->ops->read_blob(st->ctx, TUNING_STORE_KEY_ACTIVE, rbuf, sizeof(rbuf),
                               &rlen) != TUNING_STORE_BACKEND_OK ||
            rlen > sizeof(rbuf) ||
            tuning_record_pointer_decode(rbuf, rlen, &chk) != TUNING_RECORD_OK ||
            chk.slot != nptr.slot || chk.generation != nptr.generation) {
            return TUNING_STORE_COMMIT_UNCERTAIN;
        }
    }

    /* The old slot is intentionally NOT erased: crash-recovery evidence. */
    st->have_committed = true;
    st->committed_slot = next_slot;
    st->committed_generation = rec->generation;
    return TUNING_STORE_OK;
}

/* ------------------------------------------------------------------ */
/* Explicit complete policy reset (administrative tombstone)           */
/* ------------------------------------------------------------------ */

/*
 * ADMINISTRATIVE OPERATION ONLY — no automatic code path may call this
 * (see the header contract). Normal transaction completion preserves the
 * combined record via tuning_record_finalize_transaction + a regular
 * commit; no boot-recovery plan, rollback path or error path emits a
 * tombstone. W2 ships no production caller.
 */
TuningStoreResult tuning_store_admin_reset(TuningStore *st)
{
    TuningStoreResult res;

    if (st == NULL) {
        return TUNING_STORE_INVALID_ARGUMENT;
    }
    if (!st->initialized) {
        return TUNING_STORE_NOT_INITIALIZED;
    }

    res = tuning_store_load(st, &s_decode_scratch, NULL);
    if (res == TUNING_STORE_EMPTY || res == TUNING_STORE_CLEARED) {
        return TUNING_STORE_STATE_CONFLICT; /* nothing to clear */
    }
    if (res != TUNING_STORE_OK) {
        return res; /* recovery states cannot be cleared over */
    }

    /* Even an administrative reset refuses to destroy evidence: only a
     * SAFE transaction state (IDLE or COMMITTED) may be tombstoned. */
    if (s_decode_scratch.tx.state != TUNING_TX_IDLE &&
        s_decode_scratch.tx.state != TUNING_TX_COMMITTED) {
        return TUNING_STORE_STATE_CONFLICT;
    }

    tuning_record_init_tombstone(&s_tombstone_scratch);
    return tuning_store_commit_record(st, &s_tombstone_scratch);
}

/* ------------------------------------------------------------------ */
/* Stable machine tokens                                               */
/* ------------------------------------------------------------------ */

const char *tuning_store_result_str(TuningStoreResult r)
{
    switch (r) {
    case TUNING_STORE_OK: return "STORE_OK";
    case TUNING_STORE_EMPTY: return "STORE_EMPTY";
    case TUNING_STORE_CLEARED: return "STORE_CLEARED";
    case TUNING_STORE_NOT_INITIALIZED: return "STORE_NOT_INITIALIZED";
    case TUNING_STORE_INVALID_ARGUMENT: return "STORE_INVALID_ARGUMENT";
    case TUNING_STORE_IO_ERROR: return "STORE_IO_ERROR";
    case TUNING_STORE_CORRUPT: return "STORE_CORRUPT";
    case TUNING_STORE_UNSUPPORTED_SCHEMA: return "STORE_UNSUPPORTED_SCHEMA";
    case TUNING_STORE_INVALID_RECORD: return "STORE_INVALID_RECORD";
    case TUNING_STORE_ACTIVE_POINTER_INVALID: return "STORE_ACTIVE_POINTER_INVALID";
    case TUNING_STORE_ACTIVE_SLOT_INVALID: return "STORE_ACTIVE_SLOT_INVALID";
    case TUNING_STORE_RECOVERY_REQUIRED: return "STORE_RECOVERY_REQUIRED";
    case TUNING_STORE_GENERATION_EXHAUSTED: return "STORE_GENERATION_EXHAUSTED";
    case TUNING_STORE_READBACK_MISMATCH: return "STORE_READBACK_MISMATCH";
    case TUNING_STORE_COMMIT_UNCERTAIN: return "STORE_COMMIT_UNCERTAIN";
    case TUNING_STORE_STATE_CONFLICT: return "STORE_STATE_CONFLICT";
    default: return "STORE_RESULT_UNKNOWN";
    }
}
