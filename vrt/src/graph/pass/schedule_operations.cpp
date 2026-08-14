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

#include "schedule_graph_internal.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>
#include <variant>

namespace vrt::graph::schedule_detail {

/*
 * Prefer the operation instance on the requested device. Replicated controls
 * and boundaries may have several instances; callers which only need logical
 * completion fall back to the first recorded instance.
 */
std::optional<ScheduleStepId> operationStep(
    const SchedulingState& state, std::optional<NodeId> operation,
    const DeviceId& device) {
    if (!operation) return std::nullopt;
    auto exact = state.operationSteps.find({*operation, device});
    if (exact != state.operationSteps.end()) return exact->second;
    auto all = state.operationStepLists.find(*operation);
    return all == state.operationStepLists.end() || all->second.empty()
               ? std::nullopt
               : std::optional<ScheduleStepId>(all->second.front());
}

void scheduleGraphIo(SchedulingState& state) {
    /*
     * A routed graph must name one I/O host. Emit one input and one output
     * step on its root queue; final queue ordering keeps them around all
     * ordinary work even when either value list is empty.
     */
    if (!state.routed->graphIoHost()) {
        state.diagnostics.error(
            DiagCode::InternalInvariant,
            "GraphCompiler: routed graph has no graph I/O host");
        return;
    }
    std::vector<ValueId> inputs;
    std::vector<ValueId> outputs;
    for (const auto& [id, value] :
         state.routed->placed().resolved().values()) {
        if (valueDefinition(value) ==
            ValueDefinitionKind::GraphInput) {
            inputs.push_back(id);
        }
        if (value.graphOutput) outputs.push_back(id);
    }
    const QueueId queue = queueFor(
        state, state.rootRegion, *state.routed->graphIoHost());
    state.graphInput = createStep(
        state, queue, state.rootRegion,
        ScheduledGraphInput{state.rootRegion, std::move(inputs)});
    state.graphOutput = createStep(
        state, queue, state.rootRegion,
        ScheduledGraphOutput{state.rootRegion, std::move(outputs)});
}

namespace {

void recordOperationStep(
    SchedulingState& state, NodeId operation, DeviceId device,
    RegionId region) {
    const QueueId queue = queueFor(state, region, device);
    const ScheduleStepId step = createStep(
        state, queue, region, ScheduledOperation{operation});
    state.operationSteps[{operation, device}] = step;
    state.operationStepLists[operation].push_back(step);
}

}  // namespace

void scheduleOperations(SchedulingState& state) {
    /*
     * Structural boundary nodes are emitted separately. Controls get one
     * operation step per participating device; ordinary operations get one
     * step at their placement, or an invariant diagnostic if none exists.
     */
    const PlacedGraph& placed = state.routed->placed();
    for (const auto& [node, operation] :
         placed.resolved().operations()) {
        if (operation.structural) continue;
        auto control = placed.controlPlacements().find(node);
        if (control != placed.controlPlacements().end()) {
            for (const DeviceId& device :
                 controlParticipants(control->second)) {
                recordOperationStep(
                    state, node, device, operation.region);
            }
            continue;
        }
        auto placement = placed.operationPlacements().find(node);
        if (placement == placed.operationPlacements().end()) {
            state.diagnostics.error(
                DiagCode::InternalInvariant,
                "GraphCompiler: operation has no queue placement");
            continue;
        }
        recordOperationStep(
            state, node, placement->second.device, operation.region);
    }
}

void scheduleBoundaries(SchedulingState& state) {
    /*
     * Several ports can map through the same boundary on the same owning
     * device. Group them into one materialization step per boundary and
     * owner, then index that step like any other operation instance.
     */
    using BoundaryOwner = std::pair<NodeId, DeviceId>;
    std::map<BoundaryOwner, std::vector<ScheduledBoundaryMapping>>
        mappings;
    for (const BoundaryMappingPlacement& mapping :
         state.routed->placed().boundaryMappings()) {
        mappings[{mapping.boundary, mapping.owner}].push_back(
            {mapping.port, mapping.source, mapping.target});
    }
    for (auto& [owner, ownedMappings] : mappings) {
        const ResolvedOperation* boundary =
            state.routed->placed().resolved().findOperation(owner.first);
        if (!boundary) continue;
        const QueueId queue =
            queueFor(state, boundary->region, owner.second);
        const ScheduleStepId step = createStep(
            state, queue, boundary->region,
            ScheduledBoundaryMaterialization{
                owner.first, std::move(ownedMappings)});
        state.operationSteps[{owner.first, owner.second}] = step;
        state.operationStepLists[owner.first].push_back(step);
    }
}

void wireOperationDependencies(SchedulingState& state) {
    /*
     * Routed edges are connected by the transfer scheduler. For a local edge,
     * prefer the producer step on the consumer's device; if there is no such
     * replica, depend on the producer's first scheduled instance.
     */
    for (const DependencyEdge& edge :
         state.routed->dependencies()) {
        if (dependencyRoute(edge)) continue;
        const std::optional<NodeId> producer =
            dependencyProducer(edge);
        const std::optional<NodeId> consumer =
            dependencyConsumer(edge);
        if (!producer || !consumer) continue;
        auto consumerSteps =
            state.operationStepLists.find(*consumer);
        auto producerSteps =
            state.operationStepLists.find(*producer);
        if (consumerSteps == state.operationStepLists.end() ||
            producerSteps == state.operationStepLists.end()) {
            continue;
        }
        for (ScheduleStepId consumerStep : consumerSteps->second) {
            const QueueProgram* queue =
                findQueue(state, state.steps.at(consumerStep).queue);
            if (!queue) continue;
            auto exact =
                state.operationSteps.find({*producer, queue->device});
            addDependency(
                state, consumerStep,
                exact != state.operationSteps.end()
                    ? exact->second
                    : producerSteps->second.front());
        }
    }
}

namespace {

bool isStartBoundary(
    const SchedulingState& state, NodeId boundary) {
    const AuthoredOperation* authored =
        index(state).findOperation(boundary);
    const auto* concrete =
        authored ? std::get_if<AuthoredBoundary>(authored) : nullptr;
    return concrete && concrete->side == BoundarySide::Start;
}

}  // namespace

void wireBoundaryDependencies(SchedulingState& state) {
    /*
     * Every boundary materialization waits for its resolved predecessors,
     * preferring predecessor instances on the same device. A start boundary
     * also gates every non-boundary step in its queue; an end boundary needs
     * no extra queue-wide edge.
     */
    for (const auto& [node, scheduledSteps] :
         state.operationStepLists) {
        const ResolvedOperation* boundary =
            state.routed->placed().resolved().findOperation(node);
        if (!boundary || !boundary->structural) continue;
        for (ScheduleStepId boundaryStep : scheduledSteps) {
            const QueueProgram* queue =
                findQueue(state, state.steps.at(boundaryStep).queue);
            if (!queue) continue;
            for (const ResolvedDependency& dependency :
                 boundary->dependencies) {
                if (auto predecessor = operationStep(
                        state, dependency.predecessor,
                        queue->device)) {
                    addDependency(
                        state, boundaryStep, *predecessor);
                }
            }
            if (!isStartBoundary(state, node)) continue;
            for (ScheduleStepId step : queue->steps) {
                if (step != boundaryStep &&
                    !std::holds_alternative<
                        ScheduledBoundaryMaterialization>(
                        state.steps.at(step).payload)) {
                    addDependency(state, step, boundaryStep);
                }
            }
        }
    }
}

void wireGraphIoDependencies(SchedulingState& state) {
    if (!state.graphInput || !state.graphOutput) return;
    const ResolvedGraph& resolved =
        state.routed->placed().resolved();

    /* Gate direct host consumers after graph input becomes available. */
    for (const auto& [node, operation] : resolved.operations()) {
        if (operation.structural) continue;
        const bool consumesInput = std::any_of(
            operation.bindings.begin(), operation.bindings.end(),
            [&](const ResolvedBinding& binding) {
                const ResolvedValue* value =
                    resolved.findValue(binding.value);
                return value &&
                       valueDefinition(*value) ==
                           ValueDefinitionKind::GraphInput;
            });
        if (!consumesInput) continue;
        for (ScheduleStepId step :
             state.operationStepLists[node]) {
            const QueueProgram* queue =
                findQueue(state, state.steps.at(step).queue);
            if (queue && state.routed->graphIoHost() &&
                queue->device == *state.routed->graphIoHost()) {
                addDependency(
                    state, step, *state.graphInput);
            }
        }
    }

    /* Hold graph output until every instance producing an output completes. */
    for (const auto& [id, value] : resolved.values()) {
        if (!value.graphOutput) continue;
        const std::optional<NodeId> producer = valueProducer(value);
        if (!producer) continue;
        for (ScheduleStepId step :
             state.operationStepLists[*producer]) {
            addDependency(state, *state.graphOutput, step);
        }
        (void)id;
    }
}

}  // namespace vrt::graph::schedule_detail
