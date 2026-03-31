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

# Library sources (list explicitly; avoid GLOB for reproducible builds)
add_executable(vrtd
  ${CMAKE_CURRENT_SOURCE_DIR}/accept.c
  ${CMAKE_CURRENT_SOURCE_DIR}/allocator.c
  ${CMAKE_CURRENT_SOURCE_DIR}/auth.c
  ${CMAKE_CURRENT_SOURCE_DIR}/buffer.c
  ${CMAKE_CURRENT_SOURCE_DIR}/clock.c
  ${CMAKE_CURRENT_SOURCE_DIR}/config.c
  ${CMAKE_CURRENT_SOURCE_DIR}/design_writer.c
  ${CMAKE_CURRENT_SOURCE_DIR}/device.c
  ${CMAKE_CURRENT_SOURCE_DIR}/hotplug.c
  ${CMAKE_CURRENT_SOURCE_DIR}/main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/reset.c
  ${CMAKE_CURRENT_SOURCE_DIR}/serve.c
  ${CMAKE_CURRENT_SOURCE_DIR}/signals.c
  ${CMAKE_CURRENT_SOURCE_DIR}/utils.c
)

# C standard / properties
set_target_properties(vrtd PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS YES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Public include dir (for consumers) and private include dir (for .c / private headers)
target_include_directories(vrtd
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
  vrtd
  
  PRIVATE
    ami::ami
    slash::slash

    PkgConfig::SYSTEMD
    PkgConfig::INIH
    Threads::Threads
)

add_executable(vrtd::vrtd ALIAS vrtd)
