idf_component_register(
SRCS
    "test_nx_weather_pilot_diag.c"

REQUIRES
    "unity"
    "weather_pilot_diag"
    "weather_source_policy"
    "weather_client"
    "weather_runtime"
)
