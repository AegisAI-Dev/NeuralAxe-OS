/*
 * Deterministic tests for the Gate W6.1 CANONICAL TUNING SNAPSHOT — the exact
 * equality decision that replaced the 32-bit digest.
 *
 * Pure: no NVS, no networking, no task, no clock, no hardware. Nothing here
 * contains a coordinate, identity, host, credential or path.
 *
 * The fan-curve values are the committed board-601 shape; the parse from text
 * belongs to the main-side adapter (it needs nvs_config and thermal_control),
 * so these tests exercise the canonical MODEL and its comparison directly.
 */

#include <string.h>
#include "unity.h"
#include "nx_tuning_snapshot.h"

/* The committed board-601 curve, already in canonical form. */
static const uint8_t CURVE_T[NX_TUNING_CURVE_POINTS] = { 45u, 52u, 58u, 64u };
static const uint8_t CURVE_P[NX_TUNING_CURVE_POINTS] = { 25u, 45u, 70u, 100u };

static void fill(NxTuningSnapshot *s)
{
    unsigned i;

    nx_tuning_snapshot_init(s);
    s->frequency_mhz_x10 = 6250u;      /* 625.0 MHz */
    s->voltage_mv        = 1150u;
    s->fan_mode          = 1u;
    s->fan_percent       = 70u;
    s->fan_min_percent   = 25u;
    s->fan_hysteresis_c  = 2u;
    s->temp_target_c     = 60u;
    s->overheat_mode     = 0u;
    s->thermal_mode      = (uint16_t)NX_TUNING_MODE_CURVE;
    s->curve_point_count = (uint16_t)NX_TUNING_CURVE_POINTS;
    for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
        s->curve[i].temp_c  = CURVE_T[i];
        s->curve[i].fan_pct = CURVE_P[i];
    }
    TEST_ASSERT_TRUE(nx_tuning_snapshot_finalize(s));
}

/* ---------------- 1. every canonical field is represented ------------- */

TEST_CASE("W61-T1 the canonical serialization covers every field exactly once",
          "[mutation_observability]")
{
    NxTuningSnapshot s;
    uint8_t          buf[NX_TUNING_SNAPSHOT_BYTES];
    uint8_t          small[NX_TUNING_SNAPSHOT_BYTES - 1u];

    fill(&s);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)NX_TUNING_SNAPSHOT_BYTES,
                             (uint32_t)nx_tuning_snapshot_serialize(&s, buf,
                                                                    sizeof(buf)));
    /* 4 version + 1 valid + 10 u16 fields + 4 point pairs. */
    TEST_ASSERT_EQUAL_UINT32(4u + 1u + 20u + 8u,
                             (uint32_t)NX_TUNING_SNAPSHOT_BYTES);

    /* A buffer one byte short is refused rather than truncated. */
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)nx_tuning_snapshot_serialize(
                                     &s, small, sizeof(small)));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)nx_tuning_snapshot_serialize(
                                     NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)nx_tuning_snapshot_serialize(
                                     &s, NULL, sizeof(buf)));
}

/* ---------------- 2. identical semantics compare equal ---------------- */

TEST_CASE("W61-T2 identical configurations compare equal, field and byte",
          "[mutation_observability]")
{
    NxTuningSnapshot a, b;
    uint8_t          ba[NX_TUNING_SNAPSHOT_BYTES], bb[NX_TUNING_SNAPSHOT_BYTES];
    size_t           na, nb;

    fill(&a);
    fill(&b);
    TEST_ASSERT_TRUE(nx_tuning_snapshot_equal(&a, &b));

    /* The serialized form agrees with the field comparison. */
    na = nx_tuning_snapshot_serialize(&a, ba, sizeof(ba));
    nb = nx_tuning_snapshot_serialize(&b, bb, sizeof(bb));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)na, (uint32_t)nb);
    TEST_ASSERT_EQUAL_INT(0, memcmp(ba, bb, na));
}

/* ---------------- 3. canonical form absorbs representation ------------ */

TEST_CASE("W61-T3 unset curve has exactly one canonical representation",
          "[mutation_observability]")
{
    NxTuningSnapshot a, b;
    unsigned         i;

    /*
     * The committed serialized grammar is "v1;T:P;T:P;T:P;T:P" with plain
     * integers and no optional whitespace, so the TEXT admits no equivalent
     * formatting to absorb — the canonicalisation that matters is that the
     * parsed points, not the text, are stored. What CAN differ is how "no
     * curve" reaches the snapshot, so that is pinned here: an unset curve is
     * count 0 with every slot zeroed, and a leftover point makes it invalid.
     */
    fill(&a);
    a.curve_point_count = 0u;
    for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
        a.curve[i].temp_c  = 0u;
        a.curve[i].fan_pct = 0u;
    }
    TEST_ASSERT_TRUE(nx_tuning_snapshot_finalize(&a));

    fill(&b);
    b.curve_point_count = 0u;
    for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
        b.curve[i].temp_c  = 0u;
        b.curve[i].fan_pct = 0u;
    }
    TEST_ASSERT_TRUE(nx_tuning_snapshot_finalize(&b));
    TEST_ASSERT_TRUE(nx_tuning_snapshot_equal(&a, &b));

    /* count 0 with a stale point left behind is NOT a configuration. */
    b.curve[2].temp_c = 58u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&b));
    TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(&a, &b));

    /* A partial count is refused outright. */
    fill(&b);
    b.curve_point_count = 2u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&b));
}

/* ---------------- 4. every curve point matters ------------------------ */

TEST_CASE("W61-T4 every fan-curve point independently changes equality",
          "[mutation_observability]")
{
    NxTuningSnapshot base, v;
    unsigned         i;

    fill(&base);
    for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
        fill(&v);
        v.curve[i].temp_c = (uint8_t)(v.curve[i].temp_c + 1u);
        TEST_ASSERT_TRUE(nx_tuning_snapshot_finalize(&v));
        TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(&base, &v));

        fill(&v);
        v.curve[i].fan_pct = (uint8_t)(v.curve[i].fan_pct - 1u);
        TEST_ASSERT_TRUE(nx_tuning_snapshot_finalize(&v));
        TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(&base, &v));
    }
    /* And the count itself. */
    fill(&v);
    v.curve_point_count = 0u;
    for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
        v.curve[i].temp_c = 0u;
        v.curve[i].fan_pct = 0u;
    }
    TEST_ASSERT_TRUE(nx_tuning_snapshot_finalize(&v));
    TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(&base, &v));
}

/* ---------------- 5-9. every scalar field matters --------------------- */

TEST_CASE("W61-T5 every scalar tuning field independently changes equality",
          "[mutation_observability]")
{
    NxTuningSnapshot base, v;
    unsigned         i;

    fill(&base);
    for (i = 0; i < 9u; i++) {
        fill(&v);
        switch (i) {
        case 0: v.frequency_mhz_x10 += 1u; break;   /* frequency  (req 5) */
        case 1: v.voltage_mv        += 1u; break;   /* voltage    (req 6) */
        case 2: v.fan_mode           = 0u; break;   /* fan mode   (req 7) */
        case 3: v.fan_percent       -= 1u; break;   /* configured (req 7) */
        case 4: v.fan_min_percent   += 1u; break;   /* configured (req 7) */
        case 5: v.fan_hysteresis_c  += 1u; break;
        case 6: v.temp_target_c     += 1u; break;   /* target     (req 8) */
        case 7: v.overheat_mode      = 1u; break;   /* overheat   (req 8) */
        default: v.thermal_mode = (uint16_t)NX_TUNING_MODE_MANUAL; break; /* 9 */
        }
        TEST_ASSERT_TRUE(nx_tuning_snapshot_finalize(&v));
        TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(&base, &v));
    }
}

TEST_CASE("W61-T6 every thermal mode is distinct and has a stable token",
          "[mutation_observability]")
{
    NxTuningSnapshot a, b;
    unsigned         i, j;

    for (i = 0; i < (unsigned)NX_TUNING_MODE__COUNT; i++) {
        const char *t = nx_tuning_thermal_mode_str((NxTuningThermalMode)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_NOT_EQUAL(0, strcmp(t, "MODE_UNKNOWN"));
        for (j = i + 1u; j < (unsigned)NX_TUNING_MODE__COUNT; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(t,
                nx_tuning_thermal_mode_str((NxTuningThermalMode)j)));
        }
    }
    TEST_ASSERT_EQUAL_STRING("MODE_UNKNOWN",
        nx_tuning_thermal_mode_str((NxTuningThermalMode)77));

    /* Each mode compares unequal to every other. */
    for (i = 0; i < (unsigned)NX_TUNING_MODE__COUNT; i++) {
        for (j = 0; j < (unsigned)NX_TUNING_MODE__COUNT; j++) {
            fill(&a); a.thermal_mode = (uint16_t)i;
            fill(&b); b.thermal_mode = (uint16_t)j;
            TEST_ASSERT_TRUE(nx_tuning_snapshot_finalize(&a));
            TEST_ASSERT_TRUE(nx_tuning_snapshot_finalize(&b));
            if (i == j) {
                TEST_ASSERT_TRUE(nx_tuning_snapshot_equal(&a, &b));
            } else {
                TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(&a, &b));
            }
        }
    }
}

/* ---------------- 10. invalid never compares equal -------------------- */

TEST_CASE("W61-T7 invalid or unavailable input never compares equal",
          "[mutation_observability]")
{
    NxTuningSnapshot a, b;

    nx_tuning_snapshot_init(&a);
    nx_tuning_snapshot_init(&b);
    /* Byte-identical and both invalid: absence is not agreement. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(&a, &b, sizeof(a)));
    TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(&a, &b));

    fill(&a);
    fill(&b);
    b.valid = false;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(&a, &b));
    TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(&a, NULL));
    TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(NULL, &b));
    TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(NULL, NULL));

    /* A version or size mismatch is not silently tolerated. */
    fill(&b);
    b.version = NX_TUNING_SNAPSHOT_VERSION + 1u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(&a, &b));
}

TEST_CASE("W61-T8 an out-of-range field makes the snapshot invalid",
          "[mutation_observability]")
{
    NxTuningSnapshot v;

    fill(&v); v.frequency_mhz_x10 = 0u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&v));
    TEST_ASSERT_FALSE(v.valid);

    fill(&v); v.frequency_mhz_x10 = NX_TUNING_FREQ_X10_MAX + 1u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&v));

    fill(&v); v.voltage_mv = 0u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&v));

    fill(&v); v.voltage_mv = NX_TUNING_VOLTAGE_MAX + 1u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&v));

    fill(&v); v.fan_percent = 101u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&v));

    fill(&v); v.fan_min_percent = 101u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&v));

    fill(&v); v.temp_target_c = 0u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&v));

    fill(&v); v.overheat_mode = 2u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&v));

    fill(&v); v.thermal_mode = (uint16_t)NX_TUNING_MODE__COUNT;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&v));

    fill(&v); v.curve[1].fan_pct = 101u;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(&v));

    TEST_ASSERT_FALSE(nx_tuning_snapshot_finalize(NULL));
}

/* ---------------- 11. padding never participates ---------------------- */

TEST_CASE("W61-T9 no padding byte participates in the comparison",
          "[mutation_observability]")
{
    NxTuningSnapshot a, b;
    uint8_t          ba[NX_TUNING_SNAPSHOT_BYTES], bb[NX_TUNING_SNAPSHOT_BYTES];

    /*
     * Poison the two structs with DIFFERENT byte patterns first, then set the
     * identical canonical values on top. Any padding the compiler inserted
     * still differs, so a memcmp-based decision would report these unequal.
     * The field-by-field authority must report them equal — and the canonical
     * serialization must be byte-identical.
     */
    memset(&a, 0xAA, sizeof(a));
    memset(&b, 0x55, sizeof(b));
    fill(&a);
    fill(&b);

    TEST_ASSERT_TRUE(nx_tuning_snapshot_equal(&a, &b));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)NX_TUNING_SNAPSHOT_BYTES,
        (uint32_t)nx_tuning_snapshot_serialize(&a, ba, sizeof(ba)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)NX_TUNING_SNAPSHOT_BYTES,
        (uint32_t)nx_tuning_snapshot_serialize(&b, bb, sizeof(bb)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(ba, bb, sizeof(ba)));
    /* The digest agrees too — but only because the VALUES agree. */
    TEST_ASSERT_EQUAL_UINT32(nx_tuning_snapshot_digest(&a),
                             nx_tuning_snapshot_digest(&b));
}

/* ---------------- 12. a digest collision cannot decide ---------------- */

TEST_CASE("W61-T10 a simulated digest collision cannot make two different "
          "configurations compare equal", "[mutation_observability]")
{
    NxTuningSnapshot a, b;
    uint8_t          ba[NX_TUNING_SNAPSHOT_BYTES], bb[NX_TUNING_SNAPSHOT_BYTES];

    fill(&a);
    fill(&b);
    b.voltage_mv = 1200u;                 /* a REAL tuning difference */
    TEST_ASSERT_TRUE(nx_tuning_snapshot_finalize(&b));

    /* Force both digests to the same value: the worst case a 32-bit
     * non-cryptographic hash can produce. */
    nx_tuning_snapshot_digest_override_for_test(true, 0xDEADBEEFu);
    TEST_ASSERT_EQUAL_UINT32(nx_tuning_snapshot_digest(&a),
                             nx_tuning_snapshot_digest(&b));

    /* The equality authority is unaffected: it never consults the digest. */
    TEST_ASSERT_FALSE(nx_tuning_snapshot_equal(&a, &b));

    /* And the canonical bytes still differ. */
    nx_tuning_snapshot_digest_override_for_test(false, 0u);
    (void)nx_tuning_snapshot_serialize(&a, ba, sizeof(ba));
    (void)nx_tuning_snapshot_serialize(&b, bb, sizeof(bb));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(ba, bb, sizeof(ba)));
}

/* ---------------- 13-14. provider seam -------------------------------- */

static bool s_provider_ok;
static bool s_provider_partial;

static bool seam_provider(NxTuningSnapshot *out)
{
    fill(out);
    if (s_provider_partial) {
        /* A parser that failed halfway: the provider must leave nothing
         * usable, and nx_tuning_snapshot_read() must wipe what it did write. */
        out->curve_point_count = 3u;      /* impossible count */
        (void)nx_tuning_snapshot_finalize(out);
        return true;                      /* claims success, is invalid */
    }
    return s_provider_ok;
}

TEST_CASE("W61-T11 a failed or partial provider stores nothing usable",
          "[mutation_observability]")
{
    NxTuningSnapshot got;
    unsigned         i;

    nx_tuning_snapshot_provider_register(NULL);
    TEST_ASSERT_FALSE(nx_tuning_snapshot_provider_present());
    TEST_ASSERT_FALSE(nx_tuning_snapshot_read(&got));
    TEST_ASSERT_FALSE(got.valid);

    s_provider_ok = false;
    s_provider_partial = false;
    nx_tuning_snapshot_provider_register(seam_provider);
    TEST_ASSERT_TRUE(nx_tuning_snapshot_provider_present());
    TEST_ASSERT_FALSE(nx_tuning_snapshot_read(&got));
    TEST_ASSERT_FALSE(got.valid);
    /* Nothing partial survived — not even a curve point. */
    for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
        TEST_ASSERT_EQUAL_UINT8(0u, got.curve[i].temp_c);
        TEST_ASSERT_EQUAL_UINT8(0u, got.curve[i].fan_pct);
    }
    TEST_ASSERT_EQUAL_UINT16(0u, got.curve_point_count);

    /* A provider that claims success while leaving an invalid snapshot. */
    s_provider_partial = true;
    TEST_ASSERT_FALSE(nx_tuning_snapshot_read(&got));
    TEST_ASSERT_FALSE(got.valid);
    TEST_ASSERT_EQUAL_UINT16(0u, got.curve_point_count);

    s_provider_partial = false;
    s_provider_ok = true;
    TEST_ASSERT_TRUE(nx_tuning_snapshot_read(&got));
    TEST_ASSERT_TRUE(got.valid);

    TEST_ASSERT_FALSE(nx_tuning_snapshot_read(NULL));
    nx_tuning_snapshot_provider_register(NULL);
}

TEST_CASE("W61-T12 the snapshot holds no pointer and no allocation",
          "[mutation_observability]")
{
    NxTuningSnapshot s;

    fill(&s);
    /*
     * Every member is a fixed-width scalar or an array of them, so the whole
     * value can be copied by assignment and outlives any temporary the
     * provider used. A pointer field would make that false, and would let a
     * freed parse buffer be reachable from the baseline.
     */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(s), s.size);
    {
        NxTuningSnapshot copy = s;      /* value semantics, no ownership */
        TEST_ASSERT_TRUE(nx_tuning_snapshot_equal(&s, &copy));
    }
}
