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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <vrt/graph/compiler.hpp>
#include <vrt/graph/control/condition.hpp>
#include <vrt/graph/control/graph_region.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/crossdevice/bridge_op.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/graph.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/node/io_map.hpp>
#include <vrt/graph/node/io_type_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>

#include "test_support/control_specs.hpp"

using namespace vrt::graph;
using namespace vrt::graph::test_support;

namespace {

class NoopDevicePlan : public IDevicePlan {
   public:
    void launch() override {}
    void wait() override {}
};

class StubDevice : public IDevice {
   public:
    StubDevice(std::string id, DeviceType type)
        : id_(std::move(id)), type_(type) {}

    DeviceType type() const override { return type_; }
    std::string id() const override { return id_; }

    std::unique_ptr<IDevicePlan> compilePlan(const DGraph& dgraph) override {
        (void)dgraph;
        return std::make_unique<NoopDevicePlan>();
    }

   private:
    std::string id_;
    DeviceType type_ = DeviceType::CPU;
};

class RegionAwareStubDevice : public StubDevice {
   public:
    RegionAwareStubDevice(std::string id, DeviceType type)
        : StubDevice(std::move(id), type) {}

    void setRegion(const std::string& kernel, const std::string& port,
                   std::string region) {
        regions_[{kernel, port}] = std::move(region);
    }

    std::optional<std::string> resolveMemoryRegion(
        const KernelDescriptor& kernel, const std::string& portName) const override {
        auto it = regions_.find({kernel.name, portName});
        if (it == regions_.end()) return std::nullopt;
        return it->second;
    }

    std::function<void()> makeDeviceCopyAction(
        const GraphBuffer& /*source*/, const GraphBuffer& /*target*/,
        BufferType /*type*/, const std::string& /*sourceRegion*/,
        const std::string& /*targetRegion*/) override {
        return []() {};
    }

   private:
    std::map<std::pair<std::string, std::string>, std::string> regions_;
};

struct InspectionBridgeOp : IBridgeOp {
    explicit InspectionBridgeOp(std::string labelValue)
        : labelValue(std::move(labelValue)) {}

    std::string label() const override { return labelValue; }

    std::string labelValue;
};

class InspectionBridge : public IBridge {
   public:
    BridgeStepPair makeTransfer(IDevice& /*src*/, IDevice& /*dst*/,
                                 const GraphBuffer& /*buffer*/, uint64_t /*sizeHintBytes*/,
                                 const std::string& /*producerNodeId*/,
                                 const std::string& /*consumerNodeId*/) override {
        return BridgeStepPair{
            std::make_shared<InspectionBridgeOp>("inspection_xfer"),
            []() {},
            []() { return true; },
            []() {}};
    }

    BridgeStepPair makeScalarTransfer(IDevice& /*src*/, IDevice& /*dst*/,
                                      const std::string& /*scalarKey*/,
                                      const std::string& /*producerNodeId*/,
                                      const std::string& /*consumerNodeId*/) override {
        return BridgeStepPair{
            std::make_shared<InspectionBridgeOp>("inspection_scalar_xfer"),
            []() {},
            []() { return true; },
            []() {}};
    }

    BridgeStepPair makeBarrier(IDevice& /*src*/, IDevice& /*dst*/,
                                const std::string& /*producerNodeId*/,
                                const std::string& /*consumerNodeId*/) override {
        return BridgeStepPair{
            std::make_shared<InspectionBridgeOp>("inspection_barrier"),
            []() {},
            []() { return true; },
            []() {}};
    }
};

IOTypeMap singleOutputType(BufferType bufferType = BufferType::I32) {
    IOTypeMap ioType;
    ioType.outputs.push_back({"out", bufferType});
    return ioType;
}

IOTypeMap singleInputOutputType(BufferType bufferType = BufferType::I32) {
    IOTypeMap ioType;
    ioType.inputs.push_back({"in", bufferType});
    ioType.outputs.push_back({"out", bufferType});
    return ioType;
}

IOTypeMap singleInputScalarType(ScalarType scalarType = ScalarType::I32) {
    IOTypeMap ioType;
    ioType.inputScalars.push_back({"in", scalarType});
    return ioType;
}

IOTypeMap singleOutputScalarType(ScalarType scalarType = ScalarType::I32) {
    IOTypeMap ioType;
    ioType.outputScalars.push_back({"out", scalarType});
    return ioType;
}

GraphScalar testSize(GraphRegion& region) {
    static std::uint64_t next = 0;
    return region.inputScalar(ScalarType::U64, "__test_size_" + std::to_string(next++));
}

GraphBuffer testInput(GraphRegion& region, BufferType type, const std::string& name) {
    return region.inputBuffer(type, name, testSize(region));
}

std::string addOutputKernel(GraphRegion& region,
                            KernelDescriptor kernel,
                            BufferType bufferType,
                            const std::string& device,
                            std::optional<GraphScalar> size = std::nullopt) {
    IOMap kernelIo;
    GraphBuffer output;
    kernelIo.bindOutput("out", bufferType, output,
                        size ? *size : testSize(region),
                        region.scopeId());
    return region.addKernel(std::move(kernel), std::move(kernelIo), device);
}

class AddI32BufferKernel : public CpuKernel {
   public:
    AddI32BufferKernel(std::string name, std::int32_t delta)
        : CpuKernel(std::move(name)), delta_(delta) {
        ioType_.inputs.push_back({"in", BufferType::I32});
        ioType_.outputs.push_back({"out", BufferType::I32});
    }

    IOTypeMap ioTypeMap() const override { return ioType_; }

    void run(Args& args) override {
        const auto& in = args.buffer("in");
        const auto& out = args.buffer("out");
        const auto* src = in.as<const std::int32_t>();
        auto* dst = out.as<std::int32_t>();
        const std::size_t count = std::min(in.sizeBytes, out.sizeBytes) / sizeof(std::int32_t);
        for (std::size_t i = 0; i < count; ++i) {
            dst[i] = src[i] + delta_;
        }
    }

   private:
    std::int32_t delta_ = 0;
    IOTypeMap ioType_;
};

GraphBuffer bindControlOutput(IOMap& ioMap,
                              GraphRegion& region,
                              BufferType bufferType = BufferType::I32) {
    GraphBuffer output;
    ioMap.bindOutput("out", bufferType, output, testSize(region), region.scopeId());
    return output;
}

// Auto-register stub factories for every (CPU<->non-CPU) and (non-CPU<->non-CPU)
// device-type pair seen in @p graph so the unified bridge-factory check inside
// GraphCompiler::compile can pass. Inspection tests don't actually exercise
// the factories — bridge resolution goes through a custom `bridgeFor` lambda —
// but the validator still requires them to be registered.
void ensureInspectionBridgeFactories(Graph& graph) {
    auto stubFactory = [](IDevice& /*src*/, IDevice& /*dst*/) -> std::shared_ptr<IBridge> {
        return std::make_shared<InspectionBridge>();
    };
    std::set<DeviceType> seen;
    for (const auto& [id, dev] : graph.devices()) {
        (void)id;
        if (dev) seen.insert(dev->type());
    }
    auto registerIfMissing = [&](DeviceType s, DeviceType d) {
        if (s == DeviceType::CPU && d == DeviceType::CPU) return;
        const auto key = std::make_pair(s, d);
        if (graph.bridgeFactories().count(key)) return;
        graph.registerBridgeFactory(s, d, stubFactory);
    };
    for (DeviceType s : seen) {
        for (DeviceType d : seen) {
            registerIfMissing(s, d);
        }
    }
}

std::vector<DGraph> compileForInspection(Graph& graph) {
    ensureInspectionBridgeFactories(graph);
    GraphCompiler compiler;
    static InspectionBridge bridge;
    auto bridgeFor = [](const std::string& /*src*/, const std::string& /*dst*/)
        -> IBridge* { return &bridge; };
    auto compiled = compiler.compile(
        graph.rootRegion(), graph.devices(), graph.bridgeFactories(),
        bridgeFor,
        std::make_shared<std::map<std::string, uint64_t>>());
    if (!compiled.ok()) {
        throw GraphCompileError(std::move(compiled.diagnostics));
    }
    return std::move(*compiled.output).takeDGraphs();
}

std::vector<DGraph> compileForInspection(Graph& graph, IBridge& bridge) {
    ensureInspectionBridgeFactories(graph);
    GraphCompiler compiler;
    auto bridgeFor = [&bridge](const std::string& /*src*/, const std::string& /*dst*/)
        -> IBridge* { return &bridge; };
    auto compiled = compiler.compile(
        graph.rootRegion(), graph.devices(), graph.bridgeFactories(),
        bridgeFor,
        std::make_shared<std::map<std::string, uint64_t>>());
    if (!compiled.ok()) {
        throw GraphCompileError(std::move(compiled.diagnostics));
    }
    return std::move(*compiled.output).takeDGraphs();
}

const DGraph* findDGraph(const std::vector<DGraph>& dgraphs, const std::string& deviceId) {
    for (const auto& dgraph : dgraphs) {
        if (dgraph.deviceId == deviceId) return &dgraph;
    }
    return nullptr;
}

const CompiledNode* findCompiledNode(const DGraph& dgraph, const std::string& nodeId) {
    for (const auto& node : dgraph.nodes) {
        if (compiledNodeId(node) == nodeId) return &node;
    }
    return nullptr;
}

const DGraphChild* findChildDGraphs(const DGraph& dgraph,
                                    const std::string& parentNodeId,
                                    DGraphChildRole role) {
    for (const auto& child : dgraph.childDGraphs) {
        if (child.parentNodeId == parentNodeId && child.role == role) return &child;
    }
    return nullptr;
}

const DGraph* findChildDGraph(const DGraphChild& child, const std::string& deviceId) {
    for (const auto& dgraph : child.dgraphs) {
        if (dgraph && dgraph->deviceId == deviceId) return dgraph.get();
    }
    return nullptr;
}

const CompiledBridgeOpNode* findBridgeNode(const DGraph& dgraph,
                                           CompiledBridgeOpNode::Side side,
                                           const std::string& pairedKernelId) {
    for (const auto& node : dgraph.nodes) {
        const auto* bridge = std::get_if<CompiledBridgeOpNode>(&node);
        if (!bridge) continue;
        if (bridge->side == side && bridge->pairedKernelId == pairedKernelId) return bridge;
    }
    return nullptr;
}

const CompiledDeviceCopyNode* findDeviceCopyNode(const DGraph& dgraph) {
    for (const auto& node : dgraph.nodes) {
        if (const auto* copy = std::get_if<CompiledDeviceCopyNode>(&node)) return copy;
    }
    return nullptr;
}

std::size_t countDeviceCopyNodes(const std::vector<DGraph>& dgraphs) {
    std::size_t count = 0;
    for (const DGraph& dgraph : dgraphs) {
        for (const auto& node : dgraph.nodes) {
            if (std::holds_alternative<CompiledDeviceCopyNode>(node)) ++count;
        }
    }
    return count;
}

bool dependsOn(const CompiledNode& node, const std::string& dependencyId) {
    const auto& dependencies = compiledNodeDependsOn(node);
    return std::find(dependencies.begin(), dependencies.end(), dependencyId) !=
           dependencies.end();
}

bool dependsOn(const CompiledBridgeOpNode& node, const std::string& dependencyId) {
    return std::find(node.dependsOn.begin(), node.dependsOn.end(), dependencyId) !=
           node.dependsOn.end();
}

}  // namespace

TEST(RegionCompilerTest, ConditionValidatesOperandTypes) {
    auto lhs = ConditionOperand::scalar(ScalarType::I32, "lhs", 7);
    auto rhs = ConditionOperand::constant<int32_t>(10);

    auto cond = Condition::compare(CompareOp::LT, lhs, rhs);
    EXPECT_EQ(cond.op(), CompareOp::LT);
    ASSERT_TRUE(cond.lhs());
    EXPECT_EQ(cond.lhs()->scopeId(), 7);

    EXPECT_THROW(
        Condition::compare(CompareOp::EQ,
                           ConditionOperand::scalar(ScalarType::I32, "a"),
                           ConditionOperand::constant<uint32_t>(1)),
        std::invalid_argument);
}

TEST(RegionCompilerTest, EpsilonConditionsAreFloatOnly) {
    EXPECT_NO_THROW(
        Condition::compareWithEpsilon(CompareOp::EQE,
                                      ConditionOperand::scalar(ScalarType::F32, "a"),
                                      ConditionOperand::constant<float>(1.0f),
                                      ConditionOperand::constant<float>(0.001f)));

    EXPECT_THROW(
        Condition::compareWithEpsilon(CompareOp::EQE,
                                      ConditionOperand::scalar(ScalarType::I32, "a"),
                                      ConditionOperand::constant<int32_t>(1),
                                      ConditionOperand::constant<int32_t>(0)),
        std::invalid_argument);

    EXPECT_THROW(
        Condition::compare(CompareOp::EQE,
                           ConditionOperand::scalar(ScalarType::F64, "a"),
                           ConditionOperand::constant<double>(1.0)),
        std::invalid_argument);
}

TEST(RegionCompilerTest, LoopTripCountRequiresIntegerType) {
    auto scalarCount = LoopTripCount::scalar(ScalarType::I64, "n", 9);
    EXPECT_EQ(scalarCount.kind(), LoopTripCount::Kind::Scalar);
    EXPECT_EQ(scalarCount.scopeId(), 9);

    EXPECT_THROW(LoopTripCount::scalar(ScalarType::F32, "n"), std::invalid_argument);
}

TEST(RegionCompilerTest, ScopedTokensCarryRegionIdentity) {
    auto root = GraphRegion::createRoot();
    auto body = root->createChild();

    GraphBuffer rootInput = testInput(*root, BufferType::I32, "raw");
    GraphScalar bodyScalar = body->scalar(ScalarType::I32, "limit");

    EXPECT_EQ(rootInput.scopeId(), root->scopeId());
    EXPECT_EQ(bodyScalar.scopeId(), body->scopeId());
    EXPECT_NE(root->scopeId(), body->scopeId());
}

TEST(RegionCompilerTest, GraphExposesRootRegion) {
    Graph graph;

    GraphBuffer input = graph.inputBuffer(BufferType::U8, "raw");
    GraphScalar scalar = graph.globalScalar(ScalarType::I32, "n");

    EXPECT_EQ(graph.rootRegion().scopeId(), 0u);
    EXPECT_EQ(input.scopeId(), graph.rootRegion().scopeId());
    EXPECT_EQ(scalar.scopeId(), graph.rootRegion().scopeId());
}

TEST(RegionCompilerTest, GraphAddNodeAuthorsKernelInRootRegion) {
    Graph graph;

    IOTypeMap kernelType;
    kernelType.inputs.push_back({"in", BufferType::I32});
    kernelType.outputs.push_back({"out", BufferType::I32});

    GraphBuffer input = graph.inputBuffer(BufferType::I32, "raw", testSize(graph.rootRegion()));
    IOMap io;
    GraphBuffer output;
    io.bindInput("in", input)
      .bindOutput("out", BufferType::I32, output, graph.rootRegion().scopeId());

    KernelDescriptor kernel{"copy", DeviceType::CPU, std::nullopt, kernelType};
    std::string nodeId = graph.addNode(std::move(kernel), std::move(io), "cpu");

    ASSERT_EQ(graph.rootRegion().ops().size(), 1u);
    const auto& op = graph.rootRegion().ops().front();
    ASSERT_TRUE(std::holds_alternative<KernelOp>(op));
    EXPECT_EQ(std::get<KernelOp>(op).id, nodeId);

    const auto rootKernels = graph.rootKernels();
    ASSERT_EQ(rootKernels.size(), 1u);
    EXPECT_EQ(rootKernels.front().get().id, nodeId);
    EXPECT_EQ(&rootKernels.front().get(),
              &std::get<KernelOp>(graph.rootRegion().ops().front()));
}

TEST(RegionCompilerTest, GraphRootControlHelpersDelegateToRootRegion) {
    Graph graph;
    auto loopBody = graph.rootRegion().createChild();
    auto whileBody = graph.rootRegion().createChild();
    GraphScalar size = testSize(graph.rootRegion());
    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();

    std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), loopBody));
    std::string whileId = graph.addLoop(
        whileLoopSpec(Condition::alwaysTrue(), whileBody));
    std::string ifId = graph.addConditional(
        ifElseSpec(Condition::alwaysFalse(), thenRegion, elseRegion));

    ASSERT_EQ(graph.rootRegion().ops().size(), 3u);
    EXPECT_EQ(regionOpId(graph.rootRegion().ops()[0]), loopId);
    EXPECT_EQ(regionOpId(graph.rootRegion().ops()[1]), whileId);
    EXPECT_EQ(regionOpId(graph.rootRegion().ops()[2]), ifId);
    EXPECT_TRUE(std::holds_alternative<LoopOp>(graph.rootRegion().ops()[0]));
    EXPECT_TRUE(std::holds_alternative<LoopOp>(graph.rootRegion().ops()[1]));
    EXPECT_TRUE(std::holds_alternative<ConditionalOp>(graph.rootRegion().ops()[2]));
}

TEST(RegionCompilerTest, RegionStoresKernelAndControlOps) {
    auto root = GraphRegion::createRoot();
    auto body = root->createChild();

    IOTypeMap kernelType;
    kernelType.inputs.push_back({"in", BufferType::I32});
    kernelType.outputs.push_back({"out", BufferType::I32});

    GraphBuffer bodyInput = testInput(*body, BufferType::I32, "in_buf");
    IOMap bodyIo;
    GraphBuffer bodyOutput;
    bodyIo.bindInput("in", bodyInput)
          .bindOutput("out", BufferType::I32, bodyOutput, body->scopeId());

    KernelDescriptor kernel{"copy", DeviceType::CPU, std::nullopt, kernelType};
    std::string kernelId = body->addKernel(std::move(kernel), std::move(bodyIo), "cpu");

    EXPECT_EQ(body->ops().size(), 1u);
    EXPECT_EQ(regionOpId(body->ops().front()), kernelId);
    EXPECT_EQ(bodyOutput.scopeId(), body->scopeId());

    std::string loopId = root->addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(*root)), body));
    ASSERT_EQ(root->ops().size(), 1u);
    EXPECT_EQ(regionOpId(root->ops().front()), loopId);

    const auto& loop = std::get<LoopOp>(root->ops().front());
    EXPECT_EQ(loop.kind, LoopKind::FixedCount);
    ASSERT_TRUE(loop.tripCount);
    EXPECT_EQ(loop.body, body);
}

TEST(RegionCompilerTest, CompilerBuildsLoopControlNodeAndChildDGraph) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto body = graph.rootRegion().createChild();
    std::string bodyKernelId = body->addKernel(cpuKernel("body"), IOMap{}, "cpu");
    std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);

    const CompiledNode* loopNode = findCompiledNode(*cpuDGraph, loopId);
    ASSERT_NE(loopNode, nullptr);
    ASSERT_TRUE(std::holds_alternative<CompiledLoopNode>(*loopNode));
    const auto& compiledLoop = std::get<CompiledLoopNode>(*loopNode);
    EXPECT_EQ(compiledLoop.loopKind, CompiledLoopKind::FixedCount);
    ASSERT_TRUE(compiledLoop.tripCount);
    EXPECT_EQ(compiledLoop.tripCount->kind(), LoopTripCount::Kind::Scalar);
    EXPECT_EQ(compiledLoop.tripCount->type(), ScalarType::I32);

    const DGraphChild* bodyChild = findChildDGraphs(*cpuDGraph, loopId,
                                                   DGraphChildRole::LoopBody);
    ASSERT_NE(bodyChild, nullptr);
    ASSERT_EQ(bodyChild->dgraphs.size(), 1u);
    ASSERT_NE(bodyChild->dgraphs.front(), nullptr);
    EXPECT_EQ(bodyChild->dgraphs.front()->deviceId, "cpu");
    EXPECT_NE(findCompiledNode(*bodyChild->dgraphs.front(), bodyKernelId), nullptr);
}

// A constant fixed-count loop whose body is entirely FPGA kernels is placed on
// the FPGA device (for autonomous RP1 LOOP/RERUN execution), not the CPU.
TEST(RegionCompilerTest, FixedCountAllFpgaLoopPlacedOnFpgaQueue) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("fpga:0", DeviceType::FPGA));

    auto body = graph.rootRegion().createChild();
    std::string bodyKernelId = body->addKernel(
        KernelDescriptor{"body", DeviceType::FPGA, std::nullopt, IOTypeMap{}}, IOMap{}, "fpga:0");
    std::string loopId =
        graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    auto dgraphs = compileForInspection(graph);

    // Loop control node lives on the FPGA queue, not the CPU.
    const DGraph* fpgaDGraph = findDGraph(dgraphs, "fpga:0");
    ASSERT_NE(fpgaDGraph, nullptr);
    const CompiledNode* loopNode = findCompiledNode(*fpgaDGraph, loopId);
    ASSERT_NE(loopNode, nullptr);
    EXPECT_TRUE(std::holds_alternative<CompiledLoopNode>(*loopNode));

    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    if (cpuDGraph) {
        EXPECT_EQ(findCompiledNode(*cpuDGraph, loopId), nullptr);
    }

    const DGraphChild* bodyChild =
        findChildDGraphs(*fpgaDGraph, loopId, DGraphChildRole::LoopBody);
    ASSERT_NE(bodyChild, nullptr);
    ASSERT_FALSE(bodyChild->dgraphs.empty());
    EXPECT_NE(findCompiledNode(*bodyChild->dgraphs.front(), bodyKernelId), nullptr);
}

// Phase F.2: an if/else whose predicate is produced by a main-line FPGA kernel
// and whose branches are all-FPGA runs autonomously on the FPGA queue (RP1
// COND), not the CPU control path.
TEST(RegionCompilerTest, FpgaConditionalWithFpgaPredicatePlacedOnFpgaQueue) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("fpga:0", DeviceType::FPGA));

    GraphScalar p = graph.globalScalar(ScalarType::U32, "p");

    IOTypeMap predType;
    predType.outputScalars.push_back({"out", ScalarType::U32});
    IOMap predIo;
    predIo.bindOutputScalar("out", p);
    std::string predId = graph.addNode(
        KernelDescriptor{"pred", DeviceType::FPGA, std::nullopt, predType}, predIo, "fpga:0");

    auto thenR = graph.rootRegion().createChild();
    std::string thenKId = thenR->addKernel(
        KernelDescriptor{"thenK", DeviceType::FPGA, std::nullopt, IOTypeMap{}}, IOMap{}, "fpga:0");
    auto elseR = graph.rootRegion().createChild();
    std::string elseKId = elseR->addKernel(
        KernelDescriptor{"elseK", DeviceType::FPGA, std::nullopt, IOTypeMap{}}, IOMap{}, "fpga:0");

    std::string condId = graph.addConditional(ifElseSpec(
        Condition::compare(CompareOp::GE, ConditionOperand::scalar(ScalarType::U32, "p"),
                           ConditionOperand::constant<uint32_t>(1)),
        thenR, elseR, {predId}));

    auto dgraphs = compileForInspection(graph);

    const DGraph* fpgaDGraph = findDGraph(dgraphs, "fpga:0");
    ASSERT_NE(fpgaDGraph, nullptr);
    const CompiledNode* condNode = findCompiledNode(*fpgaDGraph, condId);
    ASSERT_NE(condNode, nullptr);
    EXPECT_TRUE(std::holds_alternative<CompiledConditionalNode>(*condNode));

    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    if (cpuDGraph) {
        EXPECT_EQ(findCompiledNode(*cpuDGraph, condId), nullptr);
    }

    const DGraphChild* thenChild =
        findChildDGraphs(*fpgaDGraph, condId, DGraphChildRole::ConditionalThen);
    ASSERT_NE(thenChild, nullptr);
    ASSERT_FALSE(thenChild->dgraphs.empty());
    EXPECT_NE(findCompiledNode(*thenChild->dgraphs.front(), thenKId), nullptr);

    const DGraphChild* elseChild =
        findChildDGraphs(*fpgaDGraph, condId, DGraphChildRole::ConditionalElse);
    ASSERT_NE(elseChild, nullptr);
    ASSERT_FALSE(elseChild->dgraphs.empty());
    EXPECT_NE(findCompiledNode(*elseChild->dgraphs.front(), elseKId), nullptr);
}

// Phase F.1b: a data-dependent while-loop whose predicate reads a parent scalar
// that the body produces (FPGA kernel output scalar exported to the parent each
// iteration) runs autonomously on the FPGA queue.  The device lowering aliases
// the parent scalar to the body output's SCALAR_READ slot and uses it as the
// LOOP predicate signal.
TEST(RegionCompilerTest, WhileLoopWithExportedFpgaPredicatePlacedOnFpgaQueue) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("fpga:0", DeviceType::FPGA));

    GraphScalar counter = graph.globalScalar(ScalarType::U32, "counter");

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    GraphScalar localNext = body->scalar(ScalarType::U32, "next");
    IOTypeMap bodyType;
    bodyType.outputScalars.push_back({"out", ScalarType::U32});
    IOMap bodyIo;
    bodyIo.bindOutputScalar("out", localNext);
    std::string bodyKId = body->addKernel(
        KernelDescriptor{"body", DeviceType::FPGA, std::nullopt, bodyType}, bodyIo, "fpga:0");
    body->exportToParent({{localNext, counter}}, {bodyKId});

    std::string loopId = graph.addLoop(whileLoopSpec(
        Condition::compare(CompareOp::LT, ConditionOperand::scalar(ScalarType::U32, "counter"),
                           ConditionOperand::constant<uint32_t>(4)),
        body));

    auto dgraphs = compileForInspection(graph);

    const DGraph* fpgaDGraph = findDGraph(dgraphs, "fpga:0");
    ASSERT_NE(fpgaDGraph, nullptr);
    const CompiledNode* loopNode = findCompiledNode(*fpgaDGraph, loopId);
    ASSERT_NE(loopNode, nullptr);
    ASSERT_TRUE(std::holds_alternative<CompiledLoopNode>(*loopNode));
    EXPECT_EQ(std::get<CompiledLoopNode>(*loopNode).loopKind,
              CompiledLoopKind::WhileCondition);

    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    if (cpuDGraph) {
        EXPECT_EQ(findCompiledNode(*cpuDGraph, loopId), nullptr);
    }

    const DGraphChild* bodyChild =
        findChildDGraphs(*fpgaDGraph, loopId, DGraphChildRole::LoopBody);
    ASSERT_NE(bodyChild, nullptr);
    ASSERT_FALSE(bodyChild->dgraphs.empty());
    EXPECT_NE(findCompiledNode(*bodyChild->dgraphs.front(), bodyKId), nullptr);
}

// A fixed-count loop whose body contains a CPU kernel is cross-device and stays
// on the CPU-owned control path (autonomous placement is all-or-nothing).
TEST(RegionCompilerTest, FixedCountLoopWithCpuBodyStaysOnCpu) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("fpga:0", DeviceType::FPGA));

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    body->addKernel(cpuKernel("body"), IOMap{}, "cpu");
    std::string loopId =
        graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    EXPECT_NE(findCompiledNode(*cpuDGraph, loopId), nullptr);
}

// Phase D2: a fixed-count loop whose body spans FPGA + CPU is split into per-
// queue slices: the control node is replicated onto both devices, and the
// in-body cross-device bridge becomes SIGNAL/WAIT rendezvous (no plain data
// bridge left in the FPGA slice).
TEST(RegionCompilerTest, CrossDeviceLoopSplitsIntoPerQueueRendezvous) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("fpga:0", DeviceType::FPGA));

    IOTypeMap outT;
    outT.outputs.push_back({"out", BufferType::I32});
    IOTypeMap inOutT;
    inOutT.inputs.push_back({"in", BufferType::I32});
    inOutT.outputs.push_back({"out", BufferType::I32});

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    IOMap fIo;
    GraphBuffer produced;
    fIo.bindOutput("out", BufferType::I32, produced, size, body->scopeId());
    const std::string fpgaKId = body->addKernel(
        KernelDescriptor{"fk", DeviceType::FPGA, std::nullopt, outT}, std::move(fIo), "fpga:0");

    IOMap cIo;
    GraphBuffer consumed;
    cIo.bindInput("in", produced)
       .bindOutput("out", BufferType::I32, consumed, body->scopeId());
    const std::string cpuKId =
        body->addKernel(cpuKernel("ck", inOutT), std::move(cIo), "cpu", {fpgaKId});

    const std::string loopId =
        graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);

    const DGraph* fpgaDG = findDGraph(dgraphs, "fpga:0");
    const DGraph* cpuDG  = findDGraph(dgraphs, "cpu");
    ASSERT_NE(fpgaDG, nullptr);
    ASSERT_NE(cpuDG, nullptr);

    // Control node replicated onto both queues.
    const CompiledNode* fpgaLoop = findCompiledNode(*fpgaDG, loopId);
    const CompiledNode* cpuLoop  = findCompiledNode(*cpuDG, loopId);
    ASSERT_NE(fpgaLoop, nullptr);
    ASSERT_NE(cpuLoop, nullptr);
    ASSERT_FALSE(fpgaDG->nodes.empty());
    EXPECT_EQ(compiledNodeId(fpgaDG->nodes.front()), loopId)
        << "FPGA follower LOOP must remain RP1 node zero";
    EXPECT_TRUE(std::holds_alternative<CompiledLoopNode>(*fpgaLoop));
    EXPECT_TRUE(std::holds_alternative<CompiledLoopNode>(*cpuLoop));
    const auto& fpgaControl = std::get<CompiledLoopNode>(*fpgaLoop);
    const auto& cpuControl = std::get<CompiledLoopNode>(*cpuLoop);
    EXPECT_EQ(fpgaControl.broadcastRole, SplitBroadcastRole::Follower);
    EXPECT_EQ(cpuControl.broadcastRole, SplitBroadcastRole::Authority);
    EXPECT_EQ(fpgaControl.conditionBroadcastSlot,
              cpuControl.conditionBroadcastSlot);
    EXPECT_EQ(fpgaControl.broadcastReadySlot,
              cpuControl.broadcastReadySlot);
    EXPECT_EQ(fpgaControl.broadcastAckSlot,
              cpuControl.broadcastAckSlot);
    EXPECT_NE(fpgaControl.conditionBroadcastSlot,
              fpgaControl.broadcastReadySlot);
    EXPECT_NE(fpgaControl.conditionBroadcastSlot,
              fpgaControl.broadcastAckSlot);
    EXPECT_NE(fpgaControl.broadcastReadySlot,
              fpgaControl.broadcastAckSlot);

    // FPGA slice: contains the FPGA kernel + rendezvous, and NO plain data bridge.
    const DGraphChild* fpgaBody =
        findChildDGraphs(*fpgaDG, loopId, DGraphChildRole::LoopBody);
    ASSERT_NE(fpgaBody, nullptr);
    ASSERT_FALSE(fpgaBody->dgraphs.empty());
    int fpgaSignals = 0, fpgaWaits = 0, fpgaBridges = 0;
    bool sawFpgaKernel = false;
    std::set<std::uint32_t> fpgaSlots;
    for (const auto& s : fpgaBody->dgraphs) {
        for (const auto& n : s->nodes) {
            if (std::holds_alternative<CompiledBridgeOpNode>(n)) ++fpgaBridges;
            if (const auto* sn = std::get_if<CompiledSignalNode>(&n)) { ++fpgaSignals; fpgaSlots.insert(sn->slot); }
            if (const auto* wn = std::get_if<CompiledWaitNode>(&n))   { ++fpgaWaits;   fpgaSlots.insert(wn->slot); }
            if (compiledNodeId(n) == fpgaKId) sawFpgaKernel = true;
        }
    }
    EXPECT_TRUE(sawFpgaKernel);
    EXPECT_GT(fpgaSignals + fpgaWaits, 0) << "FPGA slice missing rendezvous";
    EXPECT_EQ(fpgaBridges, 0) << "FPGA slice should have no plain data bridge after conversion";

    // CPU slice: contains the CPU kernel + rendezvous on matching slots.
    const DGraphChild* cpuBody =
        findChildDGraphs(*cpuDG, loopId, DGraphChildRole::LoopBody);
    ASSERT_NE(cpuBody, nullptr);
    bool sawCpuKernel = false;
    int cpuRendezvous = 0;
    std::set<std::uint32_t> cpuSlots;
    for (const auto& s : cpuBody->dgraphs) {
        for (const auto& n : s->nodes) {
            if (const auto* sn = std::get_if<CompiledSignalNode>(&n)) { ++cpuRendezvous; cpuSlots.insert(sn->slot); }
            if (const auto* wn = std::get_if<CompiledWaitNode>(&n))   { ++cpuRendezvous; cpuSlots.insert(wn->slot); }
            if (compiledNodeId(n) == cpuKId) sawCpuKernel = true;
        }
    }
    EXPECT_TRUE(sawCpuKernel);
    EXPECT_GT(cpuRendezvous, 0) << "CPU slice missing rendezvous";
    // The queues rendezvous on the same slots.
    std::vector<std::uint32_t> shared;
    std::set_intersection(fpgaSlots.begin(), fpgaSlots.end(), cpuSlots.begin(), cpuSlots.end(),
                          std::back_inserter(shared));
    EXPECT_FALSE(shared.empty()) << "FPGA and CPU rendezvous must share slot(s)";
}

// A multi-stage cross-device ping-pong body (CPU->FPGA->CPU->FPGA->CPU) has
// four cross-device bridges.  Every bridge's producer and consumer halves must
// rendezvous on the same slot, so each CPU rendezvous slot must reappear on the
// FPGA queue.
TEST(RegionCompilerTest, MultiStageCrossDeviceLoopRendezvousSlotsMatchPerBridge) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("fpga:0", DeviceType::FPGA));

    IOTypeMap outT;
    outT.outputs.push_back({"out", BufferType::I32});
    IOTypeMap inOutT;
    inOutT.inputs.push_back({"in", BufferType::I32});
    inOutT.outputs.push_back({"out", BufferType::I32});

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    const auto scope = body->scopeId();

    IOMap io0;
    GraphBuffer staged;
    io0.bindOutput("out", BufferType::I32, staged, size, scope);
    const std::string stageId =
        body->addKernel(cpuKernel("cpu_stage", outT), std::move(io0), "cpu");

    IOMap io1;
    GraphBuffer afterA;
    io1.bindInput("in", staged)
       .bindOutput("out", BufferType::I32, afterA, scope);
    const std::string fAId = body->addKernel(
        KernelDescriptor{"fa", DeviceType::FPGA, std::nullopt, inOutT},
        std::move(io1), "fpga:0", {stageId});

    IOMap io2;
    GraphBuffer bumped;
    io2.bindInput("in", afterA)
       .bindOutput("out", BufferType::I32, bumped, scope);
    const std::string sparseId =
        body->addKernel(cpuKernel("cpu_sparse", inOutT), std::move(io2), "cpu", {fAId});

    IOMap io3;
    GraphBuffer afterB;
    io3.bindInput("in", bumped)
       .bindOutput("out", BufferType::I32, afterB, scope);
    const std::string fBId = body->addKernel(
        KernelDescriptor{"fb", DeviceType::FPGA, std::nullopt, inOutT},
        std::move(io3), "fpga:0", {sparseId});

    IOMap io4;
    GraphBuffer result;
    io4.bindInput("in", afterB)
       .bindOutput("out", BufferType::I32, result, scope);
    body->addKernel(cpuKernel("cpu_fin", inOutT), std::move(io4), "cpu", {fBId});

    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);
    const DGraph* fpgaDG = findDGraph(dgraphs, "fpga:0");
    const DGraph* cpuDG  = findDGraph(dgraphs, "cpu");
    ASSERT_NE(fpgaDG, nullptr);
    ASSERT_NE(cpuDG, nullptr);

    auto collectSlots = [](const DGraph& dg, const std::string& loopRole) {
        std::set<std::uint32_t> slots;
        const DGraphChild* bodyChild = findChildDGraphs(dg, loopRole, DGraphChildRole::LoopBody);
        if (!bodyChild) return slots;
        for (const auto& s : bodyChild->dgraphs) {
            for (const auto& n : s->nodes) {
                if (const auto* sn = std::get_if<CompiledSignalNode>(&n)) slots.insert(sn->slot);
                if (const auto* wn = std::get_if<CompiledWaitNode>(&n)) slots.insert(wn->slot);
            }
        }
        return slots;
    };

    std::string loopId;
    for (const auto& n : cpuDG->nodes) {
        if (std::holds_alternative<CompiledLoopNode>(n)) loopId = compiledNodeId(n);
    }
    ASSERT_FALSE(loopId.empty());

    const std::set<std::uint32_t> cpuSlots = collectSlots(*cpuDG, loopId);
    const std::set<std::uint32_t> fpgaSlots = collectSlots(*fpgaDG, loopId);
    ASSERT_FALSE(cpuSlots.empty());
    ASSERT_FALSE(fpgaSlots.empty());

    for (std::uint32_t slot : cpuSlots) {
        EXPECT_TRUE(fpgaSlots.count(slot))
            << "CPU rendezvous slot " << slot << " has no matching FPGA rendezvous";
    }
}

TEST(RegionCompilerTest, SplitLoopCpuOutputConsumerUsesCpuDeliveredParentBuffer) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("fpga:0", DeviceType::FPGA));

    IOTypeMap outT;
    outT.outputs.push_back({"out", BufferType::I32});
    IOTypeMap inOutT;
    inOutT.inputs.push_back({"in", BufferType::I32});
    inOutT.outputs.push_back({"out", BufferType::I32});

    GraphBuffer raw = graph.inputBuffer(BufferType::I32, "raw", testSize(graph.rootRegion()));
    IOMap preIo;
    GraphBuffer pre;
    preIo.bindInput("in", raw)
         .bindOutput("out", BufferType::I32, pre, graph.rootRegion().scopeId());
    graph.addNode(cpuKernel("cpu_pre", inOutT), std::move(preIo), "cpu");

    GraphBuffer post = GraphBuffer::make(BufferType::I32, "post",
                                         graph.rootRegion().scopeId(),
                                         raw.sizeScalar());
    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    const auto scope = body->scopeId();
    GraphBuffer localState = body->inputBuffer(BufferType::I32, "state", raw.sizeScalar());
    body->importFromParent(std::vector<BufferBoundaryMapping>{{pre, localState}});

    IOMap io0;
    GraphBuffer staged;
    io0.bindInput("in", localState)
       .bindOutput("out", BufferType::I32, staged, scope);
    const std::string stageId =
        body->addKernel(cpuKernel("cpu_stage", inOutT), std::move(io0), "cpu");

    IOMap io1;
    GraphBuffer afterA;
    io1.bindInput("in", staged)
       .bindOutput("out", BufferType::I32, afterA, scope);
    const std::string fAId = body->addKernel(
        KernelDescriptor{"fa", DeviceType::FPGA, std::nullopt, inOutT},
        std::move(io1), "fpga:0", {stageId});

    IOMap io2;
    GraphBuffer bumped;
    io2.bindInput("in", afterA)
       .bindOutput("out", BufferType::I32, bumped, scope);
    const std::string sparseId =
        body->addKernel(cpuKernel("cpu_sparse", inOutT), std::move(io2), "cpu", {fAId});

    IOMap io3;
    GraphBuffer afterB;
    io3.bindInput("in", bumped)
       .bindOutput("out", BufferType::I32, afterB, scope);
    const std::string fBId = body->addKernel(
        KernelDescriptor{"fb", DeviceType::FPGA, std::nullopt, inOutT},
        std::move(io3), "fpga:0", {sparseId});

    IOMap io4;
    GraphBuffer finalState;
    io4.bindInput("in", afterB)
       .bindOutput("out", BufferType::I32, finalState, scope);
    const std::string finalId =
        body->addKernel(cpuKernel("cpu_finalize", inOutT), std::move(io4), "cpu", {fBId});
    body->exportToParent(std::vector<BufferBoundaryMapping>{{finalState, post},
                                                            {finalState, pre}},
                         {finalId});

    const std::string loopId =
        graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    IOMap consumeIo;
    GraphBuffer consumed;
    consumeIo.bindInput("in", post)
             .bindOutput("out", BufferType::I32, consumed, graph.rootRegion().scopeId());
    const std::string consumerId =
        graph.addNode(cpuKernel("cpu_consume_post", inOutT), std::move(consumeIo), "cpu");

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);
    const DGraph* cpuDG = findDGraph(dgraphs, "cpu");
    const DGraph* fpgaDG = findDGraph(dgraphs, "fpga:0");
    ASSERT_NE(cpuDG, nullptr);
    ASSERT_NE(fpgaDG, nullptr);
    ASSERT_FALSE(fpgaDG->nodes.empty());
    EXPECT_EQ(compiledNodeId(fpgaDG->nodes.front()), loopId)
        << "top-level rendezvous packets must follow the FPGA LOOP";

    const CompiledNode* cpuLoop = findCompiledNode(*cpuDG, loopId);
    ASSERT_NE(cpuLoop, nullptr);
    bool waitsForTopLevelStaging = false;
    for (const std::string& dep : compiledNodeDependsOn(*cpuLoop)) {
        if (dep.rfind("__event_", 0) == 0 ||
            dep.rfind("__route_", 0) == 0) {
            waitsForTopLevelStaging = true;
        }
    }
    EXPECT_TRUE(waitsForTopLevelStaging)
        << "CPU split-loop replica must not block before FPGA input staging";

    const CompiledNode* consumer = findCompiledNode(*cpuDG, consumerId);
    ASSERT_NE(consumer, nullptr);
    EXPECT_TRUE(dependsOn(*consumer, loopId));
    for (const std::string& dep : compiledNodeDependsOn(*consumer)) {
        EXPECT_NE(dep.rfind("_top_rdv_", 0), 0u)
            << "CPU consumer of CPU-delivered split-loop output should not "
               "depend on a top-level FPGA->CPU bridge";
    }

    const DGraphChild* fpgaBody =
        findChildDGraphs(*fpgaDG, loopId, DGraphChildRole::LoopBody);
    ASSERT_NE(fpgaBody, nullptr);
    std::size_t bodyRendezvous = 0;
    for (const auto& s : fpgaBody->dgraphs) {
        for (const auto& n : s->nodes) {
            if (std::holds_alternative<CompiledSignalNode>(n) ||
                std::holds_alternative<CompiledWaitNode>(n)) {
                ++bodyRendezvous;
            }
        }
    }
    EXPECT_GT(bodyRendezvous, 0u)
        << "the split loop still needs body rendezvous for CPU<->FPGA edges";
}

// Top-level CPU<->FPGA transfers are host actions, so the compiler must keep
// the transfer closure on the CPU DGraph and leave only RP1-executable
// SIGNAL/WAIT rendezvous nodes on the FPGA DGraph.
TEST(RegionCompilerTest, TopLevelCpuFpgaBridgeMovesHostActionToCpuDGraph) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("fpga:0", DeviceType::FPGA));

    IOTypeMap cpuOutT;
    cpuOutT.outputs.push_back({"out", BufferType::I32});
    IOTypeMap fpgaInT;
    fpgaInT.inputs.push_back({"in", BufferType::I32});

    IOMap cIo;
    GraphBuffer produced;
    cIo.bindOutput("out", BufferType::I32, produced, testSize(graph.rootRegion()));
    const std::string cpuProducer =
        graph.addNode(cpuKernel("produce", cpuOutT), std::move(cIo), "cpu");

    IOMap fIo;
    fIo.bindInput("in", produced);
    const std::string fpgaConsumer = graph.addNode(
        KernelDescriptor{"consume", DeviceType::FPGA, std::nullopt, fpgaInT},
        std::move(fIo), "fpga:0");

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);
    const DGraph* cpuDG = findDGraph(dgraphs, "cpu");
    const DGraph* fpgaDG = findDGraph(dgraphs, "fpga:0");
    ASSERT_NE(cpuDG, nullptr);
    ASSERT_NE(fpgaDG, nullptr);

    int cpuBridges = 0, cpuSignals = 0;
    std::set<std::uint32_t> cpuSlots;
    for (const CompiledNode& n : cpuDG->nodes) {
        if (std::holds_alternative<CompiledBridgeOpNode>(n)) ++cpuBridges;
        if (const auto* s = std::get_if<CompiledSignalNode>(&n)) {
            ++cpuSignals;
            cpuSlots.insert(s->slot);
        }
    }

    int fpgaBridges = 0, fpgaWaits = 0;
    std::set<std::uint32_t> fpgaSlots;
    bool consumerGated = false;
    const CompiledNode* fpgaNode = findCompiledNode(*fpgaDG, fpgaConsumer);
    ASSERT_NE(fpgaNode, nullptr);
    EXPECT_TRUE(std::holds_alternative<CompiledKernelNode>(*fpgaNode));
    for (const CompiledNode& n : fpgaDG->nodes) {
        if (std::holds_alternative<CompiledBridgeOpNode>(n)) ++fpgaBridges;
        if (const auto* w = std::get_if<CompiledWaitNode>(&n)) {
            ++fpgaWaits;
            fpgaSlots.insert(w->slot);
        }
    }
    for (const std::string& dependency :
         compiledNodeDependsOn(*fpgaNode)) {
        consumerGated |= dependency.rfind("__event_", 0) == 0;
    }

    EXPECT_TRUE(consumerGated)
        << "FPGA consumer should wait for the CPU-owned transfer";
    EXPECT_EQ(cpuBridges, 2);
    EXPECT_GE(cpuSignals, 1);
    EXPECT_EQ(fpgaBridges, 0);
    EXPECT_GE(fpgaWaits, 1);

    std::vector<std::uint32_t> shared;
    std::set_intersection(cpuSlots.begin(), cpuSlots.end(), fpgaSlots.begin(), fpgaSlots.end(),
                          std::back_inserter(shared));
    EXPECT_FALSE(shared.empty()) << "CPU signal and FPGA wait must share a rendezvous slot";
}

TEST(RegionCompilerTest, InsertsDeviceCopyForSameFpgaDifferentRegion) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    auto fpga = std::make_shared<RegionAwareStubDevice>("fpga:0", DeviceType::FPGA);
    fpga->setRegion("produce", "out", "HBM0");
    fpga->setRegion("consume", "in", "HBM1");
    graph.registerDevice(fpga);

    GraphScalar size = testSize(graph.rootRegion());
    IOTypeMap produceType = singleOutputType();
    IOMap produceIo;
    GraphBuffer produced;
    produceIo.bindOutput("out", BufferType::I32, produced, size);
    const std::string producerId = graph.addNode(
        KernelDescriptor{"produce", DeviceType::FPGA, std::nullopt, produceType},
        std::move(produceIo), "fpga:0");

    IOTypeMap consumeType;
    consumeType.inputs.push_back({"in", BufferType::I32});
    IOMap consumeIo;
    consumeIo.bindInput("in", produced);
    const std::string consumerId = graph.addNode(
        KernelDescriptor{"consume", DeviceType::FPGA, std::nullopt, consumeType},
        std::move(consumeIo), "fpga:0");

    auto dgraphs = compileForInspection(graph);
    const DGraph* fpgaDGraph = findDGraph(dgraphs, "fpga:0");
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(fpgaDGraph, nullptr);
    ASSERT_NE(cpuDGraph, nullptr);
    const CompiledDeviceCopyNode* copy = findDeviceCopyNode(*cpuDGraph);
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->sourceRegion, "HBM0");
    EXPECT_EQ(copy->targetRegion, "HBM1");
    EXPECT_EQ(copy->source.name(), produced.name());
    EXPECT_NE(copy->target.name(), produced.name());

    const CompiledNode* consumer = findCompiledNode(*fpgaDGraph, consumerId);
    ASSERT_NE(consumer, nullptr);
    bool waitsForCopy = false;
    for (const std::string& dependency :
         compiledNodeDependsOn(*consumer)) {
        waitsForCopy |= dependency.rfind("__event_", 0) == 0 ||
                        dependency.rfind("__route_", 0) == 0;
    }
    EXPECT_TRUE(waitsForCopy);
    (void)producerId;
}

TEST(RegionCompilerTest, SkipsDeviceCopyWhenRegionsMatch) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    auto fpga = std::make_shared<RegionAwareStubDevice>("fpga:0", DeviceType::FPGA);
    fpga->setRegion("produce", "out", "HBM0");
    fpga->setRegion("consume", "in", "HBM0");
    graph.registerDevice(fpga);

    GraphScalar size = testSize(graph.rootRegion());
    IOMap produceIo;
    GraphBuffer produced;
    produceIo.bindOutput("out", BufferType::I32, produced, size);
    const std::string producerId = graph.addNode(
        KernelDescriptor{"produce", DeviceType::FPGA, std::nullopt, singleOutputType()},
        std::move(produceIo), "fpga:0");

    IOTypeMap consumeType;
    consumeType.inputs.push_back({"in", BufferType::I32});
    IOMap consumeIo;
    consumeIo.bindInput("in", produced);
    const std::string consumerId = graph.addNode(
        KernelDescriptor{"consume", DeviceType::FPGA, std::nullopt, consumeType},
        std::move(consumeIo), "fpga:0");

    auto dgraphs = compileForInspection(graph);
    EXPECT_EQ(countDeviceCopyNodes(dgraphs), 0u);
    const DGraph* fpgaDGraph = findDGraph(dgraphs, "fpga:0");
    ASSERT_NE(fpgaDGraph, nullptr);
    const CompiledNode* consumer = findCompiledNode(*fpgaDGraph, consumerId);
    ASSERT_NE(consumer, nullptr);
    EXPECT_TRUE(dependsOn(*consumer, producerId));
}

TEST(RegionCompilerTest, SkipsDeviceCopyWhenDeviceReportsNoRegions) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("fpga:0", DeviceType::FPGA));

    GraphScalar size = testSize(graph.rootRegion());
    IOMap produceIo;
    GraphBuffer produced;
    produceIo.bindOutput("out", BufferType::I32, produced, size);
    const std::string producerId = graph.addNode(
        KernelDescriptor{"produce", DeviceType::FPGA, std::nullopt, singleOutputType()},
        std::move(produceIo), "fpga:0");

    IOTypeMap consumeType;
    consumeType.inputs.push_back({"in", BufferType::I32});
    IOMap consumeIo;
    consumeIo.bindInput("in", produced);
    const std::string consumerId = graph.addNode(
        KernelDescriptor{"consume", DeviceType::FPGA, std::nullopt, consumeType},
        std::move(consumeIo), "fpga:0");

    auto dgraphs = compileForInspection(graph);
    EXPECT_EQ(countDeviceCopyNodes(dgraphs), 0u);
    const DGraph* fpgaDGraph = findDGraph(dgraphs, "fpga:0");
    ASSERT_NE(fpgaDGraph, nullptr);
    const CompiledNode* consumer = findCompiledNode(*fpgaDGraph, consumerId);
    ASSERT_NE(consumer, nullptr);
    EXPECT_TRUE(dependsOn(*consumer, producerId));
}

TEST(RegionCompilerTest, DoesNotDeviceCopyInoutBuffers) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    auto fpga = std::make_shared<RegionAwareStubDevice>("fpga:0", DeviceType::FPGA);
    fpga->setRegion("produce", "out", "HBM0");
    graph.registerDevice(fpga);

    GraphScalar size = testSize(graph.rootRegion());
    IOMap produceIo;
    GraphBuffer produced;
    produceIo.bindOutput("out", BufferType::I32, produced, size);
    graph.addNode(
        KernelDescriptor{"produce", DeviceType::FPGA, std::nullopt, singleOutputType()},
        std::move(produceIo), "fpga:0");

    IOTypeMap rwType;
    rwType.inouts.push_back({{"rw_in", BufferType::I32}, {"rw_out", BufferType::I32}});
    IOMap rwIo;
    GraphBuffer updated;
    rwIo.bindInout("rw_in", "rw_out", produced, updated);
    graph.addNode(
        KernelDescriptor{"mutate", DeviceType::FPGA, std::nullopt, rwType},
        std::move(rwIo), "fpga:0");

    auto dgraphs = compileForInspection(graph);
    EXPECT_EQ(countDeviceCopyNodes(dgraphs), 0u);
}

TEST(RegionCompilerTest, SharesDeviceCopyForFanoutToSameRegion) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    auto fpga = std::make_shared<RegionAwareStubDevice>("fpga:0", DeviceType::FPGA);
    fpga->setRegion("produce", "out", "HBM0");
    fpga->setRegion("consume_a", "in", "HBM1");
    fpga->setRegion("consume_b", "in", "HBM1");
    graph.registerDevice(fpga);

    GraphScalar size = testSize(graph.rootRegion());
    IOMap produceIo;
    GraphBuffer produced;
    produceIo.bindOutput("out", BufferType::I32, produced, size);
    graph.addNode(
        KernelDescriptor{"produce", DeviceType::FPGA, std::nullopt, singleOutputType()},
        std::move(produceIo), "fpga:0");

    IOTypeMap consumeType;
    consumeType.inputs.push_back({"in", BufferType::I32});
    for (const std::string& name : {"consume_a", "consume_b"}) {
        IOMap consumeIo;
        consumeIo.bindInput("in", produced);
        graph.addNode(
            KernelDescriptor{name, DeviceType::FPGA, std::nullopt, consumeType},
            std::move(consumeIo), "fpga:0");
    }

    auto dgraphs = compileForInspection(graph);
    EXPECT_EQ(countDeviceCopyNodes(dgraphs), 1u);
}

TEST(RegionCompilerTest, GraphInputFanoutCopiesToSecondFpgaRegion) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    auto fpga = std::make_shared<RegionAwareStubDevice>("fpga:0", DeviceType::FPGA);
    fpga->setRegion("consume_a", "in", "HBM0");
    fpga->setRegion("consume_b", "in", "HBM1");
    graph.registerDevice(fpga);

    GraphScalar size = testSize(graph.rootRegion());
    GraphBuffer input = graph.rootRegion().inputBuffer(BufferType::I32, "input", size);
    IOTypeMap consumeType;
    consumeType.inputs.push_back({"in", BufferType::I32});
    for (const std::string& name : {"consume_a", "consume_b"}) {
        IOMap io;
        io.bindInput("in", input);
        graph.addNode(
            KernelDescriptor{name, DeviceType::FPGA, std::nullopt, consumeType},
            std::move(io), "fpga:0");
    }

    auto dgraphs = compileForInspection(graph);
    EXPECT_EQ(countDeviceCopyNodes(dgraphs), 1u)
        << "one canonical staging route should feed a second-region copy";
}

TEST(RegionCompilerTest, NestedDeviceCopyIsRejected) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    auto fpga = std::make_shared<RegionAwareStubDevice>("fpga:0", DeviceType::FPGA);
    fpga->setRegion("produce", "out", "HBM0");
    fpga->setRegion("consume", "in", "HBM1");
    graph.registerDevice(fpga);

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    IOMap produceIo;
    GraphBuffer produced;
    produceIo.bindOutput("out", BufferType::I32, produced, size, body->scopeId());
    const std::string producerId = body->addKernel(
        KernelDescriptor{"produce", DeviceType::FPGA, std::nullopt, singleOutputType()},
        std::move(produceIo), "fpga:0");

    IOTypeMap consumeType;
    consumeType.inputs.push_back({"in", BufferType::I32});
    IOMap consumeIo;
    consumeIo.bindInput("in", produced);
    body->addKernel(
        KernelDescriptor{"consume", DeviceType::FPGA, std::nullopt, consumeType},
        std::move(consumeIo), "fpga:0", {producerId});

    graph.addLoop(fixedLoopSpec(
        tripCount(tripCountScalar(graph.rootRegion())), body));

    EXPECT_THROW(compileForInspection(graph), GraphCompileError);
}

// Phase F.3b: a *data-dependent* (while) loop whose body spans FPGA + CPU splits
// into per-queue replicas with broadcast roles: the CPU replica is the Authority
// (evaluates the host condition and broadcasts its decision) and the FPGA replica
// is the Follower (reads the broadcast as its exit predicate).  Both replicas
// share the same broadcast handshake slots.
TEST(RegionCompilerTest, DataDependentCrossDeviceLoopSplitsWithBroadcastRoles) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("fpga:0", DeviceType::FPGA));

    // Main-line CPU producer for the host-evaluated loop predicate scalar.
    GraphScalar n = graph.globalScalar(ScalarType::U32, "n");
    IOTypeMap initType;
    initType.outputScalars.push_back({"out", ScalarType::U32});
    IOMap initIo;
    initIo.bindOutputScalar("out", n);
    std::string initId = graph.addNode(cpuKernel("init", initType), std::move(initIo), "cpu");

    IOTypeMap outT;
    outT.outputs.push_back({"out", BufferType::I32});
    IOTypeMap inOutT;
    inOutT.inputs.push_back({"in", BufferType::I32});
    inOutT.outputs.push_back({"out", BufferType::I32});

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    IOMap fIo;
    GraphBuffer produced;
    fIo.bindOutput("out", BufferType::I32, produced, size, body->scopeId());
    const std::string fpgaKId = body->addKernel(
        KernelDescriptor{"fk", DeviceType::FPGA, std::nullopt, outT}, std::move(fIo), "fpga:0");
    IOMap cIo;
    GraphBuffer consumed;
    cIo.bindInput("in", produced)
       .bindOutput("out", BufferType::I32, consumed, body->scopeId());
    const std::string cpuKId =
        body->addKernel(cpuKernel("ck", inOutT), std::move(cIo), "cpu", {fpgaKId});

    const std::string loopId = graph.addLoop(whileLoopSpec(
        Condition::compare(CompareOp::LT, ConditionOperand::scalar(ScalarType::U32, "n"),
                           ConditionOperand::constant<uint32_t>(4)),
        body, {initId}));

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);

    const DGraph* fpgaDG = findDGraph(dgraphs, "fpga:0");
    const DGraph* cpuDG  = findDGraph(dgraphs, "cpu");
    ASSERT_NE(fpgaDG, nullptr);
    ASSERT_NE(cpuDG, nullptr);
    const CompiledNode* fpgaLoop = findCompiledNode(*fpgaDG, loopId);
    const CompiledNode* cpuLoop  = findCompiledNode(*cpuDG, loopId);
    ASSERT_NE(fpgaLoop, nullptr);
    ASSERT_NE(cpuLoop, nullptr);
    ASSERT_TRUE(std::holds_alternative<CompiledLoopNode>(*fpgaLoop));
    ASSERT_TRUE(std::holds_alternative<CompiledLoopNode>(*cpuLoop));
    const auto& fl = std::get<CompiledLoopNode>(*fpgaLoop);
    const auto& cl = std::get<CompiledLoopNode>(*cpuLoop);

    EXPECT_EQ(fl.broadcastRole, SplitBroadcastRole::Follower);
    EXPECT_EQ(cl.broadcastRole, SplitBroadcastRole::Authority);
    // Both replicas share the same broadcast handshake slots, all distinct.
    EXPECT_EQ(fl.conditionBroadcastSlot, cl.conditionBroadcastSlot);
    EXPECT_EQ(fl.broadcastReadySlot, cl.broadcastReadySlot);
    EXPECT_EQ(fl.broadcastAckSlot, cl.broadcastAckSlot);
    EXPECT_NE(fl.conditionBroadcastSlot, fl.broadcastReadySlot);
    EXPECT_NE(fl.broadcastReadySlot, fl.broadcastAckSlot);
}

// Phase A: a fixed-count loop with an all-FPGA body that carries a buffer whose
// initial value is produced on the CPU and whose result is consumed on the CPU.
// The loop + its import/export boundaries land on the FPGA queue (no in-body
// bridge), and the loop's I/O is bridged around the control node.
TEST(RegionCompilerTest, FpgaLoopCarriedBufferWithCpuIoPlacesLoopAndBoundariesOnFpga) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("fpga:0", DeviceType::FPGA));

    IOTypeMap kt;
    kt.inputs.push_back({"in", BufferType::I32});
    kt.outputs.push_back({"out", BufferType::I32});

    GraphBuffer raw = graph.inputBuffer(BufferType::I32, "raw", testSize(graph.rootRegion()));
    IOMap initIo;
    GraphBuffer parentState;
    initIo.bindInput("in", raw)
          .bindOutput("out", BufferType::I32, parentState);
    graph.addNode(cpuKernel("init", kt), std::move(initIo), "cpu");

    auto body = graph.rootRegion().createChild();
    GraphBuffer localState = body->inputBuffer(BufferType::I32, "state", parentState.sizeScalar());
    const std::string startId = body->importFromParent(
        std::vector<BufferBoundaryMapping>{{parentState, localState}});
    IOMap bodyIo;
    GraphBuffer localNext;
    bodyIo.bindInput("in", localState)
          .bindOutput("out", BufferType::I32, localNext, body->scopeId());
    const std::string bodyId = body->addKernel(
        KernelDescriptor{"advance", DeviceType::FPGA, std::nullopt, kt},
        std::move(bodyIo), "fpga:0", {startId});
    body->exportToParent(std::vector<BufferBoundaryMapping>{{localNext, parentState}},
                         {bodyId});
    const std::string loopId =
        graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    IOMap consumeIo;
    GraphBuffer finalOut;
    consumeIo.bindInput("in", parentState)
             .bindOutput("out", BufferType::I32, finalOut);
    graph.addNode(cpuKernel("consume", kt), std::move(consumeIo), "cpu");

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);

    const DGraph* fpgaDGraph = findDGraph(dgraphs, "fpga:0");
    ASSERT_NE(fpgaDGraph, nullptr);

    // The loop control node is on the FPGA queue.
    const CompiledNode* loopNode = findCompiledNode(*fpgaDGraph, loopId);
    ASSERT_NE(loopNode, nullptr);
    EXPECT_TRUE(std::holds_alternative<CompiledLoopNode>(*loopNode));

    // The body lives entirely on the FPGA queue with no in-body bridge (the
    // carried-buffer boundaries are FPGA-resident aliases).
    const DGraphChild* bodyChild =
        findChildDGraphs(*fpgaDGraph, loopId, DGraphChildRole::LoopBody);
    ASSERT_NE(bodyChild, nullptr);
    bool bodyBridge = false;
    bool sawStartBoundary = false;
    for (const auto& d : bodyChild->dgraphs) {
        if (!d) continue;
        for (const auto& n : d->nodes) {
            if (std::holds_alternative<CompiledBridgeOpNode>(n)) bodyBridge = true;
            if (compiledNodeId(n) == startId) {
                sawStartBoundary = true;
                EXPECT_EQ(compiledNodeDeviceId(n), "fpga:0");
            }
        }
    }
    EXPECT_FALSE(bodyBridge) << "an in-body cross-device bridge disqualifies autonomy";
    EXPECT_TRUE(sawStartBoundary);

    // The loop's CPU-side I/O is ordered around the control node by FPGA
    // rendezvous nodes; the host bridge actions themselves live on the CPU DGraph.
    int fpgaBridges = 0;
    int fpgaRendezvous = 0;
    for (const auto& n : fpgaDGraph->nodes) {
        if (std::holds_alternative<CompiledBridgeOpNode>(n)) ++fpgaBridges;
        if (std::holds_alternative<CompiledSignalNode>(n) ||
            std::holds_alternative<CompiledWaitNode>(n)) ++fpgaRendezvous;
    }
    EXPECT_EQ(fpgaBridges, 0) << "FPGA DGraph must not own host bridge actions";
    EXPECT_GT(fpgaRendezvous, 0) << "expected entry/exit rendezvous around the FPGA loop";
}

TEST(RegionCompilerTest, CompilerBuildsNestedCrossDeviceBridgesInLoopBody) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("mcpu:0", DeviceType::MOCK_CPU));

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    IOTypeMap outputType = singleOutputType();
    IOTypeMap inOutType = singleInputOutputType();

    IOMap producerIo;
    GraphBuffer cpuProduced;
    producerIo.bindOutput("out", BufferType::I32, cpuProduced, size, body->scopeId());
    std::string cpuProducerId = body->addKernel(cpuKernel("produce", outputType),
                                                std::move(producerIo), "cpu");

    IOMap mockIo;
    GraphBuffer mockProduced;
    mockIo.bindInput("in", cpuProduced)
          .bindOutput("out", BufferType::I32, mockProduced, body->scopeId());
    std::string mockKernelId = body->addKernel(mockCpuKernel("mock", inOutType),
                                               std::move(mockIo), "mcpu:0");

    IOMap cpuConsumerIo;
    GraphBuffer cpuConsumed;
    cpuConsumerIo.bindInput("in", mockProduced)
                 .bindOutput("out", BufferType::I32, cpuConsumed, body->scopeId());
    std::string cpuConsumerId = body->addKernel(cpuKernel("consume", inOutType),
                                                std::move(cpuConsumerIo), "cpu");

    std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);

    const DGraphChild* bodyChild = findChildDGraphs(*cpuDGraph, loopId,
                                                   DGraphChildRole::LoopBody);
    ASSERT_NE(bodyChild, nullptr);
    ASSERT_EQ(bodyChild->dgraphs.size(), 2u);

    const DGraph* cpuChild = findChildDGraph(*bodyChild, "cpu");
    const DGraph* mockChild = findChildDGraph(*bodyChild, "mcpu:0");
    ASSERT_NE(cpuChild, nullptr);
    ASSERT_NE(mockChild, nullptr);

    EXPECT_NE(findCompiledNode(*cpuChild, cpuProducerId), nullptr);
    EXPECT_NE(findCompiledNode(*mockChild, mockKernelId), nullptr);
    EXPECT_NE(findCompiledNode(*cpuChild, cpuConsumerId), nullptr);

    const auto* cpuToMockProducer = findBridgeNode(
        *cpuChild, CompiledBridgeOpNode::Side::Producer, cpuProducerId);
    const auto* cpuToMockConsumer = findBridgeNode(
        *mockChild, CompiledBridgeOpNode::Side::Consumer, mockKernelId);
    const auto* mockToCpuProducer = findBridgeNode(
        *mockChild, CompiledBridgeOpNode::Side::Producer, mockKernelId);
    const auto* mockToCpuConsumer = findBridgeNode(
        *cpuChild, CompiledBridgeOpNode::Side::Consumer, cpuConsumerId);

    ASSERT_NE(cpuToMockProducer, nullptr);
    ASSERT_NE(cpuToMockConsumer, nullptr);
    ASSERT_NE(mockToCpuProducer, nullptr);
    ASSERT_NE(mockToCpuConsumer, nullptr);

    EXPECT_TRUE(dependsOn(*cpuToMockProducer, cpuProducerId));
    EXPECT_TRUE(dependsOn(*mockToCpuProducer, mockKernelId));

    const CompiledNode* mockKernel = findCompiledNode(*mockChild, mockKernelId);
    const CompiledNode* cpuConsumer = findCompiledNode(*cpuChild, cpuConsumerId);
    ASSERT_NE(mockKernel, nullptr);
    ASSERT_NE(cpuConsumer, nullptr);
    EXPECT_TRUE(dependsOn(*mockKernel, cpuToMockConsumer->id));
    EXPECT_TRUE(dependsOn(*cpuConsumer, mockToCpuConsumer->id));
}

TEST(RegionCompilerTest, CompilerBuildsNestedCrossDeviceBridgesInConditionalBranch) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("mcpu:0", DeviceType::MOCK_CPU));

    GraphScalar size = testSize(graph.rootRegion());
    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();
    IOTypeMap outputType = singleOutputType();
    IOTypeMap inOutType = singleInputOutputType();

    IOMap producerIo;
    GraphBuffer cpuProduced;
    producerIo.bindOutput("out", BufferType::I32, cpuProduced,
                          size, thenRegion->scopeId());
    std::string cpuProducerId = thenRegion->addKernel(
        cpuKernel("produce", outputType), std::move(producerIo), "cpu");

    IOMap mockIo;
    GraphBuffer mockProduced;
    mockIo.bindInput("in", cpuProduced)
          .bindOutput("out", BufferType::I32, mockProduced,
                            thenRegion->scopeId());
    std::string mockKernelId = thenRegion->addKernel(
        mockCpuKernel("mock", inOutType), std::move(mockIo), "mcpu:0");

    IOMap cpuConsumerIo;
    GraphBuffer cpuConsumed;
    cpuConsumerIo.bindInput("in", mockProduced)
                 .bindOutput("out", BufferType::I32, cpuConsumed,
                                   thenRegion->scopeId());
    std::string cpuConsumerId = thenRegion->addKernel(
        cpuKernel("consume", inOutType), std::move(cpuConsumerIo), "cpu");

    std::string elseKernelId = elseRegion->addKernel(cpuKernel("else"), IOMap{}, "cpu");
    std::string conditionalId = graph.addConditional(
        ifElseSpec(Condition::alwaysTrue(), thenRegion, elseRegion));

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);

    const CompiledNode* conditionalNode = findCompiledNode(*cpuDGraph, conditionalId);
    ASSERT_NE(conditionalNode, nullptr);
    ASSERT_TRUE(std::holds_alternative<CompiledConditionalNode>(*conditionalNode));

    const DGraphChild* thenChild = findChildDGraphs(
        *cpuDGraph, conditionalId, DGraphChildRole::ConditionalThen);
    const DGraphChild* elseChild = findChildDGraphs(
        *cpuDGraph, conditionalId, DGraphChildRole::ConditionalElse);
    ASSERT_NE(thenChild, nullptr);
    ASSERT_NE(elseChild, nullptr);
    ASSERT_EQ(thenChild->dgraphs.size(), 2u);
    ASSERT_EQ(elseChild->dgraphs.size(), 1u);

    const DGraph* cpuThenChild = findChildDGraph(*thenChild, "cpu");
    const DGraph* mockThenChild = findChildDGraph(*thenChild, "mcpu:0");
    const DGraph* cpuElseChild = findChildDGraph(*elseChild, "cpu");
    ASSERT_NE(cpuThenChild, nullptr);
    ASSERT_NE(mockThenChild, nullptr);
    ASSERT_NE(cpuElseChild, nullptr);

    EXPECT_NE(findCompiledNode(*cpuThenChild, cpuProducerId), nullptr);
    EXPECT_NE(findCompiledNode(*mockThenChild, mockKernelId), nullptr);
    EXPECT_NE(findCompiledNode(*cpuThenChild, cpuConsumerId), nullptr);
    EXPECT_NE(findCompiledNode(*cpuElseChild, elseKernelId), nullptr);

    const auto* cpuToMockProducer = findBridgeNode(
        *cpuThenChild, CompiledBridgeOpNode::Side::Producer, cpuProducerId);
    const auto* cpuToMockConsumer = findBridgeNode(
        *mockThenChild, CompiledBridgeOpNode::Side::Consumer, mockKernelId);
    const auto* mockToCpuProducer = findBridgeNode(
        *mockThenChild, CompiledBridgeOpNode::Side::Producer, mockKernelId);
    const auto* mockToCpuConsumer = findBridgeNode(
        *cpuThenChild, CompiledBridgeOpNode::Side::Consumer, cpuConsumerId);

    ASSERT_NE(cpuToMockProducer, nullptr);
    ASSERT_NE(cpuToMockConsumer, nullptr);
    ASSERT_NE(mockToCpuProducer, nullptr);
    ASSERT_NE(mockToCpuConsumer, nullptr);

    EXPECT_TRUE(dependsOn(*cpuToMockProducer, cpuProducerId));
    EXPECT_TRUE(dependsOn(*mockToCpuProducer, mockKernelId));

    const CompiledNode* mockKernel = findCompiledNode(*mockThenChild, mockKernelId);
    const CompiledNode* cpuConsumer = findCompiledNode(*cpuThenChild, cpuConsumerId);
    ASSERT_NE(mockKernel, nullptr);
    ASSERT_NE(cpuConsumer, nullptr);
    EXPECT_TRUE(dependsOn(*mockKernel, cpuToMockConsumer->id));
    EXPECT_TRUE(dependsOn(*cpuConsumer, mockToCpuConsumer->id));
}

TEST(RegionCompilerTest, CompilerBuildsConditionalControlNodeAndBranchDGraphs) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar size = testSize(graph.rootRegion());
    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();
    std::string thenKernelId = thenRegion->addKernel(cpuKernel("then"), IOMap{}, "cpu");
    std::string elseKernelId = elseRegion->addKernel(cpuKernel("else"), IOMap{}, "cpu");
    std::string conditionalId = graph.addConditional(
        ifElseSpec(Condition::alwaysTrue(), thenRegion, elseRegion));

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);

    const CompiledNode* conditionalNode = findCompiledNode(*cpuDGraph, conditionalId);
    ASSERT_NE(conditionalNode, nullptr);
    ASSERT_TRUE(std::holds_alternative<CompiledConditionalNode>(*conditionalNode));
    const auto& compiledConditional = std::get<CompiledConditionalNode>(*conditionalNode);
    EXPECT_EQ(compiledConditional.condition.op(), CompareOp::AlwaysTrue);

    const DGraphChild* thenChild = findChildDGraphs(*cpuDGraph, conditionalId,
                                                   DGraphChildRole::ConditionalThen);
    const DGraphChild* elseChild = findChildDGraphs(*cpuDGraph, conditionalId,
                                                   DGraphChildRole::ConditionalElse);
    ASSERT_NE(thenChild, nullptr);
    ASSERT_NE(elseChild, nullptr);
    ASSERT_EQ(thenChild->dgraphs.size(), 1u);
    ASSERT_EQ(elseChild->dgraphs.size(), 1u);
    ASSERT_NE(thenChild->dgraphs.front(), nullptr);
    ASSERT_NE(elseChild->dgraphs.front(), nullptr);
    EXPECT_NE(findCompiledNode(*thenChild->dgraphs.front(), thenKernelId), nullptr);
    EXPECT_NE(findCompiledNode(*elseChild->dgraphs.front(), elseKernelId), nullptr);
}

TEST(RegionCompilerTest, CompilerLowersScalarBoundaryMappingsInChildDGraph) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar parentCounter = graph.globalScalar(ScalarType::I32, "counter");
    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    GraphScalar localCounter = body->scalar(ScalarType::I32, "counter");

    std::string startId = body->importFromParent({{parentCounter, localCounter}});
    std::string endId = body->exportToParent({{localCounter, parentCounter}}, {startId});
    std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    const DGraphChild* bodyChild = findChildDGraphs(*cpuDGraph, loopId,
                                                   DGraphChildRole::LoopBody);
    ASSERT_NE(bodyChild, nullptr);
    ASSERT_EQ(bodyChild->dgraphs.size(), 1u);
    ASSERT_NE(bodyChild->dgraphs.front(), nullptr);

    const CompiledNode* startNode = findCompiledNode(*bodyChild->dgraphs.front(), startId);
    const CompiledNode* endNode = findCompiledNode(*bodyChild->dgraphs.front(), endId);
    ASSERT_NE(startNode, nullptr);
    ASSERT_NE(endNode, nullptr);

    const auto& compiledStart = std::get<CompiledBoundaryNode>(*startNode);
    ASSERT_EQ(compiledStart.scalarCopies.size(), 1u);
    EXPECT_EQ(compiledStart.scalarCopies.front().sourceName, parentCounter.varName());
    EXPECT_EQ(compiledStart.scalarCopies.front().sourceScopeId, parentCounter.scopeId());
    EXPECT_EQ(compiledStart.scalarCopies.front().targetName, localCounter.varName());
    EXPECT_EQ(compiledStart.scalarCopies.front().targetScopeId, localCounter.scopeId());

    const auto& compiledEnd = std::get<CompiledBoundaryNode>(*endNode);
    ASSERT_EQ(compiledEnd.scalarCopies.size(), 1u);
    EXPECT_EQ(compiledEnd.scalarCopies.front().sourceName, localCounter.varName());
    EXPECT_EQ(compiledEnd.scalarCopies.front().sourceScopeId, localCounter.scopeId());
    EXPECT_EQ(compiledEnd.scalarCopies.front().targetName, parentCounter.varName());
    EXPECT_EQ(compiledEnd.scalarCopies.front().targetScopeId, parentCounter.scopeId());
}

TEST(RegionCompilerTest, CompilerOrdersScalarBoundaryDependencies) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    IOTypeMap incrementType;
    incrementType.inputScalars.push_back({"in", ScalarType::I32});
    incrementType.outputScalars.push_back({"out", ScalarType::I32});

    GraphScalar parentCounter = graph.globalScalar(ScalarType::I32, "counter");
    auto body = graph.rootRegion().createChild();
    GraphScalar localCounter = body->scalar(ScalarType::I32, "counter");
    GraphScalar localNext = body->scalar(ScalarType::I32, "next");

    std::string startId = body->importFromParent({{parentCounter, localCounter}});
    IOMap bodyIo;
    bodyIo.bindInputScalar("in", localCounter)
          .bindOutputScalar("out", localNext);
    std::string kernelId = body->addKernel(cpuKernel("increment", incrementType),
                                           std::move(bodyIo), "cpu");
    std::string endId = body->exportToParent({{localNext, parentCounter}});
    std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    const DGraphChild* bodyChild = findChildDGraphs(*cpuDGraph, loopId,
                                                   DGraphChildRole::LoopBody);
    ASSERT_NE(bodyChild, nullptr);
    ASSERT_EQ(bodyChild->dgraphs.size(), 1u);

    const DGraph& bodyDGraph = *bodyChild->dgraphs.front();
    const CompiledNode* kernelNode = findCompiledNode(bodyDGraph, kernelId);
    const CompiledNode* endNode = findCompiledNode(bodyDGraph, endId);
    ASSERT_NE(kernelNode, nullptr);
    ASSERT_NE(endNode, nullptr);
    EXPECT_TRUE(dependsOn(*kernelNode, startId));
    EXPECT_TRUE(dependsOn(*endNode, kernelId));
}

TEST(RegionCompilerTest, CompilerAllowsLoopCarriedScalarWithInitialProducer) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar parentCounter = graph.globalScalar(ScalarType::I32, "counter");

    IOTypeMap initType;
    initType.outputScalars.push_back({"out", ScalarType::I32});
    IOMap initIo;
    initIo.bindOutputScalar("out", parentCounter);
    const std::string initId = graph.addNode(cpuKernel("init_counter", initType),
                                             std::move(initIo), "cpu");

    IOTypeMap incrementType;
    incrementType.inputScalars.push_back({"in", ScalarType::I32});
    incrementType.outputScalars.push_back({"out", ScalarType::I32});

    auto body = graph.rootRegion().createChild();
    GraphScalar localCounter = body->scalar(ScalarType::I32, "counter");
    GraphScalar localNext = body->scalar(ScalarType::I32, "next");

    const std::string startId = body->importFromParent({{parentCounter, localCounter}});
    IOMap bodyIo;
    bodyIo.bindInputScalar("in", localCounter)
          .bindOutputScalar("out", localNext);
    const std::string bodyId = body->addKernel(cpuKernel("increment", incrementType),
                                               std::move(bodyIo), "cpu", {startId});
    body->exportToParent({{localNext, parentCounter}}, {bodyId});

    const std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    IOTypeMap consumerType;
    consumerType.inputScalars.push_back({"in", ScalarType::I32});
    IOMap consumerIo;
    consumerIo.bindInputScalar("in", parentCounter);
    const std::string consumerId = graph.addNode(cpuKernel("consume_counter", consumerType),
                                                 std::move(consumerIo), "cpu");

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    const CompiledNode* loopNode = findCompiledNode(*cpuDGraph, loopId);
    const CompiledNode* consumerNode = findCompiledNode(*cpuDGraph, consumerId);
    ASSERT_NE(loopNode, nullptr);
    ASSERT_NE(consumerNode, nullptr);

    EXPECT_TRUE(dependsOn(*loopNode, initId));
    EXPECT_TRUE(dependsOn(*consumerNode, loopId));
    EXPECT_FALSE(dependsOn(*consumerNode, initId));
}

TEST(RegionCompilerTest, CompilerRejectsScalarBoundaryTypeMismatch) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar parentCounter = graph.globalScalar(ScalarType::I32, "counter");
    auto body = graph.rootRegion().createChild();
    GraphScalar localCounter = body->scalar(ScalarType::U32, "counter");

    body->importFromParent({{parentCounter, localCounter}});
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    EXPECT_THROW(graph.compile(), std::runtime_error);
}

// Helper that adds a kernel whose only output is a single I32 scalar bound to
// the given GraphScalar in the given region. Used by the end-boundary tests
// below to ensure the local scalar has a producer before being exported.
std::string addI32ScalarProducerKernel(GraphRegion& region,
                                       const std::string& kernelName,
                                       const GraphScalar& target,
                                       const std::string& device) {
    IOTypeMap kernelType;
    kernelType.outputScalars.push_back({"out", ScalarType::I32});
    IOMap kernelIo;
    kernelIo.bindOutputScalar("out", target);
    return region.addKernel(cpuKernel(kernelName, std::move(kernelType)),
                            std::move(kernelIo), device);
}

TEST(RegionCompilerTest, RootReaderDependsOnLoopForScalarExportedToParent) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar parentCounter = graph.globalScalar(ScalarType::I32, "counter");
    auto body = graph.rootRegion().createChild();
    GraphScalar localCounter = body->scalar(ScalarType::I32, "counter");
    addI32ScalarProducerKernel(*body, "body_produces_counter", localCounter, "cpu");
    body->exportToParent({{localCounter, parentCounter}});
    std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    IOTypeMap consumerType;
    consumerType.inputScalars.push_back({"in", ScalarType::I32});
    IOMap consumerIo;
    consumerIo.bindInputScalar("in", parentCounter);
    std::string consumerId = graph.addNode(cpuKernel("consume_counter", consumerType),
                                           std::move(consumerIo), "cpu");

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    const CompiledNode* consumerNode = findCompiledNode(*cpuDGraph, consumerId);
    ASSERT_NE(consumerNode, nullptr);
    EXPECT_TRUE(dependsOn(*consumerNode, loopId));
}

TEST(RegionCompilerTest, RootReaderDependsOnConditionalWhenSingleBranchExports) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar parentCounter = graph.globalScalar(ScalarType::I32, "counter");

    auto thenRegion = graph.rootRegion().createChild();
    GraphScalar thenLocal = thenRegion->scalar(ScalarType::I32, "counter");
    addI32ScalarProducerKernel(*thenRegion, "then_produces_counter", thenLocal, "cpu");
    thenRegion->exportToParent({{thenLocal, parentCounter}});

    auto elseRegion = graph.rootRegion().createChild();

    std::string condId = graph.addConditional(
        ifElseSpec(Condition::alwaysFalse(), thenRegion, elseRegion));

    IOTypeMap consumerType;
    consumerType.inputScalars.push_back({"in", ScalarType::I32});
    IOMap consumerIo;
    consumerIo.bindInputScalar("in", parentCounter);
    std::string consumerId = graph.addNode(cpuKernel("consume_counter", consumerType),
                                           std::move(consumerIo), "cpu");

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    const CompiledNode* consumerNode = findCompiledNode(*cpuDGraph, consumerId);
    ASSERT_NE(consumerNode, nullptr);
    EXPECT_TRUE(dependsOn(*consumerNode, condId));
}

TEST(RegionCompilerTest, ExplicitProducerCollidingWithControlEndBoundaryThrows) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar parentCounter = graph.globalScalar(ScalarType::I32, "counter");

    IOTypeMap producerType;
    producerType.outputScalars.push_back({"out", ScalarType::I32});
    IOMap producerIo;
    producerIo.bindOutputScalar("out", parentCounter);
    graph.addNode(cpuKernel("explicit_producer", producerType),
                  std::move(producerIo), "cpu");

    auto body = graph.rootRegion().createChild();
    GraphScalar localCounter = body->scalar(ScalarType::I32, "counter");
    addI32ScalarProducerKernel(*body, "body_produces_counter", localCounter, "cpu");
    body->exportToParent({{localCounter, parentCounter}});
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    try {
        (void)graph.compile();
        FAIL() << "expected compile() to throw on multi-producer collision";
    } catch (const std::runtime_error& ex) {
        const std::string what = ex.what();
        EXPECT_NE(what.find("multiple operations produce value"),
                  std::string::npos)
            << "actual: " << what;
        EXPECT_NE(what.find("counter"), std::string::npos)
            << "actual: " << what;
    }
}

TEST(RegionCompilerTest, CompilerLowersBufferBoundaryMappingsInChildDGraph) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphBuffer parentInput = graph.inputBuffer(BufferType::I32, "parent_input", testSize(graph.rootRegion()));
    GraphBuffer parentOutput = GraphBuffer::make(BufferType::I32, "parent_output",
                                                 graph.rootRegion().scopeId(),
                                                 parentInput.sizeScalar());
    auto body = graph.rootRegion().createChild();
    GraphBuffer localInput = body->inputBuffer(BufferType::I32, "input", parentInput.sizeScalar());

    std::string startId = body->importFromParent(
        std::vector<BufferBoundaryMapping>{{parentInput, localInput}});

    IOMap producerIo;
    GraphBuffer localOutput;
    producerIo.bindOutput("out", BufferType::I32, localOutput,
                          parentInput.sizeScalar(), body->scopeId());
    std::string producerId = body->addKernel(cpuKernel("produce", singleOutputType()),
                                             std::move(producerIo), "cpu");
    std::string endId = body->exportToParent(
        std::vector<BufferBoundaryMapping>{{localOutput, parentOutput}}, {producerId});
    std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    const DGraphChild* bodyChild = findChildDGraphs(*cpuDGraph, loopId,
                                                   DGraphChildRole::LoopBody);
    ASSERT_NE(bodyChild, nullptr);
    ASSERT_EQ(bodyChild->dgraphs.size(), 1u);

    const CompiledNode* startNode = findCompiledNode(*bodyChild->dgraphs.front(), startId);
    const CompiledNode* endNode = findCompiledNode(*bodyChild->dgraphs.front(), endId);
    ASSERT_NE(startNode, nullptr);
    ASSERT_NE(endNode, nullptr);

    const auto& compiledStart = std::get<CompiledBoundaryNode>(*startNode);
    ASSERT_EQ(compiledStart.bufferCopies.size(), 1u);
    EXPECT_EQ(compiledStart.bufferCopies.front().sourceName, parentInput.name());
    EXPECT_EQ(compiledStart.bufferCopies.front().sourceScopeId, parentInput.scopeId());
    EXPECT_EQ(compiledStart.bufferCopies.front().targetName, localInput.name());
    EXPECT_EQ(compiledStart.bufferCopies.front().targetScopeId, localInput.scopeId());

    const auto& compiledEnd = std::get<CompiledBoundaryNode>(*endNode);
    ASSERT_EQ(compiledEnd.bufferCopies.size(), 1u);
    EXPECT_EQ(compiledEnd.bufferCopies.front().sourceName, localOutput.name());
    EXPECT_EQ(compiledEnd.bufferCopies.front().sourceScopeId, localOutput.scopeId());
    EXPECT_EQ(compiledEnd.bufferCopies.front().targetName, parentOutput.name());
    EXPECT_EQ(compiledEnd.bufferCopies.front().targetScopeId, parentOutput.scopeId());
}

TEST(RegionCompilerTest, CompilerOrdersBufferBoundaryDependencies) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphBuffer parentState = graph.inputBuffer(BufferType::I32, "state", testSize(graph.rootRegion()));
    auto body = graph.rootRegion().createChild();
    GraphBuffer localState = body->inputBuffer(BufferType::I32, "state", parentState.sizeScalar());

    std::string startId = body->importFromParent(
        std::vector<BufferBoundaryMapping>{{parentState, localState}});

    IOTypeMap kernelType;
    kernelType.inputs.push_back({"in", BufferType::I32});
    kernelType.outputs.push_back({"out", BufferType::I32});
    IOMap kernelIo;
    GraphBuffer localNext;
    kernelIo.bindInput("in", localState)
            .bindOutput("out", BufferType::I32, localNext, body->scopeId());
    std::string kernelId = body->addKernel(cpuKernel("advance", kernelType),
                                           std::move(kernelIo), "cpu");
    std::string endId = body->exportToParent(
        std::vector<BufferBoundaryMapping>{{localNext, parentState}});
    std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    const DGraphChild* bodyChild = findChildDGraphs(*cpuDGraph, loopId,
                                                   DGraphChildRole::LoopBody);
    ASSERT_NE(bodyChild, nullptr);
    const DGraph& bodyDGraph = *bodyChild->dgraphs.front();

    const CompiledNode* kernelNode = findCompiledNode(bodyDGraph, kernelId);
    const CompiledNode* endNode = findCompiledNode(bodyDGraph, endId);
    ASSERT_NE(kernelNode, nullptr);
    ASSERT_NE(endNode, nullptr);
    EXPECT_TRUE(dependsOn(*kernelNode, startId));
    EXPECT_TRUE(dependsOn(*endNode, kernelId));
}

TEST(RegionCompilerTest, CompilerAllowsLoopCarriedBufferWithInitialProducer) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphBuffer raw = graph.inputBuffer(BufferType::I32, "raw", testSize(graph.rootRegion()));
    IOTypeMap kernelType;
    kernelType.inputs.push_back({"in", BufferType::I32});
    kernelType.outputs.push_back({"out", BufferType::I32});

    IOMap initIo;
    GraphBuffer parentState;
    initIo.bindInput("in", raw)
          .bindOutput("out", BufferType::I32, parentState);
    const std::string initId = graph.addNode(cpuKernel("init", kernelType),
                                             std::move(initIo), "cpu");

    auto body = graph.rootRegion().createChild();
    GraphBuffer localState = body->inputBuffer(BufferType::I32, "state", parentState.sizeScalar());
    const std::string startId = body->importFromParent(
        std::vector<BufferBoundaryMapping>{{parentState, localState}});

    IOMap bodyIo;
    GraphBuffer localNext;
    bodyIo.bindInput("in", localState)
          .bindOutput("out", BufferType::I32, localNext, body->scopeId());
    const std::string bodyId = body->addKernel(cpuKernel("advance", kernelType),
                                               std::move(bodyIo), "cpu", {startId});
    body->exportToParent(std::vector<BufferBoundaryMapping>{{localNext, parentState}},
                         {bodyId});

    const std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    IOMap consumeIo;
    GraphBuffer finalOut;
    consumeIo.bindInput("in", parentState)
             .bindOutput("out", BufferType::I32, finalOut);
    const std::string consumeId = graph.addNode(cpuKernel("consume", kernelType),
                                                std::move(consumeIo), "cpu");

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    const CompiledNode* loopNode = findCompiledNode(*cpuDGraph, loopId);
    const CompiledNode* consumeNode = findCompiledNode(*cpuDGraph, consumeId);
    ASSERT_NE(loopNode, nullptr);
    ASSERT_NE(consumeNode, nullptr);

    EXPECT_TRUE(dependsOn(*loopNode, initId));
    EXPECT_TRUE(dependsOn(*consumeNode, loopId));
    EXPECT_FALSE(dependsOn(*consumeNode, initId));
}

TEST(RegionCompilerTest, CompilerRejectsBufferBoundaryTypeMismatch) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphBuffer parentInput = graph.inputBuffer(BufferType::I32, "raw", testSize(graph.rootRegion()));
    auto body = graph.rootRegion().createChild();
    GraphBuffer localInput = body->inputBuffer(BufferType::U32, "raw", parentInput.sizeScalar());

    body->importFromParent(std::vector<BufferBoundaryMapping>{{parentInput, localInput}});
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    EXPECT_THROW(compileForInspection(graph), std::runtime_error);
}

TEST(RegionCompilerTest, CompilerRejectsUnimportedChildBufferInput) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    GraphBuffer localInput = body->inputBuffer(BufferType::I32, "raw", size);

    IOTypeMap kernelType;
    kernelType.inputs.push_back({"in", BufferType::I32});
    IOMap kernelIo;
    kernelIo.bindInput("in", localInput);
    body->addKernel(cpuKernel("consume", kernelType), std::move(kernelIo), "cpu");
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    EXPECT_THROW(compileForInspection(graph), std::runtime_error);
}

TEST(RegionCompilerTest, CompilerRejectsWrongDirectionBufferBoundaryMapping) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphBuffer parentInput = graph.inputBuffer(BufferType::I32, "raw", testSize(graph.rootRegion()));
    auto body = graph.rootRegion().createChild();
    GraphBuffer localInput = body->inputBuffer(BufferType::I32, "raw", parentInput.sizeScalar());

    body->importFromParent(std::vector<BufferBoundaryMapping>{{localInput, parentInput}});
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    EXPECT_THROW(compileForInspection(graph), std::runtime_error);
}

TEST(RegionCompilerTest, CompilerRejectsUnimportedChildScalarInput) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto body = graph.rootRegion().createChild();
    GraphScalar localCounter = body->scalar(ScalarType::I32, "counter");

    IOMap bodyIo;
    bodyIo.bindInputScalar("in", localCounter);
    body->addKernel(cpuKernel("consume_scalar", singleInputScalarType()),
                    std::move(bodyIo), "cpu");
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    EXPECT_THROW(compileForInspection(graph), std::runtime_error);
}

TEST(RegionCompilerTest, CompilerRejectsUnimportedChildConditionScalar) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto body = graph.rootRegion().createChild();
    GraphScalar localCounter = body->scalar(ScalarType::I32, "counter");
    auto nestedBody = body->createChild();

    Condition condition = Condition::compare(
        CompareOp::LT,
        ConditionOperand::scalar(ScalarType::I32, localCounter.varName(), localCounter.scopeId()),
        ConditionOperand::constant<int32_t>(1));
    body->addLoop(whileLoopSpec(std::move(condition), nestedBody));
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    EXPECT_THROW(compileForInspection(graph), std::runtime_error);
}

TEST(RegionCompilerTest, CompilerAcceptsImportedChildConditionScalar) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar parentCounter = graph.globalScalar(ScalarType::I32, "counter");
    auto body = graph.rootRegion().createChild();
    GraphScalar localCounter = body->scalar(ScalarType::I32, "counter");
    body->importFromParent({{parentCounter, localCounter}});
    auto nestedBody = body->createChild();

    Condition condition = Condition::compare(
        CompareOp::LT,
        ConditionOperand::scalar(ScalarType::I32, localCounter.varName(), localCounter.scopeId()),
        ConditionOperand::constant<int32_t>(1));
    body->addLoop(whileLoopSpec(std::move(condition), nestedBody));
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    EXPECT_NO_THROW(compileForInspection(graph));
}

TEST(RegionCompilerTest, CompilerRejectsUnimportedChildScalarTripCount) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto body = graph.rootRegion().createChild();
    GraphScalar localTripCount = body->scalar(ScalarType::I32, "trip_count");
    auto nestedBody = body->createChild();

    body->addLoop(fixedLoopSpec(
        LoopTripCount::scalar(ScalarType::I32,
                              localTripCount.varName(),
                              localTripCount.scopeId()),
        nestedBody));
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    EXPECT_THROW(compileForInspection(graph), std::runtime_error);
}

TEST(RegionCompilerTest, CompilerOrdersConditionalAfterScalarConditionProducer) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar flag = graph.globalScalar(ScalarType::I32, "flag");
    IOMap producerIo;
    producerIo.bindOutputScalar("out", flag);
    std::string producerId = graph.addNode(cpuKernel("produce_flag", singleOutputScalarType()),
                                           std::move(producerIo), "cpu");

    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();
    Condition condition = Condition::compare(
        CompareOp::EQ,
        ConditionOperand::scalar(ScalarType::I32, flag.varName(), flag.scopeId()),
        ConditionOperand::constant<int32_t>(1));
    std::string conditionalId = graph.addConditional(
        ifElseSpec(std::move(condition), thenRegion, elseRegion));

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    const CompiledNode* conditionalNode = findCompiledNode(*cpuDGraph, conditionalId);
    ASSERT_NE(conditionalNode, nullptr);
    EXPECT_TRUE(dependsOn(*conditionalNode, producerId));
}

TEST(RegionCompilerTest, CompilerOrdersLoopAfterScalarTripCountProducer) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar tripCount = graph.globalScalar(ScalarType::I32, "trip_count");
    IOMap producerIo;
    producerIo.bindOutputScalar("out", tripCount);
    std::string producerId = graph.addNode(cpuKernel("produce_trip_count",
                                                     singleOutputScalarType()),
                                           std::move(producerIo), "cpu");

    auto body = graph.rootRegion().createChild();
    std::string loopId = graph.addLoop(fixedLoopSpec(
        LoopTripCount::scalar(ScalarType::I32, tripCount.varName(), tripCount.scopeId()),
        body));

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    const CompiledNode* loopNode = findCompiledNode(*cpuDGraph, loopId);
    ASSERT_NE(loopNode, nullptr);
    EXPECT_TRUE(dependsOn(*loopNode, producerId));
}

TEST(RegionCompilerTest, CompilerInfersLoopOutputPlacementAndParentDependency) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    std::string bodyKernelId = addOutputKernel(
        *body, cpuKernel("body", singleOutputType()), BufferType::I32, "cpu", size);

    IOTypeMap loopType = singleOutputType();
    IOMap loopIo;
    GraphBuffer loopOutput = bindControlOutput(loopIo, graph.rootRegion());
    std::string loopId = graph.addLoop(fixedLoopSpec(
        std::move(loopType), std::move(loopIo),
        tripCount(tripCountScalar(graph.rootRegion())), body));

    IOTypeMap consumerType;
    consumerType.inputs.push_back({"in", BufferType::I32});
    IOMap consumerIo;
    consumerIo.bindInput("in", loopOutput);
    std::string consumerId = graph.addNode(cpuKernel("consume", consumerType),
                                           std::move(consumerIo), "cpu");

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);

    const CompiledNode* loopNode = findCompiledNode(*cpuDGraph, loopId);
    const CompiledNode* consumerNode = findCompiledNode(*cpuDGraph, consumerId);
    ASSERT_NE(loopNode, nullptr);
    ASSERT_NE(consumerNode, nullptr);

    const auto& compiledLoop = std::get<CompiledLoopNode>(*loopNode);
    const std::string scopedLoopOutput = scopedBufferKey(loopOutput.scopeId(),
                                                          loopOutput.name());
    ASSERT_EQ(compiledLoop.outputBufferPlacements.count(scopedLoopOutput), 1u);
    EXPECT_EQ(compiledLoop.outputBufferPlacements.at(scopedLoopOutput), "cpu");
    ASSERT_EQ(compiledLoop.outputBufferPublications.size(), 1u);
    EXPECT_EQ(compiledLoop.outputBufferPublications.front().portName, "out");
    EXPECT_EQ(compiledLoop.outputBufferPublications.front().parentTokenName,
              loopOutput.name());
    EXPECT_FALSE(compiledLoop.outputBufferPublications.front().sourceTokenName.empty());
    EXPECT_EQ(compiledLoop.outputBufferPublications.front().sourceDeviceId, "cpu");
    EXPECT_TRUE(dependsOn(*consumerNode, loopId));
    EXPECT_FALSE(dependsOn(*consumerNode, bodyKernelId));
}

TEST(RegionCompilerTest, CompilerInfersConditionalOutputPlacementWhenBranchesAgree) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar size = testSize(graph.rootRegion());
    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();
    addOutputKernel(*thenRegion, cpuKernel("then", singleOutputType()), BufferType::I32,
                    "cpu", size);
    addOutputKernel(*elseRegion, cpuKernel("else", singleOutputType()), BufferType::I32,
                    "cpu", size);

    IOTypeMap conditionalType = singleOutputType();
    IOMap conditionalIo;
    GraphBuffer conditionalOutput = bindControlOutput(conditionalIo, graph.rootRegion());
    std::string conditionalId = graph.addConditional(
        ifElseSpec(std::move(conditionalType), std::move(conditionalIo),
                   Condition::alwaysTrue(), thenRegion, elseRegion));

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);

    const CompiledNode* conditionalNode = findCompiledNode(*cpuDGraph, conditionalId);
    ASSERT_NE(conditionalNode, nullptr);
    const auto& compiledConditional = std::get<CompiledConditionalNode>(*conditionalNode);
    const std::string scopedCondOutput = scopedBufferKey(conditionalOutput.scopeId(),
                                                          conditionalOutput.name());
    ASSERT_EQ(compiledConditional.outputBufferPlacements.count(scopedCondOutput), 1u);
    EXPECT_EQ(compiledConditional.outputBufferPlacements.at(scopedCondOutput), "cpu");
    ASSERT_EQ(compiledConditional.outputBufferPublications.size(), 1u);
    EXPECT_EQ(compiledConditional.outputBufferPublications.front().portName, "out");
    EXPECT_EQ(compiledConditional.outputBufferPublications.front().parentTokenName,
              conditionalOutput.name());
    EXPECT_FALSE(compiledConditional.outputBufferPublications.front().thenSourceTokenName.empty());
    EXPECT_EQ(compiledConditional.outputBufferPublications.front().thenSourceDeviceId, "cpu");
    EXPECT_FALSE(compiledConditional.outputBufferPublications.front().elseSourceTokenName.empty());
    EXPECT_EQ(compiledConditional.outputBufferPublications.front().elseSourceDeviceId, "cpu");
}

TEST(RegionCompilerTest, CompilerRejectsConditionalMissingBranchOutput) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();
    addOutputKernel(*thenRegion, cpuKernel("then", singleOutputType()), BufferType::I32,
                    "cpu");

    IOTypeMap conditionalType = singleOutputType();
    IOMap conditionalIo;
    bindControlOutput(conditionalIo, graph.rootRegion());
    graph.addConditional(ifElseSpec(std::move(conditionalType), std::move(conditionalIo),
                                    Condition::alwaysTrue(), thenRegion, elseRegion));

    EXPECT_THROW(compileForInspection(graph), std::runtime_error);
}

TEST(RegionCompilerTest, CompilerRejectsConditionalBranchOutputTypeMismatch) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();
    addOutputKernel(*thenRegion, cpuKernel("then", singleOutputType(BufferType::I32)),
                    BufferType::I32, "cpu");
    addOutputKernel(*elseRegion, cpuKernel("else", singleOutputType(BufferType::F32)),
                    BufferType::F32, "cpu");

    IOTypeMap conditionalType = singleOutputType(BufferType::I32);
    IOMap conditionalIo;
    bindControlOutput(conditionalIo, graph.rootRegion(), BufferType::I32);
    graph.addConditional(ifElseSpec(std::move(conditionalType), std::move(conditionalIo),
                                    Condition::alwaysTrue(), thenRegion, elseRegion));

    EXPECT_THROW(compileForInspection(graph), std::runtime_error);
}

TEST(RegionCompilerTest, CompilerRejectsMixedBranchOutputPlacementWithoutHint) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));
    graph.registerDevice(std::make_shared<StubDevice>("mock", DeviceType::MOCK_CPU));

    GraphScalar size = testSize(graph.rootRegion());
    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();
    addOutputKernel(*thenRegion, cpuKernel("then", singleOutputType()), BufferType::I32,
                    "cpu", size);
    addOutputKernel(*elseRegion, mockCpuKernel("else", singleOutputType()), BufferType::I32,
                    "mock", size);

    IOTypeMap conditionalType = singleOutputType();
    IOMap conditionalIo;
    bindControlOutput(conditionalIo, graph.rootRegion());
    graph.addConditional(ifElseSpec(std::move(conditionalType), std::move(conditionalIo),
                                    Condition::alwaysTrue(), thenRegion, elseRegion));

    EXPECT_THROW(compileForInspection(graph), std::runtime_error);
}

TEST(RegionCompilerTest, CompilerAcceptsExplicitMixedBranchOutputPlacement) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));
    graph.registerDevice(std::make_shared<StubDevice>("mock", DeviceType::MOCK_CPU));

    GraphScalar size = testSize(graph.rootRegion());
    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();
    addOutputKernel(*thenRegion, cpuKernel("then", singleOutputType()), BufferType::I32,
                    "cpu", size);
    addOutputKernel(*elseRegion, mockCpuKernel("else", singleOutputType()), BufferType::I32,
                    "mock", size);

    ConditionalSpec spec;
    spec.ioType = singleOutputType();
    GraphBuffer conditionalOutput = bindControlOutput(spec.ioMap, graph.rootRegion());
    spec.condition = Condition::alwaysTrue();
    spec.thenRegion = thenRegion;
    spec.elseRegion = elseRegion;
    spec.outputPlacement.buffers["out"] = "cpu";
    std::string conditionalId = graph.addConditional(std::move(spec));

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);

    const CompiledNode* conditionalNode = findCompiledNode(*cpuDGraph, conditionalId);
    ASSERT_NE(conditionalNode, nullptr);
    const auto& compiledConditional = std::get<CompiledConditionalNode>(*conditionalNode);
    const std::string scopedCondOutput = scopedBufferKey(conditionalOutput.scopeId(),
                                                          conditionalOutput.name());
    ASSERT_EQ(compiledConditional.outputBufferPlacements.count(scopedCondOutput), 1u);
    EXPECT_EQ(compiledConditional.outputBufferPlacements.at(scopedCondOutput), "cpu");
}

TEST(RegionCompilerTest, CompilerBuildsOutputPlacementBridgeForLoopBodyBuffer) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("mcpu:0", DeviceType::MOCK_CPU));

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    IOMap producerIo;
    GraphBuffer bodyOutput;
    producerIo.bindOutput("out", BufferType::I32, bodyOutput, size, body->scopeId());
    std::string mockProducerId = body->addKernel(
        mockCpuKernel("remote_output", singleOutputType()), std::move(producerIo), "mcpu:0");

    LoopSpec spec;
    spec.ioType = singleOutputType();
    GraphBuffer loopOutput = bindControlOutput(spec.ioMap, graph.rootRegion());
    spec.tripCount = tripCount(tripCountScalar(graph.rootRegion()));
    spec.body = body;
    spec.outputPlacement.buffers["out"] = "cpu";
    std::string loopId = graph.addLoop(std::move(spec));

    IOTypeMap consumerType;
    consumerType.inputs.push_back({"in", BufferType::I32});
    IOMap consumerIo;
    consumerIo.bindInput("in", loopOutput);
    std::string consumerId = graph.addNode(cpuKernel("consume", consumerType),
                                           std::move(consumerIo), "cpu");

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);

    const CompiledNode* loopNode = findCompiledNode(*cpuDGraph, loopId);
    const CompiledNode* consumerNode = findCompiledNode(*cpuDGraph, consumerId);
    ASSERT_NE(loopNode, nullptr);
    ASSERT_NE(consumerNode, nullptr);

    const auto& compiledLoop = std::get<CompiledLoopNode>(*loopNode);
    const std::string scopedLoopOutput = scopedBufferKey(loopOutput.scopeId(),
                                                          loopOutput.name());
    ASSERT_EQ(compiledLoop.outputBufferPlacements.count(scopedLoopOutput), 1u);
    EXPECT_EQ(compiledLoop.outputBufferPlacements.at(scopedLoopOutput), "cpu");
    ASSERT_EQ(compiledLoop.outputBufferPublications.size(), 1u);
    EXPECT_EQ(compiledLoop.outputBufferPublications.front().sourceTokenName,
              bodyOutput.name());
    EXPECT_EQ(compiledLoop.outputBufferPublications.front().sourceScopeId,
              bodyOutput.scopeId());
    EXPECT_EQ(compiledLoop.outputBufferPublications.front().sourceDeviceId, "cpu");
    EXPECT_TRUE(dependsOn(*consumerNode, loopId));
    EXPECT_FALSE(dependsOn(*consumerNode, mockProducerId));

    const DGraphChild* bodyChild = findChildDGraphs(*cpuDGraph, loopId,
                                                   DGraphChildRole::LoopBody);
    ASSERT_NE(bodyChild, nullptr);
    const DGraph* cpuChild = findChildDGraph(*bodyChild, "cpu");
    const DGraph* mockChild = findChildDGraph(*bodyChild, "mcpu:0");
    ASSERT_NE(cpuChild, nullptr);
    ASSERT_NE(mockChild, nullptr);

    const auto* producerBridge = findBridgeNode(
        *mockChild, CompiledBridgeOpNode::Side::Producer, mockProducerId);
    const auto* consumerBridge = findBridgeNode(
        *cpuChild, CompiledBridgeOpNode::Side::Consumer, loopId);
    ASSERT_NE(producerBridge, nullptr);
    ASSERT_NE(consumerBridge, nullptr);
    EXPECT_TRUE(dependsOn(*producerBridge, mockProducerId));
}

TEST(RegionCompilerTest, CompilerBuildsOutputPlacementBridgeForConditionalBranchBuffer) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("mcpu:0", DeviceType::MOCK_CPU));

    GraphScalar size = testSize(graph.rootRegion());
    auto thenRegion = graph.rootRegion().createChild();
    IOMap thenIo;
    GraphBuffer thenOutput;
    thenIo.bindOutput("out", BufferType::I32, thenOutput, size, thenRegion->scopeId());
    thenRegion->addKernel(cpuKernel("then_output", singleOutputType()),
                          std::move(thenIo), "cpu");

    auto elseRegion = graph.rootRegion().createChild();
    IOMap elseIo;
    GraphBuffer elseOutput;
    elseIo.bindOutput("out", BufferType::I32, elseOutput, size, elseRegion->scopeId());
    std::string elseProducerId = elseRegion->addKernel(
        mockCpuKernel("else_remote_output", singleOutputType()), std::move(elseIo), "mcpu:0");

    ConditionalSpec spec;
    spec.ioType = singleOutputType();
    GraphBuffer conditionalOutput = bindControlOutput(spec.ioMap, graph.rootRegion());
    spec.condition = Condition::alwaysTrue();
    spec.thenRegion = thenRegion;
    spec.elseRegion = elseRegion;
    spec.outputPlacement.buffers["out"] = "cpu";
    std::string conditionalId = graph.addConditional(std::move(spec));

    IOTypeMap consumerType;
    consumerType.inputs.push_back({"in", BufferType::I32});
    IOMap consumerIo;
    consumerIo.bindInput("in", conditionalOutput);
    std::string consumerId = graph.addNode(cpuKernel("consume", consumerType),
                                           std::move(consumerIo), "cpu");

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);

    const CompiledNode* conditionalNode = findCompiledNode(*cpuDGraph, conditionalId);
    const CompiledNode* consumerNode = findCompiledNode(*cpuDGraph, consumerId);
    ASSERT_NE(conditionalNode, nullptr);
    ASSERT_NE(consumerNode, nullptr);

    const auto& compiledConditional = std::get<CompiledConditionalNode>(*conditionalNode);
    const std::string scopedCondOutput = scopedBufferKey(conditionalOutput.scopeId(),
                                                          conditionalOutput.name());
    ASSERT_EQ(compiledConditional.outputBufferPlacements.count(scopedCondOutput), 1u);
    EXPECT_EQ(compiledConditional.outputBufferPlacements.at(scopedCondOutput), "cpu");
    ASSERT_EQ(compiledConditional.outputBufferPublications.size(), 1u);
    EXPECT_EQ(compiledConditional.outputBufferPublications.front().thenSourceDeviceId, "cpu");
    EXPECT_EQ(compiledConditional.outputBufferPublications.front().elseSourceTokenName,
              elseOutput.name());
    EXPECT_EQ(compiledConditional.outputBufferPublications.front().elseSourceScopeId,
              elseOutput.scopeId());
    EXPECT_EQ(compiledConditional.outputBufferPublications.front().elseSourceDeviceId, "cpu");
    EXPECT_TRUE(dependsOn(*consumerNode, conditionalId));
    EXPECT_FALSE(dependsOn(*consumerNode, elseProducerId));

    const DGraphChild* thenChild = findChildDGraphs(
        *cpuDGraph, conditionalId, DGraphChildRole::ConditionalThen);
    const DGraphChild* elseChild = findChildDGraphs(
        *cpuDGraph, conditionalId, DGraphChildRole::ConditionalElse);
    ASSERT_NE(thenChild, nullptr);
    ASSERT_NE(elseChild, nullptr);
    const DGraph* cpuThenChild = findChildDGraph(*thenChild, "cpu");
    const DGraph* cpuElseChild = findChildDGraph(*elseChild, "cpu");
    const DGraph* mockElseChild = findChildDGraph(*elseChild, "mcpu:0");
    ASSERT_NE(cpuThenChild, nullptr);
    ASSERT_NE(cpuElseChild, nullptr);
    ASSERT_NE(mockElseChild, nullptr);

    EXPECT_EQ(findBridgeNode(*cpuThenChild, CompiledBridgeOpNode::Side::Consumer,
                             conditionalId), nullptr);
    const auto* producerBridge = findBridgeNode(
        *mockElseChild, CompiledBridgeOpNode::Side::Producer, elseProducerId);
    const auto* consumerBridge = findBridgeNode(
        *cpuElseChild, CompiledBridgeOpNode::Side::Consumer, conditionalId);
    ASSERT_NE(producerBridge, nullptr);
    ASSERT_NE(consumerBridge, nullptr);
    EXPECT_TRUE(dependsOn(*producerBridge, elseProducerId));
}

// A loop publishes a buffer at its CPU placement device; a parent kernel on
// `mcpu:0` reads that published buffer. The compiler must route a `cpu ->
// mcpu:0` bridge whose producer-side hangs after the control op in the CPU
// DGraph and whose consumer-side hangs before the kernel in the MOCK_CPU
// DGraph. The previous kernel-only producer check rejected this with a
// generic "not implemented yet" error.
TEST(RegionCompilerTest, ParentKernelReadsControlOutputAcrossDevices) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("mcpu:0", DeviceType::MOCK_CPU));

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    addOutputKernel(*body, cpuKernel("body", singleOutputType()), BufferType::I32, "cpu", size);

    LoopSpec loopSpec;
    loopSpec.ioType = singleOutputType();
    GraphBuffer loopOutput = bindControlOutput(loopSpec.ioMap, graph.rootRegion());
    loopSpec.tripCount = tripCount(tripCountScalar(graph.rootRegion()));
    loopSpec.body = body;
    std::string loopId = graph.addLoop(std::move(loopSpec));

    IOTypeMap consumerType;
    consumerType.inputs.push_back({"in", BufferType::I32});
    IOMap consumerIo;
    consumerIo.bindInput("in", loopOutput);
    std::string consumerId = graph.addNode(mockCpuKernel("remote_consume", consumerType),
                                           std::move(consumerIo), "mcpu:0");

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);

    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    const DGraph* mockDGraph = findDGraph(dgraphs, "mcpu:0");
    ASSERT_NE(cpuDGraph, nullptr);
    ASSERT_NE(mockDGraph, nullptr);

    const auto* producerBridge = findBridgeNode(
        *cpuDGraph, CompiledBridgeOpNode::Side::Producer, loopId);
    const auto* consumerBridge = findBridgeNode(
        *mockDGraph, CompiledBridgeOpNode::Side::Consumer, consumerId);
    ASSERT_NE(producerBridge, nullptr)
        << "no producer-side bridge after control op '" << loopId << "' on cpu";
    ASSERT_NE(consumerBridge, nullptr)
        << "no consumer-side bridge before kernel '" << consumerId << "' on mcpu:0";
    EXPECT_TRUE(dependsOn(*producerBridge, loopId));

    const CompiledNode* consumerNode = findCompiledNode(*mockDGraph, consumerId);
    ASSERT_NE(consumerNode, nullptr);
    EXPECT_TRUE(dependsOn(*consumerNode, consumerBridge->id));
}

TEST(RegionCompilerTest, FpgaLoopOutputFeedsCpuConditionalThroughBridge) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    auto fpga = std::make_shared<RegionAwareStubDevice>("fpga:0", DeviceType::FPGA);
    fpga->setRegion("produce", "in", "HBM0");
    fpga->setRegion("produce", "out", "HBM0");
    graph.registerDevice(fpga);

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    GraphBuffer rootInput = graph.rootRegion().inputBuffer(BufferType::I32, "input", size);
    GraphBuffer localInput = body->inputBuffer(BufferType::I32, "state", size);
    body->importFromParent({{rootInput, localInput}});

    IOTypeMap producerType = singleInputOutputType();
    IOMap producerIo;
    GraphBuffer localOutput;
    producerIo.bindInput("in", localInput)
              .bindOutput("out", BufferType::I32, localOutput, size, body->scopeId());
    const std::string producerId = body->addKernel(
        KernelDescriptor{"produce", DeviceType::FPGA, std::nullopt, producerType},
        std::move(producerIo), "fpga:0");

    GraphBuffer loopOutput = GraphBuffer::make(
        BufferType::I32, "loop_output", graph.rootRegion().scopeId(), size);
    body->exportToParent({{localOutput, loopOutput}, {localOutput, rootInput}}, {producerId});
    const std::string loopId = graph.addLoop(fixedLoopSpec(
        tripCount(tripCountScalar(graph.rootRegion())), body));

    GraphScalar brightness = graph.rootRegion().scalar(ScalarType::I32, "brightness");
    IOTypeMap levelType;
    levelType.inputs.push_back({"input", BufferType::I32});
    levelType.outputScalars.push_back({"level", ScalarType::I32});
    IOMap levelIo;
    levelIo.bindInput("input", rootInput)
           .bindOutputScalar("level", brightness);
    const std::string levelId =
        graph.addNode(cpuKernel("level", levelType), std::move(levelIo), "cpu");

    IOTypeMap consumerType;
    consumerType.inputs.push_back({"in", BufferType::I32});
    auto makeBranch = [&](const std::string& name) {
        auto branch = graph.rootRegion().createChild();
        GraphBuffer localInput = branch->inputBuffer(
            BufferType::I32, name + "_input", size);
        branch->importFromParent({{loopOutput, localInput}});
        IOMap io;
        io.bindInput("in", localInput);
        branch->addKernel(cpuKernel(name, consumerType), std::move(io), "cpu");
        return branch;
    };
    auto thenRegion = makeBranch("then");
    auto elseRegion = makeBranch("else");

    const std::string conditionalId = graph.addConditional(ifElseSpec(
        brightness >= 0,
        thenRegion, elseRegion));

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);
    const DGraph* fpgaDGraph = findDGraph(dgraphs, "fpga:0");
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(fpgaDGraph, nullptr);
    ASSERT_NE(cpuDGraph, nullptr);
    ASSERT_NE(findCompiledNode(*fpgaDGraph, loopId), nullptr);
    EXPECT_EQ(findCompiledNode(*cpuDGraph, loopId), nullptr)
        << "all-FPGA loop should stay autonomous, not split onto the CPU queue";
    const CompiledNode* levelNode = findCompiledNode(*cpuDGraph, levelId);
    ASSERT_NE(levelNode, nullptr);
    EXPECT_TRUE(dependsOn(*levelNode, "__graph_start"));
    EXPECT_EQ(findBridgeNode(*cpuDGraph, CompiledBridgeOpNode::Side::Producer, levelId),
              nullptr)
        << "parallel CPU reader must keep using the original graph input";

    const auto* transfer = findBridgeNode(
        *cpuDGraph, CompiledBridgeOpNode::Side::Consumer, conditionalId);
    ASSERT_NE(transfer, nullptr);

    const CompiledNode* conditionalNode =
        findCompiledNode(*cpuDGraph, conditionalId);
    ASSERT_NE(conditionalNode, nullptr);
    bool waitsForTransfer = dependsOn(*conditionalNode, transfer->id);
    for (const std::string& dependency :
         compiledNodeDependsOn(*conditionalNode)) {
        waitsForTransfer |= dependency.rfind("__event_", 0) == 0;
    }
    EXPECT_TRUE(waitsForTransfer);
}

TEST(RegionCompilerTest, GraphRunExecutesEmptyStructuredControlOnCpu) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto body = graph.rootRegion().createChild();
    GraphScalar loopCount = tripCountScalar(graph.rootRegion());
    graph.addLoop(fixedLoopSpec(tripCount(loopCount), body));

    auto exec = graph.compile();
    exec.writeScalar(loopCount, 1);
    EXPECT_NO_THROW(exec.run());
}

TEST(RegionCompilerTest, GraphRunCarriesLoopBufferStateAcrossIterations) {
    Graph graph;
    auto cpu = std::make_shared<CpuDevice>("cpu");
    graph.registerDevice(cpu);

    auto initKernel = std::make_shared<AddI32BufferKernel>("init", 10);
    auto advanceKernel = std::make_shared<AddI32BufferKernel>("advance", 1);
    auto reportKernel = std::make_shared<AddI32BufferKernel>("report", 100);
    cpu->registerKernel(initKernel);
    cpu->registerKernel(advanceKernel);
    cpu->registerKernel(reportKernel);

    GraphBuffer raw = graph.inputBuffer(BufferType::I32, "raw", testSize(graph.rootRegion()));

    IOMap initIo;
    GraphBuffer state;
    initIo.bindInput("in", raw)
          .bindOutput("out", BufferType::I32, state);
    graph.addNode(initKernel->descriptor(), std::move(initIo), "cpu");

    auto body = graph.rootRegion().createChild();
    GraphBuffer localState = body->inputBuffer(BufferType::I32, "state", state.sizeScalar());
    const std::string startId = body->importFromParent(
        std::vector<BufferBoundaryMapping>{{state, localState}});

    IOMap advanceIo;
    GraphBuffer localNext;
    advanceIo.bindInput("in", localState)
             .bindOutput("out", BufferType::I32, localNext, body->scopeId());
    const std::string advanceId = body->addKernel(advanceKernel->descriptor(),
                                                  std::move(advanceIo), "cpu", {startId});
    body->exportToParent(std::vector<BufferBoundaryMapping>{{localNext, state}},
                         {advanceId});

    GraphScalar loopCount = tripCountScalar(graph.rootRegion());
    const std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(loopCount), body));

    IOMap reportIo;
    GraphBuffer finalOut;
    reportIo.bindInput("in", state)
            .bindOutput("out", BufferType::I32, finalOut);
    graph.addNode(reportKernel->descriptor(), std::move(reportIo), "cpu", {loopId});

    const std::vector<std::int32_t> input = {0, 1, 2, 3};
    cpu->setInputBuffer(raw.name(), input.data(), input.size() * sizeof(input[0]));

    auto exec = graph.compile();
    exec.writeScalar(raw.sizeScalar(), static_cast<std::uint64_t>(input.size()));
    exec.writeScalar(loopCount, 3);
    ASSERT_NO_THROW(exec.run());

    std::vector<std::int32_t> output(input.size(), 0);
    cpu->getOutputBuffer(finalOut.name(), output.data(), output.size() * sizeof(output[0]));
    EXPECT_EQ(output, (std::vector<std::int32_t>{113, 114, 115, 116}));
}

TEST(RegionCompilerTest, CompilerRejectsDirectParentTokenUseInsideNestedRegion) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto body = graph.rootRegion().createChild();
    GraphBuffer rootInput = graph.inputBuffer(BufferType::I32, "raw", testSize(graph.rootRegion()));

    IOTypeMap kernelType;
    kernelType.inputs.push_back({"in", BufferType::I32});
    kernelType.outputs.push_back({"out", BufferType::I32});

    IOMap bodyIo;
    GraphBuffer bodyOutput;
    bodyIo.bindInput("in", rootInput)
          .bindOutput("out", BufferType::I32, bodyOutput, body->scopeId());

    KernelDescriptor kernel{"copy", DeviceType::CPU, std::nullopt, kernelType};
    body->addKernel(std::move(kernel), std::move(bodyIo), "cpu");
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    EXPECT_THROW(graph.compile(), std::runtime_error);
}

TEST(RegionCompilerTest, GraphValidationRejectsUndeclaredRootScalarInCondition) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();
    Condition condition = Condition::compare(
        CompareOp::EQ,
        ConditionOperand::scalar(ScalarType::I32, "missing", graph.rootRegion().scopeId()),
        ConditionOperand::constant<int32_t>(1));
    graph.addConditional(ifElseSpec(std::move(condition), thenRegion, elseRegion));

    EXPECT_THROW(graph.compile(), std::runtime_error);
}

TEST(RegionCompilerTest, GraphValidationRejectsUndeclaredRootScalarInBoundaryMapping) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto body = graph.rootRegion().createChild();
    GraphScalar missingParent = GraphScalar::ref(ScalarType::I32, "missing",
                                                       graph.rootRegion().scopeId());
    GraphScalar localCounter = body->scalar(ScalarType::I32, "counter");
    body->importFromParent({{missingParent, localCounter}});
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    EXPECT_THROW(graph.compile(), std::runtime_error);
}

TEST(RegionCompilerTest, ControlNodeRoutedToCpuDeviceWhenBodyIsRemote) {
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("cpu", DeviceType::CPU));
    graph.registerDevice(std::make_shared<StubDevice>("mcpu:0", DeviceType::MOCK_CPU));

    auto body = graph.rootRegion().createChild();
    std::string mockKernelId = body->addKernel(mockCpuKernel("body_remote"), IOMap{}, "mcpu:0");
    std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    InspectionBridge bridge;
    auto dgraphs = compileForInspection(graph, bridge);

    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);
    const CompiledNode* loopNode = findCompiledNode(*cpuDGraph, loopId);
    ASSERT_NE(loopNode, nullptr);
    ASSERT_TRUE(std::holds_alternative<CompiledLoopNode>(*loopNode));
    EXPECT_EQ(std::get<CompiledLoopNode>(*loopNode).deviceId, "cpu");

    const DGraphChild* bodyChild = findChildDGraphs(*cpuDGraph, loopId,
                                                   DGraphChildRole::LoopBody);
    ASSERT_NE(bodyChild, nullptr);
    const DGraph* mockChild = findChildDGraph(*bodyChild, "mcpu:0");
    ASSERT_NE(mockChild, nullptr);
    EXPECT_NE(findCompiledNode(*mockChild, mockKernelId), nullptr);

    const DGraph* mockTopDGraph = findDGraph(dgraphs, "mcpu:0");
    if (mockTopDGraph) {
        EXPECT_EQ(findCompiledNode(*mockTopDGraph, loopId), nullptr);
    }
}

TEST(RegionCompilerTest, ControlNodeWithoutCpuDeviceFails) {
    // A graph with only a non-CPU device cannot compile: the bridge-factory
    // check fires because every non-CPU device requires {CPU, T}/{T, CPU}
    // factories, and those factories cannot exist without a CPU device.
    // This subsumes the original "control node requires a CPU device" error.
    Graph graph;
    graph.registerDevice(std::make_shared<StubDevice>("mcpu:0", DeviceType::MOCK_CPU));

    auto body = graph.rootRegion().createChild();
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    try {
        compileForInspection(graph);
        FAIL() << "expected compileRegion to throw because no CPU device is registered";
    } catch (const std::runtime_error& ex) {
        EXPECT_NE(std::string(ex.what()).find("fallback control device"),
                  std::string::npos)
            << "actual: " << ex.what();
    }
}

TEST(RegionCompilerTest, GraphValidationRejectsUndeclaredRootInputBuffer) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    IOTypeMap kernelType;
    kernelType.inputs.push_back({"in", BufferType::I32});

    GraphBuffer undeclared = GraphBuffer::make(BufferType::I32, "missing_input",
                                               graph.rootRegion().scopeId());
    IOMap io;
    io.bindInput("in", undeclared);
    graph.addNode(cpuKernel("consume", kernelType), std::move(io), "cpu");

    EXPECT_THROW(graph.compile(), std::runtime_error);
}

TEST(RegionCompilerTest, CompilerAllowsScalarTripCountWithOutputs) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar size = testSize(graph.rootRegion());
    auto body = graph.rootRegion().createChild();
    addOutputKernel(*body, cpuKernel("body_out", singleOutputType()), BufferType::I32, "cpu", size);

    IOTypeMap loopType = singleOutputType();
    IOMap loopIo;
    bindControlOutput(loopIo, graph.rootRegion());
    GraphScalar loopCount = tripCountScalar(graph.rootRegion());
    graph.addLoop(fixedLoopSpec(std::move(loopType), std::move(loopIo),
                                tripCount(loopCount), body));

    EXPECT_NO_THROW((void)compileForInspection(graph));
}

TEST(RegionCompilerTest, CompilerLowersNestedLoops) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto outerBody = graph.rootRegion().createChild();
    auto innerBody = outerBody->createChild();
    std::string innerKernelId = innerBody->addKernel(cpuKernel("inner"), IOMap{}, "cpu");
    GraphScalar rootInnerCount = tripCountScalar(graph.rootRegion());
    GraphScalar innerCount = outerBody->scalar(rootInnerCount.type(), "inner_count");
    outerBody->importFromParent(std::vector<ScalarBoundaryMapping>{{rootInnerCount, innerCount}});
    std::string innerLoopId = outerBody->addLoop(
        fixedLoopSpec(tripCount(innerCount), innerBody));
    GraphScalar outerCount = tripCountScalar(graph.rootRegion());
    std::string outerLoopId = graph.addLoop(
        fixedLoopSpec(tripCount(outerCount), outerBody));

    auto dgraphs = compileForInspection(graph);
    const DGraph* cpuDGraph = findDGraph(dgraphs, "cpu");
    ASSERT_NE(cpuDGraph, nullptr);

    const CompiledNode* outerLoopNode = findCompiledNode(*cpuDGraph, outerLoopId);
    ASSERT_NE(outerLoopNode, nullptr);
    ASSERT_TRUE(std::holds_alternative<CompiledLoopNode>(*outerLoopNode));

    const DGraphChild* outerBodyChild = findChildDGraphs(*cpuDGraph, outerLoopId,
                                                         DGraphChildRole::LoopBody);
    ASSERT_NE(outerBodyChild, nullptr);
    ASSERT_EQ(outerBodyChild->dgraphs.size(), 1u);
    const DGraph* outerBodyDGraph = outerBodyChild->dgraphs.front().get();
    ASSERT_NE(outerBodyDGraph, nullptr);

    const CompiledNode* innerLoopNode = findCompiledNode(*outerBodyDGraph, innerLoopId);
    ASSERT_NE(innerLoopNode, nullptr);
    ASSERT_TRUE(std::holds_alternative<CompiledLoopNode>(*innerLoopNode));

    const DGraphChild* innerBodyChild = findChildDGraphs(*outerBodyDGraph, innerLoopId,
                                                         DGraphChildRole::LoopBody);
    ASSERT_NE(innerBodyChild, nullptr);
    ASSERT_EQ(innerBodyChild->dgraphs.size(), 1u);
    const DGraph* innerBodyDGraph = innerBodyChild->dgraphs.front().get();
    ASSERT_NE(innerBodyDGraph, nullptr);
    EXPECT_NE(findCompiledNode(*innerBodyDGraph, innerKernelId), nullptr);
}

TEST(RegionCompilerTest, GraphRunExecutesEmptyWhileLoopOnCpu) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto body = graph.rootRegion().createChild();
    graph.addLoop(whileLoopSpec(Condition::alwaysFalse(), body));

    EXPECT_NO_THROW(graph.compile().run());
}

TEST(RegionCompilerTest, CompiledGraphSurvivesGraphStructuralMutation) {
    Graph graph;
    auto cpu = std::make_shared<CpuDevice>("cpu");
    graph.registerDevice(cpu);
    auto addOne = std::make_shared<AddI32BufferKernel>("snapshot_add_one", 1);
    auto addTen = std::make_shared<AddI32BufferKernel>("snapshot_add_ten", 10);
    cpu->registerKernel(addOne);
    cpu->registerKernel(addTen);

    GraphBuffer raw = graph.inputBuffer(BufferType::I32, "snapshot_raw", testSize(graph.rootRegion()));
    IOMap firstIo;
    GraphBuffer firstOut;
    firstIo.bindInput("in", raw)
           .bindOutput("out", BufferType::I32, firstOut);
    graph.addNode(addOne->descriptor(), std::move(firstIo), "cpu");

    std::vector<std::int32_t> input = {1, 2};
    cpu->setInputBuffer(raw.name(), input.data(), input.size() * sizeof(input[0]));
    auto oldSnapshot = graph.compile();
    oldSnapshot.writeScalar(raw.sizeScalar(), static_cast<std::uint64_t>(input.size()));
    EXPECT_NO_THROW(oldSnapshot.run());

    IOMap secondIo;
    GraphBuffer secondOut;
    secondIo.bindInput("in", firstOut)
            .bindOutput("out", BufferType::I32, secondOut);
    graph.addNode(addTen->descriptor(), std::move(secondIo), "cpu");

    EXPECT_NO_THROW(oldSnapshot.run());
    std::vector<std::int32_t> oldOutput(input.size(), 0);
    cpu->getOutputBuffer(firstOut.name(), oldOutput.data(), oldOutput.size() * sizeof(oldOutput[0]));
    EXPECT_EQ(oldOutput, (std::vector<std::int32_t>{2, 3}));

    auto newSnapshot = graph.compile();
    newSnapshot.writeScalar(raw.sizeScalar(), static_cast<std::uint64_t>(input.size()));
    EXPECT_NO_THROW(newSnapshot.run());
    std::vector<std::int32_t> newOutput(input.size(), 0);
    cpu->getOutputBuffer(secondOut.name(), newOutput.data(), newOutput.size() * sizeof(newOutput[0]));
    EXPECT_EQ(newOutput, (std::vector<std::int32_t>{12, 13}));
}

TEST(RegionCompilerTest, GraphRegisterDeviceRejectsSecondCpu) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    EXPECT_THROW(
        graph.registerDevice(std::make_shared<CpuDevice>("cpu_2")),
        std::invalid_argument);

    Graph withDefaults = Graph::withDefaults();
    EXPECT_THROW(
        withDefaults.registerDevice(std::make_shared<CpuDevice>("another_cpu")),
        std::invalid_argument);
}

TEST(RegionCompilerTest, AddConditionalSpecWithoutConditionThrows) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();

    ConditionalSpec spec;
    spec.thenRegion = thenRegion;
    spec.elseRegion = elseRegion;

    EXPECT_THROW(graph.addConditional(std::move(spec)), std::invalid_argument);
}

TEST(RegionCompilerTest, CompileRejectsMissingBridgeFactoryForNonCpuDevice) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));
    graph.registerDevice(std::make_shared<StubDevice>("mcpu:0", DeviceType::MOCK_CPU));

    auto body = graph.rootRegion().createChild();
    graph.addLoop(whileLoopSpec(Condition::alwaysFalse(), body));

    try {
        (void)graph.compile();
        FAIL() << "expected compile() to throw because the MOCK_CPU device has no bridge factories";
    } catch (const std::runtime_error& ex) {
        const std::string what = ex.what();
        EXPECT_NE(what.find("bridge factories"), std::string::npos) << "actual: " << what;
        EXPECT_NE(what.find("mcpu:0"), std::string::npos) << "actual: " << what;
    }
}

TEST(RegionCompilerTest, ScopedBufferKeyAlwaysIncludesScopePrefix) {
    EXPECT_EQ(scopedBufferKey(0, "x"), "scope:0:x");
    EXPECT_EQ(scopedBufferKey(1, "x"), "scope:1:x");
    EXPECT_EQ(scopedBufferKey(42, "buffer_anonymous"), "scope:42:buffer_anonymous");
}
