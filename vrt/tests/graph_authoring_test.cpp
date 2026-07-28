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
 * @file graph_authoring_test.cpp
 * @brief Exercises the RFC struct-literal authoring API end to end on the CPU
 *        backend: typed tokens, kernel handles, addKernelCall, addLoop (with a
 *        carried buffer), addConditional, in-place (inout) kernels, scalar
 *        outputs, and Graph::write/read. The FPGA reprogram / image-safety
 *        paths are validated on hardware via examples/graph/00_multi_image_pipeline.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/graph.hpp>

using namespace vrt::graph;

namespace {

// Minimal FPGA-typed device for exercising compile-time image-safety checks
// without real hardware. compilePlan() is never reached because compilation
// fails earlier on the image-safety violation under test.
class NoopPlan : public IDevicePlan {
   public:
    void launch() override {}
    void wait() override {}
};

class StubFpgaDevice : public IDevice {
   public:
    explicit StubFpgaDevice(std::string id) : id_(std::move(id)) {}
    DeviceType type() const override { return DeviceType::FPGA; }
    std::string id() const override { return id_; }
    std::unique_ptr<IDevicePlan> compilePlan(const DGraph&) override {
        return std::make_unique<NoopPlan>();
    }

   private:
    std::string id_;
};

struct NoopBridgeOp : IBridgeOp {
    std::string label() const override { return "noop"; }
};

class NoopBridge : public IBridge {
   public:
    BridgeStepPair makeTransfer(IDevice&, IDevice&, const GraphBuffer&, uint64_t,
                                const std::string&, const std::string&) override {
        return step();
    }
    BridgeStepPair makeScalarTransfer(IDevice&, IDevice&, const std::string&,
                                      const std::string&, const std::string&) override {
        return step();
    }
    BridgeStepPair makeBarrier(IDevice&, IDevice&, const std::string&,
                               const std::string&) override {
        return step();
    }

   private:
    static BridgeStepPair step() {
        return BridgeStepPair{
            std::make_shared<NoopBridgeOp>(),
            []() {},
            []() { return true; },
            []() {}};
    }
};

Graph stubFpgaGraph() {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));
    graph.registerDevice(std::make_shared<StubFpgaDevice>("fpga:0"));
    auto factory = [](IDevice& src, IDevice& dst) -> std::shared_ptr<IBridge> {
        (void)src;
        (void)dst;
        return std::make_shared<NoopBridge>();
    };
    graph.registerBridgeFactory(DeviceType::CPU, DeviceType::FPGA, factory);
    graph.registerBridgeFactory(DeviceType::FPGA, DeviceType::CPU, factory);
    return graph;
}

class CpuPreprocess : public CpuKernel {
   public:
    CpuPreprocess() : CpuKernel("cpu_preprocess") {}
    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").out<int32_t>("out");
    }
    void run(Args& a) override {
        auto in = a.in<int32_t>("in");
        auto out = a.out<int32_t>("out");
        for (std::size_t i = 0; i < in.size(); ++i) out[i] = in[i] + 10;
    }
};

class CpuStage : public CpuKernel {
   public:
    CpuStage() : CpuKernel("cpu_stage") {}
    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").out<int32_t>("out");
    }
    void run(Args& a) override {
        auto in = a.in<int32_t>("in");
        auto out = a.out<int32_t>("out");
        for (std::size_t i = 0; i < in.size(); ++i) out[i] = in[i] + 1;
    }
};

// In-place: bumps every 10th element through an inout port.
class CpuSparse : public CpuKernel {
   public:
    CpuSparse() : CpuKernel("cpu_sparse") {}
    IOTypeMap ioTypeMap() const override { return IOTypeMap{}.inout<int32_t>("data"); }
    void run(Args& a) override {
        auto d = a.inout<int32_t>("data");
        for (std::size_t i = 0; i < d.size(); i += 10) d[i] += 1;
    }
};

class CpuFinalize : public CpuKernel {
   public:
    CpuFinalize() : CpuKernel("cpu_finalize") {}
    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").out<int32_t>("out");
    }
    void run(Args& a) override {
        auto in = a.in<int32_t>("in");
        auto out = a.out<int32_t>("out");
        for (std::size_t i = 0; i < in.size(); ++i) out[i] = in[i] - 4;
    }
};

// Writes a scalar the post-loop conditional branches on.
class CpuParity : public CpuKernel {
   public:
    CpuParity() : CpuKernel("cpu_parity") {}
    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").scalarOut<uint64_t>("parity");
    }
    void run(Args& a) override {
        auto in = a.in<int32_t>("in");
        a.scalarOut<std::uint64_t>("parity") = static_cast<std::uint64_t>(in[0] & 1);
    }
};

class CpuReport : public CpuKernel {
   public:
    CpuReport() : CpuKernel("cpu_report") {}
    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").out<int32_t>("out");
    }
    void run(Args& a) override {
        auto in = a.in<int32_t>("in");
        auto out = a.out<int32_t>("out");
        for (std::size_t i = 0; i < in.size(); ++i) out[i] = in[i] + 100;
    }
};

class CpuReportOdd : public CpuKernel {
   public:
    CpuReportOdd() : CpuKernel("cpu_report_odd") {}
    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").out<int32_t>("out");
    }
    void run(Args& a) override {
        auto in = a.in<int32_t>("in");
        auto out = a.out<int32_t>("out");
        for (std::size_t i = 0; i < in.size(); ++i) out[i] = in[i] + 200;
    }
};

std::vector<int32_t> reference(std::uint32_t n, std::uint32_t iters) {
    std::vector<int32_t> post(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        int32_t v = static_cast<int32_t>(i) + 10;
        for (std::uint32_t it = 0; it < iters; ++it) {
            v = v + 1;                  // stage
            if (i % 10 == 0) v += 1;    // sparse (in place)
            v = v - 4;                  // finalize
        }
        post[i] = v;
    }
    const int32_t bias = (post[0] & 1) == 0 ? 100 : 200;
    std::vector<int32_t> out(n);
    for (std::uint32_t i = 0; i < n; ++i) out[i] = post[i] + bias;
    return out;
}

}  // namespace

TEST(GraphAuthoringTest, ElementwiseShorthandRoundTrips) {
    constexpr std::size_t n = 8;
    Graph graph = Graph::withDefaults();

    auto addOne = graph.cpu().elementwise<int32_t>("add_one", [](int32_t v) { return v + 1; });

    GraphScalar size = graph.scalarInput<std::uint64_t>("n");
    GraphBuffer raw = graph.input<int32_t>("raw", size);
    GraphBuffer out = graph.output<int32_t>("out", size);
    graph.addKernelCall({.kernel = addOne, .inputs = {{"in", raw}}, .outputs = {{"out", out}}});

    std::vector<int32_t> input(n);
    for (std::size_t i = 0; i < n; ++i) input[i] = static_cast<int32_t>(i);
    auto exec = graph.compile();
    exec.writeScalar(size, static_cast<std::uint64_t>(n));
    exec.write(raw, input);

    exec.run();

    std::vector<int32_t> output(n, 0);
    exec.read(out, output);
    for (std::size_t i = 0; i < n; ++i) EXPECT_EQ(output[i], static_cast<int32_t>(i) + 1);
}

TEST(GraphAuthoringTest, InoutKernelMutatesInPlace) {
    constexpr std::size_t n = 25;
    Graph graph = Graph::withDefaults();

    auto sparse = graph.cpu().add<CpuSparse>();

    GraphScalar size = graph.scalarInput<std::uint64_t>("n");
    GraphBuffer raw = graph.input<int32_t>("raw", size);
    GraphBuffer bumped = graph.output<int32_t>("bumped", size);
    graph.addKernelCall({.kernel = sparse, .inouts = {{"data", raw, bumped}}});

    std::vector<int32_t> input(n, 0);
    auto exec = graph.compile();
    exec.writeScalar(size, static_cast<std::uint64_t>(n));
    exec.write(raw, input);

    exec.run();

    std::vector<int32_t> output(n, -1);
    exec.read(bumped, output);
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(output[i], (i % 10 == 0) ? 1 : 0) << "index " << i;
    }
}

TEST(GraphAuthoringTest, LoopConditionalInplaceFullPipeline) {
    constexpr std::uint32_t n = 16;
    constexpr std::uint32_t iters = 2;
    Graph graph = Graph::withDefaults();

    auto preprocess = graph.cpu().add<CpuPreprocess>();
    auto stage = graph.cpu().add<CpuStage>();
    auto sparse = graph.cpu().add<CpuSparse>();
    auto finalize = graph.cpu().add<CpuFinalize>();
    auto parityKernel = graph.cpu().add<CpuParity>();
    auto report = graph.cpu().add<CpuReport>();
    auto reportOdd = graph.cpu().add<CpuReportOdd>();

    GraphScalar size = graph.scalarInput<std::uint64_t>("n");
    GraphBuffer raw = graph.input<int32_t>("raw", size);
    GraphBuffer pre = graph.buffer<int32_t>("pre", size);
    graph.addKernelCall({.kernel = preprocess, .inputs = {{"in", raw}}, .outputs = {{"out", pre}}});

    GraphBuffer post = graph.buffer<int32_t>("post", size);
    GraphScalar loopCount = graph.scalarInput<std::uint32_t>("loop_count");
    {
        auto loop = graph.addLoop({.count = loopCount,
                                   .inputs = {{"state", pre}},
                                   .outputs = {{"state", post}}});
        GraphBuffer s = loop.input("state");

        GraphBuffer staged = loop.buffer<int32_t>("staged", size);
        loop.addKernelCall({.kernel = stage, .inputs = {{"in", s}}, .outputs = {{"out", staged}}});

        GraphBuffer bumped = loop.buffer<int32_t>("bumped", size);
        loop.addKernelCall({.kernel = sparse, .inouts = {{"data", staged, bumped}}});

        loop.addKernelCall({.kernel = finalize,
                            .inputs = {{"in", bumped}},
                            .outputs = {{"out", loop.output("state")}}});
    }

    GraphScalar parity = graph.scalar<uint64_t>("parity");
    graph.addKernelCall({.kernel = parityKernel,
                         .inputs = {{"in", post}},
                         .outputScalars = {{"parity", parity}}});

    GraphBuffer out = graph.output<int32_t>("out", size);
    {
        auto [thenBranch, elseBranch] = graph.addConditional({
            .condition = (parity == 0), .inputs = {{"x", post}}, .outputs = {{"y", out}}});
        thenBranch.addKernelCall({.kernel = report,
                                  .inputs = {{"in", thenBranch.input("x")}},
                                  .outputs = {{"out", thenBranch.output("y")}}});
        elseBranch.addKernelCall({.kernel = reportOdd,
                                  .inputs = {{"in", elseBranch.input("x")}},
                                  .outputs = {{"out", elseBranch.output("y")}}});
    }

    std::vector<int32_t> input(n);
    for (std::uint32_t i = 0; i < n; ++i) input[i] = static_cast<int32_t>(i);
    auto exec = graph.compile();
    exec.writeScalar(size, static_cast<std::uint64_t>(n));
    exec.writeScalar(loopCount, iters);
    exec.write(raw, input);

    exec.run();

    std::vector<int32_t> output(n, 0);
    exec.read(out, output);
    EXPECT_EQ(output, reference(n, iters));
}

TEST(GraphAuthoringTest, UngatedFpgaDispatchIsRejected) {
    Graph graph = stubFpgaGraph();

    // FPGA kernel that names an image but is not gated behind any reprogram.
    KernelHandle fpgaK{"graph_kernel_0", DeviceType::FPGA, "imageA",
                       IOTypeMap{}.scalarIn<uint64_t>("n").out<int32_t>("out"), "fpga:0"};
    GraphScalar n = graph.scalarInput<uint64_t>("n");
    GraphScalar size = graph.scalarInput<std::uint64_t>("n_elements");
    GraphBuffer out = graph.buffer<int32_t>("out", size);
    graph.addKernelCall({.kernel = fpgaK, .inputScalars = {{"n", n}}, .outputs = {{"out", out}}});

    EXPECT_THROW(graph.compile(), std::runtime_error);
}

TEST(GraphAuthoringTest, GatedFpgaDispatchCompiles) {
    Graph graph = stubFpgaGraph();

    KernelHandle fpgaK{"graph_kernel_0", DeviceType::FPGA, "imageA",
                       IOTypeMap{}.scalarIn<uint64_t>("n").out<int32_t>("out"), "fpga:0"};
    GraphScalar n = graph.scalarInput<uint64_t>("n");
    GraphScalar size = graph.scalarInput<std::uint64_t>("n_elements");
    GraphBuffer out = graph.buffer<int32_t>("out", size);

    auto r = graph.addReprogram({.image = {"imageA", "imageA.pdi", "fpga:0"}});
    graph.addKernelCall({.kernel = fpgaK,
                         .inputScalars = {{"n", n}},
                         .outputs = {{"out", out}},
                         .after = {r}});

    EXPECT_NO_THROW((void)graph.compile());
}

TEST(GraphAuthoringTest, StructLiteralWhileLoopCarriesPlacementHints) {
    Graph graph = Graph::withDefaults();
    auto stage = graph.cpu().add<CpuStage>();
    GraphScalar size = graph.scalarInput<std::uint64_t>("n");
    GraphScalar keepGoing = graph.scalarInput<std::uint32_t>("keep_going");
    GraphBuffer input = graph.input<std::int32_t>("input", size);
    GraphBuffer output = graph.output<std::int32_t>("output", size);

    auto loop = graph.addLoop({
        .condition = (keepGoing != 0),
        .inputs = {{"state", input}},
        .outputs = {{"state", output}},
        .outputPlacement = {.buffers = {{"state", "cpu"}}},
    });
    loop.addKernelCall({
        .kernel = stage,
        .inputs = {{"in", loop.input("state")}},
        .outputs = {{"out", loop.output("state")}},
    });

    const auto* authored =
        std::get_if<LoopOp>(&graph.rootRegion().ops().front());
    ASSERT_NE(authored, nullptr);
    EXPECT_EQ(authored->kind, LoopKind::WhileCondition);
    EXPECT_FALSE(authored->tripCount.has_value());
    EXPECT_TRUE(authored->condition.has_value());
    EXPECT_EQ(authored->outputPlacement.buffers.at("state"), "cpu");
    EXPECT_NO_THROW((void)graph.compile());
}

TEST(GraphAuthoringTest, StructLiteralControlRequiresOneLoopMode) {
    Graph graph = Graph::withDefaults();
    GraphScalar count = graph.scalarInput<std::uint32_t>("count");
    GraphScalar condition = graph.scalarInput<std::uint32_t>("condition");

    EXPECT_THROW(
        graph.addLoop({
            .count = count,
            .condition = (condition != 0),
        }),
        std::invalid_argument);
    EXPECT_THROW(graph.addLoop(LoopBuildSpec{}), std::invalid_argument);
}

TEST(GraphAuthoringTest, StructLiteralConditionalCarriesPlacementHints) {
    Graph graph = Graph::withDefaults();
    auto stage = graph.cpu().add<CpuStage>();
    GraphScalar size = graph.scalarInput<std::uint64_t>("n");
    GraphScalar flag = graph.scalarInput<std::uint32_t>("flag");
    GraphBuffer input = graph.input<std::int32_t>("input", size);
    GraphBuffer output = graph.output<std::int32_t>("output", size);

    auto [thenBranch, elseBranch] = graph.addConditional({
        .condition = (flag != 0),
        .inputs = {{"value", input}},
        .outputs = {{"result", output}},
        .outputPlacement = {.buffers = {{"result", "cpu"}}},
    });
    thenBranch.addKernelCall({
        .kernel = stage,
        .inputs = {{"in", thenBranch.input("value")}},
        .outputs = {{"out", thenBranch.output("result")}},
    });
    elseBranch.addKernelCall({
        .kernel = stage,
        .inputs = {{"in", elseBranch.input("value")}},
        .outputs = {{"out", elseBranch.output("result")}},
    });

    const auto* authored =
        std::get_if<ConditionalOp>(&graph.rootRegion().ops().front());
    ASSERT_NE(authored, nullptr);
    EXPECT_EQ(authored->outputPlacement.buffers.at("result"), "cpu");
    EXPECT_NO_THROW((void)graph.compile());
}
