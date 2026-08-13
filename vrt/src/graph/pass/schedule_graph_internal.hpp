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

#ifndef VRT_GRAPH_PASS_SCHEDULE_GRAPH_INTERNAL_HPP
#define VRT_GRAPH_PASS_SCHEDULE_GRAPH_INTERNAL_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <vrt/graph/ir/scheduled_graph.hpp>

namespace vrt::graph::schedule_detail {

using QueueKey = std::pair<RegionId, DeviceId>;
using OperationQueueKey = std::pair<NodeId, DeviceId>;

/*
 * Scheduling passes allocate logical IDs monotonically and keep canonical
 * objects here. Side indexes let later passes add dependencies without
 * rebuilding queues or searching the whole schedule.
 */
struct SchedulingState {
    /* Input graph, diagnostics, and logical ID allocators. */
    std::shared_ptr<const RoutedGraph> routed;
    Diagnostics                       diagnostics;
    RegionId                          rootRegion;
    std::uint64_t                     nextStep = 0;
    std::uint64_t                     nextRendezvous = 0;
    std::uint64_t                     nextScalarResource = 0;

    /* One queue per region and device, with stable ID-to-vector lookup. */
    std::map<QueueKey, QueueId>        queueIds;
    std::map<QueueId, std::size_t>     queueIndexes;
    std::vector<QueueProgram>          queues;

    /* Canonical steps and operation indexes used by dependency wiring. */
    std::map<ScheduleStepId, ScheduledStep> steps;
    std::map<OperationQueueKey, ScheduleStepId> operationSteps;
    std::map<NodeId, std::vector<ScheduleStepId>> operationStepLists;

    /* Terminal, executor, and launch milestones for each transfer route. */
    std::map<RouteId, ScheduleStepId> routeCompletion;
    std::map<RouteId, ScheduleStepId> routeActionCompletion;
    std::map<RouteId, ScheduleStepId> routeLaunchCompletion;

    /* Logical handshakes and resources consumed by backend lowering. */
    std::vector<LogicalRendezvous> rendezvous;
    std::vector<SplitControlProtocol> splitControls;
    std::map<NodeId, std::size_t> splitControlIndexes;
    std::vector<LogicalResourceRequirement> resources;
    std::vector<LogicalScalarRequirement> scalarResources;

    /* Root I/O steps anchor host-local and route dependencies. */
    std::optional<ScheduleStepId> graphInput;
    std::optional<ScheduleStepId> graphOutput;
};

const RegionTreeIndex& index(const SchedulingState& state);
QueueId queueFor(SchedulingState& state, RegionId region, DeviceId device);
const QueueProgram* findQueue(const SchedulingState& state, QueueId id);
QueueProgram& mutableQueue(SchedulingState& state, QueueId id);
ScheduleStepId createStep(
    SchedulingState& state, QueueId queue, RegionId region,
    ScheduledStepPayload payload);
void addDependency(
    SchedulingState& state, ScheduleStepId step,
    ScheduleStepId dependency);
void materializeQueues(SchedulingState& state);

std::optional<RendezvousId> createRendezvous(
    SchedulingState& state, QueueId publisher, QueueId waiter,
    LogicalRendezvousPayload payload);
ScheduleStepId publishAndWait(
    SchedulingState& state, QueueId publisherQueue,
    RegionId publisherRegion, ScheduleStepId publisherDependency,
    QueueId waiterQueue, RegionId waiterRegion,
    LogicalRendezvousPayload payload);

void scheduleGraphIo(SchedulingState& state);
void scheduleOperations(SchedulingState& state);
void scheduleBoundaries(SchedulingState& state);
void scheduleSplitControls(SchedulingState& state);
void scheduleRoutes(SchedulingState& state);
void wireRouteControlDependencies(SchedulingState& state);
void wireOperationDependencies(SchedulingState& state);
void wireBoundaryDependencies(SchedulingState& state);
void wireGraphIoDependencies(SchedulingState& state);
void validateSchedule(SchedulingState& state);

std::optional<ScheduleStepId> operationStep(
    const SchedulingState& state, std::optional<NodeId> operation,
    const DeviceId& device);
const SplitControlProtocol* splitControl(
    const SchedulingState& state, NodeId control);

}  // namespace vrt::graph::schedule_detail

#endif  // VRT_GRAPH_PASS_SCHEDULE_GRAPH_INTERNAL_HPP
