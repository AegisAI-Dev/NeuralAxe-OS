/*
 * NeuralAxe Weather-Aware Tuning — flag-gated boot notice (Gate W4).
 *
 * The whole translation unit is compiled out unless
 * CONFIG_NX_WEATHER_AWARE_TUNING is set, so the default image contains not a
 * single byte of it. The pure runtime files stay free of logging; all of it
 * lives here, exactly as the Gate B10.2 preflight boot adapter does.
 */

#include "sdkconfig.h"

#ifdef CONFIG_NX_WEATHER_AWARE_TUNING

#include <string.h>

#include "esp_log.h"
#include "weather_runtime.h"
#include "weather_runtime_boot.h"

static const char *TAG = "nx_weather";

static bool s_ran;

bool nx_weather_boot_notice_ran(void)
{
    return s_ran;
}

void nx_weather_boot_notice(void)
{
    WeatherRuntimeConfig  cfg;
    WeatherRuntime        rt;
    WeatherRecommendation rec;
    WeatherRuntimeState   state;
    WeatherRuntimeError   err;

    s_ran = true;

    /*
     * The SAFE default posture: runtime disabled, provider UNCONFIGURED,
     * coordinates unset. Nothing here reads a stored configuration, because
     * enabling the feature is a future, separately gated action.
     */
    weather_runtime_config_defaults(&cfg);

    /*
     * NO dependencies are injected: no PoolTimeClock, no transport, no store.
     * That is deliberate and is what makes this notice inert — the runtime
     * has nothing to read and nothing to call.
     */
    err = weather_runtime_init(&rt, &cfg, NULL);
    if (err != WEATHER_RUNTIME_OK) {
        ESP_LOGE(TAG, "WEATHER_BOOT_NOTICE state=%s",
                 weather_runtime_error_str(err));
        return;
    }

    memset(&rec, 0, sizeof(rec));
    state = weather_runtime_step(&rt, NULL, NULL, &rec);

    /*
     * Bounded machine tokens only. No hostname, no URL, no coordinate, no
     * response fragment and no numeric provider error can appear here.
     */
    ESP_LOGI(TAG, "WEATHER_BOOT_NOTICE state=%s recommendation=%s reason=%s",
             weather_runtime_state_str(state),
             rec.present ? "present" : "none",
             weather_not_executed_str(rec.not_executed));
    ESP_LOGI(TAG, "WEATHER_BOOT_NOTICE authority=none hardware=unchanged "
                  "pool=unchanged session=none sntp=not_started");
}

#endif /* CONFIG_NX_WEATHER_AWARE_TUNING */
