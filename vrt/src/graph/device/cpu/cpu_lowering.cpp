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

#include <vrt/graph/device/cpu/cpu_lowering.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <vrt/graph/backend_resource_binding.hpp>
#include <vrt/graph/backend_runtime.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>

namespace vrt::graph {

namespace {

using AuthoredIndex = std::map<NodeId, const AuthoredOperation*>;

void indexAuthored(
    const AuthoredRegion& region, AuthoredIndex& operations) {
    for (const AuthoredOperation& operation : region.operations) {
        operations[authoredNodeId(operation)] = &operation;
        if (const auto* loop = std::get_if<AuthoredLoop>(&operation)) {
            if (loop->body) indexAuthored(*loop->body, operations);
        } else if (const auto* conditional =
                       std::get_if<AuthoredConditional>(&operation)) {
            if (conditional->thenRegion) {
                indexAuthored(*conditional->thenRegion, operations);
            }
            if (conditional->elseRegion) {
                indexAuthored(*conditional->elseRegion, operations);
            }
        }
    }
}

const ValueReplica* routeSourceReplica(
    const ScheduledGraph& scheduled, const TransferRoute& route) {
    const std::optional<ReplicaId> replica =
        transferReplica(route.requirement.signature.source);
    return replica ? scheduled.routed().findReplica(*replica) : nullptr;
}

std::optional<ValueId> routeValue(
    const ScheduledGraph& scheduled, const TransferRoute& route) {
    const ValueReplica* replica = routeSourceReplica(scheduled, route);
    return replica ? std::optional<ValueId>(replica->value) : std::nullopt;
}

using CopyTargetKey = std::pair<NodeId, ValueId>;

/*
 * Only host-mediated or isolated routes need private destination storage.
 * For each usable buffer route, synthesize one stable route token and index
 * both the destination anchor and every dependency consumer that must bind to
 * that copy. Incomplete route/value metadata contributes no target.
 */
std::map<CopyTargetKey, GraphBuffer> copyTargets(
    const ScheduledGraph& scheduled) {
    std::map<CopyTargetKey, GraphBuffer> result;
    for (const TransferRoute& route : scheduled.routed().routes()) {
        if (route.legs.empty() ||
            (route.legs.front().mechanism !=
                 TransferMechanism::HostMediatedDeviceCopy &&
             !route.requirement.isolatedDestination)) {
            continue;
        }
        const std::optional<ValueId> valueId =
            routeValue(scheduled, route);
        const std::optional<NodeId> destination =
            route.requirement.destinationAnchor.operation();
        const ResolvedValue* value =
            valueId ? scheduled.routed().placed().resolved().findValue(*valueId)
                    : nullptr;
        const GraphBuffer* source =
            value ? resolvedBufferToken(*value) : nullptr;
        if (!valueId || !destination || !source) continue;
        GraphBuffer target = ::vrt::graph::detail::makeGraphBuffer(
            source->type(),
            source->name() + "__route_" +
                std::to_string(route.id.value()),
            source->scopeId(), source->maybeSizeScalar(),
            source->graphId());
        result[{*destination, *valueId}] = target;
        for (const DependencyEdge& edge :
             scheduled.routed().dependencies()) {
            const auto* data = std::get_if<ValueDependencyEdge>(&edge);
            const std::optional<NodeId> consumer =
                dependencyConsumer(edge);
            if (!data || !consumer ||
                dependencyRoute(edge) !=
                    std::optional<RouteId>(route.id)) {
                continue;
            }
            const ValueReplica* replica =
                scheduled.routed().findReplica(data->target);
            if (replica) {
                result[{*consumer, replica->value}] = target;
            }
        }
    }
    return result;
}

DeviceId valueDevice(
    const ScheduledGraph& scheduled, ValueId value,
    DeviceId fallback) {
    const ValueReplica* replica =
        scheduled.routed().placed().primaryReplica(value);
    return replica ? replica->memory.device : fallback;
}

/*
 * A control publication is lowerable only when:
 * - the target and selected incoming values all resolve;
 * - a missing second arm follows the lowering convention of reusing the first;
 * - target and sources are local to this CPU queue's device;
 * - all tokens are buffers or all are scalars.
 * Otherwise no CPU publication record is emitted.
 */
std::optional<CpuControlPublication> publication(
    const ScheduledGraph& scheduled,
    const ResolvedControlResult& result, ControlArm firstArm,
    std::optional<ControlArm> secondArm, DeviceId fallback) {
    const ResolvedGraph& resolved =
        scheduled.routed().placed().resolved();
    const ResolvedValue* target = resolved.findValue(result.result);
    auto first = std::find_if(
        result.incoming.begin(), result.incoming.end(),
        [&](const ControlIncoming& incoming) {
            return incoming.arm == firstArm;
        });
    auto second = secondArm
                      ? std::find_if(
                            result.incoming.begin(), result.incoming.end(),
                            [&](const ControlIncoming& incoming) {
                                return incoming.arm == *secondArm;
                            })
                      : first;
    if (secondArm && second == result.incoming.end()) {
        second = first;
    }
    if (!target || first == result.incoming.end()) {
        return std::nullopt;
    }
    const ResolvedValue* firstValue = resolved.findValue(first->value);
    const ResolvedValue* secondValue = resolved.findValue(second->value);
    if (!firstValue || !secondValue) return std::nullopt;
    const DeviceId targetDevice =
        valueDevice(scheduled, result.result, fallback);
    const DeviceId firstDevice =
        valueDevice(scheduled, first->value, fallback);
    const DeviceId secondDevice =
        valueDevice(scheduled, second->value, fallback);
    if (targetDevice != fallback || firstDevice != fallback ||
        secondDevice != fallback) {
        return std::nullopt;
    }
    if (const GraphBuffer* targetToken = resolvedBufferToken(*target)) {
        const GraphBuffer* firstToken = resolvedBufferToken(*firstValue);
        const GraphBuffer* secondToken = resolvedBufferToken(*secondValue);
        if (!firstToken || !secondToken) return std::nullopt;
        return CpuControlPublication{
            *targetToken, *firstToken, *secondToken,
            targetDevice, firstDevice, secondDevice};
    }
    const GraphScalar* targetToken = resolvedScalarToken(*target);
    const GraphScalar* firstToken = resolvedScalarToken(*firstValue);
    const GraphScalar* secondToken = resolvedScalarToken(*secondValue);
    if (!targetToken || !firstToken || !secondToken) return std::nullopt;
    return CpuControlPublication{
        *targetToken, *firstToken, *secondToken,
        targetDevice, firstDevice, secondDevice};
}

void appendPublications(
    const BackendLoweringContext& context, NodeId control,
    ControlArm first, std::optional<ControlArm> second,
    std::vector<CpuControlPublication>& out) {
    for (const ResolvedControlResult& result :
         context.scheduled.routed().placed().resolved().controlResults()) {
        if (result.control != control) continue;
        std::optional<CpuControlPublication> lowered =
            publication(context.scheduled, result, first, second,
                        context.queue.device);
        if (lowered) out.push_back(std::move(*lowered));
    }
}

/*
 * Split-loop metadata has three queue cases: authority, a matching follower,
 * or a non-participant. A participant must resolve value, decision, and
 * acknowledgement to physical resources owned by one device. Placement
 * currently gives the CPU the authority role; the recorded follower data
 * keeps lowering validation symmetric with other backends.
 */
void applyControlProtocol(
    const BackendLoweringContext& context, NodeId control,
    CpuLoopCommand& loop) {
    for (const SplitControlProtocol& protocol :
         context.scheduled.splitControls()) {
        if (protocol.control != control) continue;
        const SplitControlFollowerProtocol* follower = nullptr;
        if (context.queue.id == protocol.authorityQueue &&
            !protocol.followers.empty()) {
            follower = &protocol.followers.front();
            loop.role = CpuControlRole::Authority;
        } else {
            auto it = std::find_if(
                protocol.followers.begin(), protocol.followers.end(),
                [&](const SplitControlFollowerProtocol& candidate) {
                    return candidate.queue == context.queue.id;
                });
            if (it != protocol.followers.end()) {
                follower = &*it;
                loop.role = CpuControlRole::Follower;
            }
        }
        if (!follower) return;
        const BoundRendezvous* value =
            context.resources.find(follower->value);
        const BoundRendezvous* decision =
            context.resources.find(follower->decision);
        const BoundRendezvous* acknowledgement =
            context.resources.find(follower->acknowledgement);
        if (!value || !decision || !acknowledgement) {
            throw std::logic_error(
                "CpuLowering: split control has unbound rendezvous");
        }
        if (value->owner != decision->owner ||
            value->owner != acknowledgement->owner) {
            throw std::logic_error(
                "CpuLowering: split-control resources have multiple owners");
        }
        loop.resourceOwner = value->owner;
        loop.value = value->physical;
        loop.decision = decision->physical;
        loop.acknowledgement = acknowledgement->physical;
        return;
    }
}

/*
 * Authored operation lowering covers every variant:
 * - kernels retain typed bindings and rebind private routed inputs;
 * - loops retain control shape, split protocol, and result publications;
 * - conditionals retain predicates and branch publications;
 * - FPGA reprogram operations are rejected on CPU;
 * - marker operations become explicit no-ops.
 */
CpuProgramNode lowerOperation(
    const BackendLoweringContext& context, ScheduleStepId step,
    const ScheduledOperation& scheduled, const AuthoredIndex& authored,
    const std::map<CopyTargetKey, GraphBuffer>& targets) {
    CpuProgramNode node;
    node.step = step;
    const AuthoredOperation& operation = *authored.at(scheduled.operation);
    node.diagnosticId = authoredSourceId(operation);
    std::visit(
        [&](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, AuthoredKernel>) {
                node.kind = CpuProgramNodeKind::Kernel;
                node.kernel = {concrete.kernel, concrete.ioMap};
                const ResolvedOperation* resolved =
                    context.scheduled.routed().placed().resolved().findOperation(
                        concrete.id);
                if (!resolved) return;
                for (const ResolvedBinding& binding : resolved->bindings) {
                    auto target = targets.find({concrete.id, binding.value});
                    if (target != targets.end() &&
                        binding.access == ValueAccess::Input) {
                        node.kernel.ioMap.rebindInputForCompiler(
                            binding.localPort.value(), target->second);
                    } else if (
                        target != targets.end() &&
                        binding.access ==
                            ValueAccess::InoutInput) {
                        node.kernel.ioMap
                            .rebindInoutInputForCompiler(
                                binding.localPort.value(),
                                target->second);
                    }
                }
            } else if constexpr (std::is_same_v<T, AuthoredLoop>) {
                node.kind = CpuProgramNodeKind::Loop;
                node.loop.control = concrete.id;
                node.loop.diagnosticId = concrete.authoredId;
                node.loop.kind = concrete.kind;
                node.loop.tripCount = concrete.tripCount;
                node.loop.condition = concrete.condition;
                applyControlProtocol(context, concrete.id, node.loop);
                appendPublications(
                    context, concrete.id, ControlArm::LoopBackedge,
                    ControlArm::LoopInitial, node.loop.publications);
            } else if constexpr (
                std::is_same_v<T, AuthoredConditional>) {
                node.kind = CpuProgramNodeKind::Conditional;
                node.conditional.control = concrete.id;
                node.conditional.diagnosticId = concrete.authoredId;
                node.conditional.condition = concrete.condition;
                appendPublications(
                    context, concrete.id, ControlArm::ThenBranch,
                    ControlArm::ElseBranch,
                    node.conditional.publications);
            } else if constexpr (std::is_same_v<T, AuthoredReprogram>) {
                throw std::runtime_error(
                    "CpuDevice: reprogram nodes must execute on an FPGA device");
            } else {
                node.kind = CpuProgramNodeKind::Noop;
            }
        },
        operation);
    return node;
}

/*
 * Boundary materialization copies only pairs whose replicas and values still
 * resolve. Buffer pairs become storage copies, scalar pairs become bit copies,
 * and mismatched or missing token kinds are ignored as incomplete metadata.
 */
CpuProgramNode lowerBoundary(
    const BackendLoweringContext& context, ScheduleStepId step,
    const ScheduledBoundaryMaterialization& boundary) {
    CpuProgramNode node;
    node.step = step;
    node.kind = CpuProgramNodeKind::Boundary;
    node.diagnosticId =
        "boundary:" + std::to_string(boundary.boundary.value());
    const PlacedGraph& placed = context.scheduled.routed().placed();
    for (const ScheduledBoundaryMapping& mapping : boundary.mappings) {
        const ValueReplica* sourceReplica =
            placed.findReplica(mapping.source);
        const ValueReplica* targetReplica =
            placed.findReplica(mapping.target);
        const ResolvedValue* source =
            sourceReplica
                ? placed.resolved().findValue(sourceReplica->value)
                : nullptr;
        const ResolvedValue* target =
            targetReplica
                ? placed.resolved().findValue(targetReplica->value)
                : nullptr;
        if (!source || !target) continue;
        if (const GraphBuffer* sourceToken =
                resolvedBufferToken(*source)) {
            const GraphBuffer* targetToken =
                resolvedBufferToken(*target);
            if (targetToken) {
                node.boundary.bufferCopies.push_back(
                    {*sourceToken, *targetToken});
            }
        } else if (const GraphScalar* sourceToken =
                       resolvedScalarToken(*source)) {
            const GraphScalar* targetToken =
                resolvedScalarToken(*target);
            if (targetToken) {
                node.boundary.scalarCopies.push_back(
                    {*sourceToken, *targetToken});
            }
        }
    }
    return node;
}

/*
 * Runtime-step lowering has four cases:
 * - device-backed event publishes and waits become Signal/Wait nodes;
 * - host events need no CPU command;
 * - steps without host actions become no-ops;
 * - transfer steps copy all matching actions into producer or consumer nodes.
 * Consumers require every probe before ordered work runs.
 */
CpuProgramNode lowerRuntimeStep(
    const BackendLoweringContext& context, const ScheduledStep& step) {
    CpuProgramNode node;
    node.step = step.id;
    node.diagnosticId =
        "step:" + std::to_string(step.id.value());
    if (const auto* publish =
            std::get_if<ScheduledEventPublish>(&step.payload)) {
        const BoundRendezvous* binding =
            context.resources.find(publish->rendezvous);
        if (binding &&
            binding->kind != PhysicalRendezvousKind::HostEvent) {
            node.kind = CpuProgramNodeKind::Signal;
            node.resourceOwner = binding->owner;
            node.signalResource = binding->physical;
            node.signalSlot =
                static_cast<std::uint32_t>(binding->physical.value());
            node.signalValue = 1;
        } else {
            node.kind = CpuProgramNodeKind::Noop;
        }
        return node;
    }
    if (const auto* wait =
            std::get_if<ScheduledEventWait>(&step.payload)) {
        const BoundRendezvous* binding =
            context.resources.find(wait->rendezvous);
        if (binding &&
            binding->kind != PhysicalRendezvousKind::HostEvent) {
            node.kind = CpuProgramNodeKind::Wait;
            node.resourceOwner = binding->owner;
            node.signalResource = binding->physical;
            node.signalSlot =
                static_cast<std::uint32_t>(binding->physical.value());
            node.signalValue = 1;
        } else {
            node.kind = CpuProgramNodeKind::Noop;
        }
        return node;
    }
    const std::vector<HostActionId>& actions =
        context.hostActions.find(step.id);
    if (actions.empty()) {
        node.kind = CpuProgramNodeKind::Noop;
        return node;
    }
    const bool consumer =
        std::holds_alternative<ScheduledTransferConsume>(step.payload);
    node.kind = consumer ? CpuProgramNodeKind::ConsumerOp
                         : CpuProgramNodeKind::ProducerOp;
    std::vector<std::function<bool()>> readiness;
    std::vector<std::function<void()>> work;
    readiness.reserve(actions.size());
    work.reserve(actions.size());
    node.actionPins.reserve(actions.size());
    /*
     * Copy callbacks out of the assembly-time table and pin each operation.
     * This makes the CpuProgram self-contained after lowering returns.
     */
    for (HostActionId id : actions) {
        const HostAction& action = context.hostActions.at(id);
        node.actionPins.push_back(action.operation);
        readiness.push_back(action.tryReady);
        work.push_back(action.action);
    }
    node.tryReady = [readiness = std::move(readiness)] {
        return std::all_of(
            readiness.begin(), readiness.end(),
            [](const std::function<bool()>& ready) {
                return !ready || ready();
            });
    };
    node.action = [work = std::move(work)] {
        for (const std::function<void()>& action : work) {
            if (action) action();
        }
    };
    return node;
}

/*
 * Wire only dependencies whose predecessor is in this queue. Cross-queue
 * edges are already represented by explicit transfer or rendezvous nodes;
 * treating a missing local index as an unmet count would deadlock the queue.
 */
void wireDirectDependencies(
    const BackendLoweringContext& context, CpuProgram& program) {
    std::map<ScheduleStepId, std::size_t> indexes;
    for (std::size_t i = 0; i < program.nodes.size(); ++i) {
        indexes[program.nodes[i].step] = i;
    }
    for (std::size_t i = 0; i < program.nodes.size(); ++i) {
        for (ScheduleStepId dependency :
             context.scheduled.steps().at(program.nodes[i].step).dependencies) {
            auto predecessor = indexes.find(dependency);
            if (predecessor == indexes.end()) continue;
            program.nodes[predecessor->second].successors.push_back(i);
            ++program.nodes[i].initialUnmet;
        }
    }
}

}  // namespace

/*
 * CPU lowering runs in three phases:
 * - index authored operations and route-private buffer targets;
 * - translate each scheduled step by payload family;
 * - wire the queue-local DAG after every step has an index.
 */
CpuProgram CpuLowering::lower(
    const BackendLoweringContext& context) {
    CpuProgram program;
    program.queue = context.queue.id;
    program.device = context.queue.device;
    program.nodes.reserve(context.queue.steps.size());
    /* Build immutable lookup state used by every per-step translation. */
    AuthoredIndex authored;
    indexAuthored(
        context.scheduled.routed().placed().resolved().authored().root(),
        authored);
    const std::map<CopyTargetKey, GraphBuffer> targets =
        copyTargets(context.scheduled);
    /* Preserve queue order while selecting the payload-specific lowerer. */
    for (ScheduleStepId stepId : context.queue.steps) {
        const ScheduledStep& step =
            context.scheduled.steps().at(stepId);
        if (const auto* operation =
                std::get_if<ScheduledOperation>(&step.payload)) {
            program.nodes.push_back(lowerOperation(
                context, stepId, *operation, authored, targets));
        } else if (const auto* boundary =
                       std::get_if<ScheduledBoundaryMaterialization>(
                           &step.payload)) {
            program.nodes.push_back(
                lowerBoundary(context, stepId, *boundary));
        } else {
            program.nodes.push_back(
                lowerRuntimeStep(context, step));
        }
    }
    /* Dependency counters are valid only after the node vector is complete. */
    wireDirectDependencies(context, program);
    return program;
}

}  // namespace vrt::graph
