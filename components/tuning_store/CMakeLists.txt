idf_component_register(
SRCS
    "tuning_record.c"
    "tuning_store.c"
    "tuning_store_nvs.c"
    "tuning_recovery.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "tuning_profile"

PRIV_REQUIRES
    "nvs_flash"
)
