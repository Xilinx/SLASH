/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Step A: local derivative. edges[i] = |in[i+1] - in[i]|, last element = 0.
 */

#include <ap_int.h>

extern "C" void edges_kernel(ap_uint<64> n, const ap_int<32>* in, ap_int<32>* edges) {
#pragma HLS interface mode=s_axilite port=n
#pragma HLS interface mode=s_axilite port=return
#pragma HLS interface m_axi bundle=gmem0 port=in    max_widen_bitwidth=64
#pragma HLS interface m_axi bundle=gmem1 port=edges max_widen_bitwidth=64

    for (ap_uint<64> i = 0; i + 1 < n; ++i) {
        ap_int<32> diff = in[i + 1] - in[i];
        edges[i] = diff < 0 ? -diff : diff;
    }
    edges[n - 1] = 0;  // last element has no right neighbour
}
