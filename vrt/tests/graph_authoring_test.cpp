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
 * @brief Exercises both hardware-style public authoring surfaces end to end.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include <vrt/graph/backend_resource_binding.hpp>
#include <vrt/graph/detail/executable_assembler.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/execution_plan.hpp>
#include <vrt/graph/graph.hpp>

using namespace vrt::graph;

namespace {
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

static_assert(std::is_move_constructible_v<Execution>);
static_assert(!std::is_move_assignable_v<Execution>);
static_assert(std::is_move_constructible_v<BackendResourceBindings>);
static_assert(!std::is_move_assignable_v<BackendResourceBindings>);
static_assert(std::is_move_constructible_v<ExecutionPlan>);
static_assert(!std::is_move_assignable_v<ExecutionPlan>);
static_assert(
    std::is_move_constructible_v<detail::AssembledExecutables>);
static_assert(
    !std::is_move_assignable_v<detail::AssembledExecutables>);
static_assert(
    !std::is_move_assignable_v<CompileResult<ExecutionPlan>>);
static_assert(
    !std::is_move_assignable_v<
        CompileResult<detail::AssembledExecutables>>);
static_assert(
    std::is_move_constructible_v<
        std::unique_ptr<IDeviceExecutionLease>>);
static_assert(
    !std::is_copy_constructible_v<
        std::unique_ptr<IDeviceExecutionLease>>);

TEST(GraphAuthoringTest, ElementwiseShorthandRoundTrips) {
    constexpr std::size_t count = 8;
    Graph graph = Graph::withDefaults();
    auto addOne = graph.cpu().elementwise<std::int32_t>(
        "add_one", [](std::int32_t value) { return value + 1; });
    GraphScalar size = graph.scalarInput<std::uint64_t>("size");
    GraphBuffer input = graph.input<std::int32_t>("input", size);
    GraphBuffer output = graph.output<std::int32_t>("output", size);
    graph.addKernelCall({
        .kernel = addOne,
        .inputs = {{"in", input}},
        .outputs = {{"out", output}},
    });

    std::vector<std::int32_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<std::int32_t>(i);
    }
    Execution execution = graph.compile();
    execution.writeScalar(size, static_cast<std::uint64_t>(count));
    execution.write(input, values);
    execution.run();

    std::vector<std::int32_t> result(count);
    execution.read(output, result);
    for (std::size_t i = 0; i < count; ++i) {
        EXPECT_EQ(result[i], static_cast<std::int32_t>(i + 1));
    }
}

TEST(GraphAuthoringTest, CpuRejectsSecondLiveExecutionAndReleasesLease) {
    Graph graph = Graph::withDefaults();
    auto identity = graph.cpu().elementwise<std::int32_t>(
        "lease_identity", [](std::int32_t value) { return value; });
    GraphScalar size = graph.scalarInput<std::uint64_t>("size");
    GraphBuffer input = graph.input<std::int32_t>("input", size);
    GraphBuffer output = graph.output<std::int32_t>("output", size);
    graph.addKernelCall({
        .kernel = identity,
        .inputs = {{"in", input}},
        .outputs = {{"out", output}},
    });

    {
        Execution first = graph.compile();
        try {
            (void)graph.compile();
            FAIL() << "a second live CPU execution must be rejected";
        } catch (const GraphCompileError& error) {
            const Diagnostic* diagnostic =
                error.diagnostics().firstError();
            ASSERT_NE(diagnostic, nullptr);
            EXPECT_EQ(diagnostic->code, DiagCode::ResourceExhausted);
        }
    }

    EXPECT_NO_THROW({
        Execution afterRelease = graph.compile();
        (void)afterRelease;
    });
}

TEST(GraphAuthoringTest, RawIoMapInoutMintsReadableOutput) {
    constexpr std::size_t count = 25;
    Graph graph = Graph::withDefaults();
    KernelHandle sparse = graph.cpu().add<CpuSparse>();
    GraphScalar size = graph.scalarInput<std::uint64_t>("size");
    GraphBuffer input = graph.input<std::int32_t>("input", size);
    GraphBuffer output;
    IOMap io;
    io.bindInout(
        "data", "data", input, output, graph.rootRegion().scopeId());
    graph.addNode(
        KernelDescriptor{
            sparse.name, sparse.type, sparse.image, sparse.ioType},
        std::move(io), sparse.device);

    Execution execution = graph.compile();
    execution.writeScalar(size, static_cast<std::uint64_t>(count));
    execution.write(input, std::vector<std::int32_t>(count, 0));
    execution.run();
    std::vector<std::int32_t> result(count);
    execution.read(output, result);
    for (std::size_t i = 0; i < count; ++i) {
        EXPECT_EQ(result[i], i % 10 == 0 ? 1 : 0);
    }
}

TEST(GraphAuthoringTest, PublicSurfacesExecuteEquivalentCpuGraphs) {
    constexpr std::size_t count = 8;
    Graph graph = Graph::withDefaults();
    KernelHandle addOne =
        graph.cpu().elementwise<std::int32_t>(
            "raw_add_one",
            [](std::int32_t value) { return value + 1; });
    GraphScalar size = graph.scalarInput<std::uint64_t>("size");
    GraphBuffer input = graph.input<std::int32_t>("input", size);
    GraphBuffer output;
    IOMap io;
    io.bindInput("in", input)
        .bindOutput(
            "out", BufferType::I32, output, size,
            graph.rootRegion().scopeId());
    graph.addNode(
        KernelDescriptor{
            addOne.name, addOne.type, addOne.image, addOne.ioType},
        std::move(io), addOne.device);

    std::vector<std::int32_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<std::int32_t>(i);
    }
    Execution execution = graph.compile();
    execution.writeScalar(
        size, static_cast<std::uint64_t>(count));
    execution.write(input, values);
    execution.run();
    std::vector<std::int32_t> result(count);
    execution.read(output, result);
    for (std::size_t i = 0; i < count; ++i) {
        EXPECT_EQ(result[i], static_cast<std::int32_t>(i + 1));
    }
}

TEST(GraphAuthoringTest, NamedPortsCrossControlAndRepeatRuns) {
    static_assert(std::is_default_constructible_v<Graph>);
    static_assert(!std::is_copy_constructible_v<Execution>);

    constexpr std::uint32_t count = 16;
    constexpr std::uint32_t iterations = 2;
    Graph graph = Graph::withDefaults();
    auto preprocess = graph.cpu().add<CpuPreprocess>();
    auto stage = graph.cpu().add<CpuStage>();
    auto sparse = graph.cpu().add<CpuSparse>();
    auto finalize = graph.cpu().add<CpuFinalize>();
    auto parityKernel = graph.cpu().add<CpuParity>();
    auto report = graph.cpu().add<CpuReport>();
    auto reportOdd = graph.cpu().add<CpuReportOdd>();

    GraphScalar size = graph.scalarInput<std::uint64_t>("size");
    GraphBuffer input = graph.input<std::int32_t>("input", size);
    GraphBuffer preprocessed =
        graph.buffer<std::int32_t>("preprocessed", size);
    graph.addKernelCall({
        .kernel = preprocess,
        .inputs = {{"in", input}},
        .outputs = {{"out", preprocessed}},
    });

    GraphScalar loopCount =
        graph.scalarInput<std::uint32_t>("iterations");
    GraphBuffer loopOutput =
        graph.buffer<std::int32_t>("loop_output", size);
    RegionBuilder loop = graph.addLoop({
        .count = loopCount,
        .inputs = {{"state", preprocessed}},
        .outputs = {{"state", loopOutput}},
    });
    GraphBuffer staged = loop.buffer<std::int32_t>("staged", size);
    GraphBuffer bumped = loop.buffer<std::int32_t>("bumped", size);
    loop.addKernelCall({
        .kernel = stage,
        .inputs = {{"in", loop.input("state")}},
        .outputs = {{"out", staged}},
    });
    loop.addKernelCall({
        .kernel = sparse,
        .inouts = {{"data", staged, bumped}},
    });
    loop.addKernelCall({
        .kernel = finalize,
        .inputs = {{"in", bumped}},
        .outputs = {{"out", loop.output("state")}},
    });

    GraphScalar parity = graph.scalar<std::uint64_t>("parity");
    graph.addKernelCall({
        .kernel = parityKernel,
        .inputs = {{"in", loopOutput}},
        .outputScalars = {{"parity", parity}},
    });
    GraphBuffer output = graph.output<std::int32_t>("output", size);
    auto [even, odd] = graph.addConditional({
        .condition = parity == 0,
        .inputs = {{"value", loopOutput}},
        .outputs = {{"value", output}},
    });
    even.addKernelCall({
        .kernel = report,
        .inputs = {{"in", even.input("value")}},
        .outputs = {{"out", even.output("value")}},
    });
    odd.addKernelCall({
        .kernel = reportOdd,
        .inputs = {{"in", odd.input("value")}},
        .outputs = {{"out", odd.output("value")}},
    });

    std::vector<std::int32_t> values(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        values[i] = static_cast<std::int32_t>(i);
    }
    Execution execution = graph.compile();
    execution.writeScalar(size, static_cast<std::uint64_t>(count));
    execution.writeScalar(loopCount, iterations);
    for (int run = 0; run < 2; ++run) {
        execution.write(input, values);
        execution.run();
        std::vector<std::int32_t> result(count);
        execution.read(output, result);
        EXPECT_EQ(result, reference(count, iterations));
    }
}

TEST(GraphAuthoringTest,
     ZeroCountAndFalseWhilePublishInitialCpuValue) {
    for (bool whileLoop : {false, true}) {
        SCOPED_TRACE(whileLoop ? "while" : "fixed");
        Graph graph = Graph::withDefaults();
        auto stage = graph.cpu().add<CpuStage>();
        GraphScalar size =
            graph.scalarInput<std::uint64_t>("size");
        GraphScalar control =
            graph.scalarInput<std::uint32_t>("control");
        GraphBuffer input =
            graph.input<std::int32_t>("input", size);
        GraphBuffer output =
            graph.output<std::int32_t>("output", size);

        LoopBuildSpec spec;
        if (whileLoop) {
            spec.condition = control != 0u;
        } else {
            spec.count = TripCount(control);
        }
        spec.inputs = {{"state", input}};
        spec.outputs = {{"state", output}};
        RegionBuilder body = graph.addLoop(spec);
        body.addKernelCall({
            .kernel = stage,
            .inputs = {{"in", body.input("state")}},
            .outputs = {{"out", body.output("state")}},
        });

        Execution execution = graph.compile();
        const std::vector<std::int32_t> initial{3, 5, 8};
        execution.writeScalar(
            size, static_cast<std::uint64_t>(initial.size()));
        execution.writeScalar(control, 0u);
        execution.write(input, initial);
        ASSERT_NO_THROW(execution.run());
        std::vector<std::int32_t> result(initial.size());
        execution.read(output, result);
        EXPECT_EQ(result, initial);
    }
}
