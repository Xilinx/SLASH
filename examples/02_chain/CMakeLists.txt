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

cmake_minimum_required(VERSION 3.20)
project(02_chain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SLASH_USE_REPO "Build against the local repo tree instead of installed packages" OFF)

if(SLASH_USE_REPO)
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
  list(APPEND CMAKE_MODULE_PATH "${REPO_ROOT}/cmake")
  include(SlashTools)
  add_subdirectory(${REPO_ROOT}/vrt ${CMAKE_CURRENT_BINARY_DIR}/vrt)
  set(_VRT_INCLUDES "")
  set(_VRT_LIBS vrt)
else()
  find_package(vrt REQUIRED CONFIG)
  find_package(SlashTools REQUIRED)
  set(_VRT_INCLUDES "")
  set(_VRT_LIBS vrt::vrt)
endif()

# --- HLS kernels ---
set(DEVICE "xcv80-lsva4737-2MHP-e-S" CACHE STRING "Target device")

build_hls_dir(
  TARGET      hls
  ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls"
  DEVICE      "${DEVICE}"
  KERNELS     dma_in dma_out passthrough
  OUT_KERNELS _KERNELS
)

# --- VBIN targets ---
set(CFG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg")

add_vbin(TARGET "chain_hw"  PLATFORM "hw"  CFG "${CFG_FILE}" KERNELS ${_KERNELS})
add_vbin(TARGET "chain_sim" PLATFORM "sim" CFG "${CFG_FILE}" KERNELS ${_KERNELS})
add_vbin(TARGET "chain_emu" PLATFORM "emu" CFG "${CFG_FILE}" KERNELS ${_KERNELS})

# --- Executable ---
add_executable(${PROJECT_NAME} 02_example.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE ${_VRT_INCLUDES})
target_link_libraries(${PROJECT_NAME} PRIVATE ${_VRT_LIBS})
