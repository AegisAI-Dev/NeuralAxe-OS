/*
 * NeuralAxe Weather-Aware Tuning — recommendation-only pilot diagnostics
 * (Gate W6). See include/nx_weather_pilot_diag.h for the contract.
 *
 * Pure and total: no I/O, no allocation, no clock of its own, no logging, no
 * ESP-IDF dependency. It cannot disclose a private value because it never
 * formats one, and it cannot change the device because it holds no authority.
 */

#include <string.h>

#include "nx_weather_pilot_diag.h"

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */

const char *nx_weather_pilot_event_str(NxWeatherPilotEvent e)
{
    switch (e) {
    case WX_EV_NONE:                        return "WX_EV_NONE";
    case WX_EV_PILOT_BOOT:                  return "WX_PILOT_BOOT";
    case WX_EV_CONFIG_UNCONFIGURED:         return "WX_CONFIG_UNCONFIGURED";
    case WX_EV_CONFIG_INVALID:              return "WX_CONFIG_INVALID";
    case WX_EV_CONFIG_READY:                return "WX_CONFIG_READY";
    case WX_EV_WAIT_TRUSTED_TIME:           return "WX_WAIT_TRUSTED_TIME";
    case WX_EV_TRUSTED_TIME_READY:          return "WX_TRUSTED_TIME_READY";
    case WX_EV_WAIT_SCHEDULE:               return "WX_WAIT_SCHEDULE";
    case WX_EV_SCHEDULE_DUE:                return "WX_SCHEDULE_DUE";
    case WX_EV_FETCH_START:                 return "WX_FETCH_START";
    case WX_EV_FETCH_SUCCESS:               return "WX_FETCH_SUCCESS";
    case WX_EV_FETCH_TIMEOUT:               return "WX_FETCH_TIMEOUT";
    case WX_EV_FETCH_REJECTED:              return "WX_FETCH_REJECTED";
    case WX_EV_FORECAST_STALE:              return "WX_FORECAST_STALE";
    case WX_EV_POLICY_EVALUATED:            return "WX_POLICY_EVALUATED";
    case WX_EV_RECOMMENDATION_READY:        return "WX_RECOMMENDATION_READY";
    case WX_EV_RECOMMENDATION_NOT_ACTIONABLE:
                                            return "WX_RECOMMENDATION_NOT_ACTIONABLE";
    case WX_EV_NO_RECOMMENDATION:           return "WX_NO_RECOMMENDATION";
    case WX_EV_PILOT_SUMMARY:               return "WX_PILOT_SUMMARY";
    case WX_EV_INVARIANT_VIOLATION:         return "WX_INVARIANT_VIOLATION";
    case WX_EV__COUNT:
    default:                                return "WX_EV_NONE";
    }
}

const char *nx_weather_pilot_invariant_str(NxWeatherPilotInvariant v)
{
    switch (v) {
    case WX_INV_OK:                    return "WX_INV_OK";
    case WX_INV_WEATHER_DISABLED:      return "WX_INV_WEATHER_DISABLED";
    case WX_INV_SOURCE_POLICY_INVALID: return "WX_INV_SOURCE_POLICY_INVALID";
    case WX_INV_NOT_RECOMMENDATION_ONLY:
                                       return "WX_INV_NOT_RECOMMENDATION_ONLY";
    case WX_INV_EXECUTION_AVAILABLE:   return "WX_INV_EXECUTION_AVAILABLE";
    case WX_INV_COMMAND_API_AVAILABLE: return "WX_INV_COMMAND_API_AVAILABLE";
    case WX_INV_OWNER_PRESENT:         return "WX_INV_OWNER_PRESENT";
    case WX_INV_EXECUTOR_ACTION:       return "WX_INV_EXECUTOR_ACTION";
    case WX_INV_HARDWARE_WRITE:        return "WX_INV_HARDWARE_WRITE";
    case WX_INV_POOL_WRITE:            return "WX_INV_POOL_WRITE";
    case WX_INV_PROTOCOL_WRITE:        return "WX_INV_PROTOCOL_WRITE";
    case WX_INV_RESTART_REQUESTED:     return "WX_INV_RESTART_REQUESTED";
    case WX_INV_OTA_REQUESTED:         return "WX_INV_OTA_REQUESTED";
    case WX_INV_SESSION_MUTATION:      return "WX_INV_SESSION_MUTATION";
    case WX_INV_MINING_BLOCKED:        return "WX_INV_MINING_BLOCKED";
    case WX_INV_TUNING_CHANGED:        return "WX_INV_TUNING_CHANGED";
    case WX_INV_FACT_ABSENT:           return "WX_INV_FACT_ABSENT";
    case WX_INV_FACT_UNAVAILABLE:      return "WX_INV_FACT_UNAVAILABLE";
    case WX_INV_FACT_STALE:            return "WX_INV_FACT_STALE";
    case WX_INV_COUNTER_REGRESSION:    return "WX_INV_COUNTER_REGRESSION";
    case WX_INV__COUNT:
    default:                           return "WX_INV_SOURCE_POLICY_INVALID";
    }
}

/* ------------------------------------------------------------------ */
/* Invariant monitor                                                   */
/* ------------------------------------------------------------------ */

const char *nx_weather_fact_state_str(NxWeatherFactState s)
{
    switch (s) {
    case NX_WX_FACT_ABSENT:      return "WX_FACT_ABSENT";
    case NX_WX_FACT_OBSERVED:    return "WX_FACT_OBSERVED";
    case NX_WX_FACT_STRUCTURAL:  return "WX_FACT_STRUCTURAL";
    case NX_WX_FACT_UNAVAILABLE: return "WX_FACT_UNAVAILABLE";
    case NX_WX_FACT_STALE:       return "WX_FACT_STALE";
    case NX_WX_FACT__COUNT:
    default:                     return "WX_FACT_ABSENT";
    }
}

bool nx_weather_fact_usable(NxWeatherFactState s)
{
    /* Only a fact an authority actually produced counts. STRUCTURAL is
     * stronger than a runtime read: the symbol is provably not in the image. */
    return s == NX_WX_FACT_OBSERVED || s == NX_WX_FACT_STRUCTURAL;
}

bool nx_weather_pilot_counter_sane(uint32_t prev, uint32_t now)
{
    /* A mutation counter may only rise. A drop means a reset, a wrap or a
     * different authority — either way the pilot can no longer prove nothing
     * happened in between, and ambiguity is not a pass. */
    return now >= prev;
}

/* The required provenance set, checked before any value is trusted. */
static NxWeatherPilotInvariant facts_usable(const NxWeatherPilotFacts *f)
{
    const NxWeatherFactState required[] = {
        f->feature_posture, f->execution, f->command_api, f->ownership,
        f->hardware_counters, f->pool_counters, f->restart_counters,
        f->session_counters, f->tuning_snapshot, f->mining_posture,
        f->resources,
    };
    unsigned i;

    /* Report the WORST provenance problem, in a fixed order so a given gap
     * always yields the same code. */
    for (i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if ((unsigned)required[i] >= (unsigned)NX_WX_FACT__COUNT) {
            return WX_INV_FACT_ABSENT;   /* unknown enum fails closed */
        }
    }
    for (i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (required[i] == NX_WX_FACT_ABSENT) {
            return WX_INV_FACT_ABSENT;
        }
    }
    for (i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (required[i] == NX_WX_FACT_UNAVAILABLE) {
            return WX_INV_FACT_UNAVAILABLE;
        }
    }
    for (i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (required[i] == NX_WX_FACT_STALE) {
            return WX_INV_FACT_STALE;
        }
    }
    for (i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (!nx_weather_fact_usable(required[i])) {
            return WX_INV_FACT_ABSENT;
        }
    }
    return WX_INV_OK;
}

NxWeatherPilotInvariant nx_weather_pilot_check(const NxWeatherPilotPosture *p)
{
    NxWeatherPilotInvariant provenance;

    if (p == NULL) {
        /* An absent posture is not a healthy one. */
        return WX_INV_FACT_ABSENT;
    }

    /*
     * PROVENANCE BEFORE VALUES. A zeroed structure has every fact ABSENT, so
     * it reports WX_INV_FACT_ABSENT rather than sailing through on a set of
     * convenient false/zero defaults. This is what makes "no required fact
     * may silently default" a property of the type rather than a promise.
     */
    provenance = facts_usable(&p->facts);
    if (provenance != WX_INV_OK) {
        return provenance;
    }

    /* Feature posture first: a pilot that is not actually a weather pilot
     * cannot be a healthy weather pilot. */
    if (!p->weather_enabled)        return WX_INV_WEATHER_DISABLED;
    if (!p->source_policy_valid)    return WX_INV_SOURCE_POLICY_INVALID;
    if (!p->recommendation_only)    return WX_INV_NOT_RECOMMENDATION_ONLY;

    /* Surfaces that must remain unavailable. */
    if (p->execution_available)     return WX_INV_EXECUTION_AVAILABLE;
    if (p->command_api_available)   return WX_INV_COMMAND_API_AVAILABLE;
    if (p->b5_owner_present)        return WX_INV_OWNER_PRESENT;
    if (p->b7_action_observed)      return WX_INV_EXECUTOR_ACTION;

    /*
     * Before comparing any count: if a counter regressed or saturated, the
     * history between the baseline and now cannot be reconstructed, and a
     * small delta computed across that gap would be a false reassurance
     * rather than evidence.
     */
    if (p->counter_history_lost)    return WX_INV_COUNTER_REGRESSION;

    /* Mutation counters: every one must be exactly zero. */
    if (p->hardware_write_count != 0u)   return WX_INV_HARDWARE_WRITE;
    if (p->pool_write_count != 0u)       return WX_INV_POOL_WRITE;
    if (p->protocol_write_count != 0u)   return WX_INV_PROTOCOL_WRITE;
    if (p->restart_request_count != 0u)  return WX_INV_RESTART_REQUESTED;
    if (p->ota_request_count != 0u)      return WX_INV_OTA_REQUESTED;
    if (p->session_mutation_count != 0u) return WX_INV_SESSION_MUTATION;

    /* Preserved operating posture. */
    if (!p->source_mining_allowed)  return WX_INV_MINING_BLOCKED;
    if (!p->tuning_unchanged)       return WX_INV_TUNING_CHANGED;

    return WX_INV_OK;
}

bool nx_weather_pilot_healthy(NxWeatherPilotInvariant v)
{
    return v == WX_INV_OK;
}

/* ------------------------------------------------------------------ */
/* Classification                                                      */
/* ------------------------------------------------------------------ */

NxWeatherPilotEvent nx_weather_pilot_classify(NxWeatherSourceStatus source,
                                              WeatherRuntimeState state,
                                              const WeatherRecommendation *rec,
                                              bool schedule_due)
{
    /* Configuration first, in the same order the runtime gate uses. */
    if (source == NX_WX_SRC_UNCONFIGURED) {
        return WX_EV_CONFIG_UNCONFIGURED;
    }
    if (!nx_weather_source_ready(source)) {
        return WX_EV_CONFIG_INVALID;
    }

    /* Then the runtime position. */
    switch (state) {
    case WEATHER_RUNTIME_DISABLED:
        return WX_EV_CONFIG_INVALID;
    case WEATHER_RUNTIME_SOURCE_UNCONFIGURED:
        return WX_EV_CONFIG_UNCONFIGURED;
    case WEATHER_RUNTIME_WAITING_FOR_TRUSTED_TIME:
        return WX_EV_WAIT_TRUSTED_TIME;
    case WEATHER_RUNTIME_BOUNDED_TIMEOUT:
        return WX_EV_FETCH_TIMEOUT;
    case WEATHER_RUNTIME_WEATHER_REJECTED:
        return WX_EV_FETCH_REJECTED;
    case WEATHER_RUNTIME_WEATHER_STALE:
        return WX_EV_FORECAST_STALE;
    case WEATHER_RUNTIME_WAITING_FOR_WEATHER:
        /* Trusted time exists; the distinction is whether the committed
         * schedule has opened the window yet. */
        return schedule_due ? WX_EV_SCHEDULE_DUE : WX_EV_WAIT_SCHEDULE;
    case WEATHER_RUNTIME_RECOMMENDATION_READY:
        if (rec != NULL && rec->present) {
            return rec->actionable_in_future_gate
                       ? WX_EV_RECOMMENDATION_READY
                       : WX_EV_RECOMMENDATION_NOT_ACTIONABLE;
        }
        return WX_EV_NO_RECOMMENDATION;
    default:
        /* Unknown/error states fail closed to "nothing was produced". */
        return WX_EV_NO_RECOMMENDATION;
    }
}

/* ------------------------------------------------------------------ */
/* Bounded emission                                                    */
/* ------------------------------------------------------------------ */

void nx_weather_pilot_diag_init(NxWeatherPilotDiag *d)
{
    if (d == NULL) {
        return;
    }
    memset(d, 0, sizeof(*d));
    d->initialized     = true;
    d->last_event      = WX_EV_NONE;
    d->first_violation = WX_INV_OK;
}

void nx_weather_pilot_time_project(const PoolTimeSourceDiagnostics *d,
                                   bool runtime_linked,
                                   NxWeatherPilotLine *out)
{
    if (out == NULL) {
        return;
    }
    /*
     * Fail closed FIRST, so every early return below leaves an unreadable
     * projection rather than a passing zero. TIME_SOURCE_UNCONFIGURED and
     * TIME_ERR_NOT_INITIALIZED are the honest "nothing was read" tokens.
     */
    out->time_fact              = NX_WX_FACT_UNAVAILABLE;
    out->time_source_configured = false;
    out->time_source_state      = (uint8_t)TIME_SOURCE_UNCONFIGURED;
    out->time_sync_attempts     = 0u;
    out->time_operational       = false;
    out->time_available         = false;
    out->time_sync_age_valid    = false;
    out->time_sync_age_s        = 0u;
    out->last_time_sync_result  = (uint8_t)TIME_ERR_NOT_INITIALIZED;

    if (!runtime_linked) {
        /* No runtime instance exists in this image, so no trusted-time
         * provider can exist. A property of the LINK, not a reading — which
         * is stronger evidence than any value could be. */
        out->time_fact = NX_WX_FACT_STRUCTURAL;
        return;
    }
    if (!pool_time_source_diagnostics_valid(d)) {
        /* The runtime exists but published nothing this pilot may quote.
         * Say exactly that; do not invent a state on its behalf. */
        return;
    }

    out->time_fact              = NX_WX_FACT_OBSERVED;
    out->time_source_configured = d->source_configured;
    out->time_source_state      = (uint8_t)d->state;
    out->time_sync_attempts     = d->sync_attempt_count;
    out->time_operational       = d->trusted_time_operational;
    out->time_available         = d->trusted_time_available;
    out->time_sync_age_valid    = d->sync_age_valid;
    /* B10 already saturates the age; restating the gate here makes the bound
     * a property of the projection too, not one it merely inherits. */
    out->time_sync_age_s        = d->sync_age_valid ? d->sync_age_s : 0u;
    out->last_time_sync_result  = (uint8_t)d->last_sync_result;
}

bool nx_weather_pilot_should_emit(const NxWeatherPilotDiag *d,
                                  NxWeatherPilotEvent e, uint64_t now_us)
{
    if (d == NULL || !d->initialized) {
        return false;
    }
    if ((unsigned)e == 0u || (unsigned)e >= (unsigned)WX_EV__COUNT) {
        return false;
    }
    /* A violation is never suppressed, never rate limited and never
     * deduplicated: every occurrence is reported. */
    if (e == WX_EV_INVARIANT_VIOLATION) {
        return true;
    }
    if (e == WX_EV_PILOT_SUMMARY) {
        if (!d->summary_ever) {
            return true;
        }
        /* Monotonic spacing. A backwards clock cannot open the gate: the
         * unsigned difference only exceeds the interval going forward. */
        return (now_us - d->last_summary_us) >= NX_WX_PILOT_SUMMARY_MIN_INTERVAL_US;
    }
    /* Everything else is EDGE TRIGGERED: entering a condition logs once, so
     * an hours-long wait produces one line rather than thousands. */
    return e != d->last_event;
}

bool nx_weather_pilot_record(NxWeatherPilotDiag *d,
                             NxWeatherPilotEvent e,
                             uint64_t now_us,
                             const NxWeatherSourceConfig *cfg,
                             NxWeatherSourceStatus source,
                             WeatherRuntimeState state,
                             const WeatherRecommendation *rec,
                             bool schedule_due,
                             NxWeatherPilotInvariant invariant,
                             NxWeatherPilotLine *out)
{
    if (d == NULL || !d->initialized || out == NULL) {
        return false;
    }
    if ((unsigned)e == 0u || (unsigned)e >= (unsigned)WX_EV__COUNT) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (e == WX_EV_FETCH_START) {
        d->fetch_attempts++;
    }
    if (e == WX_EV_RECOMMENDATION_READY ||
        e == WX_EV_RECOMMENDATION_NOT_ACTIONABLE) {
        d->recommendations++;
    }
    if (e == WX_EV_INVARIANT_VIOLATION) {
        d->violations++;
        if (d->first_violation == WX_INV_OK) {
            d->first_violation = invariant;
        }
    }
    if (e == WX_EV_PILOT_SUMMARY) {
        d->last_summary_us = now_us;
        d->summary_ever    = true;
    } else {
        d->last_event = e;
    }
    d->sequence++;

    out->sequence = d->sequence;
    out->event    = e;

    /*
     * Configuration is reported as SHAPE ONLY. `cfg` is read to answer
     * "was a provider chosen?", never "which coordinates?" — no coordinate,
     * timezone identity or label is copied into the line, and the line type
     * has no field that could hold one.
     */
    if (cfg != NULL) {
        out->provider_configured  = cfg->provider != WEATHER_PROVIDER_UNCONFIGURED;
        out->location_configured  = cfg->latitude_e4 != 0 && cfg->longitude_e4 != 0;
        out->timezone_configured  = cfg->timezone != WEATHER_TZ_UNSPECIFIED;
        out->recommendation_only  = cfg->recommendation_only;
    }
    out->source_status = source;
    out->runtime_state = state;
    out->schedule_due  = schedule_due;

    if (rec != NULL) {
        out->freshness                 = rec->freshness;
        out->provider_result           = rec->provider_result;
        out->not_executed              = rec->not_executed;
        out->trusted_time_available    = rec->trusted_time_at_evaluation;
        out->recommendation_present    = rec->present;
        out->actionable_in_future_gate = rec->actionable_in_future_gate;
        out->policy_evaluated          = rec->present ||
                                         rec->not_executed == WEATHER_NOT_EXECUTED_NOT_DUE;
    }

    /*
     * THE LOAD-BEARING ASSERTION. There is no code path in Gate W6 — or in
     * W4/W5 — that can set this true, and no field anywhere that could
     * request an execution. It is written as a constant so the pilot's
     * central claim is visible on every single diagnostic line.
     */
    out->executed = false;

    out->fetch_attempts  = d->fetch_attempts;
    out->recommendations = d->recommendations;
    out->invariant       = invariant;
    out->uptime_us       = now_us;
    return true;
}
