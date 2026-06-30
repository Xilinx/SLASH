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
 * reduce — sum the input vector into an s_axilite output register.
 *
 *     result = Σ in[i],    for i in [0, size).
 *
 * Terminal compute node of the rp1_pipeline DAG. The result is exposed
 * as an AXI-Lite register so RP1's SCALAR_READ opcode can capture it
 * into a signal slot for the host to read back.
 */

#include <ap_int.h>

void reduce(ap_uint<32> size, ap_uint<32>* in, ap_uint<32>& result) {
#pragma hls interface mode=s_axilite port=size
#pragma hls interface m_axi bundle=gmem0 port=in max_widen_bitwidth=64
#pragma hls interface mode=s_axilite port=result
#pragma hls interface mode=s_axilite port=return

    ap_uint<32> acc = 0;
    for (ap_uint<32> i = 0; i < size; i++) {
#pragma hls pipeline II=1
        acc += in[i];
    }
    result = acc;
}
