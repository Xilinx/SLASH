/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Minimal RP1 memory-pattern writer for xsdb-loaded bring-up.
 */

#include "rp1_memtest.h"

#include <stdint.h>

static inline void dsb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

void rp1_memtest_run(void)
{
    volatile uint32_t *shared_words =
        (volatile uint32_t *)(uintptr_t)RP1_MEMTEST_SHARED_BASE;

    for (uint32_t word_index = 0; word_index < RP1_MEMTEST_WORD_COUNT; ++word_index)
        shared_words[word_index] = RP1_MEMTEST_WORD_SEED + word_index;

    dsb();
}
