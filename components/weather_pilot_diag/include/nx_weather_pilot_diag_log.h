#ifndef NX_WEATHER_PILOT_DIAG_LOG_H_
#define NX_WEATHER_PILOT_DIAG_LOG_H_

#include "nx_weather_pilot_diag.h"

/*
 * Gate W6 pilot logging adapter. Declared unconditionally; DEFINED only when
 * CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS is set, so a default,
 * W4-only or W5-only image links nothing from it.
 *
 * Creates no task, timer, queue or HTTP endpoint; writes no NVS; persists
 * nothing; prints bounded machine tokens only. It observes and reports — it
 * never recovers, restarts, clears state, changes mining or authorizes
 * anything.
 */

/* One-shot boot line stating the pilot posture. Idempotent. */
void nx_weather_pilot_log_boot(void);

/*
 * Observe one runtime step. Emits at most: one violation line (never
 * suppressed), one edge-triggered transition line, and one summary line per
 * 60 s. `posture` is the read-only safety snapshot the integrator samples
 * from the authorities that own those facts.
 */
void nx_weather_pilot_log_step(const NxWeatherSourceConfig *cfg,
                               NxWeatherSourceStatus source,
                               WeatherRuntimeState state,
                               const WeatherRecommendation *rec,
                               bool schedule_due,
                               const NxWeatherPilotPosture *posture);

/* Read-only accumulator access for tests and diagnostics. */
const NxWeatherPilotDiag *nx_weather_pilot_log_state(void);

/*
 * The self-contained pilot boot notice: builds the W5 build-time
 * configuration, runs the ordered gate, performs ONE bounded W4 step with no
 * injected clock, transport or store, and emits the diagnostic lines. Issues
 * zero network requests and authorizes nothing.
 */
void nx_weather_pilot_boot_notice(void);

/*
 * Gate W6.1 — the periodic pilot observation.
 *
 * Called from an EXISTING task; it creates none. Each call attempts the
 * one-per-boot baseline capture on monotonic time, re-gathers the
 * authoritative facts, runs the invariant monitor and emits at most one
 * summary per 60 s plus any violation. It performs no NVS write, no network
 * request and no recovery of any kind. It allocates only the two bounded
 * configuration strings the fingerprint provider reads, and frees both on
 * every path.
 *
 * A no-op until nx_weather_pilot_boot_notice() has run, because a pilot that
 * never started has nothing to observe. The W4 runtime posture is NOT
 * recomputed: with no clock, transport or store injected it is constant by
 * construction, so the boot-time result is reused rather than re-derived.
 */
void nx_weather_pilot_observe(void);

#endif /* NX_WEATHER_PILOT_DIAG_LOG_H_ */
