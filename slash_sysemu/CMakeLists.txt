# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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

cmake_minimum_required(VERSION 3.16)

project(slash_sysemu
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

# Standard GNU install layout (CMAKE_INSTALL_BINDIR, _LIBDIR, …) plus the full
# install paths the daemon bakes in as compile-time defaults.
include(GNUInstallDirs)

# -------- Build options --------

option(SLASH_SYSEMU_BUILD_TESTS "Build unit tests" OFF)
option(SLASH_SYSEMU_INSTALL     "Install the daemon, default VBIN, and assets" ON)

option(ENABLE_ASAN  "Build with AddressSanitizer"          OFF)
option(ENABLE_UBSAN "Build with Undefined Behaviour Sanitizer" OFF)
option(ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)

# Sanitisers and coverage are mutually exclusive (same as libslash)
if(ENABLE_COVERAGE AND (ENABLE_ASAN OR ENABLE_UBSAN))
  message(FATAL_ERROR "ENABLE_COVERAGE cannot be used together with ENABLE_ASAN or ENABLE_UBSAN")
endif()

if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

if(ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()

if(ENABLE_COVERAGE)
  add_compile_options(--coverage -fno-inline)
  add_link_options(--coverage)
endif()

# -------- Testing --------

# enable_testing() must be called in the top-level CMakeLists.txt so that
# CTest discovers tests registered in subdirectories.
if(SLASH_SYSEMU_BUILD_TESTS)
  enable_testing()
endif()

# -------- Subdirectories --------

add_subdirectory(src)

if(SLASH_SYSEMU_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# -------- Coverage target --------

if(ENABLE_COVERAGE)
  find_program(LCOV_EXECUTABLE    lcov    REQUIRED)
  find_program(GENHTML_EXECUTABLE genhtml REQUIRED)

  add_custom_target(coverage
    COMMENT "Generating lcov HTML coverage report"
    # Reset counters
    COMMAND ${LCOV_EXECUTABLE} --directory . --zerocounters
    # Run the tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    # Capture coverage data. mismatch/unexecuted-block warnings from the
    # GCC 13 + lcov 2.0 combination are cosmetic; downgrade them so the run
    # stays clean.
    COMMAND ${LCOV_EXECUTABLE}
              --directory .
              --capture
              --output-file coverage.info
              --ignore-errors mismatch,gcov,unused
              --rc geninfo_unexecuted_blocks=1
    # Keep only this project's own sources (src/). Extracting rather than
    # removing a long path list avoids lcov's "unused pattern" warnings.
    COMMAND ${LCOV_EXECUTABLE}
              --extract coverage.info
              "${CMAKE_SOURCE_DIR}/src/*"
              --output-file coverage_filtered.info
              --ignore-errors unused
    # Generate HTML
    COMMAND ${GENHTML_EXECUTABLE}
              coverage_filtered.info
              --output-directory coverage_html
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
endif()
