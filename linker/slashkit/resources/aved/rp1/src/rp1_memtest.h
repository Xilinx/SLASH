/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#ifndef RP1_MEMTEST_H
#define RP1_MEMTEST_H

#include <stdint.h>

#define RP1_MEMTEST_SHARED_BASE        0x30000000UL
#define RP1_MEMTEST_WORD_SEED          0x13579BDFUL
#define RP1_MEMTEST_TOTAL_BYTES        (1UL * 1024UL * 1024UL)
#define RP1_MEMTEST_WORD_COUNT         (RP1_MEMTEST_TOTAL_BYTES / sizeof(uint32_t))

void rp1_memtest_run(void);

#endif /* RP1_MEMTEST_H */
