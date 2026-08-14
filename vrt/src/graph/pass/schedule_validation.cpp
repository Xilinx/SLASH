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
#include <functional>
#include <map>
#include <queue>
#include <type_traits>
#include <utility>
#include <variant>

namespace vrt::graph::schedule_detail {

namespace {

const TransferRoute* findRoute(
    const SchedulingState& state, RouteId id) {
    auto route = std::find_if(
        state.routed->routes().begin(),
        state.routed->routes().end(),
        [&](const TransferRoute& candidate) {
            return candidate.id == id;
        });
    return route == state.routed->routes().end() ? nullptr : &*route;
}

bool hasLeg(const TransferRoute& route, TransferLegId id) {
    return std::any_of(
        route.legs.begin(), route.legs.end(),
        [&](const TransferLeg& leg) { return leg.id == id; });
}

const LogicalRendezvous* findRendezvous(
    const SchedulingState& state, RendezvousId id) {
    auto rendezvous = std::find_if(
        state.rendezvous.begin(), state.rendezvous.end(),
        [&](const LogicalRendezvous& candidate) {
            return candidate.id == id;
        });
    return rendezvous == state.rendezvous.end()
               ? nullptr
               : &*rendezvous;
}

/*
 * Check every payload-specific reference:
 * - operation steps name a resolved operation;
 * - transfer steps name an existing route leg;
 * - publish and wait steps use the rendezvous queue for their role;
 * - boundaries name an operation and carry at least one mapping;
 * - graph I/O belongs to the root region.
 */
void validatePayload(
    SchedulingState& state, const ScheduledStep& step) {
    std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, ScheduledOperation>) {
                if (!state.routed->placed().resolved().findOperation(
                        payload.operation)) {
                    state.diagnostics.error(
                        DiagCode::InternalInvariant,
                        "GraphCompiler: schedule references an "
                        "unknown operation");
                }
            } else if constexpr (
                std::is_same_v<T, ScheduledTransferProduce> ||
                std::is_same_v<T, ScheduledTransferConsume> ||
                std::is_same_v<T, ScheduledTransferAction>) {
                const TransferRoute* route =
                    findRoute(state, payload.route);
                if (!route || !hasLeg(*route, payload.leg)) {
                    state.diagnostics.error(
                        DiagCode::InternalInvariant,
                        "GraphCompiler: schedule references an "
                        "unknown transfer leg");
                }
            } else if constexpr (
                std::is_same_v<T, ScheduledEventPublish> ||
                std::is_same_v<T, ScheduledEventWait>) {
                const LogicalRendezvous* rendezvous =
                    findRendezvous(state, payload.rendezvous);
                const QueueId expected =
                    std::is_same_v<T, ScheduledEventPublish>
                        ? (rendezvous ? rendezvous->publisher
                                      : QueueId{})
                        : (rendezvous ? rendezvous->waiter
                                      : QueueId{});
                if (!rendezvous || expected != step.queue) {
                    state.diagnostics.error(
                        DiagCode::InternalInvariant,
                        "GraphCompiler: event step and rendezvous "
                        "queues disagree");
                }
            } else if constexpr (
                std::is_same_v<
                    T, ScheduledBoundaryMaterialization>) {
                if (payload.mappings.empty() ||
                    !state.routed->placed().resolved().findOperation(
                        payload.boundary)) {
                    state.diagnostics.error(
                        DiagCode::InternalInvariant,
                        "GraphCompiler: invalid scheduled boundary");
                }
            } else {
                if (payload.graph != state.rootRegion) {
                    state.diagnostics.error(
                        DiagCode::InternalInvariant,
                        "GraphCompiler: graph I/O step is not on "
                        "the root graph");
                }
            }
        },
        step.payload);
}

/*
 * Canonicalize dependency lists, then validate each step's queue, payload,
 * and dependency IDs. Finally check that both ends of every rendezvous still
 * name materialized queues.
 */
void validateReferences(SchedulingState& state) {
    for (auto& [id, step] : state.steps) {
        (void)id;
        std::sort(
            step.dependencies.begin(), step.dependencies.end());
        step.dependencies.erase(
            std::unique(
                step.dependencies.begin(),
                step.dependencies.end()),
            step.dependencies.end());
        const QueueProgram* queue = findQueue(state, step.queue);
        if (!queue || queue->region != step.region) {
            state.diagnostics.error(
                DiagCode::InternalInvariant,
                "GraphCompiler: schedule step has an invalid queue");
        }
        validatePayload(state, step);
        for (ScheduleStepId dependency : step.dependencies) {
            if (state.steps.count(dependency) == 0) {
                state.diagnostics.error(
                    DiagCode::InternalInvariant,
                    "GraphCompiler: schedule references an "
                    "unknown step");
            }
        }
    }
    for (const LogicalRendezvous& rendezvous : state.rendezvous) {
        if (!findQueue(state, rendezvous.publisher) ||
            !findQueue(state, rendezvous.waiter)) {
            state.diagnostics.error(
                DiagCode::InternalInvariant,
                "GraphCompiler: rendezvous references an "
                "unknown queue");
        }
    }
}

/*
 * Use Kahn's algorithm with the lowest step ID first for deterministic
 * results. Unknown dependencies were diagnosed above and do not take part in
 * cycle detection.
 */
std::vector<ScheduleStepId> topologicalOrder(
    SchedulingState& state) {
    std::map<ScheduleStepId, std::size_t> indegree;
    std::map<ScheduleStepId, std::vector<ScheduleStepId>>
        successors;
    for (const auto& [id, step] : state.steps) {
        indegree[id] = 0;
        for (ScheduleStepId dependency : step.dependencies) {
            if (state.steps.count(dependency) != 0) {
                successors[dependency].push_back(id);
                ++indegree[id];
            }
        }
    }
    std::priority_queue<
        ScheduleStepId, std::vector<ScheduleStepId>,
        std::greater<ScheduleStepId>>
        ready;
    for (const auto& [id, degree] : indegree) {
        if (degree == 0) ready.push(id);
    }
    std::vector<ScheduleStepId> order;
    while (!ready.empty()) {
        const ScheduleStepId id = ready.top();
        ready.pop();
        order.push_back(id);
        for (ScheduleStepId successor : successors[id]) {
            if (--indegree[successor] == 0) ready.push(successor);
        }
    }
    if (order.size() != state.steps.size()) {
        state.diagnostics.error(
            DiagCode::Cycle,
            "GraphCompiler: queue schedule contains a cycle");
        return {};
    }
    return order;
}

/*
 * Queue envelope order is, from first to last:
 * - graph input;
 * - start-boundary materialization;
 * - ordinary work;
 * - end-boundary materialization;
 * - graph output.
 */
int queueOrderPriority(
    const SchedulingState& state, ScheduleStepId id) {
    const ScheduledStepPayload& payload =
        state.steps.at(id).payload;
    if (std::holds_alternative<ScheduledGraphInput>(payload)) {
        return -2;
    }
    if (std::holds_alternative<ScheduledGraphOutput>(payload)) {
        return 2;
    }
    const auto* boundary =
        std::get_if<ScheduledBoundaryMaterialization>(&payload);
    if (!boundary) return 0;
    const AuthoredOperation* authored =
        index(state).findOperation(boundary->boundary);
    const auto* concrete =
        authored ? std::get_if<AuthoredBoundary>(authored) : nullptr;
    return concrete && concrete->side == BoundarySide::Start
               ? -1
               : 1;
}

/*
 * The global topological rank orders steps within each envelope class. The
 * explicit priorities keep graph I/O and region boundaries around ordinary
 * work without relying on step creation order.
 */
void orderQueues(
    SchedulingState& state,
    const std::vector<ScheduleStepId>& order) {
    std::map<ScheduleStepId, std::size_t> rank;
    for (std::size_t i = 0; i < order.size(); ++i) {
        rank[order[i]] = i;
    }
    for (QueueProgram& queue : state.queues) {
        std::sort(
            queue.steps.begin(), queue.steps.end(),
            [&](ScheduleStepId lhs, ScheduleStepId rhs) {
                const int lhsPriority =
                    queueOrderPriority(state, lhs);
                const int rhsPriority =
                    queueOrderPriority(state, rhs);
                if (lhsPriority != rhsPriority) {
                    return lhsPriority < rhsPriority;
                }
                return rank[lhs] < rank[rhs];
            });
    }
}

}  // namespace

void validateSchedule(SchedulingState& state) {
    /*
     * Reference errors can coexist with a partial graph. Validate them first,
     * then order queues only when a complete acyclic order is available.
     */
    validateReferences(state);
    const std::vector<ScheduleStepId> order =
        topologicalOrder(state);
    if (!order.empty()) orderQueues(state, order);
}

}  // namespace vrt::graph::schedule_detail
