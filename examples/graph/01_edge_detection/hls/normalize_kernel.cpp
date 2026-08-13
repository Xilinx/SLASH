/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Step C: normalize the edges by brightness, in place. edges[i] *= K / level.
 */

#include <ap_int.h>

extern "C" void normalize_kernel(ap_uint<64> n, ap_int<32> K, ap_int<32> level,
                                 ap_int<32>* edges) {
#pragma HLS interface mode=s_axilite port=n
#pragma HLS interface mode=s_axilite port=K
#pragma HLS interface mode=s_axilite port=level
#pragma HLS interface mode=s_axilite port=return
#pragma HLS interface m_axi bundle=gmem0 port=edges max_widen_bitwidth=64

    for (ap_uint<64> i = 0; i < n; ++i) {
        edges[i] = (ap_int<32>)((ap_int<64>)edges[i] * K / level);
    }
}
