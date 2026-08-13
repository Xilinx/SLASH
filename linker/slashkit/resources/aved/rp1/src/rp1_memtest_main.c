/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Entry point for the standalone RP1 DDR memory test image.
 */

#include "rp1_memtest.h"

static inline void wfi(void)
{
    __asm__ volatile("wfi" ::: "memory");
}

void rp1_main(void)
{
    rp1_memtest_run();

    for (;;)
        wfi();
}
