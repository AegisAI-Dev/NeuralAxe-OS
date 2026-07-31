/*
 * Exhaustive deterministic tests for the Gate W2 crash-safe dual-slot
 * store. The FAKE backend is the primary crash-consistency proof: staged
 * vs committed values with explicit commit boundaries and power loss as
 * "staged values are lost, committed values survive" (conservative reading
 * of the documented ESP-IDF NVS per-pair guarantee), plus deterministic
 * failure injection at every step of the commit sequence.
 *
 * The final section exercises the REAL ESP-IDF NVS adapter against the
 * isolated QEMU test NVS partition (never a physical device). No secrets,
 * no networking; every value is a synthetic fixture.
 */

#include <string.h>
#include "unity.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "tuning_store.h"

#define EPOCH_S 1800000000ull

/* ---------------- fake persistence backend ---------------- */

#define FK_A 0
#define FK_B 1
#define FK_P 2
#define FK_LOG_MAX 16

typedef struct {
    bool   present;
    size_t len;
    uint8_t bytes[TUNING_RECORD_MAX_ENCODED + 8u]; /* room for oversize test */
} FakeVal;

typedef struct {
    FakeVal committed[3];
    FakeVal staged[3];
    bool    staged_dirty[3];
    bool    opened;
    /* fail-on-Nth-call injection: -1 = never; 0 = fail on the next call */
    int fail_open_after;
    int fail_read_after;
    int fail_write_after;
    int fail_commit_after;
    int opens, reads, writes, commits, closes;
    int write_log[FK_LOG_MAX]; /* order of write targets */
    int write_log_len;
} FakeNvs;

static FakeNvs g_fake;

static int fk_index(const char *key)
{
    if (strcmp(key, TUNING_STORE_KEY_SLOT_A) == 0) return FK_A;
    if (strcmp(key, TUNING_STORE_KEY_SLOT_B) == 0) return FK_B;
    if (strcmp(key, TUNING_STORE_KEY_ACTIVE) == 0) return FK_P;
    return -1;
}

static bool fk_should_fail(int *counter)
{
    if (*counter < 0) {
        return false;
    }
    if (*counter == 0) {
        *counter = -1;
        return true;
    }
    (*counter)--;
    return false;
}

static int fk_open(void *ctx)
{
    (void)ctx;
    g_fake.opens++;
    if (fk_should_fail(&g_fake.fail_open_after)) {
        return TUNING_STORE_BACKEND_IO;
    }
    g_fake.opened = true;
    return TUNING_STORE_BACKEND_OK;
}

static int fk_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len)
{
    int idx = fk_index(key);
    const FakeVal *v;
    (void)ctx;
    g_fake.reads++;
    if (fk_should_fail(&g_fake.fail_read_after)) {
        return TUNING_STORE_BACKEND_IO;
    }
    if (!g_fake.opened || idx < 0 || out_len == NULL) {
        return TUNING_STORE_BACKEND_IO;
    }
    /* like real NVS: reads through the same handle see staged values */
    v = g_fake.staged_dirty[idx] ? &g_fake.staged[idx] : &g_fake.committed[idx];
    if (!v->present) {
        return TUNING_STORE_BACKEND_NOT_FOUND;
    }
    *out_len = v->len;
    if (buf == NULL || v->len > cap) {
        return TUNING_STORE_BACKEND_OK; /* length reported, nothing copied */
    }
    memcpy(buf, v->bytes, v->len);
    return TUNING_STORE_BACKEND_OK;
}

static int fk_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    int idx = fk_index(key);
    (void)ctx;
    g_fake.writes++;
    if (fk_should_fail(&g_fake.fail_write_after)) {
        return TUNING_STORE_BACKEND_IO;
    }
    if (!g_fake.opened || idx < 0 || buf == NULL || len == 0 ||
        len > sizeof(g_fake.staged[idx].bytes)) {
        return TUNING_STORE_BACKEND_IO;
    }
    g_fake.staged[idx].present = true;
    g_fake.staged[idx].len = len;
    memcpy(g_fake.staged[idx].bytes, buf, len);
    g_fake.staged_dirty[idx] = true;
    if (g_fake.write_log_len < FK_LOG_MAX) {
        g_fake.write_log[g_fake.write_log_len++] = idx;
    }
    return TUNING_STORE_BACKEND_OK;
}

static int fk_commit(void *ctx)
{
    int i;
    (void)ctx;
    g_fake.commits++;
    if (fk_should_fail(&g_fake.fail_commit_after)) {
        return TUNING_STORE_BACKEND_IO;
    }
    if (!g_fake.opened) {
        return TUNING_STORE_BACKEND_IO;
    }
    for (i = 0; i < 3; i++) {
        if (g_fake.staged_dirty[i]) {
            g_fake.committed[i] = g_fake.staged[i];
            g_fake.staged_dirty[i] = false;
        }
    }
    return TUNING_STORE_BACKEND_OK;
}

static int fk_close(void *ctx)
{
    (void)ctx;
    g_fake.closes++;
    g_fake.opened = false;
    return TUNING_STORE_BACKEND_OK;
}

static const TuningStoreBackendOps FAKE_OPS = {
    .open = fk_open, .read_blob = fk_read, .write_blob = fk_write,
    .commit = fk_commit, .close = fk_close,
};

static void fk_reset(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.fail_open_after = -1;
    g_fake.fail_read_after = -1;
    g_fake.fail_write_after = -1;
    g_fake.fail_commit_after = -1;
}

/* Power loss: staged (uncommitted) values vanish; committed survive. */
static void fk_power_loss(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        memset(&g_fake.staged[i], 0, sizeof(g_fake.staged[i]));
        g_fake.staged_dirty[i] = false;
    }
    g_fake.opened = false;
}

/* ---------------- shared fixtures (file-static; ~KB sizes) ------------ */

static TuningStore g_store;
static TuningPolicyRecord g_rec;
static TuningPolicyRecord g_out;
static TuningStoreLoadInfo g_info;
static TuningStoreNvsBackend g_nvs_backend;

/* A valid STATE record with a distinguishing marker. */
static void make_state_rec(TuningPolicyRecord *r, uint32_t marker)
{
    tuning_record_init_state(r);
    r->climate.state = TUNING_WEATHER_STATE_NORMAL;
    r->climate.last_forecast_valid = true;
    r->climate.last_forecast_max_dc = 250;
    r->climate.transition_count = marker;
    strncpy(r->last_known_safe_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(r->last_known_safe_id) - 1);
    r->last_known_safe_revision = 2u;
    r->latest_trusted_epoch_s = EPOCH_S;
}

/* A record whose transaction is mid-flight (pending apply). */
static void make_pending_rec(TuningPolicyRecord *r, uint32_t marker)
{
    make_state_rec(r, marker);
    r->tx.state = TUNING_TX_APPLYING;
    strncpy(r->tx.requested_profile_id, TUNING_PROFILE_ID_SUPERSINK_MAX,
            sizeof(r->tx.requested_profile_id) - 1);
    r->tx.actor = TUNING_ACTOR_WEATHER_POLICY;
    r->tx.reason = TUNING_REASON_WEATHER_COOL_ELIGIBLE;
    r->tx.profile_revision = 2u;
    r->tx.started_epoch_s = EPOCH_S;
    r->tx.updated_epoch_s = EPOCH_S + 5ull;
}

static void open_fake(void)
{
    TEST_ASSERT_EQUAL(TUNING_STORE_OK,
                      tuning_store_init(&g_store, &FAKE_OPS, NULL));
}

static void reopen_fake(void)
{
    (void)tuning_store_deinit(&g_store);
    open_fake();
}

/* ---------------- lifecycle + empty ---------------- */

TEST_CASE("store: init argument validation", "[tuning_store]")
{
    TuningStoreBackendOps bad = FAKE_OPS;
    fk_reset();
    TEST_ASSERT_EQUAL(TUNING_STORE_INVALID_ARGUMENT,
                      tuning_store_init(NULL, &FAKE_OPS, NULL));
    TEST_ASSERT_EQUAL(TUNING_STORE_INVALID_ARGUMENT,
                      tuning_store_init(&g_store, NULL, NULL));
    bad.commit = NULL;
    TEST_ASSERT_EQUAL(TUNING_STORE_INVALID_ARGUMENT,
                      tuning_store_init(&g_store, &bad, NULL));
    g_fake.fail_open_after = 0;
    TEST_ASSERT_EQUAL(TUNING_STORE_IO_ERROR,
                      tuning_store_init(&g_store, &FAKE_OPS, NULL));
    TEST_ASSERT_EQUAL(TUNING_STORE_NOT_INITIALIZED,
                      tuning_store_load(&g_store, &g_out, NULL));
}

TEST_CASE("store: pristine backend loads empty; commit refused nothing", "[tuning_store]")
{
    fk_reset();
    open_fake();
    TEST_ASSERT_EQUAL(TUNING_STORE_EMPTY, tuning_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_FALSE(tuning_store_committed_generation(&g_store, NULL));
    TEST_ASSERT_EQUAL(TUNING_STORE_STATE_CONFLICT, tuning_store_admin_reset(&g_store));
}

/* ---------------- commit + load + A/B alternation ---------------- */

TEST_CASE("store: first commit assigns generation 1 and loads back", "[tuning_store]")
{
    uint32_t gen = 0;
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 100u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL_UINT32(1u, g_rec.generation);
    TEST_ASSERT_TRUE(tuning_store_committed_generation(&g_store, &gen));
    TEST_ASSERT_EQUAL_UINT32(1u, gen);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(100u, g_out.climate.transition_count);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, g_out.last_known_safe_id);
    TEST_ASSERT_EQUAL_UINT8(TUNING_RECORD_SLOT_A, g_info.active_slot);
}

TEST_CASE("store: A/B alternation with pointer written LAST", "[tuning_store]")
{
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 1u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    g_fake.write_log_len = 0;
    make_state_rec(&g_rec, 2u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL_UINT32(2u, g_rec.generation);
    /* second commit targets slot B; the pointer write is strictly last */
    TEST_ASSERT_EQUAL(2, g_fake.write_log_len);
    TEST_ASSERT_EQUAL(FK_B, g_fake.write_log[0]);
    TEST_ASSERT_EQUAL(FK_P, g_fake.write_log[1]);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(2u, g_out.climate.transition_count);
    TEST_ASSERT_EQUAL_UINT8(TUNING_RECORD_SLOT_B, g_info.active_slot);
    /* old slot A intentionally remains as evidence */
    TEST_ASSERT_TRUE(g_fake.committed[FK_A].present);
}

/* ---------------- power-loss matrix ---------------- */

TEST_CASE("power loss: before any commit keeps the old record", "[tuning_store]")
{
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 10u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));

    /* slot write fails (I/O) -> old record intact after power loss */
    g_fake.fail_write_after = 0;
    make_state_rec(&g_rec, 11u);
    TEST_ASSERT_EQUAL(TUNING_STORE_IO_ERROR,
                      tuning_store_commit_record(&g_store, &g_rec));
    fk_power_loss();
    reopen_fake();
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(10u, g_out.climate.transition_count);

    /* slot commit fails -> old record intact after power loss */
    g_fake.fail_commit_after = 0;
    make_state_rec(&g_rec, 12u);
    TEST_ASSERT_EQUAL(TUNING_STORE_IO_ERROR,
                      tuning_store_commit_record(&g_store, &g_rec));
    fk_power_loss();
    reopen_fake();
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(10u, g_out.climate.transition_count);
}

TEST_CASE("power loss: during the pointer phase keeps the old record", "[tuning_store]")
{
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 20u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));

    /* pointer write fails -> COMMIT_UNCERTAIN; power loss drops the staged
     * pointer, so the OLD record remains committed */
    g_fake.fail_write_after = 1; /* slot write ok; pointer write fails */
    make_state_rec(&g_rec, 21u);
    TEST_ASSERT_EQUAL(TUNING_STORE_COMMIT_UNCERTAIN,
                      tuning_store_commit_record(&g_store, &g_rec));
    fk_power_loss();
    reopen_fake();
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(20u, g_out.climate.transition_count);
    TEST_ASSERT_TRUE(g_info.staged_newer_ignored); /* staged slot B ignored */

    /* pointer commit fails -> COMMIT_UNCERTAIN; conservative model drops
     * the staged pointer on power loss -> old record survives */
    g_fake.fail_commit_after = 1; /* slot commit ok; pointer commit fails */
    make_state_rec(&g_rec, 22u);
    TEST_ASSERT_EQUAL(TUNING_STORE_COMMIT_UNCERTAIN,
                      tuning_store_commit_record(&g_store, &g_rec));
    fk_power_loss();
    reopen_fake();
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(20u, g_out.climate.transition_count);
}

TEST_CASE("power loss: after a full commit keeps the NEW record", "[tuning_store]")
{
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 30u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    make_state_rec(&g_rec, 31u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    fk_power_loss();
    reopen_fake();
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(31u, g_out.climate.transition_count);
}

TEST_CASE("power loss: every mid-commit interruption yields old XOR new", "[tuning_store]")
{
    /* Deterministic sweep: fail each backend write/commit call in turn,
     * power-lose, reload — the committed state must always be exactly the
     * old or exactly the new record, never mixed or corrupt. */
    for (int step = 0; step < 4; step++) {
        fk_reset();
        open_fake();
        make_state_rec(&g_rec, 40u);
        TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));

        if (step % 2 == 0) {
            g_fake.fail_write_after = step / 2; /* write #1 or #2 */
        } else {
            g_fake.fail_commit_after = step / 2; /* commit #1 or #2 */
        }
        make_state_rec(&g_rec, 41u);
        TuningStoreResult r = tuning_store_commit_record(&g_store, &g_rec);
        TEST_ASSERT_NOT_EQUAL(TUNING_STORE_OK, r);
        fk_power_loss();
        reopen_fake();
        TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
        TEST_ASSERT_TRUE(g_out.climate.transition_count == 40u ||
                         g_out.climate.transition_count == 41u);
        TEST_ASSERT_EQUAL(TUNING_RECORD_OK, tuning_record_validate(&g_out));
        (void)tuning_store_deinit(&g_store);
    }
}

/* ---------------- verify failures + recovery conditions ---------------- */

TEST_CASE("store: readback failure leaves pointer untouched", "[tuning_store]")
{
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 50u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    /* commit sequence reads: base pointer, base slot, then readback */
    g_fake.fail_read_after = 2;
    make_state_rec(&g_rec, 51u);
    TEST_ASSERT_EQUAL(TUNING_STORE_READBACK_MISMATCH,
                      tuning_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(50u, g_out.climate.transition_count);
}

TEST_CASE("store: corrupt pointer and missing slot are recovery results", "[tuning_store]")
{
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 60u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));

    /* corrupt the committed pointer */
    g_fake.committed[FK_P].bytes[3] ^= 0xFFu;
    TEST_ASSERT_EQUAL(TUNING_STORE_ACTIVE_POINTER_INVALID,
                      tuning_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_TRUE(tuning_store_result_requires_recovery(
        TUNING_STORE_ACTIVE_POINTER_INVALID));
    g_fake.committed[FK_P].bytes[3] ^= 0xFFu; /* restore */

    /* pointer names a missing slot */
    g_fake.committed[FK_A].present = false;
    TEST_ASSERT_EQUAL(TUNING_STORE_ACTIVE_SLOT_INVALID,
                      tuning_store_load(&g_store, &g_out, NULL));
    /* and a commit over that refuses too */
    make_state_rec(&g_rec, 61u);
    TEST_ASSERT_EQUAL(TUNING_STORE_ACTIVE_SLOT_INVALID,
                      tuning_store_commit_record(&g_store, &g_rec));
}

TEST_CASE("store: slots without a pointer never yield a guess", "[tuning_store]")
{
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 70u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    /* delete only the pointer: staged-slot-without-pointer situation */
    g_fake.committed[FK_P].present = false;
    TEST_ASSERT_EQUAL(TUNING_STORE_RECOVERY_REQUIRED,
                      tuning_store_load(&g_store, &g_out, NULL));
    make_state_rec(&g_rec, 71u);
    TEST_ASSERT_EQUAL(TUNING_STORE_RECOVERY_REQUIRED,
                      tuning_store_commit_record(&g_store, &g_rec));
}

TEST_CASE("store: corrupt slot bytes and generation mismatch", "[tuning_store]")
{
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 80u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));

    /* flip one committed slot byte: CRC catches it -> CORRUPT */
    g_fake.committed[FK_A].bytes[30] ^= 0x5Au;
    TEST_ASSERT_EQUAL(TUNING_STORE_CORRUPT, tuning_store_load(&g_store, &g_out, NULL));
    g_fake.committed[FK_A].bytes[30] ^= 0x5Au;

    /* pointer/slot generation mismatch -> RECOVERY_REQUIRED */
    {
        TuningRecordPointer p = { .slot = TUNING_RECORD_SLOT_A, .generation = 9u };
        size_t plen = 0;
        TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                          tuning_record_pointer_encode(&p, g_fake.committed[FK_P].bytes,
                                                       sizeof(g_fake.committed[FK_P].bytes),
                                                       &plen));
        g_fake.committed[FK_P].len = plen;
    }
    TEST_ASSERT_EQUAL(TUNING_STORE_RECOVERY_REQUIRED,
                      tuning_store_load(&g_store, &g_out, NULL));
}

TEST_CASE("store: unsupported schema in the committed slot", "[tuning_store]")
{
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 90u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    /* patch the schema field and re-fix the CRC */
    {
        FakeVal *v = &g_fake.committed[FK_A];
        uint32_t crc;
        v->bytes[4] = 2u;
        crc = tuning_record_crc32(v->bytes, v->len - TUNING_RECORD_CRC_LEN);
        v->bytes[v->len - 4] = (uint8_t)(crc & 0xFFu);
        v->bytes[v->len - 3] = (uint8_t)((crc >> 8) & 0xFFu);
        v->bytes[v->len - 2] = (uint8_t)((crc >> 16) & 0xFFu);
        v->bytes[v->len - 1] = (uint8_t)((crc >> 24) & 0xFFu);
    }
    TEST_ASSERT_EQUAL(TUNING_STORE_UNSUPPORTED_SCHEMA,
                      tuning_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_TRUE(tuning_store_result_requires_recovery(
        TUNING_STORE_UNSUPPORTED_SCHEMA));
}

TEST_CASE("store: oversized stored blob is corrupt, not I/O", "[tuning_store]")
{
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 95u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    g_fake.committed[FK_A].len = TUNING_RECORD_MAX_ENCODED + 4u;
    TEST_ASSERT_EQUAL(TUNING_STORE_CORRUPT, tuning_store_load(&g_store, &g_out, NULL));
}

TEST_CASE("store: generation exhaustion is refused, never wrapped", "[tuning_store]")
{
    fk_reset();
    open_fake();
    /* hand-plant a committed record + pointer at generation UINT32_MAX */
    {
        size_t len = 0, plen = 0;
        TuningRecordPointer p = { .slot = TUNING_RECORD_SLOT_A,
                                  .generation = UINT32_MAX };
        make_state_rec(&g_rec, 99u);
        g_rec.generation = UINT32_MAX;
        TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                          tuning_record_encode(&g_rec, g_fake.committed[FK_A].bytes,
                                               sizeof(g_fake.committed[FK_A].bytes), &len));
        g_fake.committed[FK_A].present = true;
        g_fake.committed[FK_A].len = len;
        TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                          tuning_record_pointer_encode(&p, g_fake.committed[FK_P].bytes,
                                                       sizeof(g_fake.committed[FK_P].bytes),
                                                       &plen));
        g_fake.committed[FK_P].present = true;
        g_fake.committed[FK_P].len = plen;
    }
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
    make_state_rec(&g_rec, 100u);
    TEST_ASSERT_EQUAL(TUNING_STORE_GENERATION_EXHAUSTED,
                      tuning_store_commit_record(&g_store, &g_rec));
}

TEST_CASE("store: semantically invalid record is refused at commit", "[tuning_store]")
{
    fk_reset();
    open_fake();
    make_pending_rec(&g_rec, 1u);
    g_rec.tx.profile_revision = 0u; /* violates the active-tx contract */
    TEST_ASSERT_EQUAL(TUNING_STORE_INVALID_RECORD,
                      tuning_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(TUNING_STORE_EMPTY, tuning_store_load(&g_store, &g_out, NULL));
}

/* ---------------- finalization vs administrative reset ---------------- */

TEST_CASE("store: COMMITTED finalization preserves the combined record", "[tuning_store]")
{
    /* Correction 1A end-to-end: durable COMMITTED -> finalize -> normal
     * crash-safe commit -> the store still holds a STATE record (never
     * CLEARED) with everything except the tx subrecord preserved. */
    fk_reset();
    open_fake();
    make_state_rec(&g_rec, 7u);
    g_rec.tx.state = TUNING_TX_COMMITTED;
    strncpy(g_rec.tx.requested_profile_id, TUNING_PROFILE_ID_HOT_WEATHER,
            sizeof(g_rec.tx.requested_profile_id) - 1);
    strncpy(g_rec.tx.previous_profile_id, TUNING_PROFILE_ID_EMERGENCY,
            sizeof(g_rec.tx.previous_profile_id) - 1);
    g_rec.tx.actor = TUNING_ACTOR_WEATHER_POLICY;
    g_rec.tx.reason = TUNING_REASON_WEATHER_COOL_ELIGIBLE;
    g_rec.tx.profile_revision = 2u;
    g_rec.tx.boot_attempt_count = 2u;
    g_rec.tx.started_epoch_s = EPOCH_S;
    g_rec.tx.updated_epoch_s = EPOCH_S + 9ull;
    g_rec.consecutive_recovery_failures = 3u;
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));

    /* reload, finalize, commit the combined record through the same path */
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
    {
        TuningPolicyRecord before = g_out;
        TEST_ASSERT_EQUAL(TUNING_RECORD_OK,
                          tuning_record_finalize_transaction(&g_out));
        TEST_ASSERT_EQUAL(TUNING_STORE_OK,
                          tuning_store_commit_record(&g_store, &g_out));
        TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
        /* NOT cleared; tx canonical IDLE; durable facts preserved */
        TEST_ASSERT_EQUAL(TUNING_TX_IDLE, g_out.tx.state);
        TEST_ASSERT_EQUAL_STRING("", g_out.tx.requested_profile_id);
        TEST_ASSERT_EQUAL_MEMORY(&before.settings, &g_out.settings,
                                 sizeof(before.settings));
        TEST_ASSERT_EQUAL_STRING(before.last_known_safe_id,
                                 g_out.last_known_safe_id);
        TEST_ASSERT_EQUAL_UINT64(before.latest_trusted_epoch_s,
                                 g_out.latest_trusted_epoch_s);
        TEST_ASSERT_EQUAL_UINT8(before.consecutive_recovery_failures,
                                g_out.consecutive_recovery_failures);
        TEST_ASSERT_EQUAL_UINT32(before.climate.transition_count,
                                 g_out.climate.transition_count);
    }
}

TEST_CASE("store: admin reset refused for every unsafe transaction state", "[tuning_store]")
{
    /* Pending, rollback and recovery evidence can NEVER be tombstoned —
     * not even by the explicit administrative reset. */
    static const TuningTxState unsafe[] = {
        TUNING_TX_INTENT_PERSISTED, TUNING_TX_APPLY_PENDING, TUNING_TX_APPLYING,
        TUNING_TX_RESTART_PENDING, TUNING_TX_VERIFYING,
        TUNING_TX_ROLLBACK_PENDING, TUNING_TX_ROLLING_BACK,
        TUNING_TX_RECOVERY_REQUIRED,
    };
    for (size_t i = 0; i < sizeof(unsafe) / sizeof(unsafe[0]); i++) {
        fk_reset();
        open_fake();
        make_pending_rec(&g_rec, (uint32_t)i);
        g_rec.tx.state = unsafe[i];
        TEST_ASSERT_EQUAL(TUNING_STORE_OK,
                          tuning_store_commit_record(&g_store, &g_rec));
        TEST_ASSERT_EQUAL(TUNING_STORE_STATE_CONFLICT,
                          tuning_store_admin_reset(&g_store));
        TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
        TEST_ASSERT_EQUAL(unsafe[i], g_out.tx.state); /* evidence intact */
        (void)tuning_store_deinit(&g_store);
    }
}

TEST_CASE("store: explicit administrative reset is a separate manual path", "[tuning_store]")
{
    /* The ONLY route to a full-store tombstone: the explicitly named
     * administrative reset, from a safe transaction state. Normal
     * completion (previous test) never reaches it, and no W2 production
     * code calls it. */
    fk_reset();
    open_fake();

    make_state_rec(&g_rec, 2u); /* IDLE tx */
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_admin_reset(&g_store));
    TEST_ASSERT_EQUAL(TUNING_STORE_CLEARED, tuning_store_load(&g_store, NULL, &g_info));
    /* resetting again: nothing to clear */
    TEST_ASSERT_EQUAL(TUNING_STORE_STATE_CONFLICT, tuning_store_admin_reset(&g_store));

    /* a new record can be committed over a tombstone */
    make_state_rec(&g_rec, 3u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(3u, g_out.climate.transition_count);
}

/* ---------------- classifiers ---------------- */

TEST_CASE("store: result classifiers are exact", "[tuning_store]")
{
    for (int i = 0; i < TUNING_STORE_RESULT__COUNT; i++) {
        TuningStoreResult r = (TuningStoreResult)i;
        bool rec = (r == TUNING_STORE_CORRUPT || r == TUNING_STORE_UNSUPPORTED_SCHEMA ||
                    r == TUNING_STORE_INVALID_RECORD ||
                    r == TUNING_STORE_ACTIVE_POINTER_INVALID ||
                    r == TUNING_STORE_ACTIVE_SLOT_INVALID ||
                    r == TUNING_STORE_RECOVERY_REQUIRED ||
                    r == TUNING_STORE_GENERATION_EXHAUSTED);
        TEST_ASSERT_EQUAL(rec, tuning_store_result_requires_recovery(r));
        TEST_ASSERT_EQUAL(r == TUNING_STORE_OK,
                          tuning_store_result_permits_state_load(r));
        TEST_ASSERT_NOT_EQUAL(0, strcmp("STORE_RESULT_UNKNOWN",
                                        tuning_store_result_str(r)));
    }
    TEST_ASSERT_EQUAL_STRING("STORE_RESULT_UNKNOWN",
                             tuning_store_result_str((TuningStoreResult)99));
}

/* ---------------- REAL ESP-IDF NVS adapter (isolated QEMU image) ------ */

static void ensure_test_nvs(void)
{
    static bool inited = false;
    if (!inited) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            TEST_ASSERT_EQUAL(ESP_OK, nvs_flash_erase()); /* isolated QEMU image */
            err = nvs_flash_init();
        }
        TEST_ASSERT_EQUAL(ESP_OK, err);
        inited = true;
    }
}

/* Test-harness reset of the dedicated namespace on the ISOLATED QEMU NVS
 * (the adapter itself has no erase capability at all). */
static void wipe_test_namespace(void)
{
    nvs_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(TUNING_STORE_NVS_NAMESPACE, NVS_READWRITE, &h));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_erase_all(h));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(h));
    nvs_close(h);
}

TEST_CASE("nvs: adapter round trip on the dedicated namespace", "[tuning_store]")
{
    ensure_test_nvs();
    wipe_test_namespace();
    memset(&g_nvs_backend, 0, sizeof(g_nvs_backend));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK,
                      tuning_store_init(&g_store, tuning_store_nvs_ops(), &g_nvs_backend));
    TEST_ASSERT_EQUAL(TUNING_STORE_EMPTY, tuning_store_load(&g_store, &g_out, NULL));
    make_state_rec(&g_rec, 501u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(501u, g_out.climate.transition_count);
    /* close and reopen: the record persists in real NVS */
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_deinit(&g_store));
    memset(&g_nvs_backend, 0, sizeof(g_nvs_backend));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK,
                      tuning_store_init(&g_store, tuning_store_nvs_ops(), &g_nvs_backend));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(501u, g_out.climate.transition_count);
    TEST_ASSERT_EQUAL_STRING(TUNING_PROFILE_ID_HOT_WEATHER, g_out.last_known_safe_id);
    (void)tuning_store_deinit(&g_store);
}

TEST_CASE("nvs: A/B update and tombstone clear on real NVS", "[tuning_store]")
{
    uint32_t first_gen = 0;
    ensure_test_nvs();
    memset(&g_nvs_backend, 0, sizeof(g_nvs_backend));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK,
                      tuning_store_init(&g_store, tuning_store_nvs_ops(), &g_nvs_backend));
    (void)tuning_store_load(&g_store, &g_out, &g_info);
    first_gen = g_info.committed_generation;

    make_state_rec(&g_rec, 502u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_TRUE(g_rec.generation > first_gen);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(502u, g_out.climate.transition_count);

    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_admin_reset(&g_store));
    TEST_ASSERT_EQUAL(TUNING_STORE_CLEARED, tuning_store_load(&g_store, NULL, &g_info));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_deinit(&g_store));
    memset(&g_nvs_backend, 0, sizeof(g_nvs_backend));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK,
                      tuning_store_init(&g_store, tuning_store_nvs_ops(), &g_nvs_backend));
    TEST_ASSERT_EQUAL(TUNING_STORE_CLEARED, tuning_store_load(&g_store, NULL, NULL));
    (void)tuning_store_deinit(&g_store);
}

TEST_CASE("nvs: the adapter never touches unrelated namespaces", "[tuning_store]")
{
    nvs_handle_t other;
    uint32_t sentinel = 0;
    ensure_test_nvs();
    /* plant a sentinel in a DIFFERENT namespace */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("nx_wprobe", NVS_READWRITE, &other));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u32(other, "sentinel", 0xC0FFEEu));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(other));
    nvs_close(other);

    memset(&g_nvs_backend, 0, sizeof(g_nvs_backend));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK,
                      tuning_store_init(&g_store, tuning_store_nvs_ops(), &g_nvs_backend));
    make_state_rec(&g_rec, 503u);
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&g_store, &g_rec));
    (void)tuning_store_deinit(&g_store);

    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("nx_wprobe", NVS_READONLY, &other));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u32(other, "sentinel", &sentinel));
    nvs_close(other);
    TEST_ASSERT_EQUAL_UINT32(0xC0FFEEu, sentinel);
}
