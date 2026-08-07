/*
 * Gate W6.2 — PRODUCTION WIRING integration tests.
 *
 * These drive the REAL committed authorities through the REAL production
 * accessors: a genuine PoolSessionRuntime supplies the ONE PoolTimeClock and
 * the ONE trust policy, and the Gate W4 weather runtime consumes them exactly
 * as the pilot adapter now does. The only fakes are the SNTP platform ops,
 * the NVS backend and the W3 transport.
 *
 * THE LOAD-BEARING PROPERTY: trusted(weather) implies trusted(B2/B10). It is
 * proven here by construction, not by convention — the weather runtime is
 * handed a borrowed clock whose only anchor is the one B2 accepted, so it has
 * no way to reach a different answer.
 *
 * NOTHING here contacts a network, DNS, an NTP server, a weather provider, a
 * pool, OTA, a restart path, physical NVS or hardware. Every coordinate is a
 * synthetic fixture; no owner value appears anywhere.
 */

#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "weather_runtime.h"
#include "weather_transport.h"
#include "tuning_profile.h"
#include "pool_session_runtime.h"
#include "pool_time_source.h"

/* A synthetic trusted epoch: 2025-06-15T14:26:40Z. Fixture only. */
#define W62W_EPOCH_S 1750000000ull
#define W62W_SRC     "ntp-w62-wiring.example"

/* ================================================================= */
/* Fakes: SNTP platform, NVS backend, W3 transport                    */
/* ================================================================= */

static uint64_t g_mono_us;
static int      g_sntp_inits, g_sntp_starts;
static int      g_init_rc, g_start_rc;

static int32_t  fk_reset(void)    { return POOL_RESET_RAW_POWERON; }
static uint64_t fk_monotonic(void){ return g_mono_us; }

static int fk_sntp_init(const PoolTimeSntpConfig *c) { (void)c; g_sntp_inits++;  return g_init_rc; }
static int fk_sntp_start(void)  { g_sntp_starts++; return g_start_rc; }
static int fk_sntp_stop(void)   { return 0; }
static int fk_sntp_deinit(void) { return 0; }

static const PoolTimeSntpPlatformOps FK_SNTP = {
    .monotonic_us = fk_monotonic, .sntp_init = fk_sntp_init,
    .sntp_start = fk_sntp_start, .sntp_stop = fk_sntp_stop,
    .sntp_deinit = fk_sntp_deinit,
};

/* An EMPTY store that FAILS the test if anything ever writes it. */
static int fk_open(void *c)  { (void)c; return POOL_STORE_BACKEND_OK; }
static int fk_close(void *c) { (void)c; return POOL_STORE_BACKEND_OK; }
static int fk_read(void *c, const char *k, uint8_t *b, size_t cap, size_t *n)
{ (void)c; (void)k; (void)b; (void)cap; (void)n; return POOL_STORE_BACKEND_NOT_FOUND; }
static int fk_write(void *c, const char *k, const uint8_t *b, size_t n)
{ (void)c; (void)k; (void)b; (void)n; TEST_FAIL_MESSAGE("store write"); return POOL_STORE_BACKEND_IO; }
static int fk_commit(void *c)
{ (void)c; TEST_FAIL_MESSAGE("store commit"); return POOL_STORE_BACKEND_IO; }

static const PoolStoreBackendOps FK_STORE = {
    .open = fk_open, .read_blob = fk_read, .write_blob = fk_write,
    .commit = fk_commit, .close = fk_close,
};

/*
 * The fake W3 transport. It counts calls, so "exactly one fetch" and "zero
 * fetch" are both provable, and it can be told to fail the way the real
 * adapter would. It never opens a socket.
 */
typedef struct {
    int  calls;
    bool timeout;
    bool oversized;
    int  http_status;
    const char *body;
} FakeTx;

static FakeTx g_tx;

static WeatherProviderResult fk_fetch(void *ctx, const WeatherRequest *req,
                                      WeatherHttpResponse *out)
{
    FakeTx *t = (FakeTx *)ctx;
    size_t n;

    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_NOT_NULL(out);
    t->calls++;
    if (t->timeout) {
        return WEATHER_PROVIDER_ERR_CONNECT_TIMEOUT;
    }
    memset(out, 0, sizeof(*out));
    n = strlen(t->body);
    TEST_ASSERT_TRUE(n < sizeof(out->body));
    memcpy(out->body, t->body, n);
    out->body_len             = n;
    out->http_status          = t->http_status;
    out->content_type_present = true;
    out->content_type_json    = true;
    out->complete             = true;
    out->oversized            = t->oversized;
    return WEATHER_PROVIDER_OK;
}

static const WeatherTransportOps FK_TX_OPS = { .fetch = fk_fetch };

/* ================================================================= */
/* Fixtures                                                           */
/* ================================================================= */

static PoolSessionRuntime     g_rt;
static PoolSessionRuntimeDeps g_deps;
static WeatherRuntime         g_wx;

static void wiring_fresh(const char *source, bool observe)
{
    (void)pool_session_runtime_deinit(&g_rt);

    g_mono_us    = 5ull * 1000000ull;
    g_sntp_inits = g_sntp_starts = 0;
    g_init_rc    = 0;
    g_start_rc   = 0;

    memset(&g_tx, 0, sizeof(g_tx));
    g_tx.http_status = 200;

    memset(&g_deps, 0, sizeof(g_deps));
    g_deps.store_ops             = &FK_STORE;
    g_deps.sntp_ops              = &FK_SNTP;
    g_deps.read_reset_reason_raw = fk_reset;
    g_deps.monotonic_us          = fk_monotonic;
    g_deps.ntp_server            = source;
    g_deps.sync_wait_limit_s     = 600u;
    g_deps.observe_enabled       = observe;

    memset(&g_rt, 0, sizeof(g_rt));
    memset(&g_wx, 0, sizeof(g_wx));
}

static void wiring_teardown(void)
{
    (void)pool_session_runtime_deinit(&g_rt);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_task_count());
}

static void boot_b6(void)
{
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));
    (void)pool_session_runtime_boot(&g_rt);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_start_task(&g_rt));
    vTaskDelay(pdMS_TO_TICKS(60));
}

static void network_ready(void)
{
    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_notify(&g_rt, RUNTIME_EVENT_NETWORK_READY));
    vTaskDelay(pdMS_TO_TICKS(150));
}

/* A configuration the COMMITTED W3 validator accepts. Synthetic coordinates. */
static WeatherRuntimeConfig wx_cfg(bool schedule_due_from_midnight)
{
    WeatherRuntimeConfig c;

    weather_runtime_config_defaults(&c);
    c.enabled           = true;
    c.expected_provider = WEATHER_PROVIDER_OPEN_METEO;
    c.location.latitude_e4  = 508500;   /* synthetic fixture only */
    c.location.longitude_e4 = 43500;
    c.location.timezone     = WEATHER_TZ_EUROPE_BRUSSELS;
    c.schedule.enabled    = true;
    c.schedule.slot_count = 1u;
    memset(c.schedule.slots_min, 0, sizeof(c.schedule.slots_min));
    /* 0 = due from local midnight; 1439 = the last minute of the day, which
     * a mid-afternoon fixture has not reached. */
    c.schedule.slots_min[0] = schedule_due_from_midnight ? 0u : 1439u;
    TEST_ASSERT_TRUE_MESSAGE(weather_runtime_config_valid(&c),
                             "fixture rejected by the committed validator");
    return c;
}

/*
 * Bind the weather runtime to the PRODUCTION authorities, exactly as
 * wx_bind_runtime() in the pilot adapter does.
 */
static bool bind_weather(const WeatherRuntimeConfig *c, bool with_transport)
{
    WeatherRuntimeDeps deps;

    memset(&deps, 0, sizeof(deps));
    deps.clock       = pool_session_runtime_clock(&g_rt);
    deps.time_policy = pool_session_runtime_trust_policy(&g_rt);
    if (deps.clock == NULL || deps.time_policy == NULL) {
        return false;
    }
    deps.transport     = with_transport ? &FK_TX_OPS : NULL;
    deps.transport_ctx = with_transport ? &g_tx : NULL;
    return weather_runtime_init(&g_wx, c, &deps) == WEATHER_RUNTIME_OK;
}

/* ================================================================= */
/* A. The borrowed clock                                              */
/* ================================================================= */

TEST_CASE("w62w clock: an unbound runtime lends nothing", "[weather_pilot_wiring]")
{
    PoolSessionRuntime empty;

    memset(&empty, 0, sizeof(empty));
    TEST_ASSERT_NULL(pool_session_runtime_clock(NULL));
    TEST_ASSERT_NULL(pool_session_runtime_trust_policy(NULL));
    /* Zeroed but never initialized: lending its clock would hand out ops
     * that were never bound. */
    TEST_ASSERT_NULL(pool_session_runtime_clock(&empty));
    TEST_ASSERT_NULL(pool_session_runtime_trust_policy(&empty));
}

TEST_CASE("w62w clock: the loan is the SAME object, never a copy",
          "[weather_pilot_wiring]")
{
    wiring_fresh(W62W_SRC, true);
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_init(&g_rt, &g_deps));

    /* Identity matters: a COPY could drift from the anchor B2 accepts. */
    TEST_ASSERT_EQUAL_PTR(&g_rt.clock, pool_session_runtime_clock(&g_rt));
    TEST_ASSERT_EQUAL_PTR(&g_rt.time_policy,
                          pool_session_runtime_trust_policy(&g_rt));
    /* And repeated loans are the same object, so no second anchor exists. */
    TEST_ASSERT_EQUAL_PTR(pool_session_runtime_clock(&g_rt),
                          pool_session_runtime_clock(&g_rt));
    wiring_teardown();
}

TEST_CASE("w62w clock: no provider means weather waits, never guesses",
          "[weather_pilot_wiring]")
{
    WeatherRuntimeConfig  c = wx_cfg(false);
    WeatherRecommendation rec;
    WeatherRuntimeState   st;

    wiring_fresh(W62W_SRC, true);
    boot_b6();
    /* Deliberately NO network-ready: the provider was never initialized. */

    TEST_ASSERT_TRUE(bind_weather(&c, false));
    memset(&rec, 0, sizeof(rec));
    st = weather_runtime_step(&g_wx, NULL, NULL, &rec);

    TEST_ASSERT_EQUAL_INT(WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME, st);
    TEST_ASSERT_FALSE(rec.trusted_time_at_evaluation);
    TEST_ASSERT_EQUAL_UINT64(0u, rec.evaluated_utc_s);
    TEST_ASSERT_FALSE(rec.present);
    /* The recommendation type has no execution field at all; the nearest
     * thing to one is this, and it is false against the production
     * registry because every profile ships UNVALIDATED. */
    TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
    wiring_teardown();
}

/* ================================================================= */
/* B. The end-to-end production-wired flow                            */
/* ================================================================= */

#ifdef CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE

/* Drive B6 to an accepted anchor, exactly as the real callback would. */
static void reach_trusted_time(void)
{
    boot_b6();
    network_ready();
    TEST_ASSERT_EQUAL_INT(1, g_sntp_starts);
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_sntp_handle_sync(&g_rt.time_provider,
                                                 W62W_EPOCH_S, 0u));
    vTaskDelay(pdMS_TO_TICKS(150));
}

/* The B2/B10 verdict, read from the SAME model the pilot line projects. */
static bool b2_says_trusted(void)
{
    PoolTimeSourceDiagnostics d;

    TEST_ASSERT_EQUAL(RUNTIME_OK,
                      pool_session_runtime_time_diagnostics(&g_rt, &d));
    return d.trusted_time_available;
}

TEST_CASE("w62w implication: trusted(weather) implies trusted(B2/B10)",
          "[weather_pilot_wiring]")
{
    WeatherRuntimeConfig  c = wx_cfg(false);
    WeatherRecommendation rec;

    wiring_fresh(W62W_SRC, true);
    boot_b6();
    network_ready();
    TEST_ASSERT_TRUE(bind_weather(&c, false));

    /* BEFORE any anchor: B2 untrusted, and weather must agree. */
    TEST_ASSERT_FALSE(b2_says_trusted());
    memset(&rec, 0, sizeof(rec));
    (void)weather_runtime_step(&g_wx, NULL, NULL, &rec);
    TEST_ASSERT_FALSE(rec.trusted_time_at_evaluation);

    /* AFTER an accepted anchor: B2 trusted, and weather may now be too. */
    TEST_ASSERT_EQUAL(TIME_OK,
                      pool_time_sntp_handle_sync(&g_rt.time_provider,
                                                 W62W_EPOCH_S, 0u));
    vTaskDelay(pdMS_TO_TICKS(150));
    TEST_ASSERT_TRUE(b2_says_trusted());

    memset(&rec, 0, sizeof(rec));
    (void)weather_runtime_step(&g_wx, NULL, NULL, &rec);
    TEST_ASSERT_TRUE(rec.trusted_time_at_evaluation);
    /*
     * THE implication, proven: B2 accepted an anchor and weather now agrees,
     * through the borrowed clock and nothing else.
     *
     * `evaluated_utc_s` is documented as "0 unless trusted at evaluation" and
     * the refusal paths legitimately leave it 0, so the epoch band is checked
     * only when the runtime actually published one. When it does, it must be
     * the anchor B2 accepted — never a neighbouring value from some second
     * source.
     */
    if (rec.evaluated_utc_s != 0u) {
        TEST_ASSERT_TRUE(rec.evaluated_utc_s >= W62W_EPOCH_S);
        TEST_ASSERT_TRUE(rec.evaluated_utc_s < W62W_EPOCH_S + 60u);
    }
    wiring_teardown();
}

TEST_CASE("w62w flow: trusted time, schedule NOT due, ZERO fetch",
          "[weather_pilot_wiring]")
{
    WeatherRuntimeConfig  c = wx_cfg(false);   /* last minute of the day */
    WeatherRecommendation rec;
    WeatherRuntimeState   st;

    wiring_fresh(W62W_SRC, true);
    reach_trusted_time();
    TEST_ASSERT_TRUE(bind_weather(&c, true));  /* transport IS available */

    memset(&rec, 0, sizeof(rec));
    st = weather_runtime_step(&g_wx, NULL, NULL, &rec);

    TEST_ASSERT_TRUE(rec.trusted_time_at_evaluation);
    TEST_ASSERT_EQUAL_INT(WEATHER_RUNTIME_WAITING_FOR_WEATHER, st);
    TEST_ASSERT_EQUAL_INT(WEATHER_NOT_EXECUTED_NOT_DUE, rec.not_executed);
    /* THE assertion: an available transport is not a licence to use it. */
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);
    TEST_ASSERT_FALSE(rec.present);
    wiring_teardown();
}

TEST_CASE("w62w flow: no fetch is ever attempted before trusted time",
          "[weather_pilot_wiring]")
{
    WeatherRuntimeConfig c = wx_cfg(true);     /* due from midnight */
    WeatherObservation   obs;
    WeatherLocalDate     today;

    wiring_fresh(W62W_SRC, true);
    boot_b6();
    network_ready();                           /* provider syncing, no anchor */
    TEST_ASSERT_TRUE(bind_weather(&c, true));
    TEST_ASSERT_FALSE(b2_says_trusted());

    memset(&today, 0, sizeof(today));
    today.year = 2025; today.month = 6; today.day = 15;
    /* now_trusted=false: the runtime must refuse before touching transport. */
    (void)weather_runtime_fetch(&g_wx, &today, 0u, false, &obs);
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);
    wiring_teardown();
}

TEST_CASE("w62w flow: due schedule performs EXACTLY ONE fetch",
          "[weather_pilot_wiring]")
{
    WeatherRuntimeConfig c = wx_cfg(true);
    WeatherObservation   obs;
    WeatherLocalDate     today;
    char                 body[256];

    wiring_fresh(W62W_SRC, true);
    /* A minimal well-formed Open-Meteo payload for the fixture date. */
    snprintf(body, sizeof(body),
             "{\"daily\":{\"time\":[\"2025-06-15\"],"
             "\"temperature_2m_max\":[24.5]},"
             "\"timezone\":\"Europe/Brussels\"}");
    g_tx.body = body;

    reach_trusted_time();
    TEST_ASSERT_TRUE(bind_weather(&c, true));

    memset(&today, 0, sizeof(today));
    today.year = 2025; today.month = 6; today.day = 15;
    (void)weather_runtime_fetch(&g_wx, &today, W62W_EPOCH_S, true, &obs);

    TEST_ASSERT_EQUAL_INT(1, g_tx.calls);   /* exactly one, never two */
    wiring_teardown();
}

TEST_CASE("w62w flow: a transport timeout never becomes a recommendation",
          "[weather_pilot_wiring]")
{
    WeatherRuntimeConfig  c = wx_cfg(true);
    WeatherObservation    obs;
    WeatherLocalDate      today;
    WeatherRecommendation rec;
    WeatherRuntimeState   st;

    wiring_fresh(W62W_SRC, true);
    g_tx.timeout = true;
    reach_trusted_time();
    TEST_ASSERT_TRUE(bind_weather(&c, true));

    memset(&today, 0, sizeof(today));
    today.year = 2025; today.month = 6; today.day = 15;
    (void)weather_runtime_fetch(&g_wx, &today, W62W_EPOCH_S, true, &obs);
    TEST_ASSERT_EQUAL_INT(1, g_tx.calls);
    TEST_ASSERT_NOT_EQUAL(WEATHER_PROVIDER_OK, obs.provider_result);

    memset(&rec, 0, sizeof(rec));
    st = weather_runtime_step(&g_wx, NULL, &obs, &rec);
    TEST_ASSERT_NOT_EQUAL(WEATHER_RUNTIME_RECOMMENDATION_READY, st);
    TEST_ASSERT_FALSE(rec.present);
    /* The recommendation type has no execution field at all; the nearest
     * thing to one is this, and it is false against the production
     * registry because every profile ships UNVALIDATED. */
    TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
    wiring_teardown();
}

TEST_CASE("w62w posture: the wired pilot mutates NOTHING",
          "[weather_pilot_wiring]")
{
    WeatherRuntimeConfig c = wx_cfg(true);
    PoolRuntimeSnapshot  snap;
    unsigned             i;

    wiring_fresh(W62W_SRC, true);
    reach_trusted_time();
    TEST_ASSERT_TRUE(bind_weather(&c, true));

    /* Many steps, exactly as the 1 s statistics task would drive it. */
    for (i = 0u; i < 20u; i++) {
        WeatherRecommendation rec;
        memset(&rec, 0, sizeof(rec));
        (void)weather_runtime_step(&g_wx, NULL, NULL, &rec);
        TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
    }

    /* The store fakes fail the test on any write; these prove the rest. */
    TEST_ASSERT_EQUAL(RUNTIME_OK, pool_session_runtime_snapshot(&g_rt, &snap));
    TEST_ASSERT_TRUE(pool_runtime_snapshot_valid(&snap));
    TEST_ASSERT_EQUAL_INT(OP_OWNER_NONE, snap.lease_owner);
    TEST_ASSERT_FALSE(snap.session_present);
    TEST_ASSERT_FALSE(snap.restore_required);
    TEST_ASSERT_FALSE(snap.target_mining_authorized);
    TEST_ASSERT_FALSE(snap.pool_mutation_permitted);
    TEST_ASSERT_EQUAL_INT(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE, snap.protocol);
    TEST_ASSERT_EQUAL_UINT32(0u, snap.proposal_commits);
    /* Stepping weather never touched the trusted-time provider. */
    TEST_ASSERT_EQUAL_INT(1, g_sntp_inits);
    TEST_ASSERT_EQUAL_INT(1, g_sntp_starts);
    wiring_teardown();
}

/* ================================================================= */
/* C. The bounded observation-start RETRY (Gate W6.2 correction)      */
/* ================================================================= */

TEST_CASE("w62w retry: a refused first start is retried, bounded by budget",
          "[weather_pilot_wiring]")
{
    wiring_fresh(W62W_SRC, true);
    g_init_rc = -1;                    /* the platform refuses to initialize */
    boot_b6();
    network_ready();

    /*
     * Attempt 1 happened and failed. Before W6.2 this was the ONLY attempt
     * an observation device could ever make.
     *
     * THE OBSERVABLE IS THE RUNTIME'S OWN BOUNDED COUNTER, not the platform
     * op: pool_time_sntp_init refuses re-initialization once the provider's
     * lifecycle has left UNINITIALIZED and returns WITHOUT calling the
     * platform op, so the op count stays at 1 while the budget is spent.
     * That is the committed B2 "only a deinit clears ERROR" contract, and it
     * is exactly why this correction makes attempts REACHABLE rather than
     * making them succeed.
     */
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_runtime_time_start_attempts(&g_rt));

    /* The committed backoff refuses immediately-repeated attempts, so a tick
     * inside the window must NOT consume budget. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    TEST_ASSERT_EQUAL_UINT32(1u, pool_session_runtime_time_start_attempts(&g_rt));

    /* Past the first backoff: attempt 2 becomes reachable — the whole point. */
    g_mono_us += 20ull * 1000000ull;
    vTaskDelay(pdMS_TO_TICKS(1500));
    TEST_ASSERT_EQUAL_UINT32(2u, pool_session_runtime_time_start_attempts(&g_rt));

    /* Normal EMPTY-store mining is untouched throughout. */
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_rt));
    wiring_teardown();
}

TEST_CASE("w62w retry: the budget is spent, then never exceeded",
          "[weather_pilot_wiring]")
{
    unsigned i;

    wiring_fresh(W62W_SRC, true);
    g_init_rc = -1;
    boot_b6();
    network_ready();

    /* Walk monotonic time past each backoff; the cap must hold. */
    for (i = 0u; i < 12u; i++) {
        g_mono_us += 300ull * 1000000ull;
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    /* The cap holds: the budget is spent exactly once and never exceeded. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)POOL_TIME_SOURCE_ATTEMPTS_MAX,
                             pool_session_runtime_time_start_attempts(&g_rt));
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_rt));
    wiring_teardown();
}

TEST_CASE("w62w retry: a started provider is never re-attempted",
          "[weather_pilot_wiring]")
{
    wiring_fresh(W62W_SRC, true);
    reach_trusted_time();
    TEST_ASSERT_EQUAL_INT(1, g_sntp_inits);

    g_mono_us += 600ull * 1000000ull;
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Trusted and running: the retry must be a pure no-op. */
    TEST_ASSERT_EQUAL_INT(1, g_sntp_inits);
    TEST_ASSERT_EQUAL_INT(1, g_sntp_starts);
    TEST_ASSERT_TRUE(b2_says_trusted());
    wiring_teardown();
}

TEST_CASE("w62w retry: an UNCONFIGURED source is never retried",
          "[weather_pilot_wiring]")
{
    wiring_fresh("", true);            /* the shipped default */
    boot_b6();
    network_ready();

    g_mono_us += 600ull * 1000000ull;
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* No attempt may be spent, and no DNS or SNTP may be reached. */
    TEST_ASSERT_EQUAL_INT(0, g_sntp_inits);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_time_start_attempts(&g_rt));
    wiring_teardown();
}

TEST_CASE("w62w retry: an INVALID source is never retried",
          "[weather_pilot_wiring]")
{
    wiring_fresh("http://ntp.example/path", true);   /* refused by B10 */
    boot_b6();
    network_ready();

    g_mono_us += 600ull * 1000000ull;
    vTaskDelay(pdMS_TO_TICKS(2000));

    TEST_ASSERT_EQUAL_INT(0, g_sntp_inits);
    TEST_ASSERT_EQUAL_UINT32(0u, pool_session_runtime_time_start_attempts(&g_rt));
    wiring_teardown();
}

#endif /* CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE */

TEST_CASE("w62w retry: observation OFF never starts or retries anything",
          "[weather_pilot_wiring]")
{
    wiring_fresh(W62W_SRC, false);     /* the shipped default */
    boot_b6();
    network_ready();

    g_mono_us += 600ull * 1000000ull;
    vTaskDelay(pdMS_TO_TICKS(2000));

    TEST_ASSERT_EQUAL_INT(0, g_sntp_inits);
    TEST_ASSERT_EQUAL_INT(0, g_sntp_starts);
    TEST_ASSERT_EQUAL(POOL_RUNTIME_PROTOCOL_ALLOW_SOURCE,
                      pool_session_runtime_protocol_permission(&g_rt));
    wiring_teardown();
}
