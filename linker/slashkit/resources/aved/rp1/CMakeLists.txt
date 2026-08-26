# Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
#
# CMake build for RP1 baremetal firmware (ARM Cortex-R5 core 1).

cmake_minimum_required(VERSION 3.5.0)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_ASM_COMPILER_WORKS TRUE)

project(rp1 C ASM)

set(CPU_FLAGS "-mcpu=cortex-r5 -mfloat-abi=soft")
set(CMAKE_C_FLAGS "${CPU_FLAGS} -Wall -Werror -O2 -ffreestanding -nostdlib")
set(CMAKE_ASM_FLAGS "${CPU_FLAGS}")

option(QEMU_SEMIHOSTING "Enable ARM semihosting for QEMU testing" OFF)
if(QEMU_SEMIHOSTING)
    # The semihosting characterization suite shares the real 64 KB ATCM
    # layout with firmware; optimize it for size so tests do not mask overflow.
    string(REPLACE "-O2" "-Os" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
endif()

# Platform configuration is deliberately fail-closed: QEMU may select its
# explicit fixture, while hardware must name a generated R5_1 BSP contract.
# Keeping this gate in CMake protects direct builds that bypass build-rp1.sh.
set(RP1_PLATFORM_CONFIG_HEADER "" CACHE FILEPATH
    "Generated R5_1 platform configuration header")
if(QEMU_SEMIHOSTING)
    if(NOT RP1_PLATFORM_CONFIG_HEADER)
        set(RP1_PLATFORM_CONFIG_HEADER
            "${CMAKE_CURRENT_SOURCE_DIR}/config/qemu/rp1_platform_config.h")
    endif()
elseif(NOT RP1_PLATFORM_CONFIG_HEADER)
    message(FATAL_ERROR
        "Hardware RP1 builds require -DRP1_PLATFORM_CONFIG_HEADER=<generated "
        "rp1_platform_config.h>; run build-rp1.sh with XSA or "
        "RP1_BSP_XPARAMETERS")
endif()
if(NOT EXISTS "${RP1_PLATFORM_CONFIG_HEADER}")
    message(FATAL_ERROR
        "RP1 platform configuration does not exist: "
        "${RP1_PLATFORM_CONFIG_HEADER}")
endif()
file(READ "${RP1_PLATFORM_CONFIG_HEADER}" RP1_PLATFORM_CONFIG_CONTENT)
if(NOT QEMU_SEMIHOSTING AND
   NOT RP1_PLATFORM_CONFIG_CONTENT MATCHES
       "RP1_PLATFORM_CONFIG_GENERATED[ \t]+1u")
    message(FATAL_ERROR
        "Hardware RP1 build rejected a fixture/non-generated platform header")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${RP1_PLATFORM_CONFIG_HEADER}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/rp1_platform_config.h"
    COPYONLY)

set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/rp1_lscript.ld")

set(RP1_MEMTEST_SOURCES
    src/rp1_boot.S
    src/rp1_memtest.c
    src/rp1_memtest_main.c
)

# Common sources: boot stub + stores + dispatch loop + outer loop.
set(RP1_SOURCES
    src/rp1_boot.S
    src/rp1_hal.c
    src/rp1_store.c
    src/rp1_scheduler.c
    src/rp1_loop.c
    src/rp1_run.c
    src/rp1_pdi.c
)

# In semihosting mode rp1_test.c provides rp1_main(); otherwise rp1_main.c does.
# rp1_graph_test.c hosts the end-to-end scanner tests and is only linked
# when semihosting is enabled.
if(QEMU_SEMIHOSTING)
    list(APPEND RP1_SOURCES src/rp1_test.c src/rp1_graph_test.c)
else()
    list(APPEND RP1_SOURCES src/rp1_main.c)
endif()

add_executable(rp1.elf ${RP1_SOURCES})

add_executable(rp1_memtest.elf ${RP1_MEMTEST_SOURCES})

# Use the protocol UAPI header staged into this package resource tree.
set(SLASH_LIBSLASH_INCLUDE "" CACHE PATH
    "Directory containing slash/uapi/rp1_protocol.h")
if(NOT SLASH_LIBSLASH_INCLUDE)
    set(SLASH_LIBSLASH_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
endif()
if(NOT EXISTS "${SLASH_LIBSLASH_INCLUDE}/slash/uapi/rp1_protocol.h")
    message(FATAL_ERROR
        "Required packaged RP1 protocol header is missing: "
        "${SLASH_LIBSLASH_INCLUDE}/slash/uapi/rp1_protocol.h")
endif()
target_include_directories(rp1.elf PRIVATE
    "${SLASH_LIBSLASH_INCLUDE}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(QEMU_SEMIHOSTING)
    target_compile_definitions(rp1.elf PRIVATE QEMU_SEMIHOSTING)
endif()

# -Wl,--build-id=none suppresses the .note.gnu.build-id section. Newer
# gccs (e.g. Vitis 2025.1's arm-none-eabi-gcc 13.3) emit it by default and
# ld places it at LMA 0, where it collides with the .vectors section
# anchored at the start of r5_1_atcm in rp1_lscript.ld.
target_link_libraries(rp1.elf
    "-T${LINKER_SCRIPT}"
    "-nostdlib"
    "-Wl,--build-id=none"
    "-lgcc"
)

target_link_libraries(rp1_memtest.elf
    "-T${LINKER_SCRIPT}"
    "-nostdlib"
    "-Wl,--build-id=none"
    "-lgcc"
)

set_target_properties(rp1.elf PROPERTIES
    LINK_DEPENDS "${LINKER_SCRIPT}"
)

set_target_properties(rp1_memtest.elf PROPERTIES
    LINK_DEPENDS "${LINKER_SCRIPT}"
)
