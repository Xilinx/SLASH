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
#include <memory>
#include <map>
#include <optional>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <variant>
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

class PlacementPlan : public IBackendExecutable {
   public:
    PlacementPlan(QueueId queue, DeviceId device)
        : queue_(queue), device_(std::move(device)) {}
    QueueId queue() const override { return queue_; }
    DeviceId device() const override { return device_; }
    void launch() override {}
    void wait() override {}

   private:
    QueueId queue_;
    DeviceId device_;
};

struct PlacementResourceState {
    std::mutex              mutex;
    std::set<std::uint32_t> used;
};

class PlacementResourceLease : public IDeviceResourceLease {
   public:
    PlacementResourceLease(
        std::shared_ptr<PlacementResourceState> state,
        std::map<RendezvousId, BackendResourceId> rendezvous,
        std::map<ScalarResourceId, BackendScalarId> scalars)
        : state_(std::move(state)),
          rendezvous_(std::move(rendezvous)),
          scalars_(std::move(scalars)) {}

    ~PlacementResourceLease() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (const auto& [logical, physical] : rendezvous_) {
            (void)logical;
            state_->used.erase(
                static_cast<std::uint32_t>(physical.value()));
        }
        for (const auto& [logical, physical] : scalars_) {
            (void)logical;
            state_->used.erase(
                static_cast<std::uint32_t>(physical.value()));
        }
    }

    BackendResourceId rendezvousResource(
        RendezvousId logical) const override {
        return rendezvous_.at(logical);
    }

    BackendScalarId scalarResource(
        ScalarResourceId logical) const override {
        return scalars_.at(logical);
    }

   private:
    std::shared_ptr<PlacementResourceState> state_;
    std::map<RendezvousId, BackendResourceId> rendezvous_;
    std::map<ScalarResourceId, BackendScalarId> scalars_;
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
    leaseResources(
        const std::vector<RendezvousId>& rendezvous,
        const std::vector<ScalarResourceId>& scalars) override {
        std::map<RendezvousId, BackendResourceId>
            rendezvousResources;
        std::map<ScalarResourceId, BackendScalarId>
            scalarResources;
        std::lock_guard<std::mutex> lock(resourceState_->mutex);
        for (RendezvousId id : rendezvous) {
            std::uint32_t physical = 0;
            while (resourceState_->used.count(physical) != 0) {
                ++physical;
            }
            resourceState_->used.insert(physical);
            rendezvousResources[id] =
                BackendResourceId(physical);
        }
        for (ScalarResourceId id : scalars) {
            std::uint32_t physical = 0;
            while (resourceState_->used.count(physical) != 0) {
                ++physical;
            }
            resourceState_->used.insert(physical);
            scalarResources[id] = BackendScalarId(physical);
        }
        return std::make_unique<PlacementResourceLease>(
            resourceState_, std::move(rendezvousResources),
            std::move(scalarResources));
    }

    std::unique_ptr<IBackendExecutable> lowerQueue(
        const BackendLoweringContext& context) override {
        return std::make_unique<PlacementPlan>(
            context.queue.id, context.queue.device);
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

DeviceCapabilities gpuFollowerCapabilities() {
    DeviceCapabilities capabilities;
    capabilities.backend = "test_gpu";
    capabilities.kernelTypes.insert(DeviceType::GPU);
    capabilities.supportsSplitFollower = true;
    capabilities.prefersSplitPrimary = true;
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

const TransferRoute* scheduledRoute(
    const ScheduledGraph& graph, const ScheduledStep& step) {
    const RouteId* route = std::visit(
        [](const auto& payload) -> const RouteId* {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (
                std::is_same_v<T, ScheduledTransferProduce> ||
                std::is_same_v<T, ScheduledTransferConsume> ||
                std::is_same_v<T, ScheduledTransferAction>) {
                return &payload.route;
            }
            return nullptr;
        },
        step.payload);
    if (!route) return nullptr;
    auto found = std::find_if(
        graph.routed().routes().begin(),
        graph.routed().routes().end(),
        [&](const TransferRoute& candidate) {
            return candidate.id == *route;
        });
    return found == graph.routed().routes().end()
               ? nullptr
               : &*found;
}

bool isPreLaunchStep(
    const ScheduledGraph& graph, const ScheduledStep& step) {
    if (const TransferRoute* route = scheduledRoute(graph, step)) {
        return route->requirement.signature.phase ==
               TransferPhase::PreLaunch;
    }
    const RendezvousId* id = std::visit(
        [](const auto& payload) -> const RendezvousId* {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (
                std::is_same_v<T, ScheduledEventPublish> ||
                std::is_same_v<T, ScheduledEventWait>) {
                return &payload.rendezvous;
            }
            return nullptr;
        },
        step.payload);
    if (!id) return false;
    auto rendezvous = std::find_if(
        graph.rendezvous().begin(), graph.rendezvous().end(),
        [&](const LogicalRendezvous& candidate) {
            return candidate.id == *id;
        });
    if (rendezvous == graph.rendezvous().end()) return false;
    return std::visit(
        [](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (
                std::is_same_v<T, DataReadyRendezvous> ||
                std::is_same_v<T, DataConsumedRendezvous>) {
                return payload.phase == TransferPhase::PreLaunch;
            }
            return false;
        },
        rendezvous->payload);
}

AuthoredGraph nestedSnapshot() {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar trips = root->scalar(ScalarType::I32, "trips");
    auto body = root->createChild();
    body->addKernel(cpuKernel("body"), {}, "cpu");

    ::vrt::graph::detail::LoopRecord loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));
    return AuthoredGraph::snapshot(*root);
}

}  // namespace

TEST(GraphPassTest, CompilerIdsAreStrongTypes) {
    static_assert(!std::is_same_v<NodeId, RegionId>);
    static_assert(!std::is_same_v<RegionId, AuthoredScopeId>);
    static_assert(!std::is_same_v<ValueId, ReplicaId>);
    static_assert(!std::is_same_v<RouteId, TransferLegId>);
    static_assert(!std::is_same_v<PortName, DeviceId>);
    static_assert(!std::is_same_v<DeviceId, MemoryRegionId>);

    EXPECT_EQ(NodeId(7).value(), 7u);
    EXPECT_EQ(DeviceId("cpu").value(), "cpu");
    EXPECT_LT(NodeId(2), NodeId(3));
}

TEST(GraphPassTest, ScheduledStepPayloadIsClosedAndExhaustive) {
    static_assert(
        std::variant_size_v<ScheduledStepPayload> == 9);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<
                      0, ScheduledStepPayload>,
                  ScheduledOperation>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<
                      1, ScheduledStepPayload>,
                  ScheduledTransferProduce>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<
                      2, ScheduledStepPayload>,
                  ScheduledTransferConsume>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<
                      3, ScheduledStepPayload>,
                  ScheduledTransferAction>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<
                      4, ScheduledStepPayload>,
                  ScheduledEventPublish>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<
                      5, ScheduledStepPayload>,
                  ScheduledEventWait>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<
                      6, ScheduledStepPayload>,
                  ScheduledGraphInput>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<
                      7, ScheduledStepPayload>,
                  ScheduledGraphOutput>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<
                      8, ScheduledStepPayload>,
                  ScheduledBoundaryMaterialization>);

    ScheduledStepPayload payload =
        ScheduledTransferAction{RouteId(3), TransferLegId(7)};
    const auto* action =
        std::get_if<ScheduledTransferAction>(&payload);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->route, RouteId(3));
    EXPECT_EQ(action->leg, TransferLegId(7));
}

TEST(GraphPassTest, ControlPlacementVariantsPreserveInvariants) {
    static_assert(!std::is_default_constructible_v<ControlPlacement>);
    static_assert(
        !std::is_constructible_v<SplitControlPlacement, DeviceId>);

    ControlPlacement host =
        HostControlPlacement(DeviceId("cpu"));
    ControlPlacement autonomous =
        AutonomousControlPlacement(DeviceId("accel"));
    ControlPlacement split = SplitControlPlacement(
        DeviceId("cpu"), {DeviceId("accel")},
        DeviceId("accel"));

    EXPECT_NE(std::get_if<HostControlPlacement>(&host), nullptr);
    EXPECT_NE(
        std::get_if<AutonomousControlPlacement>(&autonomous), nullptr);
    const auto* splitRecord =
        std::get_if<SplitControlPlacement>(&split);
    ASSERT_NE(splitRecord, nullptr);
    EXPECT_EQ(splitRecord->authority(), DeviceId("cpu"));
    EXPECT_EQ(
        splitRecord->followers(),
        (std::vector<DeviceId>{DeviceId("accel")}));
    EXPECT_EQ(
        splitRecord->participants(),
        (std::vector<DeviceId>{DeviceId("accel"), DeviceId("cpu")}));
    EXPECT_EQ(controlPrimary(split), DeviceId("accel"));
    EXPECT_THROW(
        (SplitControlPlacement(
            DeviceId("cpu"), {}, DeviceId("cpu"))),
        std::invalid_argument);
    EXPECT_THROW(
        (SplitControlPlacement(
            DeviceId("cpu"), {DeviceId("accel")},
            DeviceId("other"))),
        std::invalid_argument);
}

TEST(GraphPassTest, AuthoredSnapshotOwnsNestedStructure) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar trips = root->scalar(ScalarType::I32, "trips");
    auto body = root->createChild();
    body->addKernel(cpuKernel("before_snapshot"), {}, "cpu");

    ::vrt::graph::detail::LoopRecord loop;
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
    EXPECT_NE(firstLoop.body->sourceScope,
              secondLoop.body->sourceScope);
    EXPECT_EQ(firstLoop.body->id, secondLoop.body->id);
    EXPECT_EQ(authoredNodeId(firstLoop.body->operations.front()),
              authoredNodeId(secondLoop.body->operations.front()));
}

TEST(GraphPassTest, AuthoredSnapshotBuildsOneSharedRegionTreeIndex) {
    const AuthoredGraph authored = nestedSnapshot();
    const AuthoredLoop& loop =
        std::get<AuthoredLoop>(authored.root().operations.front());
    ASSERT_NE(loop.body, nullptr);

    const RegionTreeIndex& index = authored.index();
    EXPECT_EQ(index.findRegion(authored.root().id), &authored.root());
    EXPECT_EQ(index.findOperation(loop.id),
              &authored.root().operations.front());
    EXPECT_EQ(index.regionForScope(loop.body->sourceScope),
              std::optional<RegionId>(loop.body->id));
    EXPECT_EQ(index.parentControl(loop.body->id),
              std::optional<NodeId>(loop.id));
    ASSERT_EQ(index.children(authored.root().id).size(), 1u);
    EXPECT_EQ(index.children(authored.root().id).front().role,
              AuthoredChildRole::LoopBody);

    const AuthoredGraph copy = authored;
    EXPECT_EQ(&copy.index(), &authored.index());
}

TEST(GraphPassTest, SnapshotResolvesKnownAfterDependencies) {
    auto root = detail::AuthoringRegion::createRoot();
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
    Graph graph = Graph::withDefaults();
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
    auto root = detail::AuthoringRegion::createRoot();
    IOTypeMap type;
    type.inputs.push_back({"required", BufferType::I32});
    root->addKernel(
        KernelDescriptor{"broken", DeviceType::CPU,
                         std::nullopt, type},
        {}, "cpu");
    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_FALSE(resolved.ok());
    ASSERT_FALSE(resolved.diagnostics.entries().empty());
    EXPECT_EQ(resolved.diagnostics.entries().front().code,
              DiagCode::UnboundPort);
}

TEST(GraphPassTest, ResolveGraphBuildsTypedValuesAndDependencies) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    IOTypeMap type;
    type.inputs.push_back({"in", BufferType::I32});
    type.outputs.push_back({"out", BufferType::I32});
    detail::PortBindings io;
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
    EXPECT_EQ(valueProducer(*graphOutput),
              std::optional<NodeId>(operation->id));
    EXPECT_EQ(valueDefinition(*graphOutput),
              ValueDefinitionKind::OperationOutput);
    ASSERT_NE(resolvedBufferToken(*graphOutput), nullptr);
    EXPECT_EQ(resolvedBufferToken(*graphOutput)->name(), "output");
    EXPECT_TRUE(graphOutput->size.has_value());
}

TEST(GraphPassTest, ResolveGraphRecordsTypedInoutPairs) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);
    IOTypeMap type =
        IOTypeMap{}.inout<std::int32_t>("data");
    detail::PortBindings io;
    io.bindExistingInout("data", "data", input, output);
    root->addKernel(
        KernelDescriptor{"mutate", DeviceType::CPU,
                         std::nullopt, type},
        std::move(io), "cpu");

    CompileResult<ResolvedGraph> result =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.output->inouts().size(), 1u);
    const ResolvedInoutBinding& inout =
        result.output->inouts().front();
    EXPECT_EQ(inout.port, PortName("data"));
    EXPECT_NE(inout.input, inout.output);
}

TEST(GraphPassTest, ResolveGraphReportsPortAndDependencyDiagnostics) {
    auto root = detail::AuthoringRegion::createRoot();
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
    auto root = detail::AuthoringRegion::createRoot();
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
    detail::PortBindings io;
    io.bindInputScalar("in", local)
        .bindOutputScalar("out", next);
    const std::string kernel = body->addKernel(
        KernelDescriptor{"increment", DeviceType::CPU,
                         std::nullopt, type},
        std::move(io), "cpu", {start});
    body->exportToParent({{next, parent}}, {kernel});

    ::vrt::graph::detail::LoopRecord loop;
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
    EXPECT_EQ(control.port, PortName("counter"));
    EXPECT_NE(control.incoming[0].value,
              control.incoming[1].value);
    ASSERT_EQ(result.output->aliases().entries().size(), 1u);
    const BoundaryAlias& alias =
        result.output->aliases().entries().front();
    EXPECT_EQ(result.output->aliases().canonical(alias.target),
              alias.source);
}

TEST(GraphPassTest, ResolveGraphDrainsImageBeforeReprogramming) {
    auto root = detail::AuthoringRegion::createRoot();
    ::vrt::graph::detail::ReprogramRecord loadA;
    loadA.imageId = "imageA";
    loadA.pdiPath = "imageA.pdi";
    loadA.device = "accel";
    const std::string reprogramA =
        root->addReprogram(std::move(loadA));

    KernelDescriptor kernel{
        "kernelA", DeviceType::FPGA, "imageA", {}};
    const std::string dispatch =
        root->addKernel(std::move(kernel), {}, "accel", {reprogramA});

    ::vrt::graph::detail::ReprogramRecord loadB;
    loadB.imageId = "imageB";
    loadB.pdiPath = "imageB.pdi";
    loadB.device = "accel";
    loadB.afterOps = {reprogramA};
    root->addReprogram(std::move(loadB));

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
    ASSERT_TRUE(resolved.ok());
    const NodeId reprogramB =
        authoredNodeId(authored.root().operations[2]);
    const ResolvedOperation* operation =
        resolved.output->findOperation(reprogramB);
    ASSERT_NE(operation, nullptr);
    EXPECT_NE(std::find_if(
                  operation->dependencies.begin(),
                  operation->dependencies.end(),
                  [&](const ResolvedDependency& dependency) {
                      return dependency.predecessor ==
                                 authoredNodeId(
                                     authored.root().operations[1]) &&
                             dependency.reason ==
                                 DependencyReason::ReprogramDrain;
                  }),
              operation->dependencies.end())
        << "the next reprogram must drain " << dispatch;

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
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, {}));
    ASSERT_TRUE(routed.ok());
    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());

    std::optional<ScheduleStepId> kernelStep;
    std::optional<ScheduleStepId> reprogramStep;
    for (const auto& [id, step] : scheduled.output->steps()) {
        const auto* operation =
            std::get_if<ScheduledOperation>(&step.payload);
        if (!operation) continue;
        if (operation->operation ==
            authoredNodeId(authored.root().operations[1])) {
            kernelStep = id;
        }
        if (operation->operation == reprogramB) {
            reprogramStep = id;
        }
    }
    ASSERT_TRUE(kernelStep.has_value());
    ASSERT_TRUE(reprogramStep.has_value());
    EXPECT_NE(std::find(
                  scheduled.output->steps()
                      .at(*reprogramStep)
                      .dependencies.begin(),
                  scheduled.output->steps()
                      .at(*reprogramStep)
                      .dependencies.end(),
                  *kernelStep),
              scheduled.output->steps()
                  .at(*reprogramStep)
                  .dependencies.end());
}

TEST(GraphPassTest, PlaceGraphUsesCapabilitiesAndMemoryRegions) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    IOTypeMap type;
    type.inputs.push_back({"in", BufferType::I32});
    type.outputs.push_back({"out", BufferType::I32});
    detail::PortBindings io;
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
        const ValueReplica* replica =
            placed.output->findReplica(port.replica);
        ASSERT_NE(replica, nullptr);
        ASSERT_TRUE(replica->memory.region.has_value());
        regions.insert(replica->memory.region->value());
    }
    EXPECT_EQ(regions,
              (std::set<std::string>{"HBM[0]", "HBM[1]"}));
}

TEST(GraphPassTest, SameDeviceControlHintPreservesInferredHbmRegion) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);
    IOTypeMap outputType =
        IOTypeMap{}.out<std::int32_t>("out");
    auto makeBranch = [&](const std::string& name) {
        auto branch = root->createChild();
        GraphBuffer local =
            branch->buffer(BufferType::I32, name, size);
        detail::PortBindings io;
        io.bindExistingOutput("out", local);
        branch->addKernel(
            KernelDescriptor{
                name, DeviceType::FPGA, std::nullopt, outputType},
            std::move(io), "accel");
        return branch;
    };

    detail::PortBindings controlIo;
    controlIo.bindExistingOutput("out", output);
    detail::ConditionalRecord conditional;
    conditional.ioType = outputType;
    conditional.ioMap = std::move(controlIo);
    conditional.condition = Condition::alwaysTrue();
    conditional.thenRegion = makeBranch("then_value");
    conditional.elseRegion = makeBranch("else_value");
    conditional.outputPlacement.buffers["out"] = "accel";
    root->addConditional(std::move(conditional));

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());
    ASSERT_EQ(resolved.output->controlResults().size(), 1u);
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities(),
        true);
    accelerator->setRegion("out", "HBM[3]");
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output, placementCatalog(accelerator));
    ASSERT_TRUE(placed.ok());

    const ValueReplica* result = placed.output->primaryReplica(
        resolved.output->controlResults().front().result);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->memory.device, DeviceId("accel"));
    EXPECT_EQ(
        result->memory.region,
        std::optional<MemoryRegionId>(
            MemoryRegionId("HBM[3]")));
}

TEST(GraphPassTest, PlaceGraphMaterializesHbmFanoutReplicas) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    IOTypeMap type;
    type.inputs.push_back({"first", BufferType::I32});
    type.inputs.push_back({"second", BufferType::I32});
    detail::PortBindings io;
    io.bindInput("first", input).bindInput("second", input);
    root->addKernel(
        KernelDescriptor{"fanout", DeviceType::FPGA,
                         std::nullopt, type},
        std::move(io), "accel");

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    accelerator->setRegion("first", "HBM[0]");
    accelerator->setRegion("second", "HBM[1]");
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output, placementCatalog(accelerator));
    ASSERT_TRUE(placed.ok());

    ValueId inputValue;
    for (const auto& [id, value] : resolved.output->values()) {
        if (value.sourceName == "input" &&
            valueDefinition(value) ==
                ValueDefinitionKind::GraphInput) {
            inputValue = id;
        }
    }
    std::set<std::pair<std::string, std::string>> locations;
    for (const auto& [id, replica] : placed.output->replicas()) {
        (void)id;
        if (replica.value != inputValue) continue;
        locations.insert({
            replica.memory.device.value(),
            replica.memory.region
                ? replica.memory.region->value()
                : "-"});
    }
    EXPECT_EQ(
        locations,
        (std::set<std::pair<std::string, std::string>>{
            {"cpu", "-"}, {"accel", "HBM[0]"},
            {"accel", "HBM[1]"}}));
    ASSERT_EQ(placed.output->portPlacements().size(), 2u);
    EXPECT_NE(
        placed.output->portPlacements()[0].replica,
        placed.output->portPlacements()[1].replica);
}

TEST(GraphPassTest, PlaceGraphOwnsBoundaryMappingsPerDevice) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphScalar trips =
        root->inputScalar(ScalarType::I32, "trips");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    auto body = root->createChild();
    GraphBuffer local =
        body->inputBuffer(BufferType::I32, "local", size);
    body->importFromParent(
        std::vector<BufferBoundaryMapping>{{input, local}});

    IOTypeMap type;
    type.inputs.push_back({"in", BufferType::I32});
    detail::PortBindings cpuIo;
    cpuIo.bindInput("in", local);
    body->addKernel(
        KernelDescriptor{"cpu_consumer", DeviceType::CPU,
                         std::nullopt, type},
        std::move(cpuIo), "cpu");
    detail::PortBindings fpgaIo;
    fpgaIo.bindInput("in", local);
    body->addKernel(
        KernelDescriptor{"fpga_consumer", DeviceType::FPGA,
                         std::nullopt, type},
        std::move(fpgaIo), "accel");

    ::vrt::graph::detail::LoopRecord loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

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

    ASSERT_EQ(placed.output->boundaryMappings().size(), 2u);
    std::set<DeviceId> owners;
    for (const BoundaryMappingPlacement& mapping :
         placed.output->boundaryMappings()) {
        owners.insert(mapping.owner);
        const ValueReplica* source =
            placed.output->findReplica(mapping.source);
        const ValueReplica* target =
            placed.output->findReplica(mapping.target);
        ASSERT_NE(source, nullptr);
        ASSERT_NE(target, nullptr);
        EXPECT_EQ(source->memory, target->memory);
        EXPECT_EQ(source->memory.device, mapping.owner);
        EXPECT_NE(source->value, target->value);
    }
    EXPECT_EQ(
        owners,
        (std::set<DeviceId>{
            DeviceId("accel"), DeviceId("cpu")}));
}

TEST(GraphPassTest, PlaceGraphChoosesAutonomousControlPostOrder) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar trips =
        root->scalar(ScalarType::I32, "trips");
    auto body = root->createChild();
    body->addKernel(
        KernelDescriptor{"body", DeviceType::FPGA,
                         std::nullopt, {}},
        {}, "accel");
    ::vrt::graph::detail::LoopRecord loop;
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
    const auto* autonomous =
        std::get_if<AutonomousControlPlacement>(&placement);
    ASSERT_NE(autonomous, nullptr);
    EXPECT_EQ(autonomous->device(), DeviceId("accel"));
    ASSERT_EQ(autonomous->participants().size(), 1u);
}

TEST(GraphPassTest, PlaceGraphRecordsAutonomousRejectionAndFallsBack) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar trips =
        root->scalar(ScalarType::I32, "trips");
    auto body = root->createChild();
    body->addKernel(
        KernelDescriptor{"body", DeviceType::FPGA,
                         std::nullopt, {}},
        {}, "accel");
    ::vrt::graph::detail::LoopRecord loop;
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
    const auto* host =
        std::get_if<HostControlPlacement>(&placement);
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(host->host(), DeviceId("cpu"));
    ASSERT_EQ(host->rejections().size(), 1u);
    EXPECT_EQ(host->rejections().front().reason,
              "test rejection");
}

TEST(GraphPassTest, PlaceGraphSplitsAcrossAuthorityAndFollower) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar trips =
        root->scalar(ScalarType::I32, "trips");
    auto body = root->createChild();
    const std::string hostBody = body->addKernel(
        KernelDescriptor{"host_body", DeviceType::CPU,
                         std::nullopt, {}},
        {}, "cpu");
    body->addKernel(
        KernelDescriptor{"device_body", DeviceType::FPGA,
                         std::nullopt, {}},
        {}, "accel", {hostBody});
    ::vrt::graph::detail::LoopRecord loop;
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
    const auto* split =
        std::get_if<SplitControlPlacement>(&placement);
    ASSERT_NE(split, nullptr);
    EXPECT_EQ(split->primary(), DeviceId("accel"));
    EXPECT_EQ(
        split->participants(),
        (std::vector<DeviceId>{DeviceId("accel"), DeviceId("cpu")}));

    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"accel", accelerator}};
    std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>
        factories;
    factories[{DeviceType::CPU, DeviceType::FPGA}] =
        markerBridgeFactory();
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(routed.ok());
    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());

    ASSERT_EQ(scheduled.output->splitControls().size(), 1u);
    const SplitControlProtocol& protocol =
        scheduled.output->splitControls().front();
    EXPECT_EQ(protocol.control, control);
    ASSERT_EQ(protocol.followers.size(), 1u);
    const SplitControlFollowerProtocol& follower =
        protocol.followers.front();
    EXPECT_NE(protocol.authorityQueue, follower.queue);
    EXPECT_NE(follower.value, follower.decision);
    EXPECT_NE(follower.value, follower.acknowledgement);
    EXPECT_NE(follower.decision, follower.acknowledgement);

    const RegionId controlRegion =
        placed.output->resolved().findOperation(control)->region;
    ASSERT_EQ(
        authored.index().children(controlRegion).size(), 1u);
    const RegionId childRegion =
        authored.index().children(controlRegion).front().region;
    std::set<std::pair<RegionId, DeviceId>> queues;
    for (const QueueProgram& queue :
         scheduled.output->queues()) {
        queues.insert({queue.region, queue.device});
    }
    for (const DeviceId& participant : split->participants()) {
        EXPECT_EQ(queues.count({controlRegion, participant}), 1u);
        EXPECT_EQ(queues.count({childRegion, participant}), 1u);
    }
    EXPECT_EQ(
        queues.count({authored.root().id, DeviceId("cpu")}), 1u);

    bool perIteration = false;
    for (const LogicalRendezvous& rendezvous :
         scheduled.output->rendezvous()) {
        std::visit(
            [&](const auto& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (
                    std::is_same_v<T, DataReadyRendezvous> ||
                    std::is_same_v<
                        T, DataConsumedRendezvous>) {
                    perIteration |=
                        payload.phase ==
                        TransferPhase::PerIteration;
                }
            },
            rendezvous.payload);
    }
    EXPECT_TRUE(perIteration);
}

TEST(GraphPassTest,
     RouteGraphPublishesSplitFpgaHbmControlResult) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphScalar trips =
        root->inputScalar(ScalarType::U32, "trips");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    auto body = root->createChild();
    GraphBuffer localInput =
        body->inputBuffer(BufferType::I32, "local_input", size);
    GraphBuffer localOutput =
        body->buffer(BufferType::I32, "local_output", size);
    body->importFromParent(
        std::vector<BufferBoundaryMapping>{{input, localInput}});
    IOTypeMap fpgaType;
    fpgaType.inputs.push_back({"body_in", BufferType::I32});
    fpgaType.outputs.push_back({"body_out", BufferType::I32});
    detail::PortBindings fpgaIo;
    fpgaIo.bindInput("body_in", localInput)
        .bindExistingOutput("body_out", localOutput);
    const std::string fpgaBody = body->addKernel(
        KernelDescriptor{"fpga_body", DeviceType::FPGA,
                         std::nullopt, fpgaType},
        std::move(fpgaIo), "accel");
    body->addKernel(
        KernelDescriptor{"cpu_body", DeviceType::CPU,
                         std::nullopt, {}},
        {}, "cpu");
    body->exportToParent(
        std::vector<BufferBoundaryMapping>{
            {localOutput, output}, {localOutput, input}},
        {fpgaBody});
    detail::LoopRecord loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    const NodeId control =
        authoredNodeId(authored.root().operations.front());
    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
    ASSERT_TRUE(resolved.ok());
    const ResolvedControlResult* outputResult = nullptr;
    for (const ResolvedControlResult& result :
         resolved.output->controlResults()) {
        const ResolvedValue* value =
            resolved.output->findValue(result.result);
        const GraphBuffer* token =
            value ? resolvedBufferToken(*value) : nullptr;
        if (token && token->name() == output.name()) {
            outputResult = &result;
        }
    }
    ASSERT_NE(outputResult, nullptr);

    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    accelerator->setRegion("body_in", "HBM[0]");
    accelerator->setRegion("body_out", "HBM[0]");
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"accel", accelerator}};
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());
    ASSERT_NE(
        std::get_if<SplitControlPlacement>(
            &placed.output->controlPlacements().at(control)),
        nullptr);
    const ValueReplica* resultReplica =
        placed.output->primaryReplica(outputResult->result);
    ASSERT_NE(resultReplica, nullptr);
    EXPECT_EQ(resultReplica->memory.device, DeviceId("accel"));
    EXPECT_EQ(
        resultReplica->memory.region,
        std::optional<MemoryRegionId>(
            MemoryRegionId("HBM[0]")));

    const ControlIncoming* initial = nullptr;
    const ControlIncoming* backedge = nullptr;
    for (const ControlIncoming& incoming : outputResult->incoming) {
        if (incoming.arm == ControlArm::LoopInitial) {
            initial = &incoming;
        } else if (incoming.arm == ControlArm::LoopBackedge) {
            backedge = &incoming;
        }
    }
    ASSERT_NE(initial, nullptr);
    ASSERT_NE(backedge, nullptr);

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

    const TransferRoute* initialPublication = nullptr;
    bool backedgePublication = false;
    for (const DependencyEdge& edge : routed.output->dependencies()) {
        const auto* value = std::get_if<ValueDependencyEdge>(&edge);
        if (!value || value->consumer !=
                          std::optional<NodeId>(control)) {
            continue;
        }
        const ValueReplica* source =
            routed.output->findReplica(value->source);
        const ValueReplica* target =
            routed.output->findReplica(value->target);
        if (!source || !target ||
            target->value != outputResult->result) {
            continue;
        }
        if (source->value == initial->value && value->route) {
            auto route = std::find_if(
                routed.output->routes().begin(),
                routed.output->routes().end(),
                [&](const TransferRoute& candidate) {
                    return candidate.id == *value->route;
                });
            ASSERT_NE(route, routed.output->routes().end());
            initialPublication = &*route;
        }
        if (source->value == backedge->value) {
            backedgePublication = true;
            EXPECT_FALSE(value->route.has_value());
        }
    }
    ASSERT_NE(initialPublication, nullptr);
    EXPECT_EQ(
        initialPublication->requirement.signature.phase,
        TransferPhase::PreLaunch);
    EXPECT_EQ(
        initialPublication->requirement.signature.sourceLocation.device,
        DeviceId("cpu"));
    EXPECT_EQ(
        initialPublication->requirement.signature.destinationLocation.device,
        DeviceId("accel"));
    EXPECT_EQ(
        initialPublication->requirement.signature.destinationLocation.region,
        std::optional<MemoryRegionId>(
            MemoryRegionId("HBM[0]")));
    EXPECT_TRUE(backedgePublication);

    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());
    EXPECT_EQ(scheduled.output->splitControls().size(), 1u);
    EXPECT_TRUE(std::any_of(
        scheduled.output->steps().begin(),
        scheduled.output->steps().end(),
        [&](const auto& entry) {
            const auto* consume =
                std::get_if<ScheduledTransferConsume>(
                    &entry.second.payload);
            return consume &&
                   consume->route == initialPublication->id;
        }));
}

TEST(GraphPassTest, RouteGraphPlansCrossDeviceGraphIoWithoutClosures) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);
    IOTypeMap type;
    type.inputs.push_back({"in", BufferType::I32});
    type.outputs.push_back({"out", BufferType::I32});
    detail::PortBindings io;
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
    const TransferCapabilityCatalog capabilities =
        TransferCapabilityCatalog::fromGraph(devices, factories);
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output, capabilities);
    CompileResult<RoutedGraph> repeated = routeGraph(
        *placed.output, capabilities);
    ASSERT_TRUE(routed.ok());
    ASSERT_TRUE(repeated.ok());
    ASSERT_EQ(routed.output->routes().size(), 2u);
    ASSERT_EQ(
        repeated.output->routes().size(),
        routed.output->routes().size());
    std::set<TransferPhase> phases;
    for (std::size_t i = 0; i < routed.output->routes().size();
         ++i) {
        const TransferRoute& route = routed.output->routes()[i];
        const TransferRoute& repeatedRoute =
            repeated.output->routes()[i];
        EXPECT_EQ(route.id, repeatedRoute.id);
        EXPECT_EQ(
            route.requirement.signature,
            repeatedRoute.requirement.signature);
        EXPECT_EQ(route.legs.size(), repeatedRoute.legs.size());
        ASSERT_EQ(route.legs.size(), 1u);
        EXPECT_EQ(route.legs.front().mechanism,
                  TransferMechanism::DirectBridge);
        EXPECT_EQ(
            route.legs.front().id,
            repeatedRoute.legs.front().id);
        EXPECT_EQ(
            route.legs.front().id.value(),
            route.id.value() << 32);
        EXPECT_NE(
            std::get_if<HostTransferExecutor>(
                &route.legs.front().executor),
            nullptr);
        EXPECT_EQ(
            route.requirement.completion,
            TransferCompletionProtocol::
                ProducerConsumerAcknowledged);
        const std::optional<ReplicaId> source = transferReplica(
            route.requirement.signature.source);
        const std::optional<ReplicaId> destination =
            transferReplica(
                route.requirement.signature.destination);
        ASSERT_TRUE(source.has_value());
        ASSERT_TRUE(destination.has_value());
        EXPECT_NE(routed.output->findReplica(*source), nullptr);
        EXPECT_NE(
            routed.output->findReplica(*destination), nullptr);
        phases.insert(route.requirement.signature.phase);
    }
    EXPECT_EQ(
        phases,
        (std::set<TransferPhase>{
            TransferPhase::PreLaunch, TransferPhase::Once}));
}

TEST(GraphPassTest, RouteGraphSharesRootInputScalarsWithoutTransfers) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar count =
        root->inputScalar(ScalarType::U32, "count");
    IOTypeMap type;
    type.inputScalars.push_back({"count", ScalarType::U32});
    detail::PortBindings io;
    io.bindInputScalar("count", count);
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
    EXPECT_TRUE(routed.output->routes().empty());
}

TEST(GraphPassTest, ScheduleGraphRunsPreLaunchHbmCopyOnHost) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size = root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);

    IOTypeMap firstType;
    firstType.inputs.push_back({"first", BufferType::I32});
    detail::PortBindings firstIo;
    firstIo.bindInput("first", input);
    root->addKernel(
        KernelDescriptor{"first", DeviceType::FPGA,
                         std::nullopt, firstType},
        std::move(firstIo), "accel");

    IOTypeMap secondType;
    secondType.inputs.push_back({"second", BufferType::I32});
    detail::PortBindings secondIo;
    secondIo.bindInput("second", input);
    root->addKernel(
        KernelDescriptor{"second", DeviceType::FPGA,
                         std::nullopt, secondType},
        std::move(secondIo), "accel");

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());
    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    accelerator->setRegion("first", "HBM[0]");
    accelerator->setRegion("second", "HBM[1]");
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

    const TransferRoute* copy = nullptr;
    for (const TransferRoute& route : routed.output->routes()) {
        if (!route.legs.empty() &&
            route.legs.front().mechanism ==
                TransferMechanism::HostMediatedDeviceCopy) {
            copy = &route;
        }
    }
    ASSERT_NE(copy, nullptr);
    ASSERT_EQ(copy->requirement.prerequisites.size(), 1u);
    const RouteId prerequisiteId =
        copy->requirement.prerequisites.front();
    auto prerequisite = std::find_if(
        routed.output->routes().begin(),
        routed.output->routes().end(),
        [&](const TransferRoute& route) {
            return route.id == prerequisiteId;
        });
    ASSERT_NE(prerequisite, routed.output->routes().end());
    const std::optional<ReplicaId> staged =
        transferReplica(copy->requirement.signature.source);
    const std::optional<ReplicaId> target =
        transferReplica(copy->requirement.signature.destination);
    ASSERT_TRUE(staged.has_value());
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(
        staged,
        transferReplica(
            prerequisite->requirement.signature.destination));
    EXPECT_NE(placed.output->findReplica(*staged), nullptr);
    EXPECT_NE(placed.output->findReplica(*target), nullptr);
    EXPECT_NE(*staged, *target);
    EXPECT_EQ(
        copy->requirement.completion,
        TransferCompletionProtocol::ExecutorSignalsReady);
    ASSERT_EQ(copy->legs.size(), 1u);
    const auto* executor =
        std::get_if<HostTransferExecutor>(
            &copy->legs.front().executor);
    ASSERT_NE(executor, nullptr);
    EXPECT_EQ(executor->device, DeviceId("cpu"));

    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());
    std::size_t readyRendezvous = 0;
    for (const LogicalRendezvous& rendezvous :
         scheduled.output->rendezvous()) {
        if (const auto* ready =
                std::get_if<DataReadyRendezvous>(
                    &rendezvous.payload);
            ready && ready->route == copy->id) {
            ++readyRendezvous;
            EXPECT_EQ(
                ready->phase, TransferPhase::PreLaunch);
            EXPECT_NE(
                std::get_if<GraphTransferScope>(
                    &ready->scope),
                nullptr);
        }
    }
    EXPECT_EQ(readyRendezvous, 1u);
}

TEST(GraphPassTest, RouteGraphSelectsHostBounceDeclaratively) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer intermediate =
        root->buffer(BufferType::I32, "intermediate", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    IOTypeMap producerType;
    producerType.outputs.push_back({"out", BufferType::I32});
    detail::PortBindings producerIo;
    producerIo.bindExistingOutput("out", intermediate);
    root->addKernel(
        KernelDescriptor{"produce", DeviceType::GPU,
                         std::nullopt, producerType},
        std::move(producerIo), "gpu");

    IOTypeMap consumerType;
    consumerType.inputs.push_back({"in", BufferType::I32});
    consumerType.outputs.push_back({"out", BufferType::I32});
    detail::PortBindings consumerIo;
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
    EXPECT_NE(bounce->legs[0].id, bounce->legs[1].id);
    EXPECT_EQ(
        bounce->legs[0].id.value(),
        bounce->id.value() << 32);
    EXPECT_EQ(
        bounce->legs[1].id.value(),
        (bounce->id.value() << 32) | 1);
    for (const TransferLeg& leg : bounce->legs) {
        const auto* executor =
            std::get_if<HostTransferExecutor>(&leg.executor);
        ASSERT_NE(executor, nullptr);
        EXPECT_EQ(executor->device, DeviceId("cpu"));
    }
    EXPECT_EQ(
        bounce->requirement.completion,
        TransferCompletionProtocol::
            ProducerConsumerAcknowledged);
}

TEST(GraphPassTest, RouteGraphKeepsOrderBarriersTyped) {
    auto root = detail::AuthoringRegion::createRoot();
    const std::string first =
        root->addKernel(cpuKernel("first"), {}, "cpu");
    root->addKernel(
        KernelDescriptor{
            "second", DeviceType::FPGA, std::nullopt, {}},
        {}, "accel", {first});
    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    ASSERT_EQ(authored.root().operations.size(), 2u);
    const NodeId producer =
        authoredNodeId(authored.root().operations[0]);
    const NodeId consumer =
        authoredNodeId(authored.root().operations[1]);

    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
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

    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(routed.ok());
    ASSERT_EQ(routed.output->routes().size(), 1u);
    const TransferRoute& route = routed.output->routes().front();
    EXPECT_EQ(
        route.requirement.signature.payload,
        TransferPayloadKind::Barrier);
    const auto* source = std::get_if<BarrierTransferEndpoint>(
        &route.requirement.signature.source);
    const auto* destination =
        std::get_if<BarrierTransferEndpoint>(
            &route.requirement.signature.destination);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(destination, nullptr);
    EXPECT_EQ(source->operation, producer);
    EXPECT_EQ(destination->operation, consumer);
    EXPECT_FALSE(
        transferReplica(route.requirement.signature.source)
            .has_value());
    EXPECT_EQ(
        route.requirement.sourceAnchor.operation(),
        std::optional<NodeId>(producer));
    EXPECT_EQ(
        route.requirement.destinationAnchor.operation(),
        std::optional<NodeId>(consumer));
    ASSERT_EQ(routed.output->dependencies().size(), 1u);
    const auto* order = std::get_if<OrderDependencyEdge>(
        &routed.output->dependencies().front());
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->producer, producer);
    EXPECT_EQ(order->consumer, consumer);
    EXPECT_EQ(order->route, std::optional<RouteId>(route.id));
}

TEST(GraphPassTest, RouteGraphMakesSameDeviceRegionCopyExplicit) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer intermediate =
        root->buffer(BufferType::I32, "intermediate", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    IOTypeMap producerType;
    producerType.outputs.push_back({"produce_out", BufferType::I32});
    detail::PortBindings producerIo;
    producerIo.bindExistingOutput("produce_out", intermediate);
    root->addKernel(
        KernelDescriptor{"produce", DeviceType::FPGA,
                         std::nullopt, producerType},
        std::move(producerIo), "accel");

    IOTypeMap consumerType;
    consumerType.inputs.push_back({"consume_in", BufferType::I32});
    consumerType.outputs.push_back({"final_out", BufferType::I32});
    detail::PortBindings consumerIo;
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
    EXPECT_EQ(copy->requirement.signature.sourceLocation.region,
              std::optional<MemoryRegionId>(
                  MemoryRegionId("HBM[0]")));
    EXPECT_EQ(
        copy->requirement.signature.destinationLocation.region,
              std::optional<MemoryRegionId>(
                  MemoryRegionId("HBM[1]")));
}

TEST(GraphPassTest, RouteGraphRejectsCrossRegionInout) {
    auto root = detail::AuthoringRegion::createRoot();
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
    detail::PortBindings io;
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
    ASSERT_EQ(resolved.output->inouts().size(), 1u);
    const ResolvedInoutBinding& inout =
        resolved.output->inouts().front();
    const ValueReplica* outputReplica =
        placed.output->primaryReplica(inout.output);
    ASSERT_NE(outputReplica, nullptr);
    const ValueReplica* inputReplica = nullptr;
    for (const PortPlacement& port :
         placed.output->portPlacements()) {
        const ValueReplica* candidate =
            placed.output->findReplica(port.replica);
        if (port.operation == inout.operation && candidate &&
            candidate->value == inout.input) {
            inputReplica = candidate;
            break;
        }
    }
    ASSERT_NE(inputReplica, nullptr);
    EXPECT_NE(
        inputReplica->memory.region,
        outputReplica->memory.region);
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

TEST(GraphPassTest,
     RouteGraphIsolatesCpuAndFpgaInoutConsumers) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer shared =
        root->buffer(BufferType::I32, "shared", size);
    GraphBuffer cpuOutput =
        root->outputBuffer(BufferType::I32, "cpu_output", size);
    GraphBuffer fpgaOutput =
        root->outputBuffer(BufferType::I32, "fpga_output", size);

    IOTypeMap producerType =
        IOTypeMap{}.out<std::int32_t>("source");
    detail::PortBindings producerIo;
    producerIo.bindExistingOutput("source", shared);
    root->addKernel(
        KernelDescriptor{
            "produce", DeviceType::FPGA, std::nullopt,
            producerType},
        std::move(producerIo), "accel");

    IOTypeMap inoutType =
        IOTypeMap{}.inout<std::int32_t>("rw");
    detail::PortBindings cpuIo;
    cpuIo.bindExistingInout(
        "rw", "rw", shared, cpuOutput);
    root->addKernel(
        KernelDescriptor{
            "cpu_mutate", DeviceType::CPU, std::nullopt,
            inoutType},
        std::move(cpuIo), "cpu");
    detail::PortBindings fpgaIo;
    fpgaIo.bindExistingInout(
        "rw", "rw", shared, fpgaOutput);
    root->addKernel(
        KernelDescriptor{
            "fpga_mutate", DeviceType::FPGA, std::nullopt,
            inoutType},
        std::move(fpgaIo), "accel");

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
    ASSERT_TRUE(resolved.ok());
    ASSERT_EQ(resolved.output->inouts().size(), 2u);
    EXPECT_EQ(
        resolved.output->inouts()[0].input,
        resolved.output->inouts()[1].input);
    EXPECT_NE(
        resolved.output->inouts()[0].output,
        resolved.output->inouts()[1].output);

    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    accelerator->setRegion("source", "HBM[0]");
    accelerator->setRegion("rw", "HBM[0]");
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

    std::set<RouteId> consumerRoutes;
    std::set<ReplicaId> consumerTargets;
    std::set<TransferMechanism> mechanisms;
    for (const DependencyEdge& edge :
         routed.output->dependencies()) {
        const auto* value =
            std::get_if<ValueDependencyEdge>(&edge);
        if (!value || !value->consumer || !value->route) continue;
        const ValueReplica* target =
            routed.output->findReplica(value->target);
        if (!target) continue;
        const bool inoutConsumer = std::any_of(
            resolved.output->inouts().begin(),
            resolved.output->inouts().end(),
            [&](const ResolvedInoutBinding& inout) {
                return inout.operation == *value->consumer &&
                       inout.input == target->value;
            });
        if (!inoutConsumer) continue;
        consumerRoutes.insert(*value->route);
        consumerTargets.insert(value->target);
        auto route = std::find_if(
            routed.output->routes().begin(),
            routed.output->routes().end(),
            [&](const TransferRoute& candidate) {
                return candidate.id == *value->route;
            });
        ASSERT_NE(route, routed.output->routes().end());
        ASSERT_FALSE(route->legs.empty());
        mechanisms.insert(route->legs.front().mechanism);
    }
    EXPECT_EQ(consumerRoutes.size(), 2u);
    EXPECT_EQ(consumerTargets.size(), 2u);
    EXPECT_EQ(
        mechanisms,
        (std::set<TransferMechanism>{
            TransferMechanism::DirectBridge,
            TransferMechanism::HostMediatedDeviceCopy}));
}

TEST(GraphPassTest,
     RouteGraphRejectsUnrepresentableInoutCopyOnWrite) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer shared =
        root->buffer(BufferType::I32, "shared", size);
    GraphBuffer output =
        root->buffer(BufferType::I32, "output", size);
    IOTypeMap producerType =
        IOTypeMap{}.out<std::int32_t>("source");
    detail::PortBindings producerIo;
    producerIo.bindExistingOutput("source", shared);
    root->addKernel(
        KernelDescriptor{
            "produce", DeviceType::FPGA, std::nullopt,
            producerType},
        std::move(producerIo), "accel");
    IOTypeMap inoutType =
        IOTypeMap{}.inout<std::int32_t>("rw");
    detail::PortBindings inoutIo;
    inoutIo.bindExistingInout(
        "rw", "rw", shared, output);
    root->addKernel(
        KernelDescriptor{
            "mutate", DeviceType::FPGA, std::nullopt,
            inoutType},
        std::move(inoutIo), "accel");

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());
    DeviceCapabilities noCopies = acceleratorCapabilities();
    noCopies.supportsMemoryRegionCopies = false;
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, noCopies);
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output, placementCatalog(accelerator));
    ASSERT_TRUE(placed.ok());
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", std::make_shared<PlacementDevice>(
                    "cpu", DeviceType::CPU, hostCapabilities())},
        {"accel", accelerator}};
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, {}));
    ASSERT_FALSE(routed.ok());
    EXPECT_TRUE(std::any_of(
        routed.diagnostics.entries().begin(),
        routed.diagnostics.entries().end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.code ==
                       DiagCode::IncompatibleMemoryPlacement &&
                   diagnostic.message.find("copy-on-write") !=
                       std::string::npos;
        }));
}

TEST(GraphPassTest, ScheduleGraphUsesLogicalRendezvousAndValidSteps) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);
    IOTypeMap type;
    type.inputs.push_back({"in", BufferType::I32});
    type.outputs.push_back({"out", BufferType::I32});
    detail::PortBindings io;
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
    std::map<ScheduleStepId, std::size_t> indegree;
    std::map<ScheduleStepId, std::vector<ScheduleStepId>>
        successors;
    for (const auto& [id, step] : scheduled.output->steps()) {
        indegree[id] = step.dependencies.size();
        for (ScheduleStepId dependency : step.dependencies) {
            successors[dependency].push_back(id);
        }
    }
    std::set<ScheduleStepId> ready;
    for (const auto& [id, degree] : indegree) {
        if (degree == 0) ready.insert(id);
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
        const ScheduleStepId id = *ready.begin();
        ready.erase(ready.begin());
        ++visited;
        for (ScheduleStepId successor : successors[id]) {
            if (--indegree[successor] == 0) {
                ready.insert(successor);
            }
        }
    }
    EXPECT_EQ(visited, scheduled.output->steps().size());
    const bool hasPreLaunch = std::any_of(
        scheduled.output->steps().begin(),
        scheduled.output->steps().end(),
        [&](const auto& entry) {
            return isPreLaunchStep(
                *scheduled.output, entry.second);
        });
    EXPECT_TRUE(hasPreLaunch);
}

TEST(GraphPassTest, ScheduleGraphGatesSplitAuthorityOnInputStaging) {
    auto root = detail::AuthoringRegion::createRoot();
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
    detail::PortBindings fpgaIo;
    fpgaIo.bindInput("in", local);
    body->addKernel(
        KernelDescriptor{"fpga_body", DeviceType::FPGA,
                         std::nullopt, fpgaType},
        std::move(fpgaIo), "accel");
    body->addKernel(
        KernelDescriptor{"cpu_body", DeviceType::CPU,
                         std::nullopt, {}},
        {}, "cpu");
    ::vrt::graph::detail::LoopRecord loop;
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
    ASSERT_NE(
        std::get_if<SplitControlPlacement>(
            &placed.output->controlPlacements().at(control)),
        nullptr);
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
    ASSERT_EQ(scheduled.output->splitControls().size(), 1u);
    const SplitControlProtocol& protocol =
        scheduled.output->splitControls().front();

    std::set<ScheduleStepId> preLaunchAuthorityReady;
    for (const auto& [id, step] : scheduled.output->steps()) {
        if (isPreLaunchStep(*scheduled.output, step)) {
            EXPECT_EQ(step.region, authored.root().id)
                << "pre-launch staging must be outside the loop body";
        }
        if (isPreLaunchStep(*scheduled.output, step) &&
            step.queue == protocol.authorityQueue) {
            preLaunchAuthorityReady.insert(id);
        }
    }
    ASSERT_FALSE(preLaunchAuthorityReady.empty());

    std::size_t replicas = 0;
    std::size_t authorities = 0;
    for (const auto& [id, step] : scheduled.output->steps()) {
        const auto* operation =
            std::get_if<ScheduledOperation>(&step.payload);
        if (!operation || operation->operation != control) continue;
        ++replicas;
        const bool gated = std::any_of(
            step.dependencies.begin(), step.dependencies.end(),
            [&](ScheduleStepId dependency) {
                return preLaunchAuthorityReady.count(dependency) != 0;
            });
        if (id == protocol.authorityStep) {
            ++authorities;
            EXPECT_TRUE(gated);
        } else {
            EXPECT_FALSE(gated)
                << "FPGA launch uses synchronous pre-launch slot polling";
        }
    }
    EXPECT_EQ(replicas, 2u);
    EXPECT_EQ(authorities, 1u);
}

TEST(GraphPassTest,
     ScheduleGraphHoistsCpuGpuHostEventBeforeChildStart) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphScalar trips =
        root->inputScalar(ScalarType::U32, "trips");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);

    auto body = root->createChild();
    GraphBuffer local =
        body->inputBuffer(BufferType::I32, "local", size);
    body->importFromParent(
        std::vector<BufferBoundaryMapping>{{input, local}});
    IOTypeMap gpuType;
    gpuType.inputs.push_back({"in", BufferType::I32});
    detail::PortBindings gpuIo;
    gpuIo.bindInput("in", local);
    body->addKernel(
        KernelDescriptor{"gpu_body", DeviceType::GPU,
                         std::nullopt, gpuType},
        std::move(gpuIo), "gpu");
    body->addKernel(
        KernelDescriptor{"cpu_body", DeviceType::CPU,
                         std::nullopt, {}},
        {}, "cpu");
    detail::LoopRecord loop;
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
    auto gpu = std::make_shared<PlacementDevice>(
        "gpu", DeviceType::GPU, gpuFollowerCapabilities());
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"gpu", gpu}};
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());
    ASSERT_NE(
        std::get_if<SplitControlPlacement>(
            &placed.output->controlPlacements().at(control)),
        nullptr);

    std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>
        factories;
    factories[{DeviceType::CPU, DeviceType::GPU}] =
        markerBridgeFactory();
    factories[{DeviceType::GPU, DeviceType::CPU}] =
        markerBridgeFactory();
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(routed.ok());
    auto transfer = std::find_if(
        routed.output->routes().begin(),
        routed.output->routes().end(),
        [](const TransferRoute& route) {
            return route.requirement.signature.phase ==
                       TransferPhase::PreLaunch &&
                   route.requirement.signature.sourceLocation.device ==
                       DeviceId("cpu") &&
                   route.requirement.signature.destinationLocation.device ==
                       DeviceId("gpu");
        });
    ASSERT_NE(transfer, routed.output->routes().end());
    ASSERT_NE(
        std::get_if<HostEventSynchronization>(
            &transfer->requirement.synchronization),
        nullptr);
    ASSERT_EQ(transfer->legs.size(), 1u);
    EXPECT_EQ(transfer->legs.front().sourceRegion, authored.root().id);
    EXPECT_EQ(
        transfer->legs.front().destinationRegion,
        authored.root().id);
    EXPECT_EQ(
        transfer->legs.front().executorRegion,
        authored.root().id);
    EXPECT_EQ(
        transfer->requirement.controlPrerequisites,
        std::vector<NodeId>{control});

    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());
    const SplitControlProtocol& protocol =
        scheduled.output->splitControls().front();
    const ScheduledStep& authority =
        scheduled.output->steps().at(protocol.authorityStep);
    std::optional<ScheduleStepId> completion;
    for (const auto& [id, step] : scheduled.output->steps()) {
        const auto* consume =
            std::get_if<ScheduledTransferConsume>(&step.payload);
        if (!consume || consume->route != transfer->id) continue;
        completion = id;
        auto queue = std::find_if(
            scheduled.output->queues().begin(),
            scheduled.output->queues().end(),
            [&](const QueueProgram& candidate) {
                return candidate.id == step.queue;
            });
        ASSERT_NE(queue, scheduled.output->queues().end());
        EXPECT_EQ(queue->device, DeviceId("cpu"));
        EXPECT_EQ(step.region, authored.root().id);
    }
    ASSERT_TRUE(completion.has_value());
    EXPECT_NE(
        std::find(
            authority.dependencies.begin(),
            authority.dependencies.end(), *completion),
        authority.dependencies.end());
    for (const auto& [id, step] : scheduled.output->steps()) {
        (void)id;
        if (isPreLaunchStep(*scheduled.output, step)) {
            EXPECT_EQ(step.region, authored.root().id);
        }
    }
}

TEST(GraphPassTest, ScheduleGraphModelsWhileSplitDecisionAndAck) {
    auto root = detail::AuthoringRegion::createRoot();
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
    ::vrt::graph::detail::LoopRecord loop;
    loop.kind = LoopKind::WhileCondition;
    loop.condition = flag != 0u;
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

    ASSERT_EQ(scheduled.output->splitControls().size(), 1u);
    const SplitControlProtocol& protocol =
        scheduled.output->splitControls().front();
    ASSERT_EQ(protocol.followers.size(), 1u);
    const SplitControlFollowerProtocol& follower =
        protocol.followers.front();
    bool decision = false;
    bool acknowledged = false;
    for (const LogicalRendezvous& rendezvous :
         scheduled.output->rendezvous()) {
        if (rendezvous.id == follower.decision) {
            decision = std::holds_alternative<
                ControlDecisionRendezvous>(
                rendezvous.payload);
        }
        if (rendezvous.id == follower.acknowledgement) {
            acknowledged = std::holds_alternative<
                ControlAcknowledgedRendezvous>(
                rendezvous.payload);
        }
    }
    EXPECT_TRUE(decision);
    EXPECT_TRUE(acknowledged);
}

TEST(GraphPassTest, ScheduleGraphHoistsProducedChildInputToControlQueue) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size = root->inputScalar(ScalarType::U64, "size");
    IOTypeMap producerType;
    producerType.outputs.push_back({"out", BufferType::I32});
    detail::PortBindings producerIo;
    GraphBuffer produced;
    producerIo.bindOutput(
        "out", BufferType::I32, produced, size, root->scopeId());
    root->addKernel(
        KernelDescriptor{"produce", DeviceType::FPGA,
                         std::nullopt, producerType},
        std::move(producerIo), "accel");

    auto makeBranch = [&](const std::string& name) {
        auto branch = root->createChild();
        GraphBuffer local =
            branch->inputBuffer(BufferType::I32, name, size);
        branch->importFromParent({{produced, local}});
        IOTypeMap consumerType;
        consumerType.inputs.push_back({"in", BufferType::I32});
        detail::PortBindings consumerIo;
        consumerIo.bindInput("in", local);
        branch->addKernel(
            KernelDescriptor{name, DeviceType::CPU,
                             std::nullopt, consumerType},
            std::move(consumerIo), "cpu");
        return branch;
    };
    ::vrt::graph::detail::ConditionalRecord conditional;
    conditional.condition = Condition::alwaysTrue();
    conditional.thenRegion = makeBranch("then");
    conditional.elseRegion = makeBranch("else");
    root->addConditional(std::move(conditional));

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
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

    bool sawHoistedAction = false;
    for (const auto& [id, step] : scheduled.output->steps()) {
        (void)id;
        if (std::holds_alternative<ScheduledTransferAction>(
                step.payload)) {
            sawHoistedAction = true;
            EXPECT_EQ(step.region, authored.root().id);
            const TransferRoute* route =
                scheduledRoute(*scheduled.output, step);
            ASSERT_NE(route, nullptr);
            EXPECT_EQ(
                route->requirement.signature.phase,
                TransferPhase::Once);
        }
    }
    EXPECT_TRUE(sawHoistedAction);
}

TEST(GraphPassTest,
     SplitChildCaptureFromRootRunsOnceAndGatesAuthority) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphScalar trips =
        root->inputScalar(ScalarType::U32, "trips");
    GraphBuffer produced =
        root->buffer(BufferType::I32, "produced", size);
    IOTypeMap producerType =
        IOTypeMap{}.out<std::int32_t>("out");
    detail::PortBindings producerIo;
    producerIo.bindExistingOutput("out", produced);
    root->addKernel(
        KernelDescriptor{
            "root_producer", DeviceType::FPGA, std::nullopt,
            producerType},
        std::move(producerIo), "accel");

    auto body = root->createChild();
    GraphBuffer local =
        body->inputBuffer(BufferType::I32, "local", size);
    body->importFromParent(
        std::vector<BufferBoundaryMapping>{{produced, local}});
    IOTypeMap consumerType =
        IOTypeMap{}.in<std::int32_t>("in");
    detail::PortBindings consumerIo;
    consumerIo.bindInput("in", local);
    body->addKernel(
        KernelDescriptor{
            "cpu_body", DeviceType::CPU, std::nullopt,
            consumerType},
        std::move(consumerIo), "cpu");
    body->addKernel(
        KernelDescriptor{
            "fpga_body", DeviceType::FPGA, std::nullopt, {}},
        {}, "accel");

    detail::LoopRecord loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    const NodeId producer =
        authoredNodeId(authored.root().operations[0]);
    const NodeId control =
        authoredNodeId(authored.root().operations[1]);
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
    ASSERT_NE(
        std::get_if<SplitControlPlacement>(
            &placed.output->controlPlacements().at(control)),
        nullptr);

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
    auto captured = std::find_if(
        routed.output->routes().begin(),
        routed.output->routes().end(),
        [&](const TransferRoute& route) {
            return route.requirement.sourceAnchor.operation() ==
                       std::optional<NodeId>(producer) &&
                   route.requirement.destinationAnchor.operation()
                       .has_value();
        });
    ASSERT_NE(captured, routed.output->routes().end());
    EXPECT_EQ(
        captured->requirement.signature.phase,
        TransferPhase::Once);
    EXPECT_NE(
        std::get_if<GraphTransferScope>(
            &captured->requirement.signature.scope),
        nullptr);

    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());
    ASSERT_EQ(scheduled.output->splitControls().size(), 1u);
    const SplitControlProtocol& protocol =
        scheduled.output->splitControls().front();
    std::optional<ScheduleStepId> completion;
    for (const auto& [id, step] : scheduled.output->steps()) {
        const auto* consume =
            std::get_if<ScheduledTransferConsume>(&step.payload);
        if (consume && consume->route == captured->id) {
            completion = id;
        }
        const TransferRoute* stepRoute =
            scheduledRoute(*scheduled.output, step);
        if (stepRoute && stepRoute->id == captured->id) {
            EXPECT_EQ(step.region, authored.root().id);
        }
    }
    ASSERT_TRUE(completion.has_value());
    const ScheduledStep& authority =
        scheduled.output->steps().at(protocol.authorityStep);
    EXPECT_NE(
        std::find(
            authority.dependencies.begin(),
            authority.dependencies.end(), *completion),
        authority.dependencies.end());
}

TEST(GraphPassTest,
     AutonomousControlQueueRetainsDependencyOrder) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar trips =
        root->inputScalar(ScalarType::U32, "trips");
    const std::string before = root->addKernel(
        KernelDescriptor{
            "before", DeviceType::FPGA, std::nullopt, {}},
        {}, "accel");
    auto body = root->createChild();
    body->addKernel(
        KernelDescriptor{
            "body", DeviceType::FPGA, std::nullopt, {}},
        {}, "accel");
    detail::LoopRecord loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    loop.afterOps = {before};
    root->addLoop(std::move(loop));

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    const NodeId beforeNode =
        authoredNodeId(authored.root().operations[0]);
    const NodeId control =
        authoredNodeId(authored.root().operations[1]);
    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
    ASSERT_TRUE(resolved.ok());
    auto host = std::make_shared<PlacementDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PlacementDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities(),
        true);
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"accel", accelerator}};
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());
    ASSERT_NE(
        std::get_if<AutonomousControlPlacement>(
            &placed.output->controlPlacements().at(control)),
        nullptr);
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, {}));
    ASSERT_TRUE(routed.ok());
    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());

    const QueueProgram* queue = nullptr;
    for (const QueueProgram& candidate :
         scheduled.output->queues()) {
        if (candidate.region == authored.root().id &&
            candidate.device == DeviceId("accel")) {
            queue = &candidate;
            break;
        }
    }
    ASSERT_NE(queue, nullptr);
    auto positionOf = [&](NodeId operation) {
        return std::find_if(
            queue->steps.begin(), queue->steps.end(),
            [&](ScheduleStepId id) {
                const auto* scheduledOperation =
                    std::get_if<ScheduledOperation>(
                        &scheduled.output->steps().at(id).payload);
                return scheduledOperation &&
                       scheduledOperation->operation == operation;
            });
    };
    const auto beforePosition = positionOf(beforeNode);
    const auto controlPosition = positionOf(control);
    ASSERT_NE(beforePosition, queue->steps.end());
    ASSERT_NE(controlPosition, queue->steps.end());
    EXPECT_LT(beforePosition, controlPosition);
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
    std::vector<LogicalScalarRequirement> scalarRequirements{
        {ScalarResourceId(0), ValueId(0), DeviceId("accel"),
         scopedScalarKey(0, "first")},
        {ScalarResourceId(1), ValueId(1), DeviceId("accel"),
         scopedScalarKey(0, "second")},
    };
    ScheduledGraph scheduled(
        std::shared_ptr<const RoutedGraph>{}, {}, {}, {},
        {}, requirements, scalarRequirements);

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
        firstPhysical.insert(static_cast<std::uint32_t>(
            binding.physical.value()));
    }
    for (const auto& [logical, binding] :
         first.output->scalars()) {
        (void)logical;
        EXPECT_EQ(firstPhysical.insert(
                      static_cast<std::uint32_t>(
                          binding.physical.value()))
                      .second,
                  true);
    }
    for (const auto& [logical, binding] :
         second.output->rendezvous()) {
        (void)logical;
        EXPECT_EQ(firstPhysical.count(
                      static_cast<std::uint32_t>(
                          binding.physical.value())),
                  0u);
    }
    for (const auto& [logical, binding] :
         second.output->scalars()) {
        (void)logical;
        EXPECT_EQ(firstPhysical.count(
                      static_cast<std::uint32_t>(
                          binding.physical.value())),
                  0u);
    }

    first.output.reset();
    CompileResult<BackendResourceBindings> third =
        bindBackendResources(scheduled, devices);
    ASSERT_TRUE(third.ok());
    std::set<std::uint32_t> thirdPhysical;
    for (const auto& [logical, binding] :
         third.output->rendezvous()) {
        (void)logical;
        thirdPhysical.insert(static_cast<std::uint32_t>(
            binding.physical.value()));
    }
    for (const auto& [logical, binding] :
         third.output->scalars()) {
        (void)logical;
        thirdPhysical.insert(static_cast<std::uint32_t>(
            binding.physical.value()));
    }
    EXPECT_EQ(thirdPhysical, firstPhysical);
}
