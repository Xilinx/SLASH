/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include "rp1_hal.h"

#include <stddef.h>

static rp1_hal_hooks_t g_hooks;

void rp1_hal_set_hooks(const rp1_hal_hooks_t *hooks)
{
    if (hooks)
        g_hooks = *hooks;
    else
        rp1_hal_reset_hooks();
}

void rp1_hal_reset_hooks(void)
{
    g_hooks.read32 = NULL;
    g_hooks.write32 = NULL;
    g_hooks.barrier = NULL;
    g_hooks.cycles = NULL;
    g_hooks.context = NULL;
}

uint32_t rp1_mmio_read32(uintptr_t address)
{
    if (g_hooks.read32)
        return g_hooks.read32(address, g_hooks.context);
    return *(volatile uint32_t *)address;
}

void rp1_mmio_write32(uintptr_t address, uint32_t value)
{
    if (g_hooks.write32) {
        g_hooks.write32(address, value, g_hooks.context);
        return;
    }
    *(volatile uint32_t *)address = value;
}

void rp1_barrier(void)
{
    if (g_hooks.barrier) {
        g_hooks.barrier(g_hooks.context);
        return;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

void rp1_pmu_init(void)
{
    uint32_t pmcr;

    __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
    /*
     * Reset and enable the 32-bit PMCCNTR with its architected divide-by-64
     * tick. Kernel and PDI deadlines then share one frequency-derived clock,
     * independent of scanner load or MMIO poll latency.
     */
    pmcr |= (1u << 0) | (1u << 2) | (1u << 3);
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(pmcr) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(1u << 31) : "memory");
}

uint32_t rp1_cycles(void)
{
    uint32_t cycles;

    if (g_hooks.cycles)
        return g_hooks.cycles(g_hooks.context);
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(cycles));
    return cycles;
}

_Static_assert(RP1_DEFAULT_KERNEL_TIMEOUT_TICKS != 0u,
               "kernel timeout must be non-zero");
_Static_assert(RP1_DEFAULT_PDI_TIMEOUT_TICKS != 0u,
               "PDI timeout must be non-zero");
