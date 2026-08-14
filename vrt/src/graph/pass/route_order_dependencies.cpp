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

#include "route_graph_internal.hpp"

namespace vrt::graph::route_detail {

/*
 * Data dependencies are planned first and already order their producer and
 * consumer, so authored ordering adds a barrier only for uncovered pairs.
 * Same-device pairs still receive an order edge with no route; structural or
 * unplaced endpoints cannot participate in executable ordering.
 */
void planOrderDependencies(RoutingState& state) {
    for (const auto& [consumer, operation] :
         state.placed->resolved().operations()) {
        if (operation.structural) continue;
        auto destinationPlacement =
            state.placed->operationPlacements().find(consumer);
        if (destinationPlacement ==
            state.placed->operationPlacements().end()) {
            continue;
        }
        for (const ResolvedDependency& dependency :
             operation.dependencies) {
            const NodeId producer = dependency.predecessor;

            /*
             * Prefer the existing data edge: adding a barrier route would
             * duplicate transport and could introduce a second handshake.
             */
            if (state.dataDependencies.count(
                    {producer, consumer}) != 0) {
                continue;
            }
            const ResolvedOperation* sourceOperation =
                state.placed->resolved().findOperation(producer);
            if (!sourceOperation || sourceOperation->structural) {
                continue;
            }
            auto sourcePlacement =
                state.placed->operationPlacements().find(producer);
            if (sourcePlacement ==
                state.placed->operationPlacements().end()) {
                continue;
            }
            const MemoryPlacement source{
                sourcePlacement->second.device, std::nullopt};
            const MemoryPlacement destination{
                destinationPlacement->second.device,
                std::nullopt};

            /*
             * Barrier selection follows ordinary route capabilities. A null
             * route is expected when device-local scheduling alone suffices.
             */
            const std::optional<RouteId> route =
                planBarrierRoute(
                    state, producer, consumer, source,
                    destination);
            state.dependencies.push_back(
                OrderDependencyEdge{
                    producer, consumer, route});
        }
    }
}

}  // namespace vrt::graph::route_detail
