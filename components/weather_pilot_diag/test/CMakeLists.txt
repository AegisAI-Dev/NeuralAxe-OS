idf_component_register(
SRCS
    "test_nx_weather_pilot_diag.c"
    "test_nx_weather_pilot_time.c"
    "test_nx_weather_pilot_wiring.c"

REQUIRES
    "unity"
    "weather_pilot_diag"
    "weather_source_policy"
    "weather_client"
    "weather_runtime"
    # Gate W6.2: the trusted-time projection tests read the same B10 model
    # the adapter projects, and drive a real runtime instance through it.
    "pool_time"
    "pool_session_runtime"
    # Gate W6.2 production-wiring tests evaluate the committed W1 chain
    # against the real production registry.
    "tuning_profile"
)
