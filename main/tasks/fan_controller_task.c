#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "fan_controller_task.h"
#include "nvs_config.h"
#include "thermal.h"
#include "thermal_control.h"
#include "PID.h"

#define EPSILON 0.0001f
#define POLL_TIME_MS 100
#define LOG_TIME_MS 2000

/* Thermal configuration (mode / curve / hysteresis) is refreshed from NVS at
 * this cadence instead of every 100 ms poll: string reads allocate, and the
 * control hot loop must stay allocation-free. Emergency inputs (overheat
 * mode, pause, pool state) are still checked every poll cycle. */
#define CONFIG_REFRESH_CYCLES (1000 / POLL_TIME_MS)

#define PID_P 5.0
#define PID_I 0.1
#define PID_D 2.0

static const char * TAG = "fan_controller";
static const char * prev_context = "";

/* Decision-reason strings (static lifetime; shared with the System API). */
static const char * REASON_EMERGENCY = "emergency thermal override";
static const char * REASON_PAUSED = "mining paused";
static const char * REASON_NO_POOL = "no pool connected";
static const char * REASON_TARGET = "target control active";
static const char * REASON_TARGET_STARTUP = "waiting for valid sensor data";
static const char * REASON_MANUAL = "manual fan active";
static const char * REASON_CURVE_FALLBACK = "curve configuration invalid - safe fallback active";

static void update_fan_speed(GlobalState * GLOBAL_STATE, float target_perc, const char * context)
{
    if (target_perc > 100.0f) target_perc = 100.0f;
    if (target_perc < 0.0f) target_perc = 0.0f;

    bool target_changed = fabs(GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_perc - target_perc) > EPSILON;
    if (strcmp(context, prev_context) != 0) {
        prev_context = context;
        ESP_LOGI(TAG, "Set to %s mode, fan speed: %.1f%%", context, target_perc);
    } else {
        if (target_changed && strcmp(context, "Auto") != 0 && strcmp(context, "Curve") != 0) {
            ESP_LOGI(TAG, "%s mode, fan speed: %.1f%%", context, target_perc);
        }
    }
    if (target_changed) {
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_perc = target_perc;
        if (Thermal_set_fan_percent(&GLOBAL_STATE->DEVICE_CONFIG, target_perc / 100.0f) != ESP_OK) {
            ESP_LOGE(TAG, "FATAL: Fan Control Failed (%s). Flagging hardware fault.", context);
            GLOBAL_STATE->SYSTEM_MODULE.hardware_fault = true;
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg, sizeof(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg), "Fan Control Failed (%s)", context);
        }
    }
}

/* Load the persisted thermal configuration. An empty/unset mode derives from
 * the legacy autofanspeed flag (existing installs keep TARGET or MANUAL
 * semantics; CURVE is strictly opt-in). An invalid stored curve is never
 * used: the board default replaces it, the error is recorded, and the caller
 * falls back to TARGET control. */
static void refresh_thermal_config(ThermalControlMode * mode, ThermalCurve * curve,
                                   bool * curve_invalid, const char ** curve_error)
{
    char * mode_str = nvs_config_get_string(NVS_CONFIG_THERMAL_MODE);
    *mode = thermal_mode_resolve(mode_str, nvs_config_get_bool(NVS_CONFIG_AUTO_FAN_SPEED));
    free(mode_str);

    char * curve_str = nvs_config_get_string(NVS_CONFIG_FAN_CURVE);
    if (curve_str == NULL || curve_str[0] == '\0') {
        thermal_curve_default(curve);
        *curve_invalid = false;
        *curve_error = NULL;
    } else {
        ThermalCurveStatus status = thermal_curve_parse(curve_str, curve);
        if (status == THERMAL_CURVE_OK) {
            *curve_invalid = false;
            *curve_error = NULL;
        } else {
            thermal_curve_default(curve);
            *curve_invalid = true;
            *curve_error = thermal_curve_status_str(status);
        }
    }
    free(curve_str);
}

void FAN_CONTROLLER_task(void * pvParameters)
{
    ESP_LOGI(TAG, "Starting");

    PIDController pid = {0};

    float pid_input = 0;
    float pid_output = 0;
    float pid_setPoint = nvs_config_get_u16(NVS_CONFIG_TEMP_TARGET);
    uint16_t pid_output_min = 0;
    int log_counter = 0;
    float filtered_input = -1.0f;

    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;

    // Initialize PID controller with pid_d_startup and PID_REVERSE directly
    pid_init(&pid, &pid_input, &pid_output, &pid_setPoint, PID_P, PID_I, PID_D, PID_P_ON_E, PID_REVERSE);
    pid_set_sample_time(&pid, POLL_TIME_MS); // Sample time in ms

    // Thermal-control configuration cache (Phase 2H)
    ThermalControlMode thermal_mode = THERMAL_MODE_TARGET;
    ThermalCurve curve;
    bool curve_invalid = false;
    const char * curve_error = NULL;
    ThermalCurveState curve_state;
    thermal_curve_state_init(&curve_state);
    ThermalControlMode prev_thermal_mode = THERMAL_MODE_TARGET;
    int config_refresh_counter = 0;

    refresh_thermal_config(&thermal_mode, &curve, &curve_invalid, &curve_error);
    prev_thermal_mode = thermal_mode;

    // Telemetry defaults until the first decision
    power_management->thermal_control_mode = (uint8_t) thermal_mode;
    power_management->effective_control_temp = -1.0f;
    power_management->requested_fan_perc = 0.0f;
    power_management->active_curve_segment = -1;
    power_management->thermal_emergency_override = false;
    power_management->thermal_hysteresis_holding = false;
    power_management->control_sensor_valid = false;
    power_management->control_sensor = "none";
    power_management->thermal_control_reason = REASON_TARGET_STARTUP;
    power_management->fan_curve_error = curve_error;

    TickType_t taskWakeTime = xTaskGetTickCount();

    while (1) {
        if (--config_refresh_counter <= 0) {
            config_refresh_counter = CONFIG_REFRESH_CYCLES;
            refresh_thermal_config(&thermal_mode, &curve, &curve_invalid, &curve_error);
            if (thermal_mode != prev_thermal_mode) {
                ESP_LOGI(TAG, "Thermal control mode: %s", thermal_mode_to_string(thermal_mode));
                thermal_curve_state_init(&curve_state);
                prev_thermal_mode = thermal_mode;
            }
            power_management->thermal_control_mode = (uint8_t) thermal_mode;
            power_management->fan_curve_error = curve_error;
        }

        // Effective control temperature: max of the valid ASIC temperatures,
        // exactly as the existing TARGET control uses. VRM temperature is
        // deliberately NOT part of normal fan control - it belongs to the
        // hard overheat protection in power_management_task.
        float raw_temp = power_management->chip_temp_avg;
        const char * control_sensor = "asic";
        if (power_management->chip_temp2_avg > raw_temp) {
            raw_temp = power_management->chip_temp2_avg;
            control_sensor = "asic2";
        }
        bool temp_valid = power_management->chip_temp_avg > 0;
        power_management->control_sensor = temp_valid ? control_sensor : "none";
        power_management->control_sensor_valid = temp_valid;

        bool emergency = nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE);
        power_management->thermal_emergency_override = emergency;

        if (emergency) {
            // Hard thermal protection: always authoritative over every mode,
            // configured maximum, hysteresis and ramp behavior.
            power_management->thermal_control_reason = REASON_EMERGENCY;
            power_management->requested_fan_perc = 100.0f;
            power_management->active_curve_segment = -1;
            power_management->thermal_hysteresis_holding = false;
            power_management->effective_control_temp = temp_valid ? raw_temp : -1.0f;
            update_fan_speed(GLOBAL_STATE, 100.0f, "Overheat");
        } else if (GLOBAL_STATE->SYSTEM_MODULE.mining_paused) {
            power_management->thermal_control_reason = REASON_PAUSED;
            power_management->requested_fan_perc = 30.0f;
            power_management->active_curve_segment = -1;
            power_management->thermal_hysteresis_holding = false;
            power_management->effective_control_temp = temp_valid ? raw_temp : -1.0f;
            update_fan_speed(GLOBAL_STATE, 30.0f, "Paused");
        } else if (GLOBAL_STATE->SYSTEM_MODULE.pools_unavailable) {
            power_management->thermal_control_reason = REASON_NO_POOL;
            power_management->requested_fan_perc = 30.0f;
            power_management->active_curve_segment = -1;
            power_management->thermal_hysteresis_holding = false;
            power_management->effective_control_temp = temp_valid ? raw_temp : -1.0f;
            update_fan_speed(GLOBAL_STATE, 30.0f, "No pool");
        } else if (thermal_mode == THERMAL_MODE_CURVE && !curve_invalid) {
            // CURVE control (Phase 2H, opt-in). Same EMA smoothing as the
            // TARGET control input; hysteresis never delays upward moves.
            float control_temp = raw_temp;
            if (temp_valid) {
                if (filtered_input < 0) {
                    filtered_input = raw_temp;
                } else {
                    filtered_input = (0.2f * raw_temp) + (0.8f * filtered_input);
                }
                control_temp = filtered_input;
            }

            uint16_t min_fan = nvs_config_get_u16(NVS_CONFIG_MIN_FAN_SPEED);
            uint16_t hysteresis = nvs_config_get_u16(NVS_CONFIG_FAN_CURVE_HYSTERESIS);
            ThermalCurveDecision decision = thermal_curve_step(&curve, &curve_state,
                                                               control_temp, temp_valid,
                                                               (uint8_t) (min_fan > 100 ? 100 : min_fan),
                                                               (uint8_t) (hysteresis > THERMAL_HYSTERESIS_MAX_C ? THERMAL_HYSTERESIS_MAX_C : hysteresis));

            power_management->effective_control_temp = temp_valid ? control_temp : -1.0f;
            power_management->requested_fan_perc = (float) decision.requested_pct;
            power_management->active_curve_segment = decision.segment;
            power_management->thermal_hysteresis_holding = decision.hysteresis_holding;
            power_management->thermal_control_reason = thermal_curve_reason_str(decision.reason);

            update_fan_speed(GLOBAL_STATE, (float) decision.applied_pct, "Curve");

            log_counter += POLL_TIME_MS;
            if (log_counter >= LOG_TIME_MS) {
                log_counter -= LOG_TIME_MS;
                ESP_LOGI(TAG, "Curve: %.1f°C -> req %u%% applied %u%% (%s)",
                         control_temp, decision.requested_pct, decision.applied_pct,
                         thermal_curve_reason_str(decision.reason));
            }
        } else if (thermal_mode != THERMAL_MODE_MANUAL) {
            // TARGET control: the existing PID behavior, unchanged. Also the
            // safe fallback when a persisted curve fails validation.
            power_management->active_curve_segment = -1;
            power_management->thermal_hysteresis_holding = false;

            // Refresh PID setpoint from NVS in case it was changed via API
            pid_setPoint = nvs_config_get_u16(NVS_CONFIG_TEMP_TARGET);

            uint16_t new_pid_output_min = nvs_config_get_u16(NVS_CONFIG_MIN_FAN_SPEED);
            if (pid_output_min != new_pid_output_min) {
                pid_output_min = new_pid_output_min;
                pid_set_output_limits(&pid, pid_output_min, 100);
            }

            if (power_management->chip_temp_avg > 0) { // Ignore uninitialized or invalid temperature readings
                // Simple EMA filter to reduce jitter from sensor noise
                // alpha = 0.2 means 20% new value, 80% old value
                if (filtered_input < 0) {
                    filtered_input = raw_temp;
                } else {
                    filtered_input = (0.2f * raw_temp) + (0.8f * filtered_input);
                }
                pid_input = filtered_input;

                // Initialize PID on first valid temperature reading
                if (pid_get_mode(&pid) == MANUAL) {
                    pid_set_mode(&pid, AUTOMATIC);
                    ESP_LOGI(TAG, "PID initialized at %.1f°C (P:%.1f I:%.1f D:%.1f", pid_input, pid.dispKp, pid.dispKi, pid.dispKd);
                }

                pid_compute(&pid);

                // Uncomment for debugging PID output directly after compute
                // ESP_LOGD(TAG, "DEBUG: PID raw output: %.2f%%, Input: %.1f, SetPoint: %.1f", pid_output, pid_input, pid_setPoint);

                power_management->effective_control_temp = pid_input;
                power_management->requested_fan_perc = pid_output;
                power_management->thermal_control_reason =
                    (thermal_mode == THERMAL_MODE_CURVE) ? REASON_CURVE_FALLBACK : REASON_TARGET;

                update_fan_speed(GLOBAL_STATE, pid_output, "Auto");

                log_counter += POLL_TIME_MS;
                if (log_counter >= LOG_TIME_MS) {
                    log_counter -= LOG_TIME_MS;
                    ESP_LOGI(TAG, "Temp: %.1f°C, SetPoint: %.1f°C, Output: %.1f%%", pid_input, pid_setPoint, pid_output);
                }
            } else {
                power_management->effective_control_temp = -1.0f;
                power_management->requested_fan_perc = 70.0f;
                power_management->thermal_control_reason = REASON_TARGET_STARTUP;
                update_fan_speed(GLOBAL_STATE, 70.0f, "Startup");
            }
        } else { // Manual fan speed
            uint16_t fan_perc_target = nvs_config_get_u16(NVS_CONFIG_MANUAL_FAN_SPEED);
            power_management->effective_control_temp = temp_valid ? raw_temp : -1.0f;
            power_management->requested_fan_perc = (float) fan_perc_target;
            power_management->active_curve_segment = -1;
            power_management->thermal_hysteresis_holding = false;
            power_management->thermal_control_reason = REASON_MANUAL;
            update_fan_speed(GLOBAL_STATE, (float) fan_perc_target, "Manual");
        }

        power_management->fan_rpm = Thermal_get_fan_speed(&GLOBAL_STATE->DEVICE_CONFIG);
        power_management->fan2_rpm = Thermal_get_fan2_speed(&GLOBAL_STATE->DEVICE_CONFIG);

        vTaskDelayUntil(&taskWakeTime, POLL_TIME_MS / portTICK_PERIOD_MS);
    }
}
