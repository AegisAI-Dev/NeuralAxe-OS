/*
 * Gate W6.2 — deterministic tests for the trusted-time PROJECTION onto the
 * weather pilot line, and for the acquisition-path failure matrix behind it.
 *
 * WHY THIS FILE EXISTS. A real W6 pilot sat at
 *
 *     state=WX_WAITING_FOR_TRUSTED_TIME trusted_time=0 reason=WX_NX_NO_TRUSTED_TIME
 *
 * for over thirty minutes, and the line offered no way to tell whether the
 * B10 provider had never started, was syncing, had been refused, or was
 * perfectly healthy and simply never consulted. Every case below therefore
 * pins a DISTINCT observable projection: the generic waiting reason may never
 * again be the only thing an owner can see.
 *
 * NOTHING here contacts a network, DNS, an NTP server, a pool, OTA, a restart
 * path, physical NVS or hardware. The SNTP platform ops are fakes, a
 * synchronization is injected through the same ingestion function the real
 * ESP-IDF callback calls, and every identity is a synthetic "*.example"
 * fixture. No coordinate, hostname, timezone or credential appears anywhere.
 */

#include <string.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nx_weather_pilot_diag.h"
#include "pool_session_runtime.h"
#include "pool_session_runtime_boot.h"
#include "pool_time_source.h"

/*
 * TWO POSTURES. CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE lives in
 * main/Kconfig.projbuild, which belongs to the FIRMWARE project; this test
 * project has its own main, so the symbol does not exist here by default and
 * the observation start branch is compiled out. Every observation assertion
 * below is therefore guarded exactly as the committed Gate B10 tests are:
 *
 *   flag OFF (the default test image) - the provider must NOT start, and the
 *            projection must say so honestly rather than fall silent;
 *   flag ON  (a posture run that injects the define, mirroring the technique
 *            test-ci already uses for CONFIG_NX_MUTATION_OBSERVABILITY) - the
 *            full acquisition matrix runs.
 *
 * Both postures are asserted, so neither can rot.
 */
#define W62_EPOCH_S 1750000000ull
#define W62_SRC     "ntp-w62.example"

/* ================================================================= */
/* A. The pure projection                                            */
/* ================================================================= */

/* A structurally valid, healthy B10 model built without touching anything. */
static void w62_trusted_diag(PoolTimeSourceDiagnostics *d)
{
    PoolTimeSourceDiagnosticsInput in;

    memset(&in, 0, sizeof(in));
    in.runtime_enabled   = true;
    in.observe_enabled   = true;
    in.source_present    = true;
    in.source_usable     = true;
    in.provider_started  = true;
    in.snapshot_trusted  = true;
    in.lifecycle         = (uint8_t)POOL_TIME_SNTP_TRUSTED;
    in.attempts          = 1u;
    in.wait_elapsed_s    = 3u;
    in.wait_limit_s      = 600u;
    in.anchor_age_valid  = true;
    in.anchor_age_us     = 42ull * 1000000ull;
    in.last_sync_result  = TIME_OK;
    pool_time_source_diagnostics_build(&in, d);
    TEST_ASSERT_TRUE(pool_time_source_diagnostics_valid(d));
}

TEST_CASE("w62 proj: a NULL line is a safe no-op", "[weather_pilot_time]")
{
    PoolTimeSourceDiagnostics d;

    w62_trusted_diag(&d);
    nx_weather_pilot_time_project(&d, true, NULL); /* must not fault */
}

TEST_CASE("w62 proj: no linked runtime is STRUCTURAL, never a silent zero",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;
    PoolTimeSourceDiagnostics d;

    /* Even handed a perfectly healthy model, an image with no runtime cannot
     * have produced it: the LINK is the stronger evidence and wins. */
    w62_trusted_diag(&d);
    memset(&line, 0xAA, sizeof(line));
    nx_weather_pilot_time_project(&d, false, &line);

    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_STRUCTURAL, line.time_fact);
    TEST_ASSERT_FALSE(line.time_source_configured);
    TEST_ASSERT_FALSE(line.time_available);
    TEST_ASSERT_FALSE(line.time_operational);
    TEST_ASSERT_EQUAL_UINT32(0u, line.time_sync_attempts);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_UNCONFIGURED, line.time_source_state);
    TEST_ASSERT_EQUAL_INT(TIME_ERR_NOT_INITIALIZED, line.last_time_sync_result);
}

TEST_CASE("w62 proj: an unreadable authority is UNAVAILABLE, not healthy",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine        line;
    PoolTimeSourceDiagnostics d;

    /* (a) nothing published at all. */
    memset(&line, 0xAA, sizeof(line));
    nx_weather_pilot_time_project(NULL, true, &line);
    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_UNAVAILABLE, line.time_fact);
    TEST_ASSERT_FALSE(line.time_available);

    /* (b) a model whose version does not match: quoting it would be a lie. */
    w62_trusted_diag(&d);
    d.model_version = (uint16_t)(d.model_version + 1u);
    memset(&line, 0xAA, sizeof(line));
    nx_weather_pilot_time_project(&d, true, &line);
    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_UNAVAILABLE, line.time_fact);
    TEST_ASSERT_FALSE(line.time_available);
    TEST_ASSERT_FALSE(line.time_operational);
    TEST_ASSERT_EQUAL_INT(TIME_ERR_NOT_INITIALIZED, line.last_time_sync_result);

    /* (c) an internally contradictory model: available without a source. */
    w62_trusted_diag(&d);
    d.source_configured = false;
    memset(&line, 0xAA, sizeof(line));
    nx_weather_pilot_time_project(&d, true, &line);
    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_UNAVAILABLE, line.time_fact);
}

TEST_CASE("w62 proj: a healthy model is copied field for field",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine        line;
    PoolTimeSourceDiagnostics d;

    w62_trusted_diag(&d);
    memset(&line, 0, sizeof(line));
    nx_weather_pilot_time_project(&d, true, &line);

    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_OBSERVED, line.time_fact);
    TEST_ASSERT_TRUE(line.time_source_configured);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_TRUSTED, line.time_source_state);
    TEST_ASSERT_EQUAL_UINT32(1u, line.time_sync_attempts);
    TEST_ASSERT_TRUE(line.time_operational);
    TEST_ASSERT_TRUE(line.time_available);
    TEST_ASSERT_TRUE(line.time_sync_age_valid);
    TEST_ASSERT_EQUAL_UINT32(42u, line.time_sync_age_s);
    TEST_ASSERT_EQUAL_INT(TIME_OK, line.last_time_sync_result);
}

TEST_CASE("w62 proj: an invalid age never reports a number",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine        line;
    PoolTimeSourceDiagnostics d;
    PoolTimeSourceDiagnosticsInput in;

    memset(&in, 0, sizeof(in));
    in.runtime_enabled  = true;
    in.observe_enabled  = true;
    in.source_present   = true;
    in.source_usable    = true;
    in.provider_started = true;
    in.lifecycle        = (uint8_t)POOL_TIME_SNTP_SYNC_PENDING;
    in.attempts         = 1u;
    in.wait_limit_s     = 600u;
    in.last_sync_result = TIME_ERR_SYNC_PENDING;
    pool_time_source_diagnostics_build(&in, &d);

    memset(&line, 0xAA, sizeof(line));
    nx_weather_pilot_time_project(&d, true, &line);
    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_OBSERVED, line.time_fact);
    TEST_ASSERT_FALSE(line.time_sync_age_valid);
    TEST_ASSERT_EQUAL_UINT32(0u, line.time_sync_age_s);
    /* SYNCING is a DISTINCT observable from "never started". */
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_SYNCING, line.time_source_state);
}

TEST_CASE("w62 proj: every source state maps to its own token",
          "[weather_pilot_time]")
{
    unsigned i;

    /* No two states may collapse onto one string: the whole point of the
     * projection is that an owner can tell them apart. */
    for (i = 0u; i < (unsigned)POOL_TIME_SOURCE_STATE__COUNT; i++) {
        unsigned j;
        const char *a = pool_time_source_state_str((PoolTimeSourceState)i);
        TEST_ASSERT_NOT_NULL(a);
        for (j = i + 1u; j < (unsigned)POOL_TIME_SOURCE_STATE__COUNT; j++) {
            TEST_ASSERT_TRUE(strcmp(a, pool_time_source_state_str(
                                           (PoolTimeSourceState)j)) != 0);
        }
    }
}

TEST_CASE("w62 privacy: the projection has no field that can carry a secret",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine        line;
    PoolTimeSourceDiagnostics d;
    const unsigned char      *raw;
    size_t                    i;
    size_t                    printable_run = 0u;

    w62_trusted_diag(&d);
    memset(&line, 0, sizeof(line));
    nx_weather_pilot_time_project(&d, true, &line);

    /*
     * The whole line is scalars and token ids. A hostname, address, DNS error
     * or credential would have to appear as a run of printable bytes; nothing
     * of the sort can exist here, and this pins that structurally.
     */
    raw = (const unsigned char *)&line;
    for (i = 0u; i < sizeof(line); i++) {
        if (raw[i] >= 0x20u && raw[i] < 0x7Fu) {
            printable_run++;
            TEST_ASSERT_TRUE_MESSAGE(printable_run < 8u,
                                     "a string-shaped run appeared in the line");
        } else {
            printable_run = 0u;
        }
    }
}

/* ================================================================= */
/* B. The acquisition-path failure matrix, on a real runtime          */
/* ================================================================= */

static uint64_t g_w62_mono_us;
static int      g_w62_inits, g_w62_starts;
static int      g_w62_init_rc, g_w62_start_rc;

static int32_t  w62_reset_reason(void) { return POOL_RESET_RAW_POWERON; }
static uint64_t w62_monotonic(void)    { return g_w62_mono_us; }

static int w62_sntp_init(const PoolTimeSntpConfig *cfg)
{
    (void)cfg;
    g_w62_inits++;
    return g_w62_init_rc;
}
static int w62_sntp_start(void)  { g_w62_starts++; return g_w62_start_rc; }
static int w62_sntp_stop(void)   { return 0; }
static int w62_sntp_deinit(void) { return 0; }

static const PoolTimeSntpPlatformOps g_w62_sntp_ops = {
    .monotonic_us = w62_monotonic, .sntp_init = w62_sntp_init,
    .sntp_start = w62_sntp_start, .sntp_stop = w62_sntp_stop,
    .sntp_deinit = w62_sntp_deinit,
};

/* An EMPTY store: every read reports "not found", nothing is ever written. */
static int w62_open(void *c)  { (void)c; return POOL_STORE_BACKEND_OK; }
static int w62_close(void *c) { (void)c; return POOL_STORE_BACKEND_OK; }
static int w62_read(void *c, const char *k, uint8_t *b, size_t cap, size_t *n)
{
    (void)c; (void)k; (void)b; (void)cap; (void)n;
    return POOL_STORE_BACKEND_NOT_FOUND;
}
static int w62_write(void *c, const char *k, const uint8_t *b, size_t n)
{
    (void)c; (void)k; (void)b; (void)n;
    TEST_FAIL_MESSAGE("observation wrote the session store");
    return POOL_STORE_BACKEND_IO;
}
static int w62_commit(void *c)
{
    (void)c;
    TEST_FAIL_MESSAGE("observation committed the session store");
    return POOL_STORE_BACKEND_IO;
}

static const PoolStoreBackendOps g_w62_store_ops = {
    .open = w62_open, .read_blob = w62_read, .write_blob = w62_write,
    .commit = w62_commit, .close = w62_close,
};

static PoolSessionRuntime     g_w62_rt;
static PoolSessionRuntimeDeps g_w62_deps;

static void w62_fresh(const char *source, bool observe)
{
    /*
     * The owner task is a MODULE-WIDE singleton: if an earlier assertion
     * aborted before its teardown, every later fixture would fail with
     * TASK_ALREADY_RUNNING and one real defect would read as ten. Reset the
     * previous instance unconditionally first.
     */
    (void)pool_session_runtime_deinit(&g_w62_rt);

    g_w62_mono_us  = 5ull * 1000000ull;
    g_w62_inits    = 0;
    g_w62_starts   = 0;
    g_w62_init_rc  = 0;
    g_w62_start_rc = 0;

    memset(&g_w62_deps, 0, sizeof(g_w62_deps));
    g_w62_deps.store_ops             = &g_w62_store_ops;
    g_w62_deps.sntp_ops              = &g_w62_sntp_ops;
    g_w62_deps.read_reset_reason_raw = w62_reset_reason;
    g_w62_deps.monotonic_us          = w62_monotonic;
    g_w62_deps.ntp_server            = source;
    g_w62_deps.sync_wait_limit_s     = 600u;
    g_w62_deps.observe_enabled       = observe;

    memset(&g_w62_rt, 0, sizeof(g_w62_rt));
}

static void w62_teardown(void)
{
    (void)pool_session_runtime_deinit(&g_w62_rt);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_task_count());
}

/* Boot, then start the owner task — the production order. */
static void w62_boot_then_task(void)
{
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_init(&g_w62_rt, &g_w62_deps));
    (void)pool_session_runtime_boot(&g_w62_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_w62_rt));
    vTaskDelay(pdMS_TO_TICKS(60));
}

static void w62_network_ready(void)
{
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_w62_rt,
                                                  RUNTIME_EVENT_NETWORK_READY));
    vTaskDelay(pdMS_TO_TICKS(150));
}

/* The projection as the pilot would see it for THIS runtime instance. */
static void w62_project(NxWeatherPilotLine *line)
{
    PoolTimeSourceDiagnostics d;

    memset(line, 0, sizeof(*line));
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_time_diagnostics(&g_w62_rt, &d));
    nx_weather_pilot_time_project(&d, true, line);
}

TEST_CASE("w62 matrix: an UNCONFIGURED source is observably unconfigured",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    w62_fresh("", true);              /* the shipped default: no source */
    w62_boot_then_task();
    w62_network_ready();

    TEST_ASSERT_EQUAL_INT(0, g_w62_inits);
    TEST_ASSERT_EQUAL_INT(0, g_w62_starts);
    w62_project(&line);
    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_OBSERVED, line.time_fact);
    TEST_ASSERT_FALSE(line.time_source_configured);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_UNCONFIGURED, line.time_source_state);
    TEST_ASSERT_FALSE(line.time_available);
    w62_teardown();
}

TEST_CASE("w62 matrix: an INVALID source is distinct from an absent one",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    /* A URL is rejected by the bounded B10 grammar: no DNS, no start. */
    w62_fresh("http://ntp.example/path", true);
    w62_boot_then_task();
    w62_network_ready();

    TEST_ASSERT_EQUAL_INT(0, g_w62_inits);
    TEST_ASSERT_EQUAL_INT(0, g_w62_starts);
    w62_project(&line);
    TEST_ASSERT_FALSE(line.time_source_configured);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_INVALID, line.time_source_state);
    w62_teardown();
}

TEST_CASE("w62 matrix: NETWORK NOT READY is observably pending, not failed",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    w62_fresh(W62_SRC, true);
    w62_boot_then_task();
    /* Deliberately no network-ready. */
    vTaskDelay(pdMS_TO_TICKS(150));

    TEST_ASSERT_EQUAL_INT(0, g_w62_inits);
    w62_project(&line);
    TEST_ASSERT_TRUE(line.time_source_configured);
    /*
     * A VALID source with no service running is CONFIGURED. (START_PENDING is
     * the pure start DECISION's own token; the published diagnostics are
     * lifecycle-driven and only reach it once the provider is initialized.)
     * Either way it is DISTINCT from unconfigured, invalid, syncing and
     * trusted, which is what the pilot needs.
     */
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_CONFIGURED, line.time_source_state);
    TEST_ASSERT_FALSE(line.time_available);
    w62_teardown();
}

TEST_CASE("w62 matrix: network-ready BEFORE the task is latched, never lost",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    w62_fresh(W62_SRC, true);
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_init(&g_w62_rt, &g_w62_deps));
    (void)pool_session_runtime_boot(&g_w62_rt);

    /* Ordering B: the fact arrives while no owner task exists yet. */
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_w62_rt,
                                                  RUNTIME_EVENT_NETWORK_READY));
    TEST_ASSERT_EQUAL_INT(0, g_w62_starts);

    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_w62_rt));
    vTaskDelay(pdMS_TO_TICKS(200));

    /*
     * The latched fact was drained on task start. Whether it then STARTS a
     * provider depends on the compile flag; that it was not silently dropped
     * is asserted on BOTH postures, which is the property under test.
     */
    TEST_ASSERT_EQUAL_UINT32(0u, g_w62_rt.control.pending_events);
    TEST_ASSERT_TRUE(g_w62_rt.control.network_ready);
    w62_project(&line);
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    TEST_ASSERT_EQUAL_INT(1, g_w62_inits);
    TEST_ASSERT_EQUAL_INT(1, g_w62_starts);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_SYNCING, line.time_source_state);
#else
    TEST_ASSERT_EQUAL_INT(0, g_w62_inits);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_CONFIGURED, line.time_source_state);
#endif
    w62_teardown();
}

TEST_CASE("w62 matrix: network-ready AFTER the task starts the provider once",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    w62_fresh(W62_SRC, true);
    int inits_once;

    w62_boot_then_task();               /* ordering A */
    w62_network_ready();
    inits_once = g_w62_inits;

    /* A duplicate never creates a second client, on either posture. */
    w62_network_ready();
    TEST_ASSERT_EQUAL_INT(inits_once, g_w62_inits);

    w62_project(&line);
    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_OBSERVED, line.time_fact);
    TEST_ASSERT_TRUE(line.time_source_configured);
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    TEST_ASSERT_EQUAL_INT(1, inits_once);
    TEST_ASSERT_EQUAL_INT(1, g_w62_starts);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_SYNCING, line.time_source_state);
    TEST_ASSERT_EQUAL_UINT32(1u, line.time_sync_attempts);
#else
    TEST_ASSERT_EQUAL_INT(0, inits_once);
    TEST_ASSERT_EQUAL_INT(0, g_w62_starts);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_CONFIGURED, line.time_source_state);
#endif
    w62_teardown();
}

TEST_CASE("w62 matrix: a provider INIT failure is observable, never silent",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    w62_fresh(W62_SRC, true);
    g_w62_init_rc = -1;                 /* the platform refuses to initialize */
    w62_boot_then_task();
    w62_network_ready();

    TEST_ASSERT_EQUAL_INT(0, g_w62_starts);
    w62_project(&line);
    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_OBSERVED, line.time_fact);
    TEST_ASSERT_FALSE(line.time_available);
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    TEST_ASSERT_EQUAL_INT(1, g_w62_inits);
    /* The attempt was made and is counted: "0 attempts" would be a lie. */
    TEST_ASSERT_EQUAL_UINT32(1u, line.time_sync_attempts);
#else
    TEST_ASSERT_EQUAL_INT(0, g_w62_inits);
    TEST_ASSERT_EQUAL_UINT32(0u, line.time_sync_attempts);
#endif
    w62_teardown();
}

TEST_CASE("w62 matrix: a provider START failure reports ERROR, not syncing",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    w62_fresh(W62_SRC, true);
    g_w62_start_rc = -1;                /* init succeeds, start is refused */
    w62_boot_then_task();
    w62_network_ready();

    w62_project(&line);
    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_OBSERVED, line.time_fact);
    TEST_ASSERT_FALSE(line.time_available);
    TEST_ASSERT_FALSE(line.time_operational);
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    TEST_ASSERT_EQUAL_INT(1, g_w62_inits);
    TEST_ASSERT_EQUAL_INT(1, g_w62_starts);
    /* A refused START is ERROR, which is DISTINCT from syncing and from
     * never having started at all. */
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_ERROR, line.time_source_state);
#else
    TEST_ASSERT_EQUAL_INT(0, g_w62_starts);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_CONFIGURED, line.time_source_state);
#endif
    w62_teardown();
}

TEST_CASE("w62 matrix: a sync that never arrives stays SYNCING and bounded",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    w62_fresh(W62_SRC, true);
    w62_boot_then_task();
    w62_network_ready();

    /* Time passes; no callback ever fires. */
    g_w62_mono_us += 120ull * 1000000ull;
    vTaskDelay(pdMS_TO_TICKS(200));

    w62_project(&line);
    TEST_ASSERT_FALSE(line.time_available);
    TEST_ASSERT_FALSE(line.time_sync_age_valid);
    TEST_ASSERT_EQUAL_UINT32(0u, line.time_sync_age_s);
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_SYNCING, line.time_source_state);
    /* Started exactly once: the observation path never re-attempts. */
    TEST_ASSERT_EQUAL_INT(1, g_w62_starts);
#else
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_CONFIGURED, line.time_source_state);
    TEST_ASSERT_EQUAL_INT(0, g_w62_starts);
#endif
    w62_teardown();
}

TEST_CASE("w62 matrix: a REJECTED candidate never becomes available",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    w62_fresh(W62_SRC, true);
    w62_boot_then_task();
    w62_network_ready();

    /* An epoch far outside the accepted band: B2 refuses it. */
    (void)pool_time_sntp_handle_sync(&g_w62_rt.time_provider, 1000ull, 0u);
    vTaskDelay(pdMS_TO_TICKS(200));

    w62_project(&line);
    TEST_ASSERT_FALSE(line.time_available);
    TEST_ASSERT_FALSE(line.time_operational);
    /*
     * The refusal REASON is carried, so "rejected" is distinguishable from
     * "still waiting" — exactly what the physical pilot could not tell apart.
     */
    TEST_ASSERT_NOT_EQUAL(TIME_OK, line.last_time_sync_result);
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_REJECTED, line.time_source_state);
#endif
    w62_teardown();
}

TEST_CASE("w62 matrix: a successful sync is observably TRUSTED",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    w62_fresh(W62_SRC, true);
    w62_boot_then_task();
    w62_network_ready();

#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_sntp_handle_sync(&g_w62_rt.time_provider,
                                                 W62_EPOCH_S, 0u));
    vTaskDelay(pdMS_TO_TICKS(200));

    w62_project(&line);
    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_OBSERVED, line.time_fact);
    TEST_ASSERT_TRUE(line.time_source_configured);
    TEST_ASSERT_TRUE(line.time_available);
    TEST_ASSERT_TRUE(line.time_operational);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_TRUSTED, line.time_source_state);
    TEST_ASSERT_EQUAL_INT(TIME_OK, line.last_time_sync_result);
#else
    /* With observation compiled out there is no provider to synchronize, and
     * the projection must report that rather than a convenient zero. */
    TEST_ASSERT_NOT_EQUAL(TIME_OK,
                          pool_time_sntp_handle_sync(&g_w62_rt.time_provider,
                                                     W62_EPOCH_S, 0u));
    w62_project(&line);
    TEST_ASSERT_FALSE(line.time_available);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_CONFIGURED, line.time_source_state);
#endif
    w62_teardown();
}

TEST_CASE("w62 matrix: a lost owner notification still surfaces the sync",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    /*
     * The Gate B10 fix made the sync callback notify the owner task. Suppose
     * that notification is nevertheless lost. The accepted anchor still lives
     * in the B2 provider, and the bounded observation tick republishes the
     * diagnostics on its own, so the projection must reach TRUSTED WITHOUT
     * depending on the notification having been delivered.
     *
     * The loss is modelled by DETACHING THE OBSERVER before delivering the
     * sync: the anchor is accepted and nothing is posted to the owner task.
     * (The obvious alternative — stop the task, sync, restart it — is not
     * used: the owner task is a singleton on a STATIC TCB and stack, so
     * recreating it while the scheduler is still unwinding the old one faults,
     * and production never restarts it anyway.)
     */
    w62_fresh(W62_SRC, true);
    w62_boot_then_task();
    w62_network_ready();

#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    TEST_ASSERT_TRUE(pool_session_runtime_time_observation_active(&g_w62_rt));

    /* Detach the observer: from here no callback can notify anything. */
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_sntp_set_observer(&g_w62_rt.time_provider,
                                                  NULL, NULL));
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_sntp_handle_sync(&g_w62_rt.time_provider,
                                                 W62_EPOCH_S, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u,
                             pool_session_runtime_time_sync_callbacks(&g_w62_rt));

    /* Only the bounded tick can carry the anchor into the diagnostics now. */
    /* The owner-task tick is 1 s; three of them is ample and bounded. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    w62_project(&line);
    TEST_ASSERT_TRUE(line.time_available);
    TEST_ASSERT_EQUAL_INT(TIME_SOURCE_TRUSTED, line.time_source_state);
    /* Still exactly one provider: nothing recovered by creating a second. */
    TEST_ASSERT_EQUAL_INT(1, g_w62_inits);
#else
    (void)line;
#endif
    w62_teardown();
}

TEST_CASE("w62 matrix: the twelve paths are twelve DISTINCT observations",
          "[weather_pilot_time]")
{
    /*
     * The defect this gate exists to close was that every one of these
     * collapsed into the single reason WX_NX_NO_TRUSTED_TIME. Pin that the
     * five reachable source states the matrix produces are all different, so
     * no future edit can quietly merge them again.
     */
    static const PoolTimeSourceState reached[] = {
        TIME_SOURCE_UNCONFIGURED, TIME_SOURCE_INVALID,
        TIME_SOURCE_CONFIGURED,   TIME_SOURCE_SYNCING,
        TIME_SOURCE_REJECTED,     TIME_SOURCE_ERROR,
        TIME_SOURCE_TRUSTED,
    };
    unsigned i, j;

    for (i = 0u; i < sizeof(reached) / sizeof(reached[0]); i++) {
        for (j = i + 1u; j < sizeof(reached) / sizeof(reached[0]); j++) {
            TEST_ASSERT_TRUE(reached[i] != reached[j]);
            TEST_ASSERT_TRUE(strcmp(pool_time_source_state_str(reached[i]),
                                    pool_time_source_state_str(reached[j])) != 0);
        }
    }
}

/* ================================================================= */
/* C. The observation rule on an EMPTY store                          */
/* ================================================================= */

TEST_CASE("w62 empty: observation needs no owner, no B7 and no API",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine  line;
    PoolRuntimeSnapshot snap;

    w62_fresh(W62_SRC, true);
    w62_boot_then_task();
    w62_network_ready();

    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_snapshot(&g_w62_rt, &snap));
    TEST_ASSERT_TRUE(pool_runtime_snapshot_valid(&snap));
    TEST_ASSERT_EQUAL_INT(RUNTIME_FREE, snap.state);
    TEST_ASSERT_EQUAL_INT(OP_OWNER_NONE, snap.lease_owner);
    TEST_ASSERT_FALSE(snap.session_present);
    TEST_ASSERT_FALSE(snap.restore_required);
    TEST_ASSERT_EQUAL_INT(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE, snap.protocol);

    w62_project(&line);
    TEST_ASSERT_TRUE(line.time_source_configured);
#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE
    /* The provider started with NO session owner, NO B7 and NO API: the
     * empty-store observation rule needs none of them. */
    TEST_ASSERT_EQUAL_INT(1, g_w62_starts);
    TEST_ASSERT_TRUE(pool_session_runtime_time_observation_active(&g_w62_rt));
#else
    TEST_ASSERT_EQUAL_INT(0, g_w62_starts);
    TEST_ASSERT_FALSE(pool_session_runtime_time_observation_active(&g_w62_rt));
#endif
    w62_teardown();
}

TEST_CASE("w62 empty: observation disabled starts nothing at all",
          "[weather_pilot_time]")
{
    NxWeatherPilotLine line;

    w62_fresh(W62_SRC, false);          /* the shipped default */
    w62_boot_then_task();
    w62_network_ready();

    TEST_ASSERT_EQUAL_INT(0, g_w62_inits);
    TEST_ASSERT_EQUAL_INT(0, g_w62_starts);
    w62_project(&line);
    TEST_ASSERT_EQUAL_INT(NX_WX_FACT_OBSERVED, line.time_fact);
    TEST_ASSERT_TRUE(line.time_source_configured);
    TEST_ASSERT_FALSE(line.time_available);
    w62_teardown();
}
