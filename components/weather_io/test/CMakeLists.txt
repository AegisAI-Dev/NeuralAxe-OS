idf_component_register(
SRCS
    "test_nx_weather_io.c"
    "test_nx_weather_io_stress.c"

REQUIRES
    "unity"
    "weather_io"
    # The tests drive the SINGLE shared fetch orchestration and the committed
    # W3 request builder / transport validation through fake ops only. The
    # real ESP-IDF adapter is never referenced, and no test performs a network
    # operation of any kind.
    "weather_runtime"
    "weather_client"
    "local_schedule"
    # Gate W6.3 requires the end-to-end assertion that an UNVALIDATED
    # production Gamma 601 profile keeps the recommendation non-actionable.
    "tuning_profile"
)
