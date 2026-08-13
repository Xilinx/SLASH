/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Step B: global brightness. level = max(1, sum(in) / n).
 */

#include <ap_int.h>

extern "C" void level_kernel(ap_uint<64> n, const ap_int<32>* in, ap_int<32>* level) {
#pragma HLS interface mode=s_axilite port=n
#pragma HLS interface mode=s_axilite port=level   // scalar output
#pragma HLS interface mode=s_axilite port=return
#pragma HLS interface m_axi bundle=gmem0 port=in max_widen_bitwidth=64

    ap_int<64> sum = 0;
    for (ap_uint<64> i = 0; i < n; ++i) {
        sum += in[i];
    }
    ap_int<32> result = (ap_int<32>)(sum / (ap_int<64>)n);
    *level = (result > 1) ? result : ap_int<32>(1);
}
