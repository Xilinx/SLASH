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

#include <vrt/graph/ir/routed_graph.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace vrt::graph {

namespace {

class GraphRouter {
   public:
    CompileResult<RoutedGraph> route(
        const PlacedGraph& placed,
        const TransferCapabilityCatalog& capabilities) {
        placed_ = std::make_shared<PlacedGraph>(placed);
        capabilities_ = &capabilities;
        indexRegions(placed_->resolved().authored().root(),
                     std::nullopt);
        buildBoundaryAliases();
        validateMutablePlacements();
        routeValueConsumers();
        routeControlPublications();
        routeGraphOutputs();
        routeOrderDependencies();

        if (diagnostics_.hasErrors()) {
            return CompileResult<RoutedGraph>::failure(
                std::move(diagnostics_));
        }
        return CompileResult<RoutedGraph>::success(
            RoutedGraph(placed_, std::move(dependencies_),
                        std::move(routes_)),
            std::move(diagnostics_));
    }

   private:
    struct RouteKey {
        ValueId                      value;
        std::optional<NodeId>        producer;
        std::optional<NodeId>        consumer;
        DeviceId                     sourceDevice;
        DeviceId                     destinationDevice;
        std::optional<MemoryRegionId> sourceRegion;
        std::optional<MemoryRegionId> destinationRegion;
        TransferPayloadKind          payload =
            TransferPayloadKind::Buffer;

        bool operator<(const RouteKey& other) const {
            return std::tie(
                       value, producer, consumer, sourceDevice,
                       destinationDevice, sourceRegion,
                       destinationRegion, payload) <
                   std::tie(
                       other.value, other.producer, other.consumer,
                       other.sourceDevice, other.destinationDevice,
                       other.sourceRegion, other.destinationRegion,
                       other.payload);
        }
    };

    void indexRegions(const AuthoredRegion& region,
                      std::optional<NodeId> parentControl) {
        parentControlByRegion_[region.id] = parentControl;
        for (const AuthoredOperation& operation : region.operations) {
            authoredOperations_[authoredNodeId(operation)] = &operation;
            if (const auto* loop =
                    std::get_if<AuthoredLoop>(&operation)) {
                if (loop->body) {
                    indexRegions(*loop->body, loop->id);
                }
            } else if (const auto* conditional =
                           std::get_if<AuthoredConditional>(&operation)) {
                if (conditional->thenRegion) {
                    indexRegions(*conditional->thenRegion,
                                 conditional->id);
                }
                if (conditional->elseRegion) {
                    indexRegions(*conditional->elseRegion,
                                 conditional->id);
                }
            }
        }
    }

    void buildBoundaryAliases() {
        for (const auto& [node, operation] :
             placed_->resolved().operations()) {
            (void)node;
            if (!operation.structural) continue;
            std::map<std::string, ValueId> sources;
            for (const ResolvedBinding& binding :
                 operation.bindings) {
                if (binding.access ==
                    ValueAccess::BoundarySource) {
                    sources[binding.port] = binding.value;
                    continue;
                }
                if (binding.access !=
                    ValueAccess::BoundaryTarget) {
                    continue;
                }
                auto source = sources.find(binding.port);
                if (source == sources.end()) continue;
                const ResolvedValue* target =
                    placed_->resolved().findValue(binding.value);
                if (target &&
                    target->definition ==
                        ValueDefinitionKind::RegionParameter) {
                    aliases_[binding.value] = source->second;
                }
            }
        }
    }

    ValueId origin(ValueId value) const {
        std::set<ValueId> seen;
        while (seen.insert(value).second) {
            auto alias = aliases_.find(value);
            if (alias == aliases_.end()) break;
            value = alias->second;
        }
        return value;
    }

    std::optional<MemoryPlacement> valuePlacement(
        ValueId value) const {
        value = origin(value);
        auto placement =
            placed_->valuePlacements().find(value);
        if (placement == placed_->valuePlacements().end()) {
            return std::nullopt;
        }
        return placement->second;
    }

    std::vector<MemoryPlacement> consumerPlacements(
        NodeId operation, ValueId value) const {
        std::vector<MemoryPlacement> result;
        for (const PortPlacement& port :
             placed_->portPlacements()) {
            if (port.operation == operation &&
                port.value == value) {
                result.push_back(port.memory);
            }
        }
        if (result.empty()) {
            auto control =
                placed_->controlPlacements().find(operation);
            if (control != placed_->controlPlacements().end() &&
                control->second.mode ==
                    ControlMode::SplitAcrossDevices) {
                for (DeviceId participant :
                     control->second.participants) {
                    result.push_back(
                        {participant, std::nullopt});
                }
            } else {
                auto placement =
                    placed_->operationPlacements().find(operation);
                if (placement !=
                    placed_->operationPlacements().end()) {
                    result.push_back(
                        {placement->second.device, std::nullopt});
                }
            }
        }
        std::sort(
            result.begin(), result.end(),
            [](const MemoryPlacement& lhs,
               const MemoryPlacement& rhs) {
                return std::tie(lhs.device, lhs.region) <
                       std::tie(rhs.device, rhs.region);
            });
        result.erase(
            std::unique(
                result.begin(), result.end(),
                [](const MemoryPlacement& lhs,
                   const MemoryPlacement& rhs) {
                    return lhs.device == rhs.device &&
                           lhs.region == rhs.region;
                }),
            result.end());
        return result;
    }

    bool nestedAutonomousCopy(NodeId consumer) const {
        const ResolvedOperation* operation =
            placed_->resolved().findOperation(consumer);
        if (!operation) return false;
        auto parent =
            parentControlByRegion_.find(operation->region);
        while (parent != parentControlByRegion_.end() &&
               parent->second) {
            auto placement =
                placed_->controlPlacements().find(*parent->second);
            if (placement !=
                    placed_->controlPlacements().end() &&
                placement->second.mode ==
                    ControlMode::AutonomousOnDevice) {
                return true;
            }
            const ResolvedOperation* control =
                placed_->resolved().findOperation(*parent->second);
            if (!control) break;
            parent =
                parentControlByRegion_.find(control->region);
        }
        return false;
    }

    std::optional<RouteId> createRoute(
        std::optional<ValueId> value,
        TransferPayloadKind payload,
        std::optional<NodeId> producer,
        std::optional<NodeId> consumer,
        const MemoryPlacement& source,
        const MemoryPlacement& destination) {
        if (source.device == destination.device &&
            (!source.region || !destination.region ||
             source.region == destination.region)) {
            return std::nullopt;
        }

        if (source.device == destination.device &&
            consumer && nestedAutonomousCopy(*consumer)) {
            diagnostics_.error(
                DiagCode::UnsupportedNestedCopy,
                "GraphCompiler: same-device memory-region copy "
                "inside autonomous control is unsupported");
            return std::nullopt;
        }

        RouteKey key;
        key.value = value.value_or(ValueId{});
        key.producer = producer;
        key.consumer =
            payload == TransferPayloadKind::Barrier
                ? consumer
                : std::nullopt;
        key.sourceDevice = source.device;
        key.destinationDevice = destination.device;
        key.sourceRegion = source.region;
        key.destinationRegion = destination.region;
        key.payload = payload;
        auto existing = routeIds_.find(key);
        if (existing != routeIds_.end()) return existing->second;

        TransferRoute route;
        route.requirement.id = RouteId(nextRoute_++);
        route.requirement.value = value;
        route.requirement.payload = payload;
        route.requirement.source = {
            producer, source.device, source.region};
        route.requirement.destination = {
            consumer, destination.device, destination.region};

        if (source.device == destination.device) {
            if (!capabilities_->supportsMemoryRegionCopies(
                    source.device)) {
                diagnostics_.error(
                    DiagCode::IncompatibleMemoryPlacement,
                    "GraphCompiler: device '" +
                        source.device.value() +
                        "' cannot copy between memory regions");
                return std::nullopt;
            }
            route.legs.push_back(
                {TransferMechanism::HostMediatedDeviceCopy,
                 source.device, destination.device,
                 capabilities_->host()});
        } else if (capabilities_->hasDirect(
                       source.device, destination.device)) {
            std::optional<DeviceId> executor;
            if (capabilities_->host() &&
                (source.device == *capabilities_->host() ||
                 destination.device == *capabilities_->host())) {
                executor = capabilities_->host();
            }
            route.legs.push_back(
                {TransferMechanism::DirectBridge,
                 source.device, destination.device, executor});
        } else if (capabilities_->host() &&
                   source.device != *capabilities_->host() &&
                   destination.device != *capabilities_->host() &&
                   capabilities_->hasDirect(
                       source.device, *capabilities_->host()) &&
                   capabilities_->hasDirect(
                       *capabilities_->host(),
                       destination.device)) {
            route.legs.push_back(
                {TransferMechanism::HostBounce, source.device,
                 *capabilities_->host(),
                 capabilities_->host()});
            route.legs.push_back(
                {TransferMechanism::HostBounce,
                 *capabilities_->host(), destination.device,
                 capabilities_->host()});
        } else {
            diagnostics_.error(
                DiagCode::MissingTransferRoute,
                "GraphCompiler: no transfer route from '" +
                    source.device.value() + "' to '" +
                    destination.device.value() + "'");
            return std::nullopt;
        }

        const RouteId id = route.requirement.id;
        routeIds_.emplace(std::move(key), id);
        routes_.push_back(std::move(route));
        return id;
    }

    static bool consumesValue(ValueAccess access) {
        return access == ValueAccess::Input ||
               access == ValueAccess::InoutInput ||
               access == ValueAccess::Condition ||
               access == ValueAccess::TripCount ||
               access == ValueAccess::BoundarySource;
    }

    void routeValueConsumers() {
        std::set<std::tuple<NodeId, ValueId, DeviceId,
                            std::optional<MemoryRegionId>>> seen;
        for (const auto& [node, operation] :
             placed_->resolved().operations()) {
            if (operation.structural) continue;
            for (const ResolvedBinding& binding :
                 operation.bindings) {
                if (!consumesValue(binding.access)) continue;
                if (binding.access == ValueAccess::BoundarySource &&
                    (operation.kind == ResolvedOperationKind::Loop ||
                     operation.kind ==
                         ResolvedOperationKind::Conditional)) {
                    continue;
                }
                const ValueId sourceValue = origin(binding.value);
                const ResolvedValue* value =
                    placed_->resolved().findValue(sourceValue);
                if (!value) continue;
                if (value->type.kind == ValueKind::Scalar &&
                    value->definition ==
                        ValueDefinitionKind::GraphInput) {
                    dependencies_.push_back(
                        {std::nullopt, node, DependencyKind::Value,
                         sourceValue, std::nullopt});
                    continue;
                }
                std::optional<MemoryPlacement> source =
                    valuePlacement(sourceValue);
                if (!source) {
                    diagnostics_.error(
                        DiagCode::InternalInvariant,
                        "GraphCompiler: value '" +
                            value->sourceName +
                            "' has no source placement");
                    continue;
                }
                for (const MemoryPlacement& destination :
                     consumerPlacements(node, binding.value)) {
                    if (!seen
                             .insert({node, sourceValue,
                                      destination.device,
                                      destination.region})
                             .second) {
                        continue;
                    }
                    const TransferPayloadKind payload =
                        value->type.kind == ValueKind::Buffer
                            ? TransferPayloadKind::Buffer
                            : TransferPayloadKind::Scalar;
                    const std::optional<RouteId> route =
                        createRoute(
                            sourceValue, payload, value->producer,
                            node, *source, destination);
                    dependencies_.push_back(
                        {value->producer, node,
                         DependencyKind::Value, sourceValue, route});
                    if (value->producer) {
                        dataDependencies_.insert(
                            {*value->producer, node});
                    }
                }
            }
        }
    }

    void routeControlPublications() {
        for (const ResolvedControlResult& result :
             placed_->resolved().controlResults()) {
            const ResolvedValue* destinationValue =
                placed_->resolved().findValue(result.result);
            std::optional<MemoryPlacement> destination =
                valuePlacement(result.result);
            if (!destinationValue || !destination) continue;
            for (const ControlIncoming& incoming :
                 result.incoming) {
                if (incoming.arm == ControlArm::LoopInitial) {
                    continue;
                }
                const ValueId sourceId = origin(incoming.value);
                const ResolvedValue* sourceValue =
                    placed_->resolved().findValue(sourceId);
                std::optional<MemoryPlacement> source =
                    valuePlacement(sourceId);
                if (!sourceValue || !source) continue;

                const TransferPayloadKind payload =
                    destinationValue->type.kind == ValueKind::Buffer
                        ? TransferPayloadKind::Buffer
                        : TransferPayloadKind::Scalar;
                const std::optional<RouteId> route =
                    createRoute(
                        sourceId, payload, sourceValue->producer,
                        result.control, *source, *destination);
                dependencies_.push_back(
                    {sourceValue->producer, result.control,
                     DependencyKind::Value, sourceId, route});
                if (sourceValue->producer) {
                    dataDependencies_.insert(
                        {*sourceValue->producer, result.control});
                }
            }
        }
    }

    void routeGraphOutputs() {
        if (!capabilities_->host()) return;
        const MemoryPlacement destination{
            *capabilities_->host(), std::nullopt};
        for (const auto& [id, value] :
             placed_->resolved().values()) {
            if (!value.graphOutput) continue;
            const ValueId sourceId = origin(id);
            const ResolvedValue* sourceValue =
                placed_->resolved().findValue(sourceId);
            std::optional<MemoryPlacement> source =
                valuePlacement(sourceId);
            if (!sourceValue || !source) continue;
            const TransferPayloadKind payload =
                sourceValue->type.kind == ValueKind::Buffer
                    ? TransferPayloadKind::Buffer
                    : TransferPayloadKind::Scalar;
            const std::optional<RouteId> route =
                createRoute(
                    sourceId, payload, sourceValue->producer,
                    std::nullopt, *source, destination);
            dependencies_.push_back(
                {sourceValue->producer, std::nullopt,
                 DependencyKind::Value, sourceId, route});
        }
    }

    void routeOrderDependencies() {
        for (const auto& [consumer, operation] :
             placed_->resolved().operations()) {
            if (operation.structural) continue;
            auto destinationPlacement =
                placed_->operationPlacements().find(consumer);
            if (destinationPlacement ==
                placed_->operationPlacements().end()) {
                continue;
            }
            for (NodeId producer : operation.dependencies) {
                if (dataDependencies_.count(
                        {producer, consumer}) != 0) {
                    continue;
                }
                const ResolvedOperation* producerOperation =
                    placed_->resolved().findOperation(producer);
                if (!producerOperation ||
                    producerOperation->structural) {
                    continue;
                }
                auto sourcePlacement =
                    placed_->operationPlacements().find(producer);
                if (sourcePlacement ==
                    placed_->operationPlacements().end()) {
                    continue;
                }
                const MemoryPlacement source{
                    sourcePlacement->second.device, std::nullopt};
                const MemoryPlacement destination{
                    destinationPlacement->second.device,
                    std::nullopt};
                const std::optional<RouteId> route =
                    createRoute(
                        std::nullopt, TransferPayloadKind::Barrier,
                        producer, consumer, source, destination);
                dependencies_.push_back(
                    {producer, consumer, DependencyKind::Order,
                     std::nullopt, route});
            }
        }
    }

    void validateMutablePlacements() {
        for (const auto& [node, operation] :
             placed_->resolved().operations()) {
            if (operation.structural) continue;
            std::vector<const ResolvedBinding*> inputs;
            std::vector<const ResolvedBinding*> outputs;
            for (const ResolvedBinding& binding :
                 operation.bindings) {
                if (binding.access == ValueAccess::InoutInput) {
                    inputs.push_back(&binding);
                } else if (
                    binding.access == ValueAccess::InoutOutput) {
                    outputs.push_back(&binding);
                }
            }
            const std::size_t pairs =
                std::min(inputs.size(), outputs.size());
            for (std::size_t i = 0; i < pairs; ++i) {
                const std::vector<MemoryPlacement> inputPlacements =
                    consumerPlacements(node, inputs[i]->value);
                auto output =
                    placed_->valuePlacements().find(
                        outputs[i]->value);
                if (inputPlacements.empty() ||
                    output ==
                        placed_->valuePlacements().end()) {
                    continue;
                }
                const MemoryPlacement& input =
                    inputPlacements.front();
                if (input.device != output->second.device ||
                    (input.region && output->second.region &&
                     input.region != output->second.region)) {
                    diagnostics_.error(
                        DiagCode::IncompatibleMemoryPlacement,
                        "GraphCompiler: inout buffer ports require one "
                        "device and memory region");
                }
            }
        }

        for (const ResolvedControlResult& result :
             placed_->resolved().controlResults()) {
            const ControlIncoming* initial = nullptr;
            const ControlIncoming* backedge = nullptr;
            for (const ControlIncoming& incoming :
                 result.incoming) {
                if (incoming.arm == ControlArm::LoopInitial) {
                    initial = &incoming;
                } else if (
                    incoming.arm == ControlArm::LoopBackedge) {
                    backedge = &incoming;
                }
            }
            if (!initial || !backedge) continue;
            std::optional<MemoryPlacement> initialPlacement =
                valuePlacement(initial->value);
            std::optional<MemoryPlacement> backedgePlacement =
                valuePlacement(backedge->value);
            if (!initialPlacement || !backedgePlacement) continue;
            if (initialPlacement->device ==
                    backedgePlacement->device &&
                initialPlacement->region &&
                backedgePlacement->region &&
                initialPlacement->region !=
                    backedgePlacement->region) {
                diagnostics_.error(
                    DiagCode::IncompatibleMemoryPlacement,
                    "GraphCompiler: loop-carried buffers cannot cross "
                    "memory regions");
            }
        }
    }

    std::shared_ptr<const PlacedGraph> placed_;
    const TransferCapabilityCatalog* capabilities_ = nullptr;
    Diagnostics diagnostics_;
    std::uint64_t nextRoute_ = 0;
    std::map<NodeId, const AuthoredOperation*> authoredOperations_;
    std::map<RegionId, std::optional<NodeId>> parentControlByRegion_;
    std::map<ValueId, ValueId> aliases_;
    std::map<RouteKey, RouteId> routeIds_;
    std::vector<DependencyEdge> dependencies_;
    std::vector<TransferRoute> routes_;
    std::set<std::pair<NodeId, NodeId>> dataDependencies_;
};

}  // namespace

CompileResult<RoutedGraph> routeGraph(
    const PlacedGraph& placed,
    const TransferCapabilityCatalog& capabilities) {
    return GraphRouter().route(placed, capabilities);
}

}  // namespace vrt::graph
