idf_component_register(
SRCS
    "pool_operation_policy.c"
    "pool_operation_coordinator.c"
    "pool_operation_http_policy.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "pool_session"
    "pool_session_store"
    "pool_session_recovery"
    "pool_time"
)
