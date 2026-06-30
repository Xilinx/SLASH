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

# Bring-up mode: while the GCQ doorbell (irq_sq) is not yet wired, RP1 has
# nothing to wake it from wfi() when the host bumps graph_seq.  This flag
# disables both wfi() calls so the R5 spins instead.  Remove (or set OFF)
# once the doorbell is in place.
option(RP1_POLLING_BRINGUP "Disable wfi() so RP1 polls (bring-up only)" OFF)

set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/rp1_lscript.ld")

set(RP1_MEMTEST_SOURCES
    src/rp1_boot.S
    src/rp1_memtest.c
    src/rp1_memtest_main.c
)

# Common sources: boot stub + stores + dispatch loop + outer loop.
set(RP1_SOURCES
    src/rp1_boot.S
    src/rp1_store.c
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

# Pick up the shared RP1 protocol UAPI header
# (driver/libslash/include/slash/uapi/rp1_protocol.h).
set(SLASH_LIBSLASH_INCLUDE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../driver/libslash/include")
target_include_directories(rp1.elf PRIVATE "${SLASH_LIBSLASH_INCLUDE}")

if(QEMU_SEMIHOSTING)
    target_compile_definitions(rp1.elf PRIVATE QEMU_SEMIHOSTING)
endif()

if(RP1_POLLING_BRINGUP)
    target_compile_definitions(rp1.elf PRIVATE RP1_POLLING_BRINGUP)
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
