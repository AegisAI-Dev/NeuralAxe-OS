idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       REQUIRES cmock pool_session_preflight pool_session
                                pool_session_store nvs_flash)
