/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

/**
 * Simple vector addition: c[i] = a[i] + b[i]
 *
 * Used to verify basic GPU compute and hipcc compilation.
 */
__global__ void vadd(const float* a, const float* b, float* c, uint32_t n)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        c[i] = a[i] + b[i];
}
