#ifndef POOL_SESSION_RECORD_H_
#define POOL_SESSION_RECORD_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pool_session.h"

/*
 * NeuralAxe timed pool sessions — persisted record model, explicit wire
 * format, pure codec and semantic validator (Phase 2M.1B, Gate B3).
 * Board 601 / BM1370 only.
 *
 * This header is the PURE persistence domain: no NVS, no ESP-IDF, no
 * FreeRTOS, no heap, no logging, no global mutable state. The store layer
 * (pool_session_store.h) moves encoded records; this layer defines what a
 * record IS and what bytes it becomes.
 *
 * Architectural sources of truth (committed):
 *   docs/NEURALAXE_PHASE_2M1A_TIMED_POOL_SESSIONS_AUDIT.md          (§11)
 *   docs/NEURALAXE_PHASE_2M1A_TIMED_POOL_SESSIONS_STATE_MACHINE.md
 *   docs/NEURALAXE_PHASE_2M1A_TIMED_POOL_SESSIONS_SECURITY.md
 *   docs/NEURALAXE_PHASE_2M1B_B1_FSM_FOUNDATION_REPORT.md
 *   docs/NEURALAXE_PHASE_2M1B_B2_TRUSTED_TIME_FOUNDATION_REPORT.md
 *
 * SERIALIZATION CONTRACT (non-negotiable):
 *  - A record is NEVER persisted by memcpy of a C struct. The on-flash form
 *    is the explicit, versioned, little-endian, field-by-field wire format
 *    defined here — sizeof(PoolSessionRecord) is NOT the flash layout.
 *  - The wire numbering of B1 enums is pinned by _Static_asserts below; a
 *    future B1 renumbering breaks the build instead of the flash format.
 *
 * SECURITY INVARIANTS:
 *  - NO password field, password bytes or password pointer exists in the
 *    record, the wire format, or any token. Keep-current-password-only is
 *    explicit on the wire (a password-policy byte that must equal KEEP).
 *  - Account/worker identity (the Stratum user) IS persisted because pool
 *    restoration requires it. It is privacy-sensitive: this component never
 *    prints, logs or returns it in any diagnostic or token. It remains
 *    PLAINTEXT AT REST because NVS/flash encryption is not enabled in the
 *    current release (Phase 2M.1A §5) — stated, not hidden.
 *  - CRC32 here is ACCIDENTAL-CORRUPTION DETECTION ONLY (torn writes,
 *    truncation, malformed blobs). It is NOT authentication, NOT
 *    anti-tamper, NOT cryptographic integrity, NOT anti-rollback and NOT
 *    anti-forgery: any writer with offline flash access can recompute it.
 *    Malicious offline rollback or forgery of records remains a documented
 *    residual on this build (no Secure Boot / flash / NVS encryption).
 */

/* ------------------------------------------------------------------ */
/* Wire-format identity                                                */
/* ------------------------------------------------------------------ */

#define POOL_RECORD_MAGIC          0x4E585053u /* "SPXN" little-endian = 'N','X','P','S' in memory */
#define POOL_RECORD_SCHEMA_VERSION 1u
#define POOL_RECORD_HEADER_LEN     24u
#define POOL_RECORD_CRC_LEN        4u
/* Upper bound of any v1 encoded record (header + max payload + CRC). */
#define POOL_RECORD_MAX_ENCODED    1024u

#define POOL_RECORD_POINTER_MAGIC   0x4E585054u /* 'N','X','P','T' in memory */
#define POOL_RECORD_POINTER_VERSION 1u
#define POOL_RECORD_POINTER_LEN     16u

/* Record kinds. */
typedef enum {
    POOL_RECORD_KIND_SESSION   = 1,
    POOL_RECORD_KIND_TOMBSTONE = 2, /* committed acknowledgement/clear marker */
} PoolRecordKind;

/* Slot identifiers for the dual A/B record slots. */
#define POOL_RECORD_SLOT_A 0u
#define POOL_RECORD_SLOT_B 1u

/* Epoch sanity band — mirrors the Gate B2 trusted-time band (parity is
 * asserted in the test suite so the production components stay uncoupled). */
#define POOL_RECORD_EPOCH_MIN_S 1735689600ull /* 2025-01-01T00:00:00Z */
#define POOL_RECORD_EPOCH_MAX_S 4102444800ull /* 2100-01-01T00:00:00Z */

/* Bounded, saturating recovery counters (explicit maxima; never wrap).
 * Policy thresholds live in Gate B4 — these are storage bounds only. */
#define POOL_RECORD_REBOOT_COUNT_MAX               100u
#define POOL_RECORD_RECOVERY_ATTEMPT_MAX           10u
#define POOL_RECORD_CONSECUTIVE_RECOVERY_FAIL_MAX  10u

/* Reset/recovery classification supplied by Gate B4 (B3 never interprets
 * esp_reset_reason). 0 is the default "not classified". */
typedef enum {
    POOL_RECORD_RESET_CLASS_NONE    = 0,
    POOL_RECORD_RESET_CLASS_CLEAN   = 1,
    POOL_RECORD_RESET_CLASS_CRASH   = 2,
    POOL_RECORD_RESET_CLASS_UNKNOWN = 3,
    POOL_RECORD_RESET_CLASS__COUNT
} PoolRecordResetClass;

/* ------------------------------------------------------------------ */
/* Persisted record model (RAM view; NOT the flash layout)             */
/* ------------------------------------------------------------------ */

/*
 * Only the bounded facts required for recovery are persisted. Identities
 * reuse the bounded B1 types in RAM; on the wire every field is encoded
 * individually (length-prefixed strings, fixed-width little-endian scalars,
 * no padding). A disabled fallback is stored CANONICALLY EMPTY (zeroed) so
 * no stale endpoint bytes ever reach flash.
 */
typedef struct {
    /* format/meta */
    uint32_t generation;   /* assigned by the store at commit; >= 1 when loaded */
    uint8_t  kind;         /* PoolRecordKind */

    /* session identity (SESSION kind only; a TOMBSTONE carries none of it) */
    uint32_t                  session_id;       /* != 0 */
    uint32_t                  b1_model_version; /* POOL_SESSION_MODEL_VERSION */
    PoolSessionState          state;            /* persistent states only */
    PoolConfigIdentity        source;           /* immutable restore identity */
    PoolConfigIdentity        target;
    PoolSessionPasswordPolicy password_policy;  /* must be KEEP_CURRENT */

    /* safety state */
    bool               restore_required; /* the monotonic B1 obligation */
    bool               cancel_requested;
    bool               restore_requested;
    PoolSessionVerify  target_verify;
    PoolSessionVerify  restore_verify;
    uint16_t           last_failure_code; /* PoolSessionError value */
    PoolSessionRetries retries;           /* each bounded by the B1 maxima */

    /* time facts (Gate B2 domain; written by future gates) */
    uint32_t duration_s;                /* [POOL_SESSION_MIN..MAX_DURATION_S] */
    bool     verified_start_valid;
    uint64_t verified_start_epoch_s;
    bool     deadline_valid;
    uint64_t deadline_epoch_s;
    uint32_t deadline_sync_generation;  /* B2 sync generation at minting */
    bool     latest_trusted_valid;      /* monotonically-advancing floor      */
    uint64_t latest_trusted_epoch_s;    /* for B4's required_min_epoch_s      */

    /* bounded recovery facts (B4 consumes; B3 stores) */
    uint8_t reboot_count;
    uint8_t recovery_attempt_count;
    uint8_t consecutive_recovery_failures;
    uint8_t last_reset_class; /* PoolRecordResetClass */
} PoolSessionRecord;

/* Decoded active-slot pointer. */
typedef struct {
    uint8_t  slot;       /* POOL_RECORD_SLOT_A or _B */
    uint32_t generation; /* committed generation; >= 1 */
} PoolRecordPointer;

/* ------------------------------------------------------------------ */
/* Stable machine-readable codec/validation errors                     */
/* ------------------------------------------------------------------ */

typedef enum {
    RECORD_OK = 0,
    RECORD_ERR_INVALID_ARGUMENT,
    RECORD_ERR_BUFFER_TOO_SMALL,
    RECORD_ERR_TRUNCATED,
    RECORD_ERR_TRAILING_BYTES,
    RECORD_ERR_BAD_MAGIC,
    RECORD_ERR_UNSUPPORTED_SCHEMA,
    RECORD_ERR_UNKNOWN_FLAGS,
    RECORD_ERR_BAD_LENGTH,
    RECORD_ERR_BAD_KIND,
    RECORD_ERR_BAD_STATE,
    RECORD_ERR_BAD_CHAIN,
    RECORD_ERR_BAD_PROTOCOL,
    RECORD_ERR_PW_MODE_UNSUPPORTED,
    RECORD_ERR_BAD_PORT,
    RECORD_ERR_BAD_STRING,
    RECORD_ERR_BAD_FLAG_BYTE,
    RECORD_ERR_BAD_FAILURE_CODE,
    RECORD_ERR_BAD_DURATION,
    RECORD_ERR_RETRY_OVERFLOW,
    RECORD_ERR_BAD_GENERATION,
    RECORD_ERR_CRC_MISMATCH,
    RECORD_ERR_BAD_SESSION_ID,
    RECORD_ERR_IDENTITY_INVALID,
    RECORD_ERR_IDENTITY_EQUAL,
    RECORD_ERR_OBLIGATION_VIOLATION,
    RECORD_ERR_VERIFY_FLAGS_INVALID,
    RECORD_ERR_EPOCH_INVALID,
    RECORD_ERR_EPOCH_REGRESSION,
    RECORD_ERR_COUNTER_OVERFLOW,
    RECORD_ERR_TOMBSTONE_MALFORMED,
    POOL_RECORD_ERR__COUNT
} PoolRecordCodecError;

/* ------------------------------------------------------------------ */
/* Pure API                                                            */
/* ------------------------------------------------------------------ */

/*
 * CRC-32 (IEEE 802.3 reflected, init 0xFFFFFFFF, final XOR 0xFFFFFFFF —
 * identical to zlib crc32 and to esp_rom_crc32_le(0, ...); the QEMU suite
 * asserts that equivalence). Exposed for tests and the store layer.
 * Accidental-corruption detection ONLY (see the header contract above).
 */
uint32_t pool_record_crc32(const void *data, size_t len);

/* Zero-initialize a record (kind SESSION, everything cleared). */
void pool_session_record_init(PoolSessionRecord *rec);

/* Initialize a committed-clear TOMBSTONE record (no session identity, no
 * pool identity, no account/worker, no obligation — payload is empty). */
void pool_session_record_init_tombstone(PoolSessionRecord *rec);

/*
 * Semantic validation of a record model (cross-field invariants; separate
 * from byte decoding). RECORD_OK only when every rule passes. Invalid
 * persisted data is never silently normalized — it is rejected.
 */
PoolRecordCodecError pool_session_record_validate(const PoolSessionRecord *rec);

/*
 * Encode a record into the explicit v1 wire format. Validates first: an
 * invalid record is never encodable. On success *out_len is the exact
 * encoded length. The output buffer beyond *out_len is untouched; on
 * failure the buffer contents are unspecified but *out_len is 0.
 * The input record is never mutated.
 */
PoolRecordCodecError pool_session_record_encode(const PoolSessionRecord *rec,
                                                uint8_t *buf, size_t cap,
                                                size_t *out_len);

/*
 * Decode + verify a v1 wire blob: length/magic/schema/flag checks, CRC over
 * every byte except the CRC field itself, strict field decoding (unknown
 * enum values, bad ports, unterminated/oversized strings, trailing bytes
 * all rejected), then full semantic validation. *out is fully zeroed on
 * entry and only meaningful when RECORD_OK is returned.
 */
PoolRecordCodecError pool_session_record_decode(const uint8_t *buf, size_t len,
                                                PoolSessionRecord *out);

/* Active-slot pointer codec (small, separately versioned encoding — never a
 * naked integer). Same CRC contract. */
PoolRecordCodecError pool_record_pointer_encode(const PoolRecordPointer *ptr,
                                                uint8_t *buf, size_t cap,
                                                size_t *out_len);
PoolRecordCodecError pool_record_pointer_decode(const uint8_t *buf, size_t len,
                                                PoolRecordPointer *out);

/*
 * Propose an update of the monotonically-advancing latest-accepted trusted
 * epoch (the Gate B4 anti-regression floor). Accepted only when the value
 * is inside the sanity band, not earlier than the current floor (equal is
 * allowed) and not earlier than a valid verified_start_epoch. This value is
 * an OPERATIONAL anti-regression floor, not cryptographic proof of time.
 * B3 does not decide the write cadence — see the store header notes.
 */
PoolRecordCodecError pool_session_record_propose_trusted_epoch(PoolSessionRecord *rec,
                                                               uint64_t epoch_s);

/* Saturating counter increment (never wraps; clamps at `max`). */
uint8_t pool_record_counter_increment(uint8_t current, uint8_t max);

/* True when any bounded recovery counter has reached its storage maximum. */
bool pool_record_counters_exhausted(const PoolSessionRecord *rec);

/*
 * Converters between the live B1 session and the persisted record. These
 * copy field-by-field (never memcpy of whole structs as a format), reject
 * non-persistent states, and canonicalize a disabled fallback to empty.
 * Time facts are NOT part of PoolSession: from_session zeroes them (future
 * gates set them explicitly) and to_session cannot restore them into the
 * session model. The tombstone kind is not convertible.
 */
PoolRecordCodecError pool_session_record_from_session(const PoolSession *s,
                                                      PoolSessionRecord *out);
PoolRecordCodecError pool_session_record_to_session(const PoolSessionRecord *rec,
                                                    PoolSession *out);

/* Stable machine token (dot-free; never carries a hostname, account or any
 * persisted value). */
const char *pool_record_codec_error_str(PoolRecordCodecError e);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */

/* The wire format pins the B1 enum numbering: a renumbering must break the
 * build here rather than silently changing the flash format. */
_Static_assert(POOL_STATE_IDLE == 0 && POOL_STATE_PREPARING == 1 &&
               POOL_STATE_TARGET_SNAPSHOT_COMMITTED == 2 &&
               POOL_STATE_APPLYING_TARGET == 3 &&
               POOL_STATE_RESTARTING_FOR_TARGET == 4 &&
               POOL_STATE_VERIFYING_TARGET == 5 &&
               POOL_STATE_TARGET_ACTIVE == 6 &&
               POOL_STATE_RESTORE_DUE == 7 &&
               POOL_STATE_APPLYING_RESTORE == 8 &&
               POOL_STATE_RESTARTING_FOR_RESTORE == 9 &&
               POOL_STATE_VERIFYING_RESTORE == 10 &&
               POOL_STATE_COMPLETE == 11 &&
               POOL_STATE_TARGET_FAILED == 12 &&
               POOL_STATE_RESTORE_FAILED == 13 &&
               POOL_STATE_INTERRUPTED == 14 &&
               POOL_STATE_RECOVERY_REQUIRED == 15 &&
               POOL_STATE_CANCELLED == 16,
               "wire format pins the B1 state numbering");
_Static_assert(POOL_CHAIN_BITCOIN == 0 && POOL_CHAIN_BITCOIN_CASH == 1 &&
               POOL_CHAIN_CUSTOM_UNKNOWN == 2,
               "wire format pins the B1 chain numbering");
_Static_assert(POOL_PROTO_STRATUM_V1 == 0 && POOL_PROTO_STRATUM_V2 == 1,
               "wire format pins the B1 protocol numbering");
_Static_assert(POOL_SESSION_PW_KEEP_CURRENT == 0,
               "wire format pins keep-current-password as zero");
_Static_assert(POOL_SESSION_ERR__COUNT == 28,
               "B1 failure-code count changed — review the persisted range check");
_Static_assert(POOL_RECORD_EPOCH_MIN_S < POOL_RECORD_EPOCH_MAX_S,
               "epoch sanity band inverted");
_Static_assert(POOL_RECORD_MAX_ENCODED >= 1000u,
               "encode buffer bound below the v1 worst case");
_Static_assert(POOL_RECORD_ERR__COUNT == 31, "codec error count changed — review tokens/tests");

#endif /* POOL_SESSION_RECORD_H_ */
