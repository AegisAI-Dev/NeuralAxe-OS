idf_component_register(
SRCS
    "weather_forecast.c"
    "weather_provider.c"
    "weather_open_meteo.c"
    "weather_open_meteo_http.c"
    "weather_retry.c"
    "weather_cache.c"
    "weather_bridge.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "local_schedule"
    "tuning_profile"
    "json"

PRIV_REQUIRES
    "esp_http_client"
    "mbedtls"
)
