idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       REQUIRES cmock pool_session_runtime pool_session pool_session_store
                                pool_session_recovery pool_operation_coordinator pool_time
                                nvs_flash)
