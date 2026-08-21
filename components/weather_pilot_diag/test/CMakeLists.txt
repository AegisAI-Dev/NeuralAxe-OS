idf_component_register(
SRCS
    "test_nx_weather_pilot_diag.c"
    "test_nx_weather_pilot_time.c"
    "test_nx_weather_pilot_wiring.c"
    "test_nx_weather_pilot_w631_flow.c"
    "test_nx_weather_pilot_w632_schedule.c"

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
    # Gate W6.3.1 end-to-end flow test. It drives the committed async I/O
    # machine, the telemetry store and the W1 input projection through the
    # SAME entry points the adapter uses, and proves the mutation counters
    # stayed clean across the whole sequence.
    "weather_io"
    "telemetry_safety"
    "tuning_input_projection"
    "mutation_observability"
)
