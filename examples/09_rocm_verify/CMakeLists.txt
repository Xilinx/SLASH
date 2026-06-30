# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
# ##################################################################################################
#
# 09_rocm_verify — Minimal ROCm/ROCr smoke test.
#
# No FPGA hardware or SLASH driver required. Tests:
#   1. HSA agent enumeration (CPU + GPU)
#   2. HIP vector-add kernel (basic GPU compute)
#   3. hsa_amd_interop_map_buffer symbol linkage (the API used by 08_p2p_gpu)
#
# Build (requires ROCm installed at /opt/rocm):
#
#   mkdir build && cd build
#   cmake .. -DCMAKE_HIP_COMPILER=/opt/rocm/bin/amdclang++ -DGPU_ARCH=gfx<your_arch>
#   cmake --build .
#   ./09_rocm_verify
#
# Find your GPU arch:
#   /opt/rocm/bin/rocminfo | grep 'Name:.*gfx'
#
# ##################################################################################################

cmake_minimum_required(VERSION 3.21)

project(09_rocm_verify LANGUAGES C CXX HIP)

# ROCm 6.x does not ship hipConfig.cmake / hsa-runtime64Config.cmake.
# Locate the runtime libraries directly instead.
find_library(HIP_LIB   amdhip64      HINTS /opt/rocm/lib REQUIRED)
find_library(HSA_LIB   hsa-runtime64 HINTS /opt/rocm/lib REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GPU_ARCH "gfx90a" CACHE STRING "HIP GPU target architecture (e.g. gfx90a, gfx1100, gfx942)")

add_executable(${PROJECT_NAME}
  09_rocm_verify.cpp
  vadd_kernel.hip
)

# Both files use HIP kernel launch syntax (<<<>>>) and must be compiled by amdclang++.
set_source_files_properties(09_rocm_verify.cpp vadd_kernel.hip PROPERTIES
  LANGUAGE          HIP
  HIP_ARCHITECTURES "${GPU_ARCH}"
)

target_include_directories(${PROJECT_NAME} PRIVATE /opt/rocm/include)

target_link_libraries(${PROJECT_NAME} PRIVATE
  ${HIP_LIB}
  ${HSA_LIB}
)
