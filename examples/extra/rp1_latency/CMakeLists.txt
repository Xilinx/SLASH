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
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
#  NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
#  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
#  DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

cmake_minimum_required(VERSION 3.20)
project(rp1_latency LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VRT_USE_REPO "Build VRT and its dependencies from this repository" OFF)
option(BUILD_VBIN "Build the hardware vbin (requires Vivado/Vitis)" ON)

if(VRT_USE_REPO)
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." REALPATH)
  set(VRT_INCLUDE_VRTD ON CACHE BOOL "Build vrtd from the repository" FORCE)
  set(VRTD_INCLUDE_LIBSLASH ON CACHE BOOL "Build libslash from the repository" FORCE)
  add_subdirectory("${REPO_ROOT}/vrt" "${CMAKE_CURRENT_BINARY_DIR}/vrt")
  set(_VRT_TARGET vrt)
else()
  find_package(vrt REQUIRED CONFIG)
  set(_VRT_TARGET vrt::vrt)
endif()

add_executable(rp1_latency rp1_latency.cpp)
target_link_libraries(rp1_latency PRIVATE ${_VRT_TARGET})
target_compile_options(rp1_latency PRIVATE -Wall -Wextra)

if(BUILD_VBIN)
  if(VRT_USE_REPO)
    list(APPEND CMAKE_MODULE_PATH "${REPO_ROOT}/cmake")
    include(SlashTools)
  else()
    find_package(SlashTools REQUIRED)
  endif()

  set(DEVICE "xcv80-lsva4737-2MHP-e-S" CACHE STRING "Target device")
  build_hls_dir(
    TARGET      latency_hls
    ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls"
    DEVICE      "${DEVICE}"
    KERNELS     latency_kernel
    OUT_KERNELS _KERNELS
  )
  add_vbin(
    TARGET    rp1_latency_hw
    PLATFORM  hw
    CFG       "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg"
    KERNELS   ${_KERNELS}
  )
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(rp1_latency_stats_test latency_stats_test.cpp)
  target_compile_options(rp1_latency_stats_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME rp1_latency_stats COMMAND rp1_latency_stats_test)

  add_executable(rp1_latency_trace_test rp1_latency_trace_test.cpp)
  target_link_libraries(rp1_latency_trace_test PRIVATE ${_VRT_TARGET})
  # The test includes the benchmark translation unit to exercise its private
  # trace parser; hardware-only benchmark helpers are intentionally unused.
  target_compile_options(
    rp1_latency_trace_test PRIVATE -Wall -Wextra -Wno-unused-function
  )
  add_test(NAME rp1_latency_trace COMMAND rp1_latency_trace_test)
endif()
