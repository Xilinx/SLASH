/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <ap_int.h>
#include <cstdint>

void slash_add(uint32_t a, uint32_t b, uint32_t &result) {
#pragma HLS interface mode=s_axilite port=a
#pragma HLS interface mode=s_axilite port=b
#pragma HLS interface mode=s_axilite port=result
#pragma HLS interface mode=s_axilite port=return

    result = a + b;
}
