/*
 * Gate W6.3.2 — SCHEDULE AUTHORIZATION.
 *
 * WHAT THIS PROVES. Gate W6.3.1 wired the W1 projection into the production
 * step, but the chain could still never start: the committed W3 schedule ships
 * `enabled = false` and nothing could set it true, so every window evaluated to
 * DISABLED and no weather request could ever be composed. W6.3.2 adds exactly
 * one build-time authority for that field. These tests pin the three postures
 * that authority creates, through the REAL W5 -> W4 projection:
 *
 *   A  configured, NOT authorized, and the time IS inside a committed window
 *      -> DISABLED, zero submission, zero fetch, zero policy evaluation
 *   B  authorized, but the time is OUTSIDE every window
 *      -> zero submission, zero fetch
 *   C  authorized and inside a window
 *      -> exactly ONE async submission -> fake W3 fetch -> WeatherObservation
 *         -> ONE telemetry snapshot -> W6.3.1 projection
 *         -> tuning_policy_evaluate() -> actionable = false, executed = false
 *
 * THE DISTINCTION THE WHOLE GATE EXISTS FOR: "weather configured" is NOT
 * "weather pilot authorized". Test A is a fully valid, READY source
 * configuration at a genuinely due instant, and it still asks for nothing.
 *
 * WHAT IS FAKE, AND ONLY WHAT IS FAKE: the trusted-time clock (a synthetic
 * anchor) and the W3 transport (a canned body). The source policy, its
 * projection onto the W4 runtime, the Brussels schedule, the bridge, the I/O
 * state machine, the telemetry store, the projection, the W1 registry and the
 * W1 policy are all the committed production code.
 *
 * Every coordinate is a synthetic fixture. Nothing here contacts a network,
 * DNS, an NTP server, a weather provider, a pool, OTA, NVS or hardware.
 */

#include <string.h>

#include "unity.h"

#include "nx_weather_source.h"
#include "weather_runtime.h"
#include "weather_transport.h"
#include "weather_open_meteo.h"
#include "nx_weather_io.h"
#include "nx_telemetry_safety.h"
#include "nx_tuning_input.h"
#include "nx_mutation_counters.h"
#include "nx_weather_pilot_diag.h"
#include "tuning_profile.h"
#include "tuning_policy.h"
#include "local_schedule.h"
#include "brussels_time.h"

#define US_PER_S 1000000ull

/*
 * 2026-07-15 in Europe/Brussels (CEST, UTC+2). The committed default slots are
 * 05:00, 11:00 and 15:00 local, catch-up disabled, on-time window 600 s.
 *   DUE      = 11:00 local = 09:00 UTC, exactly on a slot boundary.
 *   NOT DUE  = 13:00 local = 11:00 UTC, two hours past the 11:00 slot, which
 *              with catch-up disabled is CATCH_UP_DISABLED, never DUE.
 */
#define W632_MIDNIGHT_UTC 1784073600ull        /* 2026-07-15T00:00:00Z */
#define W632_EPOCH_DUE    (W632_MIDNIGHT_UTC + 9ull * 3600ull)
#define W632_EPOCH_NOTDUE (W632_MIDNIGHT_UTC + 11ull * 3600ull)

/* Synthetic, non-private fixture coordinates. */
#define W632_LAT_E4 508500
#define W632_LON_E4  43500

#define W632_ASIC_DC 552
#define W632_VRM_DC  610
#define W632_RPM    3200u

/* ================================================================== */
/* Fake trusted-time clock — the ONLY time authority in this file      */
/* ================================================================== */

typedef struct {
    uint64_t       monotonic_us;
    PoolTimeAnchor anchor;
    bool           initialized;
} SchedClock;

static SchedClock          g_clk;
static PoolTimeClock       g_clock;
static PoolTimeTrustPolicy g_policy;

static uint64_t sc_monotonic(void *ctx) { return ((SchedClock *)ctx)->monotonic_us; }

static bool sc_anchor(void *ctx, PoolTimeAnchor *out)
{
    SchedClock *c = (SchedClock *)ctx;

    if (!c->initialized) {
        return false;
    }
    *out = c->anchor;
    return true;
}

static const PoolTimeClockOps SC_OPS = { .monotonic_us = sc_monotonic,
                                         .read_anchor  = sc_anchor };

/* Trusted at exactly `epoch_s`: the anchor is fresh, so `now` == epoch_s. */
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
    g_clock.ops = &SC_OPS;
    g_clock.ctx = &g_clk;
    pool_time_trust_policy_defaults(&g_policy);
}

/* No anchor at all: B2/B10 says untrusted. */
static void clock_untrusted(void)
{
    memset(&g_clk, 0, sizeof(g_clk));
    g_clk.initialized        = true;
    g_clk.anchor.valid       = false;
    g_clk.anchor.sync_status = POOL_TIME_SYNC_STATUS_PENDING;
    g_clock.ops = &SC_OPS;
    g_clock.ctx = &g_clk;
    pool_time_trust_policy_defaults(&g_policy);
}

/* ================================================================== */
/* Fake W3 transport — counts calls; NEVER performs I/O                */
/* ================================================================== */

typedef struct {
    int         calls;
    const char *body;
} SchedTx;

static SchedTx g_tx;

static WeatherProviderResult sched_fetch(void *ctx, const WeatherRequest *req,
                                         WeatherHttpResponse *out)
{
    SchedTx *t = (SchedTx *)ctx;
    size_t   n;

    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_NOT_NULL(out);
    t->calls++;
    memset(out, 0, sizeof(*out));
    n = strlen(t->body);
    TEST_ASSERT_TRUE(n < sizeof(out->body));
    memcpy(out->body, t->body, n);
    out->body_len             = n;
    out->http_status          = 200;
    out->content_type_present = true;
    out->content_type_json    = true;
    out->complete             = true;
    return WEATHER_PROVIDER_OK;
}

static const WeatherTransportOps SCHED_TX_OPS = { .fetch = sched_fetch };

/* The ~4 KB staging buffer weather_fetch_execute() needs, off the task stack. */
static WeatherHttpResponse g_scratch;

/* The canonical Open-Meteo shape the committed parser requires. */
#define DEG_C "\xC2\xB0" "C"
#define W632_BODY \
    "{\"utc_offset_seconds\":7200,\"timezone\":\"Europe/Brussels\"," \
    "\"timezone_abbreviation\":\"CEST\"," \
    "\"current_units\":{\"temperature_2m\":\"" DEG_C "\"}," \
    "\"current\":{\"temperature_2m\":22.0}," \
    "\"daily_units\":{\"temperature_2m_max\":\"" DEG_C "\"}," \
    "\"daily\":{\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[24.5]}}"

/* ================================================================== */
/* Fixtures                                                            */
/* ================================================================== */

static WeatherRuntime g_wx;
static NxWeatherIo    g_io;

/*
 * A source configuration that is FULLY CONFIGURED and VALID. `authorized`
 * selects the one field Gate W6.3.2 owns — exactly what
 * CONFIG_NX_WEATHER_PILOT_SCHEDULE flips in nx_weather_source_from_build_config().
 * Everything else is identical between the two postures, which is what makes
 * "configured is not authorized" a controlled comparison rather than a claim.
 */
static NxWeatherSourceConfig source_cfg(bool authorized)
{
    NxWeatherSourceConfig c;

    nx_weather_source_defaults(&c);
    c.enabled            = true;
    c.distribution       = NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL;
    c.provider           = WEATHER_PROVIDER_OPEN_METEO;
    c.latitude_e4        = W632_LAT_E4;
    c.longitude_e4       = W632_LON_E4;
    c.timezone           = WEATHER_TZ_EUROPE_BRUSSELS;
    c.recommendation_only = true;
    /* The committed slots/catch-up are NOT touched; only authorization moves. */
    if (authorized) {
        c.schedule.enabled = true;
    }
    return c;
}

static void fresh(uint64_t epoch_s, bool trusted)
{
    memset(&g_wx, 0, sizeof(g_wx));
    memset(&g_tx, 0, sizeof(g_tx));
    memset(&g_scratch, 0, sizeof(g_scratch));
    g_tx.body = W632_BODY;
    nx_weather_io_init(&g_io);
    nx_telemetry_safety_reset();
    nx_mutation_counters_reset_for_test();
    if (trusted) {
        clock_trusted_at(epoch_s);
    } else {
        clock_untrusted();
    }
}

/* Bind through the REAL W5 projection, exactly as wx_bind_runtime() does. */
static NxWeatherSourceStatus bind(const NxWeatherSourceConfig *cfg)
{
    WeatherRuntimeConfig  rcfg;
    WeatherRuntimeDeps    deps;
    NxWeatherSourceStatus status;

    status = nx_weather_source_to_runtime(cfg, &rcfg);
    if (!nx_weather_source_ready(status)) {
        return status;
    }
    memset(&deps, 0, sizeof(deps));
    deps.clock       = &g_clock;
    deps.time_policy = &g_policy;
    /* The runtime transport seam stays NULL in production; the transport
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
    p.asic_temp_dc = W632_ASIC_DC; p.asic_temp_valid = true;
    p.vrm_temp_dc  = W632_VRM_DC;  p.vrm_read_ok     = true;
    p.vrm_expected = true;
    nx_telemetry_safety_publish_power(&p);

    memset(&f, 0, sizeof(f));
    f.fan_rpm = W632_RPM; f.fan_expected = true;
    nx_telemetry_safety_publish_fan(&f);
}

/*
 * The adapter's submit guard, reproduced exactly: a request may be composed
 * ONLY when the plan the step already computed says a window is due. Returns
 * true when a submission was actually accepted.
 */
static bool submit_if_due(const NxWeatherSourceConfig *cfg, uint64_t epoch_s)
{
    WeatherSchedulePlan  plan;
    NxWeatherIoRequest   req;
    BrusselsLocalTime    local;
    WeatherRequestParams params;

    if (!weather_runtime_last_plan(&g_wx, &plan)) {
        return false;
    }
    if (plan.decision != WEATHER_SCHEDULE_DUE &&
        plan.decision != WEATHER_SCHEDULE_CATCH_UP_DUE) {
        return false;
    }
    if (plan.slot_index < 0) {
        return false;
    }
    TEST_ASSERT_TRUE(brussels_local_from_utc(epoch_s, &local));

    memset(&req, 0, sizeof(req));
    memset(&params, 0, sizeof(params));
    params.latitude_e4  = cfg->latitude_e4;
    params.longitude_e4 = cfg->longitude_e4;
    params.timezone     = cfg->timezone;
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&params, &req.request));
    req.generation                = 1u;
    req.window.date               = local.date;
    req.window.slot_index         = plan.slot_index;
    req.window.provider           = cfg->provider;
    req.parse.expected_date       = local.date;
    req.parse.fetch_epoch_s       = epoch_s;
    req.parse.fetch_epoch_trusted = true;
    req.parse.source_generation   = req.generation;

    return nx_weather_io_submit(&g_io, &req, 1000ull) == WX_IO_SUBMIT_ACCEPTED;
}

/* Worker side + consumer side, exactly as the committed worker performs them. */
static void run_worker_and_consume(WeatherObservation *out_obs)
{
    NxWeatherIoRequest    claimed;
    NxWeatherIoResult     got;
    WeatherObservation    obs;
    WeatherProviderResult pr;

    memset(&claimed, 0, sizeof(claimed));
    TEST_ASSERT_TRUE(nx_weather_io_claim(&g_io, &claimed, 1000ull));

    memset(&obs, 0, sizeof(obs));
    pr = weather_fetch_execute(&SCHED_TX_OPS, &g_tx, &claimed.request,
                               &claimed.parse, NULL, &g_scratch, &obs);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, pr);
    TEST_ASSERT_TRUE(nx_weather_io_publish(&g_io, claimed.generation, pr, &obs,
                                           2000ull));
    memset(&got, 0, sizeof(got));
    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, NULL, &got, 3000ull));
    *out_obs = got.observation;
}

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

/* ================================================================== */
/* The compiled default-off proof                                      */
/* ================================================================== */

TEST_CASE("w632: the build binder leaves the schedule unauthorized by default",
          "[weather_pilot_sched]")
{
    NxWeatherSourceConfig built;
    NxWeatherSourceConfig defaults;

    /*
     * THIS image is built without CONFIG_NX_WEATHER_PILOT_SCHEDULE, so the ONE
     * production assignment is compiled out and the binder must yield the
     * committed disabled schedule. The flag-ON direction is not assertable from
     * a unit test — it is proven by the pilot posture's generated sdkconfig.h
     * and its ELF audit — so this pins the direction that protects every
     * ordinary build.
     */
    nx_weather_source_from_build_config(&built);
    TEST_ASSERT_FALSE(built.schedule.enabled);

    /* And the shipped defaults it starts from say the same thing. */
    nx_weather_source_defaults(&defaults);
    TEST_ASSERT_FALSE(defaults.schedule.enabled);

    /* Authorization is the ONLY field in question: the committed window shape
     * is present either way, so enabling it later redefines nothing. */
    TEST_ASSERT_EQUAL_UINT8(3u, defaults.schedule.slot_count);
    TEST_ASSERT_EQUAL_UINT16(WEATHER_SCHEDULE_DEFAULT_SLOT_1_MIN,
                             defaults.schedule.slots_min[0]);
    TEST_ASSERT_EQUAL_UINT16(WEATHER_SCHEDULE_DEFAULT_SLOT_2_MIN,
                             defaults.schedule.slots_min[1]);
    TEST_ASSERT_EQUAL_UINT16(WEATHER_SCHEDULE_DEFAULT_SLOT_3_MIN,
                             defaults.schedule.slots_min[2]);
    TEST_ASSERT_EQUAL_UINT32(0u, defaults.schedule.catch_up_window_s);
}

TEST_CASE("w632: an unauthorized schedule is still a READY configuration",
          "[weather_pilot_sched]")
{
    NxWeatherSourceConfig cfg = source_cfg(false);
    WeatherRuntimeConfig  rcfg;

    /*
     * The load-bearing separation: the device is fully and validly CONFIGURED
     * — provider, location, timezone, recommendation-only — and W5 reports it
     * READY. Readiness says where weather would come from. It does not say this
     * device may ask, and the projected runtime carries the refusal.
     */
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                      nx_weather_source_to_runtime(&cfg, &rcfg));
    TEST_ASSERT_TRUE(rcfg.enabled);
    TEST_ASSERT_FALSE(rcfg.schedule.enabled);

    /* Authorizing changes that one field and nothing else. */
    cfg = source_cfg(true);
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                      nx_weather_source_to_runtime(&cfg, &rcfg));
    TEST_ASSERT_TRUE(rcfg.schedule.enabled);
    TEST_ASSERT_EQUAL_UINT8(3u, rcfg.schedule.slot_count);
}

/* ================================================================== */
/* TEST A — authorization OFF, at a genuinely due instant              */
/* ================================================================== */

TEST_CASE("w632 A: unauthorized at a due instant submits and fetches nothing",
          "[weather_pilot_sched]")
{
    NxWeatherSourceConfig cfg = source_cfg(false);
    WeatherSchedulePlan   plan;
    WeatherRecommendation rec;
    WeatherRuntimeState   st;
    NxMutationSnapshot    before;
    TuningPolicyEnvironment env;
    TuningPolicyInput       in;

    fresh(W632_EPOCH_DUE, true);
    TEST_ASSERT_TRUE(nx_mutation_counters_snapshot(&before, sizeof(before)));
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind(&cfg));
    publish_healthy_telemetry();

    /* Give it every other reason to proceed: trusted time AND a usable
     * projection. Only the authorization is missing. */
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project_once(&env, &in));

    memset(&rec, 0, sizeof(rec));
    st = weather_runtime_step(&g_wx, NULL, NULL, &rec);
    TEST_ASSERT_TRUE(rec.trusted_time_at_evaluation);

    /* THE assertion: the committed schedule refuses at its first gate. */
    TEST_ASSERT_TRUE(weather_runtime_last_plan(&g_wx, &plan));
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DISABLED, plan.decision);
    TEST_ASSERT_EQUAL(WEATHER_SCHED_REASON_DISABLED, plan.reason);
    TEST_ASSERT_EQUAL_INT(-1, plan.slot_index);

    /* Nothing may be composed, submitted or fetched. */
    TEST_ASSERT_FALSE(submit_if_due(&cfg, W632_EPOCH_DUE));
    TEST_ASSERT_EQUAL_UINT32(0u, g_io.submit_count);
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);

    /* And no recommendation was produced, so no policy verdict was published. */
    TEST_ASSERT_NOT_EQUAL(WEATHER_RUNTIME_RECOMMENDATION_READY, st);
    TEST_ASSERT_FALSE(rec.present);
    TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
    assert_no_mutation(&before);
}

/* ================================================================== */
/* TEST B — authorization ON, outside every window                     */
/* ================================================================== */

TEST_CASE("w632 B: authorized but not due submits and fetches nothing",
          "[weather_pilot_sched]")
{
    NxWeatherSourceConfig cfg = source_cfg(true);
    WeatherSchedulePlan   plan;
    WeatherRecommendation rec;
    NxMutationSnapshot    before;

    fresh(W632_EPOCH_NOTDUE, true);
    TEST_ASSERT_TRUE(nx_mutation_counters_snapshot(&before, sizeof(before)));
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind(&cfg));
    publish_healthy_telemetry();

    memset(&rec, 0, sizeof(rec));
    (void)weather_runtime_step(&g_wx, NULL, NULL, &rec);
    TEST_ASSERT_TRUE(rec.trusted_time_at_evaluation);

    /*
     * 13:00 local is two hours past the 11:00 slot and catch-up is disabled by
     * the committed default, so the window is closed. Authorization is not a
     * licence to fetch whenever it likes.
     */
    TEST_ASSERT_TRUE(weather_runtime_last_plan(&g_wx, &plan));
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_NOT_DUE, plan.decision);

    TEST_ASSERT_FALSE(submit_if_due(&cfg, W632_EPOCH_NOTDUE));
    TEST_ASSERT_EQUAL_UINT32(0u, g_io.submit_count);
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);
    assert_no_mutation(&before);
}

/* ================================================================== */
/* TEST C — authorization ON and due: the whole production chain       */
/* ================================================================== */

TEST_CASE("w632 C: authorized and due runs exactly one async W3 flow",
          "[weather_pilot_sched]")
{
    NxWeatherSourceConfig   cfg = source_cfg(true);
    WeatherSchedulePlan     plan;
    WeatherObservation      obs;
    WeatherRuntimeStepEnv   step_env;
    TuningPolicyEnvironment env;
    TuningPolicyInput       in;
    WeatherRecommendation   rec;
    WeatherRuntimeState     st;
    NxMutationSnapshot      before;
    size_t                  i, n = 0;
    const TuningProfile    *reg;

    fresh(W632_EPOCH_DUE, true);
    TEST_ASSERT_TRUE(nx_mutation_counters_snapshot(&before, sizeof(before)));
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind(&cfg));

    /* 1 — the step evaluates the committed schedule exactly once. */
    memset(&rec, 0, sizeof(rec));
    (void)weather_runtime_step(&g_wx, NULL, NULL, &rec);
    TEST_ASSERT_TRUE(weather_runtime_last_plan(&g_wx, &plan));
    TEST_ASSERT_EQUAL(WEATHER_SCHEDULE_DUE, plan.decision);

    /* 2 — exactly ONE submission, and a second attempt is refused. */
    TEST_ASSERT_TRUE(submit_if_due(&cfg, W632_EPOCH_DUE));
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.submit_count);
    TEST_ASSERT_FALSE(submit_if_due(&cfg, W632_EPOCH_DUE));
    TEST_ASSERT_EQUAL_UINT32(1u, g_io.submit_count);

    /* 3 — the committed W3 orchestration, over the fake transport. */
    memset(&obs, 0, sizeof(obs));
    run_worker_and_consume(&obs);
    TEST_ASSERT_EQUAL_INT(1, g_tx.calls);        /* exactly one, never two */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, obs.provider_result);
    TEST_ASSERT_TRUE(obs.forecast_present);

    /* 4 — one telemetry snapshot, then the W6.3.1 projection. */
    publish_healthy_telemetry();
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project_once(&env, &in));
    memset(&step_env, 0, sizeof(step_env));
    step_env.policy_env = &env;
    step_env.policy_in  = &in;

    /* 5 — the step that genuinely reaches tuning_policy_evaluate(). */
    memset(&rec, 0, sizeof(rec));
    st = weather_runtime_step(&g_wx, &step_env, &obs, &rec);

    TEST_ASSERT_EQUAL_INT(WEATHER_RUNTIME_RECOMMENDATION_READY, st);
    TEST_ASSERT_TRUE(rec.present);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_OK, rec.forecast_status);

    /* 6 — and it authorized NOTHING. */
    TEST_ASSERT_NOT_EQUAL(TUNING_SELECT_PROFILE, rec.intent.action);
    TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
    TEST_ASSERT_EQUAL(WEATHER_NOT_EXECUTED_NO_PROFILE_SELECTED, rec.not_executed);
    TEST_ASSERT_EQUAL_STRING("", rec.intent.profile_id);

    /* 7 — every production profile is still UNVALIDATED and not eligible. */
    reg = tuning_registry_gamma601(&n);
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL(TUNING_VALIDATION_UNVALIDATED, reg[i].validation);
        TEST_ASSERT_FALSE(reg[i].payload_present);
        TEST_ASSERT_EQUAL_UINT32(0u, reg[i].evidence_fingerprint);
        TEST_ASSERT_NOT_EQUAL(TUNING_ELIGIBLE_OK,
                              tuning_profile_auto_eligible(&reg[i], &env.hw));
    }

    /* 8 — zero mutations across the whole authorized flow. */
    assert_no_mutation(&before);
}

/* ================================================================== */
/* Authorization never outranks trusted time                           */
/* ================================================================== */

TEST_CASE("w632: authorization cannot substitute for trusted time",
          "[weather_pilot_sched]")
{
    NxWeatherSourceConfig cfg = source_cfg(true);
    WeatherSchedulePlan   plan;
    WeatherRecommendation rec;
    WeatherRuntimeState   st;
    NxMutationSnapshot    before;

    /* Authorized, and the instant WOULD be due — but B2/B10 has no anchor. */
    fresh(W632_EPOCH_DUE, false);
    TEST_ASSERT_TRUE(nx_mutation_counters_snapshot(&before, sizeof(before)));
    TEST_ASSERT_EQUAL(NX_WX_SRC_READY_RECOMMENDATION_ONLY, bind(&cfg));
    publish_healthy_telemetry();

    memset(&rec, 0, sizeof(rec));
    st = weather_runtime_step(&g_wx, NULL, NULL, &rec);

    /* The runtime refuses before the schedule is even consulted. */
    TEST_ASSERT_EQUAL_INT(WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME, st);
    TEST_ASSERT_FALSE(rec.trusted_time_at_evaluation);
    TEST_ASSERT_EQUAL(WEATHER_NOT_EXECUTED_NO_TRUSTED_TIME, rec.not_executed);
    /* No plan was retained, so nothing can be submitted. */
    TEST_ASSERT_FALSE(weather_runtime_last_plan(&g_wx, &plan));
    TEST_ASSERT_FALSE(submit_if_due(&cfg, W632_EPOCH_DUE));
    TEST_ASSERT_EQUAL_UINT32(0u, g_io.submit_count);
    TEST_ASSERT_EQUAL_INT(0, g_tx.calls);
    assert_no_mutation(&before);
}

/* ================================================================== */
/* The pilot line separates AUTHORIZATION from DUE-NESS                */
/* ================================================================== */

TEST_CASE("w632: the pilot line reports schedule_enabled and schedule_due apart",
          "[weather_pilot_sched]")
{
    NxWeatherSourceConfig off = source_cfg(false);
    NxWeatherSourceConfig on  = source_cfg(true);
    NxWeatherPilotDiag    diag;
    NxWeatherPilotLine    line;

    nx_weather_pilot_diag_init(&diag);

    /* Unauthorized: not enabled, and necessarily not due. */
    memset(&line, 0, sizeof(line));
    TEST_ASSERT_TRUE(nx_weather_pilot_record(
        &diag, WX_EV_WAIT_SCHEDULE, 1000000ull, &off,
        NX_WX_SRC_READY_RECOMMENDATION_ONLY,
        WEATHER_RUNTIME_WAITING_FOR_WEATHER, NULL, false, WX_INV_OK, &line));
    TEST_ASSERT_FALSE(line.schedule_enabled);
    TEST_ASSERT_FALSE(line.schedule_due);
    TEST_ASSERT_FALSE(line.executed);

    /* Authorized but between windows: enabled=1, due=0 — the state that was
     * previously indistinguishable from "not authorized at all". */
    memset(&line, 0, sizeof(line));
    TEST_ASSERT_TRUE(nx_weather_pilot_record(
        &diag, WX_EV_WAIT_SCHEDULE, 2000000ull, &on,
        NX_WX_SRC_READY_RECOMMENDATION_ONLY,
        WEATHER_RUNTIME_WAITING_FOR_WEATHER, NULL, false, WX_INV_OK, &line));
    TEST_ASSERT_TRUE(line.schedule_enabled);
    TEST_ASSERT_FALSE(line.schedule_due);

    /* Authorized and due. */
    memset(&line, 0, sizeof(line));
    TEST_ASSERT_TRUE(nx_weather_pilot_record(
        &diag, WX_EV_SCHEDULE_DUE, 3000000ull, &on,
        NX_WX_SRC_READY_RECOMMENDATION_ONLY,
        WEATHER_RUNTIME_WAITING_FOR_WEATHER, NULL, true, WX_INV_OK, &line));
    TEST_ASSERT_TRUE(line.schedule_enabled);
    TEST_ASSERT_TRUE(line.schedule_due);
    TEST_ASSERT_FALSE(line.executed);
}
