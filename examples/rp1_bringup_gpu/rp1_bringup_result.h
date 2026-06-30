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

/*
 * rp1_bringup_result.h — small POD struct shared between the host harness
 * (rp1_bringup_gpu.cpp) and the HIP kernel (rp1_bringup_kernel.hip).
 *
 * The host hipMallocs one of these, the kernel populates it, the host
 * hipMemcpys it back and prints / interprets the result.
 *
 * Plain C / freestanding-friendly: only <stdint.h> is required, so the
 * header is safe to include from both the host C++ TU and the HIP TU.
 */

#ifndef SLASH_EXAMPLES_RP1_BRINGUP_GPU_RESULT_H
#define SLASH_EXAMPLES_RP1_BRINGUP_GPU_RESULT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* High-level outcome of the bringup run, written by the GPU kernel. */
enum {
    /* Initial value before the kernel touches the result. */
    RP1_BRINGUP_STATUS_PENDING  = 0,

    /* Graph completed and signal slot 0 contained 0xDEADBEEF. */
    RP1_BRINGUP_STATUS_PASS     = 1,

    /* Control block magic was not "SQR1" — RP1 firmware not loaded
     * or its DDR window not visible at BAR4 + 64 MiB. */
    RP1_BRINGUP_STATUS_NO_FW    = 2,

    /* Graph completed (graph_done_seq advanced) but signal slot 0
     * read back something other than 0xDEADBEEF. */
    RP1_BRINGUP_STATUS_BAD_SLOT = 3,

    /* graph_done_seq never advanced within the polling deadline. */
    RP1_BRINGUP_STATUS_TIMEOUT  = 4,
};

/* Diagnostics returned to the host after each run. All fields are written
 * by the GPU kernel (zeroed beforehand by hipMemset). */
typedef struct rp1_bringup_result {
    uint32_t status;          /* RP1_BRINGUP_STATUS_*                 */
    uint32_t magic_seen;      /* ctrl->magic at entry                 */
    uint32_t slot0;           /* sigs[0].value at completion          */
    uint32_t graph_done_seq;  /* ctrl->graph_done_seq at completion   */
    uint32_t rp1_state;       /* ctrl->rp1_state at completion        */
    uint32_t polls;           /* poll iterations until graph_done_seq */
} rp1_bringup_result_t;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SLASH_EXAMPLES_RP1_BRINGUP_GPU_RESULT_H */
