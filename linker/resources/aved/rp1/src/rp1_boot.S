/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Minimal ARM Cortex-R5 boot stub for RP1 (baremetal).
 * Sets up the vector table, zeroes BSS, and jumps to rp1_main().
 */

.section .vectors, "ax"
.global _boot
_boot:
    b       _start          /* Reset */
    b       .               /* Undefined Instruction */
    b       .               /* SVC */
    b       .               /* Prefetch Abort */
    b       .               /* Data Abort */
    b       .               /* Reserved */
    b       .               /* IRQ */
    b       .               /* FIQ */

.section .text, "ax"
.global _start
_start:
    /* Set up SVC mode stack */
    ldr     sp, =__stack_top

    /* Zero the BSS section */
    ldr     r0, =__bss_start__
    ldr     r1, =__bss_end__
    mov     r2, #0
bss_loop:
    cmp     r0, r1
    strlt   r2, [r0], #4
    blt     bss_loop

    /* Jump to C entry point */
    bl      rp1_main

    /* Should never return */
hang:
    wfi
    b       hang
