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
project(rp1_pdi_loop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(VRT_USE_REPO  "Build VRT (and its deps) from the repo tree instead of installed packages" OFF)
option(BUILD_KERNELS "Build the HLS kernels and link the two single-kernel vbins (requires Vivado)" ON)

if(VRT_USE_REPO)
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
  if(BUILD_KERNELS)
    list(APPEND CMAKE_MODULE_PATH "${REPO_ROOT}/cmake")
    include(SlashTools)
  endif()
  add_subdirectory(${REPO_ROOT}/vrt ${CMAKE_CURRENT_BINARY_DIR}/vrt)
  set(_VRT_TARGET vrt)
else()
  find_package(vrt REQUIRED CONFIG)
  if(BUILD_KERNELS)
    find_package(SlashTools REQUIRED)
  endif()
  set(_VRT_TARGET vrt::vrt)
endif()

# --- HLS kernels + vbins ----------------------------------------------------
#
# Two single-kernel vbins, both linked against the same base PLATFORM so
# that PDI_LOAD safely swaps only the user region.  Each `hls_*/`
# directory ships its own `pdi_kernel.cpp` writing a different magic
# constant (0xAAAA_AAAA in hls_a/, 0xBBBB_BBBB in hls_b/) and an
# identical `pdi_kernel.cfg`.  Both `config_*.cfg` declare
# `nk=pdi_kernel:1` with no `sp=` lines (the kernel is AXI-Lite only),
# so the linker places the single user-region instance at host
# 0x0202_0000_0000 -> R5 0x8800_0000 in both designs.
if(BUILD_KERNELS)
  set(DEVICE "xcv80-lsva4737-2MHP-e-S" CACHE STRING "Target device")

  build_hls_dir(
    TARGET      hls_a
    ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls_a"
    DEVICE      "${DEVICE}"
    KERNELS     pdi_kernel
    OUT_KERNELS _KERNELS_A
  )
  add_vbin(
    TARGET    rp1_pdi_loop_a_hw
    PLATFORM  hw
    CFG       "${CMAKE_CURRENT_SOURCE_DIR}/config_a.cfg"
    KERNELS   ${_KERNELS_A}
  )

  build_hls_dir(
    TARGET      hls_b
    ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls_b"
    DEVICE      "${DEVICE}"
    KERNELS     pdi_kernel
    OUT_KERNELS _KERNELS_B
  )
  add_vbin(
    TARGET    rp1_pdi_loop_b_hw
    PLATFORM  hw
    CFG       "${CMAKE_CURRENT_SOURCE_DIR}/config_b.cfg"
    KERNELS   ${_KERNELS_B}
  )
endif()

# --- Host application -------------------------------------------------------

add_executable(${PROJECT_NAME} rp1_pdi_loop.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE ${_VRT_TARGET})
