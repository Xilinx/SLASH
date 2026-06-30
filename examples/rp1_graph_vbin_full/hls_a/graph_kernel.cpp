/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Image A: increment every element by one.
 */

#include <ap_int.h>

extern "C" void graph_kernel(ap_uint<64> n, const int* in, int* out) {
#pragma HLS interface mode=s_axilite port=n
#pragma HLS interface m_axi bundle=gmem0 port=in  max_widen_bitwidth=64
#pragma HLS interface m_axi bundle=gmem1 port=out max_widen_bitwidth=64
#pragma HLS interface mode=s_axilite port=return

    for (ap_uint<64> i = 0; i < n; ++i) {
        out[i] = in[i] + 1;
    }
}
