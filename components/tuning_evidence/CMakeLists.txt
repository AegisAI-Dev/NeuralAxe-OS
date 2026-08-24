idf_component_register(
SRCS
    "nx_tuning_evidence.c"

INCLUDE_DIRS
    "include"

REQUIRES
    # The committed W1 profile model: this component judges evidence ABOUT a
    # TuningProfile and reuses the committed compatibility rules rather than
    # restating them. It reads profiles; it never writes one.
    "tuning_profile"
)

# GATE W6.5A — DELIBERATELY NOT IN main's DEPENDENCY CLOSURE.
#
# Evidence qualification is an offline/build-time governance activity. Linking
# it into firmware would add permanent runtime cost and, far worse, would put
# code on the device that reasons about its own promotion. The ELF audit for
# this gate asserts ZERO symbols from this component in BOTH firmware postures.
