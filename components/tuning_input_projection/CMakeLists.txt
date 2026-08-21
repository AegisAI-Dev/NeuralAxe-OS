idf_component_register(
SRCS
    "nx_tuning_input.c"

INCLUDE_DIRS
    "include"

REQUIRES
    # The coherent snapshot type (facts in) and the W1 contract plus its
    # committed classifiers (verdicts out). Nothing else: this component is
    # pure, holds no state, reads no hardware and knows nothing about weather.
    "telemetry_safety"
    "tuning_profile"
)
