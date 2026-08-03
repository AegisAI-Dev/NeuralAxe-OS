idf_component_register(
SRCS
    "test_nx_weather_source.c"
    "test_nx_weather_source_privacy.c"

REQUIRES
    "unity"
    "weather_source_policy"
    "weather_client"
    "weather_runtime"
    "local_schedule"
)
