/**
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

#include <hip/hip_runtime.h>
#include <stdint.h>

/**
 * AXI-Lite register map for slash_add (Vivado HLS s_axilite layout).
 *
 * void slash_add(uint32_t a, uint32_t b, uint32_t &result)
 *
 * Byte offsets — confirm from xslash_add.h after HLS synthesis:
 *   hls/_build/slash_add/solution/impl/ip/drivers/slash_add_v1_0/src/xslash_add.h
 *
 *   0x00  ap_ctrl   bit[0]=ap_start, bit[1]=ap_done, bit[2]=ap_idle
 *   0x10  a
 *   0x18  b
 *   0x20  result
 */
static constexpr uint32_t kCtrl   = 0x00 / sizeof(uint32_t);
static constexpr uint32_t kArgA   = 0x10 / sizeof(uint32_t);
static constexpr uint32_t kArgB   = 0x18 / sizeof(uint32_t);
static constexpr uint32_t kResult = 0x20 / sizeof(uint32_t);

static constexpr uint32_t kApStart = 1u << 0;
static constexpr uint32_t kApDone  = 1u << 1;

/**
 * GPU kernel that dispatches a slash_add FPGA kernel via PCIe P2P BAR writes.
 *
 * @param bar          GPU virtual address of the SLASH BAR0 (mapped via
 *                     hsa_amd_interop_map_buffer). Declared volatile so the
 *                     compiler cannot cache reads.
 * @param a            First operand.
 * @param b            Second operand.
 * @param result_out   Device memory buffer that receives the FPGA result.
 * @param poll_count_out  Device memory buffer that receives the ap_done poll count.
 *
 * Only one thread (block 0, thread 0) performs the work. The kernel is
 * inherently sequential: write args -> ap_start -> poll ap_done -> read result.
 *
 * __threadfence_system() is used before and after each PCIe write/read to
 * ensure the GPU's write-combine buffers are flushed to the fabric before
 * dependent operations are issued.
 */
__global__ void slash_add_gpu(
    volatile uint32_t* __restrict__ bar,
    uint32_t a,
    uint32_t b,
    uint32_t* __restrict__ result_out,
    uint32_t* __restrict__ poll_count_out
) {
    if (threadIdx.x != 0 || blockIdx.x != 0)
        return;

    /* Write operands to SLASH AXI-Lite registers. */
    bar[kArgA] = a;
    bar[kArgB] = b;
    /* Flush write-combine buffers to PCIe before pulsing ap_start. */
    __threadfence_system();

    /* Pulse ap_start. */
    bar[kCtrl] = kApStart;
    __threadfence_system();

    /* Poll ap_done (bit 1 of ap_ctrl). */
    uint32_t count = 0;
    uint32_t ctrl;
    do {
        __threadfence_system();
        ctrl = bar[kCtrl];
        ++count;
    } while ((ctrl & kApDone) == 0);

    /* Read result register. */
    __threadfence_system();
    uint32_t result = bar[kResult];

    if (result_out)     *result_out     = result;
    if (poll_count_out) *poll_count_out = count;
}
