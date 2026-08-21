/*
 * Gate W6.3.1 — THE END-TO-END PRODUCTION FLOW.
 *
 * WHY THIS FILE EXISTS. The Gate W6.3.1 unit tests prove the projection is
 * correct in isolation, and the Gate W6.2 wiring tests prove the borrowed clock
 * is the real one — but neither joins the links. Until this file, no test
 * exercised the sequence a real device actually performs, and the load-bearing
 * claim of the gate is about that sequence, not about any one link in it:
 *
 *   synthetic trusted time
 *     -> the committed Brussels schedule says a window is DUE
 *       -> the Gate W6.3 bounded async I/O machine (submit/claim/publish/consume)
 *         -> the committed W3 fetch orchestration over a FAKE transport
 *           -> a valid WeatherObservation
 *             -> exactly ONE NxTelemetrySafetySnapshot read
 *               -> the Gate W6.3.1 projection
 *                 -> tuning_policy_evaluate() ACTUALLY RUNS
 *                   -> a bounded recommendation, executed = false, 0 mutations
 *
 * The chain is driven through the SAME committed entry points the pilot adapter
 * uses, in the same order, so a test that passes here is a statement about the
 * production path and not about a rehearsal of it.
 *
 * WHAT IS FAKE, AND ONLY WHAT IS FAKE: the trusted-time clock (a synthetic
 * anchor) and the W3 transport (a canned body). Everything else — the schedule,
 * the Brussels conversion, the bridge, the I/O state machine, the telemetry
 * store, the projection, the W1 registry, the W1 policy and the W4 runtime — is
 * the committed production code.
 *
 * NOTHING here contacts a network, DNS, an NTP server, a weather provider, a
 * pool, OTA, a restart path, NVS or hardware. Every coordinate is a synthetic
 * fixture and no owner value appears anywhere.
 */

#include <string.h>

#include "unity.h"

#include "weather_runtime.h"
#include "weather_transport.h"
#include "weather_open_meteo.h"
#include "nx_weather_io.h"
#include "nx_telemetry_safety.h"
#include "nx_tuning_input.h"
#include "nx_mutation_counters.h"
#include "nx_weather_pilot_diag.h"
#include "nx_weather_source.h"
#include "tuning_profile.h"
#include "tuning_policy.h"
#include "local_schedule.h"
#include "brussels_time.h"

#define US_PER_S 1000000ull

/* 2026-07-15T12:00:00Z. Summer, so Brussels is CEST (UTC+2) and the local date
 * is 2026-07-15. Fixture only; it identifies nobody and nothing. */
#define W631_EPOCH_S 1784116800ull
#define W631_DATE_Y  2026
#define W631_DATE_M  7
#define W631_DATE_D  15

/* Synthetic telemetry. In-band and plausible, so the tests below turn on the
 * VALIDITY FLAGS rather than on a value being conveniently out of range. */
#define W631_ASIC_DC 552      /* 55.2 C */
#define W631_VRM_DC  610      /* 61.0 C */
#define W631_RPM     3200u

/* ================================================================== */
/* Fake trusted-time clock (the ONLY time authority in this file)      */
/* ================================================================== */

typedef struct {
    uint64_t       monotonic_us;
    PoolTimeAnchor anchor;
    bool           initialized;
} FlowClock;

static FlowClock           g_clk;
static PoolTimeClock       g_clock;
static PoolTimeTrustPolicy g_policy;

static uint64_t fc_monotonic(void *ctx) { return ((FlowClock *)ctx)->monotonic_us; }

static bool fc_anchor(void *ctx, PoolTimeAnchor *out)
{
    FlowClock *c = (FlowClock *)ctx;

    if (!c->initialized) {
        return false;
    }
    *out = c->anchor;
    return true;
}

static const PoolTimeClockOps FC_OPS = { .monotonic_us = fc_monotonic,
                                         .read_anchor  = fc_anchor };

static void clock_trusted(uint64_t epoch_s, uint64_t age_s)
{
    memset(&g_clk, 0, sizeof(g_clk));
    g_clk.initialized                     = true;
    g_clk.monotonic_us                    = (age_s + 10u) * US_PER_S;
    g_clk.anchor.valid                    = true;
    g_clk.anchor.sync_completed_this_boot = true;
    g_clk.anchor.generation               = 3u;
    g_clk.anchor.epoch_us_at_sync         = epoch_s * US_PER_S;
    g_clk.anchor.monotonic_us_at_sync     = 10u * US_PER_S;
    g_clk.anchor.sync_status              = POOL_TIME_SYNC_STATUS_COMPLETED;
    g_clk.anchor.last_error               = TIME_OK;
    g_clock.ops = &FC_OPS;
    g_clock.ctx = &g_clk;
    pool_time_trust_policy_defaults(&g_policy);
}

/* ================================================================== */
/* Fake W3 transport — counts calls; NEVER performs I/O                */
/* ================================================================== */

typedef struct {
    int         calls;
    const char *body;
} FlowTx;

static FlowTx g_tx;

static WeatherProviderResult flow_fetch(void *ctx, const WeatherRequest *req,
                                        WeatherHttpResponse *out)
{
    FlowTx *t = (FlowTx *)ctx;
    size_t  n;

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

static const WeatherTransportOps FLOW_TX_OPS = { .fetch = flow_fetch };

/*
 * The ~4 KB staging buffer weather_fetch_execute() requires. Deliberately a
 * file-scope object: the unit-test main task runs on a bounded stack and a
 * 4 KB local here would be the test harness's own bug, not the code's.
 */
static WeatherHttpResponse g_scratch;

/* ================================================================== */
/* Fixtures                                                            */
/* ================================================================== */

static WeatherRuntime g_wx;
static NxWeatherIo    g_io;

static WeatherRuntimeConfig flow_cfg(void)
{
    WeatherRuntimeConfig c;
    BrusselsLocalTime    local;

    weather_runtime_config_defaults(&c);
    c.enabled               = true;
    c.expected_provider     = WEATHER_PROVIDER_OPEN_METEO;
    c.location.latitude_e4  = 508500;      /* synthetic fixture only */
    c.location.longitude_e4 = 43500;
    c.location.timezone     = WEATHER_TZ_EUROPE_BRUSSELS;
    c.schedule.enabled      = true;
    c.schedule.slot_count   = 1u;
    memset(c.schedule.slots_min, 0, sizeof(c.schedule.slots_min));

    /*
     * THE SLOT IS DERIVED FROM THE FIXTURE'S OWN LOCAL TIME, not hardcoded.
     *
     * The committed schedule ships `catch_up_window_s == 0`, and with catch-up
     * disabled a slot is DUE only while `now - slot_instant` is inside
     * WEATHER_SCHEDULE_ON_TIME_WINDOW_S. A "due from local midnight" slot is
     * therefore NOT due at a mid-afternoon fixture — it is CATCH_UP_DISABLED,
     * fourteen hours late. Placing the slot at the fixture's current local
     * minute puts the delay at zero, which is the only honest way to reach DUE
     * without weakening the committed rule or inventing a catch-up window.
     */
    TEST_ASSERT_TRUE(brussels_local_from_utc(W631_EPOCH_S, &local));
    c.schedule.slots_min[0] = local.minute_of_day;

    TEST_ASSERT_TRUE_MESSAGE(weather_runtime_config_valid(&c),
                             "fixture rejected by the committed validator");
    return c;
}

static void publish_healthy_telemetry(void)
{
    NxTelemetryPowerFacts p;
    NxTelemetryFanFacts   f;

    memset(&p, 0, sizeof(p));
    p.asic_temp_dc  = W631_ASIC_DC; p.asic_temp_valid = true;
    p.vrm_temp_dc   = W631_VRM_DC;  p.vrm_read_ok     = true;
    p.vrm_expected  = true;
    nx_telemetry_safety_publish_power(&p);

    memset(&f, 0, sizeof(f));
    f.fan_rpm = W631_RPM; f.fan_expected = true;
    nx_telemetry_safety_publish_fan(&f);
}

/* The ONE snapshot read, projected exactly as nx_weather_pilot_observe() does. */
static NxTuningInputFact project_once(TuningPolicyEnvironment *env,
                                      TuningPolicyInput *in,
                                      NxTuningInputDiag *diag)
{
    NxTelemetrySafetySnapshot snap;
    NxTuningStructuralFacts   facts;
    const TuningProfile      *reg;
    size_t                    n = 0;

    nx_tuning_structural_facts_gamma601(&facts);
    reg = tuning_registry_gamma601(&n);
    TEST_ASSERT_TRUE(nx_telemetry_safety_read(&snap));
    return nx_tuning_input_project(&snap, &facts, reg, n, env, in, diag);
}

/*
 * Run the Gate W6.3 asynchronous half exactly as the adapter does: submit only
 * what the schedule plan says is due, claim on the worker side, run the ONE
 * committed fetch orchestration, publish, consume. No task is created — the
 * machine is the pure one, driven single-threaded, which is precisely why every
 * transition is deterministic here.
 */
static bool run_io_cycle(const WeatherRuntimeConfig *cfg,
                         const WeatherSchedulePlan *plan,
                         WeatherObservation *out_obs)
{
    NxWeatherIoRequest req;
    NxWeatherIoRequest claimed;
    NxWeatherIoResult  got;
    WeatherObservation obs;
    WeatherProviderResult pr;
    BrusselsLocalTime  local;
    uint64_t           now_us = 1000ull;

    /* The adapter's own precondition. Asserted rather than silently returned,
     * so a schedule regression names itself here instead of surfacing as a
     * confusing failure three assertions later. */
    TEST_ASSERT_TRUE_MESSAGE(plan->decision == WEATHER_SCHEDULE_DUE ||
                             plan->decision == WEATHER_SCHEDULE_CATCH_UP_DUE,
                             "no window is due: nothing may be submitted");
    TEST_ASSERT_TRUE(brussels_local_from_utc(W631_EPOCH_S, &local));

    memset(&req, 0, sizeof(req));
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK,
                      weather_open_meteo_build_request(&cfg->location,
                                                       &req.request));
    req.generation                = 1u;
    req.window.date               = local.date;
    req.window.slot_index         = plan->slot_index;
    req.window.provider           = cfg->expected_provider;
    req.parse.expected_date       = local.date;
    req.parse.fetch_epoch_s       = W631_EPOCH_S;
    req.parse.fetch_epoch_trusted = true;
    req.parse.source_generation   = req.generation;

    TEST_ASSERT_EQUAL(WX_IO_SUBMIT_ACCEPTED,
                      nx_weather_io_submit(&g_io, &req, now_us));

    memset(&claimed, 0, sizeof(claimed));
    TEST_ASSERT_TRUE(nx_weather_io_claim(&g_io, &claimed, now_us));

    /* THE committed W3 orchestration — the same function the real worker task
     * runs. fetch_counter is NULL, exactly as the worker passes it. */
    memset(&obs, 0, sizeof(obs));
    pr = weather_fetch_execute(&FLOW_TX_OPS, &g_tx, &claimed.request,
                               &claimed.parse, NULL, &g_scratch, &obs);
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, pr);

    TEST_ASSERT_TRUE(nx_weather_io_publish(&g_io, claimed.generation, pr, &obs,
                                           now_us + 1000ull));

    memset(&got, 0, sizeof(got));
    TEST_ASSERT_TRUE(nx_weather_io_consume(&g_io, &req.window, &got,
                                           now_us + 2000ull));
    *out_obs = got.observation;
    return true;
}

static void flow_fresh(const char *body)
{
    memset(&g_wx, 0, sizeof(g_wx));
    memset(&g_tx, 0, sizeof(g_tx));
    memset(&g_scratch, 0, sizeof(g_scratch));
    g_tx.body = body;
    nx_weather_io_init(&g_io);
    nx_telemetry_safety_reset();
    nx_mutation_counters_reset_for_test();
    clock_trusted(W631_EPOCH_S, 5u);
}

static bool bind_runtime(const WeatherRuntimeConfig *c)
{
    WeatherRuntimeDeps deps;

    memset(&deps, 0, sizeof(deps));
    deps.clock       = &g_clock;
    deps.time_policy = &g_policy;
    /* The production seam stays NULL: the transport belongs to the W6.3 worker
     * side above, never to the task that steps the runtime. */
    deps.transport     = NULL;
    deps.transport_ctx = NULL;
    return weather_runtime_init(&g_wx, c, &deps) == WEATHER_RUNTIME_OK;
}

/*
 * A well-formed Open-Meteo payload for the fixture date, in the SHAPE THE
 * COMMITTED PARSER ACTUALLY REQUIRES — utc_offset_seconds, timezone,
 * timezone_abbreviation, both unit blocks and both data blocks. A shorter
 * "obviously fine" body is rejected with ERR_TIMEZONE_INVALID, so the canonical
 * shape is used here rather than a hand-trimmed one that would silently degrade
 * this whole test into an error path.
 */
#define DEG_C "\xC2\xB0" "C"
#define BODY_HEAD \
    "{\"utc_offset_seconds\":7200,\"timezone\":\"Europe/Brussels\"," \
    "\"timezone_abbreviation\":\"CEST\"," \
    "\"current_units\":{\"temperature_2m\":\"" DEG_C "\"},"
#define BODY_TAIL(cur, maxi) \
    "\"current\":{\"temperature_2m\":" cur "}," \
    "\"daily_units\":{\"temperature_2m_max\":\"" DEG_C "\"}," \
    "\"daily\":{\"time\":[\"2026-07-15\"],\"temperature_2m_max\":[" maxi "]}}"

/* 24.5 C is at or below the committed cool threshold (28.0 C) -> COOL_PROFILE. */
#define BODY_COOL BODY_HEAD BODY_TAIL("22.0", "24.5")
/* 35.0 C is at or above the committed hot threshold (30.0 C) -> HOT_PROFILE.  */
#define BODY_HOT  BODY_HEAD BODY_TAIL("33.0", "35.0")

/* ================================================================== */
/* A. THE snapshot is read ONCE and the projection is bound to it      */
/* ================================================================== */

TEST_CASE("w631flow: the projection is bound to ONE snapshot, never re-read",
          "[weather_pilot_flow]")
{
    TuningPolicyEnvironment env;
    TuningPolicyInput       in;
    NxTuningInputDiag       d;
    NxTelemetrySafetySnapshot snap;
    NxTuningStructuralFacts   facts;
    const TuningProfile      *reg;
    size_t                    n = 0;
    NxTelemetryPowerFacts     bad_p;
    NxTelemetryFanFacts       bad_f;

    nx_telemetry_safety_reset();
    publish_healthy_telemetry();

    /* Take the ONE copy the pilot takes. */
    TEST_ASSERT_TRUE(nx_telemetry_safety_read(&snap));

    /*
     * Now make the LIVE store say the opposite in every field. A projection
     * that re-read anything — the store, PowerManagementModule, the TPS546
     * globals — would report this instead of the snapshot it was handed.
     */
    memset(&bad_p, 0, sizeof(bad_p));
    bad_p.asic_temp_dc = -1;  bad_p.asic_temp_valid = false;
    bad_p.vrm_temp_dc  = 0;   bad_p.vrm_read_ok     = false;
    bad_p.vrm_expected = true; bad_p.emergency_thermal_active = true;
    nx_telemetry_safety_publish_power(&bad_p);
    memset(&bad_f, 0, sizeof(bad_f));
    bad_f.fan_rpm = 0u; bad_f.fan_expected = true; bad_f.fan_control_fault = true;
    nx_telemetry_safety_publish_fan(&bad_f);

    nx_tuning_structural_facts_gamma601(&facts);
    reg = tuning_registry_gamma601(&n);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK,
                      nx_tuning_input_project(&snap, &facts, reg, n, &env, &in, &d));

    /* Every fact is the SNAPSHOT's, not the store's. */
    TEST_ASSERT_EQUAL(TUNING_SENSOR_OK, in.sensors.asic_temp);
    TEST_ASSERT_EQUAL(TUNING_SENSOR_OK, in.sensors.vrm_temp);
    TEST_ASSERT_EQUAL(TUNING_SENSOR_OK, in.sensors.fan_tach);
    TEST_ASSERT_FALSE(in.sensors.fan_control_uncertain);
    TEST_ASSERT_FALSE(in.emergency_thermal_active);
    /* And the generations name the publication it came from, not the newer one. */
    TEST_ASSERT_EQUAL_UINT32(1u, d.telemetry_power_generation);
    TEST_ASSERT_EQUAL_UINT32(1u, d.telemetry_fan_generation);
}

TEST_CASE("w631flow: projecting the same snapshot twice is identical",
          "[weather_pilot_flow]")
{
    TuningPolicyEnvironment e1, e2;
    TuningPolicyInput       i1, i2;
    NxTuningInputDiag       d1, d2;

    nx_telemetry_safety_reset();
    publish_healthy_telemetry();

    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project_once(&e1, &i1, &d1));
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project_once(&e2, &i2, &d2));

    /* Pure: no hidden state, no accumulation, no drift between evaluations. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(&i1, &i2, sizeof(i1)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(&d1, &d2, sizeof(d1)));
    TEST_ASSERT_EQUAL_PTR(e1.profiles, e2.profiles);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)e1.profile_count,
                             (uint32_t)e2.profile_count);
}

/* ================================================================== */
/* B. THE END-TO-END FLOW                                              */
/* ================================================================== */

/*
 * `hot` selects which climate direction the fixture drives. Both must end with
 * no authorized profile: the reasons differ, the safety answer does not.
 */
static void run_full_flow(bool hot, WeatherRecommendation *out_rec,
                          WeatherRuntimeState *out_state)
{
    WeatherRuntimeConfig    c;
    WeatherSchedulePlan     plan;
    WeatherObservation      obs;
    WeatherRuntimeStepEnv   step_env;
    TuningPolicyEnvironment env;
    TuningPolicyInput       in;
    NxTuningInputDiag       d;
    NxMutationSnapshot      before, after;
    NxMutationDelta         delta;
    WeatherRecommendation   rec;
    WeatherRuntimeState     st;

    flow_fresh(hot ? BODY_HOT : BODY_COOL);
    c = flow_cfg();
    TEST_ASSERT_TRUE(bind_runtime(&c));

    TEST_ASSERT_TRUE(nx_mutation_counters_snapshot(&before, sizeof(before)));

    /* 1 — one step with no observation: this is what evaluates the committed
     *     schedule exactly once and makes the plan readable. */
    memset(&rec, 0, sizeof(rec));
    st = weather_runtime_step(&g_wx, NULL, NULL, &rec);
    TEST_ASSERT_EQUAL_INT(WEATHER_RUNTIME_WAITING_FOR_WEATHER, st);
    TEST_ASSERT_TRUE(rec.trusted_time_at_evaluation);

    /* 2 — the schedule says a window is DUE. */
    TEST_ASSERT_TRUE(weather_runtime_last_plan(&g_wx, &plan));
    TEST_ASSERT_TRUE(plan.decision == WEATHER_SCHEDULE_DUE ||
                     plan.decision == WEATHER_SCHEDULE_CATCH_UP_DUE);

    /* 3 — the W6.3 async machine + the committed W3 fetch. */
    memset(&obs, 0, sizeof(obs));
    TEST_ASSERT_TRUE(run_io_cycle(&c, &plan, &obs));
    TEST_ASSERT_EQUAL_INT(1, g_tx.calls);          /* exactly one, never two */
    TEST_ASSERT_EQUAL(WEATHER_PROVIDER_OK, obs.provider_result);
    TEST_ASSERT_TRUE(obs.forecast_present);

    /* 4 — telemetry, then ONE snapshot read and the W6.3.1 projection. */
    publish_healthy_telemetry();
    TEST_ASSERT_EQUAL(NX_W1_INPUT_OK, project_once(&env, &in, &d));
    TEST_ASSERT_TRUE(nx_tuning_input_usable(NX_W1_INPUT_OK));

    memset(&step_env, 0, sizeof(step_env));
    step_env.policy_env = &env;
    step_env.policy_in  = &in;

    /* 5 — the step that now genuinely reaches tuning_policy_evaluate(). */
    memset(&rec, 0, sizeof(rec));
    st = weather_runtime_step(&g_wx, &step_env, &obs, &rec);

    /*
     * THE assertion the gate exists for. With env == NULL — the posture before
     * W6.3.1 — weather_runtime_step() returns at its env-NULL branch and
     * RECOMMENDATION_READY is structurally unreachable. Reaching it here proves
     * the policy ran on real projected inputs.
     */
    TEST_ASSERT_EQUAL_INT(WEATHER_RUNTIME_RECOMMENDATION_READY, st);
    TEST_ASSERT_TRUE(rec.present);
    TEST_ASSERT_TRUE(rec.trusted_time_at_evaluation);
    TEST_ASSERT_EQUAL(TUNING_FORECAST_OK, rec.forecast_status);

    /* 6 — and it authorized NOTHING. */
    TEST_ASSERT_NOT_EQUAL(TUNING_SELECT_PROFILE, rec.intent.action);
    TEST_ASSERT_FALSE(rec.actionable_in_future_gate);
    TEST_ASSERT_EQUAL(WEATHER_NOT_EXECUTED_NO_PROFILE_SELECTED, rec.not_executed);
    TEST_ASSERT_EQUAL_STRING("", rec.intent.profile_id);

    /* 7 — zero mutations across the whole flow. */
    TEST_ASSERT_TRUE(nx_mutation_counters_snapshot(&after, sizeof(after)));
    TEST_ASSERT_TRUE(nx_mutation_delta_compute(&before, &after, &delta));
    TEST_ASSERT_TRUE_MESSAGE(nx_mutation_delta_clean(&delta),
                             "the weather flow mutated something");
    TEST_ASSERT_EQUAL_UINT32(0u, delta.hardware_total);
    TEST_ASSERT_EQUAL_UINT32(0u, delta.pool_total);
    TEST_ASSERT_EQUAL_UINT32(0u, delta.protocol_total);
    TEST_ASSERT_EQUAL_UINT32(0u, delta.restart_total);
    TEST_ASSERT_EQUAL_UINT32(0u, delta.ota_total);

    *out_rec   = rec;
    *out_state = st;
}

TEST_CASE("w631flow: a COOL forecast reaches the policy and authorizes nothing",
          "[weather_pilot_flow]")
{
    WeatherRecommendation rec;
    WeatherRuntimeState   st;
    size_t                i, n = 0;
    const TuningProfile  *reg;
    TuningHardwareContext hw;

    run_full_flow(false, &rec, &st);

    /* The committed cool-day path with no role configured: a real verdict, not
     * a short circuit. */
    TEST_ASSERT_EQUAL(TUNING_SELECT_NONE, rec.intent.action);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_WEATHER_POLICY, rec.intent.winning_source);
    TEST_ASSERT_EQUAL(TUNING_REASON_WEATHER_COOL_ELIGIBLE, rec.intent.reason);

    /* Every production profile is still UNVALIDATED and still not eligible. */
    reg = tuning_registry_gamma601(&n);
    memset(&hw, 0, sizeof(hw));
    hw.board = TUNING_BOARD_GAMMA_601; hw.asic = TUNING_ASIC_BM1370;
    hw.cooling_installed = TUNING_COOLING_UNSPECIFIED;
    hw.psu_installed     = TUNING_PSU_UNSPECIFIED;
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL(TUNING_VALIDATION_UNVALIDATED, reg[i].validation);
        TEST_ASSERT_FALSE(reg[i].payload_present);
        TEST_ASSERT_NOT_EQUAL(TUNING_ELIGIBLE_OK,
                              tuning_profile_auto_eligible(&reg[i], &hw));
    }
}

TEST_CASE("w631flow: a HOT forecast reaches the policy and authorizes nothing",
          "[weather_pilot_flow]")
{
    WeatherRecommendation rec;
    WeatherRuntimeState   st;

    run_full_flow(true, &rec, &st);

    /* The hot-day role is unconfigured, so the committed policy surfaces
     * operator recovery — a refusal that names its own cause. */
    TEST_ASSERT_EQUAL(TUNING_SELECT_OPERATOR_RECOVERY, rec.intent.action);
    TEST_ASSERT_EQUAL(TUNING_SOURCE_WEATHER_POLICY, rec.intent.winning_source);
    TEST_ASSERT_EQUAL(TUNING_REASON_WEATHER_HOT_FORECAST, rec.intent.reason);
    TEST_ASSERT_EQUAL(TUNING_REASON_NO_VALID_HOT_PROFILE,
                      rec.intent.secondary_reason);
}

/* ================================================================== */
/* C. executed = false, on the REAL pilot line                         */
/* ================================================================== */

TEST_CASE("w631flow: the pilot line reports executed=false and not actionable",
          "[weather_pilot_flow]")
{
    WeatherRecommendation rec;
    WeatherRuntimeState   st;
    NxWeatherPilotDiag    diag;
    NxWeatherPilotLine    line;
    NxWeatherPilotEvent   ev;

    run_full_flow(false, &rec, &st);

    nx_weather_pilot_diag_init(&diag);
    ev = nx_weather_pilot_classify(NX_WX_SRC_READY_RECOMMENDATION_ONLY, st,
                                   &rec, true);
    /* A recommendation exists but nothing may act on it. */
    TEST_ASSERT_EQUAL(WX_EV_RECOMMENDATION_NOT_ACTIONABLE, ev);

    memset(&line, 0, sizeof(line));
    TEST_ASSERT_TRUE(nx_weather_pilot_record(&diag, ev, 1000000ull, NULL,
                                             NX_WX_SRC_READY_RECOMMENDATION_ONLY,
                                             st, &rec, true, WX_INV_OK, &line));

    /* The gate's central claim, on the line an owner actually reads. */
    TEST_ASSERT_FALSE(line.executed);
    TEST_ASSERT_FALSE(line.actionable_in_future_gate);
    TEST_ASSERT_TRUE(line.recommendation_present);
    TEST_ASSERT_TRUE(line.policy_evaluated);
    TEST_ASSERT_EQUAL(WEATHER_NOT_EXECUTED_NO_PROFILE_SELECTED,
                      line.not_executed);
}

/* ================================================================== */
/* D. The refusal side: an unusable projection is never evaluated      */
/* ================================================================== */

TEST_CASE("w631flow: unpublished telemetry never reaches the policy",
          "[weather_pilot_flow]")
{
    WeatherRuntimeConfig    c;
    WeatherSchedulePlan     plan;
    WeatherObservation      obs;
    TuningPolicyEnvironment env;
    TuningPolicyInput       in;
    NxTuningInputDiag       d;
    WeatherRecommendation   rec;
    WeatherRuntimeState     st;
    NxTuningInputFact       fact;

    flow_fresh(BODY_COOL);
    c = flow_cfg();
    TEST_ASSERT_TRUE(bind_runtime(&c));

    memset(&rec, 0, sizeof(rec));
    (void)weather_runtime_step(&g_wx, NULL, NULL, &rec);
    TEST_ASSERT_TRUE(weather_runtime_last_plan(&g_wx, &plan));
    memset(&obs, 0, sizeof(obs));
    TEST_ASSERT_TRUE(run_io_cycle(&c, &plan, &obs));

    /* ONLY the fan side publishes: the power side has never run. */
    {
        NxTelemetryFanFacts f;
        memset(&f, 0, sizeof(f));
        f.fan_rpm = W631_RPM; f.fan_expected = true;
        nx_telemetry_safety_publish_fan(&f);
    }
    fact = project_once(&env, &in, &d);
    TEST_ASSERT_EQUAL(NX_W1_INPUT_TELEMETRY_PARTIAL, fact);
    TEST_ASSERT_FALSE(nx_tuning_input_usable(fact));

    /*
     * The adapter therefore withholds the environment, exactly as
     * nx_weather_pilot_observe() does, and the runtime reports the honest
     * "no selection could be made" rather than evaluating a half-observed
     * device. A valid observation is NOT sufficient — the safety inputs must
     * be observable too.
     */
    memset(&rec, 0, sizeof(rec));
    st = weather_runtime_step(&g_wx, NULL, &obs, &rec);
    TEST_ASSERT_NOT_EQUAL(WEATHER_RUNTIME_RECOMMENDATION_READY, st);
    TEST_ASSERT_FALSE(rec.present);
    TEST_ASSERT_FALSE(rec.actionable_in_future_gate);

    /* Even the fail-closed projection could not have authorized anything. */
    TEST_ASSERT_NOT_EQUAL(TUNING_SENSOR_OK, in.sensors.asic_temp);
    TEST_ASSERT_NOT_EQUAL(TUNING_SENSOR_OK, in.sensors.vrm_temp);
}
