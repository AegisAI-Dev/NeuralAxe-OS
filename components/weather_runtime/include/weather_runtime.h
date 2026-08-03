#ifndef WEATHER_RUNTIME_H_
#define WEATHER_RUNTIME_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "weather_time_view.h"
#include "weather_forecast.h"
#include "weather_provider.h"
#include "weather_transport.h"
#include "weather_bridge.h"
#include "local_schedule.h"
#include "brussels_time.h"
#include "tuning_policy.h"
#include "tuning_profile.h"

/*
 * NeuralAxe Weather-Aware Tuning — default-OFF, recommendation-only runtime
 * (Gate W4).
 *
 * WHAT THIS IS
 * ------------
 * A pure, step-driven orchestrator that joins the committed W1 policy, W2
 * persistence model, W3 client/scheduler and the committed B2/B10 trusted
 * time into ONE bounded recommendation. It is the convergence layer, not a
 * new feature: every decision it reports is produced by already-committed
 * code, and every safety refusal it makes is a refusal, never a grant.
 *
 * WHAT THIS IS NOT
 * ----------------
 *  - It is NOT a task. It creates no task, no timer, no queue and no
 *    semaphore; a future integrator steps it from an existing owner task.
 *  - It is NOT a time provider. It never initializes SNTP, never writes an
 *    anchor, never establishes an epoch floor and never reads a wall clock.
 *    Trusted time arrives only through weather_time_view.h.
 *  - It is NOT an executor. The output is a WeatherRecommendation. It holds
 *    no function pointer, no frequency, no voltage, no fan value, no thermal
 *    limit, no pool field and no restart request. There is no code path from
 *    this component to B5 ownership, B7 execution, the B8 API, the ASIC, the
 *    regulator, the fan controller or the Stratum client — provable by the
 *    component's dependency list, which contains none of them.
 *
 * FAIL PASSIVE
 * ------------
 * Every failure (no trusted time, stale trusted time, unconfigured source,
 * transport error, malformed payload, bounded timeout) leaves mining and the
 * owner's settings untouched, produces no recommendation and requests no
 * restart. The runtime has no way to express anything else.
 *
 * PRIVACY
 * -------
 * No field here is a hostname, a URL, a coordinate or a raw response. The
 * recommendation carries bounded enums plus the W1 profile id only.
 */

/*
 * Operational states. The W1-W3 vocabulary is reused wherever it already
 * exists (SOURCE_UNCONFIGURED mirrors WEATHER_PROVIDER_ERR_UNCONFIGURED,
 * WEATHER_STALE mirrors TUNING_FORECAST_STALE / ERR_CACHE_STALE, and
 * WAITING_FOR_TRUSTED_TIME mirrors WEATHER_SCHEDULE_TIME_UNTRUSTED).
 * INTERNAL_ERROR is 0 so a zeroed runtime fails closed.
 */
typedef enum {
    WEATHER_RUNTIME_INTERNAL_ERROR = 0,        /* fail-closed zero         */
    WEATHER_RUNTIME_DISABLED,                  /* flag or config off       */
    WEATHER_RUNTIME_SOURCE_UNCONFIGURED,       /* no provider / no coords  */
    WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME,  /* bounded wait, no policy  */
    WEATHER_RUNTIME_WAITING_FOR_WEATHER,       /* due, awaiting a response */
    WEATHER_RUNTIME_WEATHER_STALE,             /* usable rule refused age  */
    WEATHER_RUNTIME_WEATHER_REJECTED,          /* data refused by W3       */
    WEATHER_RUNTIME_BOUNDED_TIMEOUT,           /* retry budget exhausted   */
    WEATHER_RUNTIME_RECOMMENDATION_READY,      /* one bounded result       */
    WEATHER_RUNTIME_STATE__COUNT
} WeatherRuntimeState;

/* How fresh the weather backing a recommendation was. */
typedef enum {
    WEATHER_FRESHNESS_UNKNOWN = 0,   /* fail-closed zero                   */
    WEATHER_FRESHNESS_FRESH,         /* passed weather_forecast_usable     */
    WEATHER_FRESHNESS_STALE,
    WEATHER_FRESHNESS_INVALID,
    WEATHER_FRESHNESS_UNAVAILABLE,
    WEATHER_FRESHNESS__COUNT
} WeatherFreshnessClass;

/*
 * Why the recommendation was NOT executed. In Gate W4 the answer is always
 * one of these; RECOMMENDATION_ONLY_GATE is the structural one and is
 * returned even for a perfectly actionable result, because W4 grants no
 * execution authority at all.
 */
typedef enum {
    WEATHER_NOT_EXECUTED_INTERNAL_ERROR = 0,   /* fail-closed zero         */
    WEATHER_NOT_EXECUTED_RUNTIME_DISABLED,
    WEATHER_NOT_EXECUTED_SOURCE_UNCONFIGURED,
    WEATHER_NOT_EXECUTED_NO_TRUSTED_TIME,
    WEATHER_NOT_EXECUTED_WEATHER_UNUSABLE,
    WEATHER_NOT_EXECUTED_NOT_DUE,
    WEATHER_NOT_EXECUTED_NO_PROFILE_SELECTED,
    WEATHER_NOT_EXECUTED_RECOMMENDATION_ONLY_GATE,
    WEATHER_NOT_EXECUTED__COUNT
} WeatherNotExecutedReason;

typedef enum {
    WEATHER_RUNTIME_OK = 0,
    WEATHER_RUNTIME_ERR_INVALID_ARGUMENT,
    WEATHER_RUNTIME_ERR_NOT_INITIALIZED,
    WEATHER_RUNTIME_ERR_CONFIG_INVALID,
    WEATHER_RUNTIME_ERR__COUNT
} WeatherRuntimeError;

/*
 * THE bounded recommendation. Deliberately a value type with no pointer of
 * any kind, so it cannot carry or become an executable command.
 */
typedef struct {
    bool present;                      /* a recommendation was produced    */

    /* The committed W1 output, verbatim: an action plus a profile ID.
     * TuningSelectionIntent contains no tuning payload by W1 contract. */
    TuningSelectionIntent intent;

    /* Provenance — enough to judge the result without re-running anything. */
    bool     trusted_time_at_evaluation;
    uint64_t evaluated_utc_s;          /* 0 unless trusted at evaluation   */
    WeatherTimeState time_state;
    WeatherFreshnessClass freshness;
    TuningForecastStatus forecast_status;
    WeatherProviderResult provider_result;

    /* Would a LATER, separately gated execution gate be allowed to act on
     * this? False for every W4 result today: the production registry is
     * entirely UNVALIDATED, so no profile is auto-eligible. */
    bool actionable_in_future_gate;

    /* Why nothing was executed. Never absent. */
    WeatherNotExecutedReason not_executed;

    /* Monotonically increasing witness used to prove idempotence: a
     * duplicate input does not advance it. */
    uint32_t evaluation_serial;
} WeatherRecommendation;

typedef struct {
    bool enabled;                      /* runtime gate, independent of the
                                          Kconfig compile gate            */
    uint32_t max_sync_age_s;           /* trusted-anchor ceiling           */
    uint32_t max_forecast_age_s;       /* forecast usability ceiling       */
    WeatherProviderId expected_provider;
    WeatherRequestParams location;     /* 0/0 means unconfigured           */
    WeatherScheduleConfig schedule;
    uint8_t max_attempts;              /* bounded retry budget per slot    */
} WeatherRuntimeConfig;

/*
 * Injected dependencies. EVERY one may be NULL, and NULL always degrades to
 * a bounded wait — never to a fallback that invents a value.
 *
 * `clock` is a PoolTimeClock the INTEGRATOR owns (in a future supervised
 * pilot, the one the existing B6/B10 runtime already created). This runtime
 * never constructs one, which is what makes "no second SNTP provider" a
 * structural fact rather than a promise.
 */
typedef struct {
    const PoolTimeClock       *clock;
    const PoolTimeTrustPolicy *time_policy;
    const WeatherTransportOps *transport;   /* fake in every W4 test       */
    void                      *transport_ctx;
} WeatherRuntimeDeps;

/*
 * Per-step environment supplied by the integrator: the W1 policy inputs it
 * already owns (thermal, sensors, mining health, current profile, ...) and
 * the W1 registry environment. Weather contributes ONLY the climate request.
 */
typedef struct {
    const TuningPolicyEnvironment *policy_env;
    const TuningPolicyInput       *policy_in;
} WeatherRuntimeStepEnv;

typedef struct {
    WeatherRuntimeConfig cfg;
    WeatherRuntimeDeps   deps;
    bool initialized;

    WeatherRuntimeState     state;
    WeatherScheduleProgress progress;
    bool                    progress_valid;
    TuningClimateState      climate;
    WeatherRecommendation   last;

    uint32_t evaluation_serial;
    uint8_t  attempts_this_slot;
    uint32_t fetch_generation;

    /* Bounded-write bookkeeping. The runtime persists ONLY on a change, so
     * an unchanged recommendation on every tick writes nothing. */
    uint32_t store_write_requests;
    bool     dirty;

    /* Idempotence witness over the (time, weather, schedule) input. */
    uint64_t last_input_digest;
    bool     last_input_digest_valid;
} WeatherRuntime;

/* Bounded observation handed to a step: what the W3 client produced. */
typedef struct {
    WeatherProviderResult provider_result;
    WeatherForecast       forecast;
    bool                  forecast_present;
} WeatherObservation;

/* ------------------------------------------------------------------ */

void weather_runtime_config_defaults(WeatherRuntimeConfig *out);
bool weather_runtime_config_valid(const WeatherRuntimeConfig *c);

WeatherRuntimeError weather_runtime_init(WeatherRuntime *rt,
                                         const WeatherRuntimeConfig *cfg,
                                         const WeatherRuntimeDeps *deps);

/*
 * Fetch one observation through the INJECTED W3 transport seam. Returns
 * ERR_UNCONFIGURED without touching the network when no transport, no
 * provider or no location is configured — the default posture. Pure apart
 * from the injected ops call; performs no DNS, opens no socket and builds no
 * URL of its own (the W3 request builder owns the allowlisted host/path).
 */
WeatherProviderResult weather_runtime_fetch(WeatherRuntime *rt,
                                            const WeatherLocalDate *today,
                                            uint64_t now_utc_s, bool now_trusted,
                                            WeatherObservation *out);

/*
 * ONE bounded step. Never blocks, never sleeps, never mutates hardware.
 * `observation` may be NULL (nothing fetched this step). Always writes *out
 * when out != NULL, and returns the resulting state.
 */
WeatherRuntimeState weather_runtime_step(WeatherRuntime *rt,
                                         const WeatherRuntimeStepEnv *env,
                                         const WeatherObservation *observation,
                                         WeatherRecommendation *out);

/* Stable machine tokens; never a hostname, coordinate or response fragment. */
const char *weather_runtime_state_str(WeatherRuntimeState s);
const char *weather_freshness_str(WeatherFreshnessClass f);
const char *weather_not_executed_str(WeatherNotExecutedReason r);
const char *weather_runtime_error_str(WeatherRuntimeError e);

_Static_assert(WEATHER_RUNTIME_INTERNAL_ERROR == 0,
               "a zeroed runtime must fail closed");
_Static_assert(WEATHER_FRESHNESS_UNKNOWN == 0,
               "a zeroed recommendation must not claim freshness");
_Static_assert(WEATHER_NOT_EXECUTED_INTERNAL_ERROR == 0,
               "a zeroed recommendation must not claim a benign reason");
_Static_assert(WEATHER_RUNTIME_STATE__COUNT == 9 &&
               WEATHER_FRESHNESS__COUNT == 5 &&
               WEATHER_NOT_EXECUTED__COUNT == 8,
               "W4 enums changed — review tokens, diagnostics and tests");

#endif /* WEATHER_RUNTIME_H_ */
