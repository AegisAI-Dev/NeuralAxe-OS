/*
 * NeuralAxe Weather-Aware Tuning — BUILD-TIME PRIVATE SOURCE BINDER
 * (Gate W5). See include/nx_weather_source.h.
 *
 * This is the ONE place a build-time private value becomes a runtime value.
 *
 * The repository's own defaults leave every symbol below unconfigured, so a
 * default build produces the safe posture and links no coordinate. A private
 * pilot supplies the symbols through an out-of-tree sdkconfig fragment
 * (tools/pilot/build_weather_recommendation_pilot.py), which is written
 * outside the repository and shredded after the build — the values never
 * enter Git, a manifest, a log line or a version string.
 *
 * The function is defined unconditionally so callers need no #ifdef; without
 * CONFIG_NX_WEATHER_SOURCE_POLICY it yields the unconfigured defaults and
 * the compiler folds it to a single memset.
 */

#include "sdkconfig.h"
#include "nx_weather_source.h"

void nx_weather_source_from_build_config(NxWeatherSourceConfig *out)
{
    if (out == NULL) {
        return;
    }
    nx_weather_source_defaults(out);

#ifdef CONFIG_NX_WEATHER_SOURCE_POLICY
    /*
     * Every symbol is independent and every one defaults to "not selected".
     * A partially supplied configuration therefore fails validation with a
     * specific reason instead of being completed with a guess — there is no
     * default provider, no default location and no default timezone here.
     */
    out->enabled = true;

#ifdef CONFIG_NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL
    out->distribution = NX_WEATHER_DIST_OWNER_MANAGED_EXTERNAL;
#elif defined(CONFIG_NX_WEATHER_DIST_DISABLED)
    out->distribution = NX_WEATHER_DIST_DISABLED;
#else
    out->distribution = NX_WEATHER_DIST_UNSPECIFIED;
#endif

#ifdef CONFIG_NX_WEATHER_PROVIDER_OPEN_METEO
    out->provider = WEATHER_PROVIDER_OPEN_METEO;
#else
    out->provider = WEATHER_PROVIDER_UNCONFIGURED;
#endif

#ifdef CONFIG_NX_WEATHER_LATITUDE_E4
    out->latitude_e4 = (int32_t)CONFIG_NX_WEATHER_LATITUDE_E4;
#endif
#ifdef CONFIG_NX_WEATHER_LONGITUDE_E4
    out->longitude_e4 = (int32_t)CONFIG_NX_WEATHER_LONGITUDE_E4;
#endif

#ifdef CONFIG_NX_WEATHER_TZ_EUROPE_BRUSSELS
    out->timezone = WEATHER_TZ_EUROPE_BRUSSELS;
#else
    out->timezone = WEATHER_TZ_UNSPECIFIED;
#endif

    /*
     * Not configurable, by design: there is no Kconfig symbol that can turn
     * this off, so no build can request an execution through W5.
     */
    out->recommendation_only = true;
#endif /* CONFIG_NX_WEATHER_SOURCE_POLICY */
}
