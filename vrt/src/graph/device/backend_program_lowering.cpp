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

#include <vrt/graph/backend_program_lowering.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <slash/uapi/rp1_protocol.h>

#include <vrt/graph/device/device.hpp>
#include <vrt/graph/node/compiled_node.hpp>

namespace vrt::graph {

namespace {

class BackendProgramLowerer {
   public:
    CompileResult<BackendPrograms> lower(
        const ScheduledGraph& scheduled,
        const std::map<std::string, std::shared_ptr<IDevice>>& devices,
        const BridgeLookup& bridgeFor,
        const std::shared_ptr<std::map<std::string, std::uint64_t>>&
            scalarValues) {
        scheduled_ = &scheduled;
        devices_ = &devices;
        bridgeFor_ = &bridgeFor;
        scalarValues_ = scalarValues;
        indexAuthored(
            scheduled.routed().placed().resolved().authored().root(),
            std::nullopt);
        indexQueues();
        indexRoutes();
        createRegionPrograms();

        CompileResult<BackendResourceBindings> resources =
            bindBackendResources(scheduled, devices);
        if (!resources.ok()) {
            return CompileResult<BackendPrograms>::failure(
                std::move(resources.diagnostics));
        }
        resources_ = &*resources.output;
        emitScheduledNodes();
        emitBoundaries();
        wireDependencies();
        emitHostEventRoutes();
        emitGraphIo();
        attachChildPrograms();

        if (diagnostics_.hasErrors()) {
            return CompileResult<BackendPrograms>::failure(
                std::move(diagnostics_));
        }

        std::vector<DGraph> roots;
        const RegionId rootRegion =
            scheduled.routed().placed().resolved().root().id;
        for (QueueId queue : queuesByRegion_[rootRegion]) {
            auto program = programs_.find(queue);
            if (program != programs_.end() &&
                !program->second->nodes.empty()) {
                roots.push_back(*program->second);
            }
        }
        std::sort(roots.begin(), roots.end(),
                  [](const DGraph& lhs, const DGraph& rhs) {
                      return lhs.deviceId < rhs.deviceId;
                  });

        Diagnostics diagnostics;
        diagnostics.append(std::move(resources.diagnostics));
        return CompileResult<BackendPrograms>::success(
            BackendPrograms(
                std::move(roots),
                std::move(*resources.output)),
            std::move(diagnostics));
    }

   private:
    using ConsumerValueKey = std::pair<NodeId, ValueId>;

    struct EmittedStep {
        std::string entry;
        std::string terminal;
    };

    void indexAuthored(const AuthoredRegion& region,
                       std::optional<NodeId> parentControl) {
        authoredRegions_[region.id] = &region;
        parentControlByRegion_[region.id] = parentControl;
        for (const AuthoredOperation& operation : region.operations) {
            authoredOperations_[authoredNodeId(operation)] = &operation;
            if (const auto* loop = std::get_if<AuthoredLoop>(&operation)) {
                if (loop->body) {
                    childRegions_[loop->id].push_back(
                        {DGraphChildRole::LoopBody, loop->body->id});
                    indexAuthored(*loop->body, loop->id);
                }
            } else if (const auto* conditional =
                           std::get_if<AuthoredConditional>(&operation)) {
                if (conditional->thenRegion) {
                    childRegions_[conditional->id].push_back(
                        {DGraphChildRole::ConditionalThen,
                         conditional->thenRegion->id});
                    indexAuthored(
                        *conditional->thenRegion, conditional->id);
                }
                if (conditional->elseRegion) {
                    childRegions_[conditional->id].push_back(
                        {DGraphChildRole::ConditionalElse,
                         conditional->elseRegion->id});
                    indexAuthored(
                        *conditional->elseRegion, conditional->id);
                }
            }
        }
    }

    void indexQueues() {
        for (const QueueProgram& queue : scheduled_->queues()) {
            queues_[queue.id] = &queue;
            queuesByRegion_[queue.region].push_back(queue.id);
        }
    }

    void indexRoutes() {
        for (const TransferRoute& route :
             scheduled_->routed().routes()) {
            routes_[route.requirement.id] = &route;
            if (route.requirement.value &&
                !route.legs.empty() &&
                route.legs.front().mechanism ==
                    TransferMechanism::HostMediatedDeviceCopy &&
                route.requirement.destination.operation) {
                const ResolvedValue* value =
                    scheduled_->routed()
                        .placed()
                        .resolved()
                        .findValue(*route.requirement.value);
                if (value && value->bufferToken) {
                    const GraphBuffer& source = *value->bufferToken;
                    GraphBuffer target = GraphBuffer::make(
                        source.type(),
                        source.name() + "__route_" +
                            std::to_string(
                                route.requirement.id.value()),
                        source.scopeId(), source.maybeSizeScalar());
                    copyTargets_[{
                        *route.requirement.destination.operation,
                        *route.requirement.value}] = std::move(target);
                    for (const DependencyEdge& edge :
                         scheduled_->routed().dependencies()) {
                        if (edge.route !=
                                std::optional<RouteId>(
                                    route.requirement.id) ||
                            !edge.consumer || !edge.value) {
                            continue;
                        }
                        copyTargets_[{*edge.consumer, *edge.value}] =
                            copyTargets_.at({
                                *route.requirement.destination.operation,
                                *route.requirement.value});
                    }
                }
            }
        }

        std::map<RouteId, std::size_t> nextLeg;
        for (const auto& [id, step] : scheduled_->steps()) {
            if (step.kind != ScheduledStepKind::TransferAction ||
                !step.route) {
                continue;
            }
            auto route = routes_.find(*step.route);
            if (route == routes_.end()) continue;
            const std::size_t index = nextLeg[*step.route]++;
            if (index < route->second->legs.size()) {
                actionLegs_[id] = {
                    route->second,
                    &route->second->legs[index]};
            }
        }
    }

    void createRegionPrograms() {
        syntheticQueues_.reserve(authoredRegions_.size() * 2);
        for (const QueueProgram& queue : scheduled_->queues()) {
            auto device = devices_->find(queue.device.value());
            if (device == devices_->end() || !device->second) {
                diagnostics_.error(
                    DiagCode::UnknownDevice,
                    "GraphCompiler: scheduled queue targets unknown device '" +
                        queue.device.value() + "'");
                continue;
            }
            auto program = std::make_shared<DGraph>();
            program->deviceId = queue.device.value();
            program->device = device->second;
            program->scalarValues = scalarValues_;
            program->resourcesLeased = true;
            programs_[queue.id] = std::move(program);
        }

        auto createSynthetic = [&](RegionId region, DeviceId deviceId) {
            for (QueueId existing : queuesByRegion_[region]) {
                if (queues_.at(existing)->device == deviceId) return;
            }
            QueueProgram queue;
            queue.id = QueueId(nextSyntheticQueue_++);
            queue.device = deviceId;
            queue.region = region;
            queue.parentControl = parentControlByRegion_[region];
            syntheticQueues_.push_back(queue);
            const QueueProgram* stored = &syntheticQueues_.back();
            queues_[stored->id] = stored;
            queuesByRegion_[region].push_back(stored->id);

            auto device = devices_->find(deviceId.value());
            if (device == devices_->end() || !device->second) return;
            auto program = std::make_shared<DGraph>();
            program->deviceId = deviceId.value();
            program->device = device->second;
            program->scalarValues = scalarValues_;
            program->resourcesLeased = true;
            programs_[stored->id] = std::move(program);
        };

        for (const auto& [region, authored] : authoredRegions_) {
            (void)authored;
            auto parent = parentControlByRegion_.find(region);
            if (parent == parentControlByRegion_.end() ||
                !parent->second) {
                continue;
            }
            const ControlPlacement& placement =
                scheduled_->routed()
                    .placed()
                    .controlPlacements()
                    .at(*parent->second);
            if (placement.mode ==
                ControlMode::SplitAcrossDevices) {
                for (DeviceId participant :
                     placement.participants) {
                    createSynthetic(region, participant);
                }
            } else {
                createSynthetic(region, placement.primary);
            }
        }

        const std::optional<DeviceId> host = graphHost();
        const RegionId root =
            scheduled_->routed().placed().resolved().root().id;
        bool hasHost = false;
        if (host) {
            for (QueueId queue : queuesByRegion_[root]) {
                if (queues_.at(queue)->device == *host) {
                    hasHost = true;
                    break;
                }
            }
        }
        if (host && !hasHost) {
            createSynthetic(root, *host);
        }
    }

    const QueueProgram& queueFor(ScheduleStepId step) const {
        return *queues_.at(scheduled_->steps().at(step).queue);
    }

    std::string authoredId(NodeId id) const {
        auto operation = authoredOperations_.find(id);
        return operation == authoredOperations_.end()
                   ? "node_" + std::to_string(id.value())
                   : authoredSourceId(*operation->second);
    }

    static const std::vector<AuthoredDependency>& operationAfter(
        const AuthoredOperation& operation) {
        return std::visit(
            [](const auto& concrete)
                -> const std::vector<AuthoredDependency>& {
                return concrete.after;
            },
            operation);
    }

    std::optional<std::uint32_t> physical(
        std::optional<RendezvousId> logical) const {
        if (!logical || !resources_) return std::nullopt;
        const BoundRendezvous* binding = resources_->find(*logical);
        if (!binding) return std::nullopt;
        return binding->physicalIndex;
    }

    bool routeUsesDeviceResources(RouteId route) const {
        for (const LogicalRendezvous& rendezvous :
             scheduled_->rendezvous()) {
            if (rendezvous.route != std::optional<RouteId>(route)) {
                continue;
            }
            const BoundRendezvous* binding =
                resources_->find(rendezvous.id);
            if (binding &&
                binding->kind ==
                    PhysicalRendezvousKind::DeviceResource) {
                return true;
            }
        }
        return false;
    }

    std::map<RendezvousPurpose, std::uint32_t> controlSlots(
        NodeId control) const {
        std::map<RendezvousPurpose, std::uint32_t> result;
        for (const LogicalRendezvous& rendezvous :
             scheduled_->rendezvous()) {
            if (rendezvous.control !=
                std::optional<NodeId>(control)) {
                continue;
            }
            if (auto slot = physical(rendezvous.id)) {
                result[rendezvous.purpose] = *slot;
            }
        }
        return result;
    }

    std::string outputPort(
        const AuthoredOperation& operation,
        const ResolvedValue& result) const {
        const IOMap* ioMap = std::visit(
            [](const auto& concrete) -> const IOMap* {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<T, AuthoredLoop> ||
                              std::is_same_v<T,
                                             AuthoredConditional>) {
                    return &concrete.ioMap;
                } else {
                    return nullptr;
                }
            },
            operation);
        if (ioMap && result.bufferToken) {
            for (const auto& [port, token] : ioMap->outputs()) {
                if (token.scopeId() == result.bufferToken->scopeId() &&
                    token.name() == result.bufferToken->name()) {
                    return port;
                }
            }
            for (const auto& inout : ioMap->inouts()) {
                if (inout.out.scopeId() ==
                        result.bufferToken->scopeId() &&
                    inout.out.name() == result.bufferToken->name()) {
                    return inout.outPort;
                }
            }
        }
        if (ioMap && result.scalarToken) {
            for (const auto& [port, token] :
                 ioMap->outputScalars()) {
                if (token.scopeId() == result.scalarToken->scopeId() &&
                    token.varName() == result.scalarToken->varName()) {
                    return port;
                }
            }
        }
        const std::map<std::string, GraphBuffer>* named =
            std::visit(
                [](const auto& concrete)
                    -> const std::map<std::string, GraphBuffer>* {
                    using T = std::decay_t<decltype(concrete)>;
                    if constexpr (std::is_same_v<T, AuthoredLoop> ||
                                  std::is_same_v<T,
                                                 AuthoredConditional>) {
                        return &concrete.namedOutputBuffers;
                    } else {
                        return nullptr;
                    }
                },
                operation);
        if (named && result.bufferToken) {
            for (const auto& [port, token] : *named) {
                if (token.scopeId() == result.bufferToken->scopeId() &&
                    token.name() == result.bufferToken->name()) {
                    return port;
                }
            }
        }
        return result.sourceName;
    }

    void fillControlOutputs(NodeId control,
                            CompiledLoopNode& node) const {
        const AuthoredOperation& operation =
            *authoredOperations_.at(control);
        const PlacedGraph& placed = scheduled_->routed().placed();
        for (const ResolvedControlResult& result :
             placed.resolved().controlResults()) {
            if (result.control != control) continue;
            const ResolvedValue* target =
                placed.resolved().findValue(result.result);
            if (!target) continue;
            const std::string port = outputPort(operation, *target);
            auto targetPlacement =
                placed.valuePlacements().find(result.result);
            const std::string targetDevice =
                targetPlacement == placed.valuePlacements().end()
                    ? node.deviceId
                    : targetPlacement->second.device.value();
            if (target->type.kind == ValueKind::Buffer &&
                target->bufferToken) {
                node.outputBufferPlacements[scopedBufferKey(
                    target->bufferToken->scopeId(),
                    target->bufferToken->name())] = targetDevice;
                auto incoming = std::find_if(
                    result.incoming.begin(), result.incoming.end(),
                    [](const ControlIncoming& value) {
                        return value.arm == ControlArm::LoopBackedge;
                    });
                if (incoming == result.incoming.end()) continue;
                const ResolvedValue* source =
                    placed.resolved().findValue(incoming->value);
                if (!source || !source->bufferToken) continue;
                node.outputBufferPublications.push_back({
                    port,
                    target->bufferToken->name(),
                    target->bufferToken->scopeId(),
                    source->bufferToken->name(),
                    source->bufferToken->scopeId(),
                    targetDevice});
            } else if (target->type.kind == ValueKind::Scalar &&
                       target->scalarToken) {
                node.outputScalarPlacements[scopedScalarKey(
                    target->scalarToken->scopeId(),
                    target->scalarToken->varName())] = targetDevice;
                auto incoming = std::find_if(
                    result.incoming.begin(), result.incoming.end(),
                    [](const ControlIncoming& value) {
                        return value.arm == ControlArm::LoopBackedge;
                    });
                if (incoming == result.incoming.end()) continue;
                const ResolvedValue* source =
                    placed.resolved().findValue(incoming->value);
                if (!source || !source->scalarToken) continue;
                node.outputScalarPublications.push_back({
                    port,
                    target->scalarToken->varName(),
                    target->scalarToken->scopeId(),
                    source->scalarToken->varName(),
                    source->scalarToken->scopeId(),
                    targetDevice});
            }
        }
    }

    void fillControlOutputs(NodeId control,
                            CompiledConditionalNode& node) const {
        const AuthoredOperation& operation =
            *authoredOperations_.at(control);
        const PlacedGraph& placed = scheduled_->routed().placed();
        for (const ResolvedControlResult& result :
             placed.resolved().controlResults()) {
            if (result.control != control) continue;
            const ResolvedValue* target =
                placed.resolved().findValue(result.result);
            if (!target) continue;
            const std::string port = outputPort(operation, *target);
            auto thenValue = std::find_if(
                result.incoming.begin(), result.incoming.end(),
                [](const ControlIncoming& value) {
                    return value.arm == ControlArm::ThenBranch;
                });
            auto elseValue = std::find_if(
                result.incoming.begin(), result.incoming.end(),
                [](const ControlIncoming& value) {
                    return value.arm == ControlArm::ElseBranch;
                });
            if (thenValue == result.incoming.end() ||
                elseValue == result.incoming.end()) {
                continue;
            }
            const ResolvedValue* thenSource =
                placed.resolved().findValue(thenValue->value);
            const ResolvedValue* elseSource =
                placed.resolved().findValue(elseValue->value);
            if (!thenSource || !elseSource) continue;
            auto placement =
                placed.valuePlacements().find(result.result);
            const std::string targetDevice =
                placement == placed.valuePlacements().end()
                    ? node.deviceId
                    : placement->second.device.value();
            if (target->type.kind == ValueKind::Buffer &&
                target->bufferToken && thenSource->bufferToken &&
                elseSource->bufferToken) {
                node.outputBufferPlacements[scopedBufferKey(
                    target->bufferToken->scopeId(),
                    target->bufferToken->name())] = targetDevice;
                node.outputBufferPublications.push_back({
                    port,
                    target->bufferToken->name(),
                    target->bufferToken->scopeId(),
                    thenSource->bufferToken->name(),
                    thenSource->bufferToken->scopeId(),
                    targetDevice,
                    elseSource->bufferToken->name(),
                    elseSource->bufferToken->scopeId(),
                    targetDevice});
            } else if (target->type.kind == ValueKind::Scalar &&
                       target->scalarToken && thenSource->scalarToken &&
                       elseSource->scalarToken) {
                node.outputScalarPlacements[scopedScalarKey(
                    target->scalarToken->scopeId(),
                    target->scalarToken->varName())] = targetDevice;
                node.outputScalarPublications.push_back({
                    port,
                    target->scalarToken->varName(),
                    target->scalarToken->scopeId(),
                    thenSource->scalarToken->varName(),
                    thenSource->scalarToken->scopeId(),
                    targetDevice,
                    elseSource->scalarToken->varName(),
                    elseSource->scalarToken->scopeId(),
                    targetDevice});
            }
        }
    }

    std::vector<CompiledNode> operationNodes(
        const ScheduledStep& step,
        const QueueProgram& queue) {
        if (!step.operation) return {};
        const AuthoredOperation& operation =
            *authoredOperations_.at(*step.operation);
        return std::visit(
            [&](const auto& concrete) -> std::vector<CompiledNode> {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<T, AuthoredKernel>) {
                    CompiledKernelNode node;
                    node.id = concrete.authoredId;
                    node.kernel = concrete.kernel;
                    node.deviceId = queue.device.value();
                    node.ioMap = concrete.ioMap;
                    const ResolvedOperation* resolved =
                        scheduled_->routed()
                            .placed()
                            .resolved()
                            .findOperation(concrete.id);
                    if (resolved) {
                        for (const ResolvedBinding& binding :
                             resolved->bindings) {
                            auto target = copyTargets_.find(
                                {concrete.id, binding.value});
                            if (target == copyTargets_.end() ||
                                binding.access != ValueAccess::Input) {
                                continue;
                            }
                            const std::size_t separator =
                                binding.port.find('.');
                            const std::string port =
                                separator == std::string::npos
                                    ? binding.port
                                    : binding.port.substr(separator + 1);
                            node.ioMap.rebindInputForCompiler(
                                port, target->second);
                        }
                    }
                    return {std::move(node)};
                } else if constexpr (
                    std::is_same_v<T, AuthoredReprogram>) {
                    CompiledReprogramNode node;
                    node.id = concrete.authoredId;
                    node.deviceId = queue.device.value();
                    node.imageId = concrete.imageId;
                    node.pdiPath = concrete.pdiPath;
                    node.timeoutCycles = concrete.timeoutCycles;
                    return {std::move(node)};
                } else if constexpr (
                    std::is_same_v<T, AuthoredLoop>) {
                    CompiledLoopNode node;
                    node.id = concrete.authoredId;
                    node.deviceId = queue.device.value();
                    node.loopKind =
                        concrete.kind == LoopKind::FixedCount
                            ? CompiledLoopKind::FixedCount
                            : CompiledLoopKind::WhileCondition;
                    node.tripCount = concrete.tripCount;
                    node.condition = concrete.condition;
                    const ControlPlacement& placement =
                        scheduled_->routed()
                            .placed()
                            .controlPlacements()
                            .at(concrete.id);
                    if (placement.authority &&
                        *placement.authority == queue.device) {
                        node.broadcastRole =
                            SplitBroadcastRole::Authority;
                    } else if (std::find(
                                   placement.followers.begin(),
                                   placement.followers.end(),
                                   queue.device) !=
                               placement.followers.end()) {
                        node.broadcastRole =
                            SplitBroadcastRole::Follower;
                    }
                    const auto slots = controlSlots(concrete.id);
                    auto value = slots.find(
                        RendezvousPurpose::ControlValue);
                    auto ready = slots.find(
                        RendezvousPurpose::ControlDecision);
                    auto ack = slots.find(
                        RendezvousPurpose::ControlAcknowledged);
                    if (value != slots.end()) {
                        node.conditionBroadcastSlot = value->second;
                    }
                    if (ready != slots.end()) {
                        node.broadcastReadySlot = ready->second;
                    }
                    if (ack != slots.end()) {
                        node.broadcastAckSlot = ack->second;
                    }
                    fillControlOutputs(concrete.id, node);
                    return {std::move(node)};
                } else if constexpr (
                    std::is_same_v<T, AuthoredConditional>) {
                    CompiledConditionalNode node;
                    node.id = concrete.authoredId;
                    node.deviceId = queue.device.value();
                    node.condition = concrete.condition;
                    fillControlOutputs(concrete.id, node);
                    return {std::move(node)};
                } else {
                    return {};
                }
            },
            operation);
    }

    BridgeStepPair materializeBridge(
        const TransferRoute& route, const TransferLeg& leg) {
        auto source = devices_->find(leg.source.value());
        auto destination = devices_->find(leg.destination.value());
        if (source == devices_->end() ||
            destination == devices_->end()) {
            throw std::runtime_error(
                "GraphCompiler: transfer endpoint is unavailable");
        }
        IBridge* bridge =
            (*bridgeFor_)(leg.source.value(), leg.destination.value());
        if (!bridge) {
            throw std::runtime_error(
                "GraphCompiler: transfer bridge is unavailable");
        }
        const std::string producer =
            route.requirement.source.operation
                ? authoredId(*route.requirement.source.operation)
                : "__graph_start";
        const std::string consumer =
            route.requirement.destination.operation
                ? authoredId(*route.requirement.destination.operation)
                : "__graph_end";
        if (route.requirement.payload ==
            TransferPayloadKind::Barrier) {
            return bridge->makeBarrier(
                *source->second, *destination->second,
                producer, consumer);
        }
        if (!route.requirement.value) {
            throw std::runtime_error(
                "GraphCompiler: data route has no value");
        }
        const ResolvedValue* value =
            scheduled_->routed()
                .placed()
                .resolved()
                .findValue(*route.requirement.value);
        if (!value) {
            throw std::runtime_error(
                "GraphCompiler: route value is unavailable");
        }
        if (route.requirement.payload ==
            TransferPayloadKind::Scalar) {
            if (!value->scalarToken) {
                throw std::runtime_error(
                    "GraphCompiler: scalar route has no token");
            }
            return bridge->makeScalarTransfer(
                *source->second, *destination->second,
                scopedScalarKey(
                    value->scalarToken->scopeId(),
                    value->scalarToken->varName()),
                producer, consumer);
        }
        if (!value->bufferToken) {
            throw std::runtime_error(
                "GraphCompiler: buffer route has no token");
        }
        return bridge->makeTransfer(
            *source->second, *destination->second,
            *value->bufferToken, 0, producer, consumer);
    }

    std::vector<CompiledNode> actionNodes(
        ScheduleStepId stepId, const ScheduledStep& step,
        const QueueProgram& queue) {
        auto action = actionLegs_.find(stepId);
        if (action == actionLegs_.end()) return {};
        const TransferRoute& route = *action->second.first;
        const TransferLeg& leg = *action->second.second;
        if (!routeUsesDeviceResources(route.requirement.id) &&
            leg.mechanism !=
                TransferMechanism::HostMediatedDeviceCopy) {
            return {};
        }
        const std::string stem =
            "__route_" + std::to_string(route.requirement.id.value()) +
            "_step_" + std::to_string(stepId.value());
        if (leg.mechanism ==
            TransferMechanism::HostMediatedDeviceCopy) {
            if (!route.requirement.value) return {};
            const ResolvedValue* value =
                scheduled_->routed()
                    .placed()
                    .resolved()
                    .findValue(*route.requirement.value);
            auto target = route.requirement.destination.operation
                              ? copyTargets_.find(
                                    {*route.requirement.destination.operation,
                                     *route.requirement.value})
                              : copyTargets_.end();
            if (!value || !value->bufferToken ||
                target == copyTargets_.end()) {
                throw std::runtime_error(
                    "GraphCompiler: device copy tokens are unavailable");
            }
            auto device = devices_->find(leg.source.value());
            if (device == devices_->end()) {
                throw std::runtime_error(
                    "GraphCompiler: device copy endpoint is unavailable");
            }
            CompiledDeviceCopyNode node;
            node.id = stem;
            node.deviceId = queue.device.value();
            node.source = *value->bufferToken;
            node.target = target->second;
            node.sourceRegion =
                route.requirement.source.region
                    ? route.requirement.source.region->value()
                    : "";
            node.targetRegion =
                route.requirement.destination.region
                    ? route.requirement.destination.region->value()
                    : "";
            node.action = device->second->makeDeviceCopyAction(
                node.source, node.target, node.source.type(),
                node.sourceRegion, node.targetRegion);
            return {std::move(node)};
        }

        BridgeStepPair pair = materializeBridge(route, leg);
        CompiledBridgeOpNode producer;
        producer.id = stem + "_producer";
        producer.deviceId = queue.device.value();
        producer.op = pair.op;
        producer.action = pair.producerAction;
        producer.side = CompiledBridgeOpNode::Side::Producer;
        producer.pairedKernelId =
            route.requirement.source.operation
                ? authoredId(*route.requirement.source.operation)
                : "__graph_start";

        CompiledBridgeOpNode consumer;
        consumer.id = stem + "_consumer";
        consumer.deviceId = queue.device.value();
        consumer.op = std::move(pair.op);
        consumer.action = std::move(pair.consumerAction);
        consumer.tryReady = std::move(pair.consumerTryReady);
        consumer.side = CompiledBridgeOpNode::Side::Consumer;
        consumer.pairedKernelId =
            route.requirement.destination.operation
                ? authoredId(*route.requirement.destination.operation)
                : "__graph_end";
        consumer.dependsOn = {producer.id};
        return {std::move(producer), std::move(consumer)};
    }

    std::vector<CompiledNode> eventNodes(
        const ScheduledStep& step, const QueueProgram& queue) {
        const BoundRendezvous* binding =
            step.rendezvous && resources_
                ? resources_->find(*step.rendezvous)
                : nullptr;
        if (!binding ||
            binding->kind == PhysicalRendezvousKind::HostEvent) {
            return {};
        }
        const std::optional<std::uint32_t> slot =
            physical(step.rendezvous);
        if (!slot) {
            diagnostics_.error(
                DiagCode::InternalInvariant,
                "GraphCompiler: scheduled event has no physical binding");
            return {};
        }
        const std::string stem =
            "__event_" + std::to_string(
                step.rendezvous->value()) +
            "_step_" + std::to_string(step.id.value());
        if (step.kind == ScheduledStepKind::EventPublish) {
            CompiledSignalNode signal;
            signal.id = stem + "_set";
            signal.deviceId = queue.device.value();
            signal.slot = *slot;
            signal.value = 1;
            signal.operation = RP1_SIGOP_SET;
            return {std::move(signal)};
        }
        CompiledWaitNode wait;
        wait.id = stem + "_wait";
        wait.deviceId = queue.device.value();
        wait.slot = *slot;
        wait.value = 1;
        wait.conditionOp = RP1_COP_AND_NZ;
        wait.preLaunch = step.preLaunch;

        CompiledSignalNode clear;
        clear.id = stem + "_clear";
        clear.deviceId = queue.device.value();
        clear.slot = *slot;
        clear.value = 0;
        clear.operation = RP1_SIGOP_SET;
        clear.dependsOn = {wait.id};
        return {std::move(wait), std::move(clear)};
    }

    void emitScheduledNodes() {
        for (const QueueProgram& queue : scheduled_->queues()) {
            auto program = programs_.find(queue.id);
            if (program == programs_.end()) continue;
            std::vector<CompiledNode> deferredPreLaunchNodes;
            for (ScheduleStepId stepId : queue.steps) {
                const ScheduledStep& step =
                    scheduled_->steps().at(stepId);
                std::vector<CompiledNode> nodes;
                if (step.kind == ScheduledStepKind::Operation) {
                    nodes = operationNodes(step, queue);
                } else if (
                    step.kind == ScheduledStepKind::EventPublish ||
                    step.kind == ScheduledStepKind::EventWait) {
                    nodes = eventNodes(step, queue);
                } else if (
                    step.kind == ScheduledStepKind::TransferAction) {
                    nodes = actionNodes(stepId, step, queue);
                }
                if (nodes.empty()) continue;
                if (step.operation) {
                    std::vector<std::string> authoredDependencies;
                    for (const AuthoredDependency& dependency :
                         operationAfter(
                             *authoredOperations_.at(*step.operation))) {
                        if (dependency.target) {
                            authoredDependencies.push_back(
                                authoredId(*dependency.target));
                        }
                    }
                    appendDependencies(
                        nodes.front(),
                        std::move(authoredDependencies));
                }
                EmittedStep emitted;
                emitted.entry = compiledNodeId(nodes.front());
                emitted.terminal = compiledNodeId(nodes.back());
                emittedSteps_[stepId] = emitted;
                entryNodeByStep_[stepId] = {
                    queue.id, emitted.entry};
                const bool deferPreLaunch =
                    step.preLaunch &&
                    program->second->device->type() ==
                        DeviceType::FPGA &&
                    (step.kind == ScheduledStepKind::EventPublish ||
                     step.kind == ScheduledStepKind::EventWait);
                for (CompiledNode& node : nodes) {
                    if (deferPreLaunch) {
                        deferredPreLaunchNodes.push_back(
                            std::move(node));
                    } else {
                        program->second->nodes.push_back(
                            std::move(node));
                    }
                }
            }
            for (CompiledNode& node : deferredPreLaunchNodes) {
                program->second->nodes.push_back(std::move(node));
            }
        }
    }

    std::vector<std::string> concreteDependencies(
        ScheduleStepId step, std::set<ScheduleStepId>& seen) const {
        if (!seen.insert(step).second) return {};
        auto emitted = emittedSteps_.find(step);
        if (emitted != emittedSteps_.end()) {
            return {emitted->second.terminal};
        }
        std::vector<std::string> result;
        const ScheduledStep& scheduled = scheduled_->steps().at(step);
        for (ScheduleStepId dependency : scheduled.dependencies) {
            std::vector<std::string> nested =
                concreteDependencies(dependency, seen);
            result.insert(result.end(), nested.begin(), nested.end());
        }
        return result;
    }

    static void appendDependencies(CompiledNode& node,
                                   std::vector<std::string> dependencies) {
        std::visit(
            [&](auto& concrete) {
                concrete.dependsOn.insert(
                    concrete.dependsOn.end(),
                    dependencies.begin(), dependencies.end());
                std::sort(concrete.dependsOn.begin(),
                          concrete.dependsOn.end());
                concrete.dependsOn.erase(
                    std::unique(concrete.dependsOn.begin(),
                                concrete.dependsOn.end()),
                    concrete.dependsOn.end());
            },
            node);
    }

    void wireDependencies() {
        for (const auto& [stepId, entry] : entryNodeByStep_) {
            auto program = programs_.find(entry.first);
            if (program == programs_.end()) continue;
            auto node = std::find_if(
                program->second->nodes.begin(),
                program->second->nodes.end(),
                [&](const CompiledNode& candidate) {
                    return compiledNodeId(candidate) == entry.second;
                });
            if (node == program->second->nodes.end()) continue;
            std::vector<std::string> dependencies;
            for (ScheduleStepId dependency :
                 scheduled_->steps().at(stepId).dependencies) {
                std::set<ScheduleStepId> seen;
                std::vector<std::string> concrete =
                    concreteDependencies(dependency, seen);
                dependencies.insert(dependencies.end(),
                                    concrete.begin(), concrete.end());
            }
            appendDependencies(*node, std::move(dependencies));
        }
    }

    std::set<DeviceId> operationDevices(NodeId operation) const {
        std::set<DeviceId> result;
        const PlacedGraph& placed = scheduled_->routed().placed();
        auto control = placed.controlPlacements().find(operation);
        if (control != placed.controlPlacements().end()) {
            result.insert(control->second.participants.begin(),
                          control->second.participants.end());
            return result;
        }
        auto placement = placed.operationPlacements().find(operation);
        if (placement != placed.operationPlacements().end()) {
            result.insert(placement->second.device);
        }
        return result;
    }

    bool boundaryMappingNeeded(
        const AuthoredBoundary& boundary,
        const std::string& port, DeviceId device,
        RegionId region) const {
        const ResolvedGraph& resolved =
            scheduled_->routed().placed().resolved();
        const ResolvedOperation* boundaryOperation =
            resolved.findOperation(boundary.id);
        if (!boundaryOperation) return false;
        std::optional<ValueId> source;
        std::optional<ValueId> target;
        for (const ResolvedBinding& binding :
             boundaryOperation->bindings) {
            if (binding.port != port) continue;
            if (binding.access == ValueAccess::BoundarySource) {
                source = binding.value;
            } else if (
                binding.access == ValueAccess::BoundaryTarget) {
                target = binding.value;
            }
        }
        if (!source || !target) return false;

        if (boundary.side == BoundarySide::Start) {
            bool hasConsumer = false;
            for (const auto& [node, operation] :
                 resolved.operations()) {
                if (operation.region != region ||
                    operation.structural) {
                    continue;
                }
                const bool consumes = std::any_of(
                    operation.bindings.begin(),
                    operation.bindings.end(),
                    [&](const ResolvedBinding& binding) {
                        return binding.value == *target &&
                               (binding.access == ValueAccess::Input ||
                                binding.access ==
                                    ValueAccess::InoutInput ||
                                binding.access ==
                                    ValueAccess::Condition ||
                                binding.access ==
                                    ValueAccess::TripCount ||
                                binding.access ==
                                    ValueAccess::BoundarySource);
                    });
                hasConsumer |= consumes;
                if (consumes &&
                    operationDevices(node).count(device) != 0) {
                    return true;
                }
            }
            if (hasConsumer) return false;
            const auto parent = parentControlByRegion_.find(region);
            if (parent != parentControlByRegion_.end() &&
                parent->second) {
                const ControlPlacement& placement =
                    scheduled_->routed()
                        .placed()
                        .controlPlacements()
                        .at(*parent->second);
                return placement.primary == device;
            }
            return false;
        }

        bool hasProducer = false;
        const ResolvedValue* sourceValue =
            resolved.findValue(*source);
        if (sourceValue && sourceValue->producer) {
            hasProducer = true;
            if (operationDevices(*sourceValue->producer).count(device) != 0) {
                return true;
            }
        }
        const auto parent = parentControlByRegion_.find(region);
        if (parent != parentControlByRegion_.end() &&
            parent->second) {
            for (const DependencyEdge& edge :
                 scheduled_->routed().dependencies()) {
                if (edge.value != source ||
                    edge.consumer != parent->second || !edge.route) {
                    continue;
                }
                auto route = routes_.find(*edge.route);
                if (route != routes_.end() &&
                    route->second->requirement.destination.device ==
                        device) {
                    return true;
                }
                hasProducer = true;
            }
        }
        if (!hasProducer && parent != parentControlByRegion_.end() &&
            parent->second) {
            const ControlPlacement& placement =
                scheduled_->routed()
                    .placed()
                    .controlPlacements()
                    .at(*parent->second);
            return placement.primary == device;
        }
        return false;
    }

    void emitBoundaries() {
        for (const auto& [regionId, region] : authoredRegions_) {
            auto queues = queuesByRegion_.find(regionId);
            if (queues == queuesByRegion_.end()) continue;
            for (const AuthoredOperation& operation : region->operations) {
                const auto* boundary =
                    std::get_if<AuthoredBoundary>(&operation);
                if (!boundary) continue;
                for (QueueId queue : queues->second) {
                    auto program = programs_.find(queue);
                    if (program == programs_.end()) continue;
                    auto parent =
                        parentControlByRegion_.find(regionId);
                    if (parent != parentControlByRegion_.end() &&
                        parent->second) {
                        const ControlPlacement& placement =
                            scheduled_->routed()
                                .placed()
                                .controlPlacements()
                                .at(*parent->second);
                        const DeviceId device(
                            program->second->deviceId);
                        const bool allowed =
                            placement.mode ==
                                    ControlMode::SplitAcrossDevices
                                ? std::find(
                                      placement.participants.begin(),
                                      placement.participants.end(),
                                      device) !=
                                      placement.participants.end()
                                : placement.primary == device;
                        if (!allowed) continue;
                    }
                    CompiledBoundaryNode node;
                    node.id = boundary->authoredId;
                    node.deviceId = program->second->deviceId;
                    node.side =
                        boundary->side == BoundarySide::Start
                            ? CompiledBoundaryNode::Side::Start
                            : CompiledBoundaryNode::Side::End;
                    for (std::size_t i = 0;
                         i < boundary->scalarMappings.size(); ++i) {
                        const auto& mapping =
                            boundary->scalarMappings[i];
                        if (!boundaryMappingNeeded(
                                *boundary,
                                "scalar." + std::to_string(i),
                                DeviceId(program->second->deviceId),
                                regionId)) {
                            continue;
                        }
                        node.scalarCopies.push_back({
                            mapping.source.varName(),
                            mapping.source.scopeId(),
                            mapping.target.varName(),
                            mapping.target.scopeId()});
                    }
                    for (std::size_t i = 0;
                         i < boundary->bufferMappings.size(); ++i) {
                        const auto& mapping =
                            boundary->bufferMappings[i];
                        if (!boundaryMappingNeeded(
                                *boundary,
                                "buffer." + std::to_string(i),
                                DeviceId(program->second->deviceId),
                                regionId)) {
                            continue;
                        }
                        node.bufferCopies.push_back({
                            mapping.source.name(),
                            mapping.source.scopeId(),
                            mapping.target.name(),
                            mapping.target.scopeId()});
                    }
                    for (const AuthoredDependency& dependency :
                         boundary->after) {
                        if (dependency.target) {
                            node.dependsOn.push_back(
                                authoredId(*dependency.target));
                        }
                    }
                    const ResolvedOperation* resolvedBoundary =
                        scheduled_->routed()
                            .placed()
                            .resolved()
                            .findOperation(boundary->id);
                    if (resolvedBoundary) {
                        for (NodeId dependency :
                             resolvedBoundary->dependencies) {
                            node.dependsOn.push_back(
                                authoredId(dependency));
                        }
                    }
                    std::sort(node.dependsOn.begin(),
                              node.dependsOn.end());
                    node.dependsOn.erase(
                        std::unique(node.dependsOn.begin(),
                                    node.dependsOn.end()),
                        node.dependsOn.end());
                    if ((!boundary->scalarMappings.empty() ||
                         !boundary->bufferMappings.empty()) &&
                        node.scalarCopies.empty() &&
                        node.bufferCopies.empty()) {
                        continue;
                    }
                    if (node.side ==
                        CompiledBoundaryNode::Side::Start) {
                        for (CompiledNode& existing :
                             program->second->nodes) {
                            appendDependencies(
                                existing, {node.id});
                        }
                        program->second->nodes.insert(
                            program->second->nodes.begin(),
                            std::move(node));
                    } else {
                        program->second->nodes.push_back(
                            std::move(node));
                    }
                }
            }
        }
    }

    QueueId programQueue(RegionId region, DeviceId device) const {
        auto queues = queuesByRegion_.find(region);
        if (queues == queuesByRegion_.end()) {
            throw std::runtime_error(
                "GraphCompiler: transfer region has no device program");
        }
        for (QueueId queue : queues->second) {
            if (queues_.at(queue)->device == device) return queue;
        }
        throw std::runtime_error(
            "GraphCompiler: transfer device has no region program");
    }

    void addNodeDependency(QueueId queue, const std::string& nodeId,
                           const std::string& dependency) {
        auto program = programs_.find(queue);
        if (program == programs_.end()) return;
        auto node = std::find_if(
            program->second->nodes.begin(),
            program->second->nodes.end(),
            [&](const CompiledNode& candidate) {
                return compiledNodeId(candidate) == nodeId;
            });
        if (node != program->second->nodes.end()) {
            appendDependencies(*node, {dependency});
        }
    }

    void removeNodeDependency(QueueId queue, const std::string& nodeId,
                              const std::string& dependency) {
        auto program = programs_.find(queue);
        if (program == programs_.end()) return;
        auto node = std::find_if(
            program->second->nodes.begin(),
            program->second->nodes.end(),
            [&](const CompiledNode& candidate) {
                return compiledNodeId(candidate) == nodeId;
            });
        if (node == program->second->nodes.end()) return;
        std::visit(
            [&](auto& concrete) {
                concrete.dependsOn.erase(
                    std::remove(concrete.dependsOn.begin(),
                                concrete.dependsOn.end(), dependency),
                    concrete.dependsOn.end());
            },
            *node);
    }

    void insertBefore(QueueId queue, const std::string& anchor,
                      CompiledNode node) {
        auto& nodes = programs_.at(queue)->nodes;
        auto position = std::find_if(
            nodes.begin(), nodes.end(),
            [&](const CompiledNode& candidate) {
                return compiledNodeId(candidate) == anchor;
            });
        nodes.insert(position, std::move(node));
    }

    void insertAfter(QueueId queue, const std::string& anchor,
                     CompiledNode node) {
        auto& nodes = programs_.at(queue)->nodes;
        auto position = std::find_if(
            nodes.begin(), nodes.end(),
            [&](const CompiledNode& candidate) {
                return compiledNodeId(candidate) == anchor;
            });
        if (position != nodes.end()) ++position;
        nodes.insert(position, std::move(node));
    }

    void emitHostEventRoutes() {
        const RegionId root =
            scheduled_->routed().placed().resolved().root().id;
        for (const TransferRoute& route :
             scheduled_->routed().routes()) {
            if (routeUsesDeviceResources(route.requirement.id)) continue;
            if (!route.legs.empty() &&
                route.legs.front().mechanism ==
                    TransferMechanism::HostMediatedDeviceCopy) {
                continue;
            }
            std::string previousConsumer;
            for (std::size_t index = 0; index < route.legs.size();
                 ++index) {
                const TransferLeg& leg = route.legs[index];
                const RegionId sourceRegion =
                    leg.source == route.requirement.source.device
                        ? (route.requirement.source.operation
                               ? scheduled_->routed()
                                     .placed()
                                     .resolved()
                                     .findOperation(
                                         *route.requirement.source.operation)
                                     ->region
                               : root)
                        : root;
                RegionId destinationRegion =
                    leg.destination ==
                            route.requirement.destination.device
                        ? (route.requirement.destination.operation
                               ? scheduled_->routed()
                                     .placed()
                                     .resolved()
                                     .findOperation(
                                         *route.requirement.destination.operation)
                                     ->region
                               : root)
                        : root;
                if (route.requirement.destination.operation) {
                    auto parent =
                        parentControlByRegion_.find(sourceRegion);
                    if (parent != parentControlByRegion_.end() &&
                        parent->second ==
                            route.requirement.destination.operation) {
                        destinationRegion = sourceRegion;
                    }
                }
                const QueueId sourceQueue =
                    programQueue(sourceRegion, leg.source);
                const QueueId destinationQueue =
                    programQueue(destinationRegion, leg.destination);
                BridgeStepPair pair = materializeBridge(route, leg);
                const std::string stem =
                    "__route_" +
                    std::to_string(route.requirement.id.value()) +
                    "_leg_" + std::to_string(index);

                CompiledBridgeOpNode producer;
                producer.id = stem + "_producer";
                producer.deviceId = leg.source.value();
                producer.op = pair.op;
                producer.action = pair.producerAction;
                producer.side = CompiledBridgeOpNode::Side::Producer;
                producer.pairedKernelId =
                    route.requirement.source.operation
                        ? authoredId(
                              *route.requirement.source.operation)
                        : "__graph_start";
                if (route.requirement.source.operation &&
                    index == 0) {
                    producer.dependsOn.push_back(
                        producer.pairedKernelId);
                }
                if (!previousConsumer.empty()) {
                    producer.dependsOn.push_back(previousConsumer);
                }

                CompiledBridgeOpNode consumer;
                consumer.id = stem + "_consumer";
                consumer.deviceId = leg.destination.value();
                consumer.op = std::move(pair.op);
                consumer.action = std::move(pair.consumerAction);
                consumer.tryReady = std::move(pair.consumerTryReady);
                consumer.side = CompiledBridgeOpNode::Side::Consumer;
                consumer.pairedKernelId =
                    route.requirement.destination.operation
                        ? authoredId(
                              *route.requirement.destination.operation)
                        : "__graph_end";
                consumer.dependsOn = {producer.id};

                if (route.requirement.source.operation && index == 0) {
                    insertAfter(
                        sourceQueue,
                        authoredId(*route.requirement.source.operation),
                        std::move(producer));
                } else {
                    programs_.at(sourceQueue)->nodes.push_back(
                        std::move(producer));
                }
                if (route.requirement.destination.operation &&
                    index + 1 == route.legs.size()) {
                    insertBefore(
                        destinationQueue,
                        authoredId(
                            *route.requirement.destination.operation),
                        std::move(consumer));
                } else {
                    programs_.at(destinationQueue)->nodes.push_back(
                        std::move(consumer));
                }
                previousConsumer = stem + "_consumer";
                if (index + 1 == route.legs.size()) {
                    for (const DependencyEdge& edge :
                         scheduled_->routed().dependencies()) {
                        if (edge.route !=
                                std::optional<RouteId>(
                                    route.requirement.id) ||
                            !edge.consumer) {
                            continue;
                        }
                        const ResolvedOperation* consumerOperation =
                            scheduled_->routed()
                                .placed()
                                .resolved()
                                .findOperation(*edge.consumer);
                        if (!consumerOperation) continue;
                        const QueueId consumerQueue = programQueue(
                            consumerOperation->region,
                            route.requirement.destination.device);
                        const std::string consumerId =
                            authoredId(*edge.consumer);
                        addNodeDependency(
                            consumerQueue, consumerId,
                            previousConsumer);
                        if (edge.producer) {
                            removeNodeDependency(
                                consumerQueue, consumerId,
                                authoredId(*edge.producer));
                        }
                    }
                }
            }
        }
    }

    std::optional<DeviceId> graphHost() const {
        for (const auto& [name, device] : *devices_) {
            if (device &&
                device->compilerCapabilities().hostsGraphIo) {
                return DeviceId(name);
            }
        }
        return std::nullopt;
    }

    void emitGraphIo() {
        const std::optional<DeviceId> host = graphHost();
        if (!host) return;
        const RegionId root =
            scheduled_->routed().placed().resolved().root().id;
        const QueueId queue = [&]() {
            for (QueueId candidate : queuesByRegion_[root]) {
                if (queues_.at(candidate)->device == *host) {
                    return candidate;
                }
            }
            throw std::runtime_error(
                "GraphCompiler: graph host has no root queue");
        }();
        auto program = programs_.at(queue);

        CompiledSourceNode source;
        source.id = "__graph_start";
        source.deviceId = host->value();
        CompiledSinkNode sink;
        sink.id = "__graph_end";
        sink.deviceId = host->value();
        const ResolvedGraph& resolved =
            scheduled_->routed().placed().resolved();
        for (const auto& [id, value] : resolved.values()) {
            if (value.definition == ValueDefinitionKind::GraphInput) {
                if (value.bufferToken) {
                    source.inputBufferKeys.push_back(scopedBufferKey(
                        value.bufferToken->scopeId(),
                        value.bufferToken->name()));
                } else if (value.scalarToken) {
                    source.inputScalarKeys.push_back(scopedScalarKey(
                        value.scalarToken->scopeId(),
                        value.scalarToken->varName()));
                }
            }
            if (value.graphOutput) {
                if (value.bufferToken) {
                    sink.outputBufferKeys.push_back(scopedBufferKey(
                        value.bufferToken->scopeId(),
                        value.bufferToken->name()));
                } else if (value.scalarToken) {
                    sink.outputScalarKeys.push_back(scopedScalarKey(
                        value.scalarToken->scopeId(),
                        value.scalarToken->varName()));
                }
                if (value.producer) {
                    sink.dependsOn.push_back(authoredId(*value.producer));
                }
            }
            (void)id;
        }
        auto sortUnique = [](auto& values) {
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()),
                         values.end());
        };
        sortUnique(source.inputBufferKeys);
        sortUnique(source.inputScalarKeys);
        sortUnique(sink.outputBufferKeys);
        sortUnique(sink.outputScalarKeys);
        sortUnique(sink.dependsOn);
        for (const auto& [nodeId, operation] :
             resolved.operations()) {
            if (operation.region != root) continue;
            const bool consumesInput = std::any_of(
                operation.bindings.begin(), operation.bindings.end(),
                [&](const ResolvedBinding& binding) {
                    const ResolvedValue* value =
                        resolved.findValue(binding.value);
                    return value &&
                           value->definition ==
                               ValueDefinitionKind::GraphInput &&
                           (binding.access == ValueAccess::Input ||
                            binding.access == ValueAccess::InoutInput ||
                            binding.access == ValueAccess::Condition ||
                            binding.access == ValueAccess::TripCount ||
                            binding.access ==
                                ValueAccess::BoundarySource);
                });
            if (consumesInput) {
                addNodeDependency(
                    queue, authoredId(nodeId), source.id);
            }
        }
        for (CompiledNode& node : program->nodes) {
            const auto* bridge =
                std::get_if<CompiledBridgeOpNode>(&node);
            if (bridge &&
                bridge->side ==
                    CompiledBridgeOpNode::Side::Producer &&
                bridge->pairedKernelId == source.id) {
                appendDependencies(node, {source.id});
            }
        }
        program->nodes.insert(program->nodes.begin(), std::move(source));
        program->nodes.push_back(std::move(sink));
    }

    void attachChildPrograms() {
        for (const auto& [control, children] : childRegions_) {
            const std::string parentId = authoredId(control);
            const ControlPlacement& placement =
                scheduled_->routed()
                    .placed()
                    .controlPlacements()
                    .at(control);
            for (const auto& [role, region] : children) {
                for (const auto& [queueId, program] : programs_) {
                    const ResolvedOperation* controlOperation =
                        scheduled_->routed()
                            .placed()
                            .resolved()
                            .findOperation(control);
                    if (!controlOperation ||
                        queues_.at(queueId)->region !=
                            controlOperation->region) {
                        continue;
                    }
                    auto parentNode = std::find_if(
                        program->nodes.begin(), program->nodes.end(),
                        [&](const CompiledNode& node) {
                            return compiledNodeId(node) == parentId;
                        });
                    if (parentNode == program->nodes.end()) continue;
                    DGraphChild child;
                    child.parentNodeId = parentId;
                    child.role = role;
                    for (QueueId childQueue : queuesByRegion_[region]) {
                        auto childProgram = programs_.find(childQueue);
                        if (childProgram == programs_.end()) continue;
                        if (placement.mode ==
                                ControlMode::SplitAcrossDevices &&
                            childProgram->second->deviceId !=
                                program->deviceId) {
                            continue;
                        }
                        child.dgraphs.push_back(childProgram->second);
                    }
                    if (!child.dgraphs.empty()) {
                        program->childDGraphs.push_back(std::move(child));
                    }
                }
            }
        }
    }

    const ScheduledGraph* scheduled_ = nullptr;
    const std::map<std::string, std::shared_ptr<IDevice>>* devices_ =
        nullptr;
    const BridgeLookup* bridgeFor_ = nullptr;
    std::shared_ptr<std::map<std::string, std::uint64_t>> scalarValues_;
    const BackendResourceBindings* resources_ = nullptr;
    Diagnostics diagnostics_;
    std::map<RegionId, const AuthoredRegion*> authoredRegions_;
    std::map<RegionId, std::optional<NodeId>> parentControlByRegion_;
    std::map<NodeId, const AuthoredOperation*> authoredOperations_;
    std::map<NodeId,
             std::vector<std::pair<DGraphChildRole, RegionId>>>
        childRegions_;
    std::map<QueueId, const QueueProgram*> queues_;
    std::map<RegionId, std::vector<QueueId>> queuesByRegion_;
    std::map<RouteId, const TransferRoute*> routes_;
    std::map<ScheduleStepId,
             std::pair<const TransferRoute*, const TransferLeg*>>
        actionLegs_;
    std::map<ConsumerValueKey, GraphBuffer> copyTargets_;
    std::map<QueueId, std::shared_ptr<DGraph>> programs_;
    std::map<ScheduleStepId, EmittedStep> emittedSteps_;
    std::map<ScheduleStepId, std::pair<QueueId, std::string>>
        entryNodeByStep_;
    std::vector<QueueProgram> syntheticQueues_;
    std::uint64_t nextSyntheticQueue_ = 1ULL << 63;
};

}  // namespace

CompileResult<BackendPrograms> lowerBackendPrograms(
    const ScheduledGraph& scheduled,
    const std::map<std::string, std::shared_ptr<IDevice>>& devices,
    const BridgeLookup& bridgeFor,
    const std::shared_ptr<std::map<std::string, std::uint64_t>>&
        scalarValues) {
    try {
        return BackendProgramLowerer().lower(
            scheduled, devices, bridgeFor, scalarValues);
    } catch (const std::runtime_error& error) {
        Diagnostics diagnostics;
        diagnostics.error(
            DiagCode::UnsupportedOperation, error.what());
        return CompileResult<BackendPrograms>::failure(
            std::move(diagnostics));
    }
}

}  // namespace vrt::graph
