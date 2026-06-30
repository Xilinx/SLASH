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
project(rp1_bringup_vrt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(VRT_USE_REPO    "Build VRT (and its deps) from the repo tree instead of installed packages" OFF)
option(BUILD_KERNELS   "Build the HLS kernel and pack the four-kernel vbin (requires Vivado)" ON)

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

# --- HLS kernel + vbin -----------------------------------------------------
#
# Four instances of `bringup_kernel(size, in*)` — the same compiled IP is
# instantiated four times via `nk=bringup_kernel:4` in config.cfg.  The
# linker places them at 64 KiB-aligned addresses in alphabetical instance
# order starting at host 0x0202_0000_0000 (see
# linker/src/emit/hw/tcl_gen.py and addr_ctx.py), so on the R5 side they
# show up at 0x8800_0000, 0x8801_0000, 0x8802_0000, 0x8803_0000.
# `rp1_bringup_vrt.cpp` hardcodes those addresses; if you fork the kernel
# signature or instance count you must update them to match the
# system_map.xml emitted next to the vbin.
if(BUILD_KERNELS)
  set(DEVICE "xcv80-lsva4737-2MHP-e-S" CACHE STRING "Target device")

  build_hls_dir(
    TARGET      hls
    ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls"
    DEVICE      "${DEVICE}"
    KERNELS     bringup_kernel
    OUT_KERNELS _KERNELS
  )

  set(CFG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg")
  add_vbin(
    TARGET    rp1_bringup_vrt_hw
    PLATFORM  hw
    CFG       "${CFG_FILE}"
    KERNELS   ${_KERNELS}
  )
endif()

# --- Host application ------------------------------------------------------

add_executable(${PROJECT_NAME} rp1_bringup_vrt.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE ${_VRT_TARGET})
