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
#include "pool_time_source.h"
#include "pool_session_runtime.h"
#include "nx_weather_io.h"
#include "nx_weather_io_worker.h"
#include "weather_transport.h"
#include "weather_open_meteo.h"
#include "local_schedule.h"
#include "brussels_time.h"

static const char *TAG = "nx_wx_pilot";

/* The single RAM-only accumulator. No persistence, ever. */
static NxWeatherPilotDiag s_diag;
static bool               s_booted;

/*
 * The W5 configuration, and THE ONE weather runtime instance.
 *
 * GATE W6.2 — WHY THIS IS NO LONGER CACHED. Until now the pilot stepped a
 * throwaway runtime with NO injected clock and cached the verdict, so
 * `state` and `trusted_time` on every line were compile-time constants:
 * WAITING_FOR_TRUSTED_TIME / false, on every device, forever, no matter what
 * the trusted-time provider did. That is not an observation, and a pilot
 * whose central reading cannot change is not a pilot.
 *
 * The runtime now LIVES across ticks and is stepped for real, so the schedule
 * progress, the climate hysteresis stance and the idempotence witness it
 * carries mean what they say. It is bound once, lazily, because the B6
 * runtime that owns the clock boots AFTER this notice (main.c:150 vs :161).
 *
 * RAM-only. No private value is stored: the config is the same bounded
 * structure the boot notice already held.
 */
static NxWeatherSourceConfig s_cfg;
static NxWeatherSourceStatus s_status;
static WeatherRuntimeConfig  s_runtime_cfg;
static WeatherRuntime        s_rt;
static bool                  s_rt_bound;
static WeatherRuntimeState   s_state;
static WeatherRecommendation s_rec;
static bool                  s_posture_cached;

#ifdef CONFIG_NX_WEATHER_IO_WORKER
/*
 * GATE W6.3 — the ONE bounded async weather I/O machine and its identity.
 *
 * The state machine itself lives inside nx_weather_io_worker.c, which owns the
 * critical section both tasks serialise on; this adapter reaches it only
 * through the nx_weather_io_worker_* accessors, never directly. That is what
 * keeps the statistics task and the worker task from interleaving a half-written
 * request, result or counter.
 *
 * RAM-only and deliberately so: `s_io_generation` restarts at zero on every
 * boot, which is correct for W6.3 and is NOT the W6.4 reboot-deduplication
 * problem. A reboot naturally erases every in-flight and pending item, which is
 * the safe direction — nothing stale can survive a restart.
 */
static uint32_t s_io_generation;
static bool     s_io_started;

/* The observation waiting to be handed to the next step, and the window it
 * belongs to. One slot: the machine upstream already guarantees one result. */
static WeatherObservation s_io_obs;
static bool               s_io_obs_present;
#endif

/*
 * Bind the weather runtime to the PRODUCTION trusted-time authority.
 *
 * It BORROWS the clock and the trust policy the committed Gate B6/B10
 * runtime already owns. It constructs no clock, starts no SNTP, writes no
 * anchor and sets no epoch floor — there is no API here that could — so
 * `trusted(weather)` implies `trusted(B2/B10)` structurally: the only anchor
 * reachable is the one B2 already accepted, judged by B2's own policy.
 *
 * THE RUNTIME TRANSPORT SEAM STAYS NULL — PERMANENTLY, AND ON PURPOSE.
 *
 * The committed W3 adapter (weather_open_meteo_http.c) is a SYNCHRONOUS
 * blocking fetch, and its WEATHER_HTTP_TIMEOUT_MS = 8000 is an esp_http_client
 * SOCKET timeout that applies to each blocking operation — connect/TLS, header
 * fetch, every body read — not to the call as a whole. One fetch can therefore
 * occupy tens of seconds. The only host this pilot is stepped from is the ~1 s
 * statistics task, so wiring the transport HERE would stall that task for dozens
 * of its own periods and run a TLS handshake on its shared stack.
 *
 * Gate W6.3 supplies the missing execution context, and wires the production
 * transport into the WORKER below rather than into this seam. Leaving
 * deps.transport NULL is what keeps weather_runtime_fetch() — the synchronous
 * entry point reachable from this task — structurally incapable of a network
 * call: it checks the transport pointer before anything else. The only path
 * from this device to the provider runs through the single worker task.
 *
 * Returns true once bound. Before the B6 runtime exists the clock is NULL and
 * this simply reports "not yet" — the pilot then waits, untrusted, which is
 * the correct reading rather than a fabricated one.
 */
static bool wx_bind_runtime(const WeatherRuntimeConfig *runtime_cfg)
{
    WeatherRuntimeDeps deps;

    if (s_rt_bound) {
        return true;
    }
    if (runtime_cfg == NULL || !nx_weather_source_ready(s_status)) {
        return false;
    }
    memset(&deps, 0, sizeof(deps));
#ifdef CONFIG_NX_TIMED_SESSIONS
    {
        const PoolSessionRuntime *rt = pool_session_runtime_default_instance();

        deps.clock       = pool_session_runtime_clock(rt);
        deps.time_policy = pool_session_runtime_trust_policy(rt);
    }
#endif
    /* No clock means no trusted time is obtainable, and a runtime bound to
     * nothing would freeze this pilot exactly as the old one did. Wait. */
    if (deps.clock == NULL || deps.time_policy == NULL) {
        return false;
    }
    deps.transport     = NULL;   /* see the contract above */
    deps.transport_ctx = NULL;

    if (weather_runtime_init(&s_rt, runtime_cfg, &deps) != WEATHER_RUNTIME_OK) {
        return false;
    }
    s_rt_bound = true;

#ifdef CONFIG_NX_WEATHER_IO_WORKER
    /*
     * GATE W6.3 — create the ONE weather I/O execution context, now that the
     * runtime is bound and a fetch could become due.
     *
     * This is where the committed W3 production transport is finally wired,
     * and it is wired into the WORKER, never into deps.transport above: the
     * runtime seam stays NULL, so weather_runtime_fetch() remains structurally
     * incapable of a network call from the statistics task. The only path to
     * the transport is through the worker.
     *
     * start() is idempotent and guarded module-wide, so the retry-until-bound
     * loop this function sits in cannot produce a second task.
     */
    if (!s_io_started) {
        s_io_started = nx_weather_io_worker_start(weather_open_meteo_http_ops(),
                                                  NULL);
    }
#endif
    return true;
}

#ifdef CONFIG_NX_WEATHER_IO_WORKER
/*
 * GATE W6.3 — submit one bounded request when, and only when, the committed W3
 * schedule says a window is due and nothing is already outstanding.
 *
 * EVERY refusal here is passive and costs one comparison: no work is queued, no
 * memory is taken and the caller returns immediately. The three conditions that
 * can refuse — no due window, a request already in flight, an unread result —
 * are each reported through their own counter, so a stuck pilot says WHICH kind
 * of stuck it is.
 *
 * It performs NO schedule evaluation of its own: it READS the plan the step
 * just computed. That is what keeps "exactly one scheduler" true.
 */
static void wx_io_submit_if_due(uint64_t now_us)
{
    WeatherSchedulePlan  plan;
    NxWeatherIoRequest   req;
    WeatherTimeView      tv;
    BrusselsLocalTime    local;

    if (!s_io_started) {
        return;   /* no worker: nothing may be submitted                    */
    }
    if (!weather_runtime_last_plan(&s_rt, &plan)) {
        return;
    }
    if (plan.decision != WEATHER_SCHEDULE_DUE &&
        plan.decision != WEATHER_SCHEDULE_CATCH_UP_DUE) {
        return;   /* not due: the overwhelmingly common tick                */
    }
    if (plan.slot_index < 0) {
        return;
    }
    /* Trusted time is required to date the fetch; the same B2/B10 authority
     * the runtime just used, read through the same committed seam. */
    (void)weather_time_view_read(s_rt.deps.clock, s_rt.deps.time_policy,
                                 s_rt.cfg.max_sync_age_s, &tv);
    if (!tv.trusted_time_available) {
        return;
    }
    if (!brussels_local_from_utc(tv.trusted_utc_s, &local)) {
        return;   /* outside the supported band: refuse, never approximate  */
    }

    memset(&req, 0, sizeof(req));
    /* The committed W3 builder owns the allowlisted host/path and the bounded
     * query; this adapter composes no URL and knows no hostname. */
    if (weather_open_meteo_build_request(&s_rt.cfg.location, &req.request) !=
        WEATHER_PROVIDER_OK) {
        return;
    }
    req.generation         = ++s_io_generation;
    req.window.date        = local.date;
    req.window.slot_index  = plan.slot_index;
    req.window.provider    = s_rt.cfg.expected_provider;
    req.parse.expected_date       = local.date;
    req.parse.fetch_epoch_s       = tv.trusted_utc_s;
    req.parse.fetch_epoch_trusted = true;
    req.parse.source_generation   = req.generation;

    /* Serialised submit; on acceptance it also performs the O(1) wake, outside
     * the critical section. Not a queue send: no capacity to exhaust, nothing
     * to copy, and it cannot block the statistics task. */
    if (nx_weather_io_worker_submit(&req, now_us) == WX_IO_SUBMIT_ACCEPTED) {
        /* accepted; the worker has been notified */
    } else {
        /* Refused. Give the generation back so ids stay dense and a later
         * tick can retry cleanly; the machine's own counters recorded why. */
        s_io_generation--;
    }
}
#endif /* CONFIG_NX_WEATHER_IO_WORKER */

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

/*
 * Gate W6.2 — project the committed Gate B10 trusted-time diagnostics onto
 * the line. Called on EVERY emitted line, including the boot notice, because
 * "the provider had not started yet" is exactly as informative as "it is
 * trusted" and an owner needs to see the transition between them.
 *
 * Read-only: it starts no provider, registers no callback and changes no
 * policy. The invariant verdict above was already computed and does not
 * consult any of these fields.
 */
static void fill_time(NxWeatherPilotLine *line)
{
    nx_weather_pilot_time_gather(line);
}

/*
 * Gate W6.3 — project the bounded async weather I/O machine onto the line.
 * Read-only: it submits nothing, claims nothing and starts no worker. The
 * invariant verdict above was already computed and does not consult any of
 * these fields.
 */
static void fill_io(NxWeatherPilotLine *line)
{
#ifdef CONFIG_NX_WEATHER_IO_WORKER
    NxWeatherIoDiag d;

    nx_weather_io_worker_observe((uint64_t)esp_timer_get_time(), &d);
    nx_weather_pilot_io_project(&d, true, nx_weather_io_worker_count(),
                                nx_weather_io_worker_stack_high_water(), line);
#else
    /* No worker in this image: STRUCTURAL, and no value is quoted. */
    nx_weather_pilot_io_project(NULL, false, 0u, 0u, line);
#endif
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
             "mut_session=%u tuning_same=%d "
             "time_fact=%s time_src_cfg=%d time_src=%s time_attempts=%u "
             "time_operational=%d time_available=%d time_age_valid=%d "
             "time_age_s=%u time_last=%s "
             "io_fact=%s io=%s io_ev=%s io_gen=%u io_inflight=%d "
             "io_pending=%d io_sub=%u io_busy=%u io_pend_rej=%u io_ok=%u "
             "io_fail=%u io_to=%u io_disc=%u io_used=%u io_last=%s "
             "io_age_s=%u io_workers=%u io_stack_free=%u",
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
             (int)l->tuning_unchanged,
             nx_weather_fact_state_str(l->time_fact),
             (int)l->time_source_configured,
             pool_time_source_state_str((PoolTimeSourceState)l->time_source_state),
             (unsigned)l->time_sync_attempts,
             (int)l->time_operational, (int)l->time_available,
             (int)l->time_sync_age_valid, (unsigned)l->time_sync_age_s,
             pool_time_error_str((PoolTimeError)l->last_time_sync_result),
             nx_weather_fact_state_str(l->io_fact),
             nx_weather_io_state_str((NxWeatherIoState)l->io_state),
             nx_weather_io_state_str((NxWeatherIoState)l->io_last_event),
             (unsigned)l->io_generation, (int)l->io_in_flight,
             (int)l->io_result_pending,
             (unsigned)l->io_submit_count, (unsigned)l->io_reject_busy_count,
             (unsigned)l->io_reject_pending_count,
             (unsigned)l->io_success_count, (unsigned)l->io_failure_count,
             (unsigned)l->io_timeout_count, (unsigned)l->io_discard_count,
             (unsigned)l->io_consume_count,
             weather_provider_result_str((WeatherProviderResult)l->io_last_result),
             (unsigned)l->io_request_age_s, (unsigned)l->io_worker_count,
             (unsigned)l->io_worker_stack_free);
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
        fill_time(&line);
        fill_io(&line);
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
            fill_time(&line);
            fill_io(&line);
            emit(&line);
        }
    }

    ev = nx_weather_pilot_classify(source, state, rec, schedule_due);
    if (nx_weather_pilot_should_emit(&s_diag, ev, now_us)) {
        if (nx_weather_pilot_record(&s_diag, ev, now_us, cfg, source, state,
                                    rec, schedule_due, inv, &line)) {
            fill_resources(&line);
            fill_evidence(&line, posture);
            fill_time(&line);
            fill_io(&line);
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
            fill_time(&line);
            fill_io(&line);
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

    /* The configuration is needed by the binder below, so publish it first. */
    s_cfg        = cfg;
    s_status     = status;
    s_runtime_cfg = runtime_cfg;

    memset(&rec, 0, sizeof(rec));
    state = WEATHER_RUNTIME_DISABLED;
    /*
     * Try to bind to the production clock now. It will NOT succeed here: this
     * notice runs before nx_timed_sessions_boot_init(), so no B6 runtime
     * exists yet and there is no anchor to borrow. The attempt is made anyway
     * so the boot line reports the real posture rather than a rehearsed one,
     * and the periodic observation retries until the clock appears.
     */
    if (wx_bind_runtime(&s_runtime_cfg)) {
        state = weather_runtime_step(&s_rt, NULL, NULL, &rec);
    } else if (nx_weather_source_ready(status)) {
        /*
         * Configured but not yet bound. Report the bounded WAITING state that
         * an unbound runtime genuinely is, without constructing a throwaway
         * runtime whose answer would then be mistaken for a measurement.
         */
        state = WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME;
        rec.not_executed = WEATHER_NOT_EXECUTED_NO_TRUSTED_TIME;
        rec.time_state   = WEATHER_TIME_PROVIDER_ABSENT;
    }

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

    /*
     * GATE W6.2 — step the runtime FOR REAL.
     *
     * Bind first (a no-op once bound; before the B6 runtime exists it simply
     * reports "not yet"). Then one bounded, non-blocking, allocation-free
     * step: it reads the borrowed anchor, evaluates the committed Brussels
     * schedule when time is trusted, and returns a state that can now
     * actually CHANGE — which is the whole point of the correction.
     *
     * `observation` stays NULL because the transport is NULL, so no fetch is
     * attempted and no network request is possible. `env` stays NULL because
     * the W1 policy inputs belong to the integrator and are not wired yet;
     * the runtime reports that honestly as NO_PROFILE_SELECTED rather than
     * substituting defaults.
     */
    if (wx_bind_runtime(&s_runtime_cfg)) {
        const WeatherObservation *obs = NULL;

#ifdef CONFIG_NX_WEATHER_IO_WORKER
        /*
         * GATE W6.3 — the asynchronous half, in the order that makes each step
         * O(1) and non-blocking. NONE of this performs DNS, TCP, TLS, HTTP,
         * parsing of network data, a retry sleep or a blocking wait. The only
         * blocking operation in the whole design happens on the worker task.
         */
        uint64_t now_us = (uint64_t)esp_timer_get_time();

        /* 1 — enforce the bounded deadline. This is what makes a wedged worker
         *     unable to latch the pilot: the slot is released here, by the
         *     consumer, without touching the worker at all. */
        (void)nx_weather_io_worker_tick(now_us);

        /* 2 — take at most one ready result. Consuming empties the slot, so a
         *     duplicate tick cannot apply the same observation twice. */
        if (!s_io_obs_present) {
            NxWeatherIoResult got;

            if (nx_weather_io_worker_consume(NULL, &got, now_us)) {
                s_io_obs         = got.observation;
                s_io_obs_present = true;
            }
        }
        if (s_io_obs_present) {
            obs = &s_io_obs;
        }
#endif
        memset(&s_rec, 0, sizeof(s_rec));
        s_state = weather_runtime_step(&s_rt, NULL, obs, &s_rec);

#ifdef CONFIG_NX_WEATHER_IO_WORKER
        /* The step has consumed it; release the slot for the next window. */
        if (s_io_obs_present) {
            memset(&s_io_obs, 0, sizeof(s_io_obs));
            s_io_obs_present = false;
        }
        /* 3 — submit, if and only if the runtime says a window is due. */
        wx_io_submit_if_due(now_us);
#endif
    }

    /* Re-gather from the authorities. Nothing here mutates the subsystems it
     * reads, allocates, touches NVS or issues a network request. */
    memset(&res, 0, sizeof(res));
    nx_weather_pilot_facts_gather(&s_cfg, s_status, &posture, &res);

    nx_weather_pilot_log_step(&s_cfg, s_status, s_state, &s_rec,
                              false /* the schedule gate is not evaluated here */,
                              &posture);
}

#endif /* CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS */
