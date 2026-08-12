idf_component_register(
SRCS
    "test_nx_telemetry_safety.c"

REQUIRES
    "unity"
    "telemetry_safety"
    # The readiness probe derives the W1 sensor verdicts from one snapshot
    # using the COMMITTED classifiers, proving no second raw read is needed.
    "tuning_profile"
)
