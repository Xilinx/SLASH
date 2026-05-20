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

find_program(VITIS_BINARY
  NAMES vitis
  PATHS ${VITIS_ROOT_DIR} ENV XILINX_VITIS ENV VITIS_HOME ENV VITIS
  PATH_SUFFIXES bin
)

if(NOT VITIS_BINARY)
  message(FATAL_ERROR "Vitis not found. Set XILINX_VITIS or VITIS_HOME (or add vitis to PATH).")
endif()

get_filename_component(_vitis_bin_dir "${VITIS_BINARY}" DIRECTORY)
get_filename_component(VITIS_ROOT_DIR "${_vitis_bin_dir}" DIRECTORY)

set(VITIS_INCLUDE_DIR "${VITIS_ROOT_DIR}/include")
if(NOT EXISTS "${VITIS_INCLUDE_DIR}")
  message(FATAL_ERROR "Vitis include dir not found: ${VITIS_INCLUDE_DIR}")
endif()

set(VITIS_FOUND TRUE)
message(STATUS "Found Vitis at ${VITIS_ROOT_DIR}.")
