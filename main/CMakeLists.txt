idf_component_register(
SRCS
    "adc.c"
    "i2c_bitaxe.c"
    "main.c"
    "log_buffer.c"
    "nvs_config.c"
    "nx_execution_glue.c"
    "nx_mutation_adapter.c"
    "display.c"
    "screen.c"
    "input.c"
    "filesystem.c"
    "system.c"
    "work_queue.c"
    "lv_font_portfolio-6x8.c"
    "logo.c"
    "./bap/bap.c"
    "./bap/bap_protocol.c"
    "./bap/bap_uart.c"
    "./bap/bap_handlers.c"
    "./bap/bap_subscription.c"
    "device_config.c"
    "task_monitor.c"
    "./http_server/http_server.c"
    "./http_server/websocket.c"
    "./http_server/websocket_log.c"
    "./http_server/websocket_api.c"
    "./http_server/cjson_utils.c"
    "./http_server/system_api_json.c"
    "./http_server/theme_api.c"
    "./http_server/pool_session_api.c"
    "./http_server/axe-os/api/system/asic_settings.c"
    "./self_test/self_test.c"
    "./tasks/stratum_v1_task.c"
    "./tasks/stratum_v2_task.c"
    "./tasks/protocol_coordinator.c"
    "./tasks/create_jobs_task.c"
    "./tasks/asic_result_task.c"
    "./tasks/power_management_task.c"
    "./tasks/statistics_task.c"
    "./tasks/scoreboard.c"
    "./tasks/hashrate_monitor_task.c"
    "./tasks/fan_controller_task.c"
    "./thermal/EMC2101.c"
    "./thermal/EMC2103.c"
    "./thermal/EMC2302.c"
    "./thermal/TMP1075.c"
    "./thermal/thermal.c"
    "./thermal/PID.c"
    "./power/TPS546.c"
    "./power/DS4432U.c"
    "./power/INA260.c"
    "./power/power.c"
    "./power/vcore.c"
    "./power/asic_reset.c"
    "./power/asic_init.c"

INCLUDE_DIRS
    "."
    "tasks"
    "http_server"
    "http_server/axe-os/api/system"
    "self_test"
    "bap"
    "../components/asic/include"
    "../components/connect/include"
    "../components/dns_server/include"
    "../components/stratum/include"
    "thermal"
    "power"

PRIV_REQUIRES
    "app_update"
    # Gate W4. UNCONDITIONAL on purpose: ESP-IDF resolves component
    # requirements during early expansion, before any CONFIG_* symbol exists,
    # so a config-conditional entry here silently fails to apply. The
    # component contributes nothing to the default image — its only
    # main-facing symbol lives in weather_runtime_boot.c, which is entirely
    # #ifdef'd out unless CONFIG_NX_WEATHER_AWARE_TUNING is set. Posture A is
    # verified byte-identical to the pre-W4 baseline.
    "weather_runtime"
    # Gate W5. Unconditional for the same reason as weather_runtime above:
    # ESP-IDF resolves component requirements before any CONFIG_* symbol
    # exists. Its only main-facing symbol lives in nx_weather_source_boot.c,
    # which is entirely #ifdef'd out unless CONFIG_NX_WEATHER_SOURCE_POLICY
    # is set, so the default image links nothing from it.
    "weather_source_policy"
    # Gate W6, unconditional for the same ESP-IDF reason. Its only
    # main-facing symbol lives in nx_weather_pilot_diag_log.c, which is
    # entirely #ifdef'd out unless
    # CONFIG_NX_WEATHER_RECOMMENDATION_PILOT_DIAGNOSTICS is set.
    "weather_pilot_diag"
    # Gate W6.1 mutation observability. Unconditional for the same ESP-IDF
    # reason; every increment compiles away without
    # CONFIG_NX_MUTATION_OBSERVABILITY, so the default image links nothing.
    "mutation_observability"
    # Gate W6.3T-B: the coherent telemetry safety snapshot. The power and fan
    # producers publish their already-computed facts into it; it acquires no
    # hardware, makes no decision and depends on no policy or weather code.
    "telemetry_safety"
    "driver"
    "thermal_control"
    "esp_adc"
    "esp_app_format"
    "esp_event"
    "esp_http_server"
    "esp_netif"
    "esp_psram"
    "esp_timer"
    "esp_wifi"
    "json"
    "nvs_flash"
    "spiffs"
    "vfs"
    "esp_driver_i2c"
    "tcp_transport"
    "stratum_v2"
    "esp_mm"
    "pool_session_runtime"
    "pool_session_execution"
    "pool_session_api"
    "pool_session_preflight"

EMBED_FILES "http_server/recovery_page.html"
)

idf_build_set_property(COMPILE_OPTIONS "-DLV_CONF_INCLUDE_SIMPLE=1" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DLV_CONF_PATH=\"${CMAKE_SOURCE_DIR}/main/lv_conf.h\"" APPEND)

set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/http_server/axe-os")

if("$ENV{GITHUB_ACTIONS}" STREQUAL "true")
    message(STATUS "Running on GitHub Actions. Web ui will be prebuilt.")

    spiffs_create_partition_image(www ${WEB_SRC_DIR}/dist/axe-os FLASH_IN_PROJECT)
else()
    find_program(NPM_EXECUTABLE npm)
    if(NOT NPM_EXECUTABLE AND NOT EXISTS ${WEB_SRC_DIR}/dist)
        message(FATAL_ERROR "npm is not found! Please install it to proceed.")
    endif()

    # OpenAPI generation - only when openapi.yaml changes
    set(OPENAPI_SPEC "${CMAKE_CURRENT_SOURCE_DIR}/http_server/openapi.yaml")
    set(OPENAPI_STAMP "${CMAKE_BINARY_DIR}/openapi_generate.stamp")

    add_custom_command(
        OUTPUT ${OPENAPI_STAMP}
        COMMAND ${NPM_EXECUTABLE} i
        COMMAND ${NPM_EXECUTABLE} run generate:api
        COMMAND ${CMAKE_COMMAND} -E touch ${OPENAPI_STAMP}
        WORKING_DIRECTORY ${WEB_SRC_DIR}
        DEPENDS ${OPENAPI_SPEC}
        COMMENT "Generating API client from OpenAPI spec (openapi.yaml changed)"
        VERBATIM
        USES_TERMINAL
    )

    # Collect all frontend source files to track dependencies
    file(GLOB_RECURSE WEB_UI_SOURCES
        "${WEB_SRC_DIR}/src/*.*"
    )

    # Also track build configuration files
    list(APPEND WEB_UI_SOURCES
        "${WEB_SRC_DIR}/package.json"
        "${WEB_SRC_DIR}/package-lock.json"
        "${WEB_SRC_DIR}/angular.json"
        "${WEB_SRC_DIR}/tsconfig.json"
        "${WEB_SRC_DIR}/tsconfig.app.json"
    )

    # Use a stamp file to track when the build was last done
    set(WEB_UI_STAMP "${CMAKE_BINARY_DIR}/web_ui_build.stamp")
    # Track version.txt which is generated last and doesn't get deleted
    set(WEB_UI_OUTPUT "${WEB_SRC_DIR}/dist/axe-os/version.txt")

    # Custom command that only runs when source files change
    add_custom_command(
        OUTPUT ${WEB_UI_STAMP} ${WEB_UI_OUTPUT}
        COMMAND ${NPM_EXECUTABLE} i
        COMMAND ${NPM_EXECUTABLE} run build:no-api-gen
        COMMAND ${CMAKE_COMMAND} -E touch ${WEB_UI_STAMP}
        WORKING_DIRECTORY ${WEB_SRC_DIR}
        DEPENDS ${WEB_UI_SOURCES} ${OPENAPI_STAMP}
        COMMENT "Building Web UI (source files changed)"
        VERBATIM
        USES_TERMINAL
    )

    # Create a target for the web UI build
    add_custom_target(web_ui_dist ALL
        DEPENDS ${WEB_UI_STAMP} ${OPENAPI_STAMP}
    )

    # version.txt content depends on GIT STATE, which the source-mtime stamp
    # above cannot see: a commit that touches no frontend source would pack a
    # stale web revision into www.bin while the firmware app_desc picks up the
    # new `git describe` — a mismatched release pair. Refresh version.txt on
    # every build (cheap: node + git describe), ordered after the web build and
    # before the spiffs image so the packed identity always matches the build.
    find_program(NODE_EXECUTABLE node)
    if(NODE_EXECUTABLE)
        add_custom_target(web_ui_version ALL
            COMMAND ${NODE_EXECUTABLE} generate-version.js
            WORKING_DIRECTORY ${WEB_SRC_DIR}
            DEPENDS web_ui_dist
            COMMENT "Refreshing web version.txt from git state"
            VERBATIM
        )
    else()
        message(WARNING "node not found; version.txt may go stale when only git state changes")
        add_custom_target(web_ui_version ALL DEPENDS web_ui_dist)
    endif()

    add_dependencies(${COMPONENT_LIB} web_ui_dist)

    spiffs_create_partition_image(www ${WEB_SRC_DIR}/dist/axe-os FLASH_IN_PROJECT DEPENDS web_ui_version)
endif()
