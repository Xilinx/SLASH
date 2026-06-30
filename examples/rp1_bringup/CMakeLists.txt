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
# Temporary RP1 bring-up tool. Will be replaced by a libslash/VRT GraphBuilder
# API once the RP1 graph path is on the supported surface.

cmake_minimum_required(VERSION 3.20)
project(rp1_bringup LANGUAGES C)

option(SLASH_USE_REPO "Build against the local repo tree instead of installed packages" ON)

if(SLASH_USE_REPO)
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
  add_subdirectory(${REPO_ROOT}/driver/libslash ${CMAKE_CURRENT_BINARY_DIR}/libslash)
  set(_SLASH_LIB slash)
else()
  find_package(slash REQUIRED CONFIG)
  set(_SLASH_LIB slash::slash)
endif()

add_executable(${PROJECT_NAME} rp1_bringup.c)
target_compile_features(${PROJECT_NAME} PRIVATE c_std_11)
target_link_libraries(${PROJECT_NAME} PRIVATE ${_SLASH_LIB})
