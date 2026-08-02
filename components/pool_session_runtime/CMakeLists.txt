idf_component_register(
SRCS
    "pool_session_runtime_core.c"
    "pool_session_runtime_pilot.c"
    "pool_session_runtime.c"
    "pool_session_runtime_boot.c"
    "pool_session_runtime_admission.c"

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
    # Gate B10.1: the bounded observation-pilot diagnostics read one Wi-Fi
    # association fact. The requirement is unconditional because ESP-IDF
    # resolves component requirements during early expansion, before any
    # CONFIG_ symbol exists — a config-conditional PRIV_REQUIRES silently
    # evaluates to false and breaks the pilot build.
    #
    # This adds an include path, not code: with
    # CONFIG_NX_TIMED_SESSIONS_TIME_OBSERVE_PILOT_DIAGNOSTICS disabled this
    # component references no esp_wifi symbol, so nothing extra is linked and
    # the shipped default image is unchanged (verified by build size).
    "esp_wifi"
)
