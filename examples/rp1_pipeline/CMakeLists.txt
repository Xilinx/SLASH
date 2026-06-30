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
project(rp1_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SLASH_USE_REPO "Build against the local repo tree instead of installed packages" OFF)
option(BUILD_KERNELS  "Build the HLS kernels and pack them into a .vrtbin (requires Vivado)" ON)

if(SLASH_USE_REPO)
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
  if(BUILD_KERNELS)
    list(APPEND CMAKE_MODULE_PATH "${REPO_ROOT}/cmake")
    include(SlashTools)
  endif()
  add_subdirectory(${REPO_ROOT}/vrt ${CMAKE_CURRENT_BINARY_DIR}/vrt)
  set(_VRT_LIBS vrt)
  # libslash is built as part of VRT's subdirectory; expose it explicitly so
  # the host app can drop to raw BAR access for the RP1 graph submission.
  set(_SLASH_LIB slash)
else()
  find_package(vrt REQUIRED CONFIG)
  find_package(slash REQUIRED CONFIG)
  if(BUILD_KERNELS)
    find_package(SlashTools REQUIRED)
  endif()
  set(_VRT_LIBS vrt::vrt)
  set(_SLASH_LIB slash::slash)
endif()

if(BUILD_KERNELS)
  # --- HLS kernels ---
  set(DEVICE "xcv80-lsva4737-2MHP-e-S" CACHE STRING "Target device")

  build_hls_dir(
    TARGET      hls
    ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls"
    DEVICE      "${DEVICE}"
    KERNELS     produce square cube combine reduce
    OUT_KERNELS _KERNELS
  )

  # --- VBIN target (hardware only; sim/emu paths not exercised here) ---
  set(CFG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg")
  add_vbin(TARGET "pipeline_hw" PLATFORM "hw" CFG "${CFG_FILE}" KERNELS ${_KERNELS})
endif()

# --- Host application ---
#
# Uses VRT for device + kernel discovery and HBM buffer allocation, then
# drops to libslash for raw BAR access so it can manually build an RP1
# graph in shared DDR (the proper Graph API doesn't exist yet — this is
# bring-up scaffolding).
add_executable(${PROJECT_NAME} rp1_pipeline.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE ${_VRT_LIBS} ${_SLASH_LIB})

# The shared RP1 protocol header is under driver/libslash/include.
if(SLASH_USE_REPO)
  target_include_directories(${PROJECT_NAME} PRIVATE
    "${REPO_ROOT}/driver/libslash/include")
endif()
