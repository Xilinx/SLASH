/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Linker script for RP1 baremetal firmware (ARM Cortex-R5 core 1).
 *
 * Memory regions use R5-1 local addresses:
 *   ATCM at 0x0      (64KB) - boot vectors + code
 *   BTCM at 0x20000  (64KB) - stack + data
 *   DDR private at 0x10000000 (512MB) - RP1-only scratch/staging region
 *   DDR shared  at 0x30000000  (64MB) - BAR-visible control/queue region
 */

_STACK_SIZE = 0x1000;   /* 4KB stack - sufficient for baremetal bringup */

MEMORY
{
    r5_1_atcm : ORIGIN = 0x00000000, LENGTH = 0x10000    /* 64KB */
    r5_1_btcm : ORIGIN = 0x00020000, LENGTH = 0x10000    /* 64KB */
    ddr_private : ORIGIN = 0x10000000, LENGTH = 0x20000000 /* 512MB */
    ddr_shared  : ORIGIN = 0x30000000, LENGTH = 0x04000000 /* 64MB */
}

ENTRY(_boot)

SECTIONS
{
    .vectors : {
        KEEP(*(.vectors))
    } > r5_1_atcm

    .text : {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
    } > r5_1_atcm

    /*
     * Writable scheduler adjacency storage uses otherwise idle ATCM capacity.
     * It is initialized from the accepted BTCM node snapshot before execution.
     */
    .atcm_data (NOLOAD) : {
        . = ALIGN(4);
        __atcm_data_start = .;
        *(.atcm_data)
        . = ALIGN(4);
        __atcm_data_end = .;
    } > r5_1_atcm

    ASSERT(__atcm_data_end <= ORIGIN(r5_1_atcm) + LENGTH(r5_1_atcm),
           "RP1 code and scheduler index exceed 64 KB ATCM")

    .data : {
        __data_start = .;
        *(.data)
        *(.data.*)
        __data_end = .;
    } > r5_1_btcm

    /* Hot BTCM data: node snapshot, barriers, loop counters, inflight table.
     * Explicitly placed before BSS so the complete footprint is checkable. */
    .btcm (NOLOAD) : {
        . = ALIGN(4);
        __btcm_snapshot_start = .;
        *(.btcm)
        . = ALIGN(4);
        __btcm_snapshot_end = .;
    } > r5_1_btcm

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > r5_1_btcm

    .stack (NOLOAD) : {
        . = ALIGN(16);
        _stack_end = .;
        . += _STACK_SIZE;
        . = ALIGN(16);
        __stack_top = .;
    } > r5_1_btcm

    ASSERT(__stack_top <= ORIGIN(r5_1_btcm) + LENGTH(r5_1_btcm),
           "RP1 BTCM data and stack exceed 64 KB")

    /* BAR-visible shared DDR region for control/status structures. */
    .rp1_shared (NOLOAD) : {
        *(.rp1_shared)
    } > ddr_shared

    _end = .;
}
