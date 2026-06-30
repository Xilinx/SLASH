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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Single-thread HIP benchmark for raw GPU access to the RP1 shared DDR
 * aperture through BAR4. The host maps BAR4 as a dma-buf into HSA, but
 * every measured load/store below originates on the GPU.
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#include <slash/uapi/rp1_protocol.h>

#include "rp1_mem_latency_result.h"

static constexpr uint64_t kBarCtrlOffset = 64ULL * 1024ULL * 1024ULL;
static constexpr uint32_t kPatternSeed = 0x6d656d6cu; /* "meml" */

static __device__ __forceinline__ uint32_t sample_word_index(
    const rp1_mem_latency_config_t cfg,
    uint32_t iter)
{
    const uint32_t words = static_cast<uint32_t>(cfg.scratch_bytes / sizeof(uint32_t));
    const uint32_t stride_words = cfg.stride_bytes / sizeof(uint32_t);
    if (words == 0 || stride_words == 0)
        return 0;

    const uint32_t slots = (words + stride_words - 1u) / stride_words;
    const uint32_t slot = (iter * 2654435761u) % slots;
    uint32_t word = slot * stride_words;
    if (word >= words)
        word = words - 1u;
    return word;
}

static __device__ __forceinline__ uint32_t pattern_for(uint32_t iter)
{
    return kPatternSeed ^ (iter * 0x9e3779b9u) ^ (iter >> 16);
}

static __device__ __forceinline__ uint64_t elapsed_cycles(
    unsigned long long start,
    unsigned long long end)
{
    return static_cast<uint64_t>(end - start);
}

__global__ void rp1_mem_latency_gpu_kernel(
    volatile uint8_t* bar,
    size_t bar_len,
    rp1_mem_latency_config_t cfg,
    rp1_mem_latency_result_t* result_out,
    uint64_t* write_cycles,
    uint64_t* read_cycles,
    uint64_t* rw_cycles)
{
    if (threadIdx.x != 0 || blockIdx.x != 0)
        return;

    result_out->status = RP1_MEM_LATENCY_STATUS_PENDING;
    result_out->mode = cfg.mode;
    result_out->iterations = cfg.iterations;
    result_out->warmup = cfg.warmup;
    result_out->scratch_bar_offset = cfg.scratch_bar_offset;
    result_out->scratch_bytes = cfg.scratch_bytes;
    result_out->stride_bytes = cfg.stride_bytes;
    result_out->sample_count = cfg.iterations;
    result_out->first_mismatch_iter = 0xffffffffu;

    const uint64_t needed = cfg.scratch_bar_offset + cfg.scratch_bytes;
    if (cfg.iterations == 0 ||
        cfg.scratch_bytes < sizeof(uint32_t) ||
        cfg.stride_bytes < sizeof(uint32_t) ||
        (cfg.scratch_bar_offset & 0x3u) != 0 ||
        (cfg.scratch_bytes & 0x3u) != 0 ||
        (cfg.stride_bytes & 0x3u) != 0 ||
        needed < cfg.scratch_bar_offset ||
        needed > bar_len ||
        (cfg.mode & RP1_MEM_LATENCY_MODE_RW) == 0) {
        result_out->status = RP1_MEM_LATENCY_STATUS_BAD_ARGS;
        return;
    }

    auto* ctrl = reinterpret_cast<volatile rp1_ctrl_t*>(bar + kBarCtrlOffset);
    __threadfence_system();
    result_out->magic_seen = ctrl->magic;

    auto* scratch = reinterpret_cast<volatile uint32_t*>(bar + cfg.scratch_bar_offset);
    const uint32_t total = cfg.warmup + cfg.iterations;
    uint64_t checksum = 0;
    uint32_t mismatch_iter = 0xffffffffu;
    uint32_t mismatch_expected = 0;
    uint32_t mismatch_observed = 0;

    for (uint32_t i = 0; i < total; ++i) {
        const bool measured = i >= cfg.warmup;
        const uint32_t sample = i - cfg.warmup;
        const uint32_t word = sample_word_index(cfg, i);
        const uint32_t value = pattern_for(i);

        if (cfg.mode & RP1_MEM_LATENCY_MODE_WRITE) {
            const unsigned long long t0 = clock64();
            scratch[word] = value;
            __threadfence_system();
            const unsigned long long t1 = clock64();

            if (measured && write_cycles)
                write_cycles[sample] = elapsed_cycles(t0, t1);
        }

        if (cfg.mode & RP1_MEM_LATENCY_MODE_READ) {
            __threadfence_system();
            const unsigned long long t0 = clock64();
            const uint32_t observed = scratch[word];
            const unsigned long long t1 = clock64();

            checksum += observed;
            if (measured && read_cycles)
                read_cycles[sample] = elapsed_cycles(t0, t1);
        }

        if ((cfg.mode & RP1_MEM_LATENCY_MODE_RW) == RP1_MEM_LATENCY_MODE_RW) {
            const uint32_t rw_value = value ^ 0xa5a5a5a5u;
            const unsigned long long t0 = clock64();
            scratch[word] = rw_value;
            __threadfence_system();
            const uint32_t observed = scratch[word];
            const unsigned long long t1 = clock64();

            checksum += observed;
            if (measured && rw_cycles)
                rw_cycles[sample] = elapsed_cycles(t0, t1);

            if (observed != rw_value && mismatch_iter == 0xffffffffu) {
                mismatch_iter = i;
                mismatch_expected = rw_value;
                mismatch_observed = observed;
            }
        }
    }

    result_out->checksum = checksum;
    result_out->first_mismatch_iter = mismatch_iter;
    result_out->first_mismatch_expected = mismatch_expected;
    result_out->first_mismatch_observed = mismatch_observed;
    result_out->status = (mismatch_iter == 0xffffffffu)
        ? RP1_MEM_LATENCY_STATUS_PASS
        : RP1_MEM_LATENCY_STATUS_MISMATCH;
}
