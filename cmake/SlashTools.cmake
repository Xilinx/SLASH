# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

include_guard(GLOBAL)

# Make FindVivado.cmake and FindVitis.cmake discoverable from the same directory
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

# Include BuildHLS so that find_package(SlashTools) provides everything
include("${CMAKE_CURRENT_LIST_DIR}/BuildHLS.cmake")

find_package(Vivado REQUIRED)

# --- Locate the SLASH linker ---
# Two modes:
#   1. Installed: v80++ executable on PATH (preferred)
#   2. Source tree: SLASH_REPO_ROOT points to the repository root
set(_SLASH_TOOLS_USE_INSTALLED FALSE)
set(_SLASH_TOOLS_USE_REPO FALSE)

find_program(V80PP_EXECUTABLE NAMES v80++)
if(V80PP_EXECUTABLE)
  set(_SLASH_TOOLS_USE_INSTALLED TRUE)
  message(STATUS "SlashTools: Found installed v80++ at ${V80PP_EXECUTABLE}")
endif()

if(NOT DEFINED SLASH_REPO_ROOT)
  # Try to detect if we are in the source tree
  get_filename_component(_slash_tools_candidate_root "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
  if(EXISTS "${_slash_tools_candidate_root}/linker/src/main.py")
    set(SLASH_REPO_ROOT "${_slash_tools_candidate_root}")
  endif()
endif()

if(DEFINED SLASH_REPO_ROOT AND EXISTS "${SLASH_REPO_ROOT}/linker/src/main.py")
  set(_SLASH_TOOLS_USE_REPO TRUE)
  set(SLASH_LINKER_DIR "${SLASH_REPO_ROOT}/linker/")
  set(SLASH_MAIN_PY "${SLASH_LINKER_DIR}/src/main.py")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  message(STATUS "SlashTools: Found SLASH repo at ${SLASH_REPO_ROOT}")
endif()

if(NOT _SLASH_TOOLS_USE_INSTALLED AND NOT _SLASH_TOOLS_USE_REPO)
  message(FATAL_ERROR
    "SlashTools: Cannot find the SLASH linker. Either:\n"
    "  - Install the v80++ package (provides /usr/bin/v80++), or\n"
    "  - Set SLASH_REPO_ROOT to the SLASH repository root.")
endif()

set(SLASH_FOUND TRUE)

function(add_vbin)
    set(oneValueArgs TARGET CFG PLATFORM)
    set(multiValueArgs KERNELS)
    cmake_parse_arguments(SLASH_VBIN "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    foreach(req TARGET CFG PLATFORM KERNELS)
        if("${SLASH_VBIN_${req}}" STREQUAL "")
            message(FATAL_ERROR "add_vbin: ${req} is required")
        endif()
    endforeach()

    set(SLASH_VBIN_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SLASH_VBIN_TARGET}.vbin")

    if(_SLASH_TOOLS_USE_REPO)
        # Source-tree mode: invoke main.py directly
        if(DEFINED Python3_EXECUTABLE AND NOT "${Python3_EXECUTABLE}" STREQUAL "")
            set(_py "${Python3_EXECUTABLE}")
        else()
            set(_py "python3")
        endif()

        add_custom_command(
            OUTPUT "${SLASH_VBIN_FILE}"
            COMMAND "${_py}" "${SLASH_MAIN_PY}" "link"
                "-c" "${SLASH_VBIN_CFG}"
                "-p" "${SLASH_VBIN_PLATFORM}"
                "-o" "${SLASH_VBIN_FILE}"
                "-k" ${SLASH_VBIN_KERNELS}
                "--vivado" "${VIVADO_BINARY}"
            BYPRODUCTS "${SLASH_VBIN_FILE}.prj"
            DEPENDS "${SLASH_VBIN_CFG}" "${SLASH_VBIN_KERNELS}"
            WORKING_DIRECTORY "${SLASH_LINKER_DIR}/src"
        )
    else()
        # Installed mode: invoke the v80++ wrapper
        add_custom_command(
            OUTPUT "${SLASH_VBIN_FILE}"
            COMMAND "${V80PP_EXECUTABLE}" "link"
                "-c" "${SLASH_VBIN_CFG}"
                "-p" "${SLASH_VBIN_PLATFORM}"
                "-o" "${SLASH_VBIN_FILE}"
                "-k" ${SLASH_VBIN_KERNELS}
                "--vivado" "${VIVADO_BINARY}"
            BYPRODUCTS "${SLASH_VBIN_FILE}.prj"
            DEPENDS "${SLASH_VBIN_CFG}" "${SLASH_VBIN_KERNELS}"
        )
    endif()

    add_custom_target("${SLASH_VBIN_TARGET}" DEPENDS "${SLASH_VBIN_FILE}")
endfunction()
