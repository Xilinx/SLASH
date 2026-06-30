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
# rp1_mem_latency_gpu -- GPU-side latency benchmark for raw RP1 shared-DDR
# reads and writes through PF2 BAR4 imported as a dma-buf into HSA.

cmake_minimum_required(VERSION 3.21)

find_library(HIP_LIB amdhip64
  HINTS /opt/rocm/lib /opt/rocm-slash/lib
)
find_library(HSA_LIB hsa-runtime64
  HINTS /opt/rocm/lib /opt/rocm-slash/lib
)

if(HIP_LIB AND HSA_LIB AND CMAKE_HIP_COMPILER)
  project(rp1_mem_latency_gpu LANGUAGES C CXX HIP)
  set(_HIP_AVAILABLE TRUE)
else()
  project(rp1_mem_latency_gpu LANGUAGES C CXX)
  set(_HIP_AVAILABLE FALSE)
  message(WARNING
    "HIP or HSA runtime not found -- GPU kernel target will not be built. "
    "Install ROCm and re-run cmake with -DCMAKE_HIP_COMPILER=/opt/rocm/bin/amdclang++.")
endif()

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

if(_HIP_AVAILABLE)
  set(GPU_ARCH "gfx90a" CACHE STRING "HIP GPU target architecture (e.g. gfx908, gfx90a, gfx942, gfx1100)")

  add_executable(${PROJECT_NAME} rp1_mem_latency_gpu.cpp rp1_mem_latency_kernel.hip)
  set_source_files_properties(rp1_mem_latency_gpu.cpp rp1_mem_latency_kernel.hip PROPERTIES
    LANGUAGE          HIP
    HIP_ARCHITECTURES "${GPU_ARCH}"
  )
  target_compile_definitions(${PROJECT_NAME} PRIVATE SLASH_HIP=1)
  target_include_directories(${PROJECT_NAME} PRIVATE /opt/rocm/include /opt/rocm-slash/include)
  target_link_libraries(${PROJECT_NAME} PRIVATE
    ${_SLASH_LIB}
    ${HIP_LIB}
    ${HSA_LIB}
  )
else()
  add_executable(${PROJECT_NAME} rp1_mem_latency_gpu.cpp)
  target_compile_definitions(${PROJECT_NAME} PRIVATE SLASH_HIP=0)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${_SLASH_LIB})
endif()
