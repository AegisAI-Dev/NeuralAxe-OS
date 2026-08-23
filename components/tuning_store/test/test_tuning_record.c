/*
 * Exhaustive deterministic tests for the Gate W2 persisted record model and
 * codec. No hardware, no network, no real NVS here (see the store tests for
 * the fake-backend and real-NVS suites). All ids/labels/coordinates are
 * synthetic fixtures — never real locations or credentials.
 */

#include <string.h>
#include "unity.h"
#include "tuning_record.h"

#define EPOCH_S 1800000000ull /* synthetic trusted epoch (2027) */

/* ---------------- fixtures ---------------- */

static TuningHardwareContext hw_full(void)
{
    TuningHardwareContext hw;
    memset(&hw, 0, sizeof(hw));
    hw.board = TUNING_BOARD_GAMMA_601;
    hw.asic = TUNING_ASIC_BM1370;
    hw.cooling_installed = TUNING_COOLING_SUPERSINK_DUAL_FAN;
    hw.psu_installed = TUNING_PSU_STANDARD;
    return hw;
}

/* Synthetic promoted registry (test-only VALIDATED; production ships
 * UNVALIDATED — proven in the W1 suites and re-proven here). */
static void reg_validated(TuningProfile out[TUNING_REGISTRY_GAMMA601_COUNT])
{
    size_t n = 0;
    const TuningProfile *r = tuning_registry_gamma601(&n);
    memcpy(out, r, sizeof(TuningProfile) * TUNING_REGISTRY_GAMMA601_COUNT);
    for (size_t i = 0; i < TUNING_REGISTRY_GAMMA601_COUNT; i++) {
        out[i].validation = TUNING_VALIDATION_VALIDATED;
        out[i].evidence_fingerprint = 0xE0F0u + (uint32_t)i;
    }
}

/* Settings ready for enablement checks (synthetic coordinates). */
static TuningPolicySettings settings_enableable(void)
{
    TuningPolicySettings s;
    tuning_settings_defaults(&s);
    /* Selecting a provider is an EXPLICIT configuration action — the
     * default is UNCONFIGURED and blocks enabling (tested below). */
    s.provider = TUNING_PROVIDER_OPEN_METEO;
    s.latitude_e4 = 123456;   /* synthetic fixture, not a real location */
    s.longitude_e4 = -654321;
    strncpy(s.cool_day_profile_id, TUNING_PROFILE_ID_SUPERSINK_MAX,
            sizeof(s.cool_day_profile_id) - 1);
    strncpy(s.hot_day_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(s.hot_day_profile_id) - 1);
    strncpy(s.emergency_profile_id, TUNING_PROFILE_ID_EMERGENCY,
            sizeof(s.emergency_profile_id) - 1);
    return s;
}

/* A fully-populated STATE record exercising every encoded field. */
static void rich_record(TuningPolicyRecord *r)
{
    tuning_record_init_state(r);
    r->generation = 1u;
    r->settings = settings_enableable();
    strncpy(r->settings.location_label, "test site 1",
            sizeof(r->settings.location_label) - 1);

    r->climate.state = TUNING_WEATHER_STATE_HOT;
    r->climate.last_forecast_valid = true;
    r->climate.last_forecast_max_dc = 324;
    r->climate.transition_count = 5u;

    r->tx.state = TUNING_TX_VERIFYING;
    strncpy(r->tx.previous_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(r->tx.previous_profile_id) - 1);
    strncpy(r->tx.requested_profile_id, TUNING_PROFILE_ID_SUPERSINK_MAX,
            sizeof(r->tx.requested_profile_id) - 1);
    strncpy(r->tx.rollback_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(r->tx.rollback_profile_id) - 1);
    r->tx.actor = TUNING_ACTOR_WEATHER_POLICY;
    r->tx.reason = TUNING_REASON_WEATHER_COOL_ELIGIBLE;
    r->tx.policy_generation = 9u;
    r->tx.profile_revision = 2u;
    r->tx.boot_attempt_count = 1u;
    r->tx.apply_attempt_count = 2u;
    r->tx.started_epoch_s = EPOCH_S;
    r->tx.updated_epoch_s = EPOCH_S + 60ull;

    strncpy(r->last_known_safe_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(r->last_known_safe_id) - 1);
    r->last_known_safe_revision = 2u;

    r->override_state.active = true;
    strncpy(r->override_state.profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(r->override_state.profile_id) - 1);
    r->override_state.actor = TUNING_ACTOR_MANUAL_API;
    r->override_state.created_epoch_s = EPOCH_S;
    r->override_state.expires_epoch_s = EPOCH_S + 3600ull;
    r->override_state.monotonic_expiry_us = 0ull; /* never persisted */
    r->override_state.reason_code = 7u;
    r->override_state.policy_generation = 9u;

    r->cooldown_source = TUNING_ACTOR_THERMAL;
    r->cooldown_until_epoch_s = EPOCH_S + 7200ull;
    r->latest_trusted_epoch_s = EPOCH_S;
    r->consecutive_recovery_failures = 1u;
}

/* Shared buffers (records are ~0.5 KB; keep off the test task stack). */
static TuningPolicyRecord g_rec;
static TuningPolicyRecord g_out;
static uint8_t g_buf[TUNING_RECORD_MAX_ENCODED];
static uint8_t g_buf2[TUNING_RECORD_MAX_ENCODED];

/* ---------------- defaults ---------------- */

TEST_CASE("settings: defaults are safe, disabled and valid", "[tuning_record]")
{
    TuningPolicySettings s;
    tuning_settings_defaults(&s);
    TEST_ASSERT_FALSE(s.enabled);
    /* NO implicit provider default (W0 commercial decision unresolved);
     * the timezone default is a bounded product fact implying no provider. */
    TEST_ASSERT_EQUAL(TUNING_PROVIDER_UNCONFIGURED, s.provider);
    TEST_ASSERT_EQUAL(TUNING_TZ_EUROPE_BRUSSELS, s.timezone);
    TEST_ASSERT_EQUAL_INT16(300, s.hot_threshold_dc);
    TEST_ASSERT_EQUAL_INT16(280, s.cool_threshold_dc);
    TEST_ASSERT_EQUAL_UINT8(3, s.check_time_count);
    TEST_ASSERT_EQUAL_UINT16(300, s.check_times_min[0]); /* 05:00 */
    TEST_ASSERT_EQUAL_UINT16(660, s.check_times_min[1]); /* 11:00 */
    TEST_ASSERT_EQUAL_UINT16(900, s.check_times_min[2]); /* 15:00 */
    TEST_ASSERT_EQUAL_UINT16(0, s.check_times_min[3]);
    TEST_ASSERT_EQUAL(TUNING_API_FAIL_REQUEST_HOT, s.api_failure_behavior);
    TEST_ASSERT_EQUAL_UINT32(21600, s.forecast_max_age_s);
    TEST_ASSERT_EQUAL_UINT8(2, s.retry_count);
    TEST_ASSERT_EQUAL_UINT32(3600, s.cooldown_s);
    TEST_ASSERT_TRUE(s.override_allowed);
    TEST_ASSERT_EQUAL_INT32(0, s.latitude_e4);
    TEST_ASSERT_EQUAL_INT32(0, s.longitude_e4);
    TEST_ASSERT_EQUAL_STRING("", s.cool_day_profile_id);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_settings_validate(&s));
}

/* ---------------- settings validation ---------------- */

TEST_CASE("settings validate: enum, string and bound matrix", "[tuning_record]")
{
    TuningPolicySettings s;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_INVALID_ARGUMENT, tuning_settings_validate(NULL));

    tuning_settings_defaults(&s);
    s.provider = (TuningWeatherProvider)TUNING_PROVIDER__COUNT;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_ENUM, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.timezone = (TuningTimezoneId)TUNING_TZ__COUNT;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_ENUM, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.api_failure_behavior = (TuningApiFailureBehavior)TUNING_API_FAIL__COUNT;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_ENUM, tuning_settings_validate(&s));

    tuning_settings_defaults(&s);
    memset(s.location_label, 'a', sizeof(s.location_label)); /* unterminated */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_STRING, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.location_label[0] = (char)0x01; /* control char */
    s.location_label[1] = '\0';
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_STRING, tuning_settings_validate(&s));

    tuning_settings_defaults(&s);
    s.latitude_e4 = TUNING_LATITUDE_E4_MAX + 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.longitude_e4 = -(TUNING_LONGITUDE_E4_MAX + 1);
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));

    tuning_settings_defaults(&s);
    s.hot_threshold_dc = s.cool_threshold_dc; /* ordering violated */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.hot_threshold_dc = TUNING_FORECAST_SANITY_MAX_DC + 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));

    tuning_settings_defaults(&s);
    s.check_time_count = TUNING_MAX_DAILY_CHECKS + 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.check_times_min[1] = 1440; /* out of day */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.check_times_min[1] = 300; /* duplicate of [0] */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.check_times_min[1] = 200; /* not ascending */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.check_times_min[4] = 700; /* beyond count: canonical zero violated */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));

    tuning_settings_defaults(&s);
    strncpy(s.hot_day_profile_id, "Bad Chars", sizeof(s.hot_day_profile_id) - 1);
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_STRING, tuning_settings_validate(&s));

    tuning_settings_defaults(&s);
    s.retry_count = TUNING_MAX_RETRY_COUNT + 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.forecast_max_age_s = TUNING_FORECAST_MAX_AGE_MIN_S - 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.forecast_max_age_s = TUNING_FORECAST_MAX_AGE_MAX_S + 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.cooldown_s = TUNING_COOLDOWN_MIN_S - 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));
    tuning_settings_defaults(&s);
    s.cooldown_s = TUNING_COOLDOWN_MAX_S + 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_SETTINGS_INVALID, tuning_settings_validate(&s));
}

/* ---------------- enable check ---------------- */

TEST_CASE("enable check: production registry can NEVER enable the feature", "[tuning_record]")
{
    size_t n = 0;
    const TuningProfile *prod = tuning_registry_gamma601(&n);
    TuningHardwareContext hw = hw_full();
    TuningPolicySettings s = settings_enableable();
    /* Every production profile is UNVALIDATED -> role not eligible. */
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_ROLE_NOT_ELIGIBLE,
                      tuning_settings_enable_check(&s, prod, n, &hw));
}

TEST_CASE("enable check: full gate matrix + synthetic success", "[tuning_record]")
{
    TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
    reg_validated(reg);
    TuningHardwareContext hw = hw_full();
    TuningPolicySettings s = settings_enableable();

    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_NULL,
                      tuning_settings_enable_check(NULL, reg, 3, &hw));
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_NULL,
                      tuning_settings_enable_check(&s, NULL, 3, &hw));
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_NULL,
                      tuning_settings_enable_check(&s, reg, 0, &hw));
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_NULL,
                      tuning_settings_enable_check(&s, reg, 3, NULL));

    s = settings_enableable();
    s.hot_threshold_dc = s.cool_threshold_dc;
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_SETTINGS_INVALID,
                      tuning_settings_enable_check(&s, reg, 3, &hw));

    s = settings_enableable();
    s.provider = TUNING_PROVIDER_UNCONFIGURED;
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_PROVIDER,
                      tuning_settings_enable_check(&s, reg, 3, &hw));

    s = settings_enableable();
    s.timezone = TUNING_TZ_UNSPECIFIED;
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_TIMEZONE,
                      tuning_settings_enable_check(&s, reg, 3, &hw));

    s = settings_enableable();
    s.latitude_e4 = 0;
    s.longitude_e4 = 0;
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_COORDINATES,
                      tuning_settings_enable_check(&s, reg, 3, &hw));

    s = settings_enableable();
    s.check_time_count = 0;
    memset(s.check_times_min, 0, sizeof(s.check_times_min));
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_NO_CHECK_TIMES,
                      tuning_settings_enable_check(&s, reg, 3, &hw));

    s = settings_enableable();
    s.cool_day_profile_id[0] = '\0';
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_ROLE_MISSING,
                      tuning_settings_enable_check(&s, reg, 3, &hw));

    s = settings_enableable();
    memset(s.emergency_profile_id, 0, sizeof(s.emergency_profile_id));
    strncpy(s.emergency_profile_id, "ghost-profile",
            sizeof(s.emergency_profile_id) - 1);
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_ROLE_UNKNOWN,
                      tuning_settings_enable_check(&s, reg, 3, &hw));

    /* Incompatible hardware: supersink role needs the SuperSink tier. */
    s = settings_enableable();
    hw.cooling_installed = TUNING_COOLING_STOCK;
    TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_ROLE_NOT_ELIGIBLE,
                      tuning_settings_enable_check(&s, reg, 3, &hw));

    /* Synthetic validated registry + full hardware: the success path. */
    hw = hw_full();
    s = settings_enableable();
    TEST_ASSERT_EQUAL(TUNING_ENABLE_OK,
                      tuning_settings_enable_check(&s, reg, 3, &hw));
}

TEST_CASE("provider: unconfigured default blocks weather operation", "[tuning_record]")
{
    /* Owner Correction 3: a zero record and the defaults record both carry
     * provider UNCONFIGURED; Open-Meteo is never an implicit operational
     * default and enabling with UNCONFIGURED fails. */
    TuningPolicyRecord zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL(TUNING_PROVIDER_UNCONFIGURED, zero.settings.provider);

    tuning_record_init_state(&g_rec);
    TEST_ASSERT_EQUAL(TUNING_PROVIDER_UNCONFIGURED, g_rec.settings.provider);
    TEST_ASSERT_FALSE(g_rec.settings.enabled);
    TEST_ASSERT_EQUAL_INT32(0, g_rec.settings.latitude_e4);
    TEST_ASSERT_EQUAL_INT32(0, g_rec.settings.longitude_e4);
    TEST_ASSERT_EQUAL(TUNING_TZ_EUROPE_BRUSSELS, g_rec.settings.timezone);

    /* Even with everything ELSE configured and a synthetic validated
     * registry, an unconfigured provider blocks enablement. */
    {
        TuningProfile reg[TUNING_REGISTRY_GAMMA601_COUNT];
        TuningHardwareContext hw = hw_full();
        TuningPolicySettings s = settings_enableable();
        reg_validated(reg);
        s.provider = TUNING_PROVIDER_UNCONFIGURED;
        TEST_ASSERT_EQUAL(TUNING_ENABLE_ERR_PROVIDER,
                          tuning_settings_enable_check(&s, reg,
                                                       TUNING_REGISTRY_GAMMA601_COUNT,
                                                       &hw));
        /* Selecting OPEN_METEO explicitly is what unlocks the gate. */
        s.provider = TUNING_PROVIDER_OPEN_METEO;
        TEST_ASSERT_EQUAL(TUNING_ENABLE_OK,
                          tuning_settings_enable_check(&s, reg,
                                                       TUNING_REGISTRY_GAMMA601_COUNT,
                                                       &hw));
    }
    TEST_ASSERT_EQUAL_STRING("UNCONFIGURED",
                             tuning_provider_str(TUNING_PROVIDER_UNCONFIGURED));
}

/* ---------------- transaction finalization (Correction 1A) ------------ */

TEST_CASE("finalize: COMMITTED canonicalizes ONLY the tx subrecord", "[tuning_record]")
{
    TuningPolicyRecord before;
    rich_record(&g_rec);
    g_rec.tx.state = TUNING_TX_COMMITTED;
    /* Rich fixture has an active override + cooldown + floor + counters. */
    before = g_rec;

    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_finalize_transaction(&g_rec));
    TEST_ASSERT_EQUAL(TUNING_TX_IDLE, g_rec.tx.state);
    /* Transaction subrecord is canonical zero... */
    TEST_ASSERT_EQUAL_STRING("", g_rec.tx.requested_profile_id);
    TEST_ASSERT_EQUAL_STRING("", g_rec.tx.previous_profile_id);
    TEST_ASSERT_EQUAL_STRING("", g_rec.tx.rollback_profile_id);
    TEST_ASSERT_EQUAL(TUNING_ACTOR_NONE, g_rec.tx.actor);
    TEST_ASSERT_EQUAL_UINT32(0, g_rec.tx.policy_generation);
    TEST_ASSERT_EQUAL_UINT8(0, g_rec.tx.boot_attempt_count);
    TEST_ASSERT_EQUAL_UINT8(0, g_rec.tx.apply_attempt_count);
    TEST_ASSERT_EQUAL_UINT64(0ull, g_rec.tx.started_epoch_s);
    /* ...and every other durable safety fact is preserved byte-for-byte. */
    TEST_ASSERT_EQUAL_MEMORY(&before.settings, &g_rec.settings,
                             sizeof(before.settings));
    TEST_ASSERT_EQUAL_MEMORY(&before.climate, &g_rec.climate,
                             sizeof(before.climate));
    TEST_ASSERT_EQUAL_STRING(before.last_known_safe_id, g_rec.last_known_safe_id);
    TEST_ASSERT_EQUAL_UINT16(before.last_known_safe_revision,
                             g_rec.last_known_safe_revision);
    TEST_ASSERT_EQUAL_MEMORY(&before.override_state, &g_rec.override_state,
                             sizeof(before.override_state));
    TEST_ASSERT_EQUAL(before.cooldown_source, g_rec.cooldown_source);
    TEST_ASSERT_EQUAL_UINT64(before.cooldown_until_epoch_s,
                             g_rec.cooldown_until_epoch_s);
    TEST_ASSERT_EQUAL_UINT64(before.latest_trusted_epoch_s,
                             g_rec.latest_trusted_epoch_s);
    TEST_ASSERT_EQUAL_UINT8(before.consecutive_recovery_failures,
                            g_rec.consecutive_recovery_failures);
    /* The finalized record validates and is a STATE record — not a
     * tombstone, not cleared. */
    TEST_ASSERT_EQUAL_UINT8(TUNING_RECORD_KIND_STATE, g_rec.kind);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_validate(&g_rec));
}

TEST_CASE("finalize: idempotent from IDLE, refused elsewhere", "[tuning_record]")
{
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_INVALID_ARGUMENT,
                      tuning_record_finalize_transaction(NULL));

    tuning_record_init_state(&g_rec); /* IDLE */
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_finalize_transaction(&g_rec));
    TEST_ASSERT_EQUAL(TUNING_TX_IDLE, g_rec.tx.state);

    /* Pending, rollback and recovery evidence can never be finalized away. */
    static const TuningTxState keep[] = {
        TUNING_TX_INTENT_PERSISTED, TUNING_TX_APPLY_PENDING, TUNING_TX_APPLYING,
        TUNING_TX_RESTART_PENDING, TUNING_TX_VERIFYING,
        TUNING_TX_ROLLBACK_PENDING, TUNING_TX_ROLLING_BACK,
        TUNING_TX_RECOVERY_REQUIRED,
    };
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        rich_record(&g_rec);
        g_rec.tx.state = keep[i];
        TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TX_INVALID,
                          tuning_record_finalize_transaction(&g_rec));
        TEST_ASSERT_EQUAL(keep[i], g_rec.tx.state); /* untouched */
        TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_SUPERSINK_MAX,
                                 g_rec.tx.requested_profile_id);
    }

    tuning_record_init_tombstone(&g_rec);
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_KIND,
                      tuning_record_finalize_transaction(&g_rec));
}

/* ---------------- transaction validation ---------------- */

TEST_CASE("tx validate: IDLE must be canonical-zero", "[tuning_record]")
{
    TuningTransaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.state = TUNING_TX_IDLE;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_transaction_validate(&tx));

    TuningTransaction t2 = tx;
    strncpy(t2.requested_profile_id, "supersink-max",
            sizeof(t2.requested_profile_id) - 1);
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TX_INVALID, tuning_transaction_validate(&t2));
    t2 = tx;
    t2.actor = TUNING_ACTOR_MANUAL_API;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TX_INVALID, tuning_transaction_validate(&t2));
    t2 = tx;
    t2.policy_generation = 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TX_INVALID, tuning_transaction_validate(&t2));
    t2 = tx;
    t2.boot_attempt_count = 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TX_INVALID, tuning_transaction_validate(&t2));
    t2 = tx;
    t2.started_epoch_s = EPOCH_S;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TX_INVALID, tuning_transaction_validate(&t2));
}

TEST_CASE("tx validate: active-state requirements and bounds", "[tuning_record]")
{
    TuningTransaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.state = TUNING_TX_APPLYING;
    strncpy(tx.requested_profile_id, "supersink-max",
            sizeof(tx.requested_profile_id) - 1);
    tx.actor = TUNING_ACTOR_WEATHER_POLICY;
    tx.reason = TUNING_REASON_WEATHER_COOL_ELIGIBLE;
    tx.profile_revision = 2;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_transaction_validate(&tx));

    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_INVALID_ARGUMENT,
                      tuning_transaction_validate(NULL));

    TuningTransaction t2 = tx;
    t2.state = (TuningTxState)TUNING_TX__COUNT;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_ENUM, tuning_transaction_validate(&t2));
    t2 = tx;
    t2.actor = (TuningActorClass)TUNING_ACTOR__COUNT;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_ENUM, tuning_transaction_validate(&t2));
    t2 = tx;
    t2.reason = (TuningReasonCode)TUNING_REASON__COUNT;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_ENUM, tuning_transaction_validate(&t2));

    t2 = tx;
    t2.requested_profile_id[0] = '\0';
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TX_INVALID, tuning_transaction_validate(&t2));
    t2 = tx;
    t2.actor = TUNING_ACTOR_NONE;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TX_INVALID, tuning_transaction_validate(&t2));
    t2 = tx;
    t2.profile_revision = 0;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TX_INVALID, tuning_transaction_validate(&t2));

    t2 = tx;
    t2.boot_attempt_count = TUNING_TX_BOOT_ATTEMPT_STORE_MAX + 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_COUNTER_RANGE, tuning_transaction_validate(&t2));
    t2 = tx;
    memset(t2.rollback_profile_id, 0, sizeof(t2.rollback_profile_id));
    strncpy(t2.rollback_profile_id, "Bad Id", sizeof(t2.rollback_profile_id) - 1);
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_STRING, tuning_transaction_validate(&t2));

    t2 = tx;
    t2.started_epoch_s = 100; /* below band, nonzero */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_EPOCH, tuning_transaction_validate(&t2));
    t2 = tx;
    t2.started_epoch_s = EPOCH_S;
    t2.updated_epoch_s = EPOCH_S - 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TX_INVALID, tuning_transaction_validate(&t2));

    /* RECOVERY_REQUIRED preserves evidence as-recorded. */
    t2 = tx;
    t2.state = TUNING_TX_RECOVERY_REQUIRED;
    t2.boot_attempt_count = 5;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_transaction_validate(&t2));
}

/* ---------------- record validation ---------------- */

TEST_CASE("record validate: defaults and rich fixture pass", "[tuning_record]")
{
    tuning_record_init_state(&g_rec);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_validate(&g_rec));
    rich_record(&g_rec);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_validate(&g_rec));
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_INVALID_ARGUMENT, tuning_record_validate(NULL));
}

TEST_CASE("record validate: model gate, climate, lks, override, cooldown", "[tuning_record]")
{
    rich_record(&g_rec);
    g_rec.profile_model_version = 2;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_UNSUPPORTED_MODEL, tuning_record_validate(&g_rec));

    rich_record(&g_rec);
    g_rec.climate.last_forecast_valid = false; /* dc must be canonical 0 */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_CLIMATE_INVALID, tuning_record_validate(&g_rec));
    rich_record(&g_rec);
    g_rec.climate.last_forecast_max_dc = TUNING_FORECAST_SANITY_MAX_DC + 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_CLIMATE_INVALID, tuning_record_validate(&g_rec));
    rich_record(&g_rec);
    g_rec.climate.state = (TuningWeatherState)TUNING_WEATHER_STATE__COUNT;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_CLIMATE_INVALID, tuning_record_validate(&g_rec));

    rich_record(&g_rec);
    g_rec.last_known_safe_id[0] = '\0'; /* revision must follow */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_LKS_INVALID, tuning_record_validate(&g_rec));
    rich_record(&g_rec);
    g_rec.last_known_safe_revision = 0;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_LKS_INVALID, tuning_record_validate(&g_rec));

    rich_record(&g_rec);
    g_rec.override_state.active = false; /* residue forbidden */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_OVERRIDE_INVALID, tuning_record_validate(&g_rec));
    rich_record(&g_rec);
    g_rec.override_state.actor = TUNING_ACTOR_THERMAL;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_OVERRIDE_INVALID, tuning_record_validate(&g_rec));
    rich_record(&g_rec);
    g_rec.override_state.expires_epoch_s = g_rec.override_state.created_epoch_s;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_OVERRIDE_INVALID, tuning_record_validate(&g_rec));
    rich_record(&g_rec);
    g_rec.override_state.monotonic_expiry_us = 1; /* never persisted */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_OVERRIDE_INVALID, tuning_record_validate(&g_rec));

    rich_record(&g_rec);
    g_rec.cooldown_until_epoch_s = 0; /* source must be NONE then */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_COOLDOWN_INVALID, tuning_record_validate(&g_rec));
    rich_record(&g_rec);
    g_rec.cooldown_source = TUNING_ACTOR_NONE;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_COOLDOWN_INVALID, tuning_record_validate(&g_rec));
    rich_record(&g_rec);
    g_rec.cooldown_until_epoch_s = 100; /* below band */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_COOLDOWN_INVALID, tuning_record_validate(&g_rec));

    rich_record(&g_rec);
    g_rec.latest_trusted_epoch_s = 100;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_EPOCH, tuning_record_validate(&g_rec));
    rich_record(&g_rec);
    g_rec.consecutive_recovery_failures = TUNING_CONSECUTIVE_FAILURE_STORE_MAX + 1;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_COUNTER_RANGE, tuning_record_validate(&g_rec));

    rich_record(&g_rec);
    g_rec.kind = 3;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_KIND, tuning_record_validate(&g_rec));
}

TEST_CASE("record validate: tombstone must be empty", "[tuning_record]")
{
    tuning_record_init_tombstone(&g_rec);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_validate(&g_rec));
    g_rec.settings.enabled = true;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TOMBSTONE_NOT_EMPTY,
                      tuning_record_validate(&g_rec));
    tuning_record_init_tombstone(&g_rec);
    g_rec.latest_trusted_epoch_s = EPOCH_S;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TOMBSTONE_NOT_EMPTY,
                      tuning_record_validate(&g_rec));
}

/* ---------------- codec ---------------- */

TEST_CASE("codec: default state round trip is byte-exact", "[tuning_record]")
{
    size_t len = 0, len2 = 0;
    tuning_record_init_state(&g_rec);
    g_rec.generation = 1u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&g_rec, g_buf, sizeof(g_buf), &len));
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_decode(g_buf, len, &g_out));
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&g_out, g_buf2, sizeof(g_buf2), &len2));
    TEST_ASSERT_EQUAL(len, len2);
    TEST_ASSERT_EQUAL_MEMORY(g_buf, g_buf2, len);
    TEST_ASSERT_TRUE(len <= TUNING_RECORD_MAX_ENCODED);
}

TEST_CASE("codec: rich record round trip preserves every field", "[tuning_record]")
{
    size_t len = 0, len2 = 0;
    rich_record(&g_rec);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&g_rec, g_buf, sizeof(g_buf), &len));
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_decode(g_buf, len, &g_out));

    TEST_ASSERT_EQUAL_STRING("test site 1", g_out.settings.location_label);
    TEST_ASSERT_EQUAL_INT32(123456, g_out.settings.latitude_e4);
    TEST_ASSERT_EQUAL_INT32(-654321, g_out.settings.longitude_e4);
    TEST_ASSERT_EQUAL(TUNING_WEATHER_STATE_HOT, g_out.climate.state);
    TEST_ASSERT_EQUAL_INT16(324, g_out.climate.last_forecast_max_dc);
    TEST_ASSERT_EQUAL(TUNING_TX_VERIFYING, g_out.tx.state);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_SUPERSINK_MAX,
                             g_out.tx.requested_profile_id);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER,
                             g_out.last_known_safe_id);
    TEST_ASSERT_TRUE(g_out.override_state.active);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_S + 3600ull, g_out.override_state.expires_epoch_s);
    TEST_ASSERT_EQUAL_UINT64(0ull, g_out.override_state.monotonic_expiry_us);
    TEST_ASSERT_EQUAL(TUNING_ACTOR_THERMAL, g_out.cooldown_source);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_S + 7200ull, g_out.cooldown_until_epoch_s);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_S, g_out.latest_trusted_epoch_s);
    TEST_ASSERT_EQUAL_UINT8(1, g_out.consecutive_recovery_failures);

    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&g_out, g_buf2, sizeof(g_buf2), &len2));
    TEST_ASSERT_EQUAL(len, len2);
    TEST_ASSERT_EQUAL_MEMORY(g_buf, g_buf2, len);
}

TEST_CASE("codec: tombstone round trip", "[tuning_record]")
{
    size_t len = 0;
    tuning_record_init_tombstone(&g_rec);
    g_rec.generation = 4u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&g_rec, g_buf, sizeof(g_buf), &len));
    TEST_ASSERT_EQUAL_UINT32(TUNING_RECORD_HEADER_LEN + TUNING_RECORD_CRC_LEN, len);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_decode(g_buf, len, &g_out));
    TEST_ASSERT_EQUAL_UINT8(TUNING_RECORD_KIND_TOMBSTONE, g_out.kind);
    TEST_ASSERT_EQUAL_UINT32(4u, g_out.generation);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_validate(&g_out));
}

TEST_CASE("codec: encode argument and generation rules", "[tuning_record]")
{
    size_t len = 0;
    rich_record(&g_rec);
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_INVALID_ARGUMENT,
                      tuning_record_encode(NULL, g_buf, sizeof(g_buf), &len));
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_INVALID_ARGUMENT,
                      tuning_record_encode(&g_rec, NULL, sizeof(g_buf), &len));
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_INVALID_ARGUMENT,
                      tuning_record_encode(&g_rec, g_buf, sizeof(g_buf), NULL));
    g_rec.generation = 0u; /* the store assigns >= 1 */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_GENERATION,
                      tuning_record_encode(&g_rec, g_buf, sizeof(g_buf), &len));
    g_rec.generation = 1u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BUFFER_TOO_SMALL,
                      tuning_record_encode(&g_rec, g_buf, 16, &len));
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BUFFER_TOO_SMALL,
                      tuning_record_encode(&g_rec, g_buf, 64, &len));
}

TEST_CASE("codec: every single flipped byte is rejected", "[tuning_record]")
{
    size_t len = 0;
    rich_record(&g_rec);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&g_rec, g_buf, sizeof(g_buf), &len));
    for (size_t i = 0; i < len; i++) {
        memcpy(g_buf2, g_buf, len);
        g_buf2[i] ^= 0xA5u;
        TEST_ASSERT_NOT_EQUAL(TUNING_RECORD_OK,
                              tuning_record_decode(g_buf2, len, &g_out));
    }
}

TEST_CASE("codec: framing rejections (magic/schema/flags/length/kind)", "[tuning_record]")
{
    size_t len = 0;
    uint32_t crc;
    rich_record(&g_rec);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&g_rec, g_buf, sizeof(g_buf), &len));

    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_INVALID_ARGUMENT,
                      tuning_record_decode(NULL, len, &g_out));
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TRUNCATED,
                      tuning_record_decode(g_buf, 8, &g_out));
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TRUNCATED,
                      tuning_record_decode(g_buf, len - 1, &g_out));

    /* bad magic (checked before the CRC — no fix-up needed) */
    memcpy(g_buf2, g_buf, len);
    g_buf2[0] ^= 0xFFu;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_MAGIC,
                      tuning_record_decode(g_buf2, len, &g_out));

    /* unsupported schema (checked before the CRC).
     * Derived from the constant rather than hardcoded: Gate W6.4 made v2 a
     * SUPPORTED schema, and a literal here silently became a no-op assertion.
     * One past the current version is always unsupported by construction. */
    memcpy(g_buf2, g_buf, len);
    g_buf2[4] = (uint8_t)(TUNING_RECORD_SCHEMA_VERSION + 1u);
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_UNSUPPORTED_SCHEMA,
                      tuning_record_decode(g_buf2, len, &g_out));

    /* flags byte (checked before the CRC) */
    memcpy(g_buf2, g_buf, len);
    g_buf2[17] = 1u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_UNKNOWN_FLAGS,
                      tuning_record_decode(g_buf2, len, &g_out));

    /* unknown kind, CRC re-fixed */
    memcpy(g_buf2, g_buf, len);
    g_buf2[16] = 9u;
    crc = tuning_record_crc32(g_buf2, len - TUNING_RECORD_CRC_LEN);
    g_buf2[len - 4] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_KIND,
                      tuning_record_decode(g_buf2, len, &g_out));

    /* tombstone kind with a nonzero payload, CRC re-fixed */
    memcpy(g_buf2, g_buf, len);
    g_buf2[16] = (uint8_t)TUNING_RECORD_KIND_TOMBSTONE;
    crc = tuning_record_crc32(g_buf2, len - TUNING_RECORD_CRC_LEN);
    g_buf2[len - 4] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TOMBSTONE_NOT_EMPTY,
                      tuning_record_decode(g_buf2, len, &g_out));

    /* unsupported W1 profile-model version (payload bytes 0-1), CRC fixed */
    memcpy(g_buf2, g_buf, len);
    g_buf2[TUNING_RECORD_HEADER_LEN] = 2u;
    g_buf2[TUNING_RECORD_HEADER_LEN + 1u] = 0u;
    crc = tuning_record_crc32(g_buf2, len - TUNING_RECORD_CRC_LEN);
    g_buf2[len - 4] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_UNSUPPORTED_MODEL,
                      tuning_record_decode(g_buf2, len, &g_out));
}

/* ---------------- pointer codec ---------------- */

TEST_CASE("pointer: round trip and rejection matrix", "[tuning_record]")
{
    uint8_t pbuf[TUNING_RECORD_POINTER_LEN];
    uint8_t pbad[TUNING_RECORD_POINTER_LEN + 1];
    size_t plen = 0;
    TuningRecordPointer p = { .slot = TUNING_RECORD_SLOT_B, .generation = 41u };
    TuningRecordPointer q;

    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_pointer_encode(&p, pbuf, sizeof(pbuf), &plen));
    TEST_ASSERT_EQUAL_UINT32(TUNING_RECORD_POINTER_LEN, plen);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_pointer_decode(pbuf, plen, &q));
    TEST_ASSERT_EQUAL_UINT8(TUNING_RECORD_SLOT_B, q.slot);
    TEST_ASSERT_EQUAL_UINT32(41u, q.generation);

    p.generation = 0;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_GENERATION,
                      tuning_record_pointer_encode(&p, pbuf, sizeof(pbuf), &plen));
    p.generation = 1;
    p.slot = 3;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_INVALID_ARGUMENT,
                      tuning_record_pointer_encode(&p, pbuf, sizeof(pbuf), &plen));

    p.slot = TUNING_RECORD_SLOT_A;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_pointer_encode(&p, pbuf, sizeof(pbuf), &plen));
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TRUNCATED,
                      tuning_record_pointer_decode(pbuf, plen - 1, &q));
    memcpy(pbad, pbuf, plen);
    pbad[plen] = 0;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TRAILING_BYTES,
                      tuning_record_pointer_decode(pbad, plen + 1, &q));

    memcpy(pbad, pbuf, plen);
    pbad[9] ^= 0x40u; /* generation byte: CRC must catch it */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_CRC_MISMATCH,
                      tuning_record_pointer_decode(pbad, plen, &q));

    memcpy(pbad, pbuf, plen);
    pbad[0] ^= 0xFFu;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_MAGIC,
                      tuning_record_pointer_decode(pbad, plen, &q));
}

/* ---------------- epoch floor + counters ---------------- */

TEST_CASE("epoch floor: monotonic advance inside the band", "[tuning_record]")
{
    tuning_record_init_state(&g_rec);
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_INVALID_ARGUMENT,
                      tuning_record_propose_trusted_epoch(NULL, EPOCH_S));
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_EPOCH,
                      tuning_record_propose_trusted_epoch(&g_rec, 100ull));
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_BAD_EPOCH,
                      tuning_record_propose_trusted_epoch(&g_rec,
                                                          TUNING_RECORD_EPOCH_MAX_S + 1ull));
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_propose_trusted_epoch(&g_rec, EPOCH_S));
    TEST_ASSERT_EQUAL_UINT64(EPOCH_S, g_rec.latest_trusted_epoch_s);
    /* equal is allowed; regression is refused and does not mutate */
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_propose_trusted_epoch(&g_rec, EPOCH_S));
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_EPOCH_REGRESSION,
                      tuning_record_propose_trusted_epoch(&g_rec, EPOCH_S - 1ull));
    TEST_ASSERT_EQUAL_UINT64(EPOCH_S, g_rec.latest_trusted_epoch_s);
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_propose_trusted_epoch(&g_rec, EPOCH_S + 10ull));
    TEST_ASSERT_EQUAL_UINT64(EPOCH_S + 10ull, g_rec.latest_trusted_epoch_s);
}

TEST_CASE("counters: saturating increment", "[tuning_record]")
{
    TEST_ASSERT_EQUAL_UINT8(1, tuning_record_counter_increment(0, 10));
    TEST_ASSERT_EQUAL_UINT8(10, tuning_record_counter_increment(9, 10));
    TEST_ASSERT_EQUAL_UINT8(10, tuning_record_counter_increment(10, 10));
    TEST_ASSERT_EQUAL_UINT8(10, tuning_record_counter_increment(200, 10));
}

/* ---------------- tokens ---------------- */

TEST_CASE("strings: record enums have distinct tokens", "[tuning_record]")
{
    for (int i = 0; i < TUNING_RECORD_ERR__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("ERR_UNKNOWN",
                                        tuning_record_error_str((TuningRecordError)i)));
    }
    for (int i = 0; i < TUNING_TX__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN",
                                        tuning_tx_state_str((TuningTxState)i)));
    }
    for (int i = 0; i < TUNING_PROVIDER__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN",
                                        tuning_provider_str((TuningWeatherProvider)i)));
    }
    for (int i = 0; i < TUNING_TZ__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN",
                                        tuning_timezone_str((TuningTimezoneId)i)));
    }
    for (int i = 0; i < TUNING_API_FAIL__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN",
                                        tuning_api_failure_str((TuningApiFailureBehavior)i)));
    }
    for (int i = 0; i < TUNING_ENABLE__COUNT; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("ERR_UNKNOWN",
                                        tuning_enable_check_str((TuningEnableCheckResult)i)));
    }
    TEST_ASSERT_EQUAL_STRING("ERR_UNKNOWN", tuning_record_error_str((TuningRecordError)250));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", tuning_tx_state_str((TuningTxState)250));
}

/* ================================================================== */
/* Gate W6.4 — schema v1 -> v2 evolution and window state              */
/* ================================================================== */

/*
 * Re-frame a v2 encoding as a genuine v1 encoding by DROPPING the appended
 * window bytes and repairing every length field and the CRC. This produces
 * the exact bytes a pre-W6.4 firmware would have committed, so the migration
 * test exercises a real old record rather than an approximation of one.
 */
static size_t w64_downgrade_to_v1(uint8_t *buf, size_t len)
{
    uint32_t payload_len;
    size_t   new_len;

    TEST_ASSERT_TRUE(len > TUNING_RECORD_WINDOW_V2_BYTES + TUNING_RECORD_CRC_LEN);
    new_len = len - TUNING_RECORD_WINDOW_V2_BYTES;
    payload_len = (uint32_t)(new_len - TUNING_RECORD_HEADER_LEN -
                             TUNING_RECORD_CRC_LEN);

    buf[4] = 1u; buf[5] = 0u;                       /* schema = 1          */
    buf[8]  = (uint8_t)(new_len & 0xFFu);           /* total_len           */
    buf[9]  = (uint8_t)((new_len >> 8) & 0xFFu);
    buf[10] = (uint8_t)((new_len >> 16) & 0xFFu);
    buf[11] = (uint8_t)((new_len >> 24) & 0xFFu);
    buf[20] = (uint8_t)(payload_len & 0xFFu);       /* payload_len         */
    buf[21] = (uint8_t)((payload_len >> 8) & 0xFFu);
    buf[22] = (uint8_t)((payload_len >> 16) & 0xFFu);
    buf[23] = (uint8_t)((payload_len >> 24) & 0xFFu);

    {
        uint32_t crc = tuning_record_crc32(buf, new_len - TUNING_RECORD_CRC_LEN);
        size_t   o   = new_len - TUNING_RECORD_CRC_LEN;
        buf[o]     = (uint8_t)(crc & 0xFFu);
        buf[o + 1] = (uint8_t)((crc >> 8) & 0xFFu);
        buf[o + 2] = (uint8_t)((crc >> 16) & 0xFFu);
        buf[o + 3] = (uint8_t)((crc >> 24) & 0xFFu);
    }
    return new_len;
}

TEST_CASE("w64: the encoder writes schema v2 and round-trips window state",
          "[tuning_record]")
{
    TuningPolicyRecord rec, back;
    size_t len = 0;

    tuning_record_init_state(&rec);
    rec.generation = 7u;
    rec.window.present = true;
    rec.window.year = 2026u; rec.window.month = 7u; rec.window.day = 15u;
    rec.window.served_mask = 0x05u;

    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&rec, g_buf, sizeof(g_buf), &len));
    TEST_ASSERT_EQUAL_UINT16(2u, (uint16_t)(g_buf[4] | (g_buf[5] << 8)));
    TEST_ASSERT_TRUE(len <= TUNING_RECORD_MAX_ENCODED);

    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_decode(g_buf, len, &back));
    TEST_ASSERT_TRUE(back.window.present);
    TEST_ASSERT_EQUAL_UINT16(2026u, back.window.year);
    TEST_ASSERT_EQUAL_UINT8(7u, back.window.month);
    TEST_ASSERT_EQUAL_UINT8(15u, back.window.day);
    TEST_ASSERT_EQUAL_UINT8(0x05u, back.window.served_mask);
}

TEST_CASE("w64: a valid pre-W6.4 v1 record still loads and is NOT served",
          "[tuning_record]")
{
    TuningPolicyRecord rec, back;
    size_t len = 0, v1_len;

    tuning_record_init_state(&rec);
    rec.generation = 3u;
    rec.latest_trusted_epoch_s = TUNING_RECORD_EPOCH_MIN_S + 1000ull;
    rec.consecutive_recovery_failures = 2u;
    rec.window.present = true;
    rec.window.year = 2026u; rec.window.month = 7u; rec.window.day = 15u;
    rec.window.served_mask = 0x07u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&rec, g_buf, sizeof(g_buf), &len));

    v1_len = w64_downgrade_to_v1(g_buf, len);
    TEST_ASSERT_EQUAL_UINT(len - TUNING_RECORD_WINDOW_V2_BYTES, v1_len);

    /* THE migration rule: it decodes cleanly (not corrupt, not unsupported), */
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_decode(g_buf, v1_len, &back));
    /* every existing v1 field survives, */
    TEST_ASSERT_EQUAL_UINT32(3u, back.generation);
    TEST_ASSERT_EQUAL_UINT64(TUNING_RECORD_EPOCH_MIN_S + 1000ull,
                             back.latest_trusted_epoch_s);
    TEST_ASSERT_EQUAL_UINT8(2u, back.consecutive_recovery_failures);
    /* and the absent window means NEVER CLAIMED, never already-served. */
    TEST_ASSERT_FALSE(back.window.present);
    TEST_ASSERT_EQUAL_UINT8(0u, back.window.served_mask);
    TEST_ASSERT_EQUAL_UINT16(0u, back.window.year);
}

TEST_CASE("w64: a future schema is refused and the record is left intact",
          "[tuning_record]")
{
    TuningPolicyRecord rec, back;
    size_t len = 0;
    uint8_t snapshot[TUNING_RECORD_MAX_ENCODED];

    tuning_record_init_state(&rec);
    rec.generation = 5u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&rec, g_buf, sizeof(g_buf), &len));

    g_buf[4] = 3u; g_buf[5] = 0u;              /* claim schema 3 */
    memcpy(snapshot, g_buf, len);

    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_UNSUPPORTED_SCHEMA,
                      tuning_record_decode(g_buf, len, &back));
    /* Fail closed means the bytes are untouched: no destructive rewrite. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(snapshot, g_buf, len));

    g_buf[4] = 0u;                             /* below the minimum */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_UNSUPPORTED_SCHEMA,
                      tuning_record_decode(g_buf, len, &back));
}

TEST_CASE("w64: a v1 record carrying extra bytes is rejected, not guessed",
          "[tuning_record]")
{
    TuningPolicyRecord rec, back;
    size_t len = 0;

    tuning_record_init_state(&rec);
    rec.generation = 9u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&rec, g_buf, sizeof(g_buf), &len));
    /* Claim v1 while the payload still carries the v2 tail: the cursor cannot
     * be exhausted, so this must be TRAILING_BYTES, never a silent short read. */
    g_buf[4] = 1u; g_buf[5] = 0u;
    {
        uint32_t crc = tuning_record_crc32(g_buf, len - TUNING_RECORD_CRC_LEN);
        size_t o = len - TUNING_RECORD_CRC_LEN;
        g_buf[o] = (uint8_t)(crc & 0xFFu);
        g_buf[o + 1] = (uint8_t)((crc >> 8) & 0xFFu);
        g_buf[o + 2] = (uint8_t)((crc >> 16) & 0xFFu);
        g_buf[o + 3] = (uint8_t)((crc >> 24) & 0xFFu);
    }
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TRAILING_BYTES,
                      tuning_record_decode(g_buf, len, &back));
}

TEST_CASE("w64: window state validation enforces canonical zeros and bounds",
          "[tuning_record]")
{
    TuningScheduleWindowState w;
    TuningPolicyRecord rec;
    size_t len = 0;

    memset(&w, 0, sizeof(w));
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_window_state_validate(&w));
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_INVALID_ARGUMENT,
                      tuning_window_state_validate(NULL));

    /* Absent must be canonically zero: a stray field is invalid. */
    w.served_mask = 1u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_WINDOW_INVALID,
                      tuning_window_state_validate(&w));
    memset(&w, 0, sizeof(w));
    w.year = 2026u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_WINDOW_INVALID,
                      tuning_window_state_validate(&w));

    /* Present must carry a plausible date. */
    memset(&w, 0, sizeof(w));
    w.present = true; w.year = 2026u; w.month = 7u; w.day = 15u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_window_state_validate(&w));
    w.month = 0u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_WINDOW_INVALID,
                      tuning_window_state_validate(&w));
    w.month = 13u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_WINDOW_INVALID,
                      tuning_window_state_validate(&w));
    w.month = 7u; w.day = 0u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_WINDOW_INVALID,
                      tuning_window_state_validate(&w));
    w.day = 32u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_WINDOW_INVALID,
                      tuning_window_state_validate(&w));
    w.day = 15u; w.year = 2024u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_WINDOW_INVALID,
                      tuning_window_state_validate(&w));

    /* A present day with an EMPTY mask is legitimate and must stay encodable. */
    memset(&w, 0, sizeof(w));
    w.present = true; w.year = 2026u; w.month = 7u; w.day = 15u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_window_state_validate(&w));

    /* An invalid window makes the whole record refuse to encode. */
    tuning_record_init_state(&rec);
    rec.generation = 2u;
    rec.window.present = false;
    rec.window.served_mask = 0x01u;      /* absent but non-canonical */
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_WINDOW_INVALID,
                      tuning_record_encode(&rec, g_buf, sizeof(g_buf), &len));
}

TEST_CASE("w64: a tombstone carries no window claim", "[tuning_record]")
{
    TuningPolicyRecord rec, back;
    size_t len = 0;

    tuning_record_init_tombstone(&rec);
    rec.generation = 4u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                      tuning_record_encode(&rec, g_buf, sizeof(g_buf), &len));
    TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_decode(g_buf, len, &back));
    TEST_ASSERT_FALSE(back.window.present);

    /* A tombstone that claims a window is not a tombstone. */
    rec.window.present = true;
    rec.window.year = 2026u; rec.window.month = 7u; rec.window.day = 15u;
    TEST_ASSERT_EQUAL(TUNING_RECORD_ERR_TOMBSTONE_NOT_EMPTY,
                      tuning_record_encode(&rec, g_buf, sizeof(g_buf), &len));
}
