idf_component_register(
SRCS
    "pool_session_runtime_core.c"
    "pool_session_runtime.c"
    "pool_session_runtime_boot.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "pool_session"
    "pool_session_store"
    "pool_session_recovery"
    "pool_operation_coordinator"
    "pool_time"

PRIV_REQUIRES
    "esp_timer"
    "esp_system"
)
