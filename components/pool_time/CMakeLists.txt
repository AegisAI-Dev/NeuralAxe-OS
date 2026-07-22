idf_component_register(
SRCS
    "pool_time.c"
    "pool_time_sntp.c"

INCLUDE_DIRS
    "include"

PRIV_REQUIRES
    "esp_timer"
    "esp_netif"
    "lwip"
)
