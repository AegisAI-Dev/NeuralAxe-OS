# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# (Not part of the boilerplate)
# This example uses an extra component for common functions such as Wi-Fi and Ethernet connection.
# set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ---------------------------------------------------------------------------
# THE canonical revision (see tools/pilot/canonical_revision.py)
#
# A device compares the firmware's esp_app_desc_t version with the web image's
# version.txt by EXACT STRING EQUALITY. ESP-IDF's project.cmake derives
# PROJECT_VER through git_describe() with NO --abbrev argument, so git's
# `core.abbrev=auto` picks the length; axe-os/generate-version.js used to do the
# same. auto differs by git version and host: this repository, same commit,
# produced v2.14.2-70-g34a5150 (host git 2.54, 7 hex) and v2.14.2-70-g34a51508
# (container git 2.43, 8 hex), and a real device reported BOOT PAIR MISMATCH.
#
# Neither side may choose its own abbreviation. Precedence:
#   1. -DPROJECT_VER=... on the command line (what the packaging helpers pass);
#      project.cmake guards with if(NOT DEFINED PROJECT_VER), so it wins.
#   2. NX_CANONICAL_REVISION in the environment — the same variable
#      generate-version.js honours, so one export pins both sides.
#   3. `git describe` with an EXPLICIT --abbrev=8, matching generate-version.js.
# If git cannot answer, PROJECT_VER is left undefined and ESP-IDF's own
# fallback applies, so a source export outside a checkout still builds.
# ---------------------------------------------------------------------------
if(NOT DEFINED PROJECT_VER)
    if(NOT "$ENV{NX_CANONICAL_REVISION}" STREQUAL "")
        set(PROJECT_VER "$ENV{NX_CANONICAL_REVISION}")
    else()
        execute_process(
            COMMAND git describe --tags --long --always --abbrev=8
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            OUTPUT_VARIABLE NX_CANONICAL_DESCRIBE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE NX_CANONICAL_DESCRIBE_RESULT
            ERROR_QUIET)
        if(NX_CANONICAL_DESCRIBE_RESULT EQUAL 0 AND NOT NX_CANONICAL_DESCRIBE STREQUAL "")
            set(PROJECT_VER "${NX_CANONICAL_DESCRIBE}")
        endif()
    endif()
endif()
if(DEFINED PROJECT_VER)
    message(STATUS "NeuralAxe canonical revision: ${PROJECT_VER}")
endif()

project(esp-miner)

