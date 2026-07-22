/*
 * Exhaustive deterministic tests for the crash-safe dual-slot store (B3).
 *
 * The FAKE backend below is the primary crash-consistency proof: it models
 * staged-vs-committed values with explicit commit boundaries and power loss
 * as "staged values are lost, committed values survive" — the conservative
 * reading of the documented ESP-IDF NVS per-pair guarantee. Deterministic
 * failure injection covers every step of the commit sequence.
 *
 * The final section exercises the REAL ESP-IDF NVS adapter against the
 * isolated QEMU test NVS partition (never a physical device). No secrets,
 * no networking; all identities are synthetic "*.example" fixtures.
 */

#include <string.h>
#include "unity.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "pool_session_store.h"

#define EPOCH_A_S 1750000000ull

/* ---------------- fake persistence backend ---------------- */

#define FK_A 0
#define FK_B 1
#define FK_P 2

typedef struct {
    bool   present;
    size_t len;
    uint8_t bytes[POOL_RECORD_MAX_ENCODED];
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
} FakeNvs;

static FakeNvs g_fake;

static int fk_index(const char *key)
{
    if (strcmp(key, POOL_STORE_KEY_SLOT_A) == 0) return FK_A;
    if (strcmp(key, POOL_STORE_KEY_SLOT_B) == 0) return FK_B;
    if (strcmp(key, POOL_STORE_KEY_ACTIVE) == 0) return FK_P;
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
        return POOL_STORE_BACKEND_IO;
    }
    g_fake.opened = true;
    return POOL_STORE_BACKEND_OK;
}

static int fk_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len)
{
    int idx = fk_index(key);
    const FakeVal *v;
    (void)ctx;
    g_fake.reads++;
    if (fk_should_fail(&g_fake.fail_read_after)) {
        return POOL_STORE_BACKEND_IO;
    }
    if (!g_fake.opened || idx < 0 || out_len == NULL) {
        return POOL_STORE_BACKEND_IO;
    }
    /* like real NVS: reads through the same handle see staged values */
    v = g_fake.staged_dirty[idx] ? &g_fake.staged[idx] : &g_fake.committed[idx];
    if (!v->present) {
        return POOL_STORE_BACKEND_NOT_FOUND;
    }
    *out_len = v->len;
    if (buf == NULL || v->len > cap) {
        return POOL_STORE_BACKEND_OK; /* length reported, nothing copied */
    }
    memcpy(buf, v->bytes, v->len);
    return POOL_STORE_BACKEND_OK;
}

static int fk_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    int idx = fk_index(key);
    (void)ctx;
    g_fake.writes++;
    if (fk_should_fail(&g_fake.fail_write_after)) {
        return POOL_STORE_BACKEND_IO;
    }
    if (!g_fake.opened || idx < 0 || buf == NULL || len == 0 ||
        len > sizeof(g_fake.staged[idx].bytes)) {
        return POOL_STORE_BACKEND_IO;
    }
    g_fake.staged[idx].present = true;
    g_fake.staged[idx].len = len;
    memcpy(g_fake.staged[idx].bytes, buf, len);
    g_fake.staged_dirty[idx] = true;
    return POOL_STORE_BACKEND_OK;
}

static int fk_commit(void *ctx)
{
    int i;
    (void)ctx;
    g_fake.commits++;
    if (fk_should_fail(&g_fake.fail_commit_after)) {
        return POOL_STORE_BACKEND_IO;
    }
    if (!g_fake.opened) {
        return POOL_STORE_BACKEND_IO;
    }
    for (i = 0; i < 3; i++) {
        if (g_fake.staged_dirty[i]) {
            g_fake.committed[i] = g_fake.staged[i];
            g_fake.staged_dirty[i] = false;
        }
    }
    return POOL_STORE_BACKEND_OK;
}

static int fk_close(void *ctx)
{
    (void)ctx;
    g_fake.closes++;
    g_fake.opened = false;
    return POOL_STORE_BACKEND_OK;
}

static const PoolStoreBackendOps FAKE_OPS = {
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

/* Power loss: staged (uncommitted) values vanish; committed values survive. */
static void fk_power_loss(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        memset(&g_fake.staged[i], 0, sizeof(g_fake.staged[i]));
        g_fake.staged_dirty[i] = false;
    }
    g_fake.opened = false;
}

/* ---------------- shared fixtures (file-static; ~KB sizes) ---------------- */

static PoolSessionStore g_store;
static PoolSessionRecord g_rec;
static PoolSessionRecord g_out;
static PoolStoreLoadInfo g_info;

static void fill_identity(PoolConfigIdentity *c, PoolChainType chain, const char *host,
                          uint16_t port, const char *user)
{
    memset(c, 0, sizeof(*c));
    c->chain = chain;
    strncpy(c->primary.host, host, sizeof(c->primary.host) - 1);
    c->primary.port = port;
    strncpy(c->primary.user, user, sizeof(c->primary.user) - 1);
    c->primary.protocol = POOL_PROTO_STRATUM_V1;
}

static void make_session_rec(PoolSessionRecord *r, uint32_t session_id)
{
    pool_session_record_init(r);
    r->session_id = session_id;
    r->b1_model_version = POOL_SESSION_MODEL_VERSION;
    r->state = POOL_STATE_TARGET_ACTIVE;
    fill_identity(&r->source, POOL_CHAIN_BITCOIN, "btc.example", 3333, "acct.worker");
    fill_identity(&r->target, POOL_CHAIN_BITCOIN_CASH, "bch.example", 3334, "acct.worker");
    r->password_policy = POOL_SESSION_PW_KEEP_CURRENT;
    r->restore_required = true;
    r->target_verify.connection_observed = true;
    r->target_verify.mining_observed = true;
    r->target_verify.identity_verified = true;
    r->duration_s = 3600u;
    r->verified_start_valid = true;
    r->verified_start_epoch_s = EPOCH_A_S;
    r->deadline_valid = true;
    r->deadline_epoch_s = EPOCH_A_S + 3600u;
    r->deadline_sync_generation = 1u;
}

static void make_complete_rec(PoolSessionRecord *r, uint32_t session_id)
{
    make_session_rec(r, session_id);
    r->state = POOL_STATE_COMPLETE;
    r->restore_required = false;
    r->restore_verify.connection_observed = true;
    r->restore_verify.mining_observed = true;
    r->restore_verify.identity_verified = true;
}

/* open a fresh store over the (already prepared) fake state */
static void store_open(void)
{
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&g_store, &FAKE_OPS, NULL));
}

/* fresh fake + open store */
static void fresh_store(void)
{
    fk_reset();
    store_open();
}

/* commit one session record; assert OK; return assigned generation */
static uint32_t commit_ok(uint32_t session_id)
{
    make_session_rec(&g_rec, session_id);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    return g_rec.generation;
}

/* corrupt one committed byte directly (simulated flash corruption) */
static void poke_committed(int idx, size_t at)
{
    TEST_ASSERT_TRUE(g_fake.committed[idx].present);
    TEST_ASSERT_TRUE(at < g_fake.committed[idx].len);
    g_fake.committed[idx].bytes[at] ^= 0xFFu;
}

static bool bytes_contain(const void *hay, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    const uint8_t *h = (const uint8_t *)hay;
    size_t i;
    if (n == 0u || n > len) {
        return false;
    }
    for (i = 0; i + n <= len; i++) {
        if (memcmp(h + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* ================================================================= */
/* F. Successful store tests                                          */
/* ================================================================= */

TEST_CASE("store: lifecycle and argument guards", "[pool_store]")
{
    fk_reset();
    TEST_ASSERT_EQUAL(STORE_INVALID_ARGUMENT, pool_session_store_init(NULL, &FAKE_OPS, NULL));
    TEST_ASSERT_EQUAL(STORE_INVALID_ARGUMENT, pool_session_store_init(&g_store, NULL, NULL));
    /* open failure */
    g_fake.fail_open_after = 0;
    TEST_ASSERT_EQUAL(STORE_IO_ERROR, pool_session_store_init(&g_store, &FAKE_OPS, NULL));
    /* operations before init */
    memset(&g_store, 0, sizeof(g_store));
    TEST_ASSERT_EQUAL(STORE_NOT_INITIALIZED, pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL(STORE_NOT_INITIALIZED,
                      pool_session_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(STORE_NOT_INITIALIZED, pool_session_store_commit_clear(&g_store));
    TEST_ASSERT_FALSE(pool_session_store_committed_generation(&g_store, NULL));
    /* init + deinit are deterministic and idempotent */
    fresh_store();
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&g_store));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&g_store));
    TEST_ASSERT_EQUAL_UINT32(1, g_fake.closes);
}

TEST_CASE("store: empty store loads STORE_EMPTY", "[pool_store]")
{
    fresh_store();
    TEST_ASSERT_EQUAL(STORE_EMPTY, pool_session_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_FALSE(pool_store_result_permits_session_load(STORE_EMPTY));
    TEST_ASSERT_FALSE(pool_store_result_requires_recovery(STORE_EMPTY));
    TEST_ASSERT_FALSE(pool_session_store_committed_generation(&g_store, NULL));
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("store: first commit uses slot A with generation 1", "[pool_store]")
{
    uint32_t gen;
    fresh_store();
    gen = commit_ok(101u);
    TEST_ASSERT_EQUAL_UINT32(1u, gen);
    TEST_ASSERT_TRUE(g_fake.committed[FK_A].present);
    TEST_ASSERT_TRUE(g_fake.committed[FK_P].present);
    TEST_ASSERT_FALSE(g_fake.committed[FK_B].present);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(101u, g_out.session_id);
    TEST_ASSERT_EQUAL_UINT8(POOL_RECORD_SLOT_A, g_info.active_slot);
    TEST_ASSERT_EQUAL_UINT32(1u, g_info.committed_generation);
    TEST_ASSERT_TRUE(pool_store_result_permits_session_load(STORE_OK));
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("store: commits alternate slots and increment generations", "[pool_store]")
{
    uint32_t gen;
    fresh_store();
    (void)commit_ok(101u);
    gen = commit_ok(102u);
    TEST_ASSERT_EQUAL_UINT32(2u, gen);
    TEST_ASSERT_TRUE(g_fake.committed[FK_B].present);
    /* the old slot A remains as crash-recovery evidence */
    TEST_ASSERT_TRUE(g_fake.committed[FK_A].present);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(102u, g_out.session_id);
    TEST_ASSERT_EQUAL_UINT8(POOL_RECORD_SLOT_B, g_info.active_slot);

    gen = commit_ok(103u);
    TEST_ASSERT_EQUAL_UINT32(3u, gen);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT8(POOL_RECORD_SLOT_A, g_info.active_slot);
    TEST_ASSERT_EQUAL_UINT32(103u, g_out.session_id);
    {
        uint32_t cg = 0;
        TEST_ASSERT_TRUE(pool_session_store_committed_generation(&g_store, &cg));
        TEST_ASSERT_EQUAL_UINT32(3u, cg);
    }
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("store: restore_required and the source identity survive persistence", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(7u);
    /* simulate reboot: fresh store instance over the surviving fake flash */
    (void)pool_session_store_deinit(&g_store);
    fk_power_loss();
    store_open();
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_TRUE(g_out.restore_required);
    TEST_ASSERT_EQUAL_STRING("btc.example", g_out.source.primary.host);
    TEST_ASSERT_EQUAL_STRING("acct.worker", g_out.source.primary.user);
    TEST_ASSERT_EQUAL_UINT16(3333u, g_out.source.primary.port);
    TEST_ASSERT_TRUE(g_out.deadline_valid);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 3600u, g_out.deadline_epoch_s);
    (void)pool_session_store_deinit(&g_store);
}

/* ================================================================= */
/* G. Power-loss tests                                                */
/* ================================================================= */

TEST_CASE("power-loss: before the pointer phase the old record stays committed", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(1u);

    /* crash before the slot write: nothing staged */
    fk_power_loss();
    store_open();
    make_session_rec(&g_rec, 2u);
    /* crash "during/after slot write, before slot commit": stage then lose */
    TEST_ASSERT_EQUAL(POOL_STORE_BACKEND_OK,
                      fk_write(NULL, POOL_STORE_KEY_SLOT_B, (const uint8_t *)"x", 1));
    fk_power_loss();
    store_open();
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(1u, g_out.session_id); /* old record authoritative */
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("power-loss: slot committed but pointer lost keeps the old record", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(1u);
    /* run a commit that dies right before the pointer write: inject failure
     * on the pointer write (write call #2 of the commit: slot then pointer) */
    make_session_rec(&g_rec, 2u);
    g_fake.fail_write_after = 1; /* first write (slot) ok; second (pointer) fails */
    TEST_ASSERT_EQUAL(STORE_COMMIT_UNCERTAIN,
                      pool_session_store_commit_record(&g_store, &g_rec));
    fk_power_loss(); /* staged pointer (never written) is irrelevant; slot B committed */
    store_open();
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(1u, g_out.session_id);           /* OLD record */
    TEST_ASSERT_TRUE(g_info.staged_newer_ignored);            /* newer slot ignored */
    TEST_ASSERT_TRUE(g_fake.committed[FK_B].present);         /* evidence retained */
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("power-loss: pointer staged but uncommitted keeps the old record", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(1u);
    make_session_rec(&g_rec, 2u);
    /* pointer write succeeds, pointer commit fails (commit call #2) */
    g_fake.fail_commit_after = 1;
    TEST_ASSERT_EQUAL(STORE_COMMIT_UNCERTAIN,
                      pool_session_store_commit_record(&g_store, &g_rec));
    fk_power_loss(); /* the staged pointer is lost */
    store_open();
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(1u, g_out.session_id);
    TEST_ASSERT_TRUE(g_info.staged_newer_ignored);
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("power-loss: after the pointer commit the new record is authoritative", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(1u);
    make_session_rec(&g_rec, 2u);
    /* pointer readback fails AFTER the pointer commit actually succeeded:
     * commit reports UNCERTAIN, reload learns the truth = new record */
    g_fake.fail_read_after = 2; /* commit reads: ptr, slot, slot-readback... */
    /* reads within this commit: 1 pointer, 2 committed-slot, 3 slot readback,
     * 4 pointer readback -> fail the 4th read of this sequence */
    g_fake.fail_read_after = 3;
    TEST_ASSERT_EQUAL(STORE_COMMIT_UNCERTAIN,
                      pool_session_store_commit_record(&g_store, &g_rec));
    fk_power_loss();
    store_open();
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(2u, g_out.session_id); /* NEW record committed */
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("power-loss: slot readback mismatch aborts before the pointer", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(1u);
    make_session_rec(&g_rec, 2u);
    /* fail the slot readback (3rd read of the commit sequence) */
    g_fake.fail_read_after = 2;
    TEST_ASSERT_EQUAL(STORE_READBACK_MISMATCH,
                      pool_session_store_commit_record(&g_store, &g_rec));
    /* no crash needed: the pointer was never touched */
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(1u, g_out.session_id);
    (void)pool_session_store_deinit(&g_store);
}

/* ================================================================= */
/* H. Staged-slot tests                                               */
/* ================================================================= */

TEST_CASE("staged: a newer staged slot is never auto-promoted", "[pool_store]")
{
    int i;
    fresh_store();
    (void)commit_ok(1u);
    make_session_rec(&g_rec, 2u);
    g_fake.fail_write_after = 1; /* die before the pointer write */
    TEST_ASSERT_EQUAL(STORE_COMMIT_UNCERTAIN,
                      pool_session_store_commit_record(&g_store, &g_rec));
    fk_power_loss();
    store_open();
    /* repeated loads never flip to the staged generation-2 slot */
    for (i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, &g_info));
        TEST_ASSERT_EQUAL_UINT32(1u, g_out.session_id);
        TEST_ASSERT_EQUAL_UINT32(1u, g_info.committed_generation);
        TEST_ASSERT_TRUE(g_info.staged_newer_ignored);
    }
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("staged: slots without a pointer are recovery, not data", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(1u);
    /* remove the pointer, keep the slot: staged/uncommitted layout */
    g_fake.committed[FK_P].present = false;
    TEST_ASSERT_EQUAL(STORE_RECOVERY_REQUIRED,
                      pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_TRUE(pool_store_result_requires_recovery(STORE_RECOVERY_REQUIRED));
    /* commit also refuses to bulldoze the ambiguous evidence */
    make_session_rec(&g_rec, 9u);
    TEST_ASSERT_EQUAL(STORE_RECOVERY_REQUIRED,
                      pool_session_store_commit_record(&g_store, &g_rec));
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("staged: corrupt pointer with two valid slots never guesses", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(1u);
    (void)commit_ok(2u); /* both slots now hold valid records (gens 1 and 2) */
    poke_committed(FK_P, 8u); /* corrupt the pointer generation byte */
    TEST_ASSERT_EQUAL(STORE_ACTIVE_POINTER_INVALID,
                      pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_TRUE(pool_store_result_requires_recovery(STORE_ACTIVE_POINTER_INVALID));
    /* the highest generation was NOT guessed: no session was returned */
    TEST_ASSERT_FALSE(pool_store_result_permits_session_load(STORE_ACTIVE_POINTER_INVALID));
    (void)pool_session_store_deinit(&g_store);
}

/* ================================================================= */
/* I. Active-slot corruption tests                                    */
/* ================================================================= */

TEST_CASE("corrupt: pointer to a missing slot is recovery", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(1u);
    g_fake.committed[FK_A].present = false; /* active slot vanished */
    TEST_ASSERT_EQUAL(STORE_ACTIVE_SLOT_INVALID,
                      pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_TRUE(pool_store_result_requires_recovery(STORE_ACTIVE_SLOT_INVALID));
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("corrupt: active-slot CRC corruption is detected; inactive is harmless", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(1u);
    (void)commit_ok(2u); /* active = B */
    poke_committed(FK_B, 40u);
    TEST_ASSERT_EQUAL(STORE_CORRUPT, pool_session_store_load(&g_store, &g_out, NULL));
    /* restore B; corrupt only the INACTIVE slot A: load unaffected */
    poke_committed(FK_B, 40u);
    poke_committed(FK_A, 40u);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(2u, g_out.session_id);
    TEST_ASSERT_FALSE(g_info.staged_newer_ignored); /* corrupt sibling is not "staged" */
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("corrupt: pointer/slot generation mismatch is recovery", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(1u);
    (void)commit_ok(2u); /* pointer -> B gen 2; slot A holds gen 1 */
    /* rewrite the pointer to name slot A with generation 2 (mismatch) */
    {
        PoolRecordPointer p = { .slot = POOL_RECORD_SLOT_A, .generation = 2u };
        uint8_t pbuf[POOL_RECORD_POINTER_LEN];
        size_t plen = 0;
        TEST_ASSERT_EQUAL(RECORD_OK,
                          pool_record_pointer_encode(&p, pbuf, sizeof(pbuf), &plen));
        memcpy(g_fake.committed[FK_P].bytes, pbuf, plen);
        g_fake.committed[FK_P].len = plen;
    }
    TEST_ASSERT_EQUAL(STORE_RECOVERY_REQUIRED,
                      pool_session_store_load(&g_store, &g_out, NULL));
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("corrupt: unsupported schema in the active slot", "[pool_store]")
{
    fresh_store();
    (void)commit_ok(1u);
    /* bump the schema field and re-CRC so ONLY the version is at fault */
    {
        FakeVal *v = &g_fake.committed[FK_A];
        uint32_t crc;
        v->bytes[4] = 0x02u;
        crc = pool_record_crc32(v->bytes, v->len - 4u);
        v->bytes[v->len - 4u] = (uint8_t)(crc & 0xFFu);
        v->bytes[v->len - 3u] = (uint8_t)((crc >> 8) & 0xFFu);
        v->bytes[v->len - 2u] = (uint8_t)((crc >> 16) & 0xFFu);
        v->bytes[v->len - 1u] = (uint8_t)((crc >> 24) & 0xFFu);
    }
    TEST_ASSERT_EQUAL(STORE_UNSUPPORTED_SCHEMA,
                      pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_TRUE(pool_store_result_requires_recovery(STORE_UNSUPPORTED_SCHEMA));
    /* commit refuses over an unsupported committed state */
    make_session_rec(&g_rec, 9u);
    TEST_ASSERT_EQUAL(STORE_UNSUPPORTED_SCHEMA,
                      pool_session_store_commit_record(&g_store, &g_rec));
    (void)pool_session_store_deinit(&g_store);
}

/* ================================================================= */
/* J. Tombstone tests                                                 */
/* ================================================================= */

TEST_CASE("tombstone: acknowledging COMPLETE commits a cleared state", "[pool_store]")
{
    fresh_store();
    make_complete_rec(&g_rec, 5u);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_clear(&g_store));
    TEST_ASSERT_EQUAL(STORE_CLEARED, pool_session_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(2u, g_info.committed_generation); /* advanced */
    /* clearing again is a state conflict, not an accident */
    TEST_ASSERT_EQUAL(STORE_STATE_CONFLICT, pool_session_store_commit_clear(&g_store));
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("tombstone: crashes before the pointer flip keep the old session", "[pool_store]")
{
    fresh_store();
    make_complete_rec(&g_rec, 5u);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    /* die on the tombstone-pointer write (2nd write of the clear commit) */
    g_fake.fail_write_after = 1;
    TEST_ASSERT_EQUAL(STORE_COMMIT_UNCERTAIN, pool_session_store_commit_clear(&g_store));
    fk_power_loss();
    store_open();
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(5u, g_out.session_id); /* still the old session */
    TEST_ASSERT_EQUAL(POOL_STATE_COMPLETE, g_out.state);
    TEST_ASSERT_TRUE(g_info.staged_newer_ignored);  /* tombstone staged, ignored */
    /* the surviving session can be acknowledged again, successfully */
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_clear(&g_store));
    TEST_ASSERT_EQUAL(STORE_CLEARED, pool_session_store_load(&g_store, NULL, NULL));
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("tombstone: unresolved restore obligations can never be cleared", "[pool_store]")
{
    fresh_store();
    make_session_rec(&g_rec, 5u); /* TARGET_ACTIVE, restore_required = true */
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(STORE_STATE_CONFLICT, pool_session_store_commit_clear(&g_store));
    /* still committed and loadable */
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_TRUE(g_out.restore_required);

    /* RESTORE_FAILED keeps the obligation: also unclearable */
    make_session_rec(&g_rec, 6u);
    g_rec.state = POOL_STATE_RESTORE_FAILED;
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(STORE_STATE_CONFLICT, pool_session_store_commit_clear(&g_store));
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("tombstone: clear on empty/cleared stores is a state conflict", "[pool_store]")
{
    fresh_store();
    TEST_ASSERT_EQUAL(STORE_STATE_CONFLICT, pool_session_store_commit_clear(&g_store));
    make_complete_rec(&g_rec, 5u);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_clear(&g_store));
    TEST_ASSERT_EQUAL(STORE_STATE_CONFLICT, pool_session_store_commit_clear(&g_store));
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("tombstone: pre-mutation CANCELLED and RECOVERY_REQUIRED are clearable", "[pool_store]")
{
    fresh_store();
    make_session_rec(&g_rec, 5u);
    g_rec.state = POOL_STATE_CANCELLED;
    g_rec.restore_required = false;
    g_rec.target_verify.connection_observed = false;
    g_rec.target_verify.mining_observed = false;
    g_rec.target_verify.identity_verified = false;
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_clear(&g_store));
    TEST_ASSERT_EQUAL(STORE_CLEARED, pool_session_store_load(&g_store, NULL, NULL));
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("tombstone: committed tombstone carries no identity or account bytes", "[pool_store]")
{
    fresh_store();
    make_complete_rec(&g_rec, 5u);
    strncpy(g_rec.source.primary.user, "acct-marker.worker",
            sizeof(g_rec.source.primary.user) - 1);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_clear(&g_store));
    {
        int idx = g_fake.committed[FK_A].len == 28u ? FK_A : FK_B;
        TEST_ASSERT_EQUAL_UINT32(28u, (uint32_t)g_fake.committed[idx].len);
        TEST_ASSERT_FALSE(bytes_contain(g_fake.committed[idx].bytes,
                                        g_fake.committed[idx].len, "acct-marker"));
        TEST_ASSERT_FALSE(bytes_contain(g_fake.committed[idx].bytes,
                                        g_fake.committed[idx].len, "example"));
    }
    (void)pool_session_store_deinit(&g_store);
}

/* ================================================================= */
/* O. Property-style store tests                                      */
/* ================================================================= */

TEST_CASE("property: no single-point failure changes the committed record", "[pool_store]")
{
    /* Sweep a failure over every backend call of a commit; after each
     * failed attempt (plus power loss), the committed record must still be
     * the original. This is the primary crash-consistency proof. */
    int inj;
    for (inj = 0; inj < 12; inj++) {
        int kind = inj % 3; /* 0 read, 1 write, 2 commit */
        int nth = inj / 3;  /* fail the nth call of that kind */
        PoolStoreResult res;

        fresh_store();
        (void)commit_ok(1u);
        make_session_rec(&g_rec, 2u);
        if (kind == 0) g_fake.fail_read_after = nth;
        if (kind == 1) g_fake.fail_write_after = nth;
        if (kind == 2) g_fake.fail_commit_after = nth;

        res = pool_session_store_commit_record(&g_store, &g_rec);
        fk_power_loss();
        store_open();
        if (res == STORE_OK) {
            /* injection landed after the pointer verification: new record */
            TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, NULL));
            TEST_ASSERT_EQUAL_UINT32(2u, g_out.session_id);
        } else if (res == STORE_COMMIT_UNCERTAIN) {
            /* pointer phase: reload learns the truth — either old or new,
             * but always a VALID committed record, never a guess */
            TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, NULL));
            TEST_ASSERT_TRUE(g_out.session_id == 1u || g_out.session_id == 2u);
        } else {
            /* pre-pointer failure: the old record must be untouched */
            TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, NULL));
            TEST_ASSERT_EQUAL_UINT32(1u, g_out.session_id);
        }
        (void)pool_session_store_deinit(&g_store);
    }
}

TEST_CASE("property: recovery results never permit a session or mutation", "[pool_store]")
{
    int i;
    for (i = 0; i < (int)POOL_STORE_RESULT__COUNT; i++) {
        PoolStoreResult r = (PoolStoreResult)i;
        if (r != STORE_OK) {
            TEST_ASSERT_FALSE(pool_store_result_permits_session_load(r));
        }
    }
    TEST_ASSERT_TRUE(pool_store_result_permits_session_load(STORE_OK));
    /* recovery classification is exactly the designed set */
    TEST_ASSERT_TRUE(pool_store_result_requires_recovery(STORE_CORRUPT));
    TEST_ASSERT_TRUE(pool_store_result_requires_recovery(STORE_UNSUPPORTED_SCHEMA));
    TEST_ASSERT_TRUE(pool_store_result_requires_recovery(STORE_ACTIVE_POINTER_INVALID));
    TEST_ASSERT_TRUE(pool_store_result_requires_recovery(STORE_ACTIVE_SLOT_INVALID));
    TEST_ASSERT_TRUE(pool_store_result_requires_recovery(STORE_RECOVERY_REQUIRED));
    TEST_ASSERT_TRUE(pool_store_result_requires_recovery(STORE_GENERATION_EXHAUSTED));
    TEST_ASSERT_TRUE(pool_store_result_requires_recovery(STORE_INVALID_RECORD));
    TEST_ASSERT_FALSE(pool_store_result_requires_recovery(STORE_OK));
    TEST_ASSERT_FALSE(pool_store_result_requires_recovery(STORE_EMPTY));
    TEST_ASSERT_FALSE(pool_store_result_requires_recovery(STORE_CLEARED));
    TEST_ASSERT_FALSE(pool_store_result_requires_recovery(STORE_COMMIT_UNCERTAIN));
}

TEST_CASE("privacy: store tokens are clean machine tokens", "[pool_store]")
{
    int i;
    for (i = 0; i < (int)POOL_STORE_RESULT__COUNT; i++) {
        const char *tok = pool_store_result_str((PoolStoreResult)i);
        const char *c;
        TEST_ASSERT_NOT_NULL(tok);
        TEST_ASSERT_TRUE(strlen(tok) > 0 && strlen(tok) < 40);
        for (c = tok; *c != '\0'; c++) {
            TEST_ASSERT_TRUE((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_');
        }
    }
    TEST_ASSERT_EQUAL_STRING("STORE_RESULT_UNKNOWN",
                             pool_store_result_str((PoolStoreResult)999));
}

/* ================================================================= */
/* M. Real ESP-IDF NVS adapter tests (isolated QEMU test NVS)         */
/* ================================================================= */

static PoolStoreNvsBackend g_nvs_backend;

/* One-time NVS init for the QEMU test partition (test harness only — the
 * adapter itself never initializes or erases NVS). */
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
 * (the adapter itself has no erase capability at all). Keeps the real-NVS
 * tests order-independent. */
static void wipe_test_namespace(void)
{
    nvs_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(POOL_STORE_NVS_NAMESPACE, NVS_READWRITE, &h));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_erase_all(h));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(h));
    nvs_close(h);
}

TEST_CASE("nvs: adapter round trip on the dedicated namespace", "[pool_store]")
{
    ensure_test_nvs();
    wipe_test_namespace();
    memset(&g_nvs_backend, 0, sizeof(g_nvs_backend));
    TEST_ASSERT_EQUAL(STORE_OK,
                      pool_session_store_init(&g_store, pool_store_nvs_ops(), &g_nvs_backend));
    /* a fresh namespace loads empty */
    TEST_ASSERT_EQUAL(STORE_EMPTY, pool_session_store_load(&g_store, &g_out, NULL));
    /* commit + load */
    make_session_rec(&g_rec, 501u);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, &g_info));
    TEST_ASSERT_EQUAL_UINT32(501u, g_out.session_id);
    TEST_ASSERT_TRUE(g_out.restore_required);
    /* close and reopen: the record persists in real NVS */
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&g_store));
    memset(&g_nvs_backend, 0, sizeof(g_nvs_backend));
    TEST_ASSERT_EQUAL(STORE_OK,
                      pool_session_store_init(&g_store, pool_store_nvs_ops(), &g_nvs_backend));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(501u, g_out.session_id);
    TEST_ASSERT_EQUAL_STRING("btc.example", g_out.source.primary.host);
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("nvs: A/B update and tombstone clear on real NVS", "[pool_store]")
{
    uint32_t first_gen = 0;
    ensure_test_nvs();
    memset(&g_nvs_backend, 0, sizeof(g_nvs_backend));
    TEST_ASSERT_EQUAL(STORE_OK,
                      pool_session_store_init(&g_store, pool_store_nvs_ops(), &g_nvs_backend));
    /* continue from whatever the previous test left committed */
    (void)pool_session_store_load(&g_store, &g_out, &g_info);
    first_gen = g_info.committed_generation;

    make_complete_rec(&g_rec, 502u);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_TRUE(g_rec.generation > first_gen);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_load(&g_store, &g_out, NULL));
    TEST_ASSERT_EQUAL_UINT32(502u, g_out.session_id);

    /* crash-safe clear on the real adapter */
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_clear(&g_store));
    TEST_ASSERT_EQUAL(STORE_CLEARED, pool_session_store_load(&g_store, NULL, &g_info));
    /* reopen: still cleared */
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&g_store));
    memset(&g_nvs_backend, 0, sizeof(g_nvs_backend));
    TEST_ASSERT_EQUAL(STORE_OK,
                      pool_session_store_init(&g_store, pool_store_nvs_ops(), &g_nvs_backend));
    TEST_ASSERT_EQUAL(STORE_CLEARED, pool_session_store_load(&g_store, NULL, NULL));
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("nvs: the adapter never touches unrelated namespaces", "[pool_store]")
{
    nvs_handle_t other;
    uint32_t sentinel = 0;
    ensure_test_nvs();
    /* plant a sentinel in a DIFFERENT namespace */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("nx_probe", NVS_READWRITE, &other));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u32(other, "sentinel", 0xC0FFEEu));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(other));
    nvs_close(other);

    memset(&g_nvs_backend, 0, sizeof(g_nvs_backend));
    TEST_ASSERT_EQUAL(STORE_OK,
                      pool_session_store_init(&g_store, pool_store_nvs_ops(), &g_nvs_backend));
    make_session_rec(&g_rec, 503u);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&g_store, &g_rec));
    (void)pool_session_store_deinit(&g_store);

    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("nx_probe", NVS_READONLY, &other));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u32(other, "sentinel", &sentinel));
    nvs_close(other);
    TEST_ASSERT_EQUAL_UINT32(0xC0FFEEu, sentinel);
}
