idf_component_register(
SRCS
    "pool_session_api_types.c"
    "pool_session_api_parser.c"
    "pool_session_api_status.c"
    "pool_session_api_heartbeat.c"
    "pool_session_command.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "pool_session"
    "pool_session_store"
    "pool_session_recovery"
    "pool_operation_coordinator"
    "pool_session_runtime"
    "pool_session_execution"
    "pool_time"
    "json"
)
