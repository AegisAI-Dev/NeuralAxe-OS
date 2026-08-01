idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       REQUIRES cmock pool_session_execution pool_session_runtime stratum
                                pool_session pool_session_store pool_session_recovery
                                pool_operation_coordinator pool_time nvs_flash)
