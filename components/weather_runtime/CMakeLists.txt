idf_component_register(
SRCS
    "weather_time_view.c"
    "weather_runtime.c"
    "weather_runtime_boot.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "pool_time"
    "weather_client"
    "local_schedule"
    "tuning_profile"
)
