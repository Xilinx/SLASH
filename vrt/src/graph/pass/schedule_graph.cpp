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

#include <vrt/graph/ir/scheduled_graph.hpp>

#include <memory>
#include <set>
#include <string>
#include <utility>

#include "schedule_graph_internal.hpp"

namespace vrt::graph {

namespace {

/*
 * A scalar may have replicas on several devices. Request one physical scalar
 * per device and scoped key, ignoring duplicate replicas on the same device.
 */
void collectScalarResources(schedule_detail::SchedulingState& state) {
    const PlacedGraph& placed = state.routed->placed();
    std::set<std::pair<DeviceId, std::string>> seen;
    for (const auto& [valueId, value] : placed.resolved().values()) {
        const GraphScalar* scalar = resolvedScalarToken(value);
        if (!scalar) continue;
        const std::string key =
            scopedScalarKey(scalar->scopeId(), scalar->varName());
        for (const auto& [replicaId, replica] : placed.replicas()) {
            (void)replicaId;
            if (replica.value != valueId ||
                !seen.insert({replica.memory.device, key}).second) {
                continue;
            }
            state.scalarResources.push_back({
                ScalarResourceId(state.nextScalarResource++),
                valueId, replica.memory.device, key});
        }
    }
}

}  // namespace

CompileResult<ScheduledGraph> scheduleGraph(
    const RoutedGraph& routed) {
    /*
     * Queue and step creation: emit graph I/O, operations, boundaries,
     * transfers, and split controls after all required queues are known.
     * Dependency wiring: join routes, controls, operations, boundaries, and
     * graph I/O once every referenced step has an ID.
     * Finalization: collect logical scalar resources, validate references and
     * cycles, then put each queue in executable order.
     */
    schedule_detail::SchedulingState state;
    state.routed = std::make_shared<RoutedGraph>(routed);
    state.rootRegion = routed.placed().resolved().root().id;

    schedule_detail::materializeQueues(state);
    schedule_detail::scheduleGraphIo(state);
    schedule_detail::scheduleOperations(state);
    schedule_detail::scheduleBoundaries(state);
    schedule_detail::scheduleRoutes(state);
    schedule_detail::scheduleSplitControls(state);
    schedule_detail::wireRouteControlDependencies(state);
    schedule_detail::wireOperationDependencies(state);
    schedule_detail::wireGraphIoDependencies(state);
    schedule_detail::wireBoundaryDependencies(state);
    collectScalarResources(state);
    schedule_detail::validateSchedule(state);

    if (state.diagnostics.hasErrors()) {
        return CompileResult<ScheduledGraph>::failure(
            std::move(state.diagnostics));
    }
    return CompileResult<ScheduledGraph>::success(
        ScheduledGraph(
            std::move(state.routed), std::move(state.queues),
            std::move(state.steps), std::move(state.rendezvous),
            std::move(state.splitControls),
            std::move(state.resources),
            std::move(state.scalarResources)),
        std::move(state.diagnostics));
}

}  // namespace vrt::graph
