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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * bringup_kernel — trivial AXI-Lite + m_axi kernel used by the
 * rp1_bringup_vrt diamond example.  Same AXI-Lite signature as
 * `examples/00_axilite/hls/increment.cpp` but without the AXI-Stream
 * output, so each instance can be linked without needing a stream
 * consumer in the bitstream.
 *
 * Args (written by RP1 to AXI-Lite +0x10, +0x14, +0x18):
 *   - size (ap_uint<32>)
 *   - in   (float*)            — 64-bit pointer, occupies +0x14 and +0x18
 *
 * The bringup tools pass `size = 0`, which makes the loop a no-op so
 * `ap_done` fires immediately without touching the m_axi master.
 */

#include <ap_int.h>

void bringup_kernel(ap_uint<32> size, float* in) {
#pragma hls interface mode=s_axilite port=size
#pragma hls interface m_axi bundle=gmem0 port=in max_widen_bitwidth=64
#pragma hls interface mode=s_axilite port=return

    float acc = 0;
    for (ap_uint<32> i = 0; i < size; i++) {
#pragma hls pipeline II=1
        acc += in[i];
    }
    // Keep the accumulator referenced so HLS does not dead-code-eliminate
    // the loop body when size happens to be a build-time constant.
    volatile float sink = acc;
    (void)sink;
}
