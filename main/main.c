#include "esp_event.h"
#include "esp_log.h"
#include "esp_psram.h"

#include "asic_result_task.h"
#include "create_jobs_task.h"
#include "hashrate_monitor_task.h"
#include "fan_controller_task.h"
#include "statistics_task.h"
#include "system.h"
#include "http_server.h"
#include "serial.h"
#include "protocol_coordinator.h"
#include "i2c_bitaxe.h"
#include "adc.h"
#include "nvs_config.h"
#include "self_test.h"
#include "asic.h"
#include "bap/bap.h"
#include "device_config.h"
#include "connect.h"
#include "asic_reset.h"
#include "asic_init.h"
#include "task_monitor.h"
#include "filesystem.h"
#include "input.h"
#include "log_buffer.h"
#include "neuralaxe_identity.h"
#ifdef CONFIG_NX_TIMED_SESSIONS
#include "pool_session_runtime_boot.h"
#endif
#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
#include "nx_execution_glue.h"
#endif

static GlobalState GLOBAL_STATE;

static const char * TAG = "bitaxe";

void app_main(void)
{
    if (esp_psram_is_initialized()) {
        GLOBAL_STATE.psram_is_available = true;
        log_buffer_init();
    }

    ESP_LOGI(TAG, "Welcome to the bitaxe - FOSS || GTFO!");
    ESP_LOGI(TAG, "%s %s (%s) - %s", NEURALAXE_PRODUCT_NAME, NEURALAXE_PRODUCT_VERSION, NEURALAXE_BUILD_CHANNEL, NEURALAXE_VENDOR);
    ESP_LOGI(TAG, "Based on %s %s | Target: %s %s / %s", NEURALAXE_UPSTREAM_PROJECT, NEURALAXE_UPSTREAM_VERSION, NEURALAXE_TARGET_DEVICE, NEURALAXE_TARGET_BOARD, NEURALAXE_TARGET_ASIC);

    if (xTaskCreate(cpu_monitor_task, "cpu_monitor", 4096, (void *)&GLOBAL_STATE, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Error creating cpu monitor task");
    }
#ifdef CONFIG_ENABLE_TASK_MONITOR
    if (xTaskCreate(task_monitor_task, "task_monitor", 8192, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Error creating task monitor task");
    }
#endif
  
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "No PSRAM available on ESP32 device!");
    }

    // Init I2C
    ESP_ERROR_CHECK(i2c_bitaxe_init());
    ESP_LOGI(TAG, "I2C initialized successfully");

    // Initialize RST pin to low early to minimize ASIC power consumption
    ESP_ERROR_CHECK(asic_hold_reset_low());
    ESP_LOGI(TAG, "RST pin initialized to low");

    // wait for I2C to init
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Init ADC
    ADC_init();

    // initialize the ESP32 NVS
    if (nvs_config_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS");
        return;
    }

#ifdef CONFIG_NX_TIMED_SESSIONS
    // NeuralAxe timed pool sessions (Gate B6): the earliest safe point — NVS is
    // initialized and nothing has started Stratum yet. Synchronous: loads the
    // persisted session store, plans boot recovery, bootstraps the single
    // operation coordinator and decides whether protocol startup may proceed.
    // Mutates no pool configuration and touches no Stratum.
    if (!nx_timed_sessions_boot_init()) {
        ESP_LOGW(TAG, "Timed pool session bootstrap incomplete — holding protocol start");
    }
#endif

#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
    // NeuralAxe Gate B7: bind the controlled-execution layer to the booted
    // runtime (adapters + owner-task hook). Performs no pool mutation and no
    // protocol action here; the executor acts only after the system-ready
    // notification below and only in an authorized session posture.
    if (!nx_pool_execution_boot_init(&GLOBAL_STATE)) {
        ESP_LOGW(TAG, "Timed pool session execution layer not bound");
    }
#endif

    // Ensure SSID is initialized before any screen/self-test uses it.
    GLOBAL_STATE.SYSTEM_MODULE.ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID);
    if (GLOBAL_STATE.SYSTEM_MODULE.ssid == NULL) {
        ESP_LOGW(TAG, "No SSID configured in NVS, using empty string");
        GLOBAL_STATE.SYSTEM_MODULE.ssid = strdup("");
        if (GLOBAL_STATE.SYSTEM_MODULE.ssid == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for SSID");
            return;
        }
    }

    if (device_config_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init device config");
        return;
    }

    if (self_test_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init self test");
        return;
    }

    SYSTEM_init_system(&GLOBAL_STATE);
    if (scoreboard_init(&GLOBAL_STATE.SYSTEM_MODULE.scoreboard) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init scoreboard");
    }

    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        wifi_init(&GLOBAL_STATE);
    }

    esp_err_t system_init_ret = SYSTEM_init_peripherals(&GLOBAL_STATE);
    
    if (system_init_ret == ESP_OK) {
        if (xTaskCreate(POWER_MANAGEMENT_task, "power management", 8192, (void *) &GLOBAL_STATE, 10, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Error creating power management task");
        }
        if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
            if (xTaskCreate(FAN_CONTROLLER_task, "fan_controller", 8192, (void *) &GLOBAL_STATE, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Error creating fan controller task");
            }
        }
    } else {
        ESP_LOGE(TAG, "Critical peripheral initialization failure (%s). Entering degraded mode.", esp_err_to_name(GLOBAL_STATE.SELF_TEST_MODULE.system_init_ret));
    }
    
    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        // start the API for AxeOS
        start_rest_server((void *) &GLOBAL_STATE);
    }

    // After mounting SPIFFS
    SYSTEM_init_versions(&GLOBAL_STATE);

    // Initialize BAP interface
    esp_err_t bap_ret = BAP_init(&GLOBAL_STATE);
    if (bap_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BAP interface: %d", bap_ret);
        // Continue anyway, as BAP is not critical for core functionality
    }

    while (!GLOBAL_STATE.SYSTEM_MODULE.is_connected) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

#ifdef CONFIG_NX_TIMED_SESSIONS
    // The single bounded network-ready notification. Performs no networking
    // itself; the runtime task uses it only to decide whether a trusted-time
    // provider may be started when boot recovery requires one.
    nx_timed_sessions_notify_network_ready();
#endif

    queue_init(&GLOBAL_STATE.stratum_queue);

    if (system_init_ret == ESP_OK) {
        if (asic_initialize(&GLOBAL_STATE, ASIC_INIT_COLD_BOOT, 0) == 0) {
            if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
                return;
            }

            self_test_show_message(&GLOBAL_STATE, GLOBAL_STATE.SYSTEM_MODULE.asic_status);
            system_init_ret = ESP_FAIL;
        } else {
            if (xTaskCreate(create_jobs_task, "stratum miner", 8192, (void *) &GLOBAL_STATE, 20, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Error creating stratum miner task");
            }
            if (xTaskCreate(ASIC_result_task, "asic result", 8192, (void *) &GLOBAL_STATE, 15, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Error creating asic result task");
            }

            if (xTaskCreateWithCaps(hashrate_monitor_task, "hashrate monitor", 8192, (void *) &GLOBAL_STATE, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
                ESP_LOGE(TAG, "Error creating hashrate monitor task");
            }
            if (xTaskCreateWithCaps(statistics_task, "statistics", 8192, (void *) &GLOBAL_STATE, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
                ESP_LOGE(TAG, "Error creating statistics task");
            }
        }
    }

    protocol_coordinator_init(&GLOBAL_STATE);

#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
    // The mining runtime (NVS, Wi-Fi, ASIC init, job pipeline, coordinator
    // init) is now available: the Gate B7 executor may begin controlled
    // actions when — and only when — an authorized session posture exists.
    //
    // MUTUAL EXCLUSION WITH SELF-TEST: the self-test owns the device end to
    // end and restarts autonomously when it finishes. Withholding the
    // system-ready signal in that mode keeps the executor permanently in
    // ENTRY_PENDING (it performs NO external action before system-ready), so
    // the two self-test restart paths are unreachable while B7 could own
    // execution.
    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        nx_pool_execution_notify_system_ready();
    } else {
        ESP_LOGW(TAG, "Self-test active — timed-session execution stays idle");
    }
#endif

    bool protocol_start_allowed = true;
#ifdef CONFIG_NX_TIMED_SESSIONS
    // THE protocol-start barrier (Gate B6). protocol_coordinator_task is the only
    // creator of the stratum v1/v2 tasks and the only caller of the pool probes, so
    // withholding it withholds every pool connection. A hold is never released
    // later in B6 — controlled release after live verification or restoration is
    // Gate B7's job.
    protocol_start_allowed = nx_timed_sessions_protocol_start_allowed();
    if (!protocol_start_allowed) {
        ESP_LOGW(TAG, "Protocol start held by timed pool session recovery");
    }
#endif
    if (protocol_start_allowed &&
        xTaskCreate(protocol_coordinator_task, "protocol coord", 8192, (void *) &GLOBAL_STATE, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Error creating protocol coordinator task");
    }

    if (GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        GLOBAL_STATE.SELF_TEST_MODULE.system_init_ret = system_init_ret;
        if (xTaskCreate(self_test_task, "self_test", 8192, (void *) &GLOBAL_STATE, 10, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Error creating self test task");
        }
    }
}
