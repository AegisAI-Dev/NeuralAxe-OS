/*
 * Exhaustive deterministic tests for the persisted record codec (Gate B3).
 *
 * No hardware, no networking, no real NVS in this file (the store/adapter
 * suite covers that), no secrets. All hosts/accounts are synthetic
 * "*.example" fixtures. Golden vectors below were derived with an
 * INDEPENDENT Python implementation of the v1 wire layout (zlib CRC32).
 */

#include <string.h>
#include "unity.h"
#include "pool_session_record.h"
#include "pool_time.h"    /* B2 band parity asserts only */
#include "esp_rom_crc.h"  /* CRC equivalence proof only  */

/* Gate B2 / Gate B3 epoch-band parity — test-level only, no production
 * coupling between pool_time and pool_session_store. */
_Static_assert(POOL_RECORD_EPOCH_MIN_S == POOL_TIME_EPOCH_MIN_S,
               "record epoch floor must match the B2 trusted-time band");
_Static_assert(POOL_RECORD_EPOCH_MAX_S == POOL_TIME_EPOCH_MAX_S,
               "record epoch ceiling must match the B2 trusted-time band");

#define EPOCH_A_S 1750000000ull /* mid-2025, inside the sanity band */

/* File-static fixtures: records are ~1 KB — keep them off the task stack. */
static PoolSessionRecord g_rec;
static PoolSessionRecord g_rec2;
static PoolSessionRecord g_rec3;
static uint8_t g_buf[POOL_RECORD_MAX_ENCODED];
static uint8_t g_buf2[POOL_RECORD_MAX_ENCODED];
static PoolSession g_session;

static void fill_endpoint(PoolEndpoint *e, const char *host, uint16_t port, const char *user)
{
    memset(e, 0, sizeof(*e));
    strncpy(e->host, host, sizeof(e->host) - 1);
    e->port = port;
    strncpy(e->user, user, sizeof(e->user) - 1);
    e->protocol = POOL_PROTO_STRATUM_V1;
    e->tls = false;
}

static void fill_identity(PoolConfigIdentity *c, PoolChainType chain, const char *profile,
                          const char *host, uint16_t port, const char *user)
{
    memset(c, 0, sizeof(*c));
    c->chain = chain;
    strncpy(c->profile_id, profile, sizeof(c->profile_id) - 1);
    fill_endpoint(&c->primary, host, port, user);
    c->fallback_enabled = false;
}

/* Baseline: pre-mutation TARGET_SNAPSHOT_COMMITTED session. */
static void make_baseline(PoolSessionRecord *r)
{
    pool_session_record_init(r);
    r->generation = 1u;
    r->session_id = 42u;
    r->b1_model_version = POOL_SESSION_MODEL_VERSION;
    r->state = POOL_STATE_TARGET_SNAPSHOT_COMMITTED;
    fill_identity(&r->source, POOL_CHAIN_BITCOIN, "p1", "btc.example", 3333, "acct.worker");
    fill_identity(&r->target, POOL_CHAIN_BITCOIN_CASH, "p2", "bch.example", 3334, "acct.worker");
    r->password_policy = POOL_SESSION_PW_KEEP_CURRENT;
    r->duration_s = 3600u;
}

/* Post-mutation TARGET_ACTIVE session with full evidence and time facts. */
static void make_active(PoolSessionRecord *r)
{
    make_baseline(r);
    r->state = POOL_STATE_TARGET_ACTIVE;
    r->restore_required = true;
    r->target_verify.connection_observed = true;
    r->target_verify.mining_observed = true;
    r->target_verify.identity_verified = true;
    r->verified_start_valid = true;
    r->verified_start_epoch_s = EPOCH_A_S;
    r->deadline_valid = true;
    r->deadline_epoch_s = EPOCH_A_S + 3600u;
    r->deadline_sync_generation = 1u;
    r->latest_trusted_valid = true;
    r->latest_trusted_epoch_s = EPOCH_A_S + 100u;
}

/* Encode into g_buf; assert success; return length. */
static size_t encode_ok(const PoolSessionRecord *r)
{
    size_t len = 0;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_encode(r, g_buf, sizeof(g_buf), &len));
    TEST_ASSERT_TRUE(len >= POOL_RECORD_HEADER_LEN + POOL_RECORD_CRC_LEN);
    return len;
}

/* Round trip g_rec -> bytes -> g_rec2 and assert byte-identical models. */
static void roundtrip_equal(void)
{
    size_t len = encode_ok(&g_rec);
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_decode(g_buf, len, &g_rec2));
    TEST_ASSERT_EQUAL(0, memcmp(&g_rec, &g_rec2, sizeof(g_rec)));
}

/* Corrupt one byte of an encoding, expect decode failure, restore it. */
static void assert_byte_corruption_detected(uint8_t *buf, size_t len, size_t at)
{
    uint8_t keep = buf[at];
    buf[at] = (uint8_t)(keep ^ 0xFFu);
    TEST_ASSERT_TRUE(pool_session_record_decode(buf, len, &g_rec3) != RECORD_OK);
    buf[at] = keep;
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

static void assert_clean_token(const char *tok)
{
    const char *c;
    TEST_ASSERT_NOT_NULL(tok);
    TEST_ASSERT_TRUE(strlen(tok) > 0u && strlen(tok) < 48u);
    for (c = tok; *c != '\0'; c++) {
        TEST_ASSERT_TRUE((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_');
    }
    TEST_ASSERT_NULL(strstr(tok, "EXAMPLE"));
}

/* ================================================================= */
/* A. Record-model tests                                              */
/* ================================================================= */

TEST_CASE("record: init produces a cleared SESSION model", "[pool_record]")
{
    memset(&g_rec, 0xAA, sizeof(g_rec));
    pool_session_record_init(&g_rec);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)POOL_RECORD_KIND_SESSION, g_rec.kind);
    TEST_ASSERT_EQUAL(POOL_STATE_IDLE, g_rec.state);
    TEST_ASSERT_EQUAL_UINT32(0u, g_rec.generation);
    TEST_ASSERT_FALSE(g_rec.restore_required);
    TEST_ASSERT_FALSE(g_rec.latest_trusted_valid);
    TEST_ASSERT_EQUAL_UINT8(0u, g_rec.reboot_count);
}

TEST_CASE("record: tombstone carries no identity and no obligation", "[pool_record]")
{
    memset(&g_rec, 0xAA, sizeof(g_rec));
    pool_session_record_init_tombstone(&g_rec);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)POOL_RECORD_KIND_TOMBSTONE, g_rec.kind);
    TEST_ASSERT_EQUAL_UINT32(0u, g_rec.session_id);
    TEST_ASSERT_EQUAL_STRING("", g_rec.source.primary.host);
    TEST_ASSERT_EQUAL_STRING("", g_rec.source.primary.user);
    TEST_ASSERT_EQUAL_STRING("", g_rec.target.primary.host);
    TEST_ASSERT_FALSE(g_rec.restore_required);
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
}

TEST_CASE("record: format constants are pinned", "[pool_record]")
{
    TEST_ASSERT_EQUAL_UINT32(0x4E585053u, POOL_RECORD_MAGIC);
    TEST_ASSERT_EQUAL_UINT32(0x4E585054u, POOL_RECORD_POINTER_MAGIC);
    TEST_ASSERT_EQUAL_UINT32(1u, POOL_RECORD_SCHEMA_VERSION);
    TEST_ASSERT_EQUAL_UINT32(24u, POOL_RECORD_HEADER_LEN);
    TEST_ASSERT_EQUAL_UINT32(16u, POOL_RECORD_POINTER_LEN);
    TEST_ASSERT_EQUAL_UINT32(1024u, POOL_RECORD_MAX_ENCODED);
    TEST_ASSERT_EQUAL_UINT64(1735689600ull, POOL_RECORD_EPOCH_MIN_S);
    TEST_ASSERT_EQUAL_UINT64(4102444800ull, POOL_RECORD_EPOCH_MAX_S);
}

TEST_CASE("record: baseline and active fixtures validate", "[pool_record]")
{
    make_baseline(&g_rec);
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
    make_active(&g_rec);
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
    TEST_ASSERT_EQUAL(RECORD_ERR_INVALID_ARGUMENT, pool_session_record_validate(NULL));
}

/* ================================================================= */
/* B. Serializer tests                                                */
/* ================================================================= */

TEST_CASE("codec: CRC32 matches the standard and the ROM implementation", "[pool_record]")
{
    static const uint8_t vec[] = "123456789";
    uint32_t ours = pool_record_crc32(vec, 9);
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926u, ours); /* CRC-32/ISO-HDLC check value */
    TEST_ASSERT_EQUAL_UINT32(esp_rom_crc32_le(0, vec, 9), ours);
    /* equivalence over a full encoded record too */
    make_active(&g_rec);
    {
        size_t len = encode_ok(&g_rec);
        TEST_ASSERT_EQUAL_UINT32(esp_rom_crc32_le(0, g_buf, (uint32_t)(len - 4u)),
                                 pool_record_crc32(g_buf, len - 4u));
    }
}

TEST_CASE("codec: deterministic round trip and stable bytes", "[pool_record]")
{
    size_t l1, l2;
    make_baseline(&g_rec);
    roundtrip_equal();
    /* encoding twice yields byte-identical output */
    l1 = encode_ok(&g_rec);
    memcpy(g_buf2, g_buf, l1);
    l2 = encode_ok(&g_rec);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)l1, (uint32_t)l2);
    TEST_ASSERT_EQUAL(0, memcmp(g_buf, g_buf2, l1));
}

TEST_CASE("codec: restore_required and full active state survive the round trip", "[pool_record]")
{
    make_active(&g_rec);
    roundtrip_equal();
    TEST_ASSERT_TRUE(g_rec2.restore_required);
    TEST_ASSERT_TRUE(g_rec2.target_verify.identity_verified);
    TEST_ASSERT_TRUE(g_rec2.deadline_valid);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 3600u, g_rec2.deadline_epoch_s);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 100u, g_rec2.latest_trusted_epoch_s);
    TEST_ASSERT_EQUAL_STRING("btc.example", g_rec2.source.primary.host);
    TEST_ASSERT_EQUAL_STRING("acct.worker", g_rec2.source.primary.user);
}

TEST_CASE("codec: string length extremes round trip", "[pool_record]")
{
    int i;
    make_baseline(&g_rec);
    /* 1-char host, empty user, empty profile (rebuilt canonically — the
     * codec zero-pads decoded strings, so fixtures must be canonical too) */
    fill_identity(&g_rec.source, POOL_CHAIN_BITCOIN, "", "h", 3333, "");
    roundtrip_equal();
    /* maximum-length host (79), user (127), profile (23) */
    make_baseline(&g_rec);
    for (i = 0; i < (int)POOL_SESSION_HOST_MAX - 1; i++) g_rec.source.primary.host[i] = 'h';
    g_rec.source.primary.host[POOL_SESSION_HOST_MAX - 1] = '\0';
    for (i = 0; i < (int)POOL_SESSION_USER_MAX - 1; i++) g_rec.source.primary.user[i] = 'u';
    g_rec.source.primary.user[POOL_SESSION_USER_MAX - 1] = '\0';
    for (i = 0; i < (int)POOL_SESSION_PROFILE_ID_MAX - 1; i++) g_rec.source.profile_id[i] = 'p';
    g_rec.source.profile_id[POOL_SESSION_PROFILE_ID_MAX - 1] = '\0';
    roundtrip_equal();
}

TEST_CASE("codec: fallback combinations round trip", "[pool_record]")
{
    make_baseline(&g_rec);
    g_rec.source.fallback_enabled = true;
    fill_endpoint(&g_rec.source.fallback, "btc-fb.example", 3335, "acct.fb");
    g_rec.source.fallback.protocol = POOL_PROTO_STRATUM_V2;
    g_rec.source.fallback.tls = true;
    g_rec.target.fallback_enabled = true;
    fill_endpoint(&g_rec.target.fallback, "bch-fb.example", 3336, "");
    roundtrip_equal();
    TEST_ASSERT_TRUE(g_rec2.source.fallback.tls);
    TEST_ASSERT_EQUAL(POOL_PROTO_STRATUM_V2, g_rec2.source.fallback.protocol);
    TEST_ASSERT_EQUAL_STRING("bch-fb.example", g_rec2.target.fallback.host);
}

TEST_CASE("codec: every persistent state round trips", "[pool_record]")
{
    static const PoolSessionState states[] = {
        POOL_STATE_TARGET_SNAPSHOT_COMMITTED, POOL_STATE_APPLYING_TARGET,
        POOL_STATE_TARGET_ACTIVE, POOL_STATE_RESTORE_DUE, POOL_STATE_APPLYING_RESTORE,
        POOL_STATE_COMPLETE, POOL_STATE_TARGET_FAILED, POOL_STATE_RESTORE_FAILED,
        POOL_STATE_INTERRUPTED, POOL_STATE_RECOVERY_REQUIRED, POOL_STATE_CANCELLED,
    };
    size_t i;
    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        make_baseline(&g_rec);
        g_rec.state = states[i];
        /* satisfy per-state invariants */
        if (states[i] == POOL_STATE_TARGET_SNAPSHOT_COMMITTED ||
            states[i] == POOL_STATE_CANCELLED) {
            g_rec.restore_required = false;
        } else if (states[i] == POOL_STATE_COMPLETE) {
            g_rec.restore_required = false;
            g_rec.restore_verify.connection_observed = true;
            g_rec.restore_verify.mining_observed = true;
            g_rec.restore_verify.identity_verified = true;
        } else if (states[i] == POOL_STATE_RECOVERY_REQUIRED) {
            g_rec.restore_required = false; /* pre-mutation variant */
        } else {
            g_rec.restore_required = true;
            if (states[i] == POOL_STATE_TARGET_ACTIVE) {
                g_rec.target_verify.connection_observed = true;
                g_rec.target_verify.mining_observed = true;
                g_rec.target_verify.identity_verified = true;
            }
        }
        roundtrip_equal();
        TEST_ASSERT_EQUAL(states[i], g_rec2.state);
    }
}

TEST_CASE("codec: chains, protocols, retries and failure codes round trip", "[pool_record]")
{
    make_baseline(&g_rec);
    g_rec.source.chain = POOL_CHAIN_CUSTOM_UNKNOWN;
    g_rec.target.chain = POOL_CHAIN_BITCOIN;
    g_rec.source.primary.protocol = POOL_PROTO_STRATUM_V2;
    g_rec.source.primary.tls = true;
    g_rec.retries.target_apply = POOL_SESSION_MAX_TARGET_APPLY_RETRIES;
    g_rec.retries.restore_verify = POOL_SESSION_MAX_RESTORE_VERIFY_RETRIES;
    g_rec.last_failure_code = (uint16_t)(POOL_SESSION_ERR__COUNT - 1u);
    roundtrip_equal();
    TEST_ASSERT_EQUAL(POOL_CHAIN_CUSTOM_UNKNOWN, g_rec2.source.chain);
    TEST_ASSERT_EQUAL_UINT8(3u, g_rec2.retries.target_apply);
}

TEST_CASE("codec: epoch valid/invalid combinations round trip", "[pool_record]")
{
    /* none */
    make_baseline(&g_rec);
    roundtrip_equal();
    /* start only */
    make_baseline(&g_rec);
    g_rec.verified_start_valid = true;
    g_rec.verified_start_epoch_s = EPOCH_A_S;
    roundtrip_equal();
    /* start + deadline + latest (active fixture) */
    make_active(&g_rec);
    roundtrip_equal();
    /* latest only */
    make_baseline(&g_rec);
    g_rec.latest_trusted_valid = true;
    g_rec.latest_trusted_epoch_s = EPOCH_A_S;
    roundtrip_equal();
}

TEST_CASE("codec: golden tombstone vector is byte-exact", "[pool_record]")
{
    /* Independently derived (Python/zlib): generation 1 tombstone. */
    static const uint8_t golden[28] = {
        0x53, 0x50, 0x58, 0x4E, 0x01, 0x00, 0x18, 0x00,
        0x1C, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2D, 0x18, 0x14, 0x9F,
    };
    size_t len = 0;
    pool_session_record_init_tombstone(&g_rec);
    g_rec.generation = 1u;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_encode(&g_rec, g_buf, sizeof(g_buf), &len));
    TEST_ASSERT_EQUAL_UINT32(28u, (uint32_t)len);
    TEST_ASSERT_EQUAL(0, memcmp(golden, g_buf, 28));
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_decode(golden, 28, &g_rec2));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)POOL_RECORD_KIND_TOMBSTONE, g_rec2.kind);
    TEST_ASSERT_EQUAL_UINT32(1u, g_rec2.generation);
}

TEST_CASE("codec: golden session vector pins length, order and CRC", "[pool_record]")
{
    /* Independently derived (Python/zlib): payload 158, total 186,
     * CRC 0x12B214A6 for this exact fixture. */
    size_t len = 0;
    pool_session_record_init(&g_rec);
    g_rec.generation = 7u;
    g_rec.session_id = 0x11223344u;
    g_rec.b1_model_version = 1u;
    g_rec.state = POOL_STATE_TARGET_SNAPSHOT_COMMITTED;
    fill_identity(&g_rec.source, POOL_CHAIN_BITCOIN, "p1", "btc-golden.example", 3333,
                  "acct.worker");
    fill_identity(&g_rec.target, POOL_CHAIN_BITCOIN_CASH, "p2", "bch-golden.example", 3334,
                  "acct.worker");
    g_rec.duration_s = 3600u;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_encode(&g_rec, g_buf, sizeof(g_buf), &len));
    TEST_ASSERT_EQUAL_UINT32(186u, (uint32_t)len);
    /* header spot checks: magic, schema, total, generation, kind, payload_len */
    TEST_ASSERT_EQUAL_UINT8(0x53u, g_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x50u, g_buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x58u, g_buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x4Eu, g_buf[3]);
    TEST_ASSERT_EQUAL_UINT8(0x01u, g_buf[4]);
    TEST_ASSERT_EQUAL_UINT8(186u, g_buf[8]);
    TEST_ASSERT_EQUAL_UINT8(7u, g_buf[12]);
    TEST_ASSERT_EQUAL_UINT8(0x01u, g_buf[16]); /* kind SESSION */
    TEST_ASSERT_EQUAL_UINT8(158u, g_buf[20]);  /* payload_len   */
    /* payload spot checks: session id LE, model LE, state, pw */
    TEST_ASSERT_EQUAL_UINT8(0x44u, g_buf[24]);
    TEST_ASSERT_EQUAL_UINT8(0x33u, g_buf[25]);
    TEST_ASSERT_EQUAL_UINT8(0x22u, g_buf[26]);
    TEST_ASSERT_EQUAL_UINT8(0x11u, g_buf[27]);
    TEST_ASSERT_EQUAL_UINT8(0x01u, g_buf[28]);
    TEST_ASSERT_EQUAL_UINT8(0x02u, g_buf[32]); /* state */
    TEST_ASSERT_EQUAL_UINT8(0x00u, g_buf[33]); /* pw KEEP */
    /* pinned CRC (little-endian at the tail) */
    TEST_ASSERT_EQUAL_UINT8(0xA6u, g_buf[182]);
    TEST_ASSERT_EQUAL_UINT8(0x14u, g_buf[183]);
    TEST_ASSERT_EQUAL_UINT8(0xB2u, g_buf[184]);
    TEST_ASSERT_EQUAL_UINT8(0x12u, g_buf[185]);
    TEST_ASSERT_EQUAL_UINT32(0x12B214A6u, pool_record_crc32(g_buf, len - 4u));
}

/* ================================================================= */
/* C. Decoder rejection tests                                         */
/* ================================================================= */

TEST_CASE("decode: empty and truncated blobs rejected", "[pool_record]")
{
    make_baseline(&g_rec);
    (void)encode_ok(&g_rec);
    TEST_ASSERT_EQUAL(RECORD_ERR_TRUNCATED, pool_session_record_decode(g_buf, 0, &g_rec2));
    TEST_ASSERT_EQUAL(RECORD_ERR_TRUNCATED, pool_session_record_decode(g_buf, 1, &g_rec2));
    TEST_ASSERT_EQUAL(RECORD_ERR_TRUNCATED, pool_session_record_decode(g_buf, 27, &g_rec2));
    TEST_ASSERT_EQUAL(RECORD_ERR_INVALID_ARGUMENT, pool_session_record_decode(NULL, 100, &g_rec2));
    TEST_ASSERT_EQUAL(RECORD_ERR_INVALID_ARGUMENT, pool_session_record_decode(g_buf, 100, NULL));
}

TEST_CASE("decode: bad magic and unsupported schema rejected", "[pool_record]")
{
    size_t len;
    make_baseline(&g_rec);
    len = encode_ok(&g_rec);

    memcpy(g_buf2, g_buf, len);
    g_buf2[0] ^= 0x01u;
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_MAGIC, pool_session_record_decode(g_buf2, len, &g_rec2));

    memcpy(g_buf2, g_buf, len);
    g_buf2[4] = 0x02u; /* schema 2 */
    TEST_ASSERT_EQUAL(RECORD_ERR_UNSUPPORTED_SCHEMA,
                      pool_session_record_decode(g_buf2, len, &g_rec2));

    memcpy(g_buf2, g_buf, len);
    g_buf2[6] = 0x20u; /* header_len 32 */
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_LENGTH, pool_session_record_decode(g_buf2, len, &g_rec2));
}

TEST_CASE("decode: unknown flags and reserved bits rejected", "[pool_record]")
{
    size_t len;
    uint32_t crc;
    make_baseline(&g_rec);
    len = encode_ok(&g_rec);

    memcpy(g_buf2, g_buf, len);
    g_buf2[17] = 0x01u; /* unknown flag bit */
    crc = pool_record_crc32(g_buf2, len - 4u); /* re-CRC so only the flag is at fault */
    g_buf2[len - 4u] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3u] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1u] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL(RECORD_ERR_UNKNOWN_FLAGS, pool_session_record_decode(g_buf2, len, &g_rec2));

    memcpy(g_buf2, g_buf, len);
    g_buf2[18] = 0x01u; /* reserved */
    crc = pool_record_crc32(g_buf2, len - 4u);
    g_buf2[len - 4u] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3u] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1u] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL(RECORD_ERR_UNKNOWN_FLAGS, pool_session_record_decode(g_buf2, len, &g_rec2));
}

TEST_CASE("decode: length inconsistencies rejected", "[pool_record]")
{
    size_t len;
    make_baseline(&g_rec);
    len = encode_ok(&g_rec);

    /* buffer shorter than declared total */
    TEST_ASSERT_EQUAL(RECORD_ERR_TRUNCATED, pool_session_record_decode(g_buf, len - 1u, &g_rec2));
    /* trailing byte after declared total */
    memcpy(g_buf2, g_buf, len);
    g_buf2[len] = 0x00u;
    TEST_ASSERT_EQUAL(RECORD_ERR_TRAILING_BYTES,
                      pool_session_record_decode(g_buf2, len + 1u, &g_rec2));
    /* oversized blob rejected before any parsing */
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_LENGTH,
                      pool_session_record_decode(g_buf, POOL_RECORD_MAX_ENCODED + 1u, &g_rec2));
}

TEST_CASE("decode: CRC mismatch rejected", "[pool_record]")
{
    size_t len;
    make_baseline(&g_rec);
    len = encode_ok(&g_rec);
    memcpy(g_buf2, g_buf, len);
    g_buf2[30] ^= 0x01u; /* payload bit */
    TEST_ASSERT_EQUAL(RECORD_ERR_CRC_MISMATCH, pool_session_record_decode(g_buf2, len, &g_rec2));
    memcpy(g_buf2, g_buf, len);
    g_buf2[len - 1u] ^= 0x80u; /* CRC field itself */
    TEST_ASSERT_EQUAL(RECORD_ERR_CRC_MISMATCH, pool_session_record_decode(g_buf2, len, &g_rec2));
}

TEST_CASE("decode: generation zero and unknown kinds rejected", "[pool_record]")
{
    size_t len;
    uint32_t crc;
    make_baseline(&g_rec);
    len = encode_ok(&g_rec);

    memcpy(g_buf2, g_buf, len);
    memset(&g_buf2[12], 0, 4); /* generation 0 */
    crc = pool_record_crc32(g_buf2, len - 4u);
    g_buf2[len - 4u] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3u] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1u] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_GENERATION,
                      pool_session_record_decode(g_buf2, len, &g_rec2));

    memcpy(g_buf2, g_buf, len);
    g_buf2[16] = 0x07u; /* unknown kind */
    crc = pool_record_crc32(g_buf2, len - 4u);
    g_buf2[len - 4u] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3u] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1u] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_KIND, pool_session_record_decode(g_buf2, len, &g_rec2));
}

TEST_CASE("decode: tombstone with payload rejected", "[pool_record]")
{
    size_t len;
    uint32_t crc;
    /* craft: session encoding relabeled as a tombstone (payload kept) */
    make_baseline(&g_rec);
    len = encode_ok(&g_rec);
    memcpy(g_buf2, g_buf, len);
    g_buf2[16] = (uint8_t)POOL_RECORD_KIND_TOMBSTONE;
    crc = pool_record_crc32(g_buf2, len - 4u);
    g_buf2[len - 4u] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3u] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1u] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL(RECORD_ERR_TOMBSTONE_MALFORMED,
                      pool_session_record_decode(g_buf2, len, &g_rec2));
}

TEST_CASE("decode: invalid field bytes rejected with specific codes", "[pool_record]")
{
    /* build variants through the encoder, then corrupt one field + re-CRC */
    size_t len;
    uint32_t crc;
    make_baseline(&g_rec);
    len = encode_ok(&g_rec);

    /* state byte out of range (offset 32 per the golden layout) */
    memcpy(g_buf2, g_buf, len);
    g_buf2[32] = 0x63u;
    crc = pool_record_crc32(g_buf2, len - 4u);
    g_buf2[len - 4u] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3u] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1u] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_STATE, pool_session_record_decode(g_buf2, len, &g_rec2));

    /* password-policy byte not KEEP */
    memcpy(g_buf2, g_buf, len);
    g_buf2[33] = 0x01u; /* REPLACE tag on the wire */
    crc = pool_record_crc32(g_buf2, len - 4u);
    g_buf2[len - 4u] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3u] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1u] = (uint8_t)((crc >> 24) & 0xFFu);
    /* decodes as enum value 1 (< COUNT) then fails semantic validation */
    TEST_ASSERT_EQUAL(RECORD_ERR_PW_MODE_UNSUPPORTED,
                      pool_session_record_decode(g_buf2, len, &g_rec2));

    /* chain byte out of range (offset 34 = first identity byte) */
    memcpy(g_buf2, g_buf, len);
    g_buf2[34] = 0x09u;
    crc = pool_record_crc32(g_buf2, len - 4u);
    g_buf2[len - 4u] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3u] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1u] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_CHAIN, pool_session_record_decode(g_buf2, len, &g_rec2));

    /* string length byte beyond its bound (profile len at offset 35) */
    memcpy(g_buf2, g_buf, len);
    g_buf2[35] = 0xF0u;
    crc = pool_record_crc32(g_buf2, len - 4u);
    g_buf2[len - 4u] = (uint8_t)(crc & 0xFFu);
    g_buf2[len - 3u] = (uint8_t)((crc >> 8) & 0xFFu);
    g_buf2[len - 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    g_buf2[len - 1u] = (uint8_t)((crc >> 24) & 0xFFu);
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_STRING, pool_session_record_decode(g_buf2, len, &g_rec2));
}

TEST_CASE("decode: invalid port and control characters rejected", "[pool_record]")
{
    /* invalid primary port: build via model (validator path) */
    make_baseline(&g_rec);
    g_rec.source.primary.port = 0u;
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_PORT, pool_session_record_validate(&g_rec));
    /* control character inside host: model validation rejects */
    make_baseline(&g_rec);
    g_rec.source.primary.host[2] = (char)0x07;
    TEST_ASSERT_EQUAL(RECORD_ERR_IDENTITY_INVALID, pool_session_record_validate(&g_rec));
    /* NUL-free unterminated host: model validation rejects */
    make_baseline(&g_rec);
    memset(g_rec.source.primary.host, 'h', sizeof(g_rec.source.primary.host));
    TEST_ASSERT_EQUAL(RECORD_ERR_IDENTITY_INVALID, pool_session_record_validate(&g_rec));
}

/* ================================================================= */
/* D. Semantic-invariant tests                                        */
/* ================================================================= */

TEST_CASE("semantic: target-active without full verification rejected", "[pool_record]")
{
    make_active(&g_rec);
    g_rec.target_verify.identity_verified = false;
    TEST_ASSERT_EQUAL(RECORD_ERR_VERIFY_FLAGS_INVALID, pool_session_record_validate(&g_rec));
    make_active(&g_rec);
    g_rec.target_verify.mining_observed = false;
    TEST_ASSERT_EQUAL(RECORD_ERR_VERIFY_FLAGS_INVALID, pool_session_record_validate(&g_rec));
}

TEST_CASE("semantic: COMPLETE requires restore proof and a discharged obligation", "[pool_record]")
{
    make_baseline(&g_rec);
    g_rec.state = POOL_STATE_COMPLETE;
    g_rec.restore_required = false;
    /* missing restore verification */
    TEST_ASSERT_EQUAL(RECORD_ERR_VERIFY_FLAGS_INVALID, pool_session_record_validate(&g_rec));
    g_rec.restore_verify.connection_observed = true;
    g_rec.restore_verify.mining_observed = true;
    g_rec.restore_verify.identity_verified = true;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
    /* COMPLETE with an unresolved obligation is contradictory */
    g_rec.restore_required = true;
    TEST_ASSERT_EQUAL(RECORD_ERR_OBLIGATION_VIOLATION, pool_session_record_validate(&g_rec));
}

TEST_CASE("semantic: obligation required in every post-mutation state", "[pool_record]")
{
    static const PoolSessionState post[] = {
        POOL_STATE_APPLYING_TARGET, POOL_STATE_TARGET_ACTIVE, POOL_STATE_RESTORE_DUE,
        POOL_STATE_APPLYING_RESTORE, POOL_STATE_TARGET_FAILED, POOL_STATE_RESTORE_FAILED,
        POOL_STATE_INTERRUPTED,
    };
    size_t i;
    for (i = 0; i < sizeof(post) / sizeof(post[0]); i++) {
        make_active(&g_rec);
        g_rec.state = post[i];
        if (post[i] != POOL_STATE_TARGET_ACTIVE) {
            /* only TARGET_ACTIVE demands full target verification */
            g_rec.target_verify.identity_verified = true;
        }
        g_rec.restore_required = false;
        TEST_ASSERT_EQUAL(RECORD_ERR_OBLIGATION_VIOLATION, pool_session_record_validate(&g_rec));
        g_rec.restore_required = true;
        TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
    }
}

TEST_CASE("semantic: CANCELLED persists only pre-mutation", "[pool_record]")
{
    make_baseline(&g_rec);
    g_rec.state = POOL_STATE_CANCELLED;
    g_rec.restore_required = false;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
    /* a cancelled-after-mutation record is unrepresentable on flash */
    g_rec.restore_required = true;
    TEST_ASSERT_EQUAL(RECORD_ERR_OBLIGATION_VIOLATION, pool_session_record_validate(&g_rec));
}

TEST_CASE("semantic: RECOVERY_REQUIRED accepts both obligation values", "[pool_record]")
{
    make_baseline(&g_rec);
    g_rec.state = POOL_STATE_RECOVERY_REQUIRED;
    g_rec.restore_required = false;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
    g_rec.restore_required = true;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
}

TEST_CASE("semantic: ephemeral states and source==target rejected", "[pool_record]")
{
    static const PoolSessionState eph[] = {
        POOL_STATE_IDLE, POOL_STATE_PREPARING, POOL_STATE_RESTARTING_FOR_TARGET,
        POOL_STATE_VERIFYING_TARGET, POOL_STATE_RESTARTING_FOR_RESTORE,
        POOL_STATE_VERIFYING_RESTORE,
    };
    size_t i;
    for (i = 0; i < sizeof(eph) / sizeof(eph[0]); i++) {
        make_baseline(&g_rec);
        g_rec.state = eph[i];
        TEST_ASSERT_EQUAL(RECORD_ERR_BAD_STATE, pool_session_record_validate(&g_rec));
    }
    make_baseline(&g_rec);
    g_rec.target = g_rec.source;
    TEST_ASSERT_EQUAL(RECORD_ERR_IDENTITY_EQUAL, pool_session_record_validate(&g_rec));
}

TEST_CASE("semantic: epoch relations enforced", "[pool_record]")
{
    /* deadline requires a verified start */
    make_baseline(&g_rec);
    g_rec.deadline_valid = true;
    g_rec.deadline_epoch_s = EPOCH_A_S;
    TEST_ASSERT_EQUAL(RECORD_ERR_EPOCH_INVALID, pool_session_record_validate(&g_rec));
    /* deadline before start */
    make_active(&g_rec);
    g_rec.deadline_epoch_s = g_rec.verified_start_epoch_s - 1u;
    TEST_ASSERT_EQUAL(RECORD_ERR_EPOCH_INVALID, pool_session_record_validate(&g_rec));
    /* latest trusted before start */
    make_active(&g_rec);
    g_rec.latest_trusted_epoch_s = g_rec.verified_start_epoch_s - 1u;
    TEST_ASSERT_EQUAL(RECORD_ERR_EPOCH_REGRESSION, pool_session_record_validate(&g_rec));
    /* band violations */
    make_active(&g_rec);
    g_rec.verified_start_epoch_s = POOL_RECORD_EPOCH_MIN_S - 1u;
    TEST_ASSERT_EQUAL(RECORD_ERR_EPOCH_INVALID, pool_session_record_validate(&g_rec));
    /* invalid-flag epochs must be canonically zero */
    make_baseline(&g_rec);
    g_rec.verified_start_epoch_s = EPOCH_A_S; /* valid flag false */
    TEST_ASSERT_EQUAL(RECORD_ERR_EPOCH_INVALID, pool_session_record_validate(&g_rec));
}

TEST_CASE("semantic: counters, ids, durations, layering bounds", "[pool_record]")
{
    make_baseline(&g_rec);
    g_rec.session_id = 0u;
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_SESSION_ID, pool_session_record_validate(&g_rec));

    make_baseline(&g_rec);
    g_rec.last_failure_code = (uint16_t)POOL_SESSION_ERR__COUNT;
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_FAILURE_CODE, pool_session_record_validate(&g_rec));

    make_baseline(&g_rec);
    g_rec.retries.target_verify = 4u;
    TEST_ASSERT_EQUAL(RECORD_ERR_RETRY_OVERFLOW, pool_session_record_validate(&g_rec));

    make_baseline(&g_rec);
    g_rec.duration_s = POOL_SESSION_MIN_DURATION_S - 1u;
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_DURATION, pool_session_record_validate(&g_rec));
    g_rec.duration_s = POOL_SESSION_MAX_DURATION_S + 1u;
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_DURATION, pool_session_record_validate(&g_rec));

    make_baseline(&g_rec);
    g_rec.reboot_count = POOL_RECORD_REBOOT_COUNT_MAX + 1u;
    TEST_ASSERT_EQUAL(RECORD_ERR_COUNTER_OVERFLOW, pool_session_record_validate(&g_rec));

    make_baseline(&g_rec);
    g_rec.target_verify.mining_observed = true; /* without connection */
    TEST_ASSERT_EQUAL(RECORD_ERR_VERIFY_FLAGS_INVALID, pool_session_record_validate(&g_rec));

    make_baseline(&g_rec);
    g_rec.b1_model_version = 99u;
    TEST_ASSERT_EQUAL(RECORD_ERR_UNSUPPORTED_SCHEMA, pool_session_record_validate(&g_rec));

    make_baseline(&g_rec);
    g_rec.password_policy = POOL_SESSION_PW_REPLACE;
    TEST_ASSERT_EQUAL(RECORD_ERR_PW_MODE_UNSUPPORTED, pool_session_record_validate(&g_rec));
}

TEST_CASE("semantic: fallback combinations enforced", "[pool_record]")
{
    /* enabled fallback must be a full endpoint */
    make_baseline(&g_rec);
    g_rec.source.fallback_enabled = true;
    TEST_ASSERT_EQUAL(RECORD_ERR_IDENTITY_INVALID, pool_session_record_validate(&g_rec));
    /* disabled fallback must be canonically empty */
    make_baseline(&g_rec);
    g_rec.source.fallback_enabled = false;
    strncpy(g_rec.source.fallback.host, "stale.example",
            sizeof(g_rec.source.fallback.host) - 1);
    TEST_ASSERT_EQUAL(RECORD_ERR_IDENTITY_INVALID, pool_session_record_validate(&g_rec));
}

TEST_CASE("semantic: hostname never determines chain", "[pool_record]")
{
    /* identical hostnames with different explicit chains: both valid — the
     * chain is an independent, explicit label (2M.1A Invariant 14). */
    make_baseline(&g_rec);
    fill_identity(&g_rec.source, POOL_CHAIN_BITCOIN, "", "pool-x.example", 3333, "u");
    fill_identity(&g_rec.target, POOL_CHAIN_BITCOIN_CASH, "", "pool-x.example", 3334, "u");
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
    /* a "btc-looking" hostname with a Custom chain stays Custom */
    make_baseline(&g_rec);
    g_rec.source.chain = POOL_CHAIN_CUSTOM_UNKNOWN;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));
    roundtrip_equal();
    TEST_ASSERT_EQUAL(POOL_CHAIN_CUSTOM_UNKNOWN, g_rec2.source.chain);
}

/* ================================================================= */
/* E. Pointer codec tests                                             */
/* ================================================================= */

TEST_CASE("pointer: golden vectors and A/B round trips", "[pool_record]")
{
    /* Independently derived (Python/zlib): slot A, generation 1. */
    static const uint8_t golden_a[16] = {
        0x54, 0x50, 0x58, 0x4E, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0xEA, 0x03, 0x93, 0xF4,
    };
    PoolRecordPointer p, q;
    uint8_t buf[POOL_RECORD_POINTER_LEN];
    size_t len = 0;

    p.slot = POOL_RECORD_SLOT_A;
    p.generation = 1u;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_record_pointer_encode(&p, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_UINT32(16u, (uint32_t)len);
    TEST_ASSERT_EQUAL(0, memcmp(golden_a, buf, 16));
    TEST_ASSERT_EQUAL(RECORD_OK, pool_record_pointer_decode(buf, len, &q));
    TEST_ASSERT_EQUAL_UINT8(POOL_RECORD_SLOT_A, q.slot);
    TEST_ASSERT_EQUAL_UINT32(1u, q.generation);

    p.slot = POOL_RECORD_SLOT_B;
    p.generation = 2u;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_record_pointer_encode(&p, buf, sizeof(buf), &len));
    /* pinned CRC for slot B / generation 2 */
    TEST_ASSERT_EQUAL_UINT8(0xA1u, buf[12]);
    TEST_ASSERT_EQUAL_UINT8(0x7Fu, buf[13]);
    TEST_ASSERT_EQUAL_UINT8(0x7Au, buf[14]);
    TEST_ASSERT_EQUAL_UINT8(0x2Du, buf[15]);
    TEST_ASSERT_EQUAL(RECORD_OK, pool_record_pointer_decode(buf, len, &q));
    TEST_ASSERT_EQUAL_UINT8(POOL_RECORD_SLOT_B, q.slot);
    TEST_ASSERT_EQUAL_UINT32(2u, q.generation);
}

TEST_CASE("pointer: invalid inputs rejected", "[pool_record]")
{
    PoolRecordPointer p, q;
    uint8_t buf[POOL_RECORD_POINTER_LEN];
    size_t len = 0;

    p.slot = 2u; /* invalid slot */
    p.generation = 1u;
    TEST_ASSERT_EQUAL(RECORD_ERR_INVALID_ARGUMENT,
                      pool_record_pointer_encode(&p, buf, sizeof(buf), &len));
    p.slot = POOL_RECORD_SLOT_A;
    p.generation = 0u; /* generation zero */
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_GENERATION,
                      pool_record_pointer_encode(&p, buf, sizeof(buf), &len));
    p.generation = 1u;
    TEST_ASSERT_EQUAL(RECORD_ERR_BUFFER_TOO_SMALL,
                      pool_record_pointer_encode(&p, buf, 15u, &len));
    TEST_ASSERT_EQUAL(RECORD_ERR_INVALID_ARGUMENT,
                      pool_record_pointer_encode(NULL, buf, sizeof(buf), &len));

    TEST_ASSERT_EQUAL(RECORD_OK, pool_record_pointer_encode(&p, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(RECORD_ERR_TRUNCATED, pool_record_pointer_decode(buf, 15u, &q));
    TEST_ASSERT_EQUAL(RECORD_ERR_TRAILING_BYTES, pool_record_pointer_decode(buf, 17u, &q));
    TEST_ASSERT_EQUAL(RECORD_ERR_INVALID_ARGUMENT, pool_record_pointer_decode(NULL, 16u, &q));
}

TEST_CASE("pointer: every covered bit corruption is detected", "[pool_record]")
{
    PoolRecordPointer p, q;
    uint8_t buf[POOL_RECORD_POINTER_LEN];
    size_t len = 0;
    int byte, bit;

    p.slot = POOL_RECORD_SLOT_B;
    p.generation = 0xA5A5A5A5u;
    TEST_ASSERT_EQUAL(RECORD_OK, pool_record_pointer_encode(&p, buf, sizeof(buf), &len));
    for (byte = 0; byte < 12; byte++) {
        for (bit = 0; bit < 8; bit++) {
            buf[byte] ^= (uint8_t)(1u << bit);
            TEST_ASSERT_TRUE(pool_record_pointer_decode(buf, len, &q) != RECORD_OK);
            buf[byte] ^= (uint8_t)(1u << bit);
        }
    }
    /* untouched decodes fine again */
    TEST_ASSERT_EQUAL(RECORD_OK, pool_record_pointer_decode(buf, len, &q));
}

/* ================================================================= */
/* K. Latest-trusted-epoch floor tests                                */
/* ================================================================= */

TEST_CASE("epoch-floor: first, equal and forward proposals accepted", "[pool_record]")
{
    make_active(&g_rec);
    g_rec.latest_trusted_valid = false;
    g_rec.latest_trusted_epoch_s = 0u;
    TEST_ASSERT_EQUAL(RECORD_OK,
                      pool_session_record_propose_trusted_epoch(&g_rec, EPOCH_A_S + 10u));
    TEST_ASSERT_TRUE(g_rec.latest_trusted_valid);
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 10u, g_rec.latest_trusted_epoch_s);
    /* equal accepted */
    TEST_ASSERT_EQUAL(RECORD_OK,
                      pool_session_record_propose_trusted_epoch(&g_rec, EPOCH_A_S + 10u));
    /* forward accepted */
    TEST_ASSERT_EQUAL(RECORD_OK,
                      pool_session_record_propose_trusted_epoch(&g_rec, EPOCH_A_S + 500u));
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 500u, g_rec.latest_trusted_epoch_s);
    /* survives the round trip */
    roundtrip_equal();
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 500u, g_rec2.latest_trusted_epoch_s);
}

TEST_CASE("epoch-floor: regression and invalid proposals rejected", "[pool_record]")
{
    make_active(&g_rec); /* latest = EPOCH_A + 100, start = EPOCH_A */
    TEST_ASSERT_EQUAL(RECORD_ERR_EPOCH_REGRESSION,
                      pool_session_record_propose_trusted_epoch(&g_rec, EPOCH_A_S + 99u));
    TEST_ASSERT_EQUAL_UINT64(EPOCH_A_S + 100u, g_rec.latest_trusted_epoch_s); /* unchanged */
    /* below verified start (fresh floor) */
    make_active(&g_rec);
    g_rec.latest_trusted_valid = false;
    g_rec.latest_trusted_epoch_s = 0u;
    TEST_ASSERT_EQUAL(RECORD_ERR_EPOCH_REGRESSION,
                      pool_session_record_propose_trusted_epoch(&g_rec, EPOCH_A_S - 1u));
    /* outside the sanity band */
    TEST_ASSERT_EQUAL(RECORD_ERR_EPOCH_INVALID,
                      pool_session_record_propose_trusted_epoch(&g_rec,
                                                                POOL_RECORD_EPOCH_MIN_S - 1u));
    TEST_ASSERT_EQUAL(RECORD_ERR_EPOCH_INVALID,
                      pool_session_record_propose_trusted_epoch(&g_rec,
                                                                POOL_RECORD_EPOCH_MAX_S + 1u));
    TEST_ASSERT_EQUAL(RECORD_ERR_INVALID_ARGUMENT,
                      pool_session_record_propose_trusted_epoch(NULL, EPOCH_A_S));
    TEST_ASSERT_FALSE(g_rec.latest_trusted_valid); /* failed proposals set nothing */
}

/* ================================================================= */
/* L. Counter tests                                                   */
/* ================================================================= */

TEST_CASE("counters: increments saturate and never wrap", "[pool_record]")
{
    uint8_t v = 0;
    int i;
    for (i = 0; i < 300; i++) {
        v = pool_record_counter_increment(v, POOL_RECORD_RECOVERY_ATTEMPT_MAX);
    }
    TEST_ASSERT_EQUAL_UINT8(POOL_RECORD_RECOVERY_ATTEMPT_MAX, v);
    v = pool_record_counter_increment(255u, 255u);
    TEST_ASSERT_EQUAL_UINT8(255u, v); /* even a degenerate max cannot wrap */
}

TEST_CASE("counters: exhaustion detection and round trip", "[pool_record]")
{
    make_baseline(&g_rec);
    TEST_ASSERT_FALSE(pool_record_counters_exhausted(&g_rec));
    g_rec.recovery_attempt_count = POOL_RECORD_RECOVERY_ATTEMPT_MAX;
    TEST_ASSERT_TRUE(pool_record_counters_exhausted(&g_rec));
    /* boundary values persist */
    g_rec.reboot_count = POOL_RECORD_REBOOT_COUNT_MAX;
    g_rec.consecutive_recovery_failures = POOL_RECORD_CONSECUTIVE_RECOVERY_FAIL_MAX;
    roundtrip_equal();
    TEST_ASSERT_EQUAL_UINT8(POOL_RECORD_REBOOT_COUNT_MAX, g_rec2.reboot_count);
    TEST_ASSERT_TRUE(pool_record_counters_exhausted(&g_rec2));
    /* fail safe on NULL */
    TEST_ASSERT_TRUE(pool_record_counters_exhausted(NULL));
}

/* ================================================================= */
/* Converter tests                                                    */
/* ================================================================= */

TEST_CASE("convert: live session round trips through the record", "[pool_record]")
{
    pool_session_init(&g_session);
    g_session.model_version = POOL_SESSION_MODEL_VERSION;
    g_session.session_id = 77u;
    g_session.state = POOL_STATE_TARGET_ACTIVE;
    fill_identity(&g_session.source, POOL_CHAIN_BITCOIN, "p1", "btc.example", 3333, "acct.w");
    fill_identity(&g_session.target, POOL_CHAIN_BITCOIN_CASH, "p2", "bch.example", 3334, "acct.w");
    /* plant stale bytes behind a DISABLED fallback: must be canonicalized away */
    strncpy(g_session.source.fallback.host, "stale-garbage.example",
            sizeof(g_session.source.fallback.host) - 1);
    g_session.duration_s = 3600u;
    g_session.password_policy = POOL_SESSION_PW_KEEP_CURRENT;
    g_session.restore_required = true;
    g_session.target_verify.connection_observed = true;
    g_session.target_verify.mining_observed = true;
    g_session.target_verify.identity_verified = true;
    g_session.retries.target_verify = 2u;
    g_session.last_error = ERR_TARGET_CONNECT_TIMEOUT;

    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_from_session(&g_session, &g_rec));
    TEST_ASSERT_EQUAL_STRING("", g_rec.source.fallback.host); /* canonicalized */
    TEST_ASSERT_TRUE(g_rec.restore_required);

    /* record -> encoded -> decoded -> session (the store normally assigns
     * the generation before encoding; model it here) */
    g_rec.generation = 4u;
    roundtrip_equal();
    memset(&g_session, 0, sizeof(g_session));
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_to_session(&g_rec2, &g_session));
    TEST_ASSERT_EQUAL_UINT32(77u, g_session.session_id);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_ACTIVE, g_session.state);
    TEST_ASSERT_TRUE(g_session.restore_required);
    TEST_ASSERT_EQUAL(ERR_TARGET_CONNECT_TIMEOUT, g_session.last_error);
    TEST_ASSERT_EQUAL_UINT8(2u, g_session.retries.target_verify);
    TEST_ASSERT_EQUAL_STRING("btc.example", g_session.source.primary.host);
    /* the B1 reserved generation placeholder carries the committed gen */
    TEST_ASSERT_EQUAL_UINT32(g_rec2.generation, g_session.generation);
}

TEST_CASE("convert: ephemeral sessions and tombstones rejected", "[pool_record]")
{
    pool_session_init(&g_session);
    g_session.state = POOL_STATE_VERIFYING_TARGET;
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_STATE,
                      pool_session_record_from_session(&g_session, &g_rec));
    g_session.state = POOL_STATE_IDLE;
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_STATE,
                      pool_session_record_from_session(&g_session, &g_rec));
    pool_session_record_init_tombstone(&g_rec);
    TEST_ASSERT_EQUAL(RECORD_ERR_BAD_KIND, pool_session_record_to_session(&g_rec, &g_session));
    TEST_ASSERT_EQUAL(RECORD_ERR_INVALID_ARGUMENT,
                      pool_session_record_from_session(NULL, &g_rec));
    TEST_ASSERT_EQUAL(RECORD_ERR_INVALID_ARGUMENT,
                      pool_session_record_to_session(NULL, &g_session));
}

/* ================================================================= */
/* N. Privacy tests (record layer)                                    */
/* ================================================================= */

TEST_CASE("privacy: codec tokens are clean machine tokens", "[pool_record]")
{
    int i;
    for (i = 0; i < (int)POOL_RECORD_ERR__COUNT; i++) {
        assert_clean_token(pool_record_codec_error_str((PoolRecordCodecError)i));
    }
    assert_clean_token(pool_record_codec_error_str((PoolRecordCodecError)9999));
    TEST_ASSERT_EQUAL_STRING("RECORD_ERR_UNKNOWN",
                             pool_record_codec_error_str((PoolRecordCodecError)12345));
}

TEST_CASE("privacy: tombstones and tokens carry no identity or account bytes", "[pool_record]")
{
    size_t slen, tlen = 0;
    int i;
    /* a session encoding legitimately contains its identity bytes */
    make_active(&g_rec);
    strncpy(g_rec.source.primary.user, "acct-marker.worker",
            sizeof(g_rec.source.primary.user) - 1);
    slen = encode_ok(&g_rec);
    TEST_ASSERT_TRUE(bytes_contain(g_buf, slen, "acct-marker"));
    /* the tombstone that clears it must contain NONE of it */
    pool_session_record_init_tombstone(&g_rec2);
    g_rec2.generation = 9u;
    TEST_ASSERT_EQUAL(RECORD_OK,
                      pool_session_record_encode(&g_rec2, g_buf2, sizeof(g_buf2), &tlen));
    TEST_ASSERT_EQUAL_UINT32(28u, (uint32_t)tlen);
    TEST_ASSERT_FALSE(bytes_contain(g_buf2, tlen, "acct-marker"));
    TEST_ASSERT_FALSE(bytes_contain(g_buf2, tlen, "example"));
    TEST_ASSERT_FALSE(bytes_contain(g_buf2, tlen, "btc"));
    /* no token ever carries a persisted value */
    for (i = 0; i < (int)POOL_RECORD_ERR__COUNT; i++) {
        TEST_ASSERT_NULL(strstr(pool_record_codec_error_str((PoolRecordCodecError)i), "MARKER"));
    }
}

/* ================================================================= */
/* O. Property-style tests (record layer)                             */
/* ================================================================= */

TEST_CASE("property: encode never mutates its input", "[pool_record]")
{
    make_active(&g_rec);
    g_rec3 = g_rec;
    (void)encode_ok(&g_rec);
    TEST_ASSERT_EQUAL(0, memcmp(&g_rec3, &g_rec, sizeof(g_rec)));
    (void)pool_session_record_validate(&g_rec);
    TEST_ASSERT_EQUAL(0, memcmp(&g_rec3, &g_rec, sizeof(g_rec)));
}

TEST_CASE("property: re-encoding a decoded record is byte-identical", "[pool_record]")
{
    size_t l1, l2;
    make_active(&g_rec);
    g_rec.source.fallback_enabled = true;
    fill_endpoint(&g_rec.source.fallback, "fb.example", 3335, "fbu");
    l1 = encode_ok(&g_rec);
    memcpy(g_buf2, g_buf, l1);
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_decode(g_buf2, l1, &g_rec2));
    TEST_ASSERT_EQUAL(RECORD_OK,
                      pool_session_record_encode(&g_rec2, g_buf, sizeof(g_buf), &l2));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)l1, (uint32_t)l2);
    TEST_ASSERT_EQUAL(0, memcmp(g_buf2, g_buf, l1));
}

TEST_CASE("property: every byte corruption of a session encoding is detected", "[pool_record]")
{
    size_t len, at;
    make_active(&g_rec);
    len = encode_ok(&g_rec);
    memcpy(g_buf2, g_buf, len);
    for (at = 0; at < len; at++) {
        assert_byte_corruption_detected(g_buf2, len, at);
    }
    /* buffer restored: still decodes */
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_decode(g_buf2, len, &g_rec2));
}
