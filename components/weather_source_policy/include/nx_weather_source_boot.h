#ifndef NX_WEATHER_SOURCE_BOOT_H_
#define NX_WEATHER_SOURCE_BOOT_H_

#include <stdbool.h>

#include "nx_weather_source.h"

/*
 * Gate W5 boot notice. Declared unconditionally; DEFINED only when
 * CONFIG_NX_WEATHER_SOURCE_POLICY is set, so a default or Gate W4-only image
 * links nothing from it.
 *
 * It runs the ordered configuration gate, projects a valid configuration
 * onto the committed Gate W4 runtime, performs ONE bounded step with no
 * injected clock, transport or store, and logs bounded machine tokens.
 *
 * It authorizes nothing: no frequency, voltage, fan or thermal change, no
 * pool or protocol change, no session, no lease, no SNTP start, no NVS
 * namespace, no HTTP route, no task and no restart.
 */
void nx_weather_source_boot_notice(void);

/* Diagnostics for tests: whether the notice ran this boot, and the bounded
 * status it reached. Never exposes a coordinate. */
bool                  nx_weather_source_boot_ran(void);
NxWeatherSourceStatus nx_weather_source_boot_status(void);

#endif /* NX_WEATHER_SOURCE_BOOT_H_ */
