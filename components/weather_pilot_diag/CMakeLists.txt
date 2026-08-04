idf_component_register(
SRCS
    "nx_weather_pilot_diag.c"
    "nx_weather_pilot_diag_log.c"
    "nx_weather_pilot_facts.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "weather_source_policy"
    "weather_client"
    "weather_runtime"
    "esp_timer"
    # Gate W6.1. The pilot READS these authorities and increments nothing in
    # either of them. Both are unconditional for the ESP-IDF reason: component
    # requirements are resolved before any CONFIG_* symbol exists. Without
    # CONFIG_NX_MUTATION_OBSERVABILITY the counters compile away, and without
    # CONFIG_NX_TIMED_SESSIONS the runtime has no instance storage.
    "mutation_observability"
    "pool_session_runtime"
)
