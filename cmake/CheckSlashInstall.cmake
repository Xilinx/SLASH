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

if(NOT DEFINED INSTALL_DIR OR "${INSTALL_DIR}" STREQUAL "")
  set(INSTALL_DIR "/opt/amd/slash")
endif()

set(_required_files
  "static_shell_service_layer.dcp"
  "static_shell_slash.dcp"
  "amd_v80_gen5x8_25.1.pdi"
  "top_wrapper_routed_bb.dcp"
)

set(_missing "")
foreach(_f IN LISTS _required_files)
  if(NOT EXISTS "${INSTALL_DIR}/${_f}")
    list(APPEND _missing "${INSTALL_DIR}/${_f}")
  endif()
endforeach()

if(_missing)
  message(FATAL_ERROR "install was not run. ask your admin to run install first")
endif()
