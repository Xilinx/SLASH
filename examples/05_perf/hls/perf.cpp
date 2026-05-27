/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <ap_int.h>
#include <stdint.h>

#define DATA_WIDTH 256
typedef ap_uint<DATA_WIDTH> uint256_t;

void perf(
    uint256_t* mem_ptr,
    ap_uint<32> reps,
    ap_uint<32> length,
    ap_uint<32> wr,
    ap_uint<32>& out_acc
) {
#pragma HLS INTERFACE m_axi port=mem_ptr offset=slave bundle=gmem0 max_read_burst_length=64 max_write_burst_length=64 depth=536870912
#pragma HLS INTERFACE s_axilite port=mem_ptr   bundle=control
#pragma HLS INTERFACE s_axilite port=wr        bundle=control
#pragma HLS INTERFACE s_axilite port=reps      bundle=control
#pragma HLS INTERFACE s_axilite port=length    bundle=control
#pragma HLS INTERFACE s_axilite port=out_acc   bundle=control
#pragma HLS INTERFACE s_axilite port=return    bundle=control

    ap_uint<32> acc = 0;
    for (uint32_t rep = 0; rep < reps; rep++) {
        for (uint32_t i = 0; i < length; i++) {
        #pragma HLS PIPELINE II=1
        #pragma HLS LOOP_FLATTEN
            if (wr == 0) {
                mem_ptr[i] = i;
            } else {
                uint256_t val = mem_ptr[i];
                if (rep == 0) {
                    acc ^= val.range(31, 0);
                }
            }
        }
    }
    if (wr == 1) {
        out_acc = acc;
    }
}

