/*
 * NeuralAxe timed pool sessions — persisted record codec (Gate B3).
 *
 * PURE: no NVS, no ESP-IDF, no FreeRTOS, no heap, no logging, no global
 * mutable state. Field-by-field little-endian encoding — never memcpy of a
 * C struct as a wire format. Invalid persisted data is rejected, never
 * silently normalized.
 */

#include <string.h>
#include "pool_session_record.h"

/* ------------------------------------------------------------------ */
/* CRC-32 (IEEE 802.3 reflected; equals zlib crc32 and                 */
/* esp_rom_crc32_le(0, ...) — equivalence is QEMU-tested)              */
/* ------------------------------------------------------------------ */

static const uint32_t s_crc32_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t pool_record_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    if (p == NULL) {
        return 0u; /* deterministic degenerate result; callers pass real buffers */
    }
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ s_crc32_nibble[crc & 0xFu];
        crc = (crc >> 4) ^ s_crc32_nibble[crc & 0xFu];
    }
    return ~crc;
}

/* ------------------------------------------------------------------ */
/* Bounded little-endian cursors                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   off;
    bool     overflow;
} WCur;

static void w_u8(WCur *w, uint8_t v)
{
    if (w->overflow || w->off + 1u > w->cap) {
        w->overflow = true;
        return;
    }
    w->buf[w->off++] = v;
}

static void w_u16(WCur *w, uint16_t v)
{
    w_u8(w, (uint8_t)(v & 0xFFu));
    w_u8(w, (uint8_t)(v >> 8));
}

static void w_u32(WCur *w, uint32_t v)
{
    w_u16(w, (uint16_t)(v & 0xFFFFu));
    w_u16(w, (uint16_t)(v >> 16));
}

static void w_u64(WCur *w, uint64_t v)
{
    w_u32(w, (uint32_t)(v & 0xFFFFFFFFu));
    w_u32(w, (uint32_t)(v >> 32));
}

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         off;
    bool           fail;
} RCur;

static uint8_t r_u8(RCur *r)
{
    if (r->fail || r->off + 1u > r->len) {
        r->fail = true;
        return 0u;
    }
    return r->buf[r->off++];
}

static uint16_t r_u16(RCur *r)
{
    uint16_t lo = r_u8(r);
    uint16_t hi = r_u8(r);
    return (uint16_t)(lo | (hi << 8));
}

static uint32_t r_u32(RCur *r)
{
    uint32_t lo = r_u16(r);
    uint32_t hi = r_u16(r);
    return lo | (hi << 16);
}

static uint64_t r_u64(RCur *r)
{
    uint64_t lo = r_u32(r);
    uint64_t hi = r_u32(r);
    return lo | (hi << 32);
}

/* Persisted string bytes: printable range only (no NUL, no control, no DEL). */
static bool str_char_ok(uint8_t c)
{
    return c >= 0x20u && c != 0x7Fu;
}

/* Bounded NUL-terminated string check on the RAM model. */
static bool model_str_ok(const char *s, size_t bufsize, size_t maxlen)
{
    size_t i;
    for (i = 0; i < bufsize; i++) {
        if (s[i] == '\0') {
            return i <= maxlen;
        }
        if (!str_char_ok((uint8_t)s[i])) {
            return false;
        }
    }
    return false; /* unterminated within its buffer */
}

static void w_str8(WCur *w, const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n <= maxlen && s[n] != '\0') {
        n++;
    }
    w_u8(w, (uint8_t)n);
    for (size_t i = 0; i < n; i++) {
        w_u8(w, (uint8_t)s[i]);
    }
}

static PoolRecordCodecError r_str8(RCur *r, char *dst, size_t dstcap, size_t maxlen)
{
    uint8_t n = r_u8(r);
    size_t i;
    if (r->fail) {
        return RECORD_ERR_TRUNCATED;
    }
    if ((size_t)n > maxlen || (size_t)n >= dstcap) {
        return RECORD_ERR_BAD_STRING;
    }
    for (i = 0; i < n; i++) {
        uint8_t c = r_u8(r);
        if (r->fail) {
            return RECORD_ERR_TRUNCATED;
        }
        if (!str_char_ok(c)) {
            return RECORD_ERR_BAD_STRING;
        }
        dst[i] = (char)c;
    }
    dst[n] = '\0';
    return RECORD_OK;
}

static PoolRecordCodecError r_bool(RCur *r, bool *out)
{
    uint8_t v = r_u8(r);
    if (r->fail) {
        return RECORD_ERR_TRUNCATED;
    }
    if (v > 1u) {
        return RECORD_ERR_BAD_FLAG_BYTE;
    }
    *out = (v == 1u);
    return RECORD_OK;
}

/* ------------------------------------------------------------------ */
/* Model init                                                          */
/* ------------------------------------------------------------------ */

void pool_session_record_init(PoolSessionRecord *rec)
{
    if (rec == NULL) {
        return;
    }
    memset(rec, 0, sizeof(*rec));
    rec->kind  = (uint8_t)POOL_RECORD_KIND_SESSION;
    rec->state = POOL_STATE_IDLE;
}

void pool_session_record_init_tombstone(PoolSessionRecord *rec)
{
    if (rec == NULL) {
        return;
    }
    memset(rec, 0, sizeof(*rec));
    rec->kind  = (uint8_t)POOL_RECORD_KIND_TOMBSTONE;
    rec->state = POOL_STATE_IDLE;
    /* Everything else stays zero: a tombstone carries NO session identity,
     * NO pool identity, NO account/worker and NO restore obligation. */
}

/* ------------------------------------------------------------------ */
/* Semantic validation                                                 */
/* ------------------------------------------------------------------ */

static bool endpoint_zeroed(const PoolEndpoint *e)
{
    return e->host[0] == '\0' && e->port == 0u && e->user[0] == '\0' &&
           e->protocol == POOL_PROTO_STRATUM_V1 && !e->tls;
}

static PoolRecordCodecError endpoint_semantic(const PoolEndpoint *e)
{
    if (!model_str_ok(e->host, POOL_SESSION_HOST_MAX, POOL_SESSION_HOST_MAX - 1u) ||
        e->host[0] == '\0') {
        return RECORD_ERR_IDENTITY_INVALID;
    }
    if (e->port == 0u) {
        return RECORD_ERR_BAD_PORT;
    }
    if (!model_str_ok(e->user, POOL_SESSION_USER_MAX, POOL_SESSION_USER_MAX - 1u)) {
        return RECORD_ERR_IDENTITY_INVALID; /* user may be empty, must be clean */
    }
    if ((unsigned)e->protocol >= (unsigned)POOL_PROTO__COUNT) {
        return RECORD_ERR_BAD_PROTOCOL;
    }
    return RECORD_OK;
}

static PoolRecordCodecError identity_semantic(const PoolConfigIdentity *c)
{
    PoolRecordCodecError err;
    if ((unsigned)c->chain >= (unsigned)POOL_CHAIN__COUNT) {
        return RECORD_ERR_BAD_CHAIN;
    }
    if (!model_str_ok(c->profile_id, POOL_SESSION_PROFILE_ID_MAX,
                      POOL_SESSION_PROFILE_ID_MAX - 1u)) {
        return RECORD_ERR_BAD_STRING;
    }
    err = endpoint_semantic(&c->primary);
    if (err != RECORD_OK) {
        return err;
    }
    if (c->fallback_enabled) {
        err = endpoint_semantic(&c->fallback);
        if (err != RECORD_OK) {
            return err;
        }
    } else if (!endpoint_zeroed(&c->fallback)) {
        /* Canonical form: a disabled fallback is stored fully empty so no
         * stale endpoint bytes reach flash. */
        return RECORD_ERR_IDENTITY_INVALID;
    }
    return RECORD_OK;
}

static bool record_is_zeroed_session_body(const PoolSessionRecord *rec)
{
    return rec->session_id == 0u && rec->b1_model_version == 0u &&
           rec->state == POOL_STATE_IDLE &&
           endpoint_zeroed(&rec->source.primary) && endpoint_zeroed(&rec->source.fallback) &&
           !rec->source.fallback_enabled && rec->source.profile_id[0] == '\0' &&
           rec->source.chain == POOL_CHAIN_BITCOIN &&
           endpoint_zeroed(&rec->target.primary) && endpoint_zeroed(&rec->target.fallback) &&
           !rec->target.fallback_enabled && rec->target.profile_id[0] == '\0' &&
           rec->target.chain == POOL_CHAIN_BITCOIN &&
           rec->password_policy == POOL_SESSION_PW_KEEP_CURRENT &&
           !rec->restore_required && !rec->cancel_requested && !rec->restore_requested &&
           !rec->target_verify.connection_observed && !rec->target_verify.mining_observed &&
           !rec->target_verify.identity_verified &&
           !rec->restore_verify.connection_observed && !rec->restore_verify.mining_observed &&
           !rec->restore_verify.identity_verified &&
           rec->last_failure_code == 0u &&
           rec->retries.target_apply == 0u && rec->retries.target_restart == 0u &&
           rec->retries.target_verify == 0u && rec->retries.restore_apply == 0u &&
           rec->retries.restore_restart == 0u && rec->retries.restore_verify == 0u &&
           rec->duration_s == 0u &&
           !rec->verified_start_valid && rec->verified_start_epoch_s == 0u &&
           !rec->deadline_valid && rec->deadline_epoch_s == 0u &&
           rec->deadline_sync_generation == 0u &&
           !rec->latest_trusted_valid && rec->latest_trusted_epoch_s == 0u &&
           rec->reboot_count == 0u && rec->recovery_attempt_count == 0u &&
           rec->consecutive_recovery_failures == 0u && rec->last_reset_class == 0u;
}

/* States in which the target pool MAY have been mutated: the persisted
 * restore obligation must be true (Phase 2M.1A Invariants 1-3, B1 §3). */
static bool state_requires_obligation(PoolSessionState st)
{
    switch (st) {
    case POOL_STATE_APPLYING_TARGET:
    case POOL_STATE_TARGET_ACTIVE:
    case POOL_STATE_RESTORE_DUE:
    case POOL_STATE_APPLYING_RESTORE:
    case POOL_STATE_TARGET_FAILED:
    case POOL_STATE_RESTORE_FAILED:
    case POOL_STATE_INTERRUPTED:
        return true;
    default:
        return false;
    }
}

/* States that are only persistable BEFORE mutation (or after a verified
 * restore): the obligation must be false. RECOVERY_REQUIRED may carry
 * either value (pre-mutation corrupt-record vs post-mutation exhaustion). */
static bool state_forbids_obligation(PoolSessionState st)
{
    return st == POOL_STATE_TARGET_SNAPSHOT_COMMITTED ||
           st == POOL_STATE_COMPLETE || st == POOL_STATE_CANCELLED;
}

PoolRecordCodecError pool_session_record_validate(const PoolSessionRecord *rec)
{
    PoolRecordCodecError err;

    if (rec == NULL) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }

    if (rec->kind == (uint8_t)POOL_RECORD_KIND_TOMBSTONE) {
        /* A tombstone must carry nothing: no identity, no obligation. */
        if (!record_is_zeroed_session_body(rec)) {
            return RECORD_ERR_TOMBSTONE_MALFORMED;
        }
        return RECORD_OK;
    }
    if (rec->kind != (uint8_t)POOL_RECORD_KIND_SESSION) {
        return RECORD_ERR_BAD_KIND;
    }

    if (rec->session_id == 0u) {
        return RECORD_ERR_BAD_SESSION_ID;
    }
    if (rec->b1_model_version != POOL_SESSION_MODEL_VERSION) {
        return RECORD_ERR_UNSUPPORTED_SCHEMA;
    }
    if (!pool_state_is_persistent(rec->state) || rec->state == POOL_STATE_IDLE) {
        return RECORD_ERR_BAD_STATE;
    }
    if (rec->password_policy != POOL_SESSION_PW_KEEP_CURRENT) {
        return RECORD_ERR_PW_MODE_UNSUPPORTED; /* Keep-current-only, explicit */
    }

    err = identity_semantic(&rec->source);
    if (err != RECORD_OK) {
        return err;
    }
    err = identity_semantic(&rec->target);
    if (err != RECORD_OK) {
        return err;
    }
    if (pool_config_identity_equal(&rec->source, &rec->target)) {
        return RECORD_ERR_IDENTITY_EQUAL;
    }

    if (rec->last_failure_code >= (uint16_t)POOL_SESSION_ERR__COUNT) {
        return RECORD_ERR_BAD_FAILURE_CODE;
    }
    if (rec->retries.target_apply > POOL_SESSION_MAX_TARGET_APPLY_RETRIES ||
        rec->retries.target_restart > POOL_SESSION_MAX_TARGET_RESTART_RETRIES ||
        rec->retries.target_verify > POOL_SESSION_MAX_TARGET_VERIFY_RETRIES ||
        rec->retries.restore_apply > POOL_SESSION_MAX_RESTORE_APPLY_RETRIES ||
        rec->retries.restore_restart > POOL_SESSION_MAX_RESTORE_RESTART_RETRIES ||
        rec->retries.restore_verify > POOL_SESSION_MAX_RESTORE_VERIFY_RETRIES) {
        return RECORD_ERR_RETRY_OVERFLOW;
    }
    if (rec->duration_s < POOL_SESSION_MIN_DURATION_S ||
        rec->duration_s > POOL_SESSION_MAX_DURATION_S) {
        return RECORD_ERR_BAD_DURATION;
    }

    /* Verification layering: mining/identity evidence requires connection. */
    if ((rec->target_verify.mining_observed && !rec->target_verify.connection_observed) ||
        (rec->target_verify.identity_verified && !rec->target_verify.connection_observed) ||
        (rec->restore_verify.mining_observed && !rec->restore_verify.connection_observed) ||
        (rec->restore_verify.identity_verified && !rec->restore_verify.connection_observed)) {
        return RECORD_ERR_VERIFY_FLAGS_INVALID;
    }

    /* The monotonic restore obligation vs the persisted state. */
    if (state_requires_obligation(rec->state) && !rec->restore_required) {
        return RECORD_ERR_OBLIGATION_VIOLATION;
    }
    if (state_forbids_obligation(rec->state) && rec->restore_required) {
        return RECORD_ERR_OBLIGATION_VIOLATION;
    }

    /* State-specific verification evidence. */
    if (rec->state == POOL_STATE_TARGET_ACTIVE &&
        (!rec->target_verify.connection_observed || !rec->target_verify.mining_observed ||
         !rec->target_verify.identity_verified)) {
        return RECORD_ERR_VERIFY_FLAGS_INVALID;
    }
    if (rec->state == POOL_STATE_COMPLETE &&
        (!rec->restore_verify.connection_observed || !rec->restore_verify.mining_observed ||
         !rec->restore_verify.identity_verified)) {
        return RECORD_ERR_VERIFY_FLAGS_INVALID;
    }

    /* Epoch facts: canonical zeros when invalid; sanity band when valid. */
    if (!rec->verified_start_valid && rec->verified_start_epoch_s != 0u) {
        return RECORD_ERR_EPOCH_INVALID;
    }
    if (!rec->deadline_valid &&
        (rec->deadline_epoch_s != 0u || rec->deadline_sync_generation != 0u)) {
        return RECORD_ERR_EPOCH_INVALID;
    }
    if (!rec->latest_trusted_valid && rec->latest_trusted_epoch_s != 0u) {
        return RECORD_ERR_EPOCH_INVALID;
    }
    if (rec->verified_start_valid &&
        (rec->verified_start_epoch_s < POOL_RECORD_EPOCH_MIN_S ||
         rec->verified_start_epoch_s > POOL_RECORD_EPOCH_MAX_S)) {
        return RECORD_ERR_EPOCH_INVALID;
    }
    if (rec->deadline_valid) {
        if (!rec->verified_start_valid) {
            /* B2/B3 design: a deadline is minted at TARGET_ACTIVE together
             * with the verified start; no alternate case exists. */
            return RECORD_ERR_EPOCH_INVALID;
        }
        if (rec->deadline_epoch_s < POOL_RECORD_EPOCH_MIN_S ||
            rec->deadline_epoch_s >
                POOL_RECORD_EPOCH_MAX_S + (uint64_t)POOL_SESSION_MAX_DURATION_S) {
            return RECORD_ERR_EPOCH_INVALID;
        }
        if (rec->deadline_epoch_s < rec->verified_start_epoch_s) {
            return RECORD_ERR_EPOCH_INVALID;
        }
    }
    if (rec->latest_trusted_valid) {
        if (rec->latest_trusted_epoch_s < POOL_RECORD_EPOCH_MIN_S ||
            rec->latest_trusted_epoch_s > POOL_RECORD_EPOCH_MAX_S) {
            return RECORD_ERR_EPOCH_INVALID;
        }
        if (rec->verified_start_valid &&
            rec->latest_trusted_epoch_s < rec->verified_start_epoch_s) {
            return RECORD_ERR_EPOCH_REGRESSION;
        }
    }

    if (rec->reboot_count > POOL_RECORD_REBOOT_COUNT_MAX ||
        rec->recovery_attempt_count > POOL_RECORD_RECOVERY_ATTEMPT_MAX ||
        rec->consecutive_recovery_failures > POOL_RECORD_CONSECUTIVE_RECOVERY_FAIL_MAX) {
        return RECORD_ERR_COUNTER_OVERFLOW;
    }
    if (rec->last_reset_class >= (uint8_t)POOL_RECORD_RESET_CLASS__COUNT) {
        return RECORD_ERR_BAD_FLAG_BYTE;
    }

    return RECORD_OK;
}

/* ------------------------------------------------------------------ */
/* Record encoder                                                      */
/* ------------------------------------------------------------------ */

static void encode_identity(WCur *w, const PoolConfigIdentity *c)
{
    w_u8(w, (uint8_t)c->chain);
    w_str8(w, c->profile_id, POOL_SESSION_PROFILE_ID_MAX - 1u);
    w_str8(w, c->primary.host, POOL_SESSION_HOST_MAX - 1u);
    w_u16(w, c->primary.port);
    w_str8(w, c->primary.user, POOL_SESSION_USER_MAX - 1u);
    w_u8(w, (uint8_t)c->primary.protocol);
    w_u8(w, c->primary.tls ? 1u : 0u);
    w_u8(w, c->fallback_enabled ? 1u : 0u);
    w_str8(w, c->fallback.host, POOL_SESSION_HOST_MAX - 1u);
    w_u16(w, c->fallback.port);
    w_str8(w, c->fallback.user, POOL_SESSION_USER_MAX - 1u);
    w_u8(w, (uint8_t)c->fallback.protocol);
    w_u8(w, c->fallback.tls ? 1u : 0u);
}

static void store_u16_at(uint8_t *buf, size_t off, uint16_t v)
{
    buf[off]      = (uint8_t)(v & 0xFFu);
    buf[off + 1u] = (uint8_t)(v >> 8);
}

static void store_u32_at(uint8_t *buf, size_t off, uint32_t v)
{
    store_u16_at(buf, off, (uint16_t)(v & 0xFFFFu));
    store_u16_at(buf, off + 2u, (uint16_t)(v >> 16));
}

PoolRecordCodecError pool_session_record_encode(const PoolSessionRecord *rec,
                                                uint8_t *buf, size_t cap,
                                                size_t *out_len)
{
    PoolRecordCodecError err;
    WCur w;
    size_t payload_len;
    uint32_t crc;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (rec == NULL || buf == NULL || out_len == NULL) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    err = pool_session_record_validate(rec);
    if (err != RECORD_OK) {
        return err;
    }
    if (rec->generation == 0u) {
        return RECORD_ERR_BAD_GENERATION; /* the store assigns >= 1 */
    }
    if (cap < POOL_RECORD_HEADER_LEN + POOL_RECORD_CRC_LEN) {
        return RECORD_ERR_BUFFER_TOO_SMALL;
    }

    w.buf = buf;
    w.cap = (cap < POOL_RECORD_MAX_ENCODED) ? cap : POOL_RECORD_MAX_ENCODED;
    w.off = POOL_RECORD_HEADER_LEN; /* payload first; header patched below */
    w.overflow = false;

    if (rec->kind == (uint8_t)POOL_RECORD_KIND_SESSION) {
        w_u32(&w, rec->session_id);
        w_u32(&w, rec->b1_model_version);
        w_u8(&w, (uint8_t)rec->state);
        w_u8(&w, (uint8_t)rec->password_policy);
        encode_identity(&w, &rec->source);
        encode_identity(&w, &rec->target);
        w_u8(&w, rec->restore_required ? 1u : 0u);
        w_u8(&w, rec->cancel_requested ? 1u : 0u);
        w_u8(&w, rec->restore_requested ? 1u : 0u);
        w_u8(&w, rec->target_verify.connection_observed ? 1u : 0u);
        w_u8(&w, rec->target_verify.mining_observed ? 1u : 0u);
        w_u8(&w, rec->target_verify.identity_verified ? 1u : 0u);
        w_u8(&w, rec->restore_verify.connection_observed ? 1u : 0u);
        w_u8(&w, rec->restore_verify.mining_observed ? 1u : 0u);
        w_u8(&w, rec->restore_verify.identity_verified ? 1u : 0u);
        w_u16(&w, rec->last_failure_code);
        w_u8(&w, rec->retries.target_apply);
        w_u8(&w, rec->retries.target_restart);
        w_u8(&w, rec->retries.target_verify);
        w_u8(&w, rec->retries.restore_apply);
        w_u8(&w, rec->retries.restore_restart);
        w_u8(&w, rec->retries.restore_verify);
        w_u32(&w, rec->duration_s);
        w_u8(&w, rec->verified_start_valid ? 1u : 0u);
        w_u64(&w, rec->verified_start_epoch_s);
        w_u8(&w, rec->deadline_valid ? 1u : 0u);
        w_u64(&w, rec->deadline_epoch_s);
        w_u32(&w, rec->deadline_sync_generation);
        w_u8(&w, rec->latest_trusted_valid ? 1u : 0u);
        w_u64(&w, rec->latest_trusted_epoch_s);
        w_u8(&w, rec->reboot_count);
        w_u8(&w, rec->recovery_attempt_count);
        w_u8(&w, rec->consecutive_recovery_failures);
        w_u8(&w, rec->last_reset_class);
    }
    /* TOMBSTONE: payload intentionally empty. */

    if (w.overflow) {
        return RECORD_ERR_BUFFER_TOO_SMALL;
    }
    payload_len = w.off - POOL_RECORD_HEADER_LEN;
    if (w.off + POOL_RECORD_CRC_LEN > w.cap) {
        return RECORD_ERR_BUFFER_TOO_SMALL;
    }

    /* Header (fixed 24 bytes, little-endian, no implicit padding). */
    store_u32_at(buf, 0u, POOL_RECORD_MAGIC);
    store_u16_at(buf, 4u, (uint16_t)POOL_RECORD_SCHEMA_VERSION);
    store_u16_at(buf, 6u, (uint16_t)POOL_RECORD_HEADER_LEN);
    store_u32_at(buf, 8u, (uint32_t)(w.off + POOL_RECORD_CRC_LEN)); /* total_len */
    store_u32_at(buf, 12u, rec->generation);
    buf[16] = rec->kind;
    buf[17] = 0u; /* flags: none defined in v1 */
    store_u16_at(buf, 18u, 0u); /* reserved: must be zero */
    store_u32_at(buf, 20u, (uint32_t)payload_len);

    /* CRC over every encoded byte except the CRC field itself. */
    crc = pool_record_crc32(buf, w.off);
    store_u32_at(buf, w.off, crc);
    *out_len = w.off + POOL_RECORD_CRC_LEN;
    return RECORD_OK;
}

/* ------------------------------------------------------------------ */
/* Record decoder                                                      */
/* ------------------------------------------------------------------ */

static PoolRecordCodecError decode_identity(RCur *r, PoolConfigIdentity *c)
{
    PoolRecordCodecError err;
    uint8_t v;

    v = r_u8(r);
    if (r->fail) {
        return RECORD_ERR_TRUNCATED;
    }
    if (v >= (uint8_t)POOL_CHAIN__COUNT) {
        return RECORD_ERR_BAD_CHAIN;
    }
    c->chain = (PoolChainType)v;

    err = r_str8(r, c->profile_id, sizeof(c->profile_id), POOL_SESSION_PROFILE_ID_MAX - 1u);
    if (err != RECORD_OK) {
        return err;
    }
    err = r_str8(r, c->primary.host, sizeof(c->primary.host), POOL_SESSION_HOST_MAX - 1u);
    if (err != RECORD_OK) {
        return err;
    }
    c->primary.port = r_u16(r);
    err = r_str8(r, c->primary.user, sizeof(c->primary.user), POOL_SESSION_USER_MAX - 1u);
    if (err != RECORD_OK) {
        return err;
    }
    v = r_u8(r);
    if (r->fail) {
        return RECORD_ERR_TRUNCATED;
    }
    if (v >= (uint8_t)POOL_PROTO__COUNT) {
        return RECORD_ERR_BAD_PROTOCOL;
    }
    c->primary.protocol = (PoolSessionProtocol)v;
    err = r_bool(r, &c->primary.tls);
    if (err != RECORD_OK) {
        return err;
    }
    err = r_bool(r, &c->fallback_enabled);
    if (err != RECORD_OK) {
        return err;
    }
    err = r_str8(r, c->fallback.host, sizeof(c->fallback.host), POOL_SESSION_HOST_MAX - 1u);
    if (err != RECORD_OK) {
        return err;
    }
    c->fallback.port = r_u16(r);
    err = r_str8(r, c->fallback.user, sizeof(c->fallback.user), POOL_SESSION_USER_MAX - 1u);
    if (err != RECORD_OK) {
        return err;
    }
    v = r_u8(r);
    if (r->fail) {
        return RECORD_ERR_TRUNCATED;
    }
    if (v >= (uint8_t)POOL_PROTO__COUNT) {
        return RECORD_ERR_BAD_PROTOCOL;
    }
    c->fallback.protocol = (PoolSessionProtocol)v;
    err = r_bool(r, &c->fallback.tls);
    if (err != RECORD_OK) {
        return err;
    }
    return RECORD_OK;
}

PoolRecordCodecError pool_session_record_decode(const uint8_t *buf, size_t len,
                                                PoolSessionRecord *out)
{
    uint32_t magic, total_len, payload_len, stored_crc, calc_crc, generation;
    uint16_t schema, header_len, reserved;
    uint8_t kind, flags;
    RCur r;
    PoolRecordCodecError err;

    if (out == NULL) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    pool_session_record_init(out);
    if (buf == NULL) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    if (len < POOL_RECORD_HEADER_LEN + POOL_RECORD_CRC_LEN) {
        return RECORD_ERR_TRUNCATED;
    }
    if (len > POOL_RECORD_MAX_ENCODED) {
        return RECORD_ERR_BAD_LENGTH;
    }

    r.buf = buf;
    r.len = POOL_RECORD_HEADER_LEN;
    r.off = 0;
    r.fail = false;
    magic      = r_u32(&r);
    schema     = r_u16(&r);
    header_len = r_u16(&r);
    total_len  = r_u32(&r);
    generation = r_u32(&r);
    kind       = r_u8(&r);
    flags      = r_u8(&r);
    reserved   = r_u16(&r);
    payload_len = r_u32(&r);

    if (magic != POOL_RECORD_MAGIC) {
        return RECORD_ERR_BAD_MAGIC;
    }
    if (schema != (uint16_t)POOL_RECORD_SCHEMA_VERSION) {
        return RECORD_ERR_UNSUPPORTED_SCHEMA;
    }
    if (header_len != (uint16_t)POOL_RECORD_HEADER_LEN) {
        return RECORD_ERR_BAD_LENGTH;
    }
    if (flags != 0u || reserved != 0u) {
        return RECORD_ERR_UNKNOWN_FLAGS; /* unknown bits are rejected in v1 */
    }
    if (total_len > len) {
        return RECORD_ERR_TRUNCATED;
    }
    if (total_len < len) {
        return RECORD_ERR_TRAILING_BYTES;
    }
    if ((size_t)total_len !=
        (size_t)POOL_RECORD_HEADER_LEN + (size_t)payload_len + POOL_RECORD_CRC_LEN) {
        return RECORD_ERR_BAD_LENGTH;
    }

    /* CRC before any payload interpretation. */
    stored_crc = (uint32_t)buf[len - 4u] | ((uint32_t)buf[len - 3u] << 8) |
                 ((uint32_t)buf[len - 2u] << 16) | ((uint32_t)buf[len - 1u] << 24);
    calc_crc = pool_record_crc32(buf, len - POOL_RECORD_CRC_LEN);
    if (stored_crc != calc_crc) {
        return RECORD_ERR_CRC_MISMATCH;
    }

    if (generation == 0u) {
        return RECORD_ERR_BAD_GENERATION;
    }

    if (kind == (uint8_t)POOL_RECORD_KIND_TOMBSTONE) {
        if (payload_len != 0u) {
            /* A tombstone carrying any payload (e.g. a session identity)
             * is malformed by definition. */
            return RECORD_ERR_TOMBSTONE_MALFORMED;
        }
        pool_session_record_init_tombstone(out);
        out->generation = generation;
        return pool_session_record_validate(out);
    }
    if (kind != (uint8_t)POOL_RECORD_KIND_SESSION) {
        return RECORD_ERR_BAD_KIND;
    }

    /* Payload cursor bounded to exactly the declared payload. */
    r.buf = buf + POOL_RECORD_HEADER_LEN;
    r.len = payload_len;
    r.off = 0;
    r.fail = false;

    out->generation = generation;
    out->kind = kind;
    out->session_id = r_u32(&r);
    out->b1_model_version = r_u32(&r);
    {
        uint8_t v = r_u8(&r);
        if (r.fail) {
            return RECORD_ERR_TRUNCATED;
        }
        if (v >= (uint8_t)POOL_STATE__COUNT) {
            return RECORD_ERR_BAD_STATE;
        }
        out->state = (PoolSessionState)v;
        v = r_u8(&r);
        if (r.fail) {
            return RECORD_ERR_TRUNCATED;
        }
        if (v >= (uint8_t)POOL_SESSION_PW__COUNT) {
            return RECORD_ERR_BAD_FLAG_BYTE;
        }
        out->password_policy = (PoolSessionPasswordPolicy)v;
    }
    err = decode_identity(&r, &out->source);
    if (err != RECORD_OK) {
        return err;
    }
    err = decode_identity(&r, &out->target);
    if (err != RECORD_OK) {
        return err;
    }
    err = r_bool(&r, &out->restore_required);
    if (err == RECORD_OK) err = r_bool(&r, &out->cancel_requested);
    if (err == RECORD_OK) err = r_bool(&r, &out->restore_requested);
    if (err == RECORD_OK) err = r_bool(&r, &out->target_verify.connection_observed);
    if (err == RECORD_OK) err = r_bool(&r, &out->target_verify.mining_observed);
    if (err == RECORD_OK) err = r_bool(&r, &out->target_verify.identity_verified);
    if (err == RECORD_OK) err = r_bool(&r, &out->restore_verify.connection_observed);
    if (err == RECORD_OK) err = r_bool(&r, &out->restore_verify.mining_observed);
    if (err == RECORD_OK) err = r_bool(&r, &out->restore_verify.identity_verified);
    if (err != RECORD_OK) {
        return err;
    }
    out->last_failure_code = r_u16(&r);
    out->retries.target_apply = r_u8(&r);
    out->retries.target_restart = r_u8(&r);
    out->retries.target_verify = r_u8(&r);
    out->retries.restore_apply = r_u8(&r);
    out->retries.restore_restart = r_u8(&r);
    out->retries.restore_verify = r_u8(&r);
    out->duration_s = r_u32(&r);
    err = r_bool(&r, &out->verified_start_valid);
    if (err != RECORD_OK) {
        return err;
    }
    out->verified_start_epoch_s = r_u64(&r);
    err = r_bool(&r, &out->deadline_valid);
    if (err != RECORD_OK) {
        return err;
    }
    out->deadline_epoch_s = r_u64(&r);
    out->deadline_sync_generation = r_u32(&r);
    err = r_bool(&r, &out->latest_trusted_valid);
    if (err != RECORD_OK) {
        return err;
    }
    out->latest_trusted_epoch_s = r_u64(&r);
    out->reboot_count = r_u8(&r);
    out->recovery_attempt_count = r_u8(&r);
    out->consecutive_recovery_failures = r_u8(&r);
    out->last_reset_class = r_u8(&r);

    if (r.fail) {
        return RECORD_ERR_TRUNCATED;
    }
    if (r.off != r.len) {
        return RECORD_ERR_TRAILING_BYTES; /* payload longer than the fields */
    }

    return pool_session_record_validate(out);
}

/* ------------------------------------------------------------------ */
/* Active-slot pointer codec                                           */
/* ------------------------------------------------------------------ */

PoolRecordCodecError pool_record_pointer_encode(const PoolRecordPointer *ptr,
                                                uint8_t *buf, size_t cap,
                                                size_t *out_len)
{
    uint32_t crc;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (ptr == NULL || buf == NULL || out_len == NULL) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    if (cap < POOL_RECORD_POINTER_LEN) {
        return RECORD_ERR_BUFFER_TOO_SMALL;
    }
    if (ptr->slot != POOL_RECORD_SLOT_A && ptr->slot != POOL_RECORD_SLOT_B) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    if (ptr->generation == 0u) {
        return RECORD_ERR_BAD_GENERATION;
    }
    store_u32_at(buf, 0u, POOL_RECORD_POINTER_MAGIC);
    store_u16_at(buf, 4u, (uint16_t)POOL_RECORD_POINTER_VERSION);
    buf[6] = ptr->slot;
    buf[7] = 0u; /* reserved: must be zero */
    store_u32_at(buf, 8u, ptr->generation);
    crc = pool_record_crc32(buf, 12u);
    store_u32_at(buf, 12u, crc);
    *out_len = POOL_RECORD_POINTER_LEN;
    return RECORD_OK;
}

PoolRecordCodecError pool_record_pointer_decode(const uint8_t *buf, size_t len,
                                                PoolRecordPointer *out)
{
    uint32_t magic, stored_crc, calc_crc;
    uint16_t version;

    if (out == NULL) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    out->slot = POOL_RECORD_SLOT_A;
    out->generation = 0u;
    if (buf == NULL) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    if (len < POOL_RECORD_POINTER_LEN) {
        return RECORD_ERR_TRUNCATED;
    }
    if (len > POOL_RECORD_POINTER_LEN) {
        return RECORD_ERR_TRAILING_BYTES;
    }
    magic = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
            ((uint32_t)buf[3] << 24);
    version = (uint16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
    if (magic != POOL_RECORD_POINTER_MAGIC) {
        return RECORD_ERR_BAD_MAGIC;
    }
    if (version != (uint16_t)POOL_RECORD_POINTER_VERSION) {
        return RECORD_ERR_UNSUPPORTED_SCHEMA;
    }
    stored_crc = (uint32_t)buf[12] | ((uint32_t)buf[13] << 8) | ((uint32_t)buf[14] << 16) |
                 ((uint32_t)buf[15] << 24);
    calc_crc = pool_record_crc32(buf, 12u);
    if (stored_crc != calc_crc) {
        return RECORD_ERR_CRC_MISMATCH;
    }
    if (buf[7] != 0u) {
        return RECORD_ERR_UNKNOWN_FLAGS;
    }
    if (buf[6] != POOL_RECORD_SLOT_A && buf[6] != POOL_RECORD_SLOT_B) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    out->slot = buf[6];
    out->generation = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
                      ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
    if (out->generation == 0u) {
        return RECORD_ERR_BAD_GENERATION;
    }
    return RECORD_OK;
}

/* ------------------------------------------------------------------ */
/* Latest-accepted trusted epoch floor                                 */
/* ------------------------------------------------------------------ */

PoolRecordCodecError pool_session_record_propose_trusted_epoch(PoolSessionRecord *rec,
                                                               uint64_t epoch_s)
{
    if (rec == NULL) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    if (epoch_s < POOL_RECORD_EPOCH_MIN_S || epoch_s > POOL_RECORD_EPOCH_MAX_S) {
        return RECORD_ERR_EPOCH_INVALID;
    }
    if (rec->latest_trusted_valid && epoch_s < rec->latest_trusted_epoch_s) {
        return RECORD_ERR_EPOCH_REGRESSION; /* the floor never decreases */
    }
    if (rec->verified_start_valid && epoch_s < rec->verified_start_epoch_s) {
        return RECORD_ERR_EPOCH_REGRESSION;
    }
    rec->latest_trusted_valid = true;
    rec->latest_trusted_epoch_s = epoch_s;
    return RECORD_OK;
}

/* ------------------------------------------------------------------ */
/* Bounded saturating counters                                         */
/* ------------------------------------------------------------------ */

uint8_t pool_record_counter_increment(uint8_t current, uint8_t max)
{
    if (current >= max) {
        return max; /* saturate; never wrap */
    }
    return (uint8_t)(current + 1u);
}

bool pool_record_counters_exhausted(const PoolSessionRecord *rec)
{
    if (rec == NULL) {
        return true; /* fail safe */
    }
    return rec->reboot_count >= POOL_RECORD_REBOOT_COUNT_MAX ||
           rec->recovery_attempt_count >= POOL_RECORD_RECOVERY_ATTEMPT_MAX ||
           rec->consecutive_recovery_failures >= POOL_RECORD_CONSECUTIVE_RECOVERY_FAIL_MAX;
}

/* ------------------------------------------------------------------ */
/* B1 session converters (field-by-field; never a format)              */
/* ------------------------------------------------------------------ */

static void copy_identity_canonical(PoolConfigIdentity *dst, const PoolConfigIdentity *src)
{
    *dst = *src;
    if (!dst->fallback_enabled) {
        /* Canonical persisted form: a disabled fallback carries no bytes. */
        memset(&dst->fallback, 0, sizeof(dst->fallback));
    }
}

PoolRecordCodecError pool_session_record_from_session(const PoolSession *s,
                                                      PoolSessionRecord *out)
{
    if (s == NULL || out == NULL) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    if (!pool_state_is_persistent(s->state) || s->state == POOL_STATE_IDLE) {
        return RECORD_ERR_BAD_STATE; /* ephemeral states are never persisted */
    }
    pool_session_record_init(out);
    out->session_id       = s->session_id;
    out->b1_model_version = s->model_version;
    out->state            = s->state;
    copy_identity_canonical(&out->source, &s->source);
    copy_identity_canonical(&out->target, &s->target);
    out->password_policy   = s->password_policy;
    out->restore_required  = s->restore_required;
    out->cancel_requested  = s->cancel_requested;
    out->restore_requested = s->restore_requested;
    out->target_verify     = s->target_verify;
    out->restore_verify    = s->restore_verify;
    out->last_failure_code = (uint16_t)s->last_error;
    out->retries           = s->retries;
    out->duration_s        = s->duration_s;
    /* Time facts are not part of PoolSession: future gates set them
     * explicitly (verified start, deadline, trusted-epoch floor). */
    return pool_session_record_validate(out);
}

PoolRecordCodecError pool_session_record_to_session(const PoolSessionRecord *rec,
                                                    PoolSession *out)
{
    PoolRecordCodecError err;

    if (rec == NULL || out == NULL) {
        return RECORD_ERR_INVALID_ARGUMENT;
    }
    if (rec->kind != (uint8_t)POOL_RECORD_KIND_SESSION) {
        return RECORD_ERR_BAD_KIND; /* a tombstone restores nothing */
    }
    err = pool_session_record_validate(rec);
    if (err != RECORD_OK) {
        return err;
    }
    pool_session_init(out);
    out->model_version     = rec->b1_model_version;
    out->session_id        = rec->session_id;
    out->generation        = rec->generation; /* the B1 reserved placeholder */
    out->state             = rec->state;
    out->source            = rec->source;
    out->target            = rec->target;
    out->duration_s        = rec->duration_s;
    out->password_policy   = rec->password_policy;
    out->target_verify     = rec->target_verify;
    out->restore_verify    = rec->restore_verify;
    out->retries           = rec->retries;
    out->last_error        = (PoolSessionError)rec->last_failure_code;
    out->cancel_requested  = rec->cancel_requested;
    out->restore_requested = rec->restore_requested;
    out->restore_required  = rec->restore_required;
    out->terminal_result   = pool_state_is_terminal(rec->state) ? rec->state : POOL_STATE_IDLE;
    return RECORD_OK;
}

/* ------------------------------------------------------------------ */
/* Stable machine tokens                                               */
/* ------------------------------------------------------------------ */

const char *pool_record_codec_error_str(PoolRecordCodecError e)
{
    switch (e) {
    case RECORD_OK:                       return "RECORD_OK";
    case RECORD_ERR_INVALID_ARGUMENT:     return "RECORD_ERR_INVALID_ARGUMENT";
    case RECORD_ERR_BUFFER_TOO_SMALL:     return "RECORD_ERR_BUFFER_TOO_SMALL";
    case RECORD_ERR_TRUNCATED:            return "RECORD_ERR_TRUNCATED";
    case RECORD_ERR_TRAILING_BYTES:       return "RECORD_ERR_TRAILING_BYTES";
    case RECORD_ERR_BAD_MAGIC:            return "RECORD_ERR_BAD_MAGIC";
    case RECORD_ERR_UNSUPPORTED_SCHEMA:   return "RECORD_ERR_UNSUPPORTED_SCHEMA";
    case RECORD_ERR_UNKNOWN_FLAGS:        return "RECORD_ERR_UNKNOWN_FLAGS";
    case RECORD_ERR_BAD_LENGTH:           return "RECORD_ERR_BAD_LENGTH";
    case RECORD_ERR_BAD_KIND:             return "RECORD_ERR_BAD_KIND";
    case RECORD_ERR_BAD_STATE:            return "RECORD_ERR_BAD_STATE";
    case RECORD_ERR_BAD_CHAIN:            return "RECORD_ERR_BAD_CHAIN";
    case RECORD_ERR_BAD_PROTOCOL:         return "RECORD_ERR_BAD_PROTOCOL";
    case RECORD_ERR_PW_MODE_UNSUPPORTED:  return "RECORD_ERR_PW_MODE_UNSUPPORTED";
    case RECORD_ERR_BAD_PORT:             return "RECORD_ERR_BAD_PORT";
    case RECORD_ERR_BAD_STRING:           return "RECORD_ERR_BAD_STRING";
    case RECORD_ERR_BAD_FLAG_BYTE:        return "RECORD_ERR_BAD_FLAG_BYTE";
    case RECORD_ERR_BAD_FAILURE_CODE:     return "RECORD_ERR_BAD_FAILURE_CODE";
    case RECORD_ERR_BAD_DURATION:         return "RECORD_ERR_BAD_DURATION";
    case RECORD_ERR_RETRY_OVERFLOW:       return "RECORD_ERR_RETRY_OVERFLOW";
    case RECORD_ERR_BAD_GENERATION:       return "RECORD_ERR_BAD_GENERATION";
    case RECORD_ERR_CRC_MISMATCH:         return "RECORD_ERR_CRC_MISMATCH";
    case RECORD_ERR_BAD_SESSION_ID:       return "RECORD_ERR_BAD_SESSION_ID";
    case RECORD_ERR_IDENTITY_INVALID:     return "RECORD_ERR_IDENTITY_INVALID";
    case RECORD_ERR_IDENTITY_EQUAL:       return "RECORD_ERR_IDENTITY_EQUAL";
    case RECORD_ERR_OBLIGATION_VIOLATION: return "RECORD_ERR_OBLIGATION_VIOLATION";
    case RECORD_ERR_VERIFY_FLAGS_INVALID: return "RECORD_ERR_VERIFY_FLAGS_INVALID";
    case RECORD_ERR_EPOCH_INVALID:        return "RECORD_ERR_EPOCH_INVALID";
    case RECORD_ERR_EPOCH_REGRESSION:     return "RECORD_ERR_EPOCH_REGRESSION";
    case RECORD_ERR_COUNTER_OVERFLOW:     return "RECORD_ERR_COUNTER_OVERFLOW";
    case RECORD_ERR_TOMBSTONE_MALFORMED:  return "RECORD_ERR_TOMBSTONE_MALFORMED";
    default:                              return "RECORD_ERR_UNKNOWN";
    }
}
