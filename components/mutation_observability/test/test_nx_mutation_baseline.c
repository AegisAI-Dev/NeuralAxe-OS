/*
 * Deterministic tests for the Gate W6.1 baseline READINESS PREDICATE, the
 * capture lifecycle and the baseline comparison.
 *
 * Pure: no NVS, no networking, no task, no wall clock — every time value is a
 * synthetic MONOTONIC microsecond count passed in by the test. Nothing here
 * contains a coordinate, identity, host, credential or path.
 */

#include <string.h>
#include "unity.h"
#include "nx_mutation_baseline.h"

#define AFTER_SETTLE (NX_MUTATION_BASELINE_SETTLE_US + 1000ull)

/* ---------------- controllable fingerprint provider ---------------- */

static uint16_t s_voltage;
static bool     s_provider_available;

static bool test_provider(NxTuningSnapshot *out)
{
    unsigned i;
    static const uint8_t temps[] = { 45u, 52u, 58u, 64u };
    static const uint8_t pcts[]  = { 25u, 45u, 70u, 100u };

    if (!s_provider_available) {
        return false;
    }
    nx_tuning_snapshot_init(out);
    out->frequency_mhz_x10 = 6250u;
    out->voltage_mv        = s_voltage;
    out->fan_mode          = 1u;
    out->fan_percent       = 70u;
    out->fan_min_percent   = 25u;
    out->fan_hysteresis_c  = 2u;
    out->temp_target_c     = 60u;
    out->overheat_mode     = 0u;
    out->thermal_mode      = (uint16_t)NX_TUNING_MODE_CURVE;
    out->curve_point_count = (uint16_t)NX_TUNING_CURVE_POINTS;
    for (i = 0; i < (unsigned)NX_TUNING_CURVE_POINTS; i++) {
        out->curve[i].temp_c  = temps[i];
        out->curve[i].fan_pct = pcts[i];
    }
    return nx_tuning_snapshot_finalize(out);
}

/* Every prerequisite satisfied. Each test then breaks exactly one. */
static void all_prereq(NxBaselinePrereq *p)
{
    p->config_loaded          = true;
    p->runtime_snapshot_valid = true;
    p->owner_none             = true;
    p->protocol_normal_source = true;
    p->session_posture_clean  = true;
    p->no_terminal_pending    = true;
    p->host_task_valid        = true;
}

static NxMutationSnapshot take_snapshot(void)
{
    NxMutationSnapshot s;

    memset(&s, 0xAA, sizeof(s));
    TEST_ASSERT_TRUE(nx_mutation_counters_snapshot(&s, sizeof(s)));
    return s;
}

static void fresh(void)
{
    nx_mutation_counters_reset_for_test();
    nx_mutation_baseline_reset_for_test();
    s_voltage            = 1150u;
    s_provider_available = true;
    nx_tuning_snapshot_provider_register(test_provider);
}

static void teardown(void)
{
    nx_tuning_snapshot_provider_register(NULL);
    nx_mutation_baseline_reset_for_test();
    nx_mutation_counters_reset_for_test();
}

static NxMutationBaselineState capture_ok(uint64_t now)
{
    NxBaselinePrereq p;

    all_prereq(&p);
    return nx_mutation_baseline_capture(now, &p);
}

/* ---------------- F. readiness predicate ---------------- */

TEST_CASE("W61-F1 no baseline exists until one is captured",
          "[mutation_observability]")
{
    NxMutationComparison cmp;

    fresh();
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_NONE, nx_mutation_baseline_state());
    TEST_ASSERT_EQUAL_UINT64(0u, nx_mutation_baseline_get()->captured_us);

    /* Without a reference point the comparison proves nothing. */
    TEST_ASSERT_FALSE(nx_mutation_baseline_compare(&cmp));
    TEST_ASSERT_FALSE(cmp.baseline_ready);
    TEST_ASSERT_FALSE(cmp.counters_unchanged);
    TEST_ASSERT_FALSE(cmp.tuning_unchanged);
    teardown();
}

TEST_CASE("W61-F2 the settle delay is necessary but NOT sufficient",
          "[mutation_observability]")
{
    NxBaselinePrereq p;

    fresh();
    all_prereq(&p);

    /* Before the floor: SETTLING, whatever else is true. */
    TEST_ASSERT_EQUAL_INT(NX_BASELINE_BLOCK_SETTLING,
                          nx_mutation_baseline_evaluate(0u, &p));
    TEST_ASSERT_EQUAL_INT(NX_BASELINE_BLOCK_SETTLING,
                          nx_mutation_baseline_evaluate(
                              NX_MUTATION_BASELINE_SETTLE_US - 1ull, &p));
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_PENDING,
                          nx_mutation_baseline_capture(0u, &p));
    TEST_ASSERT_EQUAL_UINT64(0u, nx_mutation_baseline_get()->captured_us);
    /* PENDING is not a refusal and must not be counted as one. */
    TEST_ASSERT_EQUAL_UINT32(0u, nx_mutation_baseline_get()->refusals);

    /* After the floor, with everything else satisfied, it captures. */
    TEST_ASSERT_EQUAL_INT(NX_BASELINE_READY_OK,
                          nx_mutation_baseline_evaluate(AFTER_SETTLE, &p));
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));
    teardown();
}

TEST_CASE("W61-F3 EVERY prerequisite independently blocks capture",
          "[mutation_observability]")
{
    struct { const char *name; size_t off; NxBaselineBlock block; } cases[] = {
        { "config_loaded",          offsetof(NxBaselinePrereq, config_loaded),
          NX_BASELINE_BLOCK_CONFIG_NOT_LOADED },
        { "host_task_valid",        offsetof(NxBaselinePrereq, host_task_valid),
          NX_BASELINE_BLOCK_HOST_TASK_INVALID },
        { "runtime_snapshot_valid", offsetof(NxBaselinePrereq, runtime_snapshot_valid),
          NX_BASELINE_BLOCK_RUNTIME_UNAVAILABLE },
        { "owner_none",             offsetof(NxBaselinePrereq, owner_none),
          NX_BASELINE_BLOCK_OWNER_PRESENT },
        { "protocol_normal_source", offsetof(NxBaselinePrereq, protocol_normal_source),
          NX_BASELINE_BLOCK_PROTOCOL_POSTURE },
        { "session_posture_clean",  offsetof(NxBaselinePrereq, session_posture_clean),
          NX_BASELINE_BLOCK_SESSION_POSTURE },
        { "no_terminal_pending",    offsetof(NxBaselinePrereq, no_terminal_pending),
          NX_BASELINE_BLOCK_TERMINAL_PENDING },
    };
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        NxBaselinePrereq p;

        fresh();
        all_prereq(&p);
        *((bool *)((char *)&p + cases[i].off)) = false;

        TEST_ASSERT_EQUAL_INT(cases[i].block,
                              nx_mutation_baseline_evaluate(AFTER_SETTLE, &p));
        TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_PENDING,
                              nx_mutation_baseline_capture(AFTER_SETTLE, &p));
        /* Nothing partial was stored. */
        TEST_ASSERT_EQUAL_UINT64(0u, nx_mutation_baseline_get()->captured_us);
        TEST_ASSERT_FALSE(nx_mutation_baseline_get()->zero_at_capture);
        teardown();
    }
}

TEST_CASE("W61-F4 a NULL prerequisite block is no evidence, not no objection",
          "[mutation_observability]")
{
    fresh();
    TEST_ASSERT_EQUAL_INT(NX_BASELINE_BLOCK_CONFIG_NOT_LOADED,
                          nx_mutation_baseline_evaluate(AFTER_SETTLE, NULL));
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_PENDING,
                          nx_mutation_baseline_capture(AFTER_SETTLE, NULL));
    TEST_ASSERT_EQUAL_UINT64(0u, nx_mutation_baseline_get()->captured_us);
    teardown();
}

TEST_CASE("W61-F5 a zeroed prerequisite struct is entirely UNREADY",
          "[mutation_observability]")
{
    NxBaselinePrereq p;

    fresh();
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_EQUAL_INT(NX_BASELINE_BLOCK_CONFIG_NOT_LOADED,
                          nx_mutation_baseline_evaluate(AFTER_SETTLE, &p));
    teardown();
}

TEST_CASE("W61-F6 an unreadable configuration blocks capture",
          "[mutation_observability]")
{
    NxBaselinePrereq p;

    fresh();
    all_prereq(&p);
    s_provider_available = false;

    TEST_ASSERT_EQUAL_INT(NX_BASELINE_BLOCK_TUNING_UNREADABLE,
                          nx_mutation_baseline_evaluate(AFTER_SETTLE, &p));
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_PENDING,
                          nx_mutation_baseline_capture(AFTER_SETTLE, &p));
    TEST_ASSERT_EQUAL_UINT64(0u, nx_mutation_baseline_get()->captured_us);

    /* Readable again: this is a FIRST capture, not a replacement. */
    s_provider_available = true;
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE * 2ull));
    TEST_ASSERT_EQUAL_UINT64(AFTER_SETTLE * 2ull,
                             nx_mutation_baseline_get()->captured_us);
    teardown();
}

TEST_CASE("W61-F7 a NON-CLEAN boot REFUSES the baseline, permanently",
          "[mutation_observability]")
{
    NxBaselinePrereq p;

    fresh();
    all_prereq(&p);
    /* Something changed configuration before the window could open. */
    nx_mutation_counter_note(NX_MUT_THERMAL_CONFIG);

    TEST_ASSERT_EQUAL_INT(NX_BASELINE_BLOCK_COUNTER_NONZERO,
                          nx_mutation_baseline_evaluate(AFTER_SETTLE, &p));
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_REFUSED,
                          nx_mutation_baseline_capture(AFTER_SETTLE, &p));
    TEST_ASSERT_EQUAL_UINT32(1u, nx_mutation_baseline_get()->refusals);
    TEST_ASSERT_EQUAL_UINT64(0u, nx_mutation_baseline_get()->captured_us);
    TEST_ASSERT_FALSE(nx_mutation_baseline_get()->zero_at_capture);

    /*
     * NOT RETRYABLE INTO SUCCESS. Counters only rise, so no amount of waiting
     * makes this device baseline-able. It never gets to claim it proved
     * anything about the interval it did not observe.
     */
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_REFUSED,
                          capture_ok(AFTER_SETTLE * 100ull));
    TEST_ASSERT_EQUAL_UINT64(0u, nx_mutation_baseline_get()->captured_us);
    teardown();
}

TEST_CASE("W61-F8 a saturated counter refuses the baseline",
          "[mutation_observability]")
{
    NxBaselinePrereq p;

    fresh();
    all_prereq(&p);
    nx_mutation_counters_preset_for_test(NX_MUT_OTA_WEB, UINT32_MAX);

    TEST_ASSERT_EQUAL_INT(NX_BASELINE_BLOCK_COUNTER_SATURATED,
                          nx_mutation_baseline_evaluate(AFTER_SETTLE, &p));
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_REFUSED,
                          nx_mutation_baseline_capture(AFTER_SETTLE, &p));
    TEST_ASSERT_EQUAL_UINT32(1u, nx_mutation_baseline_get()->refusals);
    teardown();
}

TEST_CASE("W61-F9 a READY baseline is never silently replaced",
          "[mutation_observability]")
{
    NxMutationComparison cmp;
    uint64_t             first_us;

    fresh();
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));
    first_us = nx_mutation_baseline_get()->captured_us;
    TEST_ASSERT_EQUAL_UINT64(AFTER_SETTLE, first_us);
    /* A READY baseline ALWAYS captured a clean boot: an invariant, not a
     * variable, now that a non-zero counter refuses. */
    TEST_ASSERT_TRUE(nx_mutation_baseline_get()->zero_at_capture);

    nx_mutation_counter_note(NX_MUT_FREQUENCY_CONFIG);
    s_voltage = 1200u;
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE * 4ull));
    TEST_ASSERT_EQUAL_UINT64(first_us, nx_mutation_baseline_get()->captured_us);
    TEST_ASSERT_EQUAL_UINT32(
        0u, nx_mutation_baseline_get()->snapshot.counters[NX_MUT_FREQUENCY_CONFIG]);

    /* And the change is still visible against the ORIGINAL baseline. */
    TEST_ASSERT_FALSE(nx_mutation_baseline_compare(&cmp));
    teardown();
}

TEST_CASE("W61-F10 a violation permanently prevents re-baselining",
          "[mutation_observability]")
{
    NxMutationComparison cmp;
    NxBaselinePrereq     p;

    fresh();
    all_prereq(&p);
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));

    /* A mutation happens and is observed. */
    nx_mutation_counter_note(NX_MUT_POOL_CONFIG);
    TEST_ASSERT_FALSE(nx_mutation_baseline_compare(&cmp));
    TEST_ASSERT_TRUE(nx_mutation_baseline_get()->violation_latched);

    /*
     * Even if the baseline were cleared — which nothing in production can do —
     * the latch survives and blocks a fresh capture. "Reset and look clean" is
     * structurally unavailable.
     */
    {
        NxMutationBaseline saved = *nx_mutation_baseline_get();
        (void)saved;
    }
    TEST_ASSERT_EQUAL_INT(NX_BASELINE_BLOCK_ALREADY_READY,
                          nx_mutation_baseline_evaluate(AFTER_SETTLE * 9ull, &p));
    teardown();
}

TEST_CASE("W61-F11 an externally noted violation blocks a first capture",
          "[mutation_observability]")
{
    NxBaselinePrereq p;

    fresh();
    all_prereq(&p);
    /* The integrator saw a B5 owner appear before any baseline existed. */
    nx_mutation_baseline_note_violation();

    TEST_ASSERT_EQUAL_INT(NX_BASELINE_BLOCK_VIOLATION_LATCHED,
                          nx_mutation_baseline_evaluate(AFTER_SETTLE, &p));
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_REFUSED,
                          nx_mutation_baseline_capture(AFTER_SETTLE, &p));
    TEST_ASSERT_EQUAL_UINT64(0u, nx_mutation_baseline_get()->captured_us);
    /* Still refused however long the device waits. */
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_REFUSED,
                          capture_ok(AFTER_SETTLE * 50ull));
    teardown();
}

TEST_CASE("W61-F12 a restored counter cannot un-violate a boot",
          "[mutation_observability]")
{
    NxMutationComparison cmp;

    fresh();
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));

    /* A mutation happens and is observed. */
    nx_mutation_counter_note(NX_MUT_FAN_CONFIG);
    TEST_ASSERT_FALSE(nx_mutation_baseline_compare(&cmp));
    TEST_ASSERT_FALSE(cmp.counters_unchanged);
    TEST_ASSERT_TRUE(nx_mutation_baseline_get()->violation_latched);

    /*
     * The counters are put back to zero behind the pilot's back. The baseline
     * is all-zero by construction, so the DELTA now compares equal again — and
     * without the latch the pilot would announce itself healthy after having
     * already seen a mutation. It must not.
     */
    nx_mutation_counters_reset_for_test();
    TEST_ASSERT_FALSE(nx_mutation_baseline_compare(&cmp));
    TEST_ASSERT_TRUE(cmp.counters_unchanged);   /* the delta really is zero */
    TEST_ASSERT_TRUE(cmp.violation_latched);    /* and it changes nothing   */
    teardown();
}

TEST_CASE("W61-F13 a reboot begins a completely new lifecycle",
          "[mutation_observability]")
{
    fresh();
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));
    nx_mutation_counter_note(NX_MUT_POOL_CONFIG);
    (void)nx_mutation_baseline_compare(NULL);
    nx_mutation_baseline_note_violation();

    /* reset_for_test() is the only production equivalent of a reboot: RAM is
     * gone, so state, both latches and the counters all start over. */
    nx_mutation_counters_reset_for_test();
    nx_mutation_baseline_reset_for_test();
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_NONE, nx_mutation_baseline_state());
    TEST_ASSERT_FALSE(nx_mutation_baseline_get()->violation_latched);
    TEST_ASSERT_FALSE(nx_mutation_baseline_get()->history_lost_latched);
    TEST_ASSERT_EQUAL_UINT32(0u, nx_mutation_baseline_get()->attempts);

    nx_tuning_snapshot_provider_register(test_provider);
    s_provider_available = true;
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));
    teardown();
}

TEST_CASE("W61-F14 every lifecycle and block code has a distinct token",
          "[mutation_observability]")
{
    unsigned i, j;

    for (i = 0; i < (unsigned)NX_MUT_BASELINE__COUNT; i++) {
        const char *a = nx_mutation_baseline_state_str((NxMutationBaselineState)i);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_NOT_EQUAL(0, strcmp(a, "BASELINE_UNKNOWN"));
        for (j = i + 1u; j < (unsigned)NX_MUT_BASELINE__COUNT; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(a,
                nx_mutation_baseline_state_str((NxMutationBaselineState)j)));
        }
    }
    TEST_ASSERT_EQUAL_STRING("BASELINE_UNKNOWN",
        nx_mutation_baseline_state_str((NxMutationBaselineState)42));

    for (i = 0; i < (unsigned)NX_BASELINE_BLOCK__COUNT; i++) {
        const char *a = nx_mutation_baseline_block_str((NxBaselineBlock)i);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_NOT_EQUAL(0, strcmp(a, "BLOCK_UNKNOWN"));
        for (j = i + 1u; j < (unsigned)NX_BASELINE_BLOCK__COUNT; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(a,
                nx_mutation_baseline_block_str((NxBaselineBlock)j)));
        }
    }
    TEST_ASSERT_EQUAL_STRING("BLOCK_UNKNOWN",
        nx_mutation_baseline_block_str((NxBaselineBlock)99));
}

TEST_CASE("W61-F15 evaluate() never changes state",
          "[mutation_observability]")
{
    NxMutationBaseline before;
    NxBaselinePrereq   p;

    fresh();
    all_prereq(&p);
    before = *nx_mutation_baseline_get();

    (void)nx_mutation_baseline_evaluate(0u, &p);
    (void)nx_mutation_baseline_evaluate(AFTER_SETTLE, &p);
    (void)nx_mutation_baseline_evaluate(AFTER_SETTLE, NULL);

    TEST_ASSERT_EQUAL_INT(0, memcmp(&before, nx_mutation_baseline_get(),
                                    sizeof(before)));
    teardown();
}

/* ---------------- G. comparison verdicts ---------------- */

TEST_CASE("W61-G1 an untouched device compares completely clean",
          "[mutation_observability]")
{
    NxMutationComparison cmp;

    fresh();
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));

    TEST_ASSERT_TRUE(nx_mutation_baseline_compare(&cmp));
    TEST_ASSERT_TRUE(cmp.baseline_ready);
    TEST_ASSERT_TRUE(cmp.counters_readable);
    TEST_ASSERT_TRUE(cmp.counters_unchanged);
    TEST_ASSERT_FALSE(cmp.history_lost);
    TEST_ASSERT_TRUE(cmp.tuning_readable);
    TEST_ASSERT_TRUE(cmp.tuning_unchanged);
    TEST_ASSERT_TRUE(cmp.delta.valid);
    TEST_ASSERT_EQUAL_UINT32(0u, cmp.delta.hardware_total);
    /* A clean comparison must NOT latch anything. */
    TEST_ASSERT_FALSE(nx_mutation_baseline_get()->violation_latched);
    teardown();
}

TEST_CASE("W61-G2 each counted class surfaces in the comparison",
          "[mutation_observability]")
{
    NxMutationComparison cmp;

    fresh();
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));

    nx_mutation_counter_note(NX_MUT_VOLTAGE_CONFIG);
    nx_mutation_counter_note(NX_MUT_POOL_CONFIG);
    nx_mutation_counter_note(NX_MUT_PROTOCOL_CONFIG);
    nx_mutation_counter_note(NX_MUT_RESTART_REQUEST);
    nx_mutation_counter_note(NX_MUT_OTA_FIRMWARE);

    TEST_ASSERT_FALSE(nx_mutation_baseline_compare(&cmp));
    TEST_ASSERT_TRUE(cmp.counters_readable);
    TEST_ASSERT_FALSE(cmp.counters_unchanged);
    TEST_ASSERT_FALSE(cmp.history_lost);
    TEST_ASSERT_EQUAL_UINT32(1u, cmp.delta.hardware_total);
    TEST_ASSERT_EQUAL_UINT32(1u, cmp.delta.pool_total);
    TEST_ASSERT_EQUAL_UINT32(1u, cmp.delta.protocol_total);
    TEST_ASSERT_EQUAL_UINT32(1u, cmp.delta.restart_total);
    TEST_ASSERT_EQUAL_UINT32(1u, cmp.delta.ota_total);
    /* The tuning itself did not change, and that stays separately true. */
    TEST_ASSERT_TRUE(cmp.tuning_unchanged);
    teardown();
}

TEST_CASE("W61-G3 a tuning change is caught even with no counted write",
          "[mutation_observability]")
{
    NxMutationComparison cmp;

    fresh();
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));

    /* The configuration moved without passing the counted boundary. The two
     * authorities are independent precisely so this cannot hide. */
    s_voltage = 1200u;

    TEST_ASSERT_FALSE(nx_mutation_baseline_compare(&cmp));
    TEST_ASSERT_TRUE(cmp.counters_unchanged);
    TEST_ASSERT_TRUE(cmp.tuning_readable);
    TEST_ASSERT_FALSE(cmp.tuning_unchanged);
    teardown();
}

TEST_CASE("W61-G4 an unreadable configuration is not reported as a change",
          "[mutation_observability]")
{
    NxMutationComparison cmp;

    fresh();
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));

    s_provider_available = false;
    TEST_ASSERT_FALSE(nx_mutation_baseline_compare(&cmp));
    TEST_ASSERT_FALSE(cmp.tuning_readable);
    TEST_ASSERT_FALSE(cmp.tuning_unchanged);
    /* Unreadable is its own answer: the counters were still fine. */
    TEST_ASSERT_TRUE(cmp.counters_readable);
    TEST_ASSERT_TRUE(cmp.counters_unchanged);
    teardown();
}

TEST_CASE("W61-G5 a regression is reported as lost history, not as zero",
          "[mutation_observability]")
{
    NxMutationSnapshot base, now;
    NxMutationDelta    d;

    /*
     * A PRODUCTION baseline is always all-zero and counters only rise, so a
     * regression against it is unreachable — saturation is the reachable
     * lost-history case (W61-G6). The regression guard is defence in depth
     * against a snapshot from a different authority, and is proven here at the
     * level where it can actually be exercised.
     */
    nx_mutation_counters_reset_for_test();
    nx_mutation_counter_note(NX_MUT_FAN_CONFIG);
    nx_mutation_counter_note(NX_MUT_FAN_CONFIG);
    base = take_snapshot();
    nx_mutation_counters_reset_for_test();
    now = take_snapshot();

    TEST_ASSERT_FALSE(nx_mutation_delta_compute(&base, &now, &d));
    TEST_ASSERT_TRUE(d.regressed);
    TEST_ASSERT_FALSE(d.valid);
    TEST_ASSERT_FALSE(nx_mutation_delta_clean(&d));
    /* The zeroed totals must NOT read as "nothing happened". */
    TEST_ASSERT_EQUAL_UINT32(0u, d.hardware_total);
    nx_mutation_counters_reset_for_test();
}

TEST_CASE("W61-G6 saturation after the baseline is lost history",
          "[mutation_observability]")
{
    NxMutationComparison cmp;

    fresh();
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));

    nx_mutation_counters_preset_for_test(NX_MUT_THERMAL_CONFIG, UINT32_MAX);

    TEST_ASSERT_FALSE(nx_mutation_baseline_compare(&cmp));
    TEST_ASSERT_TRUE(cmp.history_lost);
    TEST_ASSERT_FALSE(cmp.counters_unchanged);
    teardown();
}

TEST_CASE("W61-G7 the comparison never mutates the stored baseline",
          "[mutation_observability]")
{
    NxMutationComparison cmp;
    NxMutationSnapshot   snap_before;
    NxTuningSnapshot     fp_before;
    uint64_t             us_before;

    fresh();
    TEST_ASSERT_EQUAL_INT(NX_MUT_BASELINE_READY, capture_ok(AFTER_SETTLE));
    snap_before = nx_mutation_baseline_get()->snapshot;
    fp_before   = nx_mutation_baseline_get()->tuning;
    us_before   = nx_mutation_baseline_get()->captured_us;

    TEST_ASSERT_TRUE(nx_mutation_baseline_compare(&cmp));
    nx_mutation_counter_note(NX_MUT_POOL_CONFIG);
    TEST_ASSERT_FALSE(nx_mutation_baseline_compare(&cmp));

    /* A verdict with nowhere to report WHY is refused outright. */
    TEST_ASSERT_FALSE(nx_mutation_baseline_compare(NULL));

    TEST_ASSERT_EQUAL_INT(0, memcmp(&snap_before,
                                    &nx_mutation_baseline_get()->snapshot,
                                    sizeof(snap_before)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(&fp_before,
                                    &nx_mutation_baseline_get()->tuning,
                                    sizeof(fp_before)));
    TEST_ASSERT_EQUAL_UINT64(us_before, nx_mutation_baseline_get()->captured_us);
    teardown();
}
