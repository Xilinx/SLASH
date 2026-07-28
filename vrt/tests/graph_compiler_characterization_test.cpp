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

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/device/device.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/graph.hpp>
#include <vrt/graph/node/compiled_node.hpp>
#include <vrt/graph/semantic_plan.hpp>

using namespace vrt::graph;

namespace {

class NoopDevicePlan : public IDevicePlan {
   public:
    void launch() override {}
    void wait() override {}
};

class StubCpuDevice : public IDevice {
   public:
    DeviceType type() const override { return DeviceType::CPU; }
    std::string id() const override { return "cpu"; }

    std::unique_ptr<IDevicePlan> compilePlan(const DGraph&) override {
        return std::make_unique<NoopDevicePlan>();
    }
};

SemanticPlan compileNestedScalarLoop() {
    Graph graph;
    graph.registerDevice(std::make_shared<StubCpuDevice>());

    GraphScalar parent = graph.globalScalar(ScalarType::I32, "counter");
    GraphScalar trips = graph.globalScalar(ScalarType::I32, "trips");

    auto body = graph.rootRegion().createChild();
    GraphScalar local = body->scalar(ScalarType::I32, "counter");
    GraphScalar next = body->scalar(ScalarType::I32, "next");
    const std::string start = body->importFromParent({{parent, local}});

    IOTypeMap type;
    type.inputScalars.push_back({"in", ScalarType::I32});
    type.outputScalars.push_back({"out", ScalarType::I32});
    IOMap io;
    io.bindInputScalar("in", local).bindOutputScalar("out", next);
    const std::string kernel = body->addKernel(
        KernelDescriptor{"increment", DeviceType::CPU, std::nullopt, type},
        std::move(io), "cpu", {start});
    body->exportToParent({{next, parent}}, {kernel});

    LoopSpec loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    graph.addLoop(std::move(loop));

    CompiledGraph compiled = graph.compile();
    return normalizeSemanticPlan(compiled.dgraphs());
}

std::vector<DGraph> generatedFixture(const std::string& prefix,
                                     const std::string& outputName,
                                     std::uint64_t outputScope,
                                     std::uint32_t signalSlot,
                                     std::string kernelName = "transform") {
    const GraphBuffer input = GraphBuffer::make(BufferType::I32, "input", 0);
    const GraphBuffer output =
        GraphBuffer::make(BufferType::I32, outputName, outputScope);

    IOMap io;
    io.bindInput("in", input).bindExistingOutput("out", output);

    CompiledSourceNode source;
    source.id = prefix + "_source";
    source.deviceId = "cpu";
    source.inputBufferKeys.push_back(scopedBufferKey(0, "input"));

    CompiledKernelNode kernel;
    kernel.id = prefix + "_kernel";
    kernel.deviceId = "cpu";
    kernel.kernel = KernelDescriptor{
        std::move(kernelName), DeviceType::CPU, std::nullopt, {}};
    kernel.ioMap = std::move(io);
    kernel.dependsOn.push_back(source.id);

    CompiledSignalNode signal;
    signal.id = prefix + "_signal";
    signal.deviceId = "cpu";
    signal.dependsOn.push_back(kernel.id);
    signal.slot = signalSlot;
    signal.value = 1;
    signal.operation = 3;

    CompiledWaitNode wait;
    wait.id = prefix + "_wait";
    wait.deviceId = "cpu";
    wait.dependsOn.push_back(signal.id);
    wait.slot = signalSlot;
    wait.value = 1;
    wait.conditionOp = 2;

    DGraph dgraph;
    dgraph.deviceId = "cpu";
    dgraph.nodes = {
        std::move(source),
        std::move(kernel),
        std::move(signal),
        std::move(wait),
    };
    return {std::move(dgraph)};
}

}  // namespace

TEST(GraphCompilerCharacterizationTest,
     CanonicalizesGeneratedIdsScopesValuesAndPhysicalSlots) {
    const SemanticPlan first = normalizeSemanticPlan(
        generatedFixture("old_17", "out_buf_91", 42, 7));
    const SemanticPlan second = normalizeSemanticPlan(
        generatedFixture("new_3", "result_tmp_8", 901, 233));

    EXPECT_EQ(first, second) << first.toString() << "\n---\n"
                             << second.toString();
}

TEST(GraphCompilerCharacterizationTest,
     PreservesSemanticDifferences) {
    const SemanticPlan first = normalizeSemanticPlan(
        generatedFixture("a", "out_a", 3, 4, "transform"));
    const SemanticPlan second = normalizeSemanticPlan(
        generatedFixture("b", "out_b", 8, 99, "different_kernel"));

    EXPECT_NE(first, second);
}

TEST(GraphCompilerCharacterizationTest,
     NestedCompilationIsStableAcrossAuthoredScopeIds) {
    const SemanticPlan first = compileNestedScalarLoop();
    const SemanticPlan second = compileNestedScalarLoop();

    EXPECT_EQ(first, second) << first.toString() << "\n---\n"
                             << second.toString();
    EXPECT_NE(first.toString().find("loop_body"), std::string::npos);
    EXPECT_NE(first.toString().find("increment"), std::string::npos);
}
