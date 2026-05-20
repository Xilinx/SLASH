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

cmake_minimum_required(VERSION 3.10)

# Read version from packaging/version
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/version" SMI_VERSION)
string(STRIP "${SMI_VERSION}" SMI_VERSION)

# Parse into components
string(REPLACE "." ";" VERSION_LIST "${SMI_VERSION}")
list(GET VERSION_LIST 0 SMI_VERSION_MAJOR)
list(GET VERSION_LIST 1 SMI_VERSION_MINOR)
list(GET VERSION_LIST 2 SMI_VERSION_PATCH)

message(STATUS "SMI version: ${SMI_VERSION} (${SMI_VERSION_MAJOR}.${SMI_VERSION_MINOR}.${SMI_VERSION_PATCH})")

project(
    v80-smi
    
    VERSION ${SMI_VERSION}
    LANGUAGES C CXX
)

option(SMI_INCLUDE_VRT "Include vrtd as subdirectory instead of building from system" OFF)

include(GNUInstallDirs)

if (SMI_INCLUDE_VRT)
  add_subdirectory(../vrt vrt)
else()
  find_package(vrt REQUIRED CONFIG)
endif()

if(NOT TARGET vrt::vrt)
  message(FATAL_ERROR
    "vrt package is missing target vrt::vrt. "
    "Build and install vrt first (cmake --install), then configure smi again.")
endif()

find_package(CLI11 CONFIG REQUIRED)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp"
    @ONLY
)

add_subdirectory(src)

install(
    TARGETS v80-smi
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
