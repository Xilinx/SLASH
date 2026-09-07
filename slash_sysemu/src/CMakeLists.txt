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

# -------- slash_sysemu_core library --------

find_package(CLI11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(INIH REQUIRED inih)
pkg_check_modules(LIBXML2 REQUIRED libxml-2.0)
# libzmq drives the model process; matches how vrt/CMakeLists.txt links it.
pkg_check_modules(PC_ZMQ REQUIRED IMPORTED_TARGET libzmq)
# jsoncpp encodes/decodes the model protocol frames.
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
# libsystemd: sd-event main loop, sd_notify (readiness/watchdog/stopping), and
# sd_journal logging.  The daemon is designed to run only under systemd, so this
# is a hard dependency (present in both packaging Dockerfiles).
pkg_check_modules(LIBSYSTEMD REQUIRED libsystemd)
# zlib is used for gzip-compressed VBIN containers. It is already a transitive
# dependency of vrt (see vrt/src/vrtbin.cpp) and ships with every toolchain we
# target, so adding it here does not introduce a genuinely new system package.
find_package(ZLIB REQUIRED)
# std::thread in the model process monitor + ModelClient serialisation.
find_package(Threads REQUIRED)

# The PF2 control-device IOCTL ABI (struct slash_ioctl_bar_info / _bar_fd_request
# / _device_info and the SLASH_CTLDEV_IOCTL_* command numbers) is the OFFICIAL
# kernel/userspace UAPI shipped with libslash.  slash_sysemu consumes it directly
# rather than mirroring the structs, so the daemon's wire format is guaranteed to
# stay byte-for-byte identical with what libslash (and the real driver) use.
# Point SLASH_UAPI_INCLUDE_DIR at an installed libslash to override the in-tree
# copy.
set(SLASH_UAPI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../driver/libslash/include"
    CACHE PATH "Include directory containing <slash/uapi/slash_interface.h>")
if(NOT EXISTS "${SLASH_UAPI_INCLUDE_DIR}/slash/uapi/slash_interface.h")
  message(FATAL_ERROR
    "slash_sysemu requires the libslash UAPI header "
    "<slash/uapi/slash_interface.h>; not found under "
    "SLASH_UAPI_INCLUDE_DIR='${SLASH_UAPI_INCLUDE_DIR}'")
endif()
if(NOT EXISTS "${SLASH_UAPI_INCLUDE_DIR}/slash/uapi/slash_hotplug.h")
  message(FATAL_ERROR
    "slash_sysemu requires the libslash hotplug UAPI header "
    "<slash/uapi/slash_hotplug.h>; not found under "
    "SLASH_UAPI_INCLUDE_DIR='${SLASH_UAPI_INCLUDE_DIR}'")
endif()
if(NOT EXISTS "${SLASH_UAPI_INCLUDE_DIR}/slash/uapi/slash_sysemu.h")
  message(FATAL_ERROR
    "slash_sysemu requires the shared sysemu UAPI header "
    "<slash/uapi/slash_sysemu.h>; not found under "
    "SLASH_UAPI_INCLUDE_DIR='${SLASH_UAPI_INCLUDE_DIR}'")
endif()

add_library(slash_sysemu_core STATIC
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
  ctl_subsystem.cpp
  qdma_subsystem.cpp
  accelerator.cpp
  hotplug_subsystem.cpp
)

target_include_directories(slash_sysemu_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SLASH_UAPI_INCLUDE_DIR}
  PRIVATE
    ${INIH_INCLUDE_DIRS}
    ${LIBXML2_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${LIBSYSTEMD_INCLUDE_DIRS}
)

target_compile_features(slash_sysemu_core PUBLIC cxx_std_20)

target_compile_options(slash_sysemu_core PRIVATE -Wall -Wextra -Werror ${LIBXML2_CFLAGS_OTHER})

target_link_libraries(slash_sysemu_core
  PUBLIC
    CLI11::CLI11
  PRIVATE
    ${INIH_LIBRARIES}
    ${LIBXML2_LIBRARIES}
    ZLIB::ZLIB
    PkgConfig::PC_ZMQ
    ${JSONCPP_LIBRARIES}
    ${LIBSYSTEMD_LIBRARIES}
    Threads::Threads
)

# -------- default model server + default VBIN --------
# Builds the `vpp_sim` model from source and tars it (with a committed system
# map) into the default VBIN.  Exports SLASH_SYSEMU_DEFAULT_VBIN_BUILD (the
# build-tree artifact path) to this scope for the compile-time default + tests.
add_subdirectory(model)

# Bubble the built default-VBIN path up to the top-level scope so the tests
# subdirectory (a sibling of src/) can reference the artifact.
set(SLASH_SYSEMU_DEFAULT_VBIN_BUILD "${SLASH_SYSEMU_DEFAULT_VBIN_BUILD}" PARENT_SCOPE)

# The daemon's compile-time default for --default-vbin: the installed location of
# the default VBIN (see src/model/CMakeLists.txt).  Overridable at runtime by the
# SLASH_SYSEMU_DEFAULT_VBIN env var and the --default-vbin flag.
target_compile_definitions(slash_sysemu_core PRIVATE
  SLASH_SYSEMU_DEFAULT_VBIN_PATH="${CMAKE_INSTALL_PREFIX}/lib/slash-sysemu/default.vbin")

# -------- slash_sysemud executable --------

add_executable(slash_sysemud
  main.cpp
)

target_link_libraries(slash_sysemud PRIVATE slash_sysemu_core)

target_compile_options(slash_sysemud PRIVATE -Wall -Wextra -Werror)

# -------- installation --------
if(SLASH_SYSEMU_INSTALL)
  install(TARGETS slash_sysemud RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
