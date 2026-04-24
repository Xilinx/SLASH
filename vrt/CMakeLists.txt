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

cmake_minimum_required(VERSION 3.10)

# Read version from packaging/version
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/version" VRT_VERSION)
string(STRIP "${VRT_VERSION}" VRT_VERSION)

# Parse into components
string(REPLACE "." ";" VERSION_LIST "${VRT_VERSION}")
list(GET VERSION_LIST 0 VRT_VERSION_MAJOR)
list(GET VERSION_LIST 1 VRT_VERSION_MINOR)
list(GET VERSION_LIST 2 VRT_VERSION_PATCH)

message(STATUS "VRT version: ${VRT_VERSION} (${VRT_VERSION_MAJOR}.${VRT_VERSION_MINOR}.${VRT_VERSION_PATCH})")

project(
  vrt

  VERSION ${VRT_VERSION}
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(VRT_INCLUDE_VRTD "Include vrtd subdirectory instead of building from system" OFF)
option(VRT_BUILD_TESTS "Build unit tests" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_compile_options("-Wunused-result")

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

# Generate the header
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vrt_version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/vrt/vrt_version.hpp"
    @ONLY
)

file(
  GLOB
  LIB_SOURCES
  
  ${CMAKE_CURRENT_SOURCE_DIR}/src/allocator/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/include/buffer/*.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/qdma/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp 
  ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/register/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/driver/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/utils/*.cpp
)

add_library(vrt SHARED ${LIB_SOURCES})

add_library(vrt::vrt ALIAS vrt)

target_include_directories(
  vrt
  
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:include>
    
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/
)

set_target_properties(vrt PROPERTIES

  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)


if(VRT_INCLUDE_VRTD)
  add_subdirectory(vrtd vrtd)
else()
  find_package(vrtd REQUIRED CONFIG)
endif()

if(NOT TARGET vrtd::libvrtdpp)
    message(FATAL_ERROR
        "vrtd package is missing target vrtd::libvrtdpp. "
        "Build and install vrtd first (cmake --install), then configure vrt again.")
endif()

find_package(ZLIB REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)

if(NOT TARGET JsonCpp::JsonCpp AND TARGET jsoncpp_lib)
  add_library(JsonCpp::JsonCpp ALIAS jsoncpp_lib)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_ZMQ REQUIRED IMPORTED_TARGET libzmq)

target_link_libraries(vrt
  PUBLIC
    vrtd::libvrtdpp
    ZLIB::ZLIB
    LibXml2::LibXml2
    JsonCpp::JsonCpp
    PkgConfig::PC_ZMQ
)

# Testing
if(VRT_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# Installation

set_target_properties(vrt PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
  TARGETS vrt
  EXPORT  vrtTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

# -------- CMake package configuration --------
# Generate the version file
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/vrtConfigVersion.cmake"
  VERSION       ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion
)

# Configure the main package config from template
configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/vrtConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vrtConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vrt"
)

# Export targets for *install* tree
install(
  EXPORT vrtTargets
  NAMESPACE vrt::
  FILE vrtTargets.cmake
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vrt"
)

# Install the config + version files
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/vrtConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/vrtConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vrt"
)

# Export targets for *build* tree so a project can use this directory directly
export(
  EXPORT vrtTargets
  NAMESPACE vrt::
  FILE "${CMAKE_CURRENT_BINARY_DIR}/vrtTargets.cmake"
)
