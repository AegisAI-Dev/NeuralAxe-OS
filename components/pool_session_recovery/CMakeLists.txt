idf_component_register(
SRCS
    "pool_session_recovery.c"
    "pool_session_reset.c"
    "pool_session_reset_espidf.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "pool_session"
    "pool_session_store"
    "pool_time"

PRIV_REQUIRES
    "esp_system"
)
