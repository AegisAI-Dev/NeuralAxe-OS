idf_component_register(
SRCS
    "pool_session_preflight.c"
    "pool_session_preflight_nvs.c"
    "pool_session_preflight_boot.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "pool_session"
    "pool_session_store"

PRIV_REQUIRES
    "nvs_flash"
    "esp_timer"
)
