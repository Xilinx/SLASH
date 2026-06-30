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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file gpu_graph_test.hip
 * @brief GPU device backend test suite — mirrors graph_test.cpp patterns.
 *
 * Requires a ROCm-capable GPU.  Tests are compiled only when VRT_ENABLE_GPU=ON.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <hip/hip_runtime.h>

#include <vrt/graph/graph.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/crossdevice/cpu_gpu_bridge.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/gpu_device.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/node/io_map.hpp>
#include <vrt/graph/node/io_type_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>
#include <vrt/graph/node/compiled_node.hpp>

using namespace vrt::graph;

// ---------------------------------------------------------------------------
// GPU test kernels
// ---------------------------------------------------------------------------

__global__ void gpu_vadd(const float* a, const float* b, float* c, uint64_t n) {
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        c[i] = a[i] + b[i];
    }
}

__global__ void gpu_double(const float* in, float* out, uint64_t n) {
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        out[i] = in[i] * 2.0f;
    }
}

__global__ void gpu_negate(const float* in, float* out, uint64_t n) {
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        out[i] = -in[i];
    }
}

__global__ void gpu_add_offset(const float* in, float* out, uint64_t n, uint64_t offset_bits) {
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        // Reinterpret uint64_t scalar bits as float
        float offset;
        uint32_t bits32 = static_cast<uint32_t>(offset_bits);
        memcpy(&offset, &bits32, sizeof(float));
        out[i] = in[i] + offset;
    }
}

__global__ void gpu_split(const float* in, float* left, float* right, uint64_t n) {
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        left[i]  = in[i] + 1.0f;
        right[i] = in[i] * 10.0f;
    }
}

__global__ void gpu_merge(const float* left, const float* right, float* out, uint64_t n) {
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        out[i] = left[i] + right[i];
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static KernelDescriptor gpuKernel(std::string name, IOTypeMap ioType = {}) {
    return KernelDescriptor{std::move(name), DeviceType::GPU, std::nullopt, std::move(ioType)};
}

static KernelDescriptor cpuKernel(std::string name, IOTypeMap ioType = {}) {
    return KernelDescriptor{std::move(name), DeviceType::CPU, std::nullopt, std::move(ioType)};
}

static GpuKernelBinding makeBinding(const void* func, std::vector<std::string> paramOrder,
                                     dim3 block = dim3(256)) {
    GpuKernelBinding b;
    b.func = func;
    b.paramOrder = std::move(paramOrder);
    b.block = block;
    b.gridFn = [block](size_t elemCount) -> dim3 {
        return dim3(static_cast<unsigned>((elemCount + block.x - 1) / block.x));
    };
    return b;
}

class CpuAddKernel : public CpuKernel {
   public:
    CpuAddKernel() : CpuKernel("cpu_add") {}

    IOTypeMap ioTypeMap() const override { return ioType_; }

    void run(Args& args) override {
        auto in  = args.buffer("in").as<const float>();
        auto out = args.buffer("out").as<float>();
        auto n   = args.buffer("in").sizeBytes / sizeof(float);
        for (size_t i = 0; i < n; ++i) {
            out[i] = in[i] + 10.0f;
        }
    }

   private:
    IOTypeMap   ioType_;
};

// ---------------------------------------------------------------------------
// Test: Single GPU kernel (vector add)
// ---------------------------------------------------------------------------

TEST(GpuGraphTest, SingleGpuKernel) {
    auto gpu = std::make_shared<GpuDevice>("gpu:0", 0);

    gpu->registerKernel("vadd", makeBinding(
        reinterpret_cast<const void*>(&gpu_vadd),
        {"a", "b", "c", "n"}));

    Graph g;
    g.registerDevice(gpu);
    GraphScalar n = g.scalarInput<uint64_t>("n");

    GraphBuffer bufA = g.inputBuffer(BufferType::F32, "a");
    GraphBuffer bufB = g.inputBuffer(BufferType::F32, "b");

    IOMap io;
    GraphBuffer bufC;
    io.bindInput("a", bufA)
      .bindInput("b", bufB)
      .bindOutput("c", BufferType::F32, bufC)
      .bindInputScalar("n", n);
    g.addNode(gpuKernel("vadd"), std::move(io), "gpu:0");

    // Provide input data
    std::vector<float> a = {1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<float> b = {10.0f, 20.0f, 30.0f, 40.0f};
    gpu->setInputBuffer("a", a.data(), a.size() * sizeof(float));
    gpu->setInputBuffer("b", b.data(), b.size() * sizeof(float));

    auto exec = g.compile();
    exec.setScalar(n, 4ull);
    exec.run();

    // Read output
    std::vector<float> c(4);
    gpu->getOutputBuffer(bufC.name(), c.data(), c.size() * sizeof(float));

    EXPECT_FLOAT_EQ(c[0], 11.0f);
    EXPECT_FLOAT_EQ(c[1], 22.0f);
    EXPECT_FLOAT_EQ(c[2], 33.0f);
    EXPECT_FLOAT_EQ(c[3], 44.0f);
}

// ---------------------------------------------------------------------------
// Test: GPU pipeline (add → double → negate)
// ---------------------------------------------------------------------------

TEST(GpuGraphTest, GpuPipeline) {
    auto gpu = std::make_shared<GpuDevice>("gpu:0", 0);

    gpu->registerKernel("vadd", makeBinding(
        reinterpret_cast<const void*>(&gpu_vadd),
        {"a", "b", "c", "n"}));
    gpu->registerKernel("dbl", makeBinding(
        reinterpret_cast<const void*>(&gpu_double),
        {"in", "out", "n"}));
    gpu->registerKernel("neg", makeBinding(
        reinterpret_cast<const void*>(&gpu_negate),
        {"in", "out", "n"}));

    Graph g;
    g.registerDevice(gpu);
    GraphScalar n = g.scalarInput<uint64_t>("n");

    GraphBuffer bufA = g.inputBuffer(BufferType::F32, "a");
    GraphBuffer bufB = g.inputBuffer(BufferType::F32, "b");

    // Node 1: vadd
    IOMap io1;
    GraphBuffer afterAdd;
    io1.bindInput("a", bufA)
       .bindInput("b", bufB)
       .bindOutput("c", BufferType::F32, afterAdd)
       .bindInputScalar("n", n);
    g.addNode(gpuKernel("vadd"), std::move(io1), "gpu:0");

    // Node 2: double
    IOMap io2;
    GraphBuffer afterDbl;
    io2.bindInput("in", afterAdd)
       .bindOutput("out", BufferType::F32, afterDbl)
       .bindInputScalar("n", n);
    g.addNode(gpuKernel("dbl"), std::move(io2), "gpu:0");

    // Node 3: negate
    IOMap io3;
    GraphBuffer afterNeg;
    io3.bindInput("in", afterDbl)
       .bindOutput("out", BufferType::F32, afterNeg)
       .bindInputScalar("n", n);
    g.addNode(gpuKernel("neg"), std::move(io3), "gpu:0");

    // Input data
    std::vector<float> a = {1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<float> b = {10.0f, 20.0f, 30.0f, 40.0f};
    gpu->setInputBuffer("a", a.data(), a.size() * sizeof(float));
    gpu->setInputBuffer("b", b.data(), b.size() * sizeof(float));

    auto exec = g.compile();
    exec.setScalar(n, 4ull);
    exec.run();

    // Expected: -((a + b) * 2) = {-22, -44, -66, -88}
    std::vector<float> result(4);
    gpu->getOutputBuffer(afterNeg.name(), result.data(), result.size() * sizeof(float));

    EXPECT_FLOAT_EQ(result[0], -22.0f);
    EXPECT_FLOAT_EQ(result[1], -44.0f);
    EXPECT_FLOAT_EQ(result[2], -66.0f);
    EXPECT_FLOAT_EQ(result[3], -88.0f);
}

// ---------------------------------------------------------------------------
// Test: CPU → GPU cross-device pipeline
// ---------------------------------------------------------------------------

TEST(GpuGraphTest, CpuToGpuCrossDevice) {
    auto cpu = std::make_shared<CpuDevice>("cpu");
    auto gpu = std::make_shared<GpuDevice>("gpu:0", 0);

    // CPU kernel: add 10 to each element
    cpu->registerKernel(std::make_shared<CpuAddKernel>());

    // GPU kernel: double each element
    gpu->registerKernel("gpu_dbl", makeBinding(
        reinterpret_cast<const void*>(&gpu_double),
        {"in", "out", "n"}));

    Graph g;
    g.registerDevice(cpu);
    g.registerDevice(gpu);
    GraphScalar n = g.scalarInput<uint64_t>("n");
    g.registerBridgeFactory(DeviceType::CPU, DeviceType::GPU, CpuGpuBridgeFactory());
    g.registerBridgeFactory(DeviceType::GPU, DeviceType::CPU, CpuGpuBridgeFactory());

    GraphBuffer input = g.inputBuffer(BufferType::F32, "input");

    // Node 1: CPU add 10
    IOMap io1;
    GraphBuffer afterCpu;
    io1.bindInput("in", input)
       .bindOutput("out", BufferType::F32, afterCpu);
    g.addNode(cpuKernel("cpu_add"), std::move(io1), "cpu");

    // Node 2: GPU double
    IOMap io2;
    GraphBuffer afterGpu;
    io2.bindInput("in", afterCpu)
       .bindOutput("out", BufferType::F32, afterGpu)
       .bindInputScalar("n", n);
    g.addNode(gpuKernel("gpu_dbl"), std::move(io2), "gpu:0");

    // Input data
    std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f};
    cpu->setInputBuffer("input", data.data(), data.size() * sizeof(float));

    auto exec = g.compile();
    exec.setScalar(n, 4ull);
    exec.run();

    // Expected: (x + 10) * 2 = {22, 24, 26, 28}
    std::vector<float> result(4);
    gpu->getOutputBuffer(afterGpu.name(), result.data(), result.size() * sizeof(float));

    EXPECT_FLOAT_EQ(result[0], 22.0f);
    EXPECT_FLOAT_EQ(result[1], 24.0f);
    EXPECT_FLOAT_EQ(result[2], 26.0f);
    EXPECT_FLOAT_EQ(result[3], 28.0f);
}

// ---------------------------------------------------------------------------
// Test: GPU diamond DAG (split → two branches → merge)
// ---------------------------------------------------------------------------

TEST(GpuGraphTest, GpuDiamondDag) {
    auto gpu = std::make_shared<GpuDevice>("gpu:0", 0);

    gpu->registerKernel("split", makeBinding(
        reinterpret_cast<const void*>(&gpu_split),
        {"in", "left", "right", "n"}));
    gpu->registerKernel("dbl", makeBinding(
        reinterpret_cast<const void*>(&gpu_double),
        {"in", "out", "n"}));
    gpu->registerKernel("neg", makeBinding(
        reinterpret_cast<const void*>(&gpu_negate),
        {"in", "out", "n"}));
    gpu->registerKernel("merge", makeBinding(
        reinterpret_cast<const void*>(&gpu_merge),
        {"left", "right", "out", "n"}));

    Graph g;
    g.registerDevice(gpu);
    GraphScalar n = g.scalarInput<uint64_t>("n");

    GraphBuffer input = g.inputBuffer(BufferType::F32, "input");

    // Node A: split → left, right
    IOMap ioA;
    GraphBuffer bufLeft, bufRight;
    ioA.bindInput("in", input)
       .bindOutput("left", BufferType::F32, bufLeft)
       .bindOutput("right", BufferType::F32, bufRight)
       .bindInputScalar("n", n);
    g.addNode(gpuKernel("split"), std::move(ioA), "gpu:0");

    // Node B: double(left)
    IOMap ioB;
    GraphBuffer afterDbl;
    ioB.bindInput("in", bufLeft)
       .bindOutput("out", BufferType::F32, afterDbl)
       .bindInputScalar("n", n);
    g.addNode(gpuKernel("dbl"), std::move(ioB), "gpu:0");

    // Node C: negate(right)
    IOMap ioC;
    GraphBuffer afterNeg;
    ioC.bindInput("in", bufRight)
       .bindOutput("out", BufferType::F32, afterNeg)
       .bindInputScalar("n", n);
    g.addNode(gpuKernel("neg"), std::move(ioC), "gpu:0");

    // Node D: merge(afterDbl, afterNeg)
    IOMap ioD;
    GraphBuffer output;
    ioD.bindInput("left", afterDbl)
       .bindInput("right", afterNeg)
       .bindOutput("out", BufferType::F32, output)
       .bindInputScalar("n", n);
    g.addNode(gpuKernel("merge"), std::move(ioD), "gpu:0");

    // Input: {1, 2, 3, 4}
    std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f};
    gpu->setInputBuffer("input", data.data(), data.size() * sizeof(float));

    auto exec = g.compile();
    exec.setScalar(n, 4ull);
    exec.run();

    // split:  left = x+1 = {2,3,4,5},  right = x*10 = {10,20,30,40}
    // dbl:    left*2 = {4,6,8,10}
    // neg:    -right = {-10,-20,-30,-40}
    // merge:  dbl + neg = {-6,-14,-22,-30}
    std::vector<float> result(4);
    gpu->getOutputBuffer(output.name(), result.data(), result.size() * sizeof(float));

    EXPECT_FLOAT_EQ(result[0], -6.0f);
    EXPECT_FLOAT_EQ(result[1], -14.0f);
    EXPECT_FLOAT_EQ(result[2], -22.0f);
    EXPECT_FLOAT_EQ(result[3], -30.0f);
}

// ---------------------------------------------------------------------------
// Test: Error — missing kernel registration
// ---------------------------------------------------------------------------

TEST(GpuGraphTest, MissingKernelThrows) {
    auto gpu = std::make_shared<GpuDevice>("gpu:0", 0);

    // Do NOT register any kernels

    Graph g;
    g.registerDevice(gpu);
    GraphScalar n = g.scalarInput<uint64_t>("n");

    GraphBuffer input = g.inputBuffer(BufferType::F32, "input");
    IOMap io;
    GraphBuffer output;
    io.bindInput("in", input)
      .bindOutput("out", BufferType::F32, output)
      .bindInputScalar("n", n);
    g.addNode(gpuKernel("nonexistent"), std::move(io), "gpu:0");

    std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f};
    gpu->setInputBuffer("input", data.data(), data.size() * sizeof(float));

    EXPECT_THROW((void)g.compile(), std::runtime_error);
}
