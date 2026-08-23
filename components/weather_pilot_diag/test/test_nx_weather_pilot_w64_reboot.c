/*
 * Gate W6.4 — END-TO-END PRODUCTION WIRING, ACROSS A SIMULATED REBOOT.
 *
 * WHAT THIS PROVES, AND WHY IT IS NOT THE UNIT TESTS AGAIN. The decision
 * module and the persistence owner already have their own suites, including a
 * crash-injection matrix. Those prove the pieces. This file proves the CHAIN:
 * that the deduplication actually sits in the production path, in the right
 * place, and that a reboot really does suppress a window that a real
 * configuration, a real schedule and a real claim had already served.
 *
 * The whole chain runs, in production order:
 *
 *   explicit authorization (the W6.3.2 field)
 *     -> W5 source policy -> W4 runtime projection
 *     -> synthetic trusted time -> committed Brussels schedule -> DUE
 *     -> W6.4 authorization gate -> DURABLE CLAIM on fake flash
 *     -> exactly ONE W6.3 async submission
 *     -> fake W3 transport -> committed parser -> WeatherObservation
 *     -> telemetry publish -> ONE coherent snapshot read
 *     -> W6.3.1 input projection -> tuning_policy_evaluate()
 *     -> actionable = false, executed = false
 *
 *   then REBOOT (committed bytes survive, staged bytes and ALL RAM are lost)
 *     -> the same day, the same slot, still DUE
 *     -> ZERO submissions, ZERO fetches
 *
 *   then a LATER slot on the same day
 *     -> exactly ONE new claim and ONE new submission
 *
 * THE GATE IS NOT RE-IMPLEMENTED HERE. This file calls the same
 * nx_weather_window_authorize() the pilot adapter calls, which is why the
 * ordering it proves is the ordering that ships. Only the submission that
 * follows a true return is spelled out, mirroring the adapter.
 *
 * WHAT IS FAKE, AND ONLY WHAT IS FAKE: the trusted-time clock (a synthetic
 * anchor), the W3 transport (a canned body) and the W2 backend (an in-memory
 * model of the committed contract). The source policy, its projection, the
 * Brussels schedule, the record codec, the dual-slot store, the decision, the
 * claim, the I/O state machine, the telemetry store, the W1 projection, the
 * W1 registry and the W1 policy are all committed production code.
 *
 * Nothing here contacts a network, DNS, an NTP server, a weather provider, a
 * pool, OTA, physical NVS or hardware. "Reboot" is modelled the only honest
 * way: committed bytes survive, staged bytes are discarded, RAM is rebuilt.
 */

#include <string.h>

#include "unity.h"

#include "nx_weather_source.h"
#include "weather_runtime.h"
#include "weather_transport.h"
#include "weather_open_meteo.h"
#include "nx_weather_io.h"
#include "nx_weather_pilot_diag.h"
#include "nx_weather_window.h"
#include "nx_weather_window_runtime.h"
#include "nx_telemetry_safety.h"
#include "nx_tuning_input.h"
#include "nx_mutation_counters.h"
#include "tuning_store.h"
#include "tuning_record.h"
#include "tuning_profile.h"
#include "tuning_policy.h"
#include "local_schedule.h"
#include "brussels_time.h"

#define US_PER_S 1000000ull

/*
 * 2026-07-15 in Europe/Brussels (CEST, UTC+2). Committed default slots are
 * 05:00, 11:00 and 15:00 local; catch-up disabled; on-time window 600 s.
 *   SLOT 1 DUE = 11:00 local = 09:00 UTC
 *   SLOT 2 DUE = 15:00 local = 13:00 UTC
 */
#define W64_MIDNIGHT_UTC 1784073600ull        /* 2026-07-15T00:00:00Z */
#define W64_EPOCH_SLOT1  (W64_MIDNIGHT_UTC + 9ull * 3600ull)
#define W64_EPOCH_SLOT2  (W64_MIDNIGHT_UTC + 13ull * 3600ull)

/* Synthetic, non-private fixture coordinates. */
#define W64_LAT_E4 508500
#define W64_LON_E4  43500

#define W64_ASIC_DC 552
#define W64_VRM_DC  610
#define W64_RPM    3200u

/* ================================================================== */
/* Fake W2 backend — an in-memory model of the committed contract      */
/* ================================================================== */

#define FKE_KEYS 3

typedef struct {
    bool    present;
    size_t  len;
    uint8_t bytes[TUNING_RECORD_MAX_ENCODED];
} FkeVal;

static struct {
    FkeVal committed[FKE_KEYS];   /* survives a simulated power loss */
    FkeVal staged[FKE_KEYS];      /* lost until commit()             */
    int    writes;
    int    commits;
} g_fke;

static int fke_index(const char *key)
{
    if (strcmp(key, TUNING_STORE_KEY_SLOT_A) == 0) { return 0; }
    if (strcmp(key, TUNING_STORE_KEY_SLOT_B) == 0) { return 1; }
    if (strcmp(key, TUNING_STORE_KEY_ACTIVE) == 0) { return 2; }
    return -1;
}

static int fke_open(void *ctx) { (void)ctx; return TUNING_STORE_BACKEND_OK; }
static int fke_close(void *ctx) { (void)ctx; return TUNING_STORE_BACKEND_OK; }

static int fke_read(void *ctx, const char *key, uint8_t *buf, size_t cap,
                    size_t *out_len)
{
    int i = fke_index(key);
    const FkeVal *v;

    (void)ctx;
    if (i < 0) { return TUNING_STORE_BACKEND_NOT_FOUND; }
    v = g_fke.staged[i].present ? &g_fke.staged[i] : &g_fke.committed[i];
    if (!v->present) { return TUNING_STORE_BACKEND_NOT_FOUND; }
    *out_len = v->len;
    if (v->len <= cap) { memcpy(buf, v->bytes, v->len); }
    return TUNING_STORE_BACKEND_OK;
}

static int fke_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    int i = fke_index(key);

    (void)ctx;
    if (i < 0 || len > TUNING_RECORD_MAX_ENCODED) {
        return TUNING_STORE_BACKEND_IO;
    }
    g_fke.writes++;
    g_fke.staged[i].present = true;
    g_fke.staged[i].len = len;
    memcpy(g_fke.staged[i].bytes, buf, len);
    return TUNING_STORE_BACKEND_OK;
}

static int fke_commit(void *ctx)
{
    int i;

    (void)ctx;
    g_fke.commits++;
    for (i = 0; i < FKE_KEYS; i++) {
        if (g_fke.staged[i].present) {
            g_fke.committed[i] = g_fke.staged[i];
            g_fke.staged[i].present = false;
        }
    }
    return TUNING_STORE_BACKEND_OK;
}

static const TuningStoreBackendOps FKE_OPS = {
    .open = fke_open, .read_blob = fke_read, .write_blob = fke_write,
    .commit = fke_commit, .close = fke_close,
};

/* ================================================================== */
/* The FAKE PERSISTENCE OWNER (Gate W6.4.1)                            */
/* ================================================================== */

/*
 * In production the wake lands on nvs_task — the project's only flash writer
 * with an internal-RAM stack — and that task runs the transaction. Here the
 * wake is counted and the test runs the executor explicitly, which is what
 * lets the chain be observed one tick at a time.
 */
static int g_wakes;

static bool fke_wake(void *ctx)
{
    (void)ctx;
    g_wakes++;
    return true;
}

static const NxWeatherWindowExecutorOps FKE_EXEC = { .wake = fke_wake };

/* Run the persistence owner exactly once, as nvs_task would on a wake. */
static void owner_run(void)
{
    nx_weather_window_runtime_execute();
}

/* ================================================================== */
/* Fake trusted-time clock — the ONLY time authority in this file      */
/* ================================================================== */

typedef struct {
    uint64_t       monotonic_us;
    PoolTimeAnchor anchor;
    bool           initialized;
} RbClock;

static RbClock             g_clk;
static PoolTimeClock       g_clock;
static PoolTimeTrustPolicy g_policy;

static uint64_t rb_monotonic(void *ctx) { return ((RbClock *)ctx)->monotonic_us; }

static bool rb_anchor(void *ctx, PoolTimeAnchor *out)
{
    RbClock *c = (RbClock *)ctx;

    if (!c->initialized) { return false; }
    *out = c->anchor;
    return true;
}

static const PoolTimeClockOps RB_OPS = { .monotonic_us = rb_monotonic,
                                         .read_anchor  = rb_anchor };

static void clock_trusted_at(uint64_t epoch_s)
{
    memset(&g_clk, 0, sizeof(g_clk));
    g_clk.initialized                     = true;
    g_clk.monotonic_us                    = 10ull * US_PER_S;
    g_clk.anchor.valid                    = true;
    g_clk.anchor.sync_completed_this_boot = true;
    g_clk.anchor.generation               = 3u;
    g_clk.anchor.epoch_us_at_sync         = epoch_s * US_PER_S;
    g_clk.anchor.monotonic_us_at_sync     = 10ull * US_PER_S;
    g_clk.anchor.sync_status              = POOL_TIME_SYNC_STATUS_COMPLETED;
    g_clk.anchor.last_error               = TIME_OK;
    g_clock.ops = &RB_OPS;
    g_clock.ctx = &g_clk;
    pool_time_trust_policy_defaults(&g_policy);
}

/* ================================================================== */
/* Fake W3 transport — counts calls; NEVER performs I/O                */
/* ================================================================== */

typedef struct { int calls; } RbTx;

static RbTx g_tx;

#define DEG_C "\xC2\xB0" "C"
#define W64_BODY \
    "{\"utc_offset_seconds\":7200,\"timezone\":\"Europe/Brussels\"," \
    "\"timezone_abbreviation\":\"CEST\"," \
    "\"current_units\":{\"temperature_2m\":\"" DEG_C "\"}," \
    "\"current\":{\"temperature_2m\":22.0}," \
    "\"daily_units\":{\"temperature_2m_max\":\"" DEG_C "\"}," \
    "\"daily\":{\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[24.5]}}"

static WeatherProviderResult rb_fetch(void *ctx, const WeatherRequest *req,
                                      WeatherHttpResponse *out)
{
    RbTx  *t = (RbTx *)ctx;
    size_t n;

    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_NOT_NULL(out);
    t->calls++;
    memset(out, 0, sizeof(*out));
    n = strlen(W64_BODY);
    TEST_ASSERT_TRUE(n < sizeof(out->body));
    memcpy(out->body, W64_BODY, n);
    out->body_len             = n;
    out->http_status          = 200;
    out->content_type_present = true;
    out->content_type_json    = true;
    out->complete             = true;
    return WEATHER_PROVIDER_OK;
}

static const WeatherTransportOps RB_TX_OPS = { .fetch = rb_fetch };

/* The ~4 KB staging buffer weather_fetch_execute() needs, off the task stack. */
static WeatherHttpResponse g_scratch;

/* ================================================================== */
/* Fixtures                                                            */
/* ================================================================== */

static WeatherRuntime g_wx;
static NxWeatherIo    g_io;

/*
 * A fully configured, VALID, AUTHORIZED source — `schedule.enabled = true` is
 * exactly the one field CONFIG_NX_WEATHER_PILOT_SCHEDULE sets in the committed
 * W5 build binder. Everything else is the shipped default.
 */
static NxWeatherSourceConfig source_cfg(void)
{
    NxWeatherSourceConfig c;

    nx_weather_source_defaults(&c);
    c.enabled             = true;
    c.distribution        = NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL;
    c.provider            = WEATHER_PROVIDER_OPEN_METEO;
    c.latitude_e4         = W64_LAT_E4;
    c.longitude_e4        = W64_LON_E4;
    c.timezone            = WEATHER_TZ_EUROPE_BRUSSELS;
    c.recommendation_only = true;
    c.schedule.enabled    = true;
    return c;
}

/* Bind through the REAL W5 projection, exactly as wx_bind_runtime() does. */
static NxWeatherSourceStatus bind_runtime(const NxWeatherSourceConfig *cfg)
{
    WeatherRuntimeConfig  rcfg;
    WeatherRuntimeDeps    deps;
    NxWeatherSourceStatus status;

    status = nx_weather_source_to_runtime(cfg, &rcfg);
    if (!nx_weather_source_ready(status)) { return status; }
    memset(&deps, 0, sizeof(deps));
    deps.clock         = &g_clock;
    deps.time_policy   = &g_policy;
    /* The runtime transport seam stays NULL in production: the transport
     * belongs to the W6.3 worker side alone. */
    deps.transport     = NULL;
    deps.transport_ctx = NULL;
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_OK,
                      weather_runtime_init(&g_wx, &rcfg, &deps));
    return status;
}

static void publish_healthy_telemetry(void)
{
    NxTelemetryPowerFacts p;
    NxTelemetryFanFacts   f;

    memset(&p, 0, sizeof(p));
    p.asic_temp_dc = W64_ASIC_DC; p.asic_temp_valid = true;
    p.vrm_temp_dc  = W64_VRM_DC;  p.vrm_read_ok     = true;
    p.vrm_expected = true;
    nx_telemetry_safety_publish_power(&p);

    memset(&f, 0, sizeof(f));
    f.fan_rpm = W64_RPM; f.fan_expected = true;
    nx_telemetry_safety_publish_fan(&f);
}

/*
 * A COLD BOOT: every scrap of RAM is rebuilt from nothing, the committed flash
 * bytes are left exactly as they were, and the persistence authority reloads
 * from them. Called once at the start of each boot in every test below.
 */
static void boot(uint64_t epoch_s)
{
    int i;

    /* Whatever was staged but never committed is gone, as after power loss. */
    for (i = 0; i < FKE_KEYS; i++) { g_fke.staged[i].present = false; }

    memset(&g_wx, 0, sizeof(g_wx));
    memset(&g_tx, 0, sizeof(g_tx));
    memset(&g_scratch, 0, sizeof(g_scratch));
    nx_weather_io_init(&g_io);
    nx_telemetry_safety_reset();
    nx_mutation_counters_reset_for_test();
    nx_weather_window_runtime_reset_for_test();
    clock_trusted_at(epoch_s);

    /* The production startup ordering: persistence comes up BEFORE anything
     * can reach the submit gate. */
    g_wakes = 0;
    nx_weather_window_runtime_bind_executor(&FKE_EXEC, NULL);
    /* The load is REQUESTED here and PERFORMED by the owner — never by this
     * (statistics-equivalent) side. */
    (void)nx_weather_window_runtime_begin(&FKE_OPS, &g_fke);
    owner_run();
    TEST_ASSERT_TRUE(nx_weather_window_runtime_begin(&FKE_OPS, &g_fke));
}

/* A power cut at the very first boot: no flash content at all. */
static void erase_fake_flash(void)
{
    memset(&g_fke, 0, sizeof(g_fke));
}

/*
 * ONE production observation tick, in the adapter's order. Returns true when
 * an outbound submission was actually accepted.
 *
 * Everything below the authorization gate mirrors wx_io_submit_if_due(); the
 * gate itself is the SHIPPING function, not a copy of it.
 */
static bool observe_tick(const NxWeatherSourceConfig *cfg, uint64_t epoch_s)
{
    WeatherSchedulePlan  plan;
    NxWeatherIoRequest   req;
    BrusselsLocalTime    local;
    WeatherRecommendation rec;
    NxWeatherWindowGrant grant;

    /* 1 — step the runtime for real; it evaluates the committed schedule. */
    memset(&rec, 0, sizeof(rec));
    (void)weather_runtime_step(&g_wx, NULL, NULL, &rec);

    if (!weather_runtime_last_plan(&g_wx, &plan)) { return false; }
    if (plan.decision != WEATHER_SCHEDULE_DUE &&
        plan.decision != WEATHER_SCHEDULE_CATCH_UP_DUE) {
        return false;
    }
    if (plan.slot_index < 0) { return false; }
    TEST_ASSERT_TRUE(brussels_local_from_utc(epoch_s, &local));

    /*
     * 2 — THE GATE. The shipping implementation, called the shipping way.
     *
     * WAIT means the durable transaction is in flight on the persistence
     * owner. This test drives the owner immediately and re-asks, which is the
     * same sequence the real device performs across two ~1 s ticks.
     */
    {
        NxWeatherWindowGate v =
            nx_weather_window_authorize(&plan, g_wx.cfg.schedule.slot_count,
                                        &grant);
        if (v == NX_WX_GATE_WAIT) {
            owner_run();
            v = nx_weather_window_authorize(&plan, g_wx.cfg.schedule.slot_count,
                                            &grant);
        }
        if (v != NX_WX_GATE_SUBMIT) {
            return false;
        }
    }

    /* 3 — and only now may exactly one request be composed and submitted. */
    memset(&req, 0, sizeof(req));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&g_wx.cfg.location,
                                                       &req.request));
    req.generation                = 1u;
    /* From the GRANT, never re-derived: the flash record names the window. */
    req.window.date               = grant.date;
    req.window.slot_index         = grant.slot_index;
    req.window.provider           = cfg->provider;
    req.parse.expected_date       = grant.date;
    req.parse.fetch_epoch_s       = epoch_s;
    req.parse.fetch_epoch_trusted = true;
    req.parse.source_generation   = req.generation;

    return nx_weather_io_submit(&g_io, &req, 1000ull) == WX_IO_SUBMIT_ACCEPTED;
}

/* The worker side and the consumer side, exactly as the committed worker
 * performs them. */
static void run_worker_and_consume(WeatherObservation *out_obs)
{
    NxWeatherIoRequest    claimed;
    NxWeatherIoResult     got;
    WeatherObservation    obs;
    WeatherProviderResult pr;

    memset(&claimed, 0, sizeof(claimed));
    TEST_ASSERT_TRUE(nx_weather_io_claim(&g_io, &claimed, 1000ull));

    memset(&obs, 0, sizeof(obs));
    pr = weather_fetch_execute(&RB_TX_OPS, &g_tx, &claimed.request,
                               &claimed.parse, NULL, &g_scratch, &obs);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, pr);
    TEST_ASSERT_TRUE(nx_weather_io_publish(&g_io, claimed.generation, pr, &obs,
                                           2000ull));
    memset(&got, 0, sizeof(got));
    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, NULL, &got, 3000ull));
    *out_obs = got.observation;
}

/* The ONE snapshot read, projected exactly as nx_weather_pilot_observe() does. */
static NxTuningInputFact project_once(TuningPolicyEnvironment *env,
                                      TuningPolicyInput *in)
{
    NxTelemetrySafetySnapshot snap;
    NxTuningStructuralFacts   facts;
    const TuningProfile      *reg;
    size_t                    n = 0;

    nx_tuning_structural_facts_gamma601(&facts);
    reg = tuning_registry_gamma601(&n);
    TEST_ASSERT_TRUE(nx_telemetry_safety_read(&snap));
    return nx_tuning_input_project(&snap, &facts, reg, n, env, in, NULL);
}

static void assert_no_mutation(const NxMutationSnapshot *before)
{
    NxMutationSnapshot after;
    NxMutationDelta    d;

    TEST_ASSERT_TRUE(nx_mutation_counters_snapshot(&after, sizeof(after)));
    TEST_ASSERT_TRUE(nx_mutation_delta_compute(before, &after, &d));
    TEST_ASSERT_TRUE_MESSAGE(nx_mutation_delta_clean(&d), "something mutated");
    TEST_ASSERT_EQUAL_UINT32(0u, d.hardware_total);
    TEST_ASSERT_EQUAL_UINT32(0u, d.pool_total);
    TEST_ASSERT_EQUAL_UINT32(0u, d.protocol_total);
    TEST_ASSERT_EQUAL_UINT32(0u, d.restart_total);
    TEST_ASSERT_EQUAL_UINT32(0u, d.ota_total);
}

/*
 * Drive the whole downstream chain for one accepted submission and assert the
 * pilot's terminal promise: a recommendation may be produced, and NOTHING is
 * applied.
 */
static void consume_and_assert_recommendation_only(void)
{
    WeatherObservation      obs;
    TuningPolicyEnvironment env;
    TuningPolicyInput       in;
    WeatherRuntimeStepEnv   step_env;
    WeatherRecommendation   rec;
    WeatherRuntimeState     st;

    run_worker_and_consume(&obs);
    publish_healthy_telemetry();
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project_once(&env, &in));

    memset(&step_env, 0, sizeof(step_env));
    step_env.policy_env = &env;
    step_env.policy_in  = &in;

    memset(&rec, 0, sizeof(rec));
    st = weather_runtime_step(&g_wx, &step_env, &obs, &rec);

    /* The W1 evaluator really ran, on the projected input. */
    TEST_ASSERT_EQUAL(WEATHER_RUNTIME_RECOMMENDATION_READY, st);
    TEST_ASSERT_TRUE(rec.present);
    TEST_ASSERT_TRUE(rec.trusted_time_at_evaluation);

    /*
     * THE PILOT PROMISE: recommend, never act.
     *
     * "executed = false" is not a boolean on this struct — the runtime states
     * it as a REASON that is never absent, which is the stronger form. Every
     * production profile is UNVALIDATED, so the evaluator selects none and the
     * reason is NO_PROFILE_SELECTED. Asserting the reason rather than a bare
     * false also pins WHY nothing ran, so a future change that starts
     * executing cannot pass this by flipping a flag.
     */
    TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
    TEST_ASSERT_EQUAL(WEATHER_NOT_EXECUTED_NO_PROFILE_SELECTED, rec.not_executed);
    TEST_ASSERT_NOT_EQUAL(TUNING_SELECT_PROFILE, rec.intent.action);
    TEST_ASSERT_EQUAL_STRING("", rec.intent.profile_id);
}

/* The durably claimed state, read back from the persistence authority. */
static TuningScheduleWindowState durable_state(void)
{
    TuningScheduleWindowState w;

    memset(&w, 0, sizeof(w));
    (void)nx_weather_window_runtime_state(&w);
    return w;
}

/* ================================================================== */
/* THE GATE'S REASON FOR EXISTING                                      */
/* ================================================================== */

TEST_CASE("w64 e2e: a served window is not served again after a reboot",
          "[weather_pilot_w64]")
{
    NxWeatherSourceConfig     cfg = source_cfg();
    NxMutationSnapshot        before;
    TuningScheduleWindowState st;
    int                       writes_after_first;

    erase_fake_flash();

    /* ---------------- BOOT 1: the window is served exactly once --------- */
    boot(W64_EPOCH_SLOT1);
    TEST_ASSERT_TRUE(nx_mutation_counters_snapshot(&before, sizeof(before)));
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind_runtime(&cfg));

    /* A virgin store is a legitimate starting point, not corruption. */
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_READY_EMPTY, nx_weather_window_runtime_fact());

    TEST_ASSERT_TRUE(observe_tick(&cfg, W64_EPOCH_SLOT1));
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.submit_count);

    /* The claim is on flash BEFORE the fetch was ever executed. */
    st = durable_state();
    TEST_ASSERT_TRUE(st.present);
    TEST_ASSERT_EQUAL_UINT16(2026u, st.year);
    TEST_ASSERT_EQUAL_UINT8(7u, st.month);
    TEST_ASSERT_EQUAL_UINT8(15u, st.day);
    TEST_ASSERT_EQUAL_UINT8(0x02u, st.served_mask);   /* slot index 1 */
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);

    consume_and_assert_recommendation_only();
    TEST_ASSERT_EQUAL_INT(1, g_tx.calls);
    writes_after_first = g_fke.writes;

    /* Further ticks in the SAME boot must not re-serve it either. */
    TEST_ASSERT_FALSE(observe_tick(&cfg, W64_EPOCH_SLOT1));
    TEST_ASSERT_FALSE(observe_tick(&cfg, W64_EPOCH_SLOT1));
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.submit_count);
    TEST_ASSERT_EQUAL_INT(writes_after_first, g_fke.writes);

    /* ---------------- BOOT 2: the reboot the gate exists for ------------ */
    boot(W64_EPOCH_SLOT1);
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind_runtime(&cfg));

    /* The claim survived, and it was RECOVERED rather than re-created. */
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_READY_RECOVERED,
                      nx_weather_window_runtime_fact());
    st = durable_state();
    TEST_ASSERT_TRUE(st.present);
    TEST_ASSERT_EQUAL_UINT8(0x02u, st.served_mask);

    /* The schedule still says DUE — this is not "the window closed". */
    {
        WeatherSchedulePlan  plan;
        WeatherRecommendation rec;

        memset(&rec, 0, sizeof(rec));
        (void)weather_runtime_step(&g_wx, NULL, NULL, &rec);
        TEST_ASSERT_TRUE(weather_runtime_last_plan(&g_wx, &plan));
        TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DUE, plan.decision);
        TEST_ASSERT_EQUAL_INT(1, plan.slot_index);
    }

    /* THE ASSERTION THE WHOLE GATE EXISTS FOR. */
    TEST_ASSERT_FALSE(observe_tick(&cfg, W64_EPOCH_SLOT1));
    TEST_ASSERT_EQUAL_UINT32(0u, g_io.submit_count);
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);
    TEST_ASSERT_EQUAL_INT(writes_after_first, g_fke.writes);  /* and no write */

    /* ---------------- BOOT 2 CONTINUED: a LATER slot still works -------- */
    clock_trusted_at(W64_EPOCH_SLOT2);
    memset(&g_wx, 0, sizeof(g_wx));
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind_runtime(&cfg));

    TEST_ASSERT_TRUE(observe_tick(&cfg, W64_EPOCH_SLOT2));
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.submit_count);

    /* The new claim ACCUMULATED onto the same service day. */
    st = durable_state();
    TEST_ASSERT_TRUE(st.present);
    TEST_ASSERT_EQUAL_UINT8(15u, st.day);
    TEST_ASSERT_EQUAL_UINT8(0x06u, st.served_mask);   /* slots 1 and 2 */
    TEST_ASSERT_TRUE(g_fke.writes > writes_after_first);

    consume_and_assert_recommendation_only();
    TEST_ASSERT_EQUAL_INT(1, g_tx.calls);

    /* Across all three boots and every tick: nothing was applied anywhere. */
    assert_no_mutation(&before);
}

/* ================================================================== */
/* A crash BETWEEN the claim and the request                           */
/* ================================================================== */

TEST_CASE("w64 e2e: a crash after claiming and before fetching loses the "
          "window rather than repeating it",
          "[weather_pilot_w64]")
{
    NxWeatherSourceConfig cfg = source_cfg();

    erase_fake_flash();

    /*
     * The deliberate AT-MOST-ONCE cost, stated as a test so it can never be
     * mistaken for a bug later. The claim commits, then power is lost before
     * the transport ever runs. The recommendation for that window is gone for
     * good — and that is the correct outcome, because the alternative is an
     * authorized window that can be served twice.
     */
    boot(W64_EPOCH_SLOT1);
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind_runtime(&cfg));
    TEST_ASSERT_TRUE(observe_tick(&cfg, W64_EPOCH_SLOT1));
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.submit_count);
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);   /* the fetch never happened */

    boot(W64_EPOCH_SLOT1);
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind_runtime(&cfg));
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_READY_RECOVERED,
                      nx_weather_window_runtime_fact());

    TEST_ASSERT_FALSE(observe_tick(&cfg, W64_EPOCH_SLOT1));
    TEST_ASSERT_EQUAL_UINT32(0u, g_io.submit_count);
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);
}

/* ================================================================== */
/* An unserviceable store must stop the chain, not fall back to RAM    */
/* ================================================================== */

TEST_CASE("w64 e2e: an unavailable persistence authority submits nothing",
          "[weather_pilot_w64]")
{
    NxWeatherSourceConfig cfg = source_cfg();
    WeatherSchedulePlan   plan;
    WeatherRecommendation rec;

    erase_fake_flash();

    /* Serve one window so there is real committed content to corrupt. */
    boot(W64_EPOCH_SLOT1);
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind_runtime(&cfg));
    TEST_ASSERT_TRUE(observe_tick(&cfg, W64_EPOCH_SLOT1));

    /* Corrupt BOTH record slots on the fake flash. The active pointer still
     * says a record exists, so this is "unreadable", not "absent". */
    g_fke.committed[0].bytes[0] ^= 0xFFu;
    g_fke.committed[1].bytes[0] ^= 0xFFu;

    /* Next boot: the authority refuses, and refuses loudly rather than
     * silently starting over with an empty window state. */
    {
        int i;
        for (i = 0; i < FKE_KEYS; i++) { g_fke.staged[i].present = false; }
        memset(&g_wx, 0, sizeof(g_wx));
        memset(&g_tx, 0, sizeof(g_tx));
        nx_weather_io_init(&g_io);
        nx_weather_window_runtime_reset_for_test();
        clock_trusted_at(W64_EPOCH_SLOT2);
        g_wakes = 0;
        nx_weather_window_runtime_bind_executor(&FKE_EXEC, NULL);
        (void)nx_weather_window_runtime_begin(&FKE_OPS, &g_fke);
        owner_run();
        TEST_ASSERT_FALSE(nx_weather_window_runtime_begin(&FKE_OPS, &g_fke));
    }
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_UNAVAILABLE, nx_weather_window_runtime_fact());
    TEST_ASSERT_FALSE(nx_weather_window_runtime_ready());

    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind_runtime(&cfg));

    /* The schedule is genuinely DUE at this instant — so the refusal below is
     * the persistence authority's, not the scheduler's. */
    memset(&rec, 0, sizeof(rec));
    (void)weather_runtime_step(&g_wx, NULL, NULL, &rec);
    TEST_ASSERT_TRUE(weather_runtime_last_plan(&g_wx, &plan));
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DUE, plan.decision);

    TEST_ASSERT_FALSE(observe_tick(&cfg, W64_EPOCH_SLOT2));
    TEST_ASSERT_EQUAL_UINT32(0u, g_io.submit_count);
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);

    /* Nothing was erased or rewritten in an attempt to recover. */
    TEST_ASSERT_TRUE(g_fke.committed[2].present);
}

/* ================================================================== */
/* An upgraded device must not have its first window suppressed        */
/* ================================================================== */

TEST_CASE("w64 e2e: a recovered record that claims no window still serves "
          "the first window",
          "[weather_pilot_w64]")
{
    NxWeatherSourceConfig cfg = source_cfg();
    TuningStore           s;
    TuningPolicyRecord    rec;

    erase_fake_flash();

    /*
     * The upgrade hazard, at CHAIN level. A device that has a committed policy
     * record but has never claimed a window — which is exactly what a pre-W6.4
     * schema v1 record decodes to — must be treated as NEVER CLAIMED, not as
     * "already served". Getting it wrong in the suppressing direction would
     * silently disable the pilot on every upgraded device, and it would look
     * like a healthy recovered store while doing it.
     *
     * TO BE PRECISE ABOUT WHAT THIS WRITES: the store encodes with the CURRENT
     * schema, so these are v2 bytes with an absent window — the state a v1
     * record is REQUIRED to decode to. The byte-level proof that genuine v1
     * bytes decode this way (and that they are still distinguishable from
     * corruption) belongs to the record codec suite; what is proven here is
     * that the chain then behaves correctly.
     */
    memset(&rec, 0, sizeof(rec));
    tuning_record_init_state(&rec);
    memset(&rec.window, 0, sizeof(rec.window));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_init(&s, &FKE_OPS, NULL));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&s, &rec));
    tuning_store_deinit(&s);

    boot(W64_EPOCH_SLOT1);
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind_runtime(&cfg));

    /* A real record was recovered... */
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_READY_RECOVERED,
                      nx_weather_window_runtime_fact());
    /* ...and it claims no window. */
    TEST_ASSERT_FALSE(durable_state().present);

    TEST_ASSERT_TRUE(observe_tick(&cfg, W64_EPOCH_SLOT1));
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.submit_count);
    TEST_ASSERT_TRUE(durable_state().present);
}

/* ================================================================== */
/* The diagnostics projection — every posture, including the refusals  */
/* ================================================================== */

/*
 * WHY THESE EXIST. A pilot that stops submitting looks identical to a pilot
 * that is correctly suppressing an already-served window, and an unreadable
 * store produces exactly the same silence as a healthy quiet device. The line
 * is the only thing that tells those apart on a real unit, so a projection
 * that quietly reported a passing zero would be worse than no line at all.
 */

TEST_CASE("w64 line: a zeroed line can never look like a healthy store",
          "[weather_pilot_w64]")
{
    NxWeatherPilotLine line;

    memset(&line, 0, sizeof(line));

    /* The two zeros that matter are both the fail-closed tokens. */
    TEST_ASSERT_EQUAL(NX_WX_FACT_ABSENT, line.window_fact);
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_NOT_LOADED,
                      (NxWeatherWindowStoreFact)line.window_store_fact);
    TEST_ASSERT_FALSE(nx_weather_window_store_serviceable(
                          (NxWeatherWindowStoreFact)line.window_store_fact));
    TEST_ASSERT_FALSE(line.window_ready);
    TEST_ASSERT_FALSE(line.window_dedup_enabled);
}

TEST_CASE("w64 line: an image without the capability reports STRUCTURAL",
          "[weather_pilot_w64]")
{
    NxWeatherPilotLine  line;
    NxWeatherWindowDiag d;

    /* Pre-poison every field so a projection that forgets one is caught. */
    memset(&line, 0xAA, sizeof(line));
    memset(&d, 0xAA, sizeof(d));

    /* Even given an observation, `dedup_linked = false` must ignore it: the
     * absence is a property of the LINK, not a reading. */
    nx_weather_pilot_window_project(&d, false, &line);

    TEST_ASSERT_EQUAL(NX_WX_FACT_STRUCTURAL, line.window_fact);
    TEST_ASSERT_FALSE(line.window_dedup_enabled);
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_NOT_LOADED,
                      (NxWeatherWindowStoreFact)line.window_store_fact);
    TEST_ASSERT_FALSE(line.window_ready);
    TEST_ASSERT_FALSE(line.window_recovered);
    TEST_ASSERT_FALSE(line.window_day_present);
    TEST_ASSERT_EQUAL_UINT16(0u, line.window_service_year);
    TEST_ASSERT_EQUAL_UINT8(0u, line.window_service_month);
    TEST_ASSERT_EQUAL_UINT8(0u, line.window_service_day);
    TEST_ASSERT_EQUAL_UINT8(0u, line.window_served_mask);
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_UNAVAILABLE,
                      (NxWeatherWindowDecision)line.window_last_decision);
    TEST_ASSERT_EQUAL_UINT32(0u, line.window_claim_count);
    TEST_ASSERT_EQUAL_UINT32(0u, line.window_suppressed_count);
    TEST_ASSERT_EQUAL_UINT32(0u, line.window_persist_fail_count);
}

TEST_CASE("w64 line: linked but unreadable reports UNAVAILABLE, not healthy",
          "[weather_pilot_w64]")
{
    NxWeatherPilotLine line;

    memset(&line, 0xAA, sizeof(line));
    nx_weather_pilot_window_project(NULL, true, &line);

    /* The capability IS in the image — that much is true and is reported. */
    TEST_ASSERT_TRUE(line.window_dedup_enabled);
    /* But nothing was read, so nothing is claimed. */
    TEST_ASSERT_EQUAL(NX_WX_FACT_UNAVAILABLE, line.window_fact);
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_NOT_LOADED,
                      (NxWeatherWindowStoreFact)line.window_store_fact);
    TEST_ASSERT_FALSE(line.window_ready);
    TEST_ASSERT_EQUAL_UINT8(0u, line.window_served_mask);
}

TEST_CASE("w64 line: an observation is reported verbatim",
          "[weather_pilot_w64]")
{
    NxWeatherPilotLine  line;
    NxWeatherWindowDiag d;

    memset(&line, 0xAA, sizeof(line));
    memset(&d, 0, sizeof(d));
    d.fact               = (uint8_t)NX_WX_WSTORE_READY_RECOVERED;
    d.ready              = true;
    d.recovered          = true;
    d.day_present        = true;
    d.service_year       = 2026u;
    d.service_month      = 7u;
    d.service_day        = 15u;
    d.served_mask        = 0x06u;
    d.last_decision      = (uint8_t)NX_WX_WINDOW_ALREADY_CLAIMED;
    d.claim_count        = 2u;
    d.suppressed_count   = 5u;
    d.persist_fail_count = 1u;

    nx_weather_pilot_window_project(&d, true, &line);

    TEST_ASSERT_EQUAL(NX_WX_FACT_OBSERVED, line.window_fact);
    TEST_ASSERT_TRUE(line.window_dedup_enabled);
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_READY_RECOVERED,
                      (NxWeatherWindowStoreFact)line.window_store_fact);
    TEST_ASSERT_TRUE(line.window_ready);
    TEST_ASSERT_TRUE(line.window_recovered);
    TEST_ASSERT_TRUE(line.window_day_present);
    TEST_ASSERT_EQUAL_UINT16(2026u, line.window_service_year);
    TEST_ASSERT_EQUAL_UINT8(7u, line.window_service_month);
    TEST_ASSERT_EQUAL_UINT8(15u, line.window_service_day);
    TEST_ASSERT_EQUAL_UINT8(0x06u, line.window_served_mask);
    TEST_ASSERT_EQUAL(NX_WX_WINDOW_ALREADY_CLAIMED,
                      (NxWeatherWindowDecision)line.window_last_decision);
    TEST_ASSERT_EQUAL_UINT32(2u, line.window_claim_count);
    TEST_ASSERT_EQUAL_UINT32(5u, line.window_suppressed_count);
    TEST_ASSERT_EQUAL_UINT32(1u, line.window_persist_fail_count);
}

TEST_CASE("w64 line: every store fact and decision has a distinct token string",
          "[weather_pilot_w64]")
{
    int i;
    int j;

    /*
     * The line prints these tokens instead of values. A duplicated or NULL
     * token would make two different postures indistinguishable in a log —
     * including "recovered" versus "unavailable", which demand opposite
     * operator responses.
     */
    for (i = 0; i < (int)NX_WX_WSTORE__COUNT; i++) {
        const char *a = nx_weather_window_store_fact_str(
                            (NxWeatherWindowStoreFact)i);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_TRUE(a[0] != '\0');
        for (j = i + 1; j < (int)NX_WX_WSTORE__COUNT; j++) {
            TEST_ASSERT_TRUE(strcmp(a, nx_weather_window_store_fact_str(
                                           (NxWeatherWindowStoreFact)j)) != 0);
        }
    }
    for (i = 0; i < (int)NX_WX_WINDOW__COUNT; i++) {
        const char *a = nx_weather_window_decision_str(
                            (NxWeatherWindowDecision)i);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_TRUE(a[0] != '\0');
        for (j = i + 1; j < (int)NX_WX_WINDOW__COUNT; j++) {
            TEST_ASSERT_TRUE(strcmp(a, nx_weather_window_decision_str(
                                           (NxWeatherWindowDecision)j)) != 0);
        }
    }

    /* Out of range must be a token too, never a NULL the formatter would
     * dereference. */
    TEST_ASSERT_NOT_NULL(nx_weather_window_store_fact_str(
                             (NxWeatherWindowStoreFact)99));
    TEST_ASSERT_NOT_NULL(nx_weather_window_decision_str(
                             (NxWeatherWindowDecision)99));
}

TEST_CASE("w64 line: the projection carries no private-value channel",
          "[weather_pilot_w64]")
{
    /*
     * Structural privacy, checked as a property rather than asserted in a
     * comment. Every W6.4 line field is a bounded scalar or a token id, so a
     * coordinate, provider host, URL, query string, response body, credential
     * or raw epoch has nowhere to live. The widths pin that: a pointer or a
     * character array would not fit in them.
     */
    NxWeatherPilotLine line;

    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)sizeof(line.window_dedup_enabled));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)sizeof(line.window_store_fact));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)sizeof(line.window_ready));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)sizeof(line.window_recovered));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)sizeof(line.window_day_present));
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)sizeof(line.window_service_year));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)sizeof(line.window_service_month));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)sizeof(line.window_service_day));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)sizeof(line.window_served_mask));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)sizeof(line.window_last_decision));
    TEST_ASSERT_EQUAL_UINT32(4u, (uint32_t)sizeof(line.window_claim_count));
    TEST_ASSERT_EQUAL_UINT32(4u, (uint32_t)sizeof(line.window_suppressed_count));
    TEST_ASSERT_EQUAL_UINT32(4u,
                             (uint32_t)sizeof(line.window_persist_fail_count));

    /* The whole line stays small enough to state exactly. */
    TEST_ASSERT_TRUE(sizeof(line) < 512u);
}
