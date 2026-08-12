idf_component_register(
SRCS
    "nx_weather_io.c"
    "nx_weather_io_worker.c"

INCLUDE_DIRS
    "include"

REQUIRES
    # The bounded request/result value types come from the committed W3
    # provider/transport model and the committed W3 schedule identity.
    "weather_client"
    "local_schedule"
    # weather_fetch_execute() — the SINGLE shared fetch orchestration. The
    # worker calls it; it does not reimplement the sequence.
    "weather_runtime"
    # The worker task itself. Unconditional for the ESP-IDF reason the
    # committed W6.1/W6.2 components already document: component requirements
    # resolve before any CONFIG_* symbol exists. With
    # CONFIG_NX_WEATHER_IO_WORKER off the whole worker translation unit
    # compiles away and no task, stack or transport reference remains.
    "freertos"
    "esp_timer"
)
