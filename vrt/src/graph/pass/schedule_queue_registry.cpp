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

#include <utility>
#include <variant>

namespace vrt::graph::schedule_detail {

const RegionTreeIndex& index(const SchedulingState& state) {
    return state.routed->placed().resolved().authored().index();
}

/*
 * A region and device share one serial queue. First use appends the queue and
 * fixes its ID; later users recover that ID through the two lookup maps.
 */
QueueId queueFor(
    SchedulingState& state, RegionId region, DeviceId device) {
    const QueueKey key{region, device};
    auto existing = state.queueIds.find(key);
    if (existing != state.queueIds.end()) return existing->second;

    QueueProgram queue;
    queue.id = QueueId(state.queues.size());
    queue.device = std::move(device);
    queue.region = region;
    queue.parentControl = index(state).parentControl(region);
    state.queueIds[key] = queue.id;
    state.queueIndexes[queue.id] = state.queues.size();
    state.queues.push_back(std::move(queue));
    return state.queues.back().id;
}

const QueueProgram* findQueue(
    const SchedulingState& state, QueueId id) {
    auto queue = state.queueIndexes.find(id);
    return queue == state.queueIndexes.end()
               ? nullptr
               : &state.queues[queue->second];
}

QueueProgram& mutableQueue(SchedulingState& state, QueueId id) {
    return state.queues.at(state.queueIndexes.at(id));
}

/*
 * Keep the step in the global table and append its ID to the queue's
 * provisional order. Validation computes the final order after all
 * dependencies have been added.
 */
ScheduleStepId createStep(
    SchedulingState& state, QueueId queue, RegionId region,
    ScheduledStepPayload payload) {
    ScheduledStep step;
    step.id = ScheduleStepId(state.nextStep++);
    step.queue = queue;
    step.region = region;
    step.payload = std::move(payload);
    const ScheduleStepId id = step.id;
    state.steps.emplace(id, std::move(step));
    mutableQueue(state, queue).steps.push_back(id);
    return id;
}

void addDependency(
    SchedulingState& state, ScheduleStepId step,
    ScheduleStepId dependency) {
    if (step != dependency) {
        state.steps.at(step).dependencies.push_back(dependency);
    }
}

namespace {

/*
 * Placement contributes queues in three ways:
 * - region summaries cover every device used in a region;
 * - operation placements cover concrete operation sites;
 * - boundary mappings add the device which owns each materialization.
 */
void materializePlacedQueues(SchedulingState& state) {
    const PlacedGraph& placed = state.routed->placed();
    for (const auto& [region, summary] : placed.regionSummaries()) {
        for (const DeviceId& device : summary.devices) {
            queueFor(state, region, device);
        }
    }
    for (const auto& [node, placement] : placed.operationPlacements()) {
        const ResolvedOperation* operation =
            placed.resolved().findOperation(node);
        if (operation) {
            queueFor(state, operation->region, placement.device);
        }
    }
    for (const BoundaryMappingPlacement& mapping :
         placed.boundaryMappings()) {
        const ResolvedOperation* boundary =
            placed.resolved().findOperation(mapping.boundary);
        if (boundary) {
            queueFor(state, boundary->region, mapping.owner);
        }
    }
}

/*
 * Each control participant needs a queue for its control step and one in
 * every child region governed by that control.
 */
void materializeControlQueues(SchedulingState& state) {
    const PlacedGraph& placed = state.routed->placed();
    for (const auto& [control, placement] :
         placed.controlPlacements()) {
        const ResolvedOperation* operation =
            placed.resolved().findOperation(control);
        if (!operation) continue;
        for (const DeviceId& participant :
             controlParticipants(placement)) {
            queueFor(state, operation->region, participant);
            for (const AuthoredChildRegion& child :
                 index(state).children(operation->region)) {
                if (child.control == control) {
                    queueFor(state, child.region, participant);
                }
            }
        }
    }
}

/*
 * A transfer leg may run on three queue sites: source, destination, and the
 * device chosen to execute the action. queueFor() folds overlapping sites.
 */
void materializeRouteQueues(SchedulingState& state) {
    for (const TransferRoute& route : state.routed->routes()) {
        for (const TransferLeg& leg : route.legs) {
            queueFor(state, leg.sourceRegion, leg.source);
            queueFor(
                state, leg.destinationRegion, leg.destination);
            queueFor(
                state, leg.executorRegion,
                transferExecutorDevice(
                    leg.executor, leg.source, leg.destination));
        }
    }
}

}  // namespace

void materializeQueues(SchedulingState& state) {
    /*
     * Discover queues in a fixed order before emitting steps: graph host,
     * placed work, control participants, then transfer sites. This
     * keeps queue IDs deterministic and lets later passes assume every queue
     * already exists.
     */
    if (state.routed->graphIoHost()) {
        queueFor(
            state, state.rootRegion,
            *state.routed->graphIoHost());
    }
    materializePlacedQueues(state);
    materializeControlQueues(state);
    materializeRouteQueues(state);
}

}  // namespace vrt::graph::schedule_detail
