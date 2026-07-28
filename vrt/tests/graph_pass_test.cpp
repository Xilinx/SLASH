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

#include <memory>
#include <map>
#include <optional>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include <vrt/graph/diagnostics.hpp>
#include <vrt/graph/backend_resource_binding.hpp>
#include <vrt/graph/graph.hpp>
#include <vrt/graph/ids.hpp>
#include <vrt/graph/ir/authored_graph.hpp>
#include <vrt/graph/ir/placed_graph.hpp>
#include <vrt/graph/ir/resolved_graph.hpp>
#include <vrt/graph/ir/routed_graph.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>
#include <vrt/graph/semantic_plan.hpp>

using namespace vrt::graph;

namespace {

KernelDescriptor cpuKernel(std::string name) {
    return KernelDescriptor{
        std::move(name), DeviceType::CPU, std::nullopt, {}};
}

class PlacementPlan : public IDevicePlan {
   public:
    void launch() override {}
    void wait() override {}
};

struct PlacementResourceState {
    std::mutex              mutex;
    std::set<std::uint32_t> used;
};

class PlacementResourceLease : public IDeviceResourceLease {
   public:
    PlacementResourceLease(
        std::shared_ptr<PlacementResourceState> state,
        std::map<RendezvousId, std::uint32_t> resources)
        : state_(std::move(state)),
          resources_(std::move(resources)) {}

    ~PlacementResourceLease() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (const auto& [logical, physical] : resources_) {
            (void)logical;
            state_->used.erase(physical);
        }
    }

    std::uint32_t physicalIndex(
        RendezvousId logical) const override {
        return resources_.at(logical);
    }

   private:
    std::shared_ptr<PlacementResourceState>    state_;
    std::map<RendezvousId, std::uint32_t> resources_;
};

class PlacementDevice : public IDevice {
   public:
    PlacementDevice(std::string id, DeviceType type,
                    DeviceCapabilities capabilities,
                    bool acceptControl = false)
        : id_(std::move(id)),
          type_(type),
          capabilities_(std::move(capabilities)),
          acceptControl_(acceptControl) {
        capabilities_.device = DeviceId(id_);
    }

    DeviceType type() const override { return type_; }
    std::string id() const override { return id_; }

    DeviceCapabilities compilerCapabilities() const override {
        return capabilities_;
    }

    CapabilityDecision evaluateControlCapability(
        const ControlCapabilityRequest&) const override {
        return acceptControl_
                   ? CapabilityDecision::accept()
                   : CapabilityDecision::reject(
                         DeviceId(id_), "test rejection");
    }

    std::optional<std::string> resolveMemoryRegion(
        const KernelDescriptor&, const std::string& port) const override {
        auto it = regions_.find(port);
        return it == regions_.end()
                   ? std::nullopt
                   : std::optional<std::string>(it->second);
    }

    void setRegion(std::string port, std::string region) {
        regions_[std::move(port)] = std::move(region);
    }

    std::unique_ptr<IDeviceResourceLease>
    leaseRendezvousResources(
        const std::vector<RendezvousId>& logical) override {
        std::map<RendezvousId, std::uint32_t> resources;
        std::lock_guard<std::mutex> lock(resourceState_->mutex);
        for (RendezvousId id : logical) {
            std::uint32_t physical = 0;
            while (resourceState_->used.count(physical) != 0) {
                ++physical;
            }
            resourceState_->used.insert(physical);
            resources[id] = physical;
        }
        return std::make_unique<PlacementResourceLease>(
            resourceState_, std::move(resources));
    }

    std::unique_ptr<IDevicePlan> compilePlan(
        const DGraph&) override {
        return std::make_unique<PlacementPlan>();
    }

   private:
    std::string                       id_;
    DeviceType                        type_ = DeviceType::CPU;
    DeviceCapabilities                capabilities_;
    bool                              acceptControl_ = false;
    std::map<std::string, std::string> regions_;
    std::shared_ptr<PlacementResourceState> resourceState_ =
        std::make_shared<PlacementResourceState>();
};

DeviceCapabilities hostCapabilities() {
    DeviceCapabilities capabilities;
    capabilities.backend = "test_host";
    capabilities.kernelTypes.insert(DeviceType::CPU);
    capabilities.hostsGraphIo = true;
    capabilities.ownsFallbackControl = true;
    capabilities.supportsSplitAuthority = true;
    return capabilities;
}

DeviceCapabilities acceleratorCapabilities() {
    DeviceCapabilities capabilities;
    capabilities.backend = "test_accelerator";
    capabilities.kernelTypes.insert(DeviceType::FPGA);
    capabilities.supportsReprogram = true;
    capabilities.supportsAutonomousControl = true;
    capabilities.supportsSplitFollower = true;
    capabilities.prefersSplitPrimary = true;
    capabilities.supportsMemoryRegionCopies = true;
    capabilities.ownsRendezvousNamespace = true;
    return capabilities;
}

DeviceCapabilityCatalog placementCatalog(
    const std::shared_ptr<PlacementDevice>& accelerator,
    const std::shared_ptr<PlacementDevice>& host =
        std::make_shared<PlacementDevice>(
            "cpu", DeviceType::CPU, hostCapabilities())) {
    std::map<std::string, std::shared_ptr<IDevice>> devices;
    devices.emplace(host->id(), host);
    if (accelerator) {
        devices.emplace(accelerator->id(), accelerator);
    }
    return DeviceCapabilityCatalog::fromDevices(devices);
}

BridgeFactory markerBridgeFactory() {
    return [](IDevice&, IDevice&) -> std::shared_ptr<IBridge> {
        return nullptr;
    };
}

AuthoredGraph nestedSnapshot() {
    auto root = GraphRegion::createRoot();
    GraphScalar trips = root->scalar(ScalarType::I32, "trips");
    auto body = root->createChild();
    body->addKernel(cpuKernel("body"), {}, "cpu");

    LoopSpec loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));
    return AuthoredGraph::snapshot(*root);
}

}  // namespace

TEST(GraphPassTest, CompilerIdsAreStrongTypes) {
    static_assert(!std::is_same_v<NodeId, RegionId>);
    static_assert(!std::is_same_v<DeviceId, MemoryRegionId>);

    EXPECT_EQ(NodeId(7).value(), 7u);
    EXPECT_EQ(DeviceId("cpu").value(), "cpu");
    EXPECT_LT(NodeId(2), NodeId(3));
}

TEST(GraphPassTest, AuthoredSnapshotOwnsNestedStructure) {
    auto root = GraphRegion::createRoot();
    GraphScalar trips = root->scalar(ScalarType::I32, "trips");
    auto body = root->createChild();
    body->addKernel(cpuKernel("before_snapshot"), {}, "cpu");

    LoopSpec loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = body;
    root->addLoop(std::move(loop));

    const AuthoredGraph snapshot = AuthoredGraph::snapshot(*root);
    body->addKernel(cpuKernel("after_snapshot"), {}, "cpu");
    root->addKernel(cpuKernel("root_after_snapshot"), {}, "cpu");

    ASSERT_EQ(snapshot.root().operations.size(), 1u);
    const auto* authoredLoop =
        std::get_if<AuthoredLoop>(&snapshot.root().operations.front());
    ASSERT_NE(authoredLoop, nullptr);
    ASSERT_NE(authoredLoop->body, nullptr);
    ASSERT_EQ(authoredLoop->body->operations.size(), 1u);
    EXPECT_EQ(authoredSourceId(authoredLoop->body->operations.front()),
              "before_snapshot_0");
}

TEST(GraphPassTest, AuthoredIdsIgnoreProcessGlobalScopeIds) {
    const AuthoredGraph first = nestedSnapshot();
    const AuthoredGraph second = nestedSnapshot();

    EXPECT_EQ(first.root().id, second.root().id);
    ASSERT_EQ(first.root().operations.size(), second.root().operations.size());
    EXPECT_EQ(authoredNodeId(first.root().operations.front()),
              authoredNodeId(second.root().operations.front()));

    const auto& firstLoop =
        std::get<AuthoredLoop>(first.root().operations.front());
    const auto& secondLoop =
        std::get<AuthoredLoop>(second.root().operations.front());
    ASSERT_NE(firstLoop.body, nullptr);
    ASSERT_NE(secondLoop.body, nullptr);
    EXPECT_NE(firstLoop.body->sourceScopeId, secondLoop.body->sourceScopeId);
    EXPECT_EQ(firstLoop.body->id, secondLoop.body->id);
    EXPECT_EQ(authoredNodeId(firstLoop.body->operations.front()),
              authoredNodeId(secondLoop.body->operations.front()));
}

TEST(GraphPassTest, SnapshotResolvesKnownAfterDependencies) {
    auto root = GraphRegion::createRoot();
    const std::string first =
        root->addKernel(cpuKernel("first"), {}, "cpu");
    root->addKernel(cpuKernel("second"), {}, "cpu",
                    {first, "missing_op"});

    const AuthoredGraph snapshot = AuthoredGraph::snapshot(*root);
    ASSERT_EQ(snapshot.root().operations.size(), 2u);
    const auto& second =
        std::get<AuthoredKernel>(snapshot.root().operations[1]);
    ASSERT_EQ(second.after.size(), 2u);
    ASSERT_TRUE(second.after[0].target.has_value());
    EXPECT_EQ(*second.after[0].target,
              authoredNodeId(snapshot.root().operations[0]));
    EXPECT_FALSE(second.after[1].target.has_value());
    EXPECT_EQ(second.after[1].authoredId, "missing_op");
}

TEST(GraphPassTest, PublicCompileErrorsCarryStructuredDiagnostic) {
    Graph graph;
    try {
        (void)graph.compile();
        FAIL() << "expected GraphCompileError";
    } catch (const GraphCompileError& error) {
        EXPECT_STREQ(error.what(),
                     "GraphCompiler::compile: graph has no ops");
        ASSERT_EQ(error.diagnostics().entries().size(), 1u);
        EXPECT_EQ(error.diagnostics().entries().front().code,
                  DiagCode::EmptyGraph);
    }
}

TEST(GraphPassTest, PublicCompilePreservesMessageWithTypedDiagnostic) {
    Graph graph;
    graph.registerDevice(std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities()));
    IOTypeMap type;
    type.inputs.push_back({"required", BufferType::I32});
    graph.addNode(
        KernelDescriptor{"broken", DeviceType::CPU,
                         std::nullopt, type},
        {}, "cpu");

    try {
        (void)graph.compile();
        FAIL() << "expected GraphCompileError";
    } catch (const GraphCompileError& error) {
        EXPECT_STREQ(
            error.what(),
            "GraphCompiler: op 'broken_0' missing mandatory input "
            "buffer port 'required'");
        ASSERT_FALSE(error.diagnostics().entries().empty());
        EXPECT_EQ(error.diagnostics().entries().front().code,
                  DiagCode::UnboundPort);
    }
}

TEST(GraphPassTest, ResolveGraphBuildsTypedValuesAndDependencies) {
    auto root = GraphRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    IOTypeMap type;
    type.inputs.push_back({"in", BufferType::I32});
    type.outputs.push_back({"out", BufferType::I32});
    IOMap io;
    io.bindInput("in", input).bindExistingOutput("out", output);
    root->addKernel(
        KernelDescriptor{"transform", DeviceType::CPU,
                         std::nullopt, type},
        std::move(io), "cpu");

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    CompileResult<ResolvedGraph> result = resolveGraph(authored);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.output.has_value());

    const ResolvedGraph& resolved = *result.output;
    ASSERT_EQ(resolved.operations().size(), 1u);
    ASSERT_EQ(resolved.root().topologicalOrder.size(), 1u);
    const ResolvedOperation* operation = resolved.findOperation(
        resolved.root().topologicalOrder.front());
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->bindings.size(), 2u);

    const ResolvedValue* graphOutput = nullptr;
    for (const auto& [id, value] : resolved.values()) {
        (void)id;
        if (value.sourceName == "output") graphOutput = &value;
    }
    ASSERT_NE(graphOutput, nullptr);
    EXPECT_EQ(graphOutput->type.kind, ValueKind::Buffer);
    EXPECT_EQ(graphOutput->type.buffer, BufferType::I32);
    EXPECT_TRUE(graphOutput->graphOutput);
    EXPECT_EQ(graphOutput->producer,
              std::optional<NodeId>(operation->id));
    EXPECT_TRUE(graphOutput->size.has_value());
}

TEST(GraphPassTest, ResolveGraphReportsPortAndDependencyDiagnostics) {
    auto root = GraphRegion::createRoot();
    IOTypeMap type;
    type.inputs.push_back({"required", BufferType::I32});
    root->addKernel(
        KernelDescriptor{"broken", DeviceType::CPU,
                         std::nullopt, type},
        {}, "cpu", {"missing"});

    CompileResult<ResolvedGraph> result =
        resolveGraph(AuthoredGraph::snapshot(*root));
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.output.has_value());

    bool sawPort = false;
    bool sawDependency = false;
    for (const Diagnostic& diagnostic :
         result.diagnostics.entries()) {
        sawPort |= diagnostic.code == DiagCode::UnboundPort;
        sawDependency |=
            diagnostic.code == DiagCode::UnknownDependency;
    }
    EXPECT_TRUE(sawPort);
    EXPECT_TRUE(sawDependency);
}

TEST(GraphPassTest, ResolveGraphMakesLoopCarryExplicit) {
    auto root = GraphRegion::createRoot();
    GraphScalar parent =
        root->scalar(ScalarType::I32, "counter");
    GraphScalar trips =
        root->scalar(ScalarType::I32, "trips");
    auto body = root->createChild();
    GraphScalar local =
        body->scalar(ScalarType::I32, "counter");
    GraphScalar next =
        body->scalar(ScalarType::I32, "next");
    const std::string start =
        body->importFromParent({{parent, local}});

    IOTypeMap type;
    type.inputScalars.push_back({"in", ScalarType::I32});
    type.outputScalars.push_back({"out", ScalarType::I32});
    IOMap io;
    io.bindInputScalar("in", local)
        .bindOutputScalar("out", next);
    const std::string kernel = body->addKernel(
        KernelDescriptor{"increment", DeviceType::CPU,
                         std::nullopt, type},
        std::move(io), "cpu", {start});
    body->exportToParent({{next, parent}}, {kernel});

    LoopSpec loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    CompileResult<ResolvedGraph> result =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.output.has_value());
    ASSERT_EQ(result.output->controlResults().size(), 1u);

    const ResolvedControlResult& control =
        result.output->controlResults().front();
    bool sawInitial = false;
    bool sawBackedge = false;
    for (const ControlIncoming& incoming : control.incoming) {
        sawInitial |= incoming.arm == ControlArm::LoopInitial;
        sawBackedge |= incoming.arm == ControlArm::LoopBackedge;
    }
    EXPECT_TRUE(sawInitial);
    EXPECT_TRUE(sawBackedge);
    ASSERT_EQ(control.incoming.size(), 2u);
    EXPECT_NE(control.incoming[0].value,
              control.incoming[1].value);
}

TEST(GraphPassTest, PlaceGraphUsesCapabilitiesAndMemoryRegions) {
    auto root = GraphRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    IOTypeMap type;
    type.inputs.push_back({"in", BufferType::I32});
    type.outputs.push_back({"out", BufferType::I32});
    IOMap io;
    io.bindInput("in", input).bindExistingOutput("out", output);
    root->addKernel(
        KernelDescriptor{"accelerate", DeviceType::FPGA,
                         std::nullopt, type},
        std::move(io), "accel");

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
    ASSERT_TRUE(resolved.ok());

    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    accelerator->setRegion("in", "HBM[0]");
    accelerator->setRegion("out", "HBM[1]");
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output, placementCatalog(accelerator));
    ASSERT_TRUE(placed.ok());

    const NodeId kernel =
        authoredNodeId(authored.root().operations.front());
    ASSERT_EQ(placed.output->operationPlacements().count(kernel), 1u);
    EXPECT_EQ(
        placed.output->operationPlacements().at(kernel).device,
        DeviceId("accel"));
    ASSERT_EQ(placed.output->portPlacements().size(), 2u);

    std::set<std::string> regions;
    for (const PortPlacement& port :
         placed.output->portPlacements()) {
        ASSERT_TRUE(port.memory.region.has_value());
        regions.insert(port.memory.region->value());
    }
    EXPECT_EQ(regions,
              (std::set<std::string>{"HBM[0]", "HBM[1]"}));
}

TEST(GraphPassTest, PlaceGraphChoosesAutonomousControlPostOrder) {
    auto root = GraphRegion::createRoot();
    GraphScalar trips =
        root->scalar(ScalarType::I32, "trips");
    auto body = root->createChild();
    body->addKernel(
        KernelDescriptor{"body", DeviceType::FPGA,
                         std::nullopt, {}},
        {}, "accel");
    LoopSpec loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
    ASSERT_TRUE(resolved.ok());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities(),
        true);
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output, placementCatalog(accelerator));
    ASSERT_TRUE(placed.ok());

    const NodeId control =
        authoredNodeId(authored.root().operations.front());
    const ControlPlacement& placement =
        placed.output->controlPlacements().at(control);
    EXPECT_EQ(placement.mode,
              ControlMode::AutonomousOnDevice);
    EXPECT_EQ(placement.primary, DeviceId("accel"));
    ASSERT_EQ(placement.participants.size(), 1u);
}

TEST(GraphPassTest, PlaceGraphRecordsAutonomousRejectionAndFallsBack) {
    auto root = GraphRegion::createRoot();
    GraphScalar trips =
        root->scalar(ScalarType::I32, "trips");
    auto body = root->createChild();
    body->addKernel(
        KernelDescriptor{"body", DeviceType::FPGA,
                         std::nullopt, {}},
        {}, "accel");
    LoopSpec loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
    ASSERT_TRUE(resolved.ok());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities(),
        false);
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output, placementCatalog(accelerator));
    ASSERT_TRUE(placed.ok());

    const NodeId control =
        authoredNodeId(authored.root().operations.front());
    const ControlPlacement& placement =
        placed.output->controlPlacements().at(control);
    EXPECT_EQ(placement.mode, ControlMode::HostOwned);
    EXPECT_EQ(placement.primary, DeviceId("cpu"));
    ASSERT_EQ(placement.rejections.size(), 1u);
    EXPECT_EQ(placement.rejections.front().reason,
              "test rejection");
}

TEST(GraphPassTest, PlaceGraphSplitsAcrossAuthorityAndFollower) {
    auto root = GraphRegion::createRoot();
    GraphScalar trips =
        root->scalar(ScalarType::I32, "trips");
    auto body = root->createChild();
    body->addKernel(
        KernelDescriptor{"host_body", DeviceType::CPU,
                         std::nullopt, {}},
        {}, "cpu");
    body->addKernel(
        KernelDescriptor{"device_body", DeviceType::FPGA,
                         std::nullopt, {}},
        {}, "accel");
    LoopSpec loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
    ASSERT_TRUE(resolved.ok());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities(),
        false);
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output, placementCatalog(accelerator));
    ASSERT_TRUE(placed.ok());

    const NodeId control =
        authoredNodeId(authored.root().operations.front());
    const ControlPlacement& placement =
        placed.output->controlPlacements().at(control);
    EXPECT_EQ(placement.mode,
              ControlMode::SplitAcrossDevices);
    EXPECT_EQ(placement.primary, DeviceId("accel"));
    EXPECT_EQ(
        placement.participants,
        (std::vector<DeviceId>{DeviceId("accel"), DeviceId("cpu")}));
}

TEST(GraphPassTest, RouteGraphPlansCrossDeviceGraphIoWithoutClosures) {
    auto root = GraphRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);
    IOTypeMap type;
    type.inputs.push_back({"in", BufferType::I32});
    type.outputs.push_back({"out", BufferType::I32});
    IOMap io;
    io.bindInput("in", input).bindExistingOutput("out", output);
    root->addKernel(
        KernelDescriptor{"device", DeviceType::FPGA,
                         std::nullopt, type},
        std::move(io), "accel");

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());
    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"accel", accelerator}};
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());

    std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>
        factories;
    factories[{DeviceType::CPU, DeviceType::FPGA}] =
        markerBridgeFactory();
    factories[{DeviceType::FPGA, DeviceType::CPU}] =
        markerBridgeFactory();
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(routed.ok());
    ASSERT_EQ(routed.output->routes().size(), 2u);
    for (const TransferRoute& route : routed.output->routes()) {
        ASSERT_EQ(route.legs.size(), 1u);
        EXPECT_EQ(route.legs.front().mechanism,
                  TransferMechanism::DirectBridge);
    }
}

TEST(GraphPassTest, RouteGraphSelectsHostBounceDeclaratively) {
    auto root = GraphRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer intermediate =
        root->buffer(BufferType::I32, "intermediate", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    IOTypeMap producerType;
    producerType.outputs.push_back({"out", BufferType::I32});
    IOMap producerIo;
    producerIo.bindExistingOutput("out", intermediate);
    root->addKernel(
        KernelDescriptor{"produce", DeviceType::GPU,
                         std::nullopt, producerType},
        std::move(producerIo), "gpu");

    IOTypeMap consumerType;
    consumerType.inputs.push_back({"in", BufferType::I32});
    consumerType.outputs.push_back({"out", BufferType::I32});
    IOMap consumerIo;
    consumerIo.bindInput("in", intermediate)
        .bindExistingOutput("out", output);
    root->addKernel(
        KernelDescriptor{"consume", DeviceType::FPGA,
                         std::nullopt, consumerType},
        std::move(consumerIo), "accel");

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());
    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    DeviceCapabilities gpuCapabilities;
    gpuCapabilities.backend = "test_gpu";
    gpuCapabilities.kernelTypes.insert(DeviceType::GPU);
    auto gpu = std::make_shared<PlacementDevice>(
        "gpu", DeviceType::GPU, gpuCapabilities);
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"gpu", gpu}, {"accel", accelerator}};
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());

    std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>
        factories;
    factories[{DeviceType::GPU, DeviceType::CPU}] =
        markerBridgeFactory();
    factories[{DeviceType::CPU, DeviceType::GPU}] =
        markerBridgeFactory();
    factories[{DeviceType::FPGA, DeviceType::CPU}] =
        markerBridgeFactory();
    factories[{DeviceType::CPU, DeviceType::FPGA}] =
        markerBridgeFactory();
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(routed.ok());

    auto bounce = std::find_if(
        routed.output->routes().begin(),
        routed.output->routes().end(),
        [](const TransferRoute& route) {
            return route.legs.size() == 2;
        });
    ASSERT_NE(bounce, routed.output->routes().end());
    EXPECT_EQ(bounce->legs[0].mechanism,
              TransferMechanism::HostBounce);
    EXPECT_EQ(bounce->legs[0].destination, DeviceId("cpu"));
    EXPECT_EQ(bounce->legs[1].source, DeviceId("cpu"));
}

TEST(GraphPassTest, RouteGraphMakesSameDeviceRegionCopyExplicit) {
    auto root = GraphRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer intermediate =
        root->buffer(BufferType::I32, "intermediate", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    IOTypeMap producerType;
    producerType.outputs.push_back({"produce_out", BufferType::I32});
    IOMap producerIo;
    producerIo.bindExistingOutput("produce_out", intermediate);
    root->addKernel(
        KernelDescriptor{"produce", DeviceType::FPGA,
                         std::nullopt, producerType},
        std::move(producerIo), "accel");

    IOTypeMap consumerType;
    consumerType.inputs.push_back({"consume_in", BufferType::I32});
    consumerType.outputs.push_back({"final_out", BufferType::I32});
    IOMap consumerIo;
    consumerIo.bindInput("consume_in", intermediate)
        .bindExistingOutput("final_out", output);
    root->addKernel(
        KernelDescriptor{"consume", DeviceType::FPGA,
                         std::nullopt, consumerType},
        std::move(consumerIo), "accel");

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());
    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    accelerator->setRegion("produce_out", "HBM[0]");
    accelerator->setRegion("consume_in", "HBM[1]");
    accelerator->setRegion("final_out", "HBM[1]");
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"accel", accelerator}};
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());
    std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>
        factories;
    factories[{DeviceType::FPGA, DeviceType::CPU}] =
        markerBridgeFactory();
    factories[{DeviceType::CPU, DeviceType::FPGA}] =
        markerBridgeFactory();

    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(routed.ok());
    auto copy = std::find_if(
        routed.output->routes().begin(),
        routed.output->routes().end(),
        [](const TransferRoute& route) {
            return !route.legs.empty() &&
                   route.legs.front().mechanism ==
                       TransferMechanism::HostMediatedDeviceCopy;
        });
    ASSERT_NE(copy, routed.output->routes().end());
    EXPECT_EQ(copy->requirement.source.region,
              std::optional<MemoryRegionId>(
                  MemoryRegionId("HBM[0]")));
    EXPECT_EQ(copy->requirement.destination.region,
              std::optional<MemoryRegionId>(
                  MemoryRegionId("HBM[1]")));
}

TEST(GraphPassTest, RouteGraphRejectsCrossRegionInout) {
    auto root = GraphRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    IOTypeMap type;
    type.inouts.push_back(
        {{"rw_in", BufferType::I32},
         {"rw_out", BufferType::I32}});
    IOMap io;
    io.bindExistingInout("rw_in", "rw_out", input, output);
    root->addKernel(
        KernelDescriptor{"mutate", DeviceType::FPGA,
                         std::nullopt, type},
        std::move(io), "accel");

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());
    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    accelerator->setRegion("rw_in", "HBM[0]");
    accelerator->setRegion("rw_out", "HBM[1]");
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"accel", accelerator}};
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());
    std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>
        factories;
    factories[{DeviceType::FPGA, DeviceType::CPU}] =
        markerBridgeFactory();
    factories[{DeviceType::CPU, DeviceType::FPGA}] =
        markerBridgeFactory();

    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    EXPECT_FALSE(routed.ok());
    ASSERT_FALSE(routed.diagnostics.entries().empty());
    EXPECT_EQ(routed.diagnostics.entries().front().code,
              DiagCode::IncompatibleMemoryPlacement);
}

TEST(GraphPassTest, ScheduleGraphUsesLogicalRendezvousAndValidSteps) {
    auto root = GraphRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);
    IOTypeMap type;
    type.inputs.push_back({"in", BufferType::I32});
    type.outputs.push_back({"out", BufferType::I32});
    IOMap io;
    io.bindInput("in", input).bindExistingOutput("out", output);
    root->addKernel(
        KernelDescriptor{"device", DeviceType::FPGA,
                         std::nullopt, type},
        std::move(io), "accel");

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());
    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"accel", accelerator}};
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());
    std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>
        factories;
    factories[{DeviceType::CPU, DeviceType::FPGA}] =
        markerBridgeFactory();
    factories[{DeviceType::FPGA, DeviceType::CPU}] =
        markerBridgeFactory();
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(routed.ok());

    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());
    ASSERT_FALSE(scheduled.output->rendezvous().empty());
    EXPECT_EQ(scheduled.output->rendezvous().size(),
              scheduled.output->resources().size());
    for (const auto& [id, step] : scheduled.output->steps()) {
        (void)id;
        for (ScheduleStepId dependency : step.dependencies) {
            EXPECT_EQ(scheduled.output->steps().count(dependency), 1u);
        }
    }
    const bool hasPreLaunch = std::any_of(
        scheduled.output->steps().begin(),
        scheduled.output->steps().end(),
        [](const auto& entry) {
            return entry.second.preLaunch;
        });
    EXPECT_TRUE(hasPreLaunch);
}

TEST(GraphPassTest, ScheduleGraphGatesAllSplitReplicasOnInputStaging) {
    auto root = GraphRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphScalar trips =
        root->scalar(ScalarType::I32, "trips");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);

    auto body = root->createChild();
    GraphBuffer local =
        body->inputBuffer(BufferType::I32, "local", size);
    body->importFromParent(
        std::vector<BufferBoundaryMapping>{{input, local}});
    IOTypeMap fpgaType;
    fpgaType.inputs.push_back({"in", BufferType::I32});
    IOMap fpgaIo;
    fpgaIo.bindInput("in", local);
    body->addKernel(
        KernelDescriptor{"fpga_body", DeviceType::FPGA,
                         std::nullopt, fpgaType},
        std::move(fpgaIo), "accel");
    body->addKernel(
        KernelDescriptor{"cpu_body", DeviceType::CPU,
                         std::nullopt, {}},
        {}, "cpu");
    LoopSpec loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    const NodeId control =
        authoredNodeId(authored.root().operations.front());
    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
    ASSERT_TRUE(resolved.ok());
    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities(),
        false);
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"accel", accelerator}};
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());
    ASSERT_EQ(placed.output->controlPlacements().at(control).mode,
              ControlMode::SplitAcrossDevices);
    std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>
        factories;
    factories[{DeviceType::CPU, DeviceType::FPGA}] =
        markerBridgeFactory();
    factories[{DeviceType::FPGA, DeviceType::CPU}] =
        markerBridgeFactory();
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(routed.ok());
    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());

    std::set<ScheduleStepId> preLaunchCompletions;
    for (const auto& [id, step] : scheduled.output->steps()) {
        if (step.preLaunch &&
            step.kind == ScheduledStepKind::TransferConsume) {
            preLaunchCompletions.insert(id);
        }
    }
    ASSERT_FALSE(preLaunchCompletions.empty());

    std::size_t replicas = 0;
    for (const auto& [id, step] : scheduled.output->steps()) {
        (void)id;
        if (step.operation != std::optional<NodeId>(control)) continue;
        ++replicas;
        const bool gated = std::any_of(
            step.dependencies.begin(), step.dependencies.end(),
            [&](ScheduleStepId dependency) {
                return preLaunchCompletions.count(dependency) != 0;
            });
        EXPECT_TRUE(gated);
    }
    EXPECT_EQ(replicas, 2u);
}

TEST(GraphPassTest, ScheduleGraphModelsWhileSplitDecisionAndAck) {
    auto root = GraphRegion::createRoot();
    GraphScalar flag =
        root->scalar(ScalarType::U32, "flag");
    auto body = root->createChild();
    body->addKernel(
        KernelDescriptor{"host", DeviceType::CPU,
                         std::nullopt, {}},
        {}, "cpu");
    body->addKernel(
        KernelDescriptor{"device", DeviceType::FPGA,
                         std::nullopt, {}},
        {}, "accel");
    LoopSpec loop;
    loop.kind = LoopKind::WhileCondition;
    loop.condition = Condition::compare(
        CompareOp::NE,
        ConditionOperand::scalar(ScalarType::U32, "flag", 0),
        ConditionOperand::constant<std::uint32_t>(0));
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());
    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities(),
        false);
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"accel", accelerator}};
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());
    std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>
        factories;
    factories[{DeviceType::CPU, DeviceType::FPGA}] =
        markerBridgeFactory();
    factories[{DeviceType::FPGA, DeviceType::CPU}] =
        markerBridgeFactory();
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(routed.ok());
    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());

    bool decision = false;
    bool acknowledged = false;
    for (const LogicalRendezvous& rendezvous :
         scheduled.output->rendezvous()) {
        if (rendezvous.purpose ==
            RendezvousPurpose::ControlDecision) {
            decision = true;
            EXPECT_EQ(rendezvous.scope,
                      RendezvousScope::PerIteration);
        }
        if (rendezvous.purpose ==
            RendezvousPurpose::ControlAcknowledged) {
            acknowledged = true;
            EXPECT_EQ(rendezvous.scope,
                      RendezvousScope::PerIteration);
        }
    }
    EXPECT_TRUE(decision);
    EXPECT_TRUE(acknowledged);
}

TEST(GraphPassTest, BackendResourceLeasesDoNotCollideAcrossLivePlans) {
    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"accel", accelerator}};

    std::vector<LogicalResourceRequirement> requirements{
        {RendezvousId(0), {DeviceId("cpu"), DeviceId("accel")}},
        {RendezvousId(1), {DeviceId("cpu"), DeviceId("accel")}},
    };
    ScheduledGraph scheduled(
        std::shared_ptr<const RoutedGraph>{}, {}, {}, {},
        requirements);

    CompileResult<BackendResourceBindings> first =
        bindBackendResources(scheduled, devices);
    CompileResult<BackendResourceBindings> second =
        bindBackendResources(scheduled, devices);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());

    std::set<std::uint32_t> firstPhysical;
    for (const auto& [logical, binding] :
         first.output->rendezvous()) {
        (void)logical;
        EXPECT_EQ(binding.kind,
                  PhysicalRendezvousKind::DeviceResource);
        firstPhysical.insert(binding.physicalIndex);
    }
    for (const auto& [logical, binding] :
         second.output->rendezvous()) {
        (void)logical;
        EXPECT_EQ(firstPhysical.count(binding.physicalIndex), 0u);
    }

    first.output.reset();
    CompileResult<BackendResourceBindings> third =
        bindBackendResources(scheduled, devices);
    ASSERT_TRUE(third.ok());
    std::set<std::uint32_t> thirdPhysical;
    for (const auto& [logical, binding] :
         third.output->rendezvous()) {
        (void)logical;
        thirdPhysical.insert(binding.physicalIndex);
    }
    EXPECT_EQ(thirdPhysical, firstPhysical);
}

TEST(GraphPassTest, BackendAndScheduledOperationPlacementsNormalizeEqually) {
    Graph graph;
    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    graph.registerDevice(host);
    GraphScalar trips =
        graph.rootRegion().scalar(ScalarType::I32, "trips");
    auto body = graph.rootRegion().createChild();
    body->addKernel(
        KernelDescriptor{"body", DeviceType::CPU,
                         std::nullopt, {}},
        {}, "cpu");
    LoopSpec loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    graph.addLoop(std::move(loop));

    CompiledGraph compiled = graph.compile();
    ASSERT_NE(compiled.scheduledGraph(), nullptr);
    CompileResult<ResolvedGraph> resolved = resolveGraph(
        AuthoredGraph::snapshot(graph.rootRegion()));
    ASSERT_TRUE(resolved.ok());
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(graph.devices()));
    ASSERT_TRUE(placed.ok());
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(
            graph.devices(), graph.bridgeFactories()));
    ASSERT_TRUE(routed.ok());
    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());

    const SemanticPlacementPlan backendPlan =
        normalizeOperationPlacements(compiled.dgraphs());
    const SemanticPlacementPlan scheduledPlan =
        normalizeOperationPlacements(*scheduled.output);
    EXPECT_EQ(backendPlan, scheduledPlan)
        << backendPlan.toString() << "\n---\n"
        << scheduledPlan.toString();
}
