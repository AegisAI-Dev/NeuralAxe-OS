idf_component_register(
SRCS
    "nx_weather_source.c"
    "nx_weather_source_build.c"
    "nx_weather_source_boot.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "weather_client"
    "local_schedule"
    "weather_runtime"
)
