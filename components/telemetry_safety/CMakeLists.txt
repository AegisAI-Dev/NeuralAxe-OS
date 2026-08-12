idf_component_register(
SRCS
    "nx_telemetry_safety.c"

INCLUDE_DIRS
    "include"

REQUIRES
    # portMUX / portENTER_CRITICAL only. This component holds facts, never
    # decisions: it depends on no sensor driver, no policy, no persistence and
    # no weather component, and it performs no hardware access of its own.
    "freertos"
)
