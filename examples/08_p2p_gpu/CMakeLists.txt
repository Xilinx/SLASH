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

cmake_minimum_required(VERSION 3.21)

# Probe for ROCm libraries before declaring languages so we can conditionally
# enable the HIP language only when the toolchain is present.
# ROCm 6.x does not ship hipConfig.cmake / hsa-runtime64Config.cmake;
# locate the runtime libraries directly instead.
find_library(HIP_LIB   amdhip64      HINTS /opt/rocm/lib)
find_library(HSA_LIB   hsa-runtime64 HINTS /opt/rocm/lib)

if(HIP_LIB AND HSA_LIB AND CMAKE_HIP_COMPILER)
  project(08_p2p_gpu LANGUAGES C CXX HIP)
  set(_HIP_AVAILABLE TRUE)
else()
  project(08_p2p_gpu LANGUAGES C CXX)
  set(_HIP_AVAILABLE FALSE)
  message(WARNING
    "HIP or HSA runtime not found — GPU kernel target will not be built. "
    "Install ROCm and re-run cmake with -DCMAKE_HIP_COMPILER=/opt/rocm/bin/amdclang++.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SLASH_USE_REPO            "Build against the local repo tree instead of installed packages" ON)
option(SLASH_ENABLE_FPGA_TARGETS "Enable HLS/vbin targets (requires Vivado/Vitis)"                ON)

if(SLASH_USE_REPO)
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
  if(SLASH_ENABLE_FPGA_TARGETS)
    list(APPEND CMAKE_MODULE_PATH "${REPO_ROOT}/cmake")
    include(SlashTools)
  endif()
  add_subdirectory(${REPO_ROOT}/driver/libslash ${CMAKE_CURRENT_BINARY_DIR}/libslash)
  set(_SLASH_LIB slash)
else()
  if(SLASH_ENABLE_FPGA_TARGETS)
    find_package(SlashTools REQUIRED)
  endif()
  find_package(slash REQUIRED CONFIG)
  set(_SLASH_LIB slash::slash)
endif()

# --- HLS kernel and VBIN targets ---
if(SLASH_ENABLE_FPGA_TARGETS)
  set(DEVICE "xcv80-lsva4737-2MHP-e-S" CACHE STRING "Target device")

  build_hls_dir(
    TARGET      hls
    ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls"
    DEVICE      "${DEVICE}"
    KERNELS     slash_add
    OUT_KERNELS _KERNELS
  )

  set(CFG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg")
  add_vbin(TARGET "p2p_gpu_hw"  PLATFORM "hw"  CFG "${CFG_FILE}" KERNELS ${_KERNELS})
  add_vbin(TARGET "p2p_gpu_sim" PLATFORM "sim" CFG "${CFG_FILE}" KERNELS ${_KERNELS})
  add_vbin(TARGET "p2p_gpu_emu" PLATFORM "emu" CFG "${CFG_FILE}" KERNELS ${_KERNELS})
endif()

# --- Host + GPU executable ---
if(_HIP_AVAILABLE)
  set(GPU_ARCH "gfx90a" CACHE STRING "HIP GPU target architecture (e.g. gfx90a, gfx1100)")

  add_executable(${PROJECT_NAME} 08_p2p_gpu.cpp gpu_kernel.hip)
  # Both files use HIP kernel launch syntax (<<<>>>) and must be compiled by amdclang++.
  set_source_files_properties(08_p2p_gpu.cpp gpu_kernel.hip PROPERTIES
    LANGUAGE         HIP
    HIP_ARCHITECTURES "${GPU_ARCH}"
  )
  target_compile_definitions(${PROJECT_NAME} PRIVATE SLASH_HIP=1)
  target_include_directories(${PROJECT_NAME} PRIVATE /opt/rocm/include)
  target_link_libraries(${PROJECT_NAME} PRIVATE
    ${_SLASH_LIB}
    ${HIP_LIB}
    ${HSA_LIB}
  )
else()
  # Stub: builds without ROCm, prints an error at runtime.
  add_executable(${PROJECT_NAME} 08_p2p_gpu.cpp)
  target_compile_definitions(${PROJECT_NAME} PRIVATE SLASH_HIP=0)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${_SLASH_LIB})
endif()
