#ifndef NX_WEATHER_SOURCE_H_
#define NX_WEATHER_SOURCE_H_

#include <stdint.h>
#include <stdbool.h>

#include "weather_forecast.h"   /* WeatherProviderId, WeatherTimezoneId */
#include "weather_provider.h"   /* WeatherDistributionMode, eligibility */
#include "local_schedule.h"     /* WeatherScheduleConfig                */
#include "weather_runtime.h"    /* WeatherRuntimeConfig                 */

/*
 * NeuralAxe Weather-Aware Tuning — PRIVATE SOURCE POLICY
 * (Phase 2W, Gate W5). Board 601 / BM1370 only.
 *
 * W5 closes the provider/distribution/location decision deferred from Gate
 * W0. It is a PURE, BOUNDED CONFIGURATION LAYER: it decides whether a
 * weather source may be used at all, and if so which one and where — and it
 * decides nothing else. It performs no I/O, starts no task, reaches no
 * hardware and authorizes no action. Gate W4's recommendation-only contract
 * is unchanged: a validated configuration still only permits a bounded
 * RECOMMENDATION, never an execution.
 *
 * THE THREE POLICY RULES
 *
 *  1. NOTHING IS IMPLICIT. A provider is used only when it was explicitly
 *     selected, under a distribution mode that explicitly permits it, with a
 *     location the owner explicitly supplied. There is no default provider,
 *     no default location and no default timezone. The zero value of every
 *     field is "unconfigured", and unconfigured always fails closed.
 *
 *  2. LOCATION IS NEVER DISCOVERED. This layer accepts coordinates and
 *     nothing else. It has no notion of an IP address, a Wi-Fi SSID, a GPS
 *     fix, a public-IP lookup, a reverse geocode, a city name or a timezone
 *     used as a location proxy — none of those inputs exist in any structure
 *     or function here, so none can be consulted by construction.
 *
 *  3. COORDINATES ARE PRIVATE. They live in exactly one place: the
 *     NxWeatherSourceConfig the integrator holds in RAM. No validation
 *     result, status token, log line, manifest field or diagnostic derived
 *     from this header can carry a coordinate — the outcome type is a small
 *     enum precisely so that a failure can never be reported by echoing the
 *     value that failed.
 *
 * UNITS. Coordinates are signed scaled integers, degrees * 1e4, because that
 * is ALREADY the committed repository-wide unit: the W3 request parameters
 * (WeatherRequestParams.latitude_e4) and the W2 persisted record
 * (TuningSourceRecord.latitude_e4) both use it. Introducing a second unit
 * such as microdegrees would add a lossy conversion at two committed
 * boundaries and contradict the W2 wire schema, so e4 is kept deliberately.
 * 1e-4 degrees is ~11 m — far finer than a weather forecast grid needs, and
 * the owner is free to supply a deliberately coarse value.
 *
 * NO LABEL. This model carries no city, site or location label of any kind.
 * The committed W2 record has a presentation-only `location_label` field;
 * W5 never populates it and never reads it.
 */

/* ------------------------------------------------------------------ */
/* Distribution policy (W5 vocabulary)                                 */
/* ------------------------------------------------------------------ */

/*
 * How this deployment is distributed, which is what decides whether an
 * external weather source may be contacted at all.
 *
 * UNSPECIFIED (0) is the zero default and permits NOTHING — it mirrors the
 * committed W3 `WEATHER_DISTRIBUTION_UNSPECIFIED == 0` rule, so a zeroed or
 * partially initialised structure can never authorize a request.
 *
 * DISABLED is an explicit, auditable "weather sourcing is switched off"
 * statement. It is distinct from UNSPECIFIED (which means "nobody has said")
 * so an operator can record a decision rather than an absence.
 *
 * OWNER_MANAGED_EXTERNAL is the ONLY mode that permits a provider. It
 * asserts that the owner explicitly supplied the source configuration, that
 * NeuralAxe inferred neither the location nor the provider, that the source
 * is not reachable through the unauthenticated timed-session API, and that
 * the owner is responsible for the selected external service and its terms.
 */
typedef enum {
    NX_WEATHER_DIST_UNSPECIFIED = 0,
    NX_WEATHER_DIST_DISABLED,
    NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL,
    NX_WEATHER_DIST__COUNT
} NxWeatherDistribution;

/*
 * Map a W5 distribution policy onto the committed W3 licence gate. Only
 * OWNER_MANAGED_EXTERNAL maps to a mode W3 can accept; everything else maps
 * to the committed UNSPECIFIED, which W3 already refuses. The W3 function
 * remains the single authority on provider eligibility — W5 never bypasses
 * it and never widens it.
 */
WeatherDistributionMode nx_weather_distribution_to_w3(NxWeatherDistribution d);

/* True only when this policy explicitly permits `provider`, as confirmed by
 * the committed W3 eligibility function. Total; fails closed. */
bool nx_weather_distribution_permits(NxWeatherDistribution d,
                                     WeatherProviderId provider);

/* ------------------------------------------------------------------ */
/* The private source configuration                                    */
/* ------------------------------------------------------------------ */

/* Committed W3 coordinate bounds, restated so validation is self-contained. */
#define NX_WEATHER_LAT_E4_MAX  900000   /* +/- 90.0000 degrees  */
#define NX_WEATHER_LON_E4_MAX 1800000   /* +/- 180.0000 degrees */

typedef struct {
    bool                  enabled;            /* owner switched it on      */
    NxWeatherDistribution distribution;
    WeatherProviderId     provider;           /* UNCONFIGURED = not chosen */
    int32_t               latitude_e4;        /* degrees * 1e4             */
    int32_t               longitude_e4;
    WeatherTimezoneId     timezone;           /* UNSPECIFIED = not chosen  */
    WeatherScheduleConfig schedule;           /* committed W3 semantics    */
    /*
     * The pilot contract, carried explicitly so a configuration cannot
     * silently become an execution authorization. W5 REQUIRES this to be
     * true; false is rejected, and there is no field anywhere in this model
     * that could request an execution.
     */
    bool                  recommendation_only;
} NxWeatherSourceConfig;

/* ------------------------------------------------------------------ */
/* Validation outcome (stable tokens; never a coordinate)              */
/* ------------------------------------------------------------------ */

/*
 * Distinguishable failure reasons. The ONLY value that permits a request is
 * READY_RECOMMENDATION_ONLY. Each token names the FIELD CLASS that failed
 * and never the value, so a diagnostic can say "latitude invalid" without
 * ever disclosing where the device is.
 */
typedef enum {
    NX_WX_SRC_UNCONFIGURED = 0,        /* nothing selected (the default)   */
    NX_WX_SRC_INVALID_DISTRIBUTION,
    NX_WX_SRC_INVALID_PROVIDER,
    NX_WX_SRC_INVALID_LATITUDE,
    NX_WX_SRC_INVALID_LONGITUDE,
    NX_WX_SRC_INVALID_TIMEZONE,
    NX_WX_SRC_INVALID_SCHEDULE,
    NX_WX_SRC_INCOMPLETE,              /* partially supplied               */
    NX_WX_SRC_NOT_RECOMMENDATION_ONLY, /* execution was requested: refused */
    NX_WX_SRC_READY_RECOMMENDATION_ONLY,
    NX_WX_SRC__COUNT
} NxWeatherSourceStatus;

/* Zero the configuration to the safe posture: disabled, nothing selected,
 * no location, no timezone, recommendation-only. Total. */
void nx_weather_source_defaults(NxWeatherSourceConfig *out);

/*
 * Validate a private configuration. Deterministic, total, side-effect free
 * and NULL-safe (NULL validates as UNCONFIGURED). Checks, in a fixed order
 * so the reported reason is stable:
 *
 *   enabled -> distribution -> provider -> distribution permits provider
 *           -> latitude -> longitude -> completeness -> timezone
 *           -> schedule -> recommendation-only
 *
 * The zero/zero coordinate pair is REJECTED as the committed W3 "unset"
 * sentinel rather than accepted as a valid point in the Gulf of Guinea, so
 * an unconfigured device can never silently request a real location.
 */
NxWeatherSourceStatus nx_weather_source_validate(const NxWeatherSourceConfig *cfg);

/* Stable machine token for a status. Never contains a coordinate, a city, a
 * hostname or a URL. Total; unknown values map to the unconfigured token. */
const char *nx_weather_source_status_str(NxWeatherSourceStatus s);

/* True only for READY_RECOMMENDATION_ONLY. */
bool nx_weather_source_ready(NxWeatherSourceStatus s);

/*
 * Project a VALIDATED private configuration onto the committed Gate W4
 * runtime configuration. Returns the validation status; `out` is fully
 * written on every path and is left in the W4 safe posture (disabled,
 * provider UNCONFIGURED, location 0/0) unless the status is
 * READY_RECOMMENDATION_ONLY. This is the single seam through which a
 * location can reach the runtime — there is no other writer.
 */
NxWeatherSourceStatus nx_weather_source_to_runtime(const NxWeatherSourceConfig *cfg,
                                                   WeatherRuntimeConfig *out);

/* ------------------------------------------------------------------ */
/* Build-time binder (compiled only with the W5 Kconfig flag)          */
/* ------------------------------------------------------------------ */

/*
 * Materialise the configuration the firmware was BUILT with. The private
 * values arrive only through Kconfig symbols supplied by an out-of-tree
 * sdkconfig fragment (see tools/pilot/build_weather_recommendation_pilot.py);
 * the repository defaults leave every one of them unconfigured. Without
 * CONFIG_NX_WEATHER_SOURCE_POLICY this function still exists and always
 * yields the safe unconfigured posture, so callers need no #ifdef.
 */
void nx_weather_source_from_build_config(NxWeatherSourceConfig *out);

_Static_assert(NX_WEATHER_DIST_UNSPECIFIED == 0,
               "an unspecified distribution must be the zero default");
_Static_assert(NX_WX_SRC_UNCONFIGURED == 0,
               "a zeroed status must not claim readiness");
_Static_assert(NX_WEATHER_DIST__COUNT == 3 && NX_WX_SRC__COUNT == 10,
               "W5 enums changed — review tokens, mapping and tests");

#endif /* NX_WEATHER_SOURCE_H_ */
