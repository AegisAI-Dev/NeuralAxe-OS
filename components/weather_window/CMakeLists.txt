idf_component_register(
SRCS
    "nx_weather_window.c"
    "nx_weather_window_runtime.c"

INCLUDE_DIRS
    "include"

REQUIRES
    # The committed W3 schedule (window identity and the plan that produces
    # it) and the committed W2 crash-safe store (the persistence authority).
    #
    # nx_weather_window.c is PURE and stateless: it only decides.
    # nx_weather_window_runtime.c owns ONE TuningStore instance and performs
    # the durable claim through W2's committed dual-slot path. Neither creates
    # a task, a timer or a queue, neither opens an NVS handle of its own, and
    # neither knows anything about weather transport, telemetry or tuning.
    # The B3 "nx_tps" namespace is never referenced.
    "local_schedule"
    "tuning_store"
)
