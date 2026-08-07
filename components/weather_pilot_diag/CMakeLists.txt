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
    # Gate W6.2. The pilot PROJECTS the committed Gate B10 sanitized
    # trusted-time diagnostics; it needs the model and its token formatters.
    # Unconditional for the same ESP-IDF reason as the two above: component
    # requirements resolve before any CONFIG_* symbol exists. This adds an
    # include path and two pure enum-to-string maps, no provider and no
    # networking, and it starts nothing.
    "pool_time"
)
