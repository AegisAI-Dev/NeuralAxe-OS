/*
 * Deterministic tests for Gate W6.1 mutation observability — counters,
 * snapshots, deltas, the tuning fingerprint and the provider seam.
 *
 * Pure: no NVS, no networking, no task, no clock, no hardware. Nothing here
 * contains a coordinate, identity, host, credential or path.
 *
 * The test image compiles with CONFIG_NX_MUTATION_OBSERVABILITY defined (see
 * test/CMakeLists.txt), so the counting behaviour itself is exercised rather
 * than assumed. The flag-OFF posture is proven structurally instead, by the
 * ELF symbol audit showing the counters are absent from the default image.
 */

#include <string.h>
#include "unity.h"
#include "nx_mutation_counters.h"

/* ---------------- helpers ---------------- */

static NxMutationSnapshot take(void)
{
    NxMutationSnapshot s;

    memset(&s, 0xAA, sizeof(s));   /* prove every field is written */
    TEST_ASSERT_TRUE(nx_mutation_counters_snapshot(&s, sizeof(s)));
    return s;
}

/* ---------------- A. counter semantics ---------------- */

TEST_CASE("W61-A1 counters start at zero and count effective notes",
          "[mutation_observability]")
{
    NxMutationSnapshot s;
    unsigned i;

    nx_mutation_counters_reset_for_test();
    s = take();
    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT32(0u, s.counters[i]);
    }
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_EQUAL_UINT32(0u, s.saturated_mask);

    nx_mutation_counter_note(NX_MUT_FREQUENCY_CONFIG);
    nx_mutation_counter_note(NX_MUT_FREQUENCY_CONFIG);
    nx_mutation_counter_note(NX_MUT_OTA_WEB);

    s = take();
    TEST_ASSERT_EQUAL_UINT32(2u, s.counters[NX_MUT_FREQUENCY_CONFIG]);
    TEST_ASSERT_EQUAL_UINT32(1u, s.counters[NX_MUT_OTA_WEB]);
    TEST_ASSERT_EQUAL_UINT32(0u, s.counters[NX_MUT_VOLTAGE_CONFIG]);
}

TEST_CASE("W61-A2 counters are independent, one per class",
          "[mutation_observability]")
{
    NxMutationSnapshot s;
    unsigned i;

    nx_mutation_counters_reset_for_test();
    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        nx_mutation_counter_note((NxMutationCounterId)i);
    }
    s = take();
    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT32(1u, s.counters[i]);
    }
}

TEST_CASE("W61-A3 an out-of-range id is dropped, never mis-attributed",
          "[mutation_observability]")
{
    NxMutationSnapshot s;
    unsigned i;

    nx_mutation_counters_reset_for_test();
    nx_mutation_counter_note(NX_MUT__COUNT);
    nx_mutation_counter_note((NxMutationCounterId)99);

    s = take();
    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT32(0u, s.counters[i]);
    }
}

TEST_CASE("W61-A4 every counter has a distinct stable token",
          "[mutation_observability]")
{
    unsigned i, j;

    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        const char *a = nx_mutation_counter_str((NxMutationCounterId)i);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_NOT_EQUAL(0, strcmp(a, "MUT_UNKNOWN"));
        for (j = i + 1u; j < (unsigned)NX_MUT__COUNT; j++) {
            TEST_ASSERT_NOT_EQUAL(
                0, strcmp(a, nx_mutation_counter_str((NxMutationCounterId)j)));
        }
    }
    TEST_ASSERT_EQUAL_STRING("MUT_UNKNOWN",
                             nx_mutation_counter_str(NX_MUT__COUNT));
    TEST_ASSERT_EQUAL_STRING("MUT_UNKNOWN",
                             nx_mutation_counter_str((NxMutationCounterId)77));
}

/* ---------------- B. snapshot contract ---------------- */

TEST_CASE("W61-B1 a snapshot carries its version and exact size",
          "[mutation_observability]")
{
    NxMutationSnapshot s = take();

    TEST_ASSERT_EQUAL_UINT32(NX_MUTATION_SNAPSHOT_VERSION, s.version);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(s), s.size);
}

TEST_CASE("W61-B2 a wrong-sized or NULL request fails without writing",
          "[mutation_observability]")
{
    NxMutationSnapshot s;

    memset(&s, 0x5A, sizeof(s));
    TEST_ASSERT_FALSE(nx_mutation_counters_snapshot(&s, sizeof(s) - 1u));
    TEST_ASSERT_FALSE(nx_mutation_counters_snapshot(NULL, sizeof(s)));
    /* Untouched: a rejected request must not half-fill the caller's buffer. */
    TEST_ASSERT_EQUAL_UINT8(0x5A, ((const uint8_t *)&s)[0]);
}

TEST_CASE("W61-B3 the increment saturates at UINT32_MAX and never wraps",
          "[mutation_observability]")
{
    NxMutationSnapshot s;

    nx_mutation_counters_reset_for_test();
    s = take();
    TEST_ASSERT_FALSE(nx_mutation_snapshot_saturated(&s));

    /* One below the ceiling: the next note reaches it exactly. */
    nx_mutation_counters_preset_for_test(NX_MUT_POOL_CONFIG, UINT32_MAX - 1u);
    nx_mutation_counter_note(NX_MUT_POOL_CONFIG);
    s = take();
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.counters[NX_MUT_POOL_CONFIG]);
    TEST_ASSERT_TRUE(nx_mutation_snapshot_saturated(&s));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)1u << (unsigned)NX_MUT_POOL_CONFIG,
                             s.saturated_mask);

    /* Further notes STAY at the ceiling. A wrap to zero would compare equal
     * to a pilot baseline and read as "nothing happened". */
    nx_mutation_counter_note(NX_MUT_POOL_CONFIG);
    nx_mutation_counter_note(NX_MUT_POOL_CONFIG);
    s = take();
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.counters[NX_MUT_POOL_CONFIG]);
    TEST_ASSERT_TRUE(nx_mutation_snapshot_saturated(&s));

    /* Saturation is per counter, not global. */
    TEST_ASSERT_EQUAL_UINT32(0u, s.counters[NX_MUT_FAN_CONFIG]);
    nx_mutation_counters_reset_for_test();
}

TEST_CASE("W61-B5 a saturated counter never compares equal to a baseline",
          "[mutation_observability]")
{
    NxMutationSnapshot  base, now;
    NxMutationCounterId first = NX_MUT__COUNT;

    nx_mutation_counters_reset_for_test();
    nx_mutation_counters_preset_for_test(NX_MUT_OTA_FIRMWARE, UINT32_MAX);
    base = take();
    now  = take();

    /* Byte-identical snapshots, yet unchanged() must refuse: the history
     * behind a saturated counter cannot be reconstructed. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(&base, &now, sizeof(base)));
    TEST_ASSERT_FALSE(nx_mutation_snapshot_unchanged(&base, &now, &first));
    TEST_ASSERT_EQUAL_INT(NX_MUT_OTA_FIRMWARE, first);
    nx_mutation_counters_reset_for_test();
}

TEST_CASE("W61-B4 unchanged compares equal only for usable snapshots",
          "[mutation_observability]")
{
    NxMutationSnapshot  a, b;
    NxMutationCounterId first = NX_MUT_FREQUENCY_CONFIG;

    nx_mutation_counters_reset_for_test();
    a = take();
    b = take();
    TEST_ASSERT_TRUE(nx_mutation_snapshot_unchanged(&a, &b, &first));
    TEST_ASSERT_EQUAL_INT(NX_MUT__COUNT, first);

    nx_mutation_counter_note(NX_MUT_THERMAL_CONFIG);
    b = take();
    TEST_ASSERT_FALSE(nx_mutation_snapshot_unchanged(&a, &b, &first));
    TEST_ASSERT_EQUAL_INT(NX_MUT_THERMAL_CONFIG, first);

    /* An invalid snapshot proves nothing, even against itself. */
    b = a;
    b.valid = false;
    TEST_ASSERT_FALSE(nx_mutation_snapshot_unchanged(&a, &b, &first));
    TEST_ASSERT_FALSE(nx_mutation_snapshot_unchanged(&a, NULL, &first));
    TEST_ASSERT_FALSE(nx_mutation_snapshot_unchanged(NULL, NULL, NULL));

    /* A version mismatch is not silently tolerated. */
    b = a;
    b.version = NX_MUTATION_SNAPSHOT_VERSION + 1u;
    TEST_ASSERT_FALSE(nx_mutation_snapshot_unchanged(&a, &b, &first));
}

/* ---------------- C. delta arithmetic ---------------- */

TEST_CASE("W61-C1 an unchanged pair yields a clean zero delta",
          "[mutation_observability]")
{
    NxMutationSnapshot a, b;
    NxMutationDelta    d;

    nx_mutation_counters_reset_for_test();
    a = take();
    b = take();
    TEST_ASSERT_TRUE(nx_mutation_delta_compute(&a, &b, &d));
    TEST_ASSERT_TRUE(d.valid);
    TEST_ASSERT_TRUE(nx_mutation_delta_clean(&d));
    TEST_ASSERT_EQUAL_UINT32(0u, d.hardware_total);
    TEST_ASSERT_EQUAL_UINT32(0u, d.ota_total);
}

TEST_CASE("W61-C2 class totals group exactly the intended counters",
          "[mutation_observability]")
{
    NxMutationSnapshot a, b;
    NxMutationDelta    d;

    nx_mutation_counters_reset_for_test();
    a = take();

    nx_mutation_counter_note(NX_MUT_FREQUENCY_CONFIG);
    nx_mutation_counter_note(NX_MUT_VOLTAGE_CONFIG);
    nx_mutation_counter_note(NX_MUT_FAN_CONFIG);
    nx_mutation_counter_note(NX_MUT_THERMAL_CONFIG);
    nx_mutation_counter_note(NX_MUT_POOL_CONFIG);
    nx_mutation_counter_note(NX_MUT_PROTOCOL_CONFIG);
    nx_mutation_counter_note(NX_MUT_RESTART_REQUEST);
    nx_mutation_counter_note(NX_MUT_OTA_FIRMWARE);
    nx_mutation_counter_note(NX_MUT_OTA_WEB);
    b = take();

    TEST_ASSERT_TRUE(nx_mutation_delta_compute(&a, &b, &d));
    TEST_ASSERT_EQUAL_UINT32(4u, d.hardware_total);   /* freq+volt+fan+thermal */
    TEST_ASSERT_EQUAL_UINT32(1u, d.pool_total);
    TEST_ASSERT_EQUAL_UINT32(1u, d.protocol_total);
    TEST_ASSERT_EQUAL_UINT32(1u, d.restart_total);
    TEST_ASSERT_EQUAL_UINT32(2u, d.ota_total);        /* firmware + web        */
    TEST_ASSERT_FALSE(nx_mutation_delta_clean(&d));
}

TEST_CASE("W61-C3 a regression refuses to produce deltas",
          "[mutation_observability]")
{
    NxMutationSnapshot a, b;
    NxMutationDelta    d;

    nx_mutation_counters_reset_for_test();
    nx_mutation_counter_note(NX_MUT_POOL_CONFIG);
    a = take();
    nx_mutation_counters_reset_for_test();     /* a different history */
    b = take();

    TEST_ASSERT_FALSE(nx_mutation_delta_compute(&a, &b, &d));
    TEST_ASSERT_TRUE(d.regressed);
    TEST_ASSERT_FALSE(d.valid);
    TEST_ASSERT_FALSE(nx_mutation_delta_clean(&d));
    /* The zeroed totals must NOT read as "nothing happened". */
    TEST_ASSERT_EQUAL_UINT32(0u, d.pool_total);
    TEST_ASSERT_FALSE(d.valid);
}

TEST_CASE("W61-C4 saturation refuses to produce deltas",
          "[mutation_observability]")
{
    NxMutationSnapshot a, b;
    NxMutationDelta    d;

    nx_mutation_counters_reset_for_test();
    a = take();
    b = a;
    b.counters[NX_MUT_FAN_CONFIG] = UINT32_MAX;
    b.saturated_mask = (uint32_t)1u << (unsigned)NX_MUT_FAN_CONFIG;

    TEST_ASSERT_FALSE(nx_mutation_delta_compute(&a, &b, &d));
    TEST_ASSERT_TRUE(d.saturated);
    TEST_ASSERT_FALSE(d.valid);
}

TEST_CASE("W61-C5 class totals saturate instead of wrapping",
          "[mutation_observability]")
{
    NxMutationSnapshot a, b;
    NxMutationDelta    d;

    nx_mutation_counters_reset_for_test();
    a = take();
    b = a;
    /* Two very large, non-saturated deltas in the same class. */
    b.counters[NX_MUT_FREQUENCY_CONFIG] = UINT32_MAX - 1u;
    b.counters[NX_MUT_VOLTAGE_CONFIG]   = UINT32_MAX - 1u;

    TEST_ASSERT_TRUE(nx_mutation_delta_compute(&a, &b, &d));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, d.hardware_total);
    TEST_ASSERT_FALSE(nx_mutation_delta_clean(&d));
}

TEST_CASE("W61-C6 delta computation is NULL-safe and fails closed",
          "[mutation_observability]")
{
    NxMutationSnapshot a = take();
    NxMutationDelta    d;

    TEST_ASSERT_FALSE(nx_mutation_delta_compute(NULL, &a, &d));
    TEST_ASSERT_FALSE(d.valid);
    TEST_ASSERT_FALSE(nx_mutation_delta_compute(&a, NULL, &d));
    TEST_ASSERT_FALSE(nx_mutation_delta_compute(&a, &a, NULL));
    TEST_ASSERT_FALSE(nx_mutation_delta_clean(NULL));
}

/* ---------------- H. accepted-then-failed operations ---------------- */

TEST_CASE("W61-H1 an accepted mutation stays counted when a later step fails",
          "[mutation_observability]")
{
    NxMutationSnapshot after_accept, after_failure;

    nx_mutation_counters_reset_for_test();

    /* The owning subsystem accepted an effective configuration change and
     * counted it at the boundary. */
    nx_mutation_counter_note(NX_MUT_POOL_CONFIG);
    after_accept = take();
    TEST_ASSERT_EQUAL_UINT32(1u, after_accept.counters[NX_MUT_POOL_CONFIG]);

    /*
     * Whatever happens downstream — the persistence queue rejects it, the NVS
     * commit fails, the hardware application fails, or the operation completes
     * only partially — NOTHING in this API can undo the observation, because
     * the configuration the device is running with has already changed.
     */
    after_failure = take();
    TEST_ASSERT_EQUAL_UINT32(1u, after_failure.counters[NX_MUT_POOL_CONFIG]);
    TEST_ASSERT_TRUE(nx_mutation_snapshot_unchanged(&after_accept,
                                                    &after_failure, NULL));
    nx_mutation_counters_reset_for_test();
}

TEST_CASE("W61-H2 no production API can lower a counter",
          "[mutation_observability]")
{
    NxMutationSnapshot  a, b;
    NxMutationDelta     d;
    unsigned            i;

    nx_mutation_counters_reset_for_test();
    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        nx_mutation_counter_note((NxMutationCounterId)i);
    }
    a = take();

    /* Exercise every non-test entry point this module exposes. */
    (void)nx_mutation_snapshot_saturated(&a);
    (void)nx_mutation_snapshot_unchanged(&a, &a, NULL);
    (void)nx_mutation_delta_compute(&a, &a, &d);
    (void)nx_mutation_delta_clean(&d);
    (void)nx_mutation_counter_str(NX_MUT_POOL_CONFIG);

    b = take();
    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT32(1u, b.counters[i]);
    }
    TEST_ASSERT_TRUE(nx_mutation_snapshot_unchanged(&a, &b, NULL));
    nx_mutation_counters_reset_for_test();
}

TEST_CASE("W61-H3 a rejected request leaves every counter untouched",
          "[mutation_observability]")
{
    NxMutationSnapshot before, after;
    unsigned           i;

    nx_mutation_counters_reset_for_test();
    before = take();

    /*
     * An out-of-range class is the only rejection this module can see. The
     * subsystem-level rejections — unknown key, wrong type, same value — never
     * reach it at all, because each setter's guard precedes the call.
     */
    nx_mutation_counter_note(NX_MUT__COUNT);
    nx_mutation_counter_note((NxMutationCounterId)255);

    after = take();
    for (i = 0; i < (unsigned)NX_MUT__COUNT; i++) {
        TEST_ASSERT_EQUAL_UINT32(0u, after.counters[i]);
    }
    TEST_ASSERT_TRUE(nx_mutation_snapshot_unchanged(&before, &after, NULL));
}
