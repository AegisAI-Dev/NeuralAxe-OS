/*
 * NeuralAxe Weather-Aware Tuning — Gate W6 pilot logging adapter.
 *
 * The whole translation unit is compiled out unless
 * CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS is set, so neither the
 * default image, nor a W4-only image, nor a W5-configured image contains a
 * byte of it. The pure diagnostic core stays free of logging; all of it lives
 * here, exactly as the committed B10.1 and B10.2 adapters do.
 *
 * It creates NO task, NO timer, NO queue and NO HTTP endpoint, performs NO
 * NVS write and NO persistence, and prints ONLY bounded machine tokens and
 * scalars produced by the pure core. A coordinate, city, timezone, hostname,
 * URL, forecast body, NTP server, pool identity or session id cannot appear
 * here, because the line type it formats has no field that could hold one.
 */

#include "sdkconfig.h"

#ifdef CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nx_weather_pilot_diag.h"
#include "nx_weather_pilot_diag_log.h"
#include "nx_weather_pilot_facts.h"
#include "nx_mutation_baseline.h"

static const char *TAG = "nx_wx_pilot";

/* The single RAM-only accumulator. No persistence, ever. */
static NxWeatherPilotDiag s_diag;
static bool               s_booted;

/*
 * The W5 configuration and the W4 runtime posture as observed at boot. They
 * are derived from BUILD-TIME configuration and from a runtime step with no
 * injected clock, transport or store, so they cannot change while the device
 * runs. Caching them lets the periodic observation avoid rebuilding a runtime
 * whose answer is fixed by construction. No private value is stored: the
 * config here is the same bounded structure the boot notice already holds.
 */
static NxWeatherSourceConfig s_cfg;
static NxWeatherSourceStatus s_status;
static WeatherRuntimeState   s_state;
static WeatherRecommendation s_rec;
static bool                  s_posture_cached;

/*
 * The task the periodic observation is expected to run on. Latched on the
 * FIRST observation; every later call must come from the same task. A second
 * caller would mean the pilot is being driven from somewhere unaudited, which
 * is a reason to withhold the baseline rather than to average over.
 */
static TaskHandle_t s_host_task;

/* Stack headroom the host task must retain for the observation to be
 * considered safely hosted, in stack WORDS as FreeRTOS reports it. */
#define NX_WX_HOST_STACK_FLOOR_WORDS 512u

static bool host_task_valid(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    if (self == NULL) {
        return false;
    }
    if (s_host_task == NULL) {
        s_host_task = self;      /* first observation defines the host */
    }
    if (s_host_task != self) {
        return false;            /* a second, unaudited driver */
    }
    return uxTaskGetStackHighWaterMark(NULL) >= NX_WX_HOST_STACK_FLOOR_WORDS;
}

static void fill_resources(NxWeatherPilotLine *line)
{
    /* Internal heap only: the number a pilot reviewer needs, and one that
     * says nothing about the owner. */
    line->free_heap        = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    line->min_free_heap    = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    line->stack_high_water = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
}

/*
 * Copy the Gate W6.1 evidence out of the posture that was just checked, plus
 * the baseline lifecycle state. Every field is a bounded scalar or a token
 * id; nothing here can carry a coordinate, identity, host or secret.
 */
static void fill_evidence(NxWeatherPilotLine *line,
                          const NxWeatherPilotPosture *posture)
{
    const NxMutationBaseline *b = nx_mutation_baseline_get();

    line->baseline_state           = (uint8_t)b->state;
    line->baseline_block           = (uint8_t)b->last_block;
    line->baseline_us              = b->captured_us;
    line->baseline_zero_at_capture = b->zero_at_capture;
#ifdef CONFIG_NX_MUTATION_OBSERVABILITY
    line->mutation_observability_present = true;
#else
    line->mutation_observability_present = false;
#endif
    if (posture == NULL) {
        return;
    }
    line->counter_history_lost = posture->counter_history_lost;
    line->mut_hardware         = posture->hardware_write_count;
    line->mut_pool             = posture->pool_write_count;
    line->mut_protocol         = posture->protocol_write_count;
    line->mut_restart          = posture->restart_request_count;
    line->mut_ota              = posture->ota_request_count;
    line->mut_session          = posture->session_mutation_count;
    line->tuning_unchanged     = posture->tuning_unchanged;
}

static void emit(const NxWeatherPilotLine *l)
{
    /*
     * One bounded line. Every %s is a pure enum-to-string map and every %u /
     * %llu is a counter or a resource measurement.
     */
    ESP_LOGI(TAG,
             "%s seq=%u src=%s state=%s provider_cfg=%d location_cfg=%d "
             "tz_cfg=%d rec_only=%d trusted_time=%d schedule_due=%d "
             "policy=%d rec=%d actionable=%d executed=%d fresh=%s "
             "result=%s reason=%s inv=%s fetches=%u recs=%u "
             "heap=%u minheap=%u stack=%u up_us=%llu "
             "mut_obs=%d base=%s block=%s base_us=%llu base_zero=%d "
             "hist_lost=%d "
             "mut_hw=%u mut_pool=%u mut_proto=%u mut_restart=%u mut_ota=%u "
             "mut_session=%u tuning_same=%d",
             nx_weather_pilot_event_str(l->event),
             (unsigned)l->sequence,
             nx_weather_source_status_str(l->source_status),
             weather_runtime_state_str(l->runtime_state),
             (int)l->provider_configured, (int)l->location_configured,
             (int)l->timezone_configured, (int)l->recommendation_only,
             (int)l->trusted_time_available, (int)l->schedule_due,
             (int)l->policy_evaluated, (int)l->recommendation_present,
             (int)l->actionable_in_future_gate, (int)l->executed,
             weather_freshness_str(l->freshness),
             weather_provider_result_str(l->provider_result),
             weather_not_executed_str(l->not_executed),
             nx_weather_pilot_invariant_str(l->invariant),
             (unsigned)l->fetch_attempts, (unsigned)l->recommendations,
             (unsigned)l->free_heap, (unsigned)l->min_free_heap,
             (unsigned)l->stack_high_water,
             (unsigned long long)l->uptime_us,
             (int)l->mutation_observability_present,
             nx_mutation_baseline_state_str(
                 (NxMutationBaselineState)l->baseline_state),
             nx_mutation_baseline_block_str(
                 (NxBaselineBlock)l->baseline_block),
             (unsigned long long)l->baseline_us,
             (int)l->baseline_zero_at_capture, (int)l->counter_history_lost,
             (unsigned)l->mut_hardware, (unsigned)l->mut_pool,
             (unsigned)l->mut_protocol, (unsigned)l->mut_restart,
             (unsigned)l->mut_ota, (unsigned)l->mut_session,
             (int)l->tuning_unchanged);
}

void nx_weather_pilot_log_boot(void)
{
    NxWeatherPilotLine line;

    nx_weather_pilot_diag_init(&s_diag);
    s_booted = true;

    if (nx_weather_pilot_record(&s_diag, WX_EV_PILOT_BOOT,
                                (uint64_t)esp_timer_get_time(),
                                NULL, NX_WX_SRC_UNCONFIGURED,
                                WEATHER_RUNTIME_DISABLED, NULL, false,
                                WX_INV_OK, &line)) {
        fill_resources(&line);
        fill_evidence(&line, NULL);
        emit(&line);
    }
    /*
     * Gate W6.1 correction: this line no longer claims "hardware=unchanged"
     * or "pool=unchanged" at boot. Those are exactly the propositions the
     * pilot exists to PROVE, and before a baseline is captured there is
     * nothing to prove them against. What the line states now is what the
     * image structurally is, plus the honest position of the observation.
     */
    ESP_LOGI(TAG, "WX_PILOT_BOOT mode=recommendation_only execution=disabled "
                  "api=disabled persistence=none task=none "
                  "mutation_claims=pending_baseline");
}

void nx_weather_pilot_log_step(const NxWeatherSourceConfig *cfg,
                               NxWeatherSourceStatus source,
                               WeatherRuntimeState state,
                               const WeatherRecommendation *rec,
                               bool schedule_due,
                               const NxWeatherPilotPosture *posture)
{
    NxWeatherPilotEvent     ev;
    NxWeatherPilotInvariant inv;
    NxWeatherPilotLine      line;
    uint64_t                now_us;

    if (!s_booted) {
        nx_weather_pilot_log_boot();
    }
    now_us = (uint64_t)esp_timer_get_time();

    /* The monitor READS the posture and returns a code. It performs no
     * recovery: a violation is reported and the device is left alone. */
    inv = nx_weather_pilot_check(posture);
    if (!nx_weather_pilot_healthy(inv)) {
        if (nx_weather_pilot_record(&s_diag, WX_EV_INVARIANT_VIOLATION, now_us,
                                    cfg, source, state, rec, schedule_due,
                                    inv, &line)) {
            fill_resources(&line);
            fill_evidence(&line, posture);
            emit(&line);
        }
    }

    ev = nx_weather_pilot_classify(source, state, rec, schedule_due);
    if (nx_weather_pilot_should_emit(&s_diag, ev, now_us)) {
        if (nx_weather_pilot_record(&s_diag, ev, now_us, cfg, source, state,
                                    rec, schedule_due, inv, &line)) {
            fill_resources(&line);
            fill_evidence(&line, posture);
            emit(&line);
        }
    }

    /* At most one summary per minute, on top of the edge-triggered events. */
    if (nx_weather_pilot_should_emit(&s_diag, WX_EV_PILOT_SUMMARY, now_us)) {
        if (nx_weather_pilot_record(&s_diag, WX_EV_PILOT_SUMMARY, now_us, cfg,
                                    source, state, rec, schedule_due, inv,
                                    &line)) {
            fill_resources(&line);
            fill_evidence(&line, posture);
            emit(&line);
        }
    }
}

const NxWeatherPilotDiag *nx_weather_pilot_log_state(void)
{
    return &s_diag;
}

/*
 * The boot notice. It gathers from the authorities exactly as the periodic
 * observation does — some facts are structural (decided at compile time by
 * which flags are linked), the rest are read live — and it emits the first
 * bounded line.
 *
 * At boot the mutation facts are necessarily ABSENT: the baseline needs a
 * settled device, so the observation window has not opened yet. That is
 * reported as an unhealthy-because-unobservable pilot, which is the honest
 * answer for the first two minutes and NOT a claim that nothing changed.
 */
void nx_weather_pilot_boot_notice(void)
{
    NxWeatherSourceConfig cfg;
    WeatherRuntimeConfig  runtime_cfg;
    WeatherRuntime        rt;
    WeatherRecommendation rec;
    WeatherRuntimeState   state;
    NxWeatherSourceStatus status;
    NxWeatherPilotPosture posture;
    NxWeatherPilotResources res;

    nx_weather_pilot_log_boot();

    /* The configuration the firmware was BUILT with; repository defaults
     * leave every field unselected. */
    nx_weather_source_from_build_config(&cfg);
    status = nx_weather_source_to_runtime(&cfg, &runtime_cfg);
    /*
     * Gather from the AUTHORITIES. Fields this firmware cannot source are
     * stamped UNAVAILABLE, so the checker below reports an unobservable
     * pilot rather than a healthy one it cannot justify.
     */
    memset(&res, 0, sizeof(res));
    nx_weather_pilot_facts_gather(&cfg, status, &posture, &res);

    memset(&rec, 0, sizeof(rec));
    state = WEATHER_RUNTIME_DISABLED;
    if (nx_weather_source_ready(status) &&
        weather_runtime_init(&rt, &runtime_cfg, NULL) == WEATHER_RUNTIME_OK) {
        /*
         * NO dependencies injected — no clock, no transport, no store — so
         * this step performs ZERO network requests and reports a bounded
         * waiting state. That is a structural fact, not a promise.
         */
        state = weather_runtime_step(&rt, NULL, NULL, &rec);
    }

    /*
     * Cache the posture for the periodic observation. Both inputs are fixed
     * by construction — the configuration is compile-time, and a runtime step
     * with no injected clock, transport or store always reaches the same
     * bounded state — so re-deriving them every second would produce an
     * identical answer at a cost the pilot has no reason to pay.
     */
    s_cfg            = cfg;
    s_status         = status;
    s_state          = state;
    s_rec            = rec;
    s_posture_cached = true;

    nx_weather_pilot_log_step(&cfg, status, state, &rec,
                              false /* schedule not evaluated at boot */,
                              &posture);
}

void nx_weather_pilot_observe(void)
{
    NxWeatherPilotPosture   posture;
    NxWeatherPilotResources res;
    NxBaselinePrereq        pre;

    /* A pilot that never started has nothing to observe. */
    if (!s_booted || !s_posture_cached) {
        return;
    }

    /*
     * Attempt the one-per-boot baseline capture. The 120-second monotonic
     * settle is only ONE of FOURTEEN prerequisites: the configuration must be
     * loaded, the host task must be the expected one, the B5/B6 snapshot must
     * be available with no owner and a normal-source protocol posture, no
     * session or recovery posture and no pending terminal result, the counter
     * snapshot must be readable, unsaturated and still entirely ZERO, and the
     * tuning must be readable. A refusal stores nothing, and a mutation or a
     * violation blocks the baseline permanently for this boot.
     */
    nx_weather_pilot_prereq_gather(&pre, host_task_valid());
    (void)nx_mutation_baseline_capture((uint64_t)esp_timer_get_time(), &pre);

    /* Re-gather from the authorities. Nothing here mutates the subsystems it
     * reads, allocates, touches NVS or issues a network request. */
    memset(&res, 0, sizeof(res));
    nx_weather_pilot_facts_gather(&s_cfg, s_status, &posture, &res);

    nx_weather_pilot_log_step(&s_cfg, s_status, s_state, &s_rec,
                              false /* the schedule gate is not evaluated here */,
                              &posture);
}

#endif /* CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS */
