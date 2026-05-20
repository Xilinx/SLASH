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

#include "ap_axi_sdata.h"
#include "ap_int.h"
#include "hls_stream.h"

#define DWIDTH 512
#define TDWIDTH 3

typedef ap_axiu<DWIDTH, 1, 1, TDWIDTH> pkt;

void traffic_consumer(hls::stream<pkt> &axis_in,
                      ap_uint<32>     &rx_flits)
{
#pragma HLS INTERFACE mode=axis      port=axis_in depth=16
#pragma HLS INTERFACE mode=s_axilite port=rx_flits bundle=control
#pragma HLS INTERFACE ap_ctrl_none   port=return

    ap_uint<32> rx = 0;
    rx_flits = 0;

    while (1) {
#pragma HLS PIPELINE II=1
        if (!axis_in.empty()) {
            (void)axis_in.read();  // consume one beat
            rx++;
            rx_flits = rx;         // readable via AXI-Lite
        }
    }
}
