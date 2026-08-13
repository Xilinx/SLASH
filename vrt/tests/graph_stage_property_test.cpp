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
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <vrt/graph/device/device.hpp>
#include <vrt/graph/graph.hpp>
#include <vrt/graph/ir/placed_graph.hpp>
#include <vrt/graph/ir/resolved_graph.hpp>
#include <vrt/graph/ir/routed_graph.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>

#include "test_support/graph_internal.hpp"

using namespace vrt::graph;

namespace {

class CopyKernel : public CpuKernel {
   public:
    CopyKernel() : CpuKernel("property_copy") {}

    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<std::int32_t>("in").out<std::int32_t>("out");
    }

    void run(Args& args) override {
        auto input = args.in<std::int32_t>("in");
        auto output = args.out<std::int32_t>("out");
        std::copy(input.begin(), input.end(), output.begin());
    }
};

class InoutKernel : public CpuKernel {
   public:
    InoutKernel() : CpuKernel("property_inout") {}

    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.inout<std::int32_t>("value");
    }

    void run(Args&) override {}
};

class ParityKernel : public CpuKernel {
   public:
    ParityKernel() : CpuKernel("property_parity") {}

    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}
            .in<std::int32_t>("in")
            .scalarOut<std::uint64_t>("parity");
    }

    void run(Args& args) override {
        auto input = args.in<std::int32_t>("in");
        args.scalarOut<std::uint64_t>("parity") =
            input.empty() ? 0u : static_cast<std::uint64_t>(input[0] & 1);
    }
};

class PropertyDevice : public IDevice {
   public:
    PropertyDevice(std::string id, DeviceType type,
                   DeviceCapabilities capabilities)
        : id_(std::move(id)),
          type_(type),
          capabilities_(std::move(capabilities)) {
        capabilities_.device = DeviceId(id_);
    }

    DeviceType type() const override { return type_; }
    std::string id() const override { return id_; }
    DeviceCapabilities compilerCapabilities() const override {
        return capabilities_;
    }

    std::optional<std::string> resolveMemoryRegion(
        const KernelDescriptor&, const std::string& port) const override {
        auto found = regions_.find(port);
        return found == regions_.end()
                   ? std::nullopt
                   : std::optional<std::string>(found->second);
    }

    void setRegion(std::string port, std::string region) {
        regions_[std::move(port)] = std::move(region);
    }

   private:
    std::string id_;
    DeviceType type_;
    DeviceCapabilities capabilities_;
    std::map<std::string, std::string> regions_;
};

DeviceCapabilities hostCapabilities() {
    DeviceCapabilities capabilities;
    capabilities.backend = "property_host";
    capabilities.kernelTypes.insert(DeviceType::CPU);
    capabilities.hostsGraphIo = true;
    capabilities.ownsFallbackControl = true;
    capabilities.supportsSplitAuthority = true;
    return capabilities;
}

DeviceCapabilities acceleratorCapabilities() {
    DeviceCapabilities capabilities;
    capabilities.backend = "property_accelerator";
    capabilities.kernelTypes.insert(DeviceType::FPGA);
    capabilities.supportsMemoryRegionCopies = true;
    capabilities.ownsRendezvousNamespace = true;
    return capabilities;
}

std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>
directBridgeFactories() {
    auto marker = [](IDevice&, IDevice&) -> std::shared_ptr<IBridge> {
        return nullptr;
    };
    return {
        {{DeviceType::CPU, DeviceType::FPGA}, marker},
        {{DeviceType::FPGA, DeviceType::CPU}, marker},
    };
}

void expectAuthoredIndexIntegrity(const AuthoredGraph& authored) {
    const RegionTreeIndex& index = authored.index();
    ASSERT_EQ(index.findRegion(authored.root().id), &authored.root());
    ASSERT_EQ(index.regionForScope(authored.root().sourceScope),
              std::optional<RegionId>(authored.root().id));

    for (const auto& [regionId, region] : index.regions()) {
        ASSERT_NE(region, nullptr);
        EXPECT_EQ(region->id, regionId);
        EXPECT_EQ(index.regionForScope(region->sourceScope),
                  std::optional<RegionId>(regionId));
        for (const AuthoredOperation& operation : region->operations) {
            const NodeId id = authoredNodeId(operation);
            EXPECT_EQ(index.findOperation(id), &operation);
            EXPECT_EQ(index.regionForOperation(id),
                      std::optional<RegionId>(regionId));
        }
        for (const AuthoredChildRegion& child : index.children(regionId)) {
            ASSERT_NE(index.findRegion(child.region), nullptr);
            EXPECT_EQ(index.parentControl(child.region),
                      std::optional<NodeId>(child.control));
        }
    }
}

void expectResolvedIntegrity(const ResolvedGraph& resolved) {
    std::function<void(const ResolvedRegion&)> expectRegion =
        [&](const ResolvedRegion& region) {
            std::map<NodeId, std::size_t> position;
            for (std::size_t i = 0;
                 i < region.topologicalOrder.size(); ++i) {
                EXPECT_TRUE(
                    position.emplace(region.topologicalOrder[i], i)
                        .second);
                const ResolvedOperation* operation =
                    resolved.findOperation(
                        region.topologicalOrder[i]);
                ASSERT_NE(operation, nullptr);
                EXPECT_EQ(operation->region, region.id);
            }
            for (const auto& [id, operation] :
                 resolved.operations()) {
                if (operation.region != region.id) continue;
                ASSERT_EQ(position.count(id), 1u);
                for (const ResolvedDependency& dependency :
                     operation.dependencies) {
                    const ResolvedOperation* predecessor =
                        resolved.findOperation(
                            dependency.predecessor);
                    ASSERT_NE(predecessor, nullptr);
                    if (predecessor->region == region.id) {
                        EXPECT_LT(position.at(predecessor->id),
                                  position.at(id));
                    }
                }
            }
            for (const std::shared_ptr<const ResolvedRegion>& child :
                 region.children) {
                ASSERT_NE(child, nullptr);
                EXPECT_EQ(child->parent,
                          std::optional<RegionId>(region.id));
                expectRegion(*child);
            }
        };
    expectRegion(resolved.root());

    for (const auto& [id, value] : resolved.values()) {
        EXPECT_EQ(id, value.id);
        EXPECT_NE(resolved.authored().index().findRegion(value.region),
                  nullptr);
        if (const std::optional<NodeId> producer = valueProducer(value)) {
            EXPECT_NE(resolved.findOperation(*producer), nullptr);
        }
        if (value.size) {
            EXPECT_NE(resolved.findValue(*value.size), nullptr);
        }
    }
    for (const auto& [id, operation] : resolved.operations()) {
        EXPECT_EQ(id, operation.id);
        for (const ResolvedDependency& dependency :
             operation.dependencies) {
            EXPECT_NE(resolved.findOperation(dependency.predecessor),
                      nullptr);
            if (dependency.value) {
                EXPECT_NE(resolved.findValue(*dependency.value), nullptr);
            }
        }
        for (const ResolvedBinding& binding : operation.bindings) {
            EXPECT_NE(resolved.findValue(binding.value), nullptr);
        }
    }
    for (const ResolvedInoutBinding& inout : resolved.inouts()) {
        EXPECT_NE(inout.input, inout.output);
        EXPECT_NE(resolved.findOperation(inout.operation), nullptr);
        EXPECT_NE(resolved.findValue(inout.input), nullptr);
        EXPECT_NE(resolved.findValue(inout.output), nullptr);
    }
    for (const BoundaryAlias& alias : resolved.aliases().entries()) {
        EXPECT_NE(resolved.findOperation(alias.boundary), nullptr);
        EXPECT_NE(resolved.findValue(alias.source), nullptr);
        EXPECT_NE(resolved.findValue(alias.target), nullptr);
        EXPECT_EQ(resolved.aliases().canonical(alias.target),
                  resolved.aliases().canonical(alias.source));
    }
    for (const ResolvedControlResult& result :
         resolved.controlResults()) {
        EXPECT_NE(resolved.findOperation(result.control), nullptr);
        EXPECT_NE(resolved.findValue(result.result), nullptr);
        ASSERT_FALSE(result.incoming.empty());
        for (const ControlIncoming& incoming : result.incoming) {
            EXPECT_NE(resolved.findValue(incoming.value), nullptr);
            EXPECT_NE(resolved.authored().index().findRegion(
                          incoming.region),
                      nullptr);
        }
    }
}

void expectPlacedReplicaIntegrity(const PlacedGraph& placed) {
    for (const auto& [id, replica] : placed.replicas()) {
        EXPECT_EQ(id, replica.id);
        EXPECT_NE(placed.resolved().findValue(replica.value), nullptr);
    }
    for (const PortPlacement& port : placed.portPlacements()) {
        EXPECT_NE(placed.resolved().findOperation(port.operation), nullptr);
        EXPECT_NE(placed.findReplica(port.replica), nullptr);
    }
    for (const auto& [id, value] : placed.resolved().values()) {
        (void)value;
        EXPECT_NE(placed.primaryReplica(id), nullptr);
    }
}

std::vector<std::string> routeSignature(const RoutedGraph& routed) {
    std::vector<std::string> signature;
    for (const TransferRoute& route : routed.routes()) {
        std::string entry =
            std::to_string(route.id.value()) + ":" +
            route.requirement.signature.sourceLocation.device.value() +
            "->" +
            route.requirement.signature.destinationLocation.device.value() +
            ":" +
            std::to_string(static_cast<int>(
                route.requirement.signature.payload));
        for (const TransferLeg& leg : route.legs) {
            entry += "/" + std::to_string(leg.id.value()) + ":" +
                     leg.source.value() + "->" +
                     leg.destination.value();
        }
        signature.push_back(std::move(entry));
    }
    return signature;
}

void expectScheduledIntegrity(const ScheduledGraph& scheduled) {
    std::map<ScheduleStepId, int> state;
    std::function<void(ScheduleStepId)> visit =
        [&](ScheduleStepId id) {
            ASSERT_EQ(state[id], 0) << "cycle at step " << id;
            state[id] = 1;
            const ScheduledStep& step = scheduled.steps().at(id);
            for (ScheduleStepId dependency : step.dependencies) {
                ASSERT_EQ(scheduled.steps().count(dependency), 1u);
                if (state[dependency] == 0) visit(dependency);
                ASSERT_EQ(state[dependency], 2);
            }
            state[id] = 2;
        };

    std::set<ScheduleStepId> queued;
    for (const QueueProgram& queue : scheduled.queues()) {
        for (ScheduleStepId id : queue.steps) {
            ASSERT_EQ(scheduled.steps().count(id), 1u);
            EXPECT_EQ(scheduled.steps().at(id).queue, queue.id);
            EXPECT_TRUE(queued.insert(id).second);
        }
    }
    EXPECT_EQ(queued.size(), scheduled.steps().size());
    for (const auto& [id, step] : scheduled.steps()) {
        EXPECT_EQ(id, step.id);
        if (state[id] == 0) visit(id);
        std::visit(
            [&](const auto& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, ScheduledOperation>) {
                    EXPECT_NE(scheduled.routed().placed().resolved()
                                  .findOperation(payload.operation),
                              nullptr);
                } else if constexpr (
                    std::is_same_v<T, ScheduledTransferProduce> ||
                    std::is_same_v<T, ScheduledTransferConsume> ||
                    std::is_same_v<T, ScheduledTransferAction>) {
                    EXPECT_NE(
                        std::find_if(
                            scheduled.routed().routes().begin(),
                            scheduled.routed().routes().end(),
                            [&](const TransferRoute& route) {
                                return route.id == payload.route;
                            }),
                        scheduled.routed().routes().end());
                } else if constexpr (
                    std::is_same_v<T, ScheduledGraphInput> ||
                    std::is_same_v<T, ScheduledGraphOutput>) {
                    for (ValueId value : payload.values) {
                        EXPECT_NE(scheduled.routed().placed().resolved()
                                      .findValue(value),
                                  nullptr);
                    }
                } else if constexpr (
                    std::is_same_v<T,
                                   ScheduledBoundaryMaterialization>) {
                    for (const ScheduledBoundaryMapping& mapping :
                         payload.mappings) {
                        EXPECT_NE(scheduled.routed().findReplica(
                                      mapping.source),
                                  nullptr);
                        EXPECT_NE(scheduled.routed().findReplica(
                                      mapping.target),
                                  nullptr);
                    }
                }
            },
            step.payload);
    }
}

void expectInvalidDiagnostics(std::uint32_t seed) {
    Graph malformed = detail::GraphTestAccess::make();
    IOTypeMap type;
    type.inputs.push_back({"required", BufferType::I32});
    detail::GraphTestAccess::addNode(
        malformed,
        KernelDescriptor{"broken_" + std::to_string(seed),
                         DeviceType::CPU, std::nullopt, type},
        {}, "cpu",
        seed % 2 == 0 ? std::vector<std::string>{}
                      : std::vector<std::string>{"missing"});

    CompileResult<ResolvedGraph> result =
        resolveGraph(detail::GraphTestAccess::snapshot(malformed));
    ASSERT_FALSE(result.ok());
    bool sawUnbound = false;
    bool sawUnknownDependency = false;
    for (const Diagnostic& diagnostic :
         result.diagnostics.entries()) {
        EXPECT_FALSE(diagnostic.message.empty());
        sawUnbound |= diagnostic.code == DiagCode::UnboundPort;
        sawUnknownDependency |=
            diagnostic.code == DiagCode::UnknownDependency;
    }
    EXPECT_TRUE(sawUnbound);
    EXPECT_EQ(sawUnknownDependency, seed % 2 != 0);
}

void expectFanoutAndRouteDeterminism(std::uint32_t seed) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "fanout_size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "fanout_input", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "fanout_output", size);

    IOTypeMap type;
    type.inputs.push_back({"first", BufferType::I32});
    type.inputs.push_back({"second", BufferType::I32});
    type.outputs.push_back({"out", BufferType::I32});
    detail::PortBindings bindings;
    bindings.bindInput("first", input)
        .bindInput("second", input)
        .bindExistingOutput("out", output);
    root->addKernel(
        KernelDescriptor{"fanout", DeviceType::FPGA,
                         std::nullopt, type},
        std::move(bindings), "accel");

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());

    auto host = std::make_shared<PropertyDevice>(
        "cpu", DeviceType::CPU, hostCapabilities());
    auto accelerator = std::make_shared<PropertyDevice>(
        "accel", DeviceType::FPGA, acceleratorCapabilities());
    accelerator->setRegion(
        "first", seed % 2 == 0 ? "HBM[0]" : "HBM[1]");
    accelerator->setRegion(
        "second", seed % 2 == 0 ? "HBM[1]" : "HBM[0]");
    accelerator->setRegion("out", "HBM[2]");
    std::map<std::string, std::shared_ptr<IDevice>> devices{
        {"cpu", host}, {"accel", accelerator}};

    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());
    expectPlacedReplicaIntegrity(*placed.output);

    std::size_t inputReplicas = 0;
    for (const auto& [id, replica] : placed.output->replicas()) {
        (void)id;
        if (resolved.output->findValue(replica.value)->sourceName ==
            "fanout_input") {
            ++inputReplicas;
        }
    }
    EXPECT_GE(inputReplicas, 3u);

    const auto factories = directBridgeFactories();
    CompileResult<RoutedGraph> first = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    CompileResult<RoutedGraph> second = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_FALSE(first.output->routes().empty());
    EXPECT_EQ(routeSignature(*first.output),
              routeSignature(*second.output));

    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*first.output);
    ASSERT_TRUE(scheduled.ok());
    expectScheduledIntegrity(*scheduled.output);
}

class GraphStagePropertyTest :
    public ::testing::TestWithParam<std::uint32_t> {};

TEST_P(GraphStagePropertyTest, PreservesTypedStageInvariants) {
    const std::uint32_t seed = GetParam();
    SCOPED_TRACE("seed=" + std::to_string(seed));

    Graph graph = Graph::withDefaults();
    KernelHandle copy = graph.cpu().add<CopyKernel>();
    KernelHandle mutate = graph.cpu().add<InoutKernel>();
    KernelHandle parityKernel = graph.cpu().add<ParityKernel>();
    GraphScalar size = graph.scalarInput<std::uint64_t>(
        "size_" + std::to_string(seed));
    GraphBuffer input = graph.input<std::int32_t>(
        "input_" + std::to_string(seed), size);
    GraphBuffer value = graph.buffer<std::int32_t>(
        "value_" + std::to_string(seed), size);
    graph.addKernelCall({
        .kernel = copy,
        .inputs = {{"in", input}},
        .outputs = {{"out", value}},
    });

    const bool hasInout = (seed & 1u) != 0;
    const bool hasLoop = (seed & 2u) != 0;
    const bool hasConditional = (seed & 4u) != 0;
    if (hasInout) {
        GraphBuffer next = graph.buffer<std::int32_t>(
            "mutated_" + std::to_string(seed), size);
        graph.addKernelCall({
            .kernel = mutate,
            .inouts = {{"value", value, next}},
        });
        value = next;
    }
    if (hasLoop) {
        GraphScalar count = graph.scalarInput<std::uint32_t>(
            "count_" + std::to_string(seed));
        GraphBuffer loopResult = graph.buffer<std::int32_t>(
            "loop_result_" + std::to_string(seed), size);
        RegionBuilder body = graph.addLoop({
            .count = count,
            .inputs = {{"state", value}},
            .outputs = {{"state", loopResult}},
        });
        if ((seed & 8u) != 0) {
            GraphBuffer next = body.buffer<std::int32_t>(
                "loop_next_" + std::to_string(seed), size);
            body.addKernelCall({
                .kernel = copy,
                .inputs = {{"in", body.input("state")}},
                .outputs = {{"out", next}},
            });
            body.addKernelCall({
                .kernel = mutate,
                .inouts = {
                    {"value", next, body.output("state")}},
            });
        } else {
            body.addKernelCall({
                .kernel = copy,
                .inputs = {{"in", body.input("state")}},
                .outputs = {{"out", body.output("state")}},
            });
        }
        value = loopResult;
    }
    if (hasConditional) {
        GraphScalar parity = graph.scalar<std::uint64_t>(
            "parity_" + std::to_string(seed));
        graph.addKernelCall({
            .kernel = parityKernel,
            .inputs = {{"in", value}},
            .outputScalars = {{"parity", parity}},
        });
        GraphBuffer selected = graph.buffer<std::int32_t>(
            "selected_" + std::to_string(seed), size);
        auto [thenBranch, elseBranch] = graph.addConditional({
            .condition =
                parity == static_cast<std::uint64_t>(seed & 1u),
            .inputs = {{"value", value}},
            .outputs = {{"value", selected}},
        });
        thenBranch.addKernelCall({
            .kernel = copy,
            .inputs = {{"in", thenBranch.input("value")}},
            .outputs = {{"out", thenBranch.output("value")}},
        });
        elseBranch.addKernelCall({
            .kernel = copy,
            .inputs = {{"in", elseBranch.input("value")}},
            .outputs = {{"out", elseBranch.output("value")}},
        });
        value = selected;
    }
    GraphBuffer output = graph.output<std::int32_t>(
        "output_" + std::to_string(seed), size);
    graph.addKernelCall({
        .kernel = copy,
        .inputs = {{"in", value}},
        .outputs = {{"out", output}},
    });

    AuthoredGraph authored =
        detail::GraphTestAccess::snapshot(graph);
    expectAuthoredIndexIntegrity(authored);

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(authored);
    ASSERT_TRUE(resolved.ok())
        << (resolved.diagnostics.firstError()
                ? resolved.diagnostics.firstError()->message
                : "no diagnostic");
    expectResolvedIntegrity(*resolved.output);
    EXPECT_GE(resolved.output->inouts().size(),
              hasInout ? 1u : 0u);
    EXPECT_EQ(resolved.output->controlResults().empty(),
              !(hasLoop || hasConditional));
    EXPECT_EQ(resolved.output->aliases().entries().empty(),
              !(hasLoop || hasConditional));

    const auto& devices =
        detail::GraphTestAccess::devices(graph);
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output,
        DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());
    expectPlacedReplicaIntegrity(*placed.output);

    const auto& factories =
        detail::GraphTestAccess::bridgeFactories(graph);
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(routed.ok());
    CompileResult<RoutedGraph> routedAgain = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, factories));
    ASSERT_TRUE(routedAgain.ok());
    EXPECT_EQ(routeSignature(*routed.output),
              routeSignature(*routedAgain.output));

    CompileResult<ScheduledGraph> scheduled =
        scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());
    expectScheduledIntegrity(*scheduled.output);

    expectFanoutAndRouteDeterminism(seed);
    expectInvalidDiagnostics(seed);
}

INSTANTIATE_TEST_SUITE_P(
    DeterministicSeeds, GraphStagePropertyTest,
    ::testing::Range<std::uint32_t>(0, 160),
    [](const ::testing::TestParamInfo<std::uint32_t>& info) {
        return "Seed" + std::to_string(info.param);
    });

}  // namespace
