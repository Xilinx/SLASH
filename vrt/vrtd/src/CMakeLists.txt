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

# Static library containing all daemon logic (everything except main.c).
# Used by both the vrtd executable and the unit test targets.
add_library(vrtd_core STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/accept.c
  ${CMAKE_CURRENT_SOURCE_DIR}/allocator.c
  ${CMAKE_CURRENT_SOURCE_DIR}/auth.c
  ${CMAKE_CURRENT_SOURCE_DIR}/buffer.c
  ${CMAKE_CURRENT_SOURCE_DIR}/clock.c
  ${CMAKE_CURRENT_SOURCE_DIR}/config.c
  ${CMAKE_CURRENT_SOURCE_DIR}/design_writer.c
  ${CMAKE_CURRENT_SOURCE_DIR}/device.c
  ${CMAKE_CURRENT_SOURCE_DIR}/hotplug.c
  ${CMAKE_CURRENT_SOURCE_DIR}/reset.c
  ${CMAKE_CURRENT_SOURCE_DIR}/serve.c
  ${CMAKE_CURRENT_SOURCE_DIR}/signals.c
  ${CMAKE_CURRENT_SOURCE_DIR}/utils.c
)

set_target_properties(vrtd_core PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS YES
)

target_include_directories(vrtd_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(vrtd_core
  PUBLIC
    ami::ami
    slash::slash
    PkgConfig::SYSTEMD
    PkgConfig::INIH
    Threads::Threads
)

# Daemon executable
add_executable(vrtd ${CMAKE_CURRENT_SOURCE_DIR}/main.c)

target_link_libraries(vrtd PRIVATE vrtd_core)

set_target_properties(vrtd PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS YES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

add_executable(vrtd::vrtd ALIAS vrtd)
