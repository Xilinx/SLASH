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

# Read version from packaging/version
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../packaging/version" LIBSLASH_VERSION)
string(STRIP "${LIBSLASH_VERSION}" LIBSLASH_VERSION)

# Parse into components
string(REPLACE "." ";" VERSION_LIST "${LIBSLASH_VERSION}")
list(GET VERSION_LIST 0 LIBSLASH_VERSION_MAJOR)
list(GET VERSION_LIST 1 LIBSLASH_VERSION_MINOR)
list(GET VERSION_LIST 2 LIBSLASH_VERSION_PATCH)

message(STATUS "LIBSLASH version: ${LIBSLASH_VERSION} (${LIBSLASH_VERSION_MAJOR}.${LIBSLASH_VERSION_MINOR}.${LIBSLASH_VERSION_PATCH})")

project(libslash
  VERSION ${LIBSLASH_VERSION}
  LANGUAGES C CXX
)

# Allow user to choose shared vs static (standard CMake variable)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(LIBSLASH_BUILD_TESTS "Build unit tests" OFF)

option(ENABLE_SANITIZERS "Build with AddressSanitizer and UBSan" OFF)
if(ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

option(ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)
if(ENABLE_COVERAGE)
  if(ENABLE_SANITIZERS)
    message(FATAL_ERROR "ENABLE_COVERAGE and ENABLE_SANITIZERS cannot be used together")
  endif()
  add_compile_options(--coverage -fno-inline)
  add_link_options(--coverage)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_subdirectory(src)

if(LIBSLASH_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# -------- Installation: headers and library --------
# Public headers are under include/  (layout: include/slash/*.h)
install(
  DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  TARGETS slash
  EXPORT  slashTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

# -------- CMake package configuration --------
# Generate the version file
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/slashConfigVersion.cmake"
  VERSION       ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion
)

# Configure the main package config from template
configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/slashConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/slashConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/slash"
)

# Export targets for *install* tree
install(
  EXPORT slashTargets
  NAMESPACE slash::
  FILE slashTargets.cmake
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/slash"
)

# Install the config + version files
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/slashConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/slashConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/slash"
)

# Export targets for *build* tree so a project can use this directory directly
export(
  EXPORT slashTargets
  NAMESPACE slash::
  FILE "${CMAKE_CURRENT_BINARY_DIR}/slashTargets.cmake"
)
