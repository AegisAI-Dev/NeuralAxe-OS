idf_component_register(
SRCS
    "pool_time.c"
    "pool_time_sntp.c"
    "pool_time_source.c"

INCLUDE_DIRS
    "include"

PRIV_REQUIRES
    "esp_timer"
    "esp_netif"
    "lwip"
)
