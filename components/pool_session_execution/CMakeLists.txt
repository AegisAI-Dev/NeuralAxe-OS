idf_component_register(
SRCS
    "pool_session_execution_core.c"
    "pool_session_execution.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "pool_session"
    "pool_session_store"
    "pool_session_recovery"
    "pool_operation_coordinator"
    "pool_session_runtime"
    "pool_time"
)
