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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/crossdevice/bridge_op.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/graph.hpp>
#include <vrt/graph/node/io_map.hpp>
#include <vrt/graph/node/io_type_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>
#include <vrt/graph/node/compiled_node.hpp>
#include <vrt/graph/render/dot.hpp>

#include "test_support/control_specs.hpp"

using namespace vrt::graph;
using namespace vrt::graph::test_support;

namespace {

class CopyKernel : public CpuKernel {
   public:
    CopyKernel(std::string name, IOTypeMap ioType)
        : CpuKernel(std::move(name)), ioType_(std::move(ioType)) {}

    IOTypeMap ioTypeMap() const override { return ioType_; }

    void run(Args& args) override {
        const auto& in  = args.buffer("in");
        const auto& out = args.buffer("out");
        auto bytes      = std::min(in.sizeBytes, out.sizeBytes);
        std::memcpy(out.data, in.data, bytes);
    }

   private:
    IOTypeMap   ioType_;
};

// Build a 3-node chain on a single CpuDevice and return the Graph.
// Node IDs (auto): kA_0, kB_1, kC_2
struct ChainGraph {
    Graph                       g;
    std::shared_ptr<CpuDevice>  cpu;
    GraphScalar                 size = GraphScalar::ref(ScalarType::U64, "__unset_size");
    std::string                 nodeA, nodeB, nodeC;
};

ChainGraph buildChain() {
    ChainGraph c;
    c.cpu = std::make_shared<CpuDevice>("cpu");

    IOTypeMap io;
    io.inputs.push_back({"in", BufferType::U8});
    io.outputs.push_back({"out", BufferType::U8});

    c.cpu->registerKernel(std::make_shared<CopyKernel>("kA", io));
    c.cpu->registerKernel(std::make_shared<CopyKernel>("kB", io));
    c.cpu->registerKernel(std::make_shared<CopyKernel>("kC", io));
    c.g.registerDevice(c.cpu);

    c.size = c.g.scalarInput<std::uint64_t>("elements");
    GraphBuffer raw = c.g.inputBuffer(BufferType::U8, "raw", c.size);

    GraphBuffer outA, outB, outC;

    IOMap mA;
    mA.bindInput("in", raw).bindOutput("out", BufferType::U8, outA);
    c.nodeA = c.g.addNode(cpuKernel("kA", io), std::move(mA), "cpu");

    IOMap mB;
    mB.bindInput("in", outA).bindOutput("out", BufferType::U8, outB);
    c.nodeB = c.g.addNode(cpuKernel("kB", io), std::move(mB), "cpu");

    IOMap mC;
    mC.bindInput("in", outB).bindOutput("out", BufferType::U8, outC);
    c.nodeC = c.g.addNode(cpuKernel("kC", io), std::move(mC), "cpu",
                          /*afterNodes=*/{c.nodeA});  // explicit ordering edge

    return c;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Set VRT_RENDER_PRINT=1 (or any non-empty value) in the environment to make
// the render tests dump their DOT / ASCII output to stdout.  When unset they
// stay quiet so normal CI logs aren't cluttered.
//
//   VRT_RENDER_PRINT=1 ./tests/render_test
//   VRT_RENDER_PRINT=1 ctest -R render_test --output-on-failure -V
bool verbosePrint() {
    const char* v = std::getenv("VRT_RENDER_PRINT");
    return v && v[0] != '\0';
}

void dumpSection(const std::string& title, const std::string& body) {
    if (!verbosePrint()) return;
    std::cout << "\n--- " << title << " ---\n" << body;
    if (body.empty() || body.back() != '\n') std::cout << '\n';
    std::cout.flush();
}

}  // namespace

// ---------------------------------------------------------------------------
// renderToDot(Graph)
// ---------------------------------------------------------------------------

TEST(RenderDotTest, GraphContainsHeaderNodesAndEdges) {
    auto c   = buildChain();
    auto dot = render::renderToDot(c.g);
    dumpSection("Graph DOT", dot);

    EXPECT_TRUE(contains(dot, "digraph G"));
    EXPECT_TRUE(contains(dot, "subgraph cluster_"));
    EXPECT_TRUE(contains(dot, c.nodeA));
    EXPECT_TRUE(contains(dot, c.nodeB));
    EXPECT_TRUE(contains(dot, c.nodeC));
    EXPECT_TRUE(contains(dot, "->"));
    // Solid (data) edge from A -> B is unconditional; "after" edge for A -> C
    EXPECT_TRUE(contains(dot, "style=dashed"));
}

TEST(RenderDotTest, GraphLabelsCpuCluster) {
    auto c   = buildChain();
    auto dot = render::renderToDot(c.g);
    EXPECT_TRUE(contains(dot, "cpu [CPU]"));
}

TEST(RenderDotTest, CompilerStagesAreIndependentlyRenderable) {
    auto chain = buildChain();
    CompiledGraph compiled = chain.g.compile();
    ASSERT_NE(compiled.scheduledGraph(), nullptr);
    const ScheduledGraph& scheduled = *compiled.scheduledGraph();
    const RoutedGraph& routed = scheduled.routed();
    const PlacedGraph& placed = routed.placed();
    const ResolvedGraph& resolved = placed.resolved();

    const std::string resolvedDot = render::renderToDot(resolved);
    const std::string placedDot = render::renderToDot(placed);
    const std::string routedDot = render::renderToDot(routed);
    const std::string scheduledDot = render::renderToDot(scheduled);

    EXPECT_TRUE(contains(resolvedDot, "digraph ResolvedGraph"));
    EXPECT_TRUE(contains(placedDot, "digraph PlacedGraph"));
    EXPECT_TRUE(contains(placedDot, "@cpu"));
    EXPECT_TRUE(contains(routedDot, "digraph RoutedGraph"));
    EXPECT_TRUE(contains(scheduledDot, "digraph ScheduledGraph"));
    EXPECT_TRUE(contains(scheduledDot, "cluster_q"));
}

TEST(RenderDotTest, GraphRendersAuthoredLoopRegion) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto body = graph.rootRegion().createChild();
    std::string bodyKernelId = body->addKernel(cpuKernel("loopBody"), IOMap{}, "cpu");
    std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    auto dot = render::renderToDot(graph);
    dumpSection("Graph DOT (authored loop)", dot);

    EXPECT_TRUE(contains(dot, loopId));
    EXPECT_TRUE(contains(dot, bodyKernelId));
    EXPECT_TRUE(contains(dot, "[Loop]"));
    EXPECT_TRUE(contains(dot, "FixedCount"));
    EXPECT_TRUE(contains(dot, loopId + " loop body"));
}

TEST(RenderDotTest, GraphRendersAuthoredConditionalRegions) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();
    std::string thenKernelId = thenRegion->addKernel(cpuKernel("then"), IOMap{}, "cpu");
    std::string elseKernelId = elseRegion->addKernel(cpuKernel("else"), IOMap{}, "cpu");
    std::string conditionalId = graph.addConditional(
        ifElseSpec(Condition::alwaysTrue(), thenRegion, elseRegion));

    auto dot = render::renderToDot(graph);
    dumpSection("Graph DOT (authored conditional)", dot);

    EXPECT_TRUE(contains(dot, conditionalId));
    EXPECT_TRUE(contains(dot, thenKernelId));
    EXPECT_TRUE(contains(dot, elseKernelId));
    EXPECT_TRUE(contains(dot, "[Conditional]"));
    EXPECT_TRUE(contains(dot, conditionalId + " then"));
    EXPECT_TRUE(contains(dot, conditionalId + " else"));
}

TEST(RenderDotTest, GraphRendersAuthoredBoundaryNodes) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar parentCounter = graph.globalScalar(ScalarType::I32, "counter");
    auto body = graph.rootRegion().createChild();
    GraphScalar localCounter = body->scalar(ScalarType::I32, "counter");
    std::string startId = body->importFromParent({{parentCounter, localCounter}});
    std::string endId = body->exportToParent({{localCounter, parentCounter}}, {startId});
    graph.addLoop(fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    auto dot = render::renderToDot(graph);
    dumpSection("Graph DOT (authored boundaries)", dot);

    EXPECT_TRUE(contains(dot, startId));
    EXPECT_TRUE(contains(dot, endId));
    EXPECT_TRUE(contains(dot, "[Boundary]"));
    EXPECT_TRUE(contains(dot, "(Start)"));
    EXPECT_TRUE(contains(dot, "(End)"));
    // Boundary ops live in the child body region; the renderer namespaces
    // their Graphviz ids by the region's scope so that identical authored
    // ids in different regions don't collide once nested clusters share one
    // `digraph G`.
    const std::string scopePrefix = "scope" + std::to_string(body->scopeId()) + "__";
    EXPECT_TRUE(contains(dot,
                         "\"" + scopePrefix + startId + "\" -> \"" +
                             scopePrefix + endId + "\""));
}

TEST(RenderDotTest, GraphRendersLoopOutputsThroughLoopNodeAtParentScope) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphBuffer raw = graph.inputBuffer(BufferType::I32, "raw");
    IOMap produceIo;
    GraphBuffer beforeLoop;
    produceIo.bindInput("in", raw)
             .bindOutput("out", BufferType::I32, beforeLoop);
    std::string producerId =
        graph.addNode(cpuKernel("produce"), std::move(produceIo), "cpu");

    auto body = graph.rootRegion().createChild();
    GraphBuffer localIn = body->inputBuffer(BufferType::I32, "state");
    body->importFromParent(std::vector<BufferBoundaryMapping>{{beforeLoop, localIn}});
    IOMap bodyIo;
    GraphBuffer localOut;
    bodyIo.bindInput("in", localIn)
          .bindOutput("out", BufferType::I32, localOut, body->scopeId());
    std::string bodyId = body->addKernel(cpuKernel("body"), std::move(bodyIo), "cpu");
    GraphBuffer afterLoop = GraphBuffer::make(BufferType::I32, "after_loop");
    body->exportToParent(std::vector<BufferBoundaryMapping>{{localOut, afterLoop}},
                         {bodyId});
    std::string loopId = graph.addLoop(
        fixedLoopSpec(tripCount(tripCountScalar(graph.rootRegion())), body));

    IOMap consumeIo;
    GraphBuffer finalOut;
    consumeIo.bindInput("in", afterLoop)
             .bindOutput("out", BufferType::I32, finalOut);
    std::string consumerId =
        graph.addNode(cpuKernel("consume"), std::move(consumeIo), "cpu", {loopId});

    auto dot = render::renderToDot(graph);
    EXPECT_TRUE(contains(dot, "\"" + producerId + "\" -> \"" + loopId + "\""));
    EXPECT_TRUE(contains(dot, "\"" + loopId + "\" -> \"" + consumerId + "\""));
    EXPECT_FALSE(contains(dot, "subgraph_end_1\" -> \"" + consumerId + "\""));
}

TEST(RenderDotTest, GraphRendersConditionalOutputsThroughConditionalNodeAtParentScope) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphBuffer raw = graph.inputBuffer(BufferType::I32, "raw");
    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();

    auto buildBranch = [&](const std::shared_ptr<GraphRegion>& region,
                           const std::string& kernelName) {
        GraphBuffer localIn = region->inputBuffer(BufferType::I32, "in");
        region->importFromParent(std::vector<BufferBoundaryMapping>{{raw, localIn}});
        IOMap io;
        GraphBuffer localOut;
        io.bindInput("in", localIn)
          .bindOutput("out", BufferType::I32, localOut, region->scopeId());
        std::string kid = region->addKernel(cpuKernel(kernelName), std::move(io), "cpu");
        return std::pair<GraphBuffer, std::string>{localOut, kid};
    };

    auto [thenOut, thenId] = buildBranch(thenRegion, "then");
    auto [elseOut, elseId] = buildBranch(elseRegion, "else");
    GraphBuffer condOut = GraphBuffer::make(BufferType::I32, "cond_out");
    thenRegion->exportToParent(std::vector<BufferBoundaryMapping>{{thenOut, condOut}},
                               {thenId});
    elseRegion->exportToParent(std::vector<BufferBoundaryMapping>{{elseOut, condOut}},
                               {elseId});

    std::string condId = graph.addConditional(
        ifElseSpec(Condition::alwaysTrue(), thenRegion, elseRegion));

    IOMap consumeIo;
    GraphBuffer finalOut;
    consumeIo.bindInput("in", condOut)
             .bindOutput("out", BufferType::I32, finalOut);
    std::string consumerId =
        graph.addNode(cpuKernel("consume"), std::move(consumeIo), "cpu", {condId});

    auto dot = render::renderToDot(graph);
    EXPECT_TRUE(contains(dot, "\"subgraph_start_"));
    EXPECT_TRUE(contains(dot, "\"" + condId + "\" -> \"" + consumerId + "\""));
    EXPECT_FALSE(contains(dot, "subgraph_end_1\" -> \"" + consumerId + "\""));
}

TEST(RenderDotTest, GraphRendersScalarConditionEdgeToConditionalNode) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    IOTypeMap predType;
    predType.outputScalars.push_back({"out", ScalarType::I32});
    GraphScalar flag = graph.globalScalar(ScalarType::I32, "flag");
    IOMap predIo;
    predIo.bindOutputScalar("out", flag);
    std::string predId = graph.addNode(
        KernelDescriptor{"pred", DeviceType::CPU, std::nullopt, predType},
        std::move(predIo), "cpu");

    auto thenRegion = graph.rootRegion().createChild();
    auto elseRegion = graph.rootRegion().createChild();
    std::string condId = graph.addConditional(ifElseSpec(
        Condition::compare(CompareOp::EQ,
                           ConditionOperand::scalar(ScalarType::I32, flag.varName(),
                                                    flag.scopeId()),
                           ConditionOperand::constant<int32_t>(1)),
        thenRegion, elseRegion, {predId}));

    auto dot = render::renderToDot(graph);
    EXPECT_TRUE(contains(dot, "\"" + predId + "\" -> \"" + condId + "\""));
    EXPECT_TRUE(contains(dot, "scalar: flag"));
}

TEST(RenderDotTest, GraphRendersScalarTripCountEdgeToLoopNode) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    IOTypeMap predType;
    predType.outputScalars.push_back({"out", ScalarType::I32});
    GraphScalar count = graph.globalScalar(ScalarType::I32, "trip_count");
    IOMap predIo;
    predIo.bindOutputScalar("out", count);
    std::string predId = graph.addNode(
        KernelDescriptor{"count_producer", DeviceType::CPU, std::nullopt, predType},
        std::move(predIo), "cpu");

    auto body = graph.rootRegion().createChild();
    LoopSpec loop;
    loop.tripCount = LoopTripCount::scalar(count.type(), count.varName(), count.scopeId());
    loop.body = body;
    loop.afterOps = {predId};
    std::string loopId = graph.addLoop(std::move(loop));

    auto dot = render::renderToDot(graph);
    EXPECT_TRUE(contains(dot, "\"" + predId + "\" -> \"" + loopId + "\""));
    EXPECT_TRUE(contains(dot, "scalar: trip_count"));
}

TEST(RenderDotTest, GraphRendersUnproducedScalarTripCountAsInputNode) {
    Graph graph;
    graph.registerDevice(std::make_shared<CpuDevice>("cpu"));

    GraphScalar count = graph.scalarInput<std::uint32_t>("dispatch_count");
    auto body = graph.rootRegion().createChild();
    std::string loopId = graph.addLoop(fixedLoopSpec(LoopTripCount::scalar(count), body));

    auto dot = render::renderToDot(graph);
    EXPECT_TRUE(contains(dot, "scalar input\\ndispatch_count"));
    EXPECT_TRUE(contains(dot, "\"__scalar_input_0_dispatch_count\" -> \"" + loopId + "\""));
    EXPECT_TRUE(contains(dot, "scalar: dispatch_count"));
}

// ---------------------------------------------------------------------------
// renderToDot(DGraph)
// ---------------------------------------------------------------------------

TEST(RenderDotTest, DGraphRendersAfterCompile) {
    auto c = buildChain();
    std::vector<uint8_t> data(16, 0xAB);
    c.cpu->setInputBuffer("raw", data.data(), data.size());
    auto exec = c.g.compile();
    exec.writeScalar(c.size, static_cast<std::uint64_t>(data.size()));
    exec.run();

    ASSERT_FALSE(exec.dgraphs().empty());
    bool sawCpuDg = false;
    for (const auto& dg : exec.dgraphs()) {
        if (dg.deviceId == "cpu") {
            sawCpuDg = true;
            auto dot = render::renderToDot(dg);
            dumpSection("DGraph DOT [" + dg.deviceId + "]", dot);
            EXPECT_TRUE(contains(dot, "digraph"));
            EXPECT_TRUE(contains(dot, c.nodeA));
            EXPECT_TRUE(contains(dot, c.nodeB));
            EXPECT_TRUE(contains(dot, c.nodeC));
            EXPECT_TRUE(contains(dot, "->"));
        }
    }
    EXPECT_TRUE(sawCpuDg);
}

TEST(RenderDotTest, DGraphIncludesCompiledBoundaryMetadata) {
    CompiledBoundaryNode start;
    start.id = "boundary_start";
    start.deviceId = "cpu";
    start.side = CompiledBoundaryNode::Side::Start;
    start.scalarCopies.push_back(CompiledScalarBoundaryCopy{"parent_count", 0,
                                                            "local_count", 1});
    start.bufferCopies.push_back(CompiledBufferBoundaryCopy{"parent_in", 0,
                                                            "local_in", 1});
    start.bufferCopies.push_back(CompiledBufferBoundaryCopy{"parent_aux", 0,
                                                            "local_aux", 1});

    CompiledBoundaryNode end;
    end.id = "boundary_end";
    end.deviceId = "cpu";
    end.side = CompiledBoundaryNode::Side::End;
    end.bufferCopies.push_back(CompiledBufferBoundaryCopy{"local_out", 1,
                                                          "parent_out", 0});

    DGraph dg;
    dg.deviceId = "cpu";
    dg.nodes.emplace_back(std::move(start));
    dg.nodes.emplace_back(std::move(end));

    auto dot = render::renderToDot(dg);
    dumpSection("DGraph DOT (compiled boundary metadata)", dot);

    EXPECT_TRUE(contains(dot, "boundary_start"));
    EXPECT_TRUE(contains(dot, "boundary_end"));
    EXPECT_TRUE(contains(dot, "[Boundary]"));
    EXPECT_TRUE(contains(dot, "(Start)"));
    EXPECT_TRUE(contains(dot, "(End)"));
    EXPECT_TRUE(contains(dot, "copies: 2 buffers, 1 scalar"));
    EXPECT_TRUE(contains(dot, "copies: 1 buffer, 0 scalars"));
}

TEST(RenderDotTest, DGraphIncludesCompiledFixedLoopMetadata) {
    CompiledLoopNode loop;
    loop.id = "loop_0";
    loop.deviceId = "cpu";
    loop.loopKind = CompiledLoopKind::FixedCount;
    loop.tripCount = LoopTripCount::scalar(ScalarType::I32, "trip_count");
    loop.outputBufferPlacements["out"] = "cpu";
    loop.outputScalarPlacements["count"] = "cpu";

    CompiledLoopBufferPublication bufferPublication;
    bufferPublication.portName = "out";
    bufferPublication.parentTokenName = "parent_out";
    loop.outputBufferPublications.push_back(std::move(bufferPublication));

    CompiledLoopScalarPublication scalarPublication;
    scalarPublication.portName = "count";
    scalarPublication.parentTokenName = "parent_count";
    loop.outputScalarPublications.push_back(std::move(scalarPublication));

    DGraph dg;
    dg.deviceId = "cpu";
    dg.nodes.emplace_back(std::move(loop));

    auto dot = render::renderToDot(dg);
    dumpSection("DGraph DOT (compiled fixed loop metadata)", dot);

    EXPECT_TRUE(contains(dot, "loop_0"));
    EXPECT_TRUE(contains(dot, "[Loop]"));
    EXPECT_TRUE(contains(dot, "(FixedCount)"));
    EXPECT_TRUE(contains(dot, "trip: scalar trip_count"));
    EXPECT_TRUE(contains(dot, "outputs: 1 buffer, 1 scalar"));
    EXPECT_TRUE(contains(dot, "placements: 1 buffer, 1 scalar"));
}

TEST(RenderDotTest, DGraphIncludesCompiledWhileLoopConditionMetadata) {
    CompiledLoopNode loop;
    loop.id = "while_0";
    loop.deviceId = "cpu";
    loop.loopKind = CompiledLoopKind::WhileCondition;
    loop.condition = Condition::compare(
        CompareOp::LT,
        ConditionOperand::scalar(ScalarType::I32, "counter", 7),
        ConditionOperand::constant<int32_t>(10));

    DGraph dg;
    dg.deviceId = "cpu";
    dg.nodes.emplace_back(std::move(loop));

    auto dot = render::renderToDot(dg);
    dumpSection("DGraph DOT (compiled while metadata)", dot);

    EXPECT_TRUE(contains(dot, "while_0"));
    EXPECT_TRUE(contains(dot, "[Loop]"));
    EXPECT_TRUE(contains(dot, "(WhileCondition)"));
    EXPECT_TRUE(contains(dot, "condition: LT"));
    EXPECT_TRUE(contains(dot, "1 scalar"));
}

TEST(RenderDotTest, DGraphIncludesCompiledConditionalMetadata) {
    CompiledConditionalNode conditional;
    conditional.id = "conditional_0";
    conditional.deviceId = "cpu";
    conditional.condition = Condition::alwaysFalse();
    conditional.outputBufferPlacements["out"] = "cpu";

    CompiledConditionalBufferPublication bufferPublication;
    bufferPublication.portName = "out";
    bufferPublication.parentTokenName = "parent_out";
    conditional.outputBufferPublications.push_back(std::move(bufferPublication));

    CompiledConditionalScalarPublication scalarPublication;
    scalarPublication.portName = "flag";
    scalarPublication.parentTokenName = "parent_flag";
    conditional.outputScalarPublications.push_back(std::move(scalarPublication));

    DGraph dg;
    dg.deviceId = "cpu";
    dg.nodes.emplace_back(std::move(conditional));

    auto dot = render::renderToDot(dg);
    dumpSection("DGraph DOT (compiled conditional metadata)", dot);

    EXPECT_TRUE(contains(dot, "conditional_0"));
    EXPECT_TRUE(contains(dot, "[Conditional]"));
    EXPECT_TRUE(contains(dot, "condition: always false"));
    EXPECT_TRUE(contains(dot, "outputs: 1 buffer, 1 scalar"));
    EXPECT_TRUE(contains(dot, "placements: 1 buffer, 0 scalars"));
}

// ---------------------------------------------------------------------------
// writeToDotFile: writes the DOT source to disk verbatim.
// ---------------------------------------------------------------------------

TEST(RenderDotTest, WriteToDotFileWritesGraphAndDGraph) {
    auto c = buildChain();
    std::vector<uint8_t> data(8, 0xCD);
    c.cpu->setInputBuffer("raw", data.data(), data.size());
    auto exec = c.g.compile();
    exec.writeScalar(c.size, static_cast<std::uint64_t>(data.size()));
    exec.run();

    char gpath[]  = "/tmp/vrt_render_test_graph_XXXXXX.dot";
    char dgpath[] = "/tmp/vrt_render_test_dgraph_XXXXXX.dot";
    int gfd  = mkstemps(gpath,  4);
    int dgfd = mkstemps(dgpath, 4);
    ASSERT_GE(gfd,  0);
    ASSERT_GE(dgfd, 0);
    ::close(gfd);
    ::close(dgfd);

    ASSERT_NO_THROW(render::writeToDotFile(c.g, gpath));
    ASSERT_FALSE(exec.dgraphs().empty());
    ASSERT_NO_THROW(render::writeToDotFile(exec.dgraphs().front(), dgpath));

    auto slurp = [](const std::string& p) {
        std::ifstream     ifs(p);
        std::stringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    };
    auto gtxt  = slurp(gpath);
    auto dgtxt = slurp(dgpath);
    EXPECT_EQ(gtxt,  render::renderToDot(c.g));
    EXPECT_EQ(dgtxt, render::renderToDot(exec.dgraphs().front()));
    EXPECT_EQ(gtxt.rfind("digraph", 0), 0u);
    EXPECT_EQ(dgtxt.rfind("digraph", 0), 0u);

    std::remove(gpath);
    std::remove(dgpath);
}

TEST(RenderDotTest, WriteToDotFileThrowsOnBadPath) {
    auto c = buildChain();
    EXPECT_THROW(render::writeToDotFile(c.g, "/no/such/dir/out.dot"),
                 std::runtime_error);
}

// ---------------------------------------------------------------------------
// DGraph rendering: CompiledBridgeOpNodes appear as dashed ellipses with the
// bridge-supplied label.
// ---------------------------------------------------------------------------

TEST(RenderDotTest, DGraphIncludesBridgeOpNodes) {
    // Build a DGraph by hand containing one CompiledKernelNode and one CompiledBridgeOpNode
    // (Producer side). This bypasses the compiler so we don't need a full
    // cross-device pipeline just to exercise the renderer.
    struct StubBridgeOp : IBridgeOp {
        std::string label() const override { return "stub_xfer"; }
    };

    CompiledKernelNode k;
    k.id     = "kA_0";
    k.kernel = cpuKernel("kA");

    CompiledBridgeOpNode b;
    b.id              = "_bridge_0_p";
    b.deviceId        = "cpu";
    b.op              = std::make_shared<StubBridgeOp>();
    b.action          = []{};
    b.side            = CompiledBridgeOpNode::Side::Producer;
    b.pairedKernelId  = "kA_0";
    b.dependsOn       = {"kA_0"};  // Phase 1: explicit predecessor.

    DGraph dg;
    dg.deviceId = "cpu";
    dg.nodes.emplace_back(std::move(k));
    dg.nodes.emplace_back(std::move(b));

    auto dot = render::renderToDot(dg);
    dumpSection("DGraph DOT (with bridge op)", dot);

    EXPECT_TRUE(contains(dot, "kA_0"));
    EXPECT_TRUE(contains(dot, "_bridge_0_p"));
    EXPECT_TRUE(contains(dot, "shape=ellipse"));
    EXPECT_TRUE(contains(dot, "stub_xfer"));
    EXPECT_TRUE(contains(dot, "Producer"));
    // The dependsOn entry must materialise as an edge.
    EXPECT_TRUE(contains(dot, "\"kA_0\" -> \"_bridge_0_p\""));
}

TEST(RenderDotTest, DGraphIncludesDeviceCopyNodes) {
    CompiledKernelNode k;
    k.id = "producer";
    k.kernel = cpuKernel("producer");

    CompiledDeviceCopyNode copy;
    copy.id = "_device_copy_0";
    copy.deviceId = "cpu";
    copy.source = GraphBuffer::make(BufferType::I32, "buf", 0);
    copy.target = GraphBuffer::make(BufferType::I32, "buf__copy", 0);
    copy.sourceRegion = "HBM0";
    copy.targetRegion = "HBM1";
    copy.mechanism = "host_qdma";
    copy.dependsOn = {"producer"};

    DGraph dg;
    dg.deviceId = "cpu";
    dg.nodes.emplace_back(std::move(k));
    dg.nodes.emplace_back(std::move(copy));

    auto dot = render::renderToDot(dg);
    dumpSection("DGraph DOT (with device copy)", dot);

    EXPECT_TRUE(contains(dot, "_device_copy_0"));
    EXPECT_TRUE(contains(dot, "DeviceCopy"));
    EXPECT_TRUE(contains(dot, "HBM0 -> HBM1"));
    EXPECT_TRUE(contains(dot, "host_qdma"));
    EXPECT_TRUE(contains(dot, "\"producer\" -> \"_device_copy_0\""));
}

// ---------------------------------------------------------------------------
// Every dependsOn entry must produce exactly one edge in the rendered DOT.
// ---------------------------------------------------------------------------

TEST(RenderDotTest, DGraphRendersEveryDependsOnAsEdge) {
    struct StubBridgeOp : IBridgeOp {
        std::string label() const override { return "stub"; }
    };

    CompiledKernelNode kA; kA.id = "kA"; kA.kernel = cpuKernel("kA");
    CompiledKernelNode kB; kB.id = "kB"; kB.kernel = cpuKernel("kB");
    kB.dependsOn = {"kA"};

    CompiledBridgeOpNode bp;
    bp.id = "_bridge_p"; bp.deviceId = "cpu";
    bp.op = std::make_shared<StubBridgeOp>(); bp.action = []{};
    bp.side = CompiledBridgeOpNode::Side::Producer; bp.pairedKernelId = "kB";
    bp.dependsOn = {"kB"};

    CompiledBridgeOpNode bc;
    bc.id = "_bridge_c"; bc.deviceId = "cpu";
    bc.op = std::make_shared<StubBridgeOp>(); bc.action = []{};
    bc.side = CompiledBridgeOpNode::Side::Consumer; bc.pairedKernelId = "kA";
    bc.dependsOn = {"_bridge_p"};  // arbitrary cross-bridge dep, e.g. bounce chain

    DGraph dg;
    dg.deviceId = "cpu";
    dg.nodes.emplace_back(std::move(kA));
    dg.nodes.emplace_back(std::move(kB));
    dg.nodes.emplace_back(std::move(bp));
    dg.nodes.emplace_back(std::move(bc));

    auto dot = render::renderToDot(dg);
    EXPECT_TRUE(contains(dot, "\"kA\" -> \"kB\""));
    EXPECT_TRUE(contains(dot, "\"kB\" -> \"_bridge_p\""));
    EXPECT_TRUE(contains(dot, "\"_bridge_p\" -> \"_bridge_c\""));
}

// ---------------------------------------------------------------------------
// Barrier op renders with its label and produces edges.
// ---------------------------------------------------------------------------

TEST(RenderDotTest, BarrierOpRendersInDot) {
    struct BarrierStub : IBridgeOp {
        std::string label() const override { return "barrier"; }
    };

    CompiledKernelNode kT; kT.id = "kTarget"; kT.kernel = cpuKernel("kT");
    CompiledBridgeOpNode bc;
    bc.id = "_barrier_0_c"; bc.deviceId = "cpu";
    bc.op = std::make_shared<BarrierStub>(); bc.action = []{};
    bc.side = CompiledBridgeOpNode::Side::Consumer; bc.pairedKernelId = "kTarget";
    kT.dependsOn = {"_barrier_0_c"};

    DGraph dg;
    dg.deviceId = "cpu";
    dg.nodes.emplace_back(std::move(bc));
    dg.nodes.emplace_back(std::move(kT));

    auto dot = render::renderToDot(dg);
    EXPECT_TRUE(contains(dot, "_barrier_0_c"));
    EXPECT_TRUE(contains(dot, "[barrier]"));
    EXPECT_TRUE(contains(dot, "\"_barrier_0_c\" -> \"kTarget\""));
}
