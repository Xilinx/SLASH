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

/**
 * 09_rocm_verify — Minimal ROCm/ROCr smoke test.
 *
 * Checks three things needed by 08_p2p_gpu:
 *
 *   [1] HSA agent enumeration  — hsa_init + hsa_iterate_agents
 *   [2] GPU compute            — vector-add HIP kernel, validates result
 *   [3] P2P import linkage     — hsa_amd_interop_map_buffer is callable
 *                                (will return an error without a real dma-buf fd,
 *                                 but confirms the symbol resolves at runtime)
 *
 * Usage: ./09_rocm_verify
 */

#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

/* Forward declaration — defined in vadd_kernel.hip */
__global__ void vadd(const float* a, const float* b, float* c, uint32_t n);

/* ── Error helpers ─────────────────────────────────────────────────────── */

#define HIP_CHECK(call)                                                     \
    do {                                                                    \
        hipError_t _e = (call);                                             \
        if (_e != hipSuccess)                                               \
            throw std::runtime_error(std::string("[HIP] " #call ": ") +    \
                                     hipGetErrorString(_e));                \
    } while (0)

#define HSA_CHECK(call)                                                     \
    do {                                                                    \
        hsa_status_t _s = (call);                                           \
        if (_s != HSA_STATUS_SUCCESS) {                                     \
            const char* _msg = nullptr;                                     \
            hsa_status_string(_s, &_msg);                                   \
            throw std::runtime_error(std::string("[HSA] " #call ": ") +    \
                                     (_msg ? _msg : "unknown error"));      \
        }                                                                   \
    } while (0)

/* ── [1] HSA agent enumeration ─────────────────────────────────────────── */

struct AgentList {
    std::vector<hsa_agent_t> cpus;
    std::vector<hsa_agent_t> gpus;
};

static hsa_status_t collect_agents(hsa_agent_t agent, void* data)
{
    auto* list = reinterpret_cast<AgentList*>(data);

    hsa_device_type_t type;
    if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) != HSA_STATUS_SUCCESS)
        return HSA_STATUS_SUCCESS;

    if (type == HSA_DEVICE_TYPE_CPU)
        list->cpus.push_back(agent);
    else if (type == HSA_DEVICE_TYPE_GPU)
        list->gpus.push_back(agent);

    return HSA_STATUS_SUCCESS;
}

static void print_agent(hsa_agent_t agent, const char* label, int idx)
{
    char name[64]{};
    char vendor[64]{};
    hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, name);
    hsa_agent_get_info(agent, HSA_AGENT_INFO_VENDOR_NAME, vendor);
    std::cout << "    " << label << "[" << idx << "]  " << vendor << " " << name << "\n";
}

static AgentList check_hsa_agents()
{
    std::cout << "[1] HSA agent enumeration\n";

    HSA_CHECK(hsa_init());

    AgentList list;
    HSA_CHECK(hsa_iterate_agents(collect_agents, &list));

    std::cout << "    CPU agents: " << list.cpus.size() << "\n";
    for (int i = 0; i < static_cast<int>(list.cpus.size()); ++i)
        print_agent(list.cpus[i], "CPU", i);

    std::cout << "    GPU agents: " << list.gpus.size() << "\n";
    for (int i = 0; i < static_cast<int>(list.gpus.size()); ++i)
        print_agent(list.gpus[i], "GPU", i);

    if (list.gpus.empty())
        throw std::runtime_error("No GPU HSA agent found — is amdgpu loaded and /dev/kfd accessible?");

    std::cout << "    OK\n\n";
    return list;
}

/* ── [2] GPU compute: vector-add ───────────────────────────────────────── */

static void check_gpu_compute()
{
    std::cout << "[2] GPU compute (vector-add)\n";

    constexpr uint32_t N        = 1024 * 1024; /* 1 M elements */
    constexpr uint32_t THREADS  = 256;
    constexpr uint32_t BLOCKS   = (N + THREADS - 1) / THREADS;
    constexpr float    VAL_A    = 1.5f;
    constexpr float    VAL_B    = 2.5f;
    constexpr float    EXPECTED = VAL_A + VAL_B;

    /* Allocate and initialise host arrays */
    std::vector<float> h_a(N, VAL_A);
    std::vector<float> h_b(N, VAL_B);
    std::vector<float> h_c(N, 0.0f);

    /* Allocate device memory */
    float *d_a = nullptr, *d_b = nullptr, *d_c = nullptr;
    HIP_CHECK(hipMalloc(&d_a, N * sizeof(float)));
    HIP_CHECK(hipMalloc(&d_b, N * sizeof(float)));
    HIP_CHECK(hipMalloc(&d_c, N * sizeof(float)));

    /* Copy inputs to device */
    HIP_CHECK(hipMemcpy(d_a, h_a.data(), N * sizeof(float), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_b, h_b.data(), N * sizeof(float), hipMemcpyHostToDevice));

    /* Launch kernel */
    vadd<<<dim3(BLOCKS), dim3(THREADS), 0, 0>>>(d_a, d_b, d_c, N);
    HIP_CHECK(hipDeviceSynchronize());

    /* Copy result back */
    HIP_CHECK(hipMemcpy(h_c.data(), d_c, N * sizeof(float), hipMemcpyDeviceToHost));

    hipFree(d_a);
    hipFree(d_b);
    hipFree(d_c);

    /* Validate */
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (std::fabs(h_c[i] - EXPECTED) > 1e-5f) {
            ++mismatches;
            if (mismatches <= 5)
                std::cerr << "    mismatch at [" << i << "]: got " << h_c[i]
                          << " expected " << EXPECTED << "\n";
        }
    }
    if (mismatches)
        throw std::runtime_error("vector-add: " + std::to_string(mismatches) + " mismatches");

    std::cout << "    " << N << " elements: a=" << VAL_A << " + b=" << VAL_B
              << " = " << EXPECTED << " — all correct\n";
    std::cout << "    OK\n\n";
}

/* ── [3] hsa_amd_interop_map_buffer linkage ─────────────────────────────── */

static void check_interop_linkage(const AgentList& agents)
{
    std::cout << "[3] hsa_amd_interop_map_buffer linkage\n";

    /* Call with fd=-1: expected to fail, but the symbol must resolve. */
    hsa_agent_t gpu = agents.gpus[0];
    size_t      sz  = 0;
    void*       ptr = nullptr;

    hsa_status_t st = hsa_amd_interop_map_buffer(
        1, &gpu,
        -1,      /* invalid fd — will fail, that's fine */
        0,
        &sz, &ptr,
        nullptr, nullptr
    );

    if (st == HSA_STATUS_SUCCESS) {
        /* Shouldn't succeed with fd=-1, but unmap cleanly if it does */
        hsa_amd_interop_unmap_buffer(ptr);
        std::cout << "    (unexpectedly succeeded with fd=-1)\n";
    } else {
        const char* msg = nullptr;
        hsa_status_string(st, &msg);
        std::cout << "    Called with fd=-1, got expected error: "
                  << (msg ? msg : "unknown") << "\n";
    }

    std::cout << "    Symbol resolves and is callable — OK\n\n";
}

/* ── Entry point ───────────────────────────────────────────────────────── */

int main()
{
    std::cout << "=== 09_rocm_verify ===\n\n";

    try {
        AgentList agents = check_hsa_agents();
        check_gpu_compute();
        check_interop_linkage(agents);
        hsa_shut_down();
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        hsa_shut_down();
        return 1;
    }

    std::cout << "All checks passed — ROCm/ROCr is ready for 08_p2p_gpu.\n";
    return 0;
}
