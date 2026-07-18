#ifndef POWER_MANAGEMENT_TASK_H_
#define POWER_MANAGEMENT_TASK_H_

typedef struct
{
    float fan_perc;
    uint16_t fan_rpm;
    uint16_t fan2_rpm;
    float chip_temp_avg;
    float chip_temp2_avg;
    float vr_temp;
    float voltage;
    float frequency_value;
    float actual_frequency;
    float expected_hashrate;
    float power;
    float current;
    float core_voltage;

    // NeuralAxe Phase 2H thermal-control decision telemetry (additive,
    // written by fan_controller_task every control cycle; read-only
    // elsewhere). Strings are static-lifetime constants only.
    uint8_t thermal_control_mode;        // ThermalControlMode driving the decision
    float effective_control_temp;        // temperature feeding the active control path; -1 when invalid
    float requested_fan_perc;            // raw control decision before hysteresis/clamps
    int8_t active_curve_segment;         // curve segment index; -1 outside curve mode
    bool thermal_emergency_override;     // overheat branch is forcing 100%
    bool thermal_hysteresis_holding;     // curve hold is keeping a higher duty
    bool control_sensor_valid;           // the driving temperature is currently valid
    const char * control_sensor;         // "asic" | "asic2" | "none"
    const char * thermal_control_reason; // human-readable decision reason
    const char * fan_curve_error;        // stored-curve diagnostic; NULL when the curve is valid
} PowerManagementModule;

void POWER_MANAGEMENT_init_frequency(void * pvParameters);

void POWER_MANAGEMENT_task(void * pvParameters);

#endif
