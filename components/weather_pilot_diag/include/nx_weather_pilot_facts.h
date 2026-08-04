#ifndef NX_WEATHER_PILOT_FACTS_H_
#define NX_WEATHER_PILOT_FACTS_H_

#include "nx_weather_pilot_diag.h"
#include "nx_mutation_baseline.h"

/*
 * Gate W6 authoritative fact adapter. Declared unconditionally; DEFINED only
 * with CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS.
 *
 * It GATHERS; the pure checker DECIDES. It reads only, allocates nothing,
 * creates no task/timer/queue/endpoint, writes no NVS and mutates nothing —
 * including the subsystems it observes.
 *
 * Every field it produces is stamped with its provenance. A fact this
 * firmware cannot obtain from the subsystem that owns it is stamped
 * UNAVAILABLE, never defaulted to a passing zero, so the checker fails
 * closed instead of reporting a health it cannot justify.
 */

/* Resource observations, separated because they carry no safety meaning. */
typedef struct {
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t stack_high_water;
    uint64_t uptime_us;
} NxWeatherPilotResources;

/*
 * Fill `out` (and `res`, when non-NULL) from the authorities available in
 * THIS image. `out` is fully written on every path and starts fully ABSENT,
 * so a field this function does not explicitly stamp cannot pass as evidence.
 */
void nx_weather_pilot_facts_gather(const NxWeatherSourceConfig *cfg,
                                   NxWeatherSourceStatus source,
                                   NxWeatherPilotPosture *out,
                                   NxWeatherPilotResources *res);

/*
 * Gate W6.1 — gather the BASELINE READINESS prerequisites this component can
 * source, for nx_mutation_baseline_capture().
 *
 * `config_loaded` is evidenced by the fingerprint provider being registered:
 * main installs it immediately after nvs_config_init() returns ESP_OK, so a
 * registered provider means the boot configuration was loaded. The remaining
 * fields come from the committed Gate B6 published runtime snapshot, and are
 * STRUCTURALLY satisfied when CONFIG_NX_TIMED_SESSIONS is not linked (there
 * is no instance, hence no owner, no session and no protocol hold).
 *
 * `host_task_valid` is supplied by the CALLER, because only the caller knows
 * which task it is running on. `out` is fully written on every path and
 * starts entirely false, so a field this function cannot source blocks the
 * baseline rather than passing it.
 */
void nx_weather_pilot_prereq_gather(NxBaselinePrereq *out, bool host_task_valid);

#endif /* NX_WEATHER_PILOT_FACTS_H_ */
