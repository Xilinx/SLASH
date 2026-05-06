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

cmake_minimum_required(VERSION 3.16)

# Read version from packaging/version
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../packaging/version" VRTD_VERSION)
string(STRIP "${VRTD_VERSION}" VRTD_VERSION)

# Parse into components
string(REPLACE "." ";" VERSION_LIST "${VRTD_VERSION}")
list(GET VERSION_LIST 0 VRTD_VERSION_MAJOR)
list(GET VERSION_LIST 1 VRTD_VERSION_MINOR)
list(GET VERSION_LIST 2 VRTD_VERSION_PATCH)

message(STATUS "VRTD version: ${VRTD_VERSION} (${VRTD_VERSION_MAJOR}.${VRTD_VERSION_MINOR}.${VRTD_VERSION_PATCH})")

project(vrtd
  VERSION ${VRTD_VERSION}
  LANGUAGES C CXX
)

# Optionally build examples
option(VRTD_BUILD_EXAMPLES "Build example executables" OFF)
option(VRTD_BUILD_TESTS "Build unit tests" OFF)
option(VRTD_INCLUDE_LIBSLASH "Include libslash subdirectory instead of building from system" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

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

if(VRTD_INCLUDE_LIBSLASH)
  add_subdirectory(../../driver/libslash libslash)
else()
  find_package(slash REQUIRED)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ami.cmake)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# Find libsystemd
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

# Find libinih
pkg_check_modules(INIH REQUIRED IMPORTED_TARGET inih)

add_subdirectory(src)

add_subdirectory(libvrtd)
add_subdirectory(libvrtdpp)

if(VRTD_BUILD_TESTS)
  add_subdirectory(tests)
endif()

#if(VRTD_BUILD_EXAMPLES)
#  add_subdirectory(examples)
#endif()

# -------- Installation: headers and library --------
# Public headers are under include/  (layout: include/vrtd/*.h)
install(
  DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  DIRECTORY "${PROJECT_SOURCE_DIR}/libvrtd/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  DIRECTORY "${PROJECT_SOURCE_DIR}/libvrtdpp/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  TARGETS libvrtd libvrtdpp
  EXPORT  vrtdTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  TARGETS vrtd
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# -------- CMake package configuration --------
# Generate the version file
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/vrtdConfigVersion.cmake"
  VERSION       ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion
)

# Configure the main package config from template
configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/vrtdConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vrtdConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vrtd"
)

# Export targets for *install* tree
install(
  EXPORT vrtdTargets
  NAMESPACE vrtd::
  FILE vrtdTargets.cmake
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vrtd"
)

# Install the config + version files
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/vrtdConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/vrtdConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vrtd"
)

# Export targets for *build* tree so a project can use this directory directly
export(
  EXPORT vrtdTargets
  NAMESPACE vrtd::
  FILE "${CMAKE_CURRENT_BINARY_DIR}/vrtdTargets.cmake"
)
