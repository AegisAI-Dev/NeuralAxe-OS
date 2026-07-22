idf_component_register(
SRCS
    "pool_session_record.c"
    "pool_session_store.c"
    "pool_session_store_nvs.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "pool_session"

PRIV_REQUIRES
    "nvs_flash"
)
