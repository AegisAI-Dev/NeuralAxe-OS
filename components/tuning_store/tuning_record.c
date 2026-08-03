/*
 * NeuralAxe Weather-Aware Tuning — persisted record codec (Gate W2).
 *
 * PURE: no NVS, no ESP-IDF, no FreeRTOS, no heap, no logging, no global
 * mutable state. Field-by-field little-endian encoding — never memcpy of a
 * C struct as a wire format. Invalid persisted data is rejected, never
 * silently normalized.
 */

#include <string.h>
#include "tuning_record.h"

/* ------------------------------------------------------------------ */
/* CRC-32 (IEEE 802.3 reflected; equals zlib crc32 and                 */
/* esp_rom_crc32_le(0, ...) — same table as the QEMU-proven B3 codec)  */
/* ------------------------------------------------------------------ */

static const uint32_t s_crc32_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t tuning_record_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    if (p == NULL) {
        return 0u;
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

static void store_u16_at(uint8_t *buf, size_t off, uint16_t v)
{
    buf[off] = (uint8_t)(v & 0xFFu);
    buf[off + 1u] = (uint8_t)(v >> 8);
}

static void store_u32_at(uint8_t *buf, size_t off, uint32_t v)
{
    store_u16_at(buf, off, (uint16_t)(v & 0xFFFFu));
    store_u16_at(buf, off + 2u, (uint16_t)(v >> 16));
}

static uint16_t load_u16_at(const uint8_t *buf, size_t off)
{
    return (uint16_t)((uint16_t)buf[off] | ((uint16_t)buf[off + 1u] << 8));
}

static uint32_t load_u32_at(const uint8_t *buf, size_t off)
{
    return (uint32_t)load_u16_at(buf, off) |
           ((uint32_t)load_u16_at(buf, off + 2u) << 16);
}

/* Persisted string bytes: printable range only (no NUL/control/DEL). */
static bool str_char_ok(uint8_t c)
{
    return c >= 0x20u && c != 0x7Fu;
}

/* Bounded NUL-terminated printable string on the RAM model. */
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

/* Stable-id charset (mirrors the Gate W1 tuning_profile id rule):
 * lowercase [a-z0-9-], bounded, NUL-terminated. Empty allowed. */
static bool model_id_ok(const char *s, size_t bufsize)
{
    size_t i;
    for (i = 0; i < bufsize; i++) {
        char c = s[i];
        if (c == '\0') {
            return true;
        }
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return false; /* unterminated */
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

static TuningRecordError r_str8(RCur *r, char *dst, size_t dstcap, size_t maxlen)
{
    uint8_t n = r_u8(r);
    size_t i;
    if (r->fail) {
        return TUNING_RECORD_ERR_TRUNCATED;
    }
    if ((size_t)n > maxlen || (size_t)n >= dstcap) {
        return TUNING_RECORD_ERR_BAD_STRING;
    }
    for (i = 0; i < n; i++) {
        uint8_t c = r_u8(r);
        if (r->fail) {
            return TUNING_RECORD_ERR_TRUNCATED;
        }
        if (!str_char_ok(c)) {
            return TUNING_RECORD_ERR_BAD_STRING;
        }
        dst[i] = (char)c;
    }
    dst[n] = '\0';
    return TUNING_RECORD_OK;
}

static TuningRecordError r_bool(RCur *r, bool *out)
{
    uint8_t v = r_u8(r);
    if (r->fail) {
        return TUNING_RECORD_ERR_TRUNCATED;
    }
    if (v > 1u) {
        return TUNING_RECORD_ERR_BAD_FLAG_BYTE;
    }
    *out = (v == 1u);
    return TUNING_RECORD_OK;
}

static TuningRecordError r_enum8(RCur *r, unsigned limit, uint8_t *out)
{
    uint8_t v = r_u8(r);
    if (r->fail) {
        return TUNING_RECORD_ERR_TRUNCATED;
    }
    if ((unsigned)v >= limit) {
        return TUNING_RECORD_ERR_BAD_ENUM;
    }
    *out = v;
    return TUNING_RECORD_OK;
}

/* 0 is always allowed; nonzero must sit inside the sanity band. */
static bool epoch_zero_or_band(uint64_t e)
{
    return e == 0ull ||
           (e >= TUNING_RECORD_EPOCH_MIN_S && e <= TUNING_RECORD_EPOCH_MAX_S);
}

/* ------------------------------------------------------------------ */
/* Defaults + init                                                     */
/* ------------------------------------------------------------------ */

void tuning_settings_defaults(TuningPolicySettings *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->enabled = false; /* the feature ships DISABLED */
    /* NO implicit provider: selecting OPEN_METEO is an explicit future
     * configuration action (the W0 commercial decision is unresolved). */
    out->provider = TUNING_PROVIDER_UNCONFIGURED;
    /* Bounded product timezone default — implies no provider. */
    out->timezone = TUNING_TZ_EUROPE_BRUSSELS;
    out->hot_threshold_dc = TUNING_DEFAULT_HOT_THRESHOLD_DC;
    out->cool_threshold_dc = TUNING_DEFAULT_COOL_THRESHOLD_DC;
    out->check_time_count = TUNING_DEFAULT_CHECK_COUNT;
    out->check_times_min[0] = (uint16_t)TUNING_DEFAULT_CHECK_1_MIN;
    out->check_times_min[1] = (uint16_t)TUNING_DEFAULT_CHECK_2_MIN;
    out->check_times_min[2] = (uint16_t)TUNING_DEFAULT_CHECK_3_MIN;
    out->api_failure_behavior = TUNING_API_FAIL_REQUEST_HOT;
    out->forecast_max_age_s = TUNING_DEFAULT_FORECAST_MAX_AGE_S;
    out->retry_count = (uint8_t)TUNING_MAX_RETRY_COUNT;
    out->cooldown_s = TUNING_DEFAULT_COOLDOWN_S;
    out->override_allowed = true;
    /* latitude/longitude stay 0/0 = UNSET (owner-confirmed default is a
     * later product decision); role ids stay empty. Both block enabling. */
}

void tuning_record_init_state(TuningPolicyRecord *rec)
{
    if (rec == NULL) {
        return;
    }
    memset(rec, 0, sizeof(*rec));
    rec->kind = (uint8_t)TUNING_RECORD_KIND_STATE;
    rec->profile_model_version = (uint16_t)TUNING_PROFILE_MODEL_VERSION;
    tuning_settings_defaults(&rec->settings);
    tuning_climate_state_init(&rec->climate);
    rec->tx.state = TUNING_TX_IDLE;
}

void tuning_record_init_tombstone(TuningPolicyRecord *rec)
{
    if (rec == NULL) {
        return;
    }
    memset(rec, 0, sizeof(*rec));
    rec->kind = (uint8_t)TUNING_RECORD_KIND_TOMBSTONE;
    /* Everything else stays zero: a tombstone carries NO settings, NO
     * transaction, NO override and NO epochs. */
}

TuningRecordError tuning_record_finalize_transaction(TuningPolicyRecord *rec)
{
    if (rec == NULL) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    if (rec->kind != (uint8_t)TUNING_RECORD_KIND_STATE) {
        return TUNING_RECORD_ERR_BAD_KIND;
    }
    if (rec->tx.state == TUNING_TX_IDLE) {
        return TUNING_RECORD_OK; /* idempotent */
    }
    if (rec->tx.state != TUNING_TX_COMMITTED) {
        /* Pending, rollback and recovery transactions are preserved
         * evidence — finalization never destroys them. */
        return TUNING_RECORD_ERR_TX_INVALID;
    }
    /* Canonicalize ONLY the transaction subrecord; every other durable
     * safety fact (settings, climate, last-known-safe, override, cooldown,
     * epoch floor, record-level counters) is untouched by construction. */
    memset(&rec->tx, 0, sizeof(rec->tx));
    rec->tx.state = TUNING_TX_IDLE;
    return TUNING_RECORD_OK;
}

/* ------------------------------------------------------------------ */
/* Settings validation                                                 */
/* ------------------------------------------------------------------ */

TuningRecordError tuning_settings_validate(const TuningPolicySettings *s)
{
    if (s == NULL) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    if ((unsigned)s->provider >= TUNING_PROVIDER__COUNT ||
        (unsigned)s->timezone >= TUNING_TZ__COUNT ||
        (unsigned)s->api_failure_behavior >= TUNING_API_FAIL__COUNT) {
        return TUNING_RECORD_ERR_BAD_ENUM;
    }
    if (!model_str_ok(s->location_label, sizeof(s->location_label),
                      TUNING_LOCATION_LABEL_MAX - 1u)) {
        return TUNING_RECORD_ERR_BAD_STRING;
    }
    if (s->latitude_e4 < -TUNING_LATITUDE_E4_MAX ||
        s->latitude_e4 > TUNING_LATITUDE_E4_MAX ||
        s->longitude_e4 < -TUNING_LONGITUDE_E4_MAX ||
        s->longitude_e4 > TUNING_LONGITUDE_E4_MAX) {
        return TUNING_RECORD_ERR_SETTINGS_INVALID;
    }
    {
        TuningClimateThresholds th = {
            .hot_threshold_dc = s->hot_threshold_dc,
            .cool_threshold_dc = s->cool_threshold_dc,
        };
        if (!tuning_climate_thresholds_valid(&th)) {
            return TUNING_RECORD_ERR_SETTINGS_INVALID;
        }
    }
    if (s->check_time_count > TUNING_MAX_DAILY_CHECKS) {
        return TUNING_RECORD_ERR_SETTINGS_INVALID;
    }
    for (uint8_t i = 0; i < TUNING_MAX_DAILY_CHECKS; i++) {
        if (i < s->check_time_count) {
            if (s->check_times_min[i] >= TUNING_MINUTES_PER_DAY) {
                return TUNING_RECORD_ERR_SETTINGS_INVALID;
            }
            if (i > 0 && s->check_times_min[i] <= s->check_times_min[i - 1u]) {
                return TUNING_RECORD_ERR_SETTINGS_INVALID; /* sorted, no dups */
            }
        } else if (s->check_times_min[i] != 0u) {
            return TUNING_RECORD_ERR_SETTINGS_INVALID; /* canonical zeros */
        }
    }
    if (!model_id_ok(s->cool_day_profile_id, sizeof(s->cool_day_profile_id)) ||
        !model_id_ok(s->hot_day_profile_id, sizeof(s->hot_day_profile_id)) ||
        !model_id_ok(s->emergency_profile_id, sizeof(s->emergency_profile_id))) {
        return TUNING_RECORD_ERR_BAD_STRING;
    }
    if (s->retry_count > TUNING_MAX_RETRY_COUNT) {
        return TUNING_RECORD_ERR_SETTINGS_INVALID;
    }
    if (s->forecast_max_age_s < TUNING_FORECAST_MAX_AGE_MIN_S ||
        s->forecast_max_age_s > TUNING_FORECAST_MAX_AGE_MAX_S) {
        return TUNING_RECORD_ERR_SETTINGS_INVALID;
    }
    if (s->cooldown_s < TUNING_COOLDOWN_MIN_S ||
        s->cooldown_s > TUNING_COOLDOWN_MAX_S) {
        return TUNING_RECORD_ERR_SETTINGS_INVALID;
    }
    return TUNING_RECORD_OK;
}

TuningEnableCheckResult tuning_settings_enable_check(const TuningPolicySettings *s,
                                                     const TuningProfile *profiles,
                                                     size_t profile_count,
                                                     const TuningHardwareContext *hw)
{
    if (s == NULL || profiles == NULL || profile_count == 0 || hw == NULL) {
        return TUNING_ENABLE_ERR_NULL;
    }
    if (tuning_settings_validate(s) != TUNING_RECORD_OK) {
        return TUNING_ENABLE_ERR_SETTINGS_INVALID;
    }
    if (s->provider == TUNING_PROVIDER_UNCONFIGURED) {
        /* Enabling requires an EXPLICITLY configured provider. */
        return TUNING_ENABLE_ERR_PROVIDER;
    }
    if (s->timezone == TUNING_TZ_UNSPECIFIED) {
        return TUNING_ENABLE_ERR_TIMEZONE;
    }
    if (s->latitude_e4 == 0 && s->longitude_e4 == 0) {
        return TUNING_ENABLE_ERR_COORDINATES; /* unset */
    }
    if (s->check_time_count == 0) {
        return TUNING_ENABLE_ERR_NO_CHECK_TIMES;
    }
    {
        const char *roles[3] = {
            s->cool_day_profile_id,
            s->hot_day_profile_id,
            s->emergency_profile_id,
        };
        for (size_t i = 0; i < 3; i++) {
            const TuningProfile *p;
            if (roles[i][0] == '\0') {
                return TUNING_ENABLE_ERR_ROLE_MISSING;
            }
            p = tuning_registry_find(profiles, profile_count, roles[i]);
            if (p == NULL) {
                return TUNING_ENABLE_ERR_ROLE_UNKNOWN;
            }
            if (tuning_profile_auto_eligible(p, hw) != TUNING_ELIGIBLE_OK) {
                /* UNVALIDATED / retired / disabled / incompatible: the
                 * feature cannot be enabled (task-brief rule). */
                return TUNING_ENABLE_ERR_ROLE_NOT_ELIGIBLE;
            }
        }
    }
    return TUNING_ENABLE_OK;
}

/* ------------------------------------------------------------------ */
/* Transaction validation                                              */
/* ------------------------------------------------------------------ */

static bool tx_state_is_active(TuningTxState st)
{
    switch (st) {
    case TUNING_TX_INTENT_PERSISTED:
    case TUNING_TX_APPLY_PENDING:
    case TUNING_TX_APPLYING:
    case TUNING_TX_RESTART_PENDING:
    case TUNING_TX_VERIFYING:
    case TUNING_TX_ROLLBACK_PENDING:
    case TUNING_TX_ROLLING_BACK:
        return true;
    default:
        return false;
    }
}

TuningRecordError tuning_transaction_validate(const TuningTransaction *tx)
{
    if (tx == NULL) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    if ((unsigned)tx->state >= TUNING_TX__COUNT) {
        return TUNING_RECORD_ERR_BAD_ENUM;
    }
    if ((unsigned)tx->actor >= TUNING_ACTOR__COUNT ||
        (unsigned)tx->reason >= TUNING_REASON__COUNT) {
        return TUNING_RECORD_ERR_BAD_ENUM;
    }
    if (tx->boot_attempt_count > TUNING_TX_BOOT_ATTEMPT_STORE_MAX ||
        tx->apply_attempt_count > TUNING_TX_APPLY_ATTEMPT_STORE_MAX) {
        return TUNING_RECORD_ERR_COUNTER_RANGE;
    }
    if (!model_id_ok(tx->previous_profile_id, sizeof(tx->previous_profile_id)) ||
        !model_id_ok(tx->requested_profile_id, sizeof(tx->requested_profile_id)) ||
        !model_id_ok(tx->rollback_profile_id, sizeof(tx->rollback_profile_id))) {
        return TUNING_RECORD_ERR_BAD_STRING;
    }
    if (!epoch_zero_or_band(tx->started_epoch_s) ||
        !epoch_zero_or_band(tx->updated_epoch_s)) {
        return TUNING_RECORD_ERR_BAD_EPOCH;
    }
    if (tx->started_epoch_s != 0 && tx->updated_epoch_s != 0 &&
        tx->updated_epoch_s < tx->started_epoch_s) {
        return TUNING_RECORD_ERR_TX_INVALID;
    }

    if (tx->state == TUNING_TX_IDLE) {
        /* Fully canonical-zero: no covert transaction evidence. */
        if (tx->previous_profile_id[0] != '\0' ||
            tx->requested_profile_id[0] != '\0' ||
            tx->rollback_profile_id[0] != '\0' ||
            tx->actor != TUNING_ACTOR_NONE ||
            tx->reason != TUNING_REASON_NONE ||
            tx->policy_generation != 0u || tx->profile_revision != 0u ||
            tx->boot_attempt_count != 0u || tx->apply_attempt_count != 0u ||
            tx->started_epoch_s != 0ull || tx->updated_epoch_s != 0ull) {
            return TUNING_RECORD_ERR_TX_INVALID;
        }
        return TUNING_RECORD_OK;
    }

    if (tx_state_is_active(tx->state) || tx->state == TUNING_TX_COMMITTED) {
        if (tx->requested_profile_id[0] == '\0') {
            return TUNING_RECORD_ERR_TX_INVALID;
        }
        if (tx->actor == TUNING_ACTOR_NONE) {
            return TUNING_RECORD_ERR_TX_INVALID;
        }
        if (tx->profile_revision == 0u) {
            return TUNING_RECORD_ERR_TX_INVALID;
        }
    }
    /* RECOVERY_REQUIRED: evidence preserved as-recorded — only the global
     * bounds above apply. */
    return TUNING_RECORD_OK;
}

/* ------------------------------------------------------------------ */
/* Record validation                                                   */
/* ------------------------------------------------------------------ */

static TuningRecordError validate_state_payload(const TuningPolicyRecord *rec)
{
    TuningRecordError err;

    if (rec->profile_model_version != (uint16_t)TUNING_PROFILE_MODEL_VERSION) {
        return TUNING_RECORD_ERR_UNSUPPORTED_MODEL;
    }
    err = tuning_settings_validate(&rec->settings);
    if (err != TUNING_RECORD_OK) {
        return err;
    }
    if ((unsigned)rec->climate.state >= TUNING_WEATHER_STATE__COUNT) {
        return TUNING_RECORD_ERR_CLIMATE_INVALID;
    }
    if (!rec->climate.last_forecast_valid) {
        if (rec->climate.last_forecast_max_dc != 0) {
            return TUNING_RECORD_ERR_CLIMATE_INVALID; /* canonical zero */
        }
    } else {
        if (rec->climate.last_forecast_max_dc < TUNING_FORECAST_SANITY_MIN_DC ||
            rec->climate.last_forecast_max_dc > TUNING_FORECAST_SANITY_MAX_DC) {
            return TUNING_RECORD_ERR_CLIMATE_INVALID;
        }
    }
    err = tuning_transaction_validate(&rec->tx);
    if (err != TUNING_RECORD_OK) {
        return err;
    }
    if (!model_id_ok(rec->last_known_safe_id, sizeof(rec->last_known_safe_id))) {
        return TUNING_RECORD_ERR_LKS_INVALID;
    }
    if (rec->last_known_safe_id[0] == '\0') {
        if (rec->last_known_safe_revision != 0u) {
            return TUNING_RECORD_ERR_LKS_INVALID;
        }
    } else if (rec->last_known_safe_revision == 0u) {
        return TUNING_RECORD_ERR_LKS_INVALID;
    }

    {
        const TuningManualOverride *ov = &rec->override_state;
        if (!ov->active) {
            if (ov->profile_id[0] != '\0' || ov->actor != TUNING_ACTOR_NONE ||
                ov->created_epoch_s != 0ull || ov->expires_epoch_s != 0ull ||
                ov->monotonic_expiry_us != 0ull || ov->reason_code != 0u ||
                ov->policy_generation != 0u) {
                return TUNING_RECORD_ERR_OVERRIDE_INVALID; /* canonical zero */
            }
        } else {
            if (!model_id_ok(ov->profile_id, sizeof(ov->profile_id)) ||
                ov->profile_id[0] == '\0') {
                return TUNING_RECORD_ERR_OVERRIDE_INVALID;
            }
            if (ov->actor != TUNING_ACTOR_MANUAL_API &&
                ov->actor != TUNING_ACTOR_OPERATOR_RECOVERY) {
                return TUNING_RECORD_ERR_OVERRIDE_INVALID;
            }
            if (ov->created_epoch_s < TUNING_RECORD_EPOCH_MIN_S ||
                ov->created_epoch_s > TUNING_RECORD_EPOCH_MAX_S ||
                ov->expires_epoch_s < TUNING_RECORD_EPOCH_MIN_S ||
                ov->expires_epoch_s > TUNING_RECORD_EPOCH_MAX_S ||
                ov->expires_epoch_s <= ov->created_epoch_s) {
                return TUNING_RECORD_ERR_OVERRIDE_INVALID;
            }
            if (ov->monotonic_expiry_us != 0ull) {
                /* The per-boot monotonic anchor is NEVER persisted. */
                return TUNING_RECORD_ERR_OVERRIDE_INVALID;
            }
        }
    }

    if (rec->cooldown_until_epoch_s == 0ull) {
        if (rec->cooldown_source != TUNING_ACTOR_NONE) {
            return TUNING_RECORD_ERR_COOLDOWN_INVALID;
        }
    } else {
        if (rec->cooldown_until_epoch_s < TUNING_RECORD_EPOCH_MIN_S ||
            rec->cooldown_until_epoch_s > TUNING_RECORD_EPOCH_MAX_S) {
            return TUNING_RECORD_ERR_COOLDOWN_INVALID;
        }
        if (rec->cooldown_source == TUNING_ACTOR_NONE ||
            (unsigned)rec->cooldown_source >= TUNING_ACTOR__COUNT) {
            return TUNING_RECORD_ERR_COOLDOWN_INVALID;
        }
    }

    if (!epoch_zero_or_band(rec->latest_trusted_epoch_s)) {
        return TUNING_RECORD_ERR_BAD_EPOCH;
    }
    if (rec->consecutive_recovery_failures > TUNING_CONSECUTIVE_FAILURE_STORE_MAX) {
        return TUNING_RECORD_ERR_COUNTER_RANGE;
    }
    return TUNING_RECORD_OK;
}

/* Explicit canonical-zero check for tombstones (field by field — never a
 * struct memcmp, padding is not a contract). */
static bool tombstone_payload_zero(const TuningPolicyRecord *rec)
{
    const TuningPolicySettings *s = &rec->settings;
    const TuningTransaction *tx = &rec->tx;
    const TuningManualOverride *ov = &rec->override_state;

    if (rec->profile_model_version != 0u) {
        return false;
    }
    if (s->enabled || s->provider != TUNING_PROVIDER_UNCONFIGURED ||
        s->location_label[0] != '\0' || s->latitude_e4 != 0 ||
        s->longitude_e4 != 0 || s->timezone != TUNING_TZ_UNSPECIFIED ||
        s->hot_threshold_dc != 0 || s->cool_threshold_dc != 0 ||
        s->check_time_count != 0u ||
        s->cool_day_profile_id[0] != '\0' || s->hot_day_profile_id[0] != '\0' ||
        s->emergency_profile_id[0] != '\0' ||
        s->api_failure_behavior != TUNING_API_FAIL_REQUEST_HOT /* == 0 */ ||
        s->forecast_max_age_s != 0u || s->retry_count != 0u ||
        s->cooldown_s != 0u || s->override_allowed) {
        return false;
    }
    for (uint8_t i = 0; i < TUNING_MAX_DAILY_CHECKS; i++) {
        if (s->check_times_min[i] != 0u) {
            return false;
        }
    }
    if (rec->climate.state != TUNING_WEATHER_STATE_UNKNOWN ||
        rec->climate.last_forecast_valid ||
        rec->climate.last_forecast_max_dc != 0 ||
        rec->climate.transition_count != 0u) {
        return false;
    }
    if (tx->state != TUNING_TX_IDLE || tx->previous_profile_id[0] != '\0' ||
        tx->requested_profile_id[0] != '\0' || tx->rollback_profile_id[0] != '\0' ||
        tx->actor != TUNING_ACTOR_NONE || tx->reason != TUNING_REASON_NONE ||
        tx->policy_generation != 0u || tx->profile_revision != 0u ||
        tx->boot_attempt_count != 0u || tx->apply_attempt_count != 0u ||
        tx->started_epoch_s != 0ull || tx->updated_epoch_s != 0ull) {
        return false;
    }
    if (rec->last_known_safe_id[0] != '\0' || rec->last_known_safe_revision != 0u) {
        return false;
    }
    if (ov->active || ov->profile_id[0] != '\0' || ov->actor != TUNING_ACTOR_NONE ||
        ov->created_epoch_s != 0ull || ov->expires_epoch_s != 0ull ||
        ov->monotonic_expiry_us != 0ull || ov->reason_code != 0u ||
        ov->policy_generation != 0u) {
        return false;
    }
    if (rec->cooldown_source != TUNING_ACTOR_NONE ||
        rec->cooldown_until_epoch_s != 0ull ||
        rec->latest_trusted_epoch_s != 0ull ||
        rec->consecutive_recovery_failures != 0u) {
        return false;
    }
    return true;
}

TuningRecordError tuning_record_validate(const TuningPolicyRecord *rec)
{
    if (rec == NULL) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    if (rec->kind == (uint8_t)TUNING_RECORD_KIND_STATE) {
        return validate_state_payload(rec);
    }
    if (rec->kind == (uint8_t)TUNING_RECORD_KIND_TOMBSTONE) {
        return tombstone_payload_zero(rec) ? TUNING_RECORD_OK
                                           : TUNING_RECORD_ERR_TOMBSTONE_NOT_EMPTY;
    }
    return TUNING_RECORD_ERR_BAD_KIND;
}

/* ------------------------------------------------------------------ */
/* Record encoder                                                      */
/* ------------------------------------------------------------------ */

TuningRecordError tuning_record_encode(const TuningPolicyRecord *rec,
                                       uint8_t *buf, size_t cap, size_t *out_len)
{
    TuningRecordError err;
    WCur w;
    size_t payload_len;
    uint32_t crc;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (rec == NULL || buf == NULL || out_len == NULL) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    err = tuning_record_validate(rec);
    if (err != TUNING_RECORD_OK) {
        return err;
    }
    if (rec->generation == 0u) {
        return TUNING_RECORD_ERR_BAD_GENERATION; /* the store assigns >= 1 */
    }
    if (cap < TUNING_RECORD_HEADER_LEN + TUNING_RECORD_CRC_LEN) {
        return TUNING_RECORD_ERR_BUFFER_TOO_SMALL;
    }

    w.buf = buf;
    w.cap = (cap < TUNING_RECORD_MAX_ENCODED) ? cap : TUNING_RECORD_MAX_ENCODED;
    w.off = TUNING_RECORD_HEADER_LEN; /* payload first; header patched below */
    w.overflow = false;

    if (rec->kind == (uint8_t)TUNING_RECORD_KIND_STATE) {
        const TuningPolicySettings *s = &rec->settings;
        const TuningTransaction *tx = &rec->tx;
        const TuningManualOverride *ov = &rec->override_state;

        w_u16(&w, rec->profile_model_version);

        /* settings */
        w_u8(&w, s->enabled ? 1u : 0u);
        w_u8(&w, (uint8_t)s->provider);
        w_str8(&w, s->location_label, TUNING_LOCATION_LABEL_MAX - 1u);
        w_u32(&w, (uint32_t)s->latitude_e4);
        w_u32(&w, (uint32_t)s->longitude_e4);
        w_u8(&w, (uint8_t)s->timezone);
        w_u16(&w, (uint16_t)s->hot_threshold_dc);
        w_u16(&w, (uint16_t)s->cool_threshold_dc);
        w_u8(&w, s->check_time_count);
        for (uint8_t i = 0; i < TUNING_MAX_DAILY_CHECKS; i++) {
            w_u16(&w, s->check_times_min[i]);
        }
        w_str8(&w, s->cool_day_profile_id, TUNING_PROFILE_ID_MAX - 1u);
        w_str8(&w, s->hot_day_profile_id, TUNING_PROFILE_ID_MAX - 1u);
        w_str8(&w, s->emergency_profile_id, TUNING_PROFILE_ID_MAX - 1u);
        w_u8(&w, (uint8_t)s->api_failure_behavior);
        w_u32(&w, s->forecast_max_age_s);
        w_u8(&w, s->retry_count);
        w_u32(&w, s->cooldown_s);
        w_u8(&w, s->override_allowed ? 1u : 0u);

        /* climate */
        w_u8(&w, (uint8_t)rec->climate.state);
        w_u8(&w, rec->climate.last_forecast_valid ? 1u : 0u);
        w_u16(&w, (uint16_t)rec->climate.last_forecast_max_dc);
        w_u32(&w, rec->climate.transition_count);

        /* transaction */
        w_u8(&w, (uint8_t)tx->state);
        w_str8(&w, tx->previous_profile_id, TUNING_PROFILE_ID_MAX - 1u);
        w_str8(&w, tx->requested_profile_id, TUNING_PROFILE_ID_MAX - 1u);
        w_str8(&w, tx->rollback_profile_id, TUNING_PROFILE_ID_MAX - 1u);
        w_u8(&w, (uint8_t)tx->actor);
        w_u8(&w, (uint8_t)tx->reason);
        w_u32(&w, tx->policy_generation);
        w_u16(&w, tx->profile_revision);
        w_u8(&w, tx->boot_attempt_count);
        w_u8(&w, tx->apply_attempt_count);
        w_u64(&w, tx->started_epoch_s);
        w_u64(&w, tx->updated_epoch_s);

        /* last known safe */
        w_str8(&w, rec->last_known_safe_id, TUNING_PROFILE_ID_MAX - 1u);
        w_u16(&w, rec->last_known_safe_revision);

        /* manual override (monotonic anchor deliberately NOT encoded) */
        w_u8(&w, ov->active ? 1u : 0u);
        w_str8(&w, ov->profile_id, TUNING_PROFILE_ID_MAX - 1u);
        w_u8(&w, (uint8_t)ov->actor);
        w_u64(&w, ov->created_epoch_s);
        w_u64(&w, ov->expires_epoch_s);
        w_u16(&w, ov->reason_code);
        w_u32(&w, ov->policy_generation);

        /* cooldown + floor + counters */
        w_u8(&w, (uint8_t)rec->cooldown_source);
        w_u64(&w, rec->cooldown_until_epoch_s);
        w_u64(&w, rec->latest_trusted_epoch_s);
        w_u8(&w, rec->consecutive_recovery_failures);
    }
    /* TOMBSTONE: payload intentionally empty. */

    if (w.overflow) {
        return TUNING_RECORD_ERR_BUFFER_TOO_SMALL;
    }
    payload_len = w.off - TUNING_RECORD_HEADER_LEN;
    if (w.off + TUNING_RECORD_CRC_LEN > w.cap) {
        return TUNING_RECORD_ERR_BUFFER_TOO_SMALL;
    }

    /* Header (fixed 24 bytes, little-endian, no implicit padding). */
    store_u32_at(buf, 0u, TUNING_RECORD_MAGIC);
    store_u16_at(buf, 4u, (uint16_t)TUNING_RECORD_SCHEMA_VERSION);
    store_u16_at(buf, 6u, (uint16_t)TUNING_RECORD_HEADER_LEN);
    store_u32_at(buf, 8u, (uint32_t)(w.off + TUNING_RECORD_CRC_LEN)); /* total */
    store_u32_at(buf, 12u, rec->generation);
    buf[16] = rec->kind;
    buf[17] = 0u;               /* flags: none defined in v1 */
    store_u16_at(buf, 18u, 0u); /* reserved: must be zero    */
    store_u32_at(buf, 20u, (uint32_t)payload_len);

    /* CRC over every encoded byte except the CRC field itself. */
    crc = tuning_record_crc32(buf, w.off);
    store_u32_at(buf, w.off, crc);
    *out_len = w.off + TUNING_RECORD_CRC_LEN;
    return TUNING_RECORD_OK;
}

/* ------------------------------------------------------------------ */
/* Record decoder                                                      */
/* ------------------------------------------------------------------ */

TuningRecordError tuning_record_decode(const uint8_t *buf, size_t len,
                                       TuningPolicyRecord *out)
{
    uint32_t magic, total_len, payload_len, stored_crc, calc_crc, generation;
    uint16_t schema, header_len;
    uint8_t kind;
    TuningRecordError err;

    if (out == NULL) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (buf == NULL) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    if (len < TUNING_RECORD_HEADER_LEN + TUNING_RECORD_CRC_LEN) {
        return TUNING_RECORD_ERR_TRUNCATED;
    }
    if (len > TUNING_RECORD_MAX_ENCODED) {
        return TUNING_RECORD_ERR_BAD_LENGTH;
    }

    magic = load_u32_at(buf, 0u);
    schema = load_u16_at(buf, 4u);
    header_len = load_u16_at(buf, 6u);
    total_len = load_u32_at(buf, 8u);
    generation = load_u32_at(buf, 12u);
    kind = buf[16];
    if (magic != TUNING_RECORD_MAGIC) {
        return TUNING_RECORD_ERR_BAD_MAGIC;
    }
    if (schema != (uint16_t)TUNING_RECORD_SCHEMA_VERSION) {
        return TUNING_RECORD_ERR_UNSUPPORTED_SCHEMA;
    }
    if (header_len != (uint16_t)TUNING_RECORD_HEADER_LEN) {
        return TUNING_RECORD_ERR_BAD_LENGTH;
    }
    if (total_len != (uint32_t)len) {
        return (total_len < (uint32_t)len) ? TUNING_RECORD_ERR_TRAILING_BYTES
                                           : TUNING_RECORD_ERR_TRUNCATED;
    }
    if (buf[17] != 0u || load_u16_at(buf, 18u) != 0u) {
        return TUNING_RECORD_ERR_UNKNOWN_FLAGS;
    }
    payload_len = load_u32_at(buf, 20u);
    if ((size_t)payload_len !=
        len - TUNING_RECORD_HEADER_LEN - TUNING_RECORD_CRC_LEN) {
        return TUNING_RECORD_ERR_BAD_LENGTH;
    }
    stored_crc = load_u32_at(buf, len - TUNING_RECORD_CRC_LEN);
    calc_crc = tuning_record_crc32(buf, len - TUNING_RECORD_CRC_LEN);
    if (stored_crc != calc_crc) {
        return TUNING_RECORD_ERR_CRC_MISMATCH;
    }
    if (generation == 0u) {
        return TUNING_RECORD_ERR_BAD_GENERATION;
    }
    if (kind != (uint8_t)TUNING_RECORD_KIND_STATE &&
        kind != (uint8_t)TUNING_RECORD_KIND_TOMBSTONE) {
        return TUNING_RECORD_ERR_BAD_KIND;
    }
    out->kind = kind;
    out->generation = generation;

    if (kind == (uint8_t)TUNING_RECORD_KIND_TOMBSTONE) {
        if (payload_len != 0u) {
            return TUNING_RECORD_ERR_TOMBSTONE_NOT_EMPTY;
        }
        return TUNING_RECORD_OK; /* zeroed payload model */
    }

    {
        RCur r;
        TuningPolicySettings *s = &out->settings;
        TuningTransaction *tx = &out->tx;
        TuningManualOverride *ov = &out->override_state;
        uint8_t e8;

        r.buf = buf + TUNING_RECORD_HEADER_LEN;
        r.len = (size_t)payload_len;
        r.off = 0;
        r.fail = false;

        out->profile_model_version = r_u16(&r);

        /* settings */
        err = r_bool(&r, &s->enabled);
        if (err != TUNING_RECORD_OK) { return err; }
        err = r_enum8(&r, TUNING_PROVIDER__COUNT, &e8);
        if (err != TUNING_RECORD_OK) { return err; }
        s->provider = (TuningWeatherProvider)e8;
        err = r_str8(&r, s->location_label, sizeof(s->location_label),
                     TUNING_LOCATION_LABEL_MAX - 1u);
        if (err != TUNING_RECORD_OK) { return err; }
        s->latitude_e4 = (int32_t)r_u32(&r);
        s->longitude_e4 = (int32_t)r_u32(&r);
        err = r_enum8(&r, TUNING_TZ__COUNT, &e8);
        if (err != TUNING_RECORD_OK) { return err; }
        s->timezone = (TuningTimezoneId)e8;
        s->hot_threshold_dc = (int16_t)r_u16(&r);
        s->cool_threshold_dc = (int16_t)r_u16(&r);
        s->check_time_count = r_u8(&r);
        for (uint8_t i = 0; i < TUNING_MAX_DAILY_CHECKS; i++) {
            s->check_times_min[i] = r_u16(&r);
        }
        err = r_str8(&r, s->cool_day_profile_id, sizeof(s->cool_day_profile_id),
                     TUNING_PROFILE_ID_MAX - 1u);
        if (err != TUNING_RECORD_OK) { return err; }
        err = r_str8(&r, s->hot_day_profile_id, sizeof(s->hot_day_profile_id),
                     TUNING_PROFILE_ID_MAX - 1u);
        if (err != TUNING_RECORD_OK) { return err; }
        err = r_str8(&r, s->emergency_profile_id, sizeof(s->emergency_profile_id),
                     TUNING_PROFILE_ID_MAX - 1u);
        if (err != TUNING_RECORD_OK) { return err; }
        err = r_enum8(&r, TUNING_API_FAIL__COUNT, &e8);
        if (err != TUNING_RECORD_OK) { return err; }
        s->api_failure_behavior = (TuningApiFailureBehavior)e8;
        s->forecast_max_age_s = r_u32(&r);
        s->retry_count = r_u8(&r);
        s->cooldown_s = r_u32(&r);
        err = r_bool(&r, &s->override_allowed);
        if (err != TUNING_RECORD_OK) { return err; }

        /* climate */
        err = r_enum8(&r, TUNING_WEATHER_STATE__COUNT, &e8);
        if (err != TUNING_RECORD_OK) { return err; }
        out->climate.state = (TuningWeatherState)e8;
        err = r_bool(&r, &out->climate.last_forecast_valid);
        if (err != TUNING_RECORD_OK) { return err; }
        out->climate.last_forecast_max_dc = (int16_t)r_u16(&r);
        out->climate.transition_count = r_u32(&r);

        /* transaction */
        err = r_enum8(&r, TUNING_TX__COUNT, &e8);
        if (err != TUNING_RECORD_OK) { return err; }
        tx->state = (TuningTxState)e8;
        err = r_str8(&r, tx->previous_profile_id, sizeof(tx->previous_profile_id),
                     TUNING_PROFILE_ID_MAX - 1u);
        if (err != TUNING_RECORD_OK) { return err; }
        err = r_str8(&r, tx->requested_profile_id, sizeof(tx->requested_profile_id),
                     TUNING_PROFILE_ID_MAX - 1u);
        if (err != TUNING_RECORD_OK) { return err; }
        err = r_str8(&r, tx->rollback_profile_id, sizeof(tx->rollback_profile_id),
                     TUNING_PROFILE_ID_MAX - 1u);
        if (err != TUNING_RECORD_OK) { return err; }
        err = r_enum8(&r, TUNING_ACTOR__COUNT, &e8);
        if (err != TUNING_RECORD_OK) { return err; }
        tx->actor = (TuningActorClass)e8;
        err = r_enum8(&r, TUNING_REASON__COUNT, &e8);
        if (err != TUNING_RECORD_OK) { return err; }
        tx->reason = (TuningReasonCode)e8;
        tx->policy_generation = r_u32(&r);
        tx->profile_revision = r_u16(&r);
        tx->boot_attempt_count = r_u8(&r);
        tx->apply_attempt_count = r_u8(&r);
        tx->started_epoch_s = r_u64(&r);
        tx->updated_epoch_s = r_u64(&r);

        /* last known safe */
        err = r_str8(&r, out->last_known_safe_id, sizeof(out->last_known_safe_id),
                     TUNING_PROFILE_ID_MAX - 1u);
        if (err != TUNING_RECORD_OK) { return err; }
        out->last_known_safe_revision = r_u16(&r);

        /* manual override */
        err = r_bool(&r, &ov->active);
        if (err != TUNING_RECORD_OK) { return err; }
        err = r_str8(&r, ov->profile_id, sizeof(ov->profile_id),
                     TUNING_PROFILE_ID_MAX - 1u);
        if (err != TUNING_RECORD_OK) { return err; }
        err = r_enum8(&r, TUNING_ACTOR__COUNT, &e8);
        if (err != TUNING_RECORD_OK) { return err; }
        ov->actor = (TuningActorClass)e8;
        ov->created_epoch_s = r_u64(&r);
        ov->expires_epoch_s = r_u64(&r);
        ov->reason_code = r_u16(&r);
        ov->policy_generation = r_u32(&r);
        ov->monotonic_expiry_us = 0ull; /* never persisted */

        /* cooldown + floor + counters */
        err = r_enum8(&r, TUNING_ACTOR__COUNT, &e8);
        if (err != TUNING_RECORD_OK) { return err; }
        out->cooldown_source = (TuningActorClass)e8;
        out->cooldown_until_epoch_s = r_u64(&r);
        out->latest_trusted_epoch_s = r_u64(&r);
        out->consecutive_recovery_failures = r_u8(&r);

        if (r.fail) {
            return TUNING_RECORD_ERR_TRUNCATED;
        }
        if (r.off != r.len) {
            return TUNING_RECORD_ERR_TRAILING_BYTES;
        }
    }

    return tuning_record_validate(out);
}

/* ------------------------------------------------------------------ */
/* Active-slot pointer codec                                           */
/* ------------------------------------------------------------------ */

TuningRecordError tuning_record_pointer_encode(const TuningRecordPointer *ptr,
                                               uint8_t *buf, size_t cap,
                                               size_t *out_len)
{
    uint32_t crc;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (ptr == NULL || buf == NULL || out_len == NULL) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    if (cap < TUNING_RECORD_POINTER_LEN) {
        return TUNING_RECORD_ERR_BUFFER_TOO_SMALL;
    }
    if (ptr->slot != TUNING_RECORD_SLOT_A && ptr->slot != TUNING_RECORD_SLOT_B) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    if (ptr->generation == 0u) {
        return TUNING_RECORD_ERR_BAD_GENERATION;
    }
    store_u32_at(buf, 0u, TUNING_RECORD_POINTER_MAGIC);
    store_u16_at(buf, 4u, (uint16_t)TUNING_RECORD_POINTER_VERSION);
    buf[6] = ptr->slot;
    buf[7] = 0u; /* reserved: must be zero */
    store_u32_at(buf, 8u, ptr->generation);
    crc = tuning_record_crc32(buf, 12u);
    store_u32_at(buf, 12u, crc);
    *out_len = TUNING_RECORD_POINTER_LEN;
    return TUNING_RECORD_OK;
}

TuningRecordError tuning_record_pointer_decode(const uint8_t *buf, size_t len,
                                               TuningRecordPointer *out)
{
    uint32_t magic, stored_crc, calc_crc;
    uint16_t version;

    if (out == NULL) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    out->slot = TUNING_RECORD_SLOT_A;
    out->generation = 0u;
    if (buf == NULL) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    if (len < TUNING_RECORD_POINTER_LEN) {
        return TUNING_RECORD_ERR_TRUNCATED;
    }
    if (len > TUNING_RECORD_POINTER_LEN) {
        return TUNING_RECORD_ERR_TRAILING_BYTES;
    }
    magic = load_u32_at(buf, 0u);
    version = load_u16_at(buf, 4u);
    if (magic != TUNING_RECORD_POINTER_MAGIC) {
        return TUNING_RECORD_ERR_BAD_MAGIC;
    }
    if (version != (uint16_t)TUNING_RECORD_POINTER_VERSION) {
        return TUNING_RECORD_ERR_UNSUPPORTED_SCHEMA;
    }
    stored_crc = load_u32_at(buf, 12u);
    calc_crc = tuning_record_crc32(buf, 12u);
    if (stored_crc != calc_crc) {
        return TUNING_RECORD_ERR_CRC_MISMATCH;
    }
    if (buf[7] != 0u) {
        return TUNING_RECORD_ERR_UNKNOWN_FLAGS;
    }
    if (buf[6] != TUNING_RECORD_SLOT_A && buf[6] != TUNING_RECORD_SLOT_B) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    out->slot = buf[6];
    out->generation = load_u32_at(buf, 8u);
    if (out->generation == 0u) {
        return TUNING_RECORD_ERR_BAD_GENERATION;
    }
    return TUNING_RECORD_OK;
}

/* ------------------------------------------------------------------ */
/* Trusted-epoch floor + counters                                      */
/* ------------------------------------------------------------------ */

TuningRecordError tuning_record_propose_trusted_epoch(TuningPolicyRecord *rec,
                                                      uint64_t epoch_s)
{
    if (rec == NULL) {
        return TUNING_RECORD_ERR_INVALID_ARGUMENT;
    }
    if (epoch_s < TUNING_RECORD_EPOCH_MIN_S || epoch_s > TUNING_RECORD_EPOCH_MAX_S) {
        return TUNING_RECORD_ERR_BAD_EPOCH;
    }
    if (rec->latest_trusted_epoch_s != 0ull &&
        epoch_s < rec->latest_trusted_epoch_s) {
        return TUNING_RECORD_ERR_EPOCH_REGRESSION; /* the floor never decreases */
    }
    rec->latest_trusted_epoch_s = epoch_s;
    return TUNING_RECORD_OK;
}

uint8_t tuning_record_counter_increment(uint8_t current, uint8_t max)
{
    if (current >= max) {
        return max;
    }
    return (uint8_t)(current + 1u);
}

/* ------------------------------------------------------------------ */
/* Stable machine tokens                                               */
/* ------------------------------------------------------------------ */

const char *tuning_record_error_str(TuningRecordError e)
{
    switch (e) {
    case TUNING_RECORD_OK: return "OK";
    case TUNING_RECORD_ERR_INVALID_ARGUMENT: return "ERR_INVALID_ARGUMENT";
    case TUNING_RECORD_ERR_BUFFER_TOO_SMALL: return "ERR_BUFFER_TOO_SMALL";
    case TUNING_RECORD_ERR_TRUNCATED: return "ERR_TRUNCATED";
    case TUNING_RECORD_ERR_TRAILING_BYTES: return "ERR_TRAILING_BYTES";
    case TUNING_RECORD_ERR_BAD_MAGIC: return "ERR_BAD_MAGIC";
    case TUNING_RECORD_ERR_UNSUPPORTED_SCHEMA: return "ERR_UNSUPPORTED_SCHEMA";
    case TUNING_RECORD_ERR_UNSUPPORTED_MODEL: return "ERR_UNSUPPORTED_MODEL";
    case TUNING_RECORD_ERR_BAD_LENGTH: return "ERR_BAD_LENGTH";
    case TUNING_RECORD_ERR_UNKNOWN_FLAGS: return "ERR_UNKNOWN_FLAGS";
    case TUNING_RECORD_ERR_CRC_MISMATCH: return "ERR_CRC_MISMATCH";
    case TUNING_RECORD_ERR_BAD_GENERATION: return "ERR_BAD_GENERATION";
    case TUNING_RECORD_ERR_BAD_KIND: return "ERR_BAD_KIND";
    case TUNING_RECORD_ERR_BAD_STRING: return "ERR_BAD_STRING";
    case TUNING_RECORD_ERR_BAD_FLAG_BYTE: return "ERR_BAD_FLAG_BYTE";
    case TUNING_RECORD_ERR_BAD_ENUM: return "ERR_BAD_ENUM";
    case TUNING_RECORD_ERR_BAD_EPOCH: return "ERR_BAD_EPOCH";
    case TUNING_RECORD_ERR_EPOCH_REGRESSION: return "ERR_EPOCH_REGRESSION";
    case TUNING_RECORD_ERR_SETTINGS_INVALID: return "ERR_SETTINGS_INVALID";
    case TUNING_RECORD_ERR_TX_INVALID: return "ERR_TX_INVALID";
    case TUNING_RECORD_ERR_CLIMATE_INVALID: return "ERR_CLIMATE_INVALID";
    case TUNING_RECORD_ERR_OVERRIDE_INVALID: return "ERR_OVERRIDE_INVALID";
    case TUNING_RECORD_ERR_COOLDOWN_INVALID: return "ERR_COOLDOWN_INVALID";
    case TUNING_RECORD_ERR_LKS_INVALID: return "ERR_LKS_INVALID";
    case TUNING_RECORD_ERR_TOMBSTONE_NOT_EMPTY: return "ERR_TOMBSTONE_NOT_EMPTY";
    case TUNING_RECORD_ERR_COUNTER_RANGE: return "ERR_COUNTER_RANGE";
    default: return "ERR_UNKNOWN";
    }
}

const char *tuning_tx_state_str(TuningTxState s)
{
    switch (s) {
    case TUNING_TX_IDLE: return "IDLE";
    case TUNING_TX_INTENT_PERSISTED: return "INTENT_PERSISTED";
    case TUNING_TX_APPLY_PENDING: return "APPLY_PENDING";
    case TUNING_TX_APPLYING: return "APPLYING";
    case TUNING_TX_RESTART_PENDING: return "RESTART_PENDING";
    case TUNING_TX_VERIFYING: return "VERIFYING";
    case TUNING_TX_COMMITTED: return "COMMITTED";
    case TUNING_TX_ROLLBACK_PENDING: return "ROLLBACK_PENDING";
    case TUNING_TX_ROLLING_BACK: return "ROLLING_BACK";
    case TUNING_TX_RECOVERY_REQUIRED: return "RECOVERY_REQUIRED";
    default: return "UNKNOWN";
    }
}

const char *tuning_provider_str(TuningWeatherProvider p)
{
    switch (p) {
    case TUNING_PROVIDER_UNCONFIGURED: return "UNCONFIGURED";
    case TUNING_PROVIDER_OPEN_METEO: return "OPEN_METEO";
    default: return "UNKNOWN";
    }
}

const char *tuning_timezone_str(TuningTimezoneId t)
{
    switch (t) {
    case TUNING_TZ_UNSPECIFIED: return "UNSPECIFIED";
    case TUNING_TZ_EUROPE_BRUSSELS: return "EUROPE_BRUSSELS";
    default: return "UNKNOWN";
    }
}

const char *tuning_api_failure_str(TuningApiFailureBehavior b)
{
    switch (b) {
    case TUNING_API_FAIL_REQUEST_HOT: return "REQUEST_HOT";
    case TUNING_API_FAIL_RETAIN_INHIBIT: return "RETAIN_INHIBIT";
    default: return "UNKNOWN";
    }
}

const char *tuning_enable_check_str(TuningEnableCheckResult r)
{
    switch (r) {
    case TUNING_ENABLE_OK: return "OK";
    case TUNING_ENABLE_ERR_NULL: return "ERR_NULL";
    case TUNING_ENABLE_ERR_SETTINGS_INVALID: return "ERR_SETTINGS_INVALID";
    case TUNING_ENABLE_ERR_PROVIDER: return "ERR_PROVIDER";
    case TUNING_ENABLE_ERR_TIMEZONE: return "ERR_TIMEZONE";
    case TUNING_ENABLE_ERR_COORDINATES: return "ERR_COORDINATES";
    case TUNING_ENABLE_ERR_NO_CHECK_TIMES: return "ERR_NO_CHECK_TIMES";
    case TUNING_ENABLE_ERR_ROLE_MISSING: return "ERR_ROLE_MISSING";
    case TUNING_ENABLE_ERR_ROLE_UNKNOWN: return "ERR_ROLE_UNKNOWN";
    case TUNING_ENABLE_ERR_ROLE_NOT_ELIGIBLE: return "ERR_ROLE_NOT_ELIGIBLE";
    default: return "ERR_UNKNOWN";
    }
}
