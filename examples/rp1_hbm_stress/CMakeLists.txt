# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
#  BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
#  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
#  DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################
#
# rp1_hbm_stress -- hand-authored 69-node RP1 graph that dispatches 64 hbm_bandwidth
# instances in two 32-kernel waves inside a 5-iteration loop.  No vrtbin / VRT --
# uses libslash for raw BAR4 access and the shared slash/uapi/rp1_protocol.h ABI.

cmake_minimum_required(VERSION 3.20)
project(rp1_hbm_stress LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SLASH_USE_REPO "Build against the local repo tree instead of installed packages" ON)

if(SLASH_USE_REPO)
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
  add_subdirectory(${REPO_ROOT}/driver/libslash ${CMAKE_CURRENT_BINARY_DIR}/libslash)
  set(_SLASH_LIB slash)
else()
  find_package(slash REQUIRED CONFIG)
  set(_SLASH_LIB slash::slash)
endif()

add_executable(${PROJECT_NAME} rp1_hbm_stress.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE ${_SLASH_LIB})
