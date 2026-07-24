/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * One iteration of the discrete Laplacian sharpening stencil (edge-replicated
 * boundaries): out[i] = in[i] + alpha * (2*in[i] - left - right).
 */

#include <ap_int.h>

extern "C" void sharpen_kernel(ap_uint<64> n, ap_int<32> alpha,
                               const ap_int<32>* in, ap_int<32>* out) {
#pragma HLS interface mode=s_axilite port=n
#pragma HLS interface mode=s_axilite port=alpha
#pragma HLS interface mode=s_axilite port=return
#pragma HLS interface m_axi bundle=gmem0 port=in  max_widen_bitwidth=64
#pragma HLS interface m_axi bundle=gmem1 port=out max_widen_bitwidth=64

    if (n == 0) return;

    // The graph runtime aliases loop-carried input/output buffers. Keep a
    // sliding window of original values so this remains correct when in == out.
    ap_int<32> left = in[0];
    ap_int<32> current = in[0];
    for (ap_uint<64> i = 0; i < n; ++i) {
        ap_int<32> right = (i + 1 == n) ? current : in[i + 1];
        ap_int<32> lap = 2 * current - left - right;
        out[i] = current + alpha * lap;
        left = current;
        current = right;
    }
}
