#ifndef TUNING_RECORD_H_
#define TUNING_RECORD_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "tuning_profile.h"
#include "tuning_policy.h"

/*
 * NeuralAxe Weather-Aware Tuning — persisted policy-state record model and
 * codec (Gate W2).
 *
 * PURE: no NVS, no ESP-IDF, no FreeRTOS, no heap, no logging, no clock
 * reads, no network, no profile application. Field-by-field little-endian
 * encoding — NEVER memcpy of a C struct as a wire format. Invalid persisted
 * data is rejected, never silently normalized (forward policy is
 * reject-to-recovery, not migrate).
 *
 * ONE record carries the whole weather-policy persistent state so a single
 * crash-safe dual-slot commit keeps settings, climate stance, transaction,
 * last-known-safe reference, manual override, cooldown and the trusted-
 * epoch floor mutually consistent:
 *  - versioned weather-policy SETTINGS (feature DISABLED by default; safe
 *    defaults; strict bounds; enabling is additionally gated by
 *    tuning_settings_enable_check, which REFUSES enablement while any role
 *    profile is absent or not auto-eligible — in production ALL profiles
 *    ship UNVALIDATED, so the feature cannot be enabled until the owner
 *    records validation evidence);
 *  - persisted CLIMATE hysteresis stance (Gate W1 TuningClimateState);
 *  - the crash-safe profile TRANSACTION (pending/active/rollback states);
 *  - the LAST-KNOWN-SAFE profile reference (advanced only after verified
 *    local health — a Gate W4 runtime duty);
 *  - MANUAL OVERRIDE persistence (trusted epochs only; the per-boot
 *    monotonic anchor is NEVER persisted and must re-arm each boot);
 *  - COOLDOWN persistence (trusted-epoch end; in-boot enforcement stays
 *    monotonic at runtime);
 *  - the monotonically advancing latest-accepted trusted epoch floor
 *    (B2/B3 anti-regression pattern).
 *
 * SECURITY / PRIVACY INVARIANTS: no password, account, worker, wallet,
 * hostname or URL exists anywhere in this record — only bounded profile
 * ids, enums, scaled integers and epochs. CRC is accidental-corruption
 * detection ONLY — not authentication, anti-tamper or anti-rollback
 * (documented residual on this build: NVS encryption/flash encryption/
 * Secure Boot are all disabled).
 *
 * The wire format pins the Gate W1 enum numbering via the W1 headers'
 * _Static_asserts plus the pins at the bottom of this header: renumbering
 * must break the build, never the flash format. The B3 pool-session
 * namespace and schema are NOT reused (Gate W0 rule): this is a NEW record
 * family with its own magic, schema and namespace.
 */

#define TUNING_RECORD_SCHEMA_VERSION 1u

/* Fixed wire framing (mirrors the proven B3 layout, new magic values). */
#define TUNING_RECORD_MAGIC 0x4E585752u          /* "NXWR" */
#define TUNING_RECORD_HEADER_LEN 24u
#define TUNING_RECORD_CRC_LEN 4u
#define TUNING_RECORD_MAX_ENCODED 512u

#define TUNING_RECORD_POINTER_MAGIC 0x4E585750u  /* "NXWP" */
#define TUNING_RECORD_POINTER_VERSION 1u
#define TUNING_RECORD_POINTER_LEN 16u

/* Record kinds (u8 on the wire; 0 is deliberately invalid). */
typedef enum {
    TUNING_RECORD_KIND_STATE = 1,
    TUNING_RECORD_KIND_TOMBSTONE = 2,
} TuningRecordKind;

/* Dual-slot identifiers (u8 on the wire; 0 is deliberately invalid). */
#define TUNING_RECORD_SLOT_A 1u
#define TUNING_RECORD_SLOT_B 2u

/* Trusted-epoch sanity band (2025-01-01 .. 2100-01-01; equals the B2/B3
 * band by design — asserted below against the values, not the headers). */
#define TUNING_RECORD_EPOCH_MIN_S 1735689600ull
#define TUNING_RECORD_EPOCH_MAX_S 4102444800ull

/* Bounded settings fields. */
#define TUNING_LOCATION_LABEL_MAX 32   /* buffer incl. NUL; strlen <= 31 */
#define TUNING_MAX_DAILY_CHECKS 6
#define TUNING_DEFAULT_CHECK_COUNT 3
#define TUNING_DEFAULT_CHECK_1_MIN 300u  /* 05:00 local */
#define TUNING_DEFAULT_CHECK_2_MIN 660u  /* 11:00 local */
#define TUNING_DEFAULT_CHECK_3_MIN 900u  /* 15:00 local */
#define TUNING_MINUTES_PER_DAY 1440u
#define TUNING_MAX_RETRY_COUNT 2u        /* attempts per scheduled check   */
#define TUNING_FORECAST_MAX_AGE_MIN_S 600u
#define TUNING_FORECAST_MAX_AGE_MAX_S 172800u
#define TUNING_DEFAULT_FORECAST_MAX_AGE_S 21600u /* aligned to provider
                                                    1-6 h model refresh    */
#define TUNING_COOLDOWN_MIN_S 300u
#define TUNING_COOLDOWN_MAX_S 86400u
#define TUNING_DEFAULT_COOLDOWN_S 3600u
/* Coordinate bounds, degrees scaled by 1e4 (owner-confirmed defaults are a
 * later product decision; 0/0 = unset and blocks enablement). */
#define TUNING_LATITUDE_E4_MAX 900000
#define TUNING_LONGITUDE_E4_MAX 1800000

/* Bounded counter storage maxima (policy thresholds live in
 * tuning_recovery.h, mirroring the B3/B4 split). */
#define TUNING_TX_BOOT_ATTEMPT_STORE_MAX 10u
#define TUNING_TX_APPLY_ATTEMPT_STORE_MAX 10u
#define TUNING_CONSECUTIVE_FAILURE_STORE_MAX 10u

/* ------------------------------------------------------------------ */
/* Enumerations (numeric values ARE the wire format — pinned below)    */
/* ------------------------------------------------------------------ */

/*
 * Weather provider identity. UNCONFIGURED (0) is the OPERATIONAL DEFAULT:
 * the Gate W0 commercial/distribution decision for Open-Meteo is
 * unresolved, so no provider is ever an implicit product default.
 * Selecting OPEN_METEO is an explicit future configuration action, and
 * enabling the feature fails while the provider is UNCONFIGURED. No
 * endpoint hostname, URL or API key is persisted anywhere in this schema
 * or component — the enum is a bounded identity, not an endpoint.
 */
typedef enum {
    TUNING_PROVIDER_UNCONFIGURED = 0, /* default; blocks enablement       */
    TUNING_PROVIDER_OPEN_METEO = 1,   /* explicit future configuration    */
    TUNING_PROVIDER__COUNT
} TuningWeatherProvider;

/* Bounded timezone identity — never free text. The product-facing name for
 * EUROPE_BRUSSELS is "Europe/Brussels"; the on-device conversion mechanism
 * (pure UTC rule) is a Gate W3 deliverable. */
typedef enum {
    TUNING_TZ_UNSPECIFIED = 0,      /* invalid when enabled               */
    TUNING_TZ_EUROPE_BRUSSELS = 1,
    TUNING_TZ__COUNT
} TuningTimezoneId;

/* Behavior after the bounded retry budget fails (task-brief default:
 * request the hot-day profile; never an upgrade either way). */
typedef enum {
    TUNING_API_FAIL_REQUEST_HOT = 0,
    TUNING_API_FAIL_RETAIN_INHIBIT = 1,
    TUNING_API_FAIL__COUNT
} TuningApiFailureBehavior;

/* Crash-safe profile-transaction states (task-brief PROFILE_TX_* set).
 * RESTART_PENDING is retained for future restart-requiring fields even
 * though the audited frequency/voltage apply path is live (Gate W0 §5). */
typedef enum {
    TUNING_TX_IDLE = 0,
    TUNING_TX_INTENT_PERSISTED = 1,
    TUNING_TX_APPLY_PENDING = 2,
    TUNING_TX_APPLYING = 3,
    TUNING_TX_RESTART_PENDING = 4,
    TUNING_TX_VERIFYING = 5,
    TUNING_TX_COMMITTED = 6,
    TUNING_TX_ROLLBACK_PENDING = 7,
    TUNING_TX_ROLLING_BACK = 8,
    TUNING_TX_RECOVERY_REQUIRED = 9,
    TUNING_TX__COUNT
} TuningTxState;

/* Codec/validation result codes (bounded machine codes; string fn below). */
typedef enum {
    TUNING_RECORD_OK = 0,
    TUNING_RECORD_ERR_INVALID_ARGUMENT,
    TUNING_RECORD_ERR_BUFFER_TOO_SMALL,
    TUNING_RECORD_ERR_TRUNCATED,
    TUNING_RECORD_ERR_TRAILING_BYTES,
    TUNING_RECORD_ERR_BAD_MAGIC,
    TUNING_RECORD_ERR_UNSUPPORTED_SCHEMA,
    TUNING_RECORD_ERR_UNSUPPORTED_MODEL, /* W1 profile-model version gate  */
    TUNING_RECORD_ERR_BAD_LENGTH,
    TUNING_RECORD_ERR_UNKNOWN_FLAGS,
    TUNING_RECORD_ERR_CRC_MISMATCH,
    TUNING_RECORD_ERR_BAD_GENERATION,
    TUNING_RECORD_ERR_BAD_KIND,
    TUNING_RECORD_ERR_BAD_STRING,
    TUNING_RECORD_ERR_BAD_FLAG_BYTE,
    TUNING_RECORD_ERR_BAD_ENUM,
    TUNING_RECORD_ERR_BAD_EPOCH,
    TUNING_RECORD_ERR_EPOCH_REGRESSION,
    TUNING_RECORD_ERR_SETTINGS_INVALID,
    TUNING_RECORD_ERR_TX_INVALID,
    TUNING_RECORD_ERR_CLIMATE_INVALID,
    TUNING_RECORD_ERR_OVERRIDE_INVALID,
    TUNING_RECORD_ERR_COOLDOWN_INVALID,
    TUNING_RECORD_ERR_LKS_INVALID,
    TUNING_RECORD_ERR_TOMBSTONE_NOT_EMPTY,
    TUNING_RECORD_ERR_COUNTER_RANGE,
    TUNING_RECORD_ERR__COUNT
} TuningRecordError;

/* Enable-check results (semantic gate consulted before setting enabled).  */
typedef enum {
    TUNING_ENABLE_OK = 0,
    TUNING_ENABLE_ERR_NULL,
    TUNING_ENABLE_ERR_SETTINGS_INVALID,
    TUNING_ENABLE_ERR_PROVIDER,
    TUNING_ENABLE_ERR_TIMEZONE,
    TUNING_ENABLE_ERR_COORDINATES,
    TUNING_ENABLE_ERR_NO_CHECK_TIMES,
    TUNING_ENABLE_ERR_ROLE_MISSING,      /* a role id is empty            */
    TUNING_ENABLE_ERR_ROLE_UNKNOWN,      /* id not present in registry    */
    TUNING_ENABLE_ERR_ROLE_NOT_ELIGIBLE, /* unvalidated / retired /
                                            disabled / incompatible       */
    TUNING_ENABLE__COUNT
} TuningEnableCheckResult;

/* ------------------------------------------------------------------ */
/* Models                                                              */
/* ------------------------------------------------------------------ */

/*
 * Versioned weather-policy settings. Defaults are SAFE: feature disabled,
 * provider UNCONFIGURED (no implicit Open-Meteo default — an explicit
 * future configuration action selects it), Brussels timezone (a bounded
 * product default that implies NO provider), 30.0/28.0 C thresholds,
 * 05:00/11:00/15:00 checks, empty role ids, request-hot on API failure,
 * 2 retries, 6 h forecast max age, 1 h cooldown, override allowed.
 * Coordinates default to 0/0 = UNSET (the owner-confirmed product default
 * is a later decision) and block enablement. Weather operation cannot
 * start from the default record. No URL, no API key, no free-text host
 * anywhere.
 */
typedef struct {
    bool enabled;
    TuningWeatherProvider provider;
    char location_label[TUNING_LOCATION_LABEL_MAX]; /* presentation only  */
    int32_t latitude_e4;   /* degrees * 1e4; 0 with longitude 0 = unset   */
    int32_t longitude_e4;
    TuningTimezoneId timezone;
    int16_t hot_threshold_dc;   /* deci-C; validated hot > cool           */
    int16_t cool_threshold_dc;
    uint8_t check_time_count;   /* 0..TUNING_MAX_DAILY_CHECKS             */
    uint16_t check_times_min[TUNING_MAX_DAILY_CHECKS]; /* minutes since
        local midnight; strictly ascending; entries past count must be 0  */
    char cool_day_profile_id[TUNING_PROFILE_ID_MAX];  /* "" = unset       */
    char hot_day_profile_id[TUNING_PROFILE_ID_MAX];
    char emergency_profile_id[TUNING_PROFILE_ID_MAX];
    TuningApiFailureBehavior api_failure_behavior;
    uint32_t forecast_max_age_s;
    uint8_t retry_count;        /* 0..TUNING_MAX_RETRY_COUNT              */
    uint32_t cooldown_s;
    bool override_allowed;
} TuningPolicySettings;

/*
 * Crash-safe profile transaction. IDLE is fully canonical-zero except the
 * state itself; active states carry the requested profile id and bounded
 * evidence. The requested profile is NEVER assumed applied: only the Gate
 * W4 runtime moves last_known_safe after verified local health.
 */
typedef struct {
    TuningTxState state;
    char previous_profile_id[TUNING_PROFILE_ID_MAX]; /* committed before tx */
    char requested_profile_id[TUNING_PROFILE_ID_MAX];
    char rollback_profile_id[TUNING_PROFILE_ID_MAX];
    TuningActorClass actor;
    TuningReasonCode reason;
    uint32_t policy_generation;
    uint16_t profile_revision;  /* requested profile's revision at intent */
    uint8_t boot_attempt_count;   /* <= TUNING_TX_BOOT_ATTEMPT_STORE_MAX  */
    uint8_t apply_attempt_count;  /* <= TUNING_TX_APPLY_ATTEMPT_STORE_MAX */
    uint64_t started_epoch_s;   /* trusted epoch; 0 only when unknown     */
    uint64_t updated_epoch_s;   /* >= started when both nonzero           */
} TuningTransaction;

/*
 * The persisted record. `generation` is store-assigned (>= 1). The manual
 * override is persisted WITHOUT its per-boot monotonic anchor
 * (monotonic_expiry_us must be 0 in any record; the runtime re-arms it
 * after boot from trusted time). A tombstone carries a zero payload.
 */
typedef struct {
    uint8_t kind;               /* TuningRecordKind                       */
    uint32_t generation;        /* store-assigned on commit               */
    uint16_t profile_model_version; /* == TUNING_PROFILE_MODEL_VERSION    */

    TuningPolicySettings settings;
    TuningClimateState climate; /* persisted stance; when
        last_forecast_valid == false, last_forecast_max_dc must be 0      */
    TuningTransaction tx;

    char last_known_safe_id[TUNING_PROFILE_ID_MAX]; /* "" = none yet      */
    uint16_t last_known_safe_revision;              /* 0 iff id empty     */

    TuningManualOverride override_state; /* monotonic_expiry_us == 0      */

    TuningActorClass cooldown_source;    /* NONE iff until == 0           */
    uint64_t cooldown_until_epoch_s;     /* 0 = no cooldown persisted     */

    uint64_t latest_trusted_epoch_s;     /* 0 = never; else in band;
                                            monotonically advancing      */
    uint8_t consecutive_recovery_failures;
} TuningPolicyRecord;

/* Active-slot pointer (small, separately versioned encoding — never a
 * naked integer). */
typedef struct {
    uint8_t slot;        /* TUNING_RECORD_SLOT_A / _B */
    uint32_t generation; /* >= 1                       */
} TuningRecordPointer;

/* ------------------------------------------------------------------ */
/* API (all pure)                                                      */
/* ------------------------------------------------------------------ */

/* Safe defaults (see TuningPolicySettings docs). */
void tuning_settings_defaults(TuningPolicySettings *out);

/* Structural settings validation (bounds, ordering, charsets, canonical
 * zeros). Does NOT consult the registry — see the enable check. */
TuningRecordError tuning_settings_validate(const TuningPolicySettings *s);

/*
 * Semantic enablement gate (task brief: "Enabling must fail validation
 * when required profiles are absent or unvalidated"). Checks, as if the
 * feature were being enabled: provider, timezone, coordinates set and in
 * range, at least one check time, and all three role profiles present in
 * the registry AND auto-eligible (tuning_profile_auto_eligible — which
 * refuses UNVALIDATED/retired/disabled/incompatible). With the production
 * registry this ALWAYS fails today (all profiles UNVALIDATED) — proven by
 * test; synthetic validated fixtures exercise the success path.
 */
TuningEnableCheckResult tuning_settings_enable_check(const TuningPolicySettings *s,
                                                     const TuningProfile *profiles,
                                                     size_t profile_count,
                                                     const TuningHardwareContext *hw);

/* Transaction structural validation (state-dependent canonical rules). */
TuningRecordError tuning_transaction_validate(const TuningTransaction *tx);

/* Initialize a STATE record with safe defaults / a zero-payload tombstone. */
void tuning_record_init_state(TuningPolicyRecord *rec);
void tuning_record_init_tombstone(TuningPolicyRecord *rec);

/*
 * TRANSACTION FINALIZATION (Correction 1A). After a durably verified
 * PROFILE_TX_COMMITTED result, canonicalize ONLY the transaction subrecord
 * to IDLE (all transaction fields become canonical zero) and PRESERVE
 * every other durable safety fact: settings, provider/location
 * configuration, climate hysteresis state, the newly verified
 * last-known-safe profile, trusted-epoch floor, cooldown/override facts
 * and record-level counters. The caller then commits the updated combined
 * record crash-safely through the normal dual-slot path — finalization
 * NEVER tombstones or clears the policy record.
 * Accepts: COMMITTED (finalizes) and IDLE (idempotent no-op). Any other
 * transaction state is preserved evidence and is refused (ERR_TX_INVALID);
 * a tombstone kind is refused (ERR_BAD_KIND).
 */
TuningRecordError tuning_record_finalize_transaction(TuningPolicyRecord *rec);

/* Full semantic validation of a record (either kind). */
TuningRecordError tuning_record_validate(const TuningPolicyRecord *rec);

/*
 * Encode a record to the v1 wire format. Validates first; requires a
 * store-assigned generation >= 1. Field-by-field little-endian encoding
 * with a fixed 24-byte header and CRC-32 over every byte except the CRC
 * field itself. `cap` must be >= TUNING_RECORD_MAX_ENCODED for any valid
 * record to be safe.
 */
TuningRecordError tuning_record_encode(const TuningPolicyRecord *rec,
                                       uint8_t *buf, size_t cap, size_t *out_len);

/* Decode + verify a v1 wire blob (length/magic/schema/model/flag checks,
 * CRC, strict field decoding, trailing-byte rejection, then full semantic
 * validation). *out is zeroed on entry and meaningful only on OK. */
TuningRecordError tuning_record_decode(const uint8_t *buf, size_t len,
                                       TuningPolicyRecord *out);

/* Active-slot pointer codec (same CRC contract). */
TuningRecordError tuning_record_pointer_encode(const TuningRecordPointer *ptr,
                                               uint8_t *buf, size_t cap,
                                               size_t *out_len);
TuningRecordError tuning_record_pointer_decode(const uint8_t *buf, size_t len,
                                               TuningRecordPointer *out);

/*
 * Propose an update of the monotonically-advancing latest-accepted trusted
 * epoch (anti-regression floor; operational, not cryptographic). Accepted
 * only inside the sanity band and never below the current floor (equal is
 * allowed). The write cadence is NOT decided here (state-transition-only —
 * see the store header wear note).
 */
TuningRecordError tuning_record_propose_trusted_epoch(TuningPolicyRecord *rec,
                                                      uint64_t epoch_s);

/* Saturating counter increment (never wraps; clamps at `max`). */
uint8_t tuning_record_counter_increment(uint8_t current, uint8_t max);

/* CRC-32 (IEEE reflected; equals zlib crc32 / esp_rom_crc32_le(0, ...)).
 * Public for tests and equivalence checks — crash-consistency only. */
uint32_t tuning_record_crc32(const void *data, size_t len);

/* Stable machine tokens (dot-free; never carry persisted values). */
const char *tuning_record_error_str(TuningRecordError e);
const char *tuning_tx_state_str(TuningTxState s);
const char *tuning_provider_str(TuningWeatherProvider p);
const char *tuning_timezone_str(TuningTimezoneId t);
const char *tuning_api_failure_str(TuningApiFailureBehavior b);
const char *tuning_enable_check_str(TuningEnableCheckResult r);

/* ------------------------------------------------------------------ */
/* Compile-time guards (wire-format pins)                              */
/* ------------------------------------------------------------------ */

_Static_assert(TUNING_TX_IDLE == 0 && TUNING_TX_INTENT_PERSISTED == 1 &&
               TUNING_TX_APPLY_PENDING == 2 && TUNING_TX_APPLYING == 3 &&
               TUNING_TX_RESTART_PENDING == 4 && TUNING_TX_VERIFYING == 5 &&
               TUNING_TX_COMMITTED == 6 && TUNING_TX_ROLLBACK_PENDING == 7 &&
               TUNING_TX_ROLLING_BACK == 8 && TUNING_TX_RECOVERY_REQUIRED == 9 &&
               TUNING_TX__COUNT == 10,
               "wire format pins the transaction state numbering");
_Static_assert(TUNING_PROVIDER_UNCONFIGURED == 0 &&
               TUNING_PROVIDER_OPEN_METEO == 1 &&
               TUNING_PROVIDER__COUNT == 2, "wire format pins provider ids");
_Static_assert(TUNING_TZ_UNSPECIFIED == 0 && TUNING_TZ_EUROPE_BRUSSELS == 1 &&
               TUNING_TZ__COUNT == 2, "wire format pins timezone ids");
_Static_assert(TUNING_API_FAIL_REQUEST_HOT == 0 &&
               TUNING_API_FAIL_RETAIN_INHIBIT == 1 &&
               TUNING_API_FAIL__COUNT == 2, "wire format pins failure behavior");
_Static_assert(TUNING_RECORD_KIND_STATE == 1 && TUNING_RECORD_KIND_TOMBSTONE == 2,
               "wire format pins record kinds");
_Static_assert(TUNING_RECORD_SLOT_A == 1u && TUNING_RECORD_SLOT_B == 2u,
               "wire format pins slot ids");
_Static_assert(TUNING_RECORD_EPOCH_MIN_S < TUNING_RECORD_EPOCH_MAX_S,
               "epoch sanity band inverted");
_Static_assert(TUNING_RECORD_MAX_ENCODED >= 448u,
               "encode buffer bound below the v1 worst case");
_Static_assert(TUNING_RECORD_ERR__COUNT == 26,
               "codec error count changed — review tokens/tests");
_Static_assert(TUNING_ENABLE__COUNT == 10,
               "enable-check result count changed — review tokens/tests");
/* The W1 model pins this format depends on (values re-asserted here so a
 * W1 renumbering breaks THIS build unit too). */
_Static_assert(TUNING_ACTOR_NONE == 0 && TUNING_ACTOR_MANUAL_API == 6 &&
               TUNING_ACTOR_OPERATOR_RECOVERY == 8 && TUNING_ACTOR__COUNT == 9,
               "wire format pins the W1 actor numbering");
_Static_assert(TUNING_WEATHER_STATE_UNKNOWN == 0 && TUNING_WEATHER_STATE__COUNT == 3,
               "wire format pins the W1 weather-state numbering");
_Static_assert(TUNING_REASON__COUNT == 24,
               "wire format pins the W1 reason range");
_Static_assert(TUNING_PROFILE_ID_MAX == 24,
               "wire format pins the W1 profile-id bound");

#endif /* TUNING_RECORD_H_ */
