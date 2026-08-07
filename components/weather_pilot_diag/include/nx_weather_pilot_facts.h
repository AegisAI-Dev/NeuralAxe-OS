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

/*
 * Gate W6.2 — project the committed Gate B10 sanitized trusted-time
 * diagnostics onto the pilot line.
 *
 * STRICTLY READ-ONLY. It copies the diagnostics the single Gate B6 owner task
 * has already published; it starts no provider, registers no callback, opens
 * no socket, performs no DNS, touches no lock the runtime does not take for
 * itself and changes no trust policy. There is exactly ONE trusted-time
 * provider in the image and this is not it.
 *
 * The fields are stamped like every other pilot fact:
 *   OBSERVED     the runtime published a STRUCTURALLY VALID diagnostics model;
 *   STRUCTURAL   CONFIG_NX_TIMED_SESSIONS is absent, so no provider can exist
 *                anywhere in this image — a property of the link;
 *   UNAVAILABLE  the runtime exists but published nothing usable.
 *
 * `out` is fully written on every path and starts fail-closed, so a field
 * this function cannot source reports UNAVAILABLE rather than a reassuring
 * zero. Nothing here feeds nx_weather_pilot_check(): the invariant verdict
 * does not depend on trusted time and must not begin to.
 */
void nx_weather_pilot_time_gather(NxWeatherPilotLine *out);

#endif /* NX_WEATHER_PILOT_FACTS_H_ */
