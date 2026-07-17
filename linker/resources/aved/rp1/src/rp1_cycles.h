/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Cortex-R5 PMU cycle counter helpers.
 */

#ifndef RP1_CYCLES_H
#define RP1_CYCLES_H

#include <stdint.h>

static inline void rp1_pmu_init(void)
{
    uint32_t pmcr;

    __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));

    /*
     * Enable the PMU, reset PMCCNTR, and keep the divider enabled so the
     * 32-bit counter ticks once per 64 CPU cycles.
     */
    pmcr |= (1u << 0) | (1u << 2) | (1u << 3);
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(pmcr) : "memory");

    /* Enable the cycle counter (PMCNTENSET.C, bit 31). */
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(1u << 31) : "memory");
}

static inline uint32_t rp1_cycles(void)
{
    uint32_t cycles;

    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(cycles));
    return cycles;
}

#endif /* RP1_CYCLES_H */
