idf_component_register(
SRCS
    "test_nx_weather_window.c"
    "test_nx_weather_window_runtime.c"

REQUIRES
    "unity"
    "weather_window"
    "local_schedule"
    "tuning_store"
)
