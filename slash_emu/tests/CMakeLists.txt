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

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_custom_target(unit_tests)

macro(add_slash_emu_test test_name test_source)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE GTest::gtest_main GTest::gmock slash_emu_core)
  set_target_properties(${test_name} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
  )
  target_compile_options(${test_name} PRIVATE -Wall -Wextra -Werror)
  gtest_discover_tests(${test_name})
  add_dependencies(unit_tests ${test_name})
endmacro()

add_slash_emu_test(daemon_test     daemon_test.cpp)
add_slash_emu_test(transport_test transport_test.cpp)
add_slash_emu_test(config_test    config_test.cpp)

# -------- reusable mock vpp_sim model server (test helper) --------
# pkg_check_modules populates directory-scoped variables, so re-query here for
# the tests subdirectory (the imported target PkgConfig::PC_ZMQ is global).
find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_ZMQ REQUIRED IMPORTED_TARGET libzmq)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

# Built as a static library so later steps (8, 10) can link the same mock.
add_library(mock_model_server STATIC mock_model_server.cpp)
target_include_directories(mock_model_server
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE
    ${JSONCPP_INCLUDE_DIRS}
)
target_compile_features(mock_model_server PUBLIC cxx_std_20)
target_compile_options(mock_model_server PRIVATE -Wall -Wextra -Werror)
target_link_libraries(mock_model_server PRIVATE PkgConfig::PC_ZMQ ${JSONCPP_LIBRARIES})

add_slash_emu_test(model_client_test model_client_test.cpp)
target_link_libraries(model_client_test PRIVATE mock_model_server PkgConfig::PC_ZMQ ${JSONCPP_LIBRARIES})
target_include_directories(model_client_test PRIVATE ${JSONCPP_INCLUDE_DIRS})

# ────────────────────────────────────────────────────────────────────────────
# Fixture generation for VBIN / system-map tests
#
# Strategy:
#   * The two real sample VBINs (axilite_{sim,emu}.vbin) are referenced by
#     absolute path — they are large and authoritative, so we do not copy them.
#   * Small, self-contained VBIN fixtures (good/broken) are BUILT here from the
#     checked-in source trees under fixtures/ using `tar`/`gzip`, mirroring the
#     approach vrt/tests/CMakeLists.txt uses.  This keeps binaries out of git
#     while making the tests hermetic.
# All paths are handed to the test via a generated fixtures_paths.h header.
# ────────────────────────────────────────────────────────────────────────────

find_program(TAR_EXECUTABLE  tar  REQUIRED)
find_program(GZIP_EXECUTABLE gzip REQUIRED)

set(FIXTURE_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
set(FIXTURE_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated_fixtures)
file(MAKE_DIRECTORY ${FIXTURE_OUT_DIR})

# 1. good_emu.vbin — a well-formed raw-tar Emulation VBIN.
add_custom_command(
  OUTPUT ${FIXTURE_OUT_DIR}/good_emu.vbin
  COMMAND ${TAR_EXECUTABLE} cf ${FIXTURE_OUT_DIR}/good_emu.vbin
          -C ${FIXTURE_SRC_DIR}/good_emu system_map.xml vpp_emu emu_manifest.json
  DEPENDS ${FIXTURE_SRC_DIR}/good_emu/system_map.xml
          ${FIXTURE_SRC_DIR}/good_emu/vpp_emu
          ${FIXTURE_SRC_DIR}/good_emu/emu_manifest.json
  COMMENT "Building fixture good_emu.vbin"
)

# 2. good_emu.vbin.gz — gzip-compressed copy exercising the gzip path.
add_custom_command(
  OUTPUT ${FIXTURE_OUT_DIR}/good_emu.vbin.gz
  COMMAND ${GZIP_EXECUTABLE} -c ${FIXTURE_OUT_DIR}/good_emu.vbin
          > ${FIXTURE_OUT_DIR}/good_emu.vbin.gz
  DEPENDS ${FIXTURE_OUT_DIR}/good_emu.vbin
  COMMENT "Building fixture good_emu.vbin.gz"
)

# 3. no_map.vbin — a tar with the executable but no system_map.xml.
add_custom_command(
  OUTPUT ${FIXTURE_OUT_DIR}/no_map.vbin
  COMMAND ${TAR_EXECUTABLE} cf ${FIXTURE_OUT_DIR}/no_map.vbin
          -C ${FIXTURE_SRC_DIR}/good_emu vpp_emu emu_manifest.json
  DEPENDS ${FIXTURE_SRC_DIR}/good_emu/vpp_emu
          ${FIXTURE_SRC_DIR}/good_emu/emu_manifest.json
  COMMENT "Building fixture no_map.vbin"
)

# 4. no_exec.vbin — an Emulation map but no vpp_emu executable.
add_custom_command(
  OUTPUT ${FIXTURE_OUT_DIR}/no_exec.vbin
  COMMAND ${TAR_EXECUTABLE} cf ${FIXTURE_OUT_DIR}/no_exec.vbin
          -C ${FIXTURE_SRC_DIR}/good_emu system_map.xml emu_manifest.json
  DEPENDS ${FIXTURE_SRC_DIR}/good_emu/system_map.xml
          ${FIXTURE_SRC_DIR}/good_emu/emu_manifest.json
  COMMENT "Building fixture no_exec.vbin"
)

# 5. traversal.vbin — a tar whose member escapes via ../ (must be rejected).
#    Built with a transform so the on-disk source stays inside the tree.
add_custom_command(
  OUTPUT ${FIXTURE_OUT_DIR}/traversal.vbin
  COMMAND ${TAR_EXECUTABLE} cf ${FIXTURE_OUT_DIR}/traversal.vbin
          --transform=s,^system_map.xml,../escape.xml,
          -C ${FIXTURE_SRC_DIR}/good_emu system_map.xml
  DEPENDS ${FIXTURE_SRC_DIR}/good_emu/system_map.xml
  COMMENT "Building fixture traversal.vbin"
)

# 5b. no_exec_sim.vbin — a Simulation map but no vpp_sim executable.
add_custom_command(
  OUTPUT ${FIXTURE_OUT_DIR}/no_exec_sim.vbin
  COMMAND ${TAR_EXECUTABLE} cf ${FIXTURE_OUT_DIR}/no_exec_sim.vbin
          -C ${FIXTURE_SRC_DIR}/good_sim system_map.xml
  DEPENDS ${FIXTURE_SRC_DIR}/good_sim/system_map.xml
  COMMENT "Building fixture no_exec_sim.vbin"
)

# 6. hardware.vbin — a well-formed tar whose map targets Hardware (must be
#    rejected by the daemon as out of scope).
add_custom_command(
  OUTPUT ${FIXTURE_OUT_DIR}/hardware.vbin
  COMMAND ${TAR_EXECUTABLE} cf ${FIXTURE_OUT_DIR}/hardware.vbin
          -C ${FIXTURE_SRC_DIR}/hardware system_map.xml
  DEPENDS ${FIXTURE_SRC_DIR}/hardware/system_map.xml
  COMMENT "Building fixture hardware.vbin"
)

# 7. longname.vbin — good emu VBIN plus a member with a >100 char path, forcing
#    a GNU 'L' long-name header so that code path is exercised.
add_custom_command(
  OUTPUT ${FIXTURE_OUT_DIR}/longname.vbin
  COMMAND ${CMAKE_COMMAND} -E make_directory
          ${FIXTURE_OUT_DIR}/longname_src/this_is_a_deliberately_very_long_directory_name_to_exceed_one_hundred_chars/and_then_even_more_nesting_here
  COMMAND ${CMAKE_COMMAND} -E copy ${FIXTURE_SRC_DIR}/good_emu/system_map.xml
          ${FIXTURE_OUT_DIR}/longname_src/system_map.xml
  COMMAND ${CMAKE_COMMAND} -E copy ${FIXTURE_SRC_DIR}/good_emu/vpp_emu
          ${FIXTURE_OUT_DIR}/longname_src/vpp_emu
  COMMAND ${CMAKE_COMMAND} -E copy ${FIXTURE_SRC_DIR}/good_emu/emu_manifest.json
          ${FIXTURE_OUT_DIR}/longname_src/this_is_a_deliberately_very_long_directory_name_to_exceed_one_hundred_chars/and_then_even_more_nesting_here/payload.bin
  COMMAND ${TAR_EXECUTABLE} --format=gnu -cf ${FIXTURE_OUT_DIR}/longname.vbin
          -C ${FIXTURE_OUT_DIR}/longname_src .
  DEPENDS ${FIXTURE_SRC_DIR}/good_emu/system_map.xml
          ${FIXTURE_SRC_DIR}/good_emu/vpp_emu
          ${FIXTURE_SRC_DIR}/good_emu/emu_manifest.json
  COMMENT "Building fixture longname.vbin"
)

# 8. absolute.vbin — a tar whose member has an absolute path (must be rejected).
add_custom_command(
  OUTPUT ${FIXTURE_OUT_DIR}/absolute.vbin
  COMMAND ${TAR_EXECUTABLE} cf ${FIXTURE_OUT_DIR}/absolute.vbin
          --transform=s,^system_map.xml,/abs/escape.xml,
          --absolute-names
          -C ${FIXTURE_SRC_DIR}/good_emu system_map.xml
  DEPENDS ${FIXTURE_SRC_DIR}/good_emu/system_map.xml
  COMMENT "Building fixture absolute.vbin"
)

add_custom_target(slash_emu_fixtures ALL
  DEPENDS
    ${FIXTURE_OUT_DIR}/good_emu.vbin
    ${FIXTURE_OUT_DIR}/good_emu.vbin.gz
    ${FIXTURE_OUT_DIR}/no_map.vbin
    ${FIXTURE_OUT_DIR}/no_exec.vbin
    ${FIXTURE_OUT_DIR}/no_exec_sim.vbin
    ${FIXTURE_OUT_DIR}/traversal.vbin
    ${FIXTURE_OUT_DIR}/absolute.vbin
    ${FIXTURE_OUT_DIR}/hardware.vbin
    ${FIXTURE_OUT_DIR}/longname.vbin
)

# Generate a header exposing all fixture paths as string constants.
set(REAL_SIM_VBIN /home/jopdenho/SLASH/examples/00_axilite/build/axilite_sim.vbin)
set(REAL_EMU_VBIN /home/jopdenho/SLASH/examples/00_axilite/build/axilite_emu.vbin)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/fixtures_paths.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/fixtures_paths.h
  @ONLY
)

add_slash_emu_test(system_map_test system_map_test.cpp)
target_include_directories(system_map_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_slash_emu_test(vbin_test vbin_test.cpp)
target_include_directories(vbin_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(vbin_test slash_emu_fixtures)
