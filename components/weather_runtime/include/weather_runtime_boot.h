#ifndef WEATHER_RUNTIME_BOOT_H_
#define WEATHER_RUNTIME_BOOT_H_

#include <stdbool.h>

/*
 * NeuralAxe Weather-Aware Tuning — the ONLY firmware integration point
 * (Gate W4). Compiled ONLY when CONFIG_NX_WEATHER_AWARE_TUNING is set.
 *
 * This is an OBSERVATION-ONLY boot notice, not a runtime driver. It exists so
 * the default-disabled feature has a real, build-observable integration that
 * can be inspected on a bench device without granting the feature any
 * authority whatsoever.
 *
 * On the enabled build it:
 *  - constructs the pure weather runtime with NO injected dependencies (no
 *    trusted-time clock, no transport, no store backend);
 *  - performs exactly ONE bounded step, which therefore always reports
 *    WAITING_FOR_TRUSTED_TIME — the honest state for a device where no
 *    integrator has supplied a clock;
 *  - emits ONE bounded machine-token line and returns.
 *
 * It creates no task, registers no event handler, opens no socket, opens no
 * NVS namespace, starts no SNTP service, reads no wall clock, touches no
 * ASIC/regulator/fan/thermal setting, changes no pool configuration and never
 * restarts. It cannot enable the feature: the runtime config it builds is the
 * safe default (disabled, provider UNCONFIGURED, coordinates unset).
 */
void nx_weather_boot_notice(void);

/* True once the notice has run this boot (diagnostics/tests). */
bool nx_weather_boot_notice_ran(void);

#endif /* WEATHER_RUNTIME_BOOT_H_ */
