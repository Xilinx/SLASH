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

# -------- slash_emu_core library --------

find_package(CLI11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(INIH REQUIRED inih)
pkg_check_modules(LIBXML2 REQUIRED libxml-2.0)
# libzmq drives the model process; matches how vrt/CMakeLists.txt links it.
pkg_check_modules(PC_ZMQ REQUIRED IMPORTED_TARGET libzmq)
# jsoncpp encodes/decodes the model protocol frames.
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
# zlib is used for gzip-compressed VBIN containers. It is already a transitive
# dependency of vrt (see vrt/src/vrtbin.cpp) and ships with every toolchain we
# target, so adding it here does not introduce a genuinely new system package.
find_package(ZLIB REQUIRED)
# std::thread in the model process monitor + ModelClient serialisation.
find_package(Threads REQUIRED)

add_library(slash_emu_core STATIC
  daemon.cpp
  transport.cpp
  config.cpp
  system_map.cpp
  vbin.cpp
  vbin_store.cpp
  model_client.cpp
  model_process.cpp
  reconfigure.cpp
  bar_memfd.cpp
  model_control_workers.cpp
)

target_include_directories(slash_emu_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE
    ${INIH_INCLUDE_DIRS}
    ${LIBXML2_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
)

target_compile_features(slash_emu_core PUBLIC cxx_std_20)

target_compile_options(slash_emu_core PRIVATE -Wall -Wextra -Werror ${LIBXML2_CFLAGS_OTHER})

target_link_libraries(slash_emu_core
  PUBLIC
    CLI11::CLI11
  PRIVATE
    ${INIH_LIBRARIES}
    ${LIBXML2_LIBRARIES}
    ZLIB::ZLIB
    PkgConfig::PC_ZMQ
    ${JSONCPP_LIBRARIES}
    Threads::Threads
)

# -------- slash_emu_daemon executable --------

add_executable(slash_emu_daemon
  main.cpp
)

target_link_libraries(slash_emu_daemon PRIVATE slash_emu_core)

target_compile_options(slash_emu_daemon PRIVATE -Wall -Wextra -Werror)
