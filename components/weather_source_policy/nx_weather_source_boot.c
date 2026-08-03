/*
 * NeuralAxe Weather-Aware Tuning — Gate W5 flag-gated source-policy notice.
 *
 * The whole translation unit is compiled out unless
 * CONFIG_NX_WEATHER_SOURCE_POLICY is set, so neither the default image nor a
 * Gate W4-only image contains a byte of it. It lives in the W5 component
 * rather than in the W4 runtime because W5 depends on W4 — putting it the
 * other way round would create a component cycle.
 *
 * It performs the ORDERED CONFIGURATION GATE and stops there:
 *
 *   1 feature enabled   -> 2 distribution valid -> 3 provider selected
 *   -> 4 location valid -> 5 timezone valid     -> 6 schedule valid
 *   -> 7 recommendation-only
 *
 * and then hands the projected configuration to the committed W4 runtime,
 * which owns steps 8-10 (trusted time, schedule due, fetch, freshness,
 * recommendation). No dependency is injected here — no clock, no transport,
 * no store — so even a fully valid configuration performs ZERO network
 * requests at boot and reports a bounded waiting state. Applying a
 * recommendation to hardware remains a separate future gate that does not
 * exist.
 *
 * Every log line is a bounded machine token. No coordinate, city, hostname,
 * URL, query string or response fragment can appear: the only values printed
 * come from the W5/W4 token functions, which are pure enum-to-string maps.
 */

#include "sdkconfig.h"

#ifdef CONFIG_NX_WEATHER_SOURCE_POLICY

#include <string.h>

#include "esp_log.h"
#include "nx_weather_source.h"
#include "nx_weather_source_boot.h"

static const char *TAG = "nx_wx_src";

static bool                  s_ran;
static NxWeatherSourceStatus s_status = NX_WX_SRC_UNCONFIGURED;

bool nx_weather_source_boot_ran(void)
{
    return s_ran;
}

NxWeatherSourceStatus nx_weather_source_boot_status(void)
{
    return s_status;
}

void nx_weather_source_boot_notice(void)
{
    NxWeatherSourceConfig cfg;
    WeatherRuntimeConfig  runtime_cfg;
    WeatherRuntime        rt;
    WeatherRecommendation rec;
    WeatherRuntimeState   state;
    WeatherRuntimeError   err;

    s_ran = true;

    /* The configuration the firmware was BUILT with. Repository defaults
     * leave every field unselected. */
    nx_weather_source_from_build_config(&cfg);

    /* Steps 1-7. On any failure `runtime_cfg` is left in the W4 safe posture
     * (disabled, provider UNCONFIGURED, location 0/0). */
    s_status = nx_weather_source_to_runtime(&cfg, &runtime_cfg);

    if (!nx_weather_source_ready(s_status)) {
        /* Passive bounded state: no runtime is started, so there is nothing
         * that could evaluate a policy or reach a network. */
        ESP_LOGI(TAG, "WEATHER_SOURCE_NOTICE source=%s runtime=not_started "
                      "requests=0 recommendation=none",
                 nx_weather_source_status_str(s_status));
        ESP_LOGI(TAG, "WEATHER_SOURCE_NOTICE authority=none hardware=unchanged "
                      "pool=unchanged session=none sntp=not_started");
        return;
    }

    /*
     * A valid configuration. NO dependencies are injected — no PoolTimeClock,
     * no transport, no store — which is what makes "zero weather network
     * requests at boot" a structural fact rather than a promise. The step
     * below therefore reports a bounded WAITING_FOR_TRUSTED_TIME.
     */
    err = weather_runtime_init(&rt, &runtime_cfg, NULL);
    if (err != WEATHER_RUNTIME_OK) {
        ESP_LOGE(TAG, "WEATHER_SOURCE_NOTICE source=%s runtime=%s",
                 nx_weather_source_status_str(s_status),
                 weather_runtime_error_str(err));
        return;
    }

    memset(&rec, 0, sizeof(rec));
    state = weather_runtime_step(&rt, NULL, NULL, &rec);

    ESP_LOGI(TAG, "WEATHER_SOURCE_NOTICE source=%s state=%s recommendation=%s "
                  "reason=%s",
             nx_weather_source_status_str(s_status),
             weather_runtime_state_str(state),
             rec.present ? "present" : "none",
             weather_not_executed_str(rec.not_executed));
    ESP_LOGI(TAG, "WEATHER_SOURCE_NOTICE authority=none hardware=unchanged "
                  "pool=unchanged session=none sntp=not_started "
                  "recommendation_only=true");
}

#endif /* CONFIG_NX_WEATHER_SOURCE_POLICY */
