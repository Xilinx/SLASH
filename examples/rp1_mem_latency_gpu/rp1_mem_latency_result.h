/*
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef SLASH_EXAMPLES_RP1_MEM_LATENCY_GPU_RESULT_H
#define SLASH_EXAMPLES_RP1_MEM_LATENCY_GPU_RESULT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RP1_MEM_LATENCY_STATUS_PENDING  = 0,
    RP1_MEM_LATENCY_STATUS_PASS     = 1,
    RP1_MEM_LATENCY_STATUS_BAD_ARGS = 2,
    RP1_MEM_LATENCY_STATUS_MISMATCH = 3,
};

enum {
    RP1_MEM_LATENCY_MODE_READ  = 1u << 0,
    RP1_MEM_LATENCY_MODE_WRITE = 1u << 1,
    RP1_MEM_LATENCY_MODE_RW    = RP1_MEM_LATENCY_MODE_READ |
                                  RP1_MEM_LATENCY_MODE_WRITE,
};

typedef struct rp1_mem_latency_config {
    uint64_t scratch_bar_offset; /* BAR4-relative byte offset */
    uint64_t scratch_bytes;
    uint32_t iterations;
    uint32_t warmup;
    uint32_t stride_bytes;
    uint32_t mode;
} rp1_mem_latency_config_t;

typedef struct rp1_mem_latency_result {
    uint32_t status;
    uint32_t mode;
    uint32_t iterations;
    uint32_t warmup;
    uint64_t scratch_bar_offset;
    uint64_t scratch_bytes;
    uint32_t stride_bytes;
    uint32_t sample_count;
    uint32_t magic_seen;
    uint32_t first_mismatch_iter;
    uint32_t first_mismatch_expected;
    uint32_t first_mismatch_observed;
    uint64_t checksum;
} rp1_mem_latency_result_t;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SLASH_EXAMPLES_RP1_MEM_LATENCY_GPU_RESULT_H */
