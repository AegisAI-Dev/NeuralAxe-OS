# This is the project CMakeLists.txt file for the test subproject
cmake_minimum_required(VERSION 3.16)

# Include the components directory of the main application:
#
set(EXTRA_COMPONENT_DIRS "../components")

# Set the components to include the tests for.
# This can be overriden from CMake cache:
# - when invoking CMake directly: cmake -D TEST_COMPONENTS="xxxxx" ..
# - when using idf.py: idf.py -T xxxxx build
#
#
# UNION of the timed-session/trusted-time list (this branch, Gates B1-B10.2)
# and the Weather-Aware Tuning list (Gates W1-W3). The weather branch forked at
# Gate B5 and replaced this line, so a straight take-either would silently drop
# one suite; Gate W4 keeps both and adds weather_runtime.
#
# This union is the CANONICAL complete list — nothing is ever dropped from it.
# A single image containing every suite does, however, exceed the stock 1 MB
# factory partition, and test-ci/sdkconfig.defaults is deliberately NOT changed
# to enlarge it. Deterministic QEMU validation therefore runs as TWO bounded
# images, using the documented TEST_COMPONENTS cache override above (the lists
# are disjoint, so the totals add with no double counting):
#
#   idf.py -B <dir1> -DSDKCONFIG=<sdk1> -DTEST_COMPONENTS="stratum asic \
#       thermal_control pool_session pool_time pool_session_store \
#       pool_session_recovery pool_operation_coordinator pool_session_runtime \
#       pool_session_execution pool_session_api pool_session_preflight" build
#   idf.py -B <dir2> -DSDKCONFIG=<sdk2> -DTEST_COMPONENTS="tuning_profile \
#       tuning_store local_schedule weather_client weather_runtime" build
#
set(TEST_COMPONENTS "stratum asic thermal_control pool_session pool_time pool_session_store pool_session_recovery pool_operation_coordinator pool_session_runtime pool_session_execution pool_session_api pool_session_preflight tuning_profile tuning_store local_schedule weather_client weather_runtime" CACHE STRING "List of components to test")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(IDF_TARGET "esp32s3")

idf_build_set_property(COMPILE_DEFINITIONS "-DCONFIG_ASIC_FREQUENCY=100" APPEND)

project(unit_test_stratum)
