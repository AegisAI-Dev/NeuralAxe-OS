/*
 * NeuralAxe Weather-Aware Tuning — default-OFF, recommendation-only runtime
 * (Gate W4).
 *
 * PURE apart from the injected transport call. Deliberately absent from this
 * translation unit: <time.h>, esp_timer, SNTP, NVS, sockets, FreeRTOS,
 * logging, heap, and every hardware/pool/protocol/restart header. The
 * component's dependency list contains no path to any of them.
 */

#include <string.h>

#include "weather_runtime.h"
#include "weather_transport.h"
#include "weather_open_meteo.h"

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */

const char *weather_runtime_state_str(WeatherRuntimeState s)
{
    switch (s) {
    case WEATHER_RUNTIME_INTERNAL_ERROR:       return "WX_INTERNAL_ERROR";
    case WEATHER_RUNTIME_DISABLED:             return "WX_DISABLED";
    case WEATHER_RUNTIME_SOURCE_UNCONFIGURED:  return "WX_SOURCE_UNCONFIGURED";
    case WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME:
                                               return "WX_WAITING_FOR_TRUSTED_TIME";
    case WEATHER_RUNTIME_WAITING_FOR_WEATHER:  return "WX_WAITING_FOR_WEATHER";
    case WEATHER_RUNTIME_WEATHER_STALE:        return "WX_WEATHER_STALE";
    case WEATHER_RUNTIME_WEATHER_REJECTED:     return "WX_WEATHER_REJECTED";
    case WEATHER_RUNTIME_BOUNDED_TIMEOUT:      return "WX_BOUNDED_TIMEOUT";
    case WEATHER_RUNTIME_RECOMMENDATION_READY: return "WX_RECOMMENDATION_READY";
    case WEATHER_RUNTIME_STATE__COUNT:
    default:                                   return "WX_INTERNAL_ERROR";
    }
}

const char *weather_freshness_str(WeatherFreshnessClass f)
{
    switch (f) {
    case WEATHER_FRESHNESS_UNKNOWN:     return "WX_FRESH_UNKNOWN";
    case WEATHER_FRESHNESS_FRESH:       return "WX_FRESH";
    case WEATHER_FRESHNESS_STALE:       return "WX_STALE";
    case WEATHER_FRESHNESS_INVALID:     return "WX_INVALID";
    case WEATHER_FRESHNESS_UNAVAILABLE: return "WX_UNAVAILABLE";
    case WEATHER_FRESHNESS__COUNT:
    default:                            return "WX_FRESH_UNKNOWN";
    }
}

const char *weather_not_executed_str(WeatherNotExecutedReason r)
{
    switch (r) {
    case WEATHER_NOT_EXECUTED_INTERNAL_ERROR:    return "WX_NX_INTERNAL_ERROR";
    case WEATHER_NOT_EXECUTED_RUNTIME_DISABLED:  return "WX_NX_RUNTIME_DISABLED";
    case WEATHER_NOT_EXECUTED_SOURCE_UNCONFIGURED:
                                                 return "WX_NX_SOURCE_UNCONFIGURED";
    case WEATHER_NOT_EXECUTED_NO_TRUSTED_TIME:   return "WX_NX_NO_TRUSTED_TIME";
    case WEATHER_NOT_EXECUTED_WEATHER_UNUSABLE:  return "WX_NX_WEATHER_UNUSABLE";
    case WEATHER_NOT_EXECUTED_NOT_DUE:           return "WX_NX_NOT_DUE";
    case WEATHER_NOT_EXECUTED_NO_PROFILE_SELECTED:
                                                 return "WX_NX_NO_PROFILE_SELECTED";
    case WEATHER_NOT_EXECUTED_RECOMMENDATION_ONLY_GATE:
                                                 return "WX_NX_RECOMMENDATION_ONLY_GATE";
    case WEATHER_NOT_EXECUTED__COUNT:
    default:                                     return "WX_NX_INTERNAL_ERROR";
    }
}

const char *weather_runtime_error_str(WeatherRuntimeError e)
{
    switch (e) {
    case WEATHER_RUNTIME_OK:                    return "WX_OK";
    case WEATHER_RUNTIME_ERR_INVALID_ARGUMENT:  return "WX_ERR_INVALID_ARGUMENT";
    case WEATHER_RUNTIME_ERR_NOT_INITIALIZED:   return "WX_ERR_NOT_INITIALIZED";
    case WEATHER_RUNTIME_ERR_CONFIG_INVALID:    return "WX_ERR_CONFIG_INVALID";
    case WEATHER_RUNTIME_ERR__COUNT:
    default:                                    return "WX_ERR_INVALID_ARGUMENT";
    }
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/*
 * Defaults are the SAFE posture and mirror the committed W2 defaults:
 * disabled, provider UNCONFIGURED, coordinates unset. Open-Meteo is never an
 * implicit operational default and no location is ever baked in.
 */
void weather_runtime_config_defaults(WeatherRuntimeConfig *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->enabled            = false;
    out->max_sync_age_s     = WEATHER_TIME_MAX_SYNC_AGE_S_DEFAULT;
    out->max_forecast_age_s = 6u * 3600u;
    out->expected_provider  = WEATHER_PROVIDER_UNCONFIGURED;
    out->location.latitude_e4  = 0;
    out->location.longitude_e4 = 0;
    out->location.timezone     = WEATHER_TZ_EUROPE_BRUSSELS;
    out->max_attempts       = 2u;   /* matches the W3/W2 retry budget */
    weather_schedule_config_defaults(&out->schedule);
}

bool weather_runtime_config_valid(const WeatherRuntimeConfig *c)
{
    if (c == NULL) {
        return false;
    }
    if (!weather_time_max_sync_age_valid(c->max_sync_age_s)) {
        return false;
    }
    if (c->max_forecast_age_s == 0u || c->max_forecast_age_s > 86400u) {
        return false;
    }
    if (c->max_attempts == 0u || c->max_attempts > 8u) {
        return false;
    }
    if ((int)c->expected_provider < 0 ||
        (int)c->expected_provider >= (int)WEATHER_PROVIDER__COUNT) {
        return false;
    }
    return weather_schedule_config_valid(&c->schedule);
}

/* A source is configured only with BOTH a real provider and real coords. */
static bool source_configured(const WeatherRuntimeConfig *c)
{
    if (c->expected_provider == WEATHER_PROVIDER_UNCONFIGURED) {
        return false;
    }
    if (c->location.latitude_e4 == 0 && c->location.longitude_e4 == 0) {
        return false;   /* the W3 "unset 0/0" rule */
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

WeatherRuntimeError weather_runtime_init(WeatherRuntime *rt,
                                         const WeatherRuntimeConfig *cfg,
                                         const WeatherRuntimeDeps *deps)
{
    if (rt == NULL || cfg == NULL) {
        return WEATHER_RUNTIME_ERR_INVALID_ARGUMENT;
    }
    if (!weather_runtime_config_valid(cfg)) {
        memset(rt, 0, sizeof(*rt));
        return WEATHER_RUNTIME_ERR_CONFIG_INVALID;
    }

    memset(rt, 0, sizeof(*rt));
    rt->cfg = *cfg;
    if (deps != NULL) {
        rt->deps = *deps;
    }
    rt->initialized = true;
    rt->state       = cfg->enabled ? WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME
                                   : WEATHER_RUNTIME_DISABLED;
    rt->climate.state = TUNING_WEATHER_STATE_UNKNOWN;
    return WEATHER_RUNTIME_OK;
}

/* ------------------------------------------------------------------ */
/* W3 client seam                                                      */
/* ------------------------------------------------------------------ */

WeatherProviderResult weather_runtime_fetch(WeatherRuntime *rt,
                                            const WeatherLocalDate *today,
                                            uint64_t now_utc_s, bool now_trusted,
                                            WeatherObservation *out)
{
    WeatherRequest       req;
    WeatherParseContext  pctx;
    WeatherHttpResponse *resp;
    WeatherProviderResult r;

    if (out == NULL) {
        return WEATHER_PROVIDER_ERR_INTERNAL;
    }
    memset(out, 0, sizeof(*out));
    weather_forecast_init(&out->forecast);
    out->provider_result = WEATHER_PROVIDER_ERR_UNCONFIGURED;

    if (rt == NULL || !rt->initialized || today == NULL) {
        out->provider_result = WEATHER_PROVIDER_ERR_INTERNAL;
        return out->provider_result;
    }
    /*
     * NO SOURCE => NO NETWORK REQUEST. This is checked before anything else
     * and before any transport pointer is dereferenced.
     */
    if (!rt->cfg.enabled || !source_configured(&rt->cfg) ||
        rt->deps.transport == NULL || rt->deps.transport->fetch == NULL) {
        return out->provider_result;
    }
    /* Untrusted time makes a fetch epoch unverifiable, so it is not fetched. */
    if (!now_trusted) {
        out->provider_result = WEATHER_PROVIDER_ERR_CACHE_STALE;
        return out->provider_result;
    }

    /* The W3 builder owns the allowlisted host/path; W4 composes no URL. */
    r = weather_open_meteo_build_request(&rt->cfg.location, &req);
    if (r != WEATHER_PROVIDER_OK) {
        out->provider_result = r;
        return r;
    }

    /*
     * The bounded response buffer is large; keep it off this call frame by
     * making it static. The runtime is stepped from ONE integrator task by
     * contract (the same single-owner-task doctrine as Gate B6), so there is
     * no concurrent second user of this buffer.
     */
    {
        static WeatherHttpResponse s_resp;
        resp = &s_resp;
        memset(resp, 0, sizeof(*resp));

        r = rt->deps.transport->fetch(rt->deps.transport_ctx, &req, resp);
        if (r != WEATHER_PROVIDER_OK) {
            out->provider_result = r;
            return r;
        }
        r = weather_transport_validate(resp);
        if (r != WEATHER_PROVIDER_OK) {
            out->provider_result = r;
            return r;
        }
        /* The parse context carries the trusted fetch epoch and today's
         * trusted local date, so the W3 parser itself rejects a wrong-date
         * or untrusted-fetch payload. W4 fabricates none of it. */
        rt->fetch_generation++;
        pctx.expected_date      = *today;
        pctx.fetch_epoch_s      = now_utc_s;
        pctx.fetch_epoch_trusted = true;
        pctx.source_generation  = rt->fetch_generation;

        r = weather_open_meteo_parse(resp->body, resp->body_len, &pctx,
                                     &out->forecast);
        /* The raw body is never copied out, never logged and never stored. */
        memset(resp, 0, sizeof(*resp));
    }

    if (r != WEATHER_PROVIDER_OK) {
        weather_forecast_init(&out->forecast);
        out->provider_result = r;
        return r;
    }

    out->forecast_present = true;
    out->provider_result  = WEATHER_PROVIDER_OK;
    return WEATHER_PROVIDER_OK;
}

/* ------------------------------------------------------------------ */
/* Step                                                                */
/* ------------------------------------------------------------------ */

/*
 * Is the selected profile auto-eligible in the integrator's own registry?
 * Uses the committed W1 rule verbatim (structurally valid + VALIDATED + not
 * disabled + hardware-compatible). Against the production registry this is
 * false for every profile, because all three ship UNVALIDATED.
 */
static bool profile_auto_eligible(const TuningPolicyEnvironment *env,
                                  const char *profile_id)
{
    size_t i;

    if (env == NULL || env->profiles == NULL || profile_id == NULL ||
        profile_id[0] == '\0') {
        return false;
    }
    for (i = 0; i < env->profile_count; i++) {
        if (strncmp(env->profiles[i].id, profile_id, TUNING_PROFILE_ID_MAX) == 0) {
            return tuning_profile_auto_eligible(&env->profiles[i], &env->hw) ==
                   TUNING_ELIGIBLE_OK;
        }
    }
    return false;
}

/* FNV-1a over the decision-relevant inputs; a duplicate step must not
 * advance the evaluation serial or request a write. */
static uint64_t input_digest(const WeatherTimeView *tv,
                             const WeatherObservation *obs,
                             const WeatherSchedulePlan *plan)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t *p;
    size_t i;

#define MIX(ptr, len)                                   \
    do {                                                \
        p = (const uint8_t *)(ptr);                     \
        for (i = 0; i < (size_t)(len); i++) {           \
            h ^= (uint64_t)p[i];                        \
            h *= 1099511628211ull;                      \
        }                                               \
    } while (0)

    MIX(&tv->state, sizeof(tv->state));
    MIX(&tv->trusted_utc_s, sizeof(tv->trusted_utc_s));
    MIX(&tv->sync_generation, sizeof(tv->sync_generation));
    MIX(&plan->decision, sizeof(plan->decision));
    MIX(&plan->slot_index, sizeof(plan->slot_index));
    if (obs != NULL) {
        MIX(&obs->provider_result, sizeof(obs->provider_result));
        MIX(&obs->forecast_present, sizeof(obs->forecast_present));
        MIX(&obs->forecast.forecast_max_dc, sizeof(obs->forecast.forecast_max_dc));
        MIX(&obs->forecast.local_date, sizeof(obs->forecast.local_date));
        MIX(&obs->forecast.validated, sizeof(obs->forecast.validated));
    }
#undef MIX
    return h;
}

static WeatherFreshnessClass freshness_of(TuningForecastStatus s)
{
    switch (s) {
    case TUNING_FORECAST_OK:          return WEATHER_FRESHNESS_FRESH;
    case TUNING_FORECAST_STALE:       return WEATHER_FRESHNESS_STALE;
    case TUNING_FORECAST_INVALID:     return WEATHER_FRESHNESS_INVALID;
    case TUNING_FORECAST_UNAVAILABLE: return WEATHER_FRESHNESS_UNAVAILABLE;
    case TUNING_FORECAST__COUNT:
    default:                          return WEATHER_FRESHNESS_UNKNOWN;
    }
}

/* Refuse and publish. Never produces a recommendation. */
static WeatherRuntimeState refuse(WeatherRuntime *rt,
                                  WeatherRuntimeState state,
                                  WeatherNotExecutedReason why,
                                  const WeatherTimeView *tv,
                                  WeatherRecommendation *out)
{
    WeatherRecommendation r;

    memset(&r, 0, sizeof(r));
    r.present                    = false;
    r.trusted_time_at_evaluation = (tv != NULL) && tv->trusted_time_available;
    r.evaluated_utc_s            = 0u;   /* never published without trust */
    r.time_state                 = (tv != NULL) ? tv->state
                                                : WEATHER_TIME_PROVIDER_ABSENT;
    r.freshness                  = WEATHER_FRESHNESS_UNKNOWN;
    r.forecast_status            = TUNING_FORECAST_UNAVAILABLE;
    r.provider_result            = WEATHER_PROVIDER_ERR_UNCONFIGURED;
    r.actionable_in_future_gate  = false;
    r.not_executed               = why;
    r.evaluation_serial          = rt->evaluation_serial;

    rt->state = state;
    rt->last  = r;
    if (out != NULL) {
        *out = r;
    }
    return state;
}

WeatherRuntimeState weather_runtime_step(WeatherRuntime *rt,
                                         const WeatherRuntimeStepEnv *env,
                                         const WeatherObservation *observation,
                                         WeatherRecommendation *out)
{
    WeatherTimeView       tv;
    WeatherSchedulePlan   plan;
    WeatherLocalDate      today;
    TuningForecastInput   fin;
    TuningPolicyInput     pin;
    TuningSelectionIntent intent;
    TuningClimateState    climate_next;
    TuningClimateRequest  climate_req;
    WeatherRecommendation rec;
    uint64_t              digest;
    bool                  duplicate;

    if (rt == NULL) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return WEATHER_RUNTIME_INTERNAL_ERROR;
    }
    if (!rt->initialized) {
        return refuse(rt, WEATHER_RUNTIME_INTERNAL_ERROR,
                      WEATHER_NOT_EXECUTED_INTERNAL_ERROR, NULL, out);
    }

    /* 1 — the runtime gate. Disabled means nothing at all happens. */
    if (!rt->cfg.enabled) {
        return refuse(rt, WEATHER_RUNTIME_DISABLED,
                      WEATHER_NOT_EXECUTED_RUNTIME_DISABLED, NULL, out);
    }

    /* 2 — trusted time, through the ONE seam. Never a wall clock. */
    (void)weather_time_view_read(rt->deps.clock, rt->deps.time_policy,
                                 rt->cfg.max_sync_age_s, &tv);
    if (!tv.trusted_time_available) {
        /* Bounded WAITING state. No policy evaluation, no hardware change,
         * and explicitly NO fallback to raw time. */
        return refuse(rt, WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME,
                      WEATHER_NOT_EXECUTED_NO_TRUSTED_TIME, &tv, out);
    }

    /* 3 — a configured source. Without one, no request is ever made. */
    if (!source_configured(&rt->cfg)) {
        return refuse(rt, WEATHER_RUNTIME_SOURCE_UNCONFIGURED,
                      WEATHER_NOT_EXECUTED_SOURCE_UNCONFIGURED, &tv, out);
    }

    /* 4 — the local date and the W3 schedule. Timezone conversion happens
     * here, OUTSIDE pool_time, in the pure Brussels module. */
    {
        BrusselsLocalTime local;
        if (!brussels_local_from_utc(tv.trusted_utc_s, &local)) {
            /* Outside the supported epoch band: refuse, never approximate. */
            return refuse(rt, WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME,
                          WEATHER_NOT_EXECUTED_NO_TRUSTED_TIME, &tv, out);
        }
        today = local.date;
    }
    if (!rt->progress_valid) {
        weather_schedule_progress_init(&rt->progress, &today);
        rt->progress_valid = true;
    }
    weather_schedule_evaluate(&rt->cfg.schedule, &rt->progress,
                              tv.trusted_utc_s, tv.trusted_time_available, &plan);

    digest    = input_digest(&tv, observation, &plan);
    duplicate = rt->last_input_digest_valid && rt->last_input_digest == digest;
    /*
     * Record the digest HERE, not only on the success path: a repeated
     * FAILURE must be idempotent too, or a stuck input would keep consuming
     * retry budget and keep requesting writes on every wakeup. Every guarded
     * side effect below is conditioned on !duplicate.
     */
    rt->last_input_digest       = digest;
    rt->last_input_digest_valid = true;

    if (plan.decision == WEATHER_SCHEDULE_DAY_ROLLOVER && plan.proposal_present) {
        rt->progress = plan.proposed_progress;
        if (!duplicate) {
            rt->store_write_requests++;   /* a real state change */
        }
    }

    /* 5 — no observation yet, or not due: bounded waiting, never a guess. */
    if (observation == NULL) {
        if (plan.decision == WEATHER_SCHEDULE_DUE ||
            plan.decision == WEATHER_SCHEDULE_CATCH_UP_DUE) {
            if (rt->attempts_this_slot >= rt->cfg.max_attempts) {
                return refuse(rt, WEATHER_RUNTIME_BOUNDED_TIMEOUT,
                              WEATHER_NOT_EXECUTED_WEATHER_UNUSABLE, &tv, out);
            }
            return refuse(rt, WEATHER_RUNTIME_WAITING_FOR_WEATHER,
                          WEATHER_NOT_EXECUTED_WEATHER_UNUSABLE, &tv, out);
        }
        return refuse(rt, WEATHER_RUNTIME_WAITING_FOR_WEATHER,
                      WEATHER_NOT_EXECUTED_NOT_DUE, &tv, out);
    }

    /* 6 — classify the observation through the committed W3 bridge. The
     * bridge is the ONLY path from weather facts into policy, and every
     * failure direction it produces is fail-safe (never an upgrade). */
    weather_bridge_to_policy(observation->forecast_present ? &observation->forecast
                                                           : NULL,
                             observation->provider_result,
                             rt->cfg.expected_provider, &today,
                             tv.trusted_utc_s, tv.trusted_time_available,
                             rt->cfg.max_forecast_age_s, &fin);

    if (fin.status != TUNING_FORECAST_OK && !duplicate) {
        if (rt->attempts_this_slot < 0xFFu) {
            rt->attempts_this_slot++;
        }
    }

    /* 7 — the committed W1 chain: climate hysteresis then precedence. */
    {
        TuningClimateThresholds th;
        tuning_climate_thresholds_defaults(&th);
        climate_next = rt->climate;
        climate_req  = TUNING_CLIMATE_REQ_FAIL_SAFE;   /* fail safe first */
        (void)tuning_climate_step(&rt->climate, &th, &fin,
                                  &climate_next, &climate_req);
    }

    if (env == NULL || env->policy_env == NULL || env->policy_in == NULL) {
        /* Without the integrator's policy inputs no selection can be made.
         * Reported honestly rather than substituted with defaults. */
        rt->climate = climate_next;
        if (fin.status == TUNING_FORECAST_STALE) {
            return refuse(rt, WEATHER_RUNTIME_WEATHER_STALE,
                          WEATHER_NOT_EXECUTED_WEATHER_UNUSABLE, &tv, out);
        }
        if (fin.status != TUNING_FORECAST_OK) {
            return refuse(rt, WEATHER_RUNTIME_WEATHER_REJECTED,
                          WEATHER_NOT_EXECUTED_WEATHER_UNUSABLE, &tv, out);
        }
        return refuse(rt, WEATHER_RUNTIME_WAITING_FOR_WEATHER,
                      WEATHER_NOT_EXECUTED_NO_PROFILE_SELECTED, &tv, out);
    }

    pin = *env->policy_in;
    /* Weather contributes EXACTLY two fields; everything else is the
     * integrator's own already-owned state, passed through untouched. */
    pin.climate_request   = climate_req;
    pin.trusted_time_valid = tv.trusted_time_available;
    pin.now_epoch_s        = tv.trusted_utc_s;

    memset(&intent, 0, sizeof(intent));
    (void)tuning_policy_evaluate(env->policy_env, &pin, &intent);

    rt->climate = climate_next;

    /* Stale weather must never become recommendation-ready. */
    if (fin.status == TUNING_FORECAST_STALE) {
        return refuse(rt, WEATHER_RUNTIME_WEATHER_STALE,
                      WEATHER_NOT_EXECUTED_WEATHER_UNUSABLE, &tv, out);
    }
    if (fin.status != TUNING_FORECAST_OK) {
        return refuse(rt, WEATHER_RUNTIME_WEATHER_REJECTED,
                      WEATHER_NOT_EXECUTED_WEATHER_UNUSABLE, &tv, out);
    }

    /* 8 — a fresh, bounded recommendation. Nothing is applied. */
    if (!duplicate) {
        rt->evaluation_serial++;
        rt->attempts_this_slot = 0u;
        if (plan.proposal_present &&
            (plan.decision == WEATHER_SCHEDULE_DUE ||
             plan.decision == WEATHER_SCHEDULE_CATCH_UP_DUE)) {
            rt->progress = plan.proposed_progress;
            rt->store_write_requests++;   /* commit on transition only */
        }
    }

    memset(&rec, 0, sizeof(rec));
    rec.present                    = true;
    rec.intent                     = intent;
    rec.trusted_time_at_evaluation = true;
    rec.evaluated_utc_s            = tv.trusted_utc_s;
    rec.time_state                 = tv.state;
    rec.freshness                  = freshness_of(fin.status);
    rec.forecast_status            = fin.status;
    rec.provider_result            = observation->provider_result;
    /*
     * "Actionable" means a LATER, separately gated execution gate could act.
     * It requires an actual profile selection whose target is auto-eligible.
     * Against the committed production registry every profile is UNVALIDATED,
     * so this is false for every real device today — by design, not by luck.
     */
    rec.actionable_in_future_gate =
        (intent.action == TUNING_SELECT_PROFILE) &&
        profile_auto_eligible(env->policy_env, intent.profile_id);
    /* W4 grants no execution authority, so this is the structural answer. */
    rec.not_executed = (intent.action == TUNING_SELECT_PROFILE)
                           ? WEATHER_NOT_EXECUTED_RECOMMENDATION_ONLY_GATE
                           : WEATHER_NOT_EXECUTED_NO_PROFILE_SELECTED;
    rec.evaluation_serial = rt->evaluation_serial;

    rt->last                    = rec;
    rt->last_input_digest       = digest;
    rt->last_input_digest_valid = true;
    rt->state                   = WEATHER_RUNTIME_RECOMMENDATION_READY;
    if (out != NULL) {
        *out = rec;
    }
    return rt->state;
}
