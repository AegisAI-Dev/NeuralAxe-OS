/*
 * Deterministic tests for the Gate W6 recommendation-only pilot diagnostics
 * and invariant monitor.
 *
 * Pure: no NVS, no networking, no tasks, no clock, no transport. All
 * coordinates are synthetic and mid-ocean; no real location appears anywhere.
 */

#include <string.h>
#include "unity.h"
#include "nx_weather_pilot_diag.h"

#define SYN_LAT_E4  123456
#define SYN_LON_E4  -654321

/* ---------------- fixtures ---------------- */

/* A posture whose every required fact was actually sourced. Without this the
 * whole suite would be testing values that carry no provenance. */
static void all_facts_observed(NxWeatherPilotFacts *f)
{
    f->feature_posture   = NX_WX_FACT_STRUCTURAL;
    f->execution         = NX_WX_FACT_STRUCTURAL;
    f->command_api       = NX_WX_FACT_STRUCTURAL;
    f->ownership         = NX_WX_FACT_OBSERVED;
    f->hardware_counters = NX_WX_FACT_OBSERVED;
    f->pool_counters     = NX_WX_FACT_OBSERVED;
    f->restart_counters  = NX_WX_FACT_OBSERVED;
    f->session_counters  = NX_WX_FACT_OBSERVED;
    f->tuning_snapshot   = NX_WX_FACT_OBSERVED;
    f->mining_posture    = NX_WX_FACT_OBSERVED;
    f->resources         = NX_WX_FACT_OBSERVED;
}

static void healthy_posture(NxWeatherPilotPosture *p)
{
    memset(p, 0, sizeof(*p));
    all_facts_observed(&p->facts);
    p->weather_enabled       = true;
    p->source_policy_valid   = true;
    p->recommendation_only   = true;
    p->source_mining_allowed = true;
    p->tuning_unchanged      = true;
    /* every availability flag false, every counter zero */
}

static void ready_config(NxWeatherSourceConfig *c)
{
    nx_weather_source_defaults(c);
    c->enabled             = true;
    c->distribution        = NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL;
    c->provider            = WEATHER_PROVIDER_OPEN_METEO;
    c->latitude_e4         = SYN_LAT_E4;
    c->longitude_e4        = SYN_LON_E4;
    c->timezone            = WEATHER_TZ_EUROPE_BRUSSELS;
    c->recommendation_only = true;
    weather_schedule_config_defaults(&c->schedule);
}

/* ---------------- C. invariant checker ---------------- */

TEST_CASE("w6 inv: the healthy fixture really is healthy", "[nx_wx_pilot]")
{
    /* Guards the whole suite: if the fixture were unhealthy, every
     * "violation" assertion below would hold trivially. */
    NxWeatherPilotPosture p;
    healthy_posture(&p);
    TEST_ASSERT_EQUAL(WX_INV_OK, nx_weather_pilot_check(&p));
    TEST_ASSERT_TRUE(nx_weather_pilot_healthy(nx_weather_pilot_check(&p)));
}

TEST_CASE("w6 inv: a NULL or zeroed posture fails closed", "[nx_wx_pilot]")
{
    NxWeatherPilotPosture zero;
    memset(&zero, 0, sizeof(zero));
    /* A zeroed posture has every fact ABSENT: it must report the PROVENANCE
     * failure, not a value failure, and never OK. */
    TEST_ASSERT_EQUAL(WX_INV_FACT_ABSENT, nx_weather_pilot_check(NULL));
    TEST_ASSERT_EQUAL(WX_INV_FACT_ABSENT, nx_weather_pilot_check(&zero));
    TEST_ASSERT_FALSE(nx_weather_pilot_healthy(nx_weather_pilot_check(NULL)));
}

TEST_CASE("w6 inv: every forbidden surface is a distinct violation", "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p;

    healthy_posture(&p); p.weather_enabled = false;
    TEST_ASSERT_EQUAL(WX_INV_WEATHER_DISABLED, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.source_policy_valid = false;
    TEST_ASSERT_EQUAL(WX_INV_SOURCE_POLICY_INVALID, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.recommendation_only = false;
    TEST_ASSERT_EQUAL(WX_INV_NOT_RECOMMENDATION_ONLY, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.execution_available = true;
    TEST_ASSERT_EQUAL(WX_INV_EXECUTION_AVAILABLE, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.command_api_available = true;
    TEST_ASSERT_EQUAL(WX_INV_COMMAND_API_AVAILABLE, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.b5_owner_present = true;
    TEST_ASSERT_EQUAL(WX_INV_OWNER_PRESENT, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.b7_action_observed = true;
    TEST_ASSERT_EQUAL(WX_INV_EXECUTOR_ACTION, nx_weather_pilot_check(&p));
}

TEST_CASE("w6 inv: any non-zero mutation counter is a violation", "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p;

    healthy_posture(&p); p.hardware_write_count = 1u;
    TEST_ASSERT_EQUAL(WX_INV_HARDWARE_WRITE, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.pool_write_count = 1u;
    TEST_ASSERT_EQUAL(WX_INV_POOL_WRITE, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.protocol_write_count = 1u;
    TEST_ASSERT_EQUAL(WX_INV_PROTOCOL_WRITE, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.restart_request_count = 1u;
    TEST_ASSERT_EQUAL(WX_INV_RESTART_REQUESTED, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.ota_request_count = 1u;
    TEST_ASSERT_EQUAL(WX_INV_OTA_REQUESTED, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.session_mutation_count = 1u;
    TEST_ASSERT_EQUAL(WX_INV_SESSION_MUTATION, nx_weather_pilot_check(&p));
    /* Even a single write counts: there is no tolerance band. */
    healthy_posture(&p); p.hardware_write_count = UINT32_MAX;
    TEST_ASSERT_EQUAL(WX_INV_HARDWARE_WRITE, nx_weather_pilot_check(&p));
}

TEST_CASE("w6 inv: preserved operating posture is required", "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p;

    healthy_posture(&p); p.source_mining_allowed = false;
    TEST_ASSERT_EQUAL(WX_INV_MINING_BLOCKED, nx_weather_pilot_check(&p));
    healthy_posture(&p); p.tuning_unchanged = false;
    TEST_ASSERT_EQUAL(WX_INV_TUNING_CHANGED, nx_weather_pilot_check(&p));
}

TEST_CASE("w6 inv: the first violation in a broken posture is deterministic", "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p;
    int i;

    /* Everything broken at once always reports the same, earliest code. */
    memset(&p, 0, sizeof(p));
    all_facts_observed(&p.facts);
    p.execution_available   = true;
    p.command_api_available = true;
    p.b5_owner_present      = true;
    p.hardware_write_count  = 9u;
    for (i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(WX_INV_WEATHER_DISABLED, nx_weather_pilot_check(&p));
    }
}

TEST_CASE("w6 inv: the checker mutates nothing it is given", "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p, before;
    int i;

    healthy_posture(&p);
    p.hardware_write_count = 3u;   /* a violation, so the path is non-trivial */
    before = p;
    for (i = 0; i < 8; i++) {
        (void)nx_weather_pilot_check(&p);
    }
    TEST_ASSERT_EQUAL_MEMORY(&before, &p, sizeof(p));
}

TEST_CASE("w6 inv: violation tokens are stable, unique and value-free", "[nx_wx_pilot]")
{
    int i, j;

    for (i = 0; i < (int)WX_INV__COUNT; i++) {
        const char *t = nx_weather_pilot_invariant_str((NxWeatherPilotInvariant)i);
        TEST_ASSERT_NOT_NULL(t);
        for (const char *c = t; *c; c++) {
            TEST_ASSERT_TRUE(*c < '0' || *c > '9');   /* no coordinate digit */
        }
        TEST_ASSERT_NULL(strstr(t, "."));
        TEST_ASSERT_NULL(strstr(t, "http"));
        for (j = 0; j < i; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(t,
                nx_weather_pilot_invariant_str((NxWeatherPilotInvariant)j)));
        }
    }
    /* Out of range fails closed — never to OK. */
    TEST_ASSERT_NOT_EQUAL(0, strcmp("WX_INV_OK",
        nx_weather_pilot_invariant_str((NxWeatherPilotInvariant)99)));
}

/* ---------------- authoritative provenance (Gate W6 blocker) ---------------- */

TEST_CASE("w6 fact: only an authority-sourced fact is usable", "[nx_wx_pilot]")
{
    TEST_ASSERT_TRUE(nx_weather_fact_usable(NX_WX_FACT_OBSERVED));
    /* STRUCTURAL is stronger than a runtime read: the symbol is not linked. */
    TEST_ASSERT_TRUE(nx_weather_fact_usable(NX_WX_FACT_STRUCTURAL));
    TEST_ASSERT_FALSE(nx_weather_fact_usable(NX_WX_FACT_ABSENT));
    TEST_ASSERT_FALSE(nx_weather_fact_usable(NX_WX_FACT_UNAVAILABLE));
    TEST_ASSERT_FALSE(nx_weather_fact_usable(NX_WX_FACT_STALE));
    TEST_ASSERT_FALSE(nx_weather_fact_usable((NxWeatherFactState)99));
}

TEST_CASE("w6 fact: every required field must carry provenance", "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p;
    NxWeatherFactState *fields[11];
    unsigned i;

    healthy_posture(&p);
    fields[0]  = &p.facts.feature_posture;
    fields[1]  = &p.facts.execution;
    fields[2]  = &p.facts.command_api;
    fields[3]  = &p.facts.ownership;
    fields[4]  = &p.facts.hardware_counters;
    fields[5]  = &p.facts.pool_counters;
    fields[6]  = &p.facts.restart_counters;
    fields[7]  = &p.facts.session_counters;
    fields[8]  = &p.facts.tuning_snapshot;
    fields[9]  = &p.facts.mining_posture;
    fields[10] = &p.facts.resources;

    /* Knocking out ANY single field must break health — proving no field is
     * optional and none is silently defaulted. */
    for (i = 0; i < 11u; i++) {
        NxWeatherFactState saved = *fields[i];
        *fields[i] = NX_WX_FACT_ABSENT;
        TEST_ASSERT_EQUAL(WX_INV_FACT_ABSENT, nx_weather_pilot_check(&p));
        *fields[i] = NX_WX_FACT_UNAVAILABLE;
        TEST_ASSERT_EQUAL(WX_INV_FACT_UNAVAILABLE, nx_weather_pilot_check(&p));
        *fields[i] = NX_WX_FACT_STALE;
        TEST_ASSERT_EQUAL(WX_INV_FACT_STALE, nx_weather_pilot_check(&p));
        *fields[i] = (NxWeatherFactState)99;
        TEST_ASSERT_EQUAL(WX_INV_FACT_ABSENT, nx_weather_pilot_check(&p));
        *fields[i] = saved;
        TEST_ASSERT_EQUAL(WX_INV_OK, nx_weather_pilot_check(&p));
    }
}

TEST_CASE("w6 fact: provenance outranks convenient values", "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p;

    /*
     * The exact defect this blocker targets: a posture whose VALUES all look
     * perfect but whose counters were never sourced. It must not pass.
     */
    healthy_posture(&p);
    p.facts.hardware_counters = NX_WX_FACT_UNAVAILABLE;
    TEST_ASSERT_EQUAL_UINT32(0u, p.hardware_write_count);   /* looks fine ... */
    TEST_ASSERT_EQUAL(WX_INV_FACT_UNAVAILABLE,              /* ... still not */
                      nx_weather_pilot_check(&p));
    TEST_ASSERT_FALSE(nx_weather_pilot_healthy(nx_weather_pilot_check(&p)));
}

TEST_CASE("w6 fact: a counter that goes backwards is ambiguous", "[nx_wx_pilot]")
{
    TEST_ASSERT_TRUE(nx_weather_pilot_counter_sane(0u, 0u));
    TEST_ASSERT_TRUE(nx_weather_pilot_counter_sane(0u, 1u));
    TEST_ASSERT_TRUE(nx_weather_pilot_counter_sane(5u, 5u));
    TEST_ASSERT_TRUE(nx_weather_pilot_counter_sane(5u, UINT32_MAX));
    /* A reset, a wrap or a different authority — all unprovable. */
    TEST_ASSERT_FALSE(nx_weather_pilot_counter_sane(1u, 0u));
    TEST_ASSERT_FALSE(nx_weather_pilot_counter_sane(UINT32_MAX, 0u));
    TEST_ASSERT_FALSE(nx_weather_pilot_counter_sane(9u, 8u));
}

TEST_CASE("w6 fact: state tokens are stable, unique and value-free", "[nx_wx_pilot]")
{
    int i, j;

    for (i = 0; i < (int)NX_WX_FACT__COUNT; i++) {
        const char *t = nx_weather_fact_state_str((NxWeatherFactState)i);
        TEST_ASSERT_NOT_NULL(t);
        for (const char *c = t; *c; c++) {
            TEST_ASSERT_TRUE(*c < '0' || *c > '9');
        }
        for (j = 0; j < i; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(t,
                nx_weather_fact_state_str((NxWeatherFactState)j)));
        }
    }
    TEST_ASSERT_EQUAL_STRING("WX_FACT_ABSENT",
        nx_weather_fact_state_str((NxWeatherFactState)99));
}

TEST_CASE("w6 fact: the checker mutates neither posture nor provenance", "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p, before;
    int i;

    healthy_posture(&p);
    p.facts.pool_counters = NX_WX_FACT_UNAVAILABLE;   /* non-trivial path */
    before = p;
    for (i = 0; i < 8; i++) {
        (void)nx_weather_pilot_check(&p);
    }
    TEST_ASSERT_EQUAL_MEMORY(&before, &p, sizeof(p));
}

/* ---------------- B. event vocabulary ---------------- */

TEST_CASE("w6 ev: event tokens are stable, unique and value-free", "[nx_wx_pilot]")
{
    int i, j;

    for (i = 0; i < (int)WX_EV__COUNT; i++) {
        const char *t = nx_weather_pilot_event_str((NxWeatherPilotEvent)i);
        TEST_ASSERT_NOT_NULL(t);
        for (const char *c = t; *c; c++) {
            TEST_ASSERT_TRUE(*c < '0' || *c > '9');
        }
        TEST_ASSERT_NULL(strstr(t, "."));
        TEST_ASSERT_NULL(strstr(t, "http"));
        TEST_ASSERT_NULL(strstr(t, "open-meteo"));
        TEST_ASSERT_NULL(strstr(t, "api"));
        for (j = 0; j < i; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(t,
                nx_weather_pilot_event_str((NxWeatherPilotEvent)j)));
        }
    }
    TEST_ASSERT_EQUAL_STRING("WX_EV_NONE",
        nx_weather_pilot_event_str((NxWeatherPilotEvent)99));
}

TEST_CASE("w6 ev: classification follows the runtime gate order", "[nx_wx_pilot]")
{
    WeatherRecommendation rec;

    memset(&rec, 0, sizeof(rec));

    /* Configuration outranks runtime position. */
    TEST_ASSERT_EQUAL(WX_EV_CONFIG_UNCONFIGURED,
        nx_weather_pilot_classify(NX_WX_SRC_UNCONFIGURED,
            WEATHER_RUNTIME_RECOMMENDATION_READY, &rec, true));
    TEST_ASSERT_EQUAL(WX_EV_CONFIG_INVALID,
        nx_weather_pilot_classify(NX_WX_SRC_INVALID_PROVIDER,
            WEATHER_RUNTIME_RECOMMENDATION_READY, &rec, true));

    /* Then the runtime states. */
    TEST_ASSERT_EQUAL(WX_EV_WAIT_TRUSTED_TIME,
        nx_weather_pilot_classify(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME, &rec, false));
    TEST_ASSERT_EQUAL(WX_EV_FETCH_TIMEOUT,
        nx_weather_pilot_classify(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_BOUNDED_TIMEOUT, &rec, true));
    TEST_ASSERT_EQUAL(WX_EV_FETCH_REJECTED,
        nx_weather_pilot_classify(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_WEATHER_REJECTED, &rec, true));
    TEST_ASSERT_EQUAL(WX_EV_FORECAST_STALE,
        nx_weather_pilot_classify(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_WEATHER_STALE, &rec, true));

    /* The schedule distinguishes waiting from due. */
    TEST_ASSERT_EQUAL(WX_EV_WAIT_SCHEDULE,
        nx_weather_pilot_classify(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_WAITING_FOR_WEATHER, &rec, false));
    TEST_ASSERT_EQUAL(WX_EV_SCHEDULE_DUE,
        nx_weather_pilot_classify(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_WAITING_FOR_WEATHER, &rec, true));

    /* And the recommendation shape. */
    rec.present = true; rec.actionable_in_future_gate = true;
    TEST_ASSERT_EQUAL(WX_EV_RECOMMENDATION_READY,
        nx_weather_pilot_classify(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_RECOMMENDATION_READY, &rec, true));
    rec.actionable_in_future_gate = false;
    TEST_ASSERT_EQUAL(WX_EV_RECOMMENDATION_NOT_ACTIONABLE,
        nx_weather_pilot_classify(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_RECOMMENDATION_READY, &rec, true));
    rec.present = false;
    TEST_ASSERT_EQUAL(WX_EV_NO_RECOMMENDATION,
        nx_weather_pilot_classify(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_RECOMMENDATION_READY, &rec, true));
    /* A NULL recommendation is never "ready". */
    TEST_ASSERT_EQUAL(WX_EV_NO_RECOMMENDATION,
        nx_weather_pilot_classify(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_RECOMMENDATION_READY, NULL, true));
}

/* ---------------- rate limiting and edge triggering ---------------- */

TEST_CASE("w6 emit: transitions are edge-triggered, not per-step", "[nx_wx_pilot]")
{
    NxWeatherPilotDiag d;
    NxWeatherPilotLine line;
    int i;

    nx_weather_pilot_diag_init(&d);
    TEST_ASSERT_TRUE(nx_weather_pilot_should_emit(&d, WX_EV_WAIT_SCHEDULE, 0));
    TEST_ASSERT_TRUE(nx_weather_pilot_record(&d, WX_EV_WAIT_SCHEDULE, 0, NULL,
        NX_WX_SRC_READY_RECOMMENDATION_ONLY, WEATHER_RUNTIME_WAITING_FOR_WEATHER,
        NULL, false, WX_INV_OK, &line));

    /* An hours-long wait must not produce a log flood. */
    for (i = 0; i < 5000; i++) {
        TEST_ASSERT_FALSE(nx_weather_pilot_should_emit(&d, WX_EV_WAIT_SCHEDULE,
                                                       (uint64_t)i * 1000ull));
    }
    /* A genuine transition still emits. */
    TEST_ASSERT_TRUE(nx_weather_pilot_should_emit(&d, WX_EV_SCHEDULE_DUE, 1));
}

TEST_CASE("w6 emit: the summary is limited to once per 60 seconds", "[nx_wx_pilot]")
{
    NxWeatherPilotDiag d;
    NxWeatherPilotLine line;

    nx_weather_pilot_diag_init(&d);
    /* The first summary is always allowed. */
    TEST_ASSERT_TRUE(nx_weather_pilot_should_emit(&d, WX_EV_PILOT_SUMMARY, 0));
    TEST_ASSERT_TRUE(nx_weather_pilot_record(&d, WX_EV_PILOT_SUMMARY, 0, NULL,
        NX_WX_SRC_UNCONFIGURED, WEATHER_RUNTIME_DISABLED, NULL, false,
        WX_INV_OK, &line));

    TEST_ASSERT_FALSE(nx_weather_pilot_should_emit(&d, WX_EV_PILOT_SUMMARY, 1));
    TEST_ASSERT_FALSE(nx_weather_pilot_should_emit(&d, WX_EV_PILOT_SUMMARY,
        NX_WX_PILOT_SUMMARY_MIN_INTERVAL_US - 1ull));
    TEST_ASSERT_TRUE(nx_weather_pilot_should_emit(&d, WX_EV_PILOT_SUMMARY,
        NX_WX_PILOT_SUMMARY_MIN_INTERVAL_US));
    TEST_ASSERT_TRUE(nx_weather_pilot_should_emit(&d, WX_EV_PILOT_SUMMARY,
        NX_WX_PILOT_SUMMARY_MIN_INTERVAL_US * 4ull));
}

TEST_CASE("w6 emit: a violation is never suppressed or deduplicated", "[nx_wx_pilot]")
{
    NxWeatherPilotDiag d;
    NxWeatherPilotLine line;
    int i;

    nx_weather_pilot_diag_init(&d);
    for (i = 0; i < 32; i++) {
        TEST_ASSERT_TRUE(nx_weather_pilot_should_emit(&d,
            WX_EV_INVARIANT_VIOLATION, (uint64_t)i));
        TEST_ASSERT_TRUE(nx_weather_pilot_record(&d, WX_EV_INVARIANT_VIOLATION,
            (uint64_t)i, NULL, NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_RECOMMENDATION_READY, NULL, true,
            WX_INV_HARDWARE_WRITE, &line));
    }
    TEST_ASSERT_EQUAL_UINT32(32u, d.violations);
    /* The FIRST violation is retained, not overwritten by later ones. */
    TEST_ASSERT_EQUAL(WX_INV_HARDWARE_WRITE, d.first_violation);
}

TEST_CASE("w6 emit: an uninitialised or invalid request emits nothing", "[nx_wx_pilot]")
{
    NxWeatherPilotDiag d;
    NxWeatherPilotLine line;

    memset(&d, 0, sizeof(d));   /* not initialised */
    TEST_ASSERT_FALSE(nx_weather_pilot_should_emit(&d, WX_EV_PILOT_BOOT, 0));
    TEST_ASSERT_FALSE(nx_weather_pilot_record(&d, WX_EV_PILOT_BOOT, 0, NULL,
        NX_WX_SRC_UNCONFIGURED, WEATHER_RUNTIME_DISABLED, NULL, false,
        WX_INV_OK, &line));

    nx_weather_pilot_diag_init(&d);
    TEST_ASSERT_FALSE(nx_weather_pilot_should_emit(&d, WX_EV_NONE, 0));
    TEST_ASSERT_FALSE(nx_weather_pilot_should_emit(&d,
        (NxWeatherPilotEvent)99, 0));
    TEST_ASSERT_FALSE(nx_weather_pilot_record(&d, WX_EV_NONE, 0, NULL,
        NX_WX_SRC_UNCONFIGURED, WEATHER_RUNTIME_DISABLED, NULL, false,
        WX_INV_OK, &line));
    TEST_ASSERT_FALSE(nx_weather_pilot_record(&d, WX_EV_PILOT_BOOT, 0, NULL,
        NX_WX_SRC_UNCONFIGURED, WEATHER_RUNTIME_DISABLED, NULL, false,
        WX_INV_OK, NULL));
}

TEST_CASE("w6 emit: the sequence is monotonic and gap-free", "[nx_wx_pilot]")
{
    NxWeatherPilotDiag d;
    NxWeatherPilotLine line;
    uint32_t prev = 0u;
    int i;

    nx_weather_pilot_diag_init(&d);
    for (i = 0; i < 50; i++) {
        TEST_ASSERT_TRUE(nx_weather_pilot_record(&d, WX_EV_INVARIANT_VIOLATION,
            (uint64_t)i, NULL, NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_RECOMMENDATION_READY, NULL, true,
            WX_INV_POOL_WRITE, &line));
        TEST_ASSERT_EQUAL_UINT32(prev + 1u, line.sequence);
        prev = line.sequence;
    }
}

/* ---------------- E/H. recommendation-only and privacy ---------------- */

TEST_CASE("w6 line: executed is false on every single line", "[nx_wx_pilot]")
{
    NxWeatherPilotDiag d;
    NxWeatherPilotLine line;
    NxWeatherSourceConfig cfg;
    WeatherRecommendation rec;
    int e;

    ready_config(&cfg);
    memset(&rec, 0, sizeof(rec));
    rec.present = true;
    rec.actionable_in_future_gate = true;

    /* Even for the most "successful" outcome, and for every event token. */
    for (e = 1; e < (int)WX_EV__COUNT; e++) {
        nx_weather_pilot_diag_init(&d);
        TEST_ASSERT_TRUE(nx_weather_pilot_record(&d, (NxWeatherPilotEvent)e, 0,
            &cfg, NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_RECOMMENDATION_READY, &rec, true, WX_INV_OK,
            &line));
        TEST_ASSERT_FALSE(line.executed);
    }
}

TEST_CASE("w6 line: configuration is reported as shape, never as value", "[nx_wx_pilot]")
{
    NxWeatherPilotDiag d;
    NxWeatherPilotLine line;
    NxWeatherSourceConfig cfg;

    ready_config(&cfg);
    nx_weather_pilot_diag_init(&d);
    TEST_ASSERT_TRUE(nx_weather_pilot_record(&d, WX_EV_CONFIG_READY, 0, &cfg,
        NX_WX_SRC_READY_RECOMMENDATION_ONLY, WEATHER_RUNTIME_WAITING_FOR_WEATHER,
        NULL, false, WX_INV_OK, &line));

    /* The booleans are true ... */
    TEST_ASSERT_TRUE(line.provider_configured);
    TEST_ASSERT_TRUE(line.location_configured);
    TEST_ASSERT_TRUE(line.timezone_configured);
    TEST_ASSERT_TRUE(line.recommendation_only);

    /* ... and the private values appear nowhere in the emitted line. The
     * line is scanned byte-for-byte for the synthetic coordinates. */
    {
        const unsigned char *raw = (const unsigned char *)&line;
        int32_t lat = SYN_LAT_E4, lon = SYN_LON_E4;
        size_t i;
        bool found_lat = false, found_lon = false;
        for (i = 0; i + sizeof(int32_t) <= sizeof(line); i++) {
            int32_t v;
            memcpy(&v, raw + i, sizeof(v));
            if (v == lat) found_lat = true;
            if (v == lon) found_lon = true;
        }
        TEST_ASSERT_FALSE(found_lat);
        TEST_ASSERT_FALSE(found_lon);
    }
}

TEST_CASE("w6 line: an unconfigured source reports every shape false", "[nx_wx_pilot]")
{
    NxWeatherPilotDiag d;
    NxWeatherPilotLine line;
    NxWeatherSourceConfig cfg;

    nx_weather_source_defaults(&cfg);
    nx_weather_pilot_diag_init(&d);
    TEST_ASSERT_TRUE(nx_weather_pilot_record(&d, WX_EV_CONFIG_UNCONFIGURED, 0,
        &cfg, NX_WX_SRC_UNCONFIGURED, WEATHER_RUNTIME_SOURCE_UNCONFIGURED,
        NULL, false, WX_INV_OK, &line));
    TEST_ASSERT_FALSE(line.provider_configured);
    TEST_ASSERT_FALSE(line.location_configured);
    TEST_ASSERT_FALSE(line.timezone_configured);
    TEST_ASSERT_FALSE(line.recommendation_present);
    TEST_ASSERT_FALSE(line.executed);
    TEST_ASSERT_EQUAL_UINT32(0u, line.fetch_attempts);
}

TEST_CASE("w6 line: fetch and recommendation counters are bounded and honest", "[nx_wx_pilot]")
{
    NxWeatherPilotDiag d;
    NxWeatherPilotLine line;
    int i;

    nx_weather_pilot_diag_init(&d);
    for (i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(nx_weather_pilot_record(&d, WX_EV_FETCH_START,
            (uint64_t)i, NULL, NX_WX_SRC_READY_RECOMMENDATION_ONLY,
            WEATHER_RUNTIME_WAITING_FOR_WEATHER, NULL, true, WX_INV_OK, &line));
    }
    TEST_ASSERT_EQUAL_UINT32(3u, line.fetch_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, line.recommendations);

    TEST_ASSERT_TRUE(nx_weather_pilot_record(&d, WX_EV_RECOMMENDATION_READY, 9,
        NULL, NX_WX_SRC_READY_RECOMMENDATION_ONLY,
        WEATHER_RUNTIME_RECOMMENDATION_READY, NULL, true, WX_INV_OK, &line));
    TEST_ASSERT_EQUAL_UINT32(1u, line.recommendations);
    TEST_ASSERT_FALSE(line.executed);
}

TEST_CASE("w6 prop: diagnostics never mutate the inputs they observe", "[nx_wx_pilot]")
{
    NxWeatherPilotDiag d;
    NxWeatherPilotLine line;
    NxWeatherSourceConfig cfg, cfg_before;
    WeatherRecommendation rec, rec_before;

    ready_config(&cfg);
    cfg_before = cfg;
    memset(&rec, 0, sizeof(rec));
    rec.present = true;
    rec_before = rec;

    nx_weather_pilot_diag_init(&d);
    (void)nx_weather_pilot_record(&d, WX_EV_RECOMMENDATION_READY, 0, &cfg,
        NX_WX_SRC_READY_RECOMMENDATION_ONLY,
        WEATHER_RUNTIME_RECOMMENDATION_READY, &rec, true, WX_INV_OK, &line);

    TEST_ASSERT_EQUAL_MEMORY(&cfg_before, &cfg, sizeof(cfg));
    TEST_ASSERT_EQUAL_MEMORY(&rec_before, &rec, sizeof(rec));
}

/* ------------------------------------------------------------------ */
/* Gate W6.1 — authoritative mutation evidence in the pure checker     */
/* ------------------------------------------------------------------ */

/* These reuse the healthy_posture() fixture above, so each test below
 * changes exactly one thing and the resulting code is unambiguous. */

TEST_CASE("w61 check: lost counter history is reported as a regression",
          "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p;

    healthy_posture(&p);
    TEST_ASSERT_EQUAL_INT(WX_INV_OK, nx_weather_pilot_check(&p));

    /* A counter that regressed or saturated means the interval since the
     * baseline cannot be reconstructed. Zero deltas across that gap are not
     * evidence of calm. */
    p.counter_history_lost = true;
    TEST_ASSERT_EQUAL_INT(WX_INV_COUNTER_REGRESSION, nx_weather_pilot_check(&p));
    TEST_ASSERT_EQUAL_UINT32(0u, p.hardware_write_count);
    TEST_ASSERT_EQUAL_UINT32(0u, p.pool_write_count);
}

TEST_CASE("w61 check: lost history outranks the individual counter values",
          "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p;

    healthy_posture(&p);
    p.counter_history_lost = true;
    p.hardware_write_count = 5u;
    p.pool_write_count     = 7u;

    /* The deltas are meaningless once the history is gone, so the ambiguity
     * is reported rather than a specific class that may be wrong. */
    TEST_ASSERT_EQUAL_INT(WX_INV_COUNTER_REGRESSION, nx_weather_pilot_check(&p));
}

TEST_CASE("w61 check: an absent mutation fact never passes on zeros",
          "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p;

    /* Exactly the state during the settle window: the counters are readable
     * but no baseline exists yet, so the facts are ABSENT and every count is
     * zero. The pilot must NOT call that healthy. */
    healthy_posture(&p);
    p.facts.hardware_counters = NX_WX_FACT_ABSENT;
    p.facts.pool_counters     = NX_WX_FACT_ABSENT;
    p.facts.restart_counters  = NX_WX_FACT_ABSENT;
    p.facts.tuning_snapshot   = NX_WX_FACT_ABSENT;
    TEST_ASSERT_EQUAL_INT(WX_INV_FACT_ABSENT, nx_weather_pilot_check(&p));

    /* And the flag-off image, where no counter exists at all. */
    healthy_posture(&p);
    p.facts.hardware_counters = NX_WX_FACT_UNAVAILABLE;
    p.facts.pool_counters     = NX_WX_FACT_UNAVAILABLE;
    p.facts.restart_counters  = NX_WX_FACT_UNAVAILABLE;
    p.facts.tuning_snapshot   = NX_WX_FACT_UNAVAILABLE;
    TEST_ASSERT_EQUAL_INT(WX_INV_FACT_UNAVAILABLE, nx_weather_pilot_check(&p));
}

TEST_CASE("w61 check: each mutation class maps to its own violation",
          "[nx_wx_pilot]")
{
    NxWeatherPilotPosture p;

    healthy_posture(&p);
    p.hardware_write_count = 1u;
    TEST_ASSERT_EQUAL_INT(WX_INV_HARDWARE_WRITE, nx_weather_pilot_check(&p));

    healthy_posture(&p);
    p.pool_write_count = 1u;
    TEST_ASSERT_EQUAL_INT(WX_INV_POOL_WRITE, nx_weather_pilot_check(&p));

    healthy_posture(&p);
    p.protocol_write_count = 1u;
    TEST_ASSERT_EQUAL_INT(WX_INV_PROTOCOL_WRITE, nx_weather_pilot_check(&p));

    healthy_posture(&p);
    p.restart_request_count = 1u;
    TEST_ASSERT_EQUAL_INT(WX_INV_RESTART_REQUESTED, nx_weather_pilot_check(&p));

    healthy_posture(&p);
    p.ota_request_count = 1u;
    TEST_ASSERT_EQUAL_INT(WX_INV_OTA_REQUESTED, nx_weather_pilot_check(&p));

    healthy_posture(&p);
    p.session_mutation_count = 1u;
    TEST_ASSERT_EQUAL_INT(WX_INV_SESSION_MUTATION, nx_weather_pilot_check(&p));

    healthy_posture(&p);
    p.tuning_unchanged = false;
    TEST_ASSERT_EQUAL_INT(WX_INV_TUNING_CHANGED, nx_weather_pilot_check(&p));
}

TEST_CASE("w61 line: the evidence block carries only bounded scalars",
          "[nx_wx_pilot]")
{
    NxWeatherPilotDiag d;
    NxWeatherPilotLine line;

    nx_weather_pilot_diag_init(&d);
    memset(&line, 0xEE, sizeof(line));
    TEST_ASSERT_TRUE(nx_weather_pilot_record(&d, WX_EV_PILOT_SUMMARY, 1000,
        NULL, NX_WX_SRC_READY_RECOMMENDATION_ONLY,
        WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME, NULL, false,
        WX_INV_OK, &line));

    /* record() zeroes the whole line, so the W6.1 evidence a caller has not
     * filled in yet can never be stale bytes from a previous emission. */
    TEST_ASSERT_EQUAL_UINT8(0u, line.baseline_state);
    TEST_ASSERT_EQUAL_UINT64(0u, line.baseline_us);
    TEST_ASSERT_FALSE(line.baseline_zero_at_capture);
    TEST_ASSERT_FALSE(line.mutation_observability_present);
    TEST_ASSERT_FALSE(line.counter_history_lost);
    TEST_ASSERT_EQUAL_UINT32(0u, line.mut_hardware);
    TEST_ASSERT_EQUAL_UINT32(0u, line.mut_pool);
    TEST_ASSERT_EQUAL_UINT32(0u, line.mut_protocol);
    TEST_ASSERT_EQUAL_UINT32(0u, line.mut_restart);
    TEST_ASSERT_EQUAL_UINT32(0u, line.mut_ota);
    TEST_ASSERT_EQUAL_UINT32(0u, line.mut_session);
    TEST_ASSERT_FALSE(line.tuning_unchanged);
    TEST_ASSERT_FALSE(line.executed);
}
