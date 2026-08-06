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
#include <memory>
#include <utility>

#include "route_graph_internal.hpp"

namespace vrt::graph {

namespace {

/*
 * Placed replicas remain immutable source storage. Seed the location index
 * and advance the allocator past every placed id so routing can add transfer
 * targets without colliding with placement identities.
 */
route_detail::RoutingState makeState(
    const PlacedGraph& placed,
    const TransferCapabilityCatalog& capabilities) {
    route_detail::RoutingState state;
    state.placed = std::make_shared<PlacedGraph>(placed);
    state.capabilities = &capabilities;
    for (const auto& [id, replica] : placed.replicas()) {
        state.nextReplica =
            std::max(state.nextReplica, id.value() + 1);
        state.replicasByLocation.emplace(
            std::make_pair(replica.value, replica.memory), id);
    }
    return state;
}

}  // namespace

/*
 * Routing first validates storage identities, then plans data motion, and
 * only afterward fills ordering-only edges. The ordering pass relies on the
 * completed data-dependency set to avoid redundant barriers; final validation
 * assumes every route and prerequisite has already been interned.
 */
CompileResult<RoutedGraph> routeGraph(
    const PlacedGraph& placed,
    const TransferCapabilityCatalog& capabilities) {
    route_detail::RoutingState state =
        makeState(placed, capabilities);

    /*
     * Reject mutable-storage placements before considering copies; routing
     * must not repair an aliasing violation by silently changing identity.
     */
    route_detail::validateMutableStorage(state);
    const std::vector<route_detail::ValueUse> uses =
        route_detail::discoverValueUses(placed);

    /*
     * Materialize ordinary uses first, then control publications and graph
     * outputs, so later routes may reuse earlier transfer replicas.
     */
    route_detail::planValueRoutes(state, uses);
    route_detail::planControlPublicationRoutes(state);
    route_detail::planGraphOutputRoutes(state);

    /*
     * Data routes already impose producer/consumer order. Add barriers only
     * for the remaining authored dependencies, then audit the closed route set.
     */
    route_detail::planOrderDependencies(state);
    route_detail::validateRoutes(state);

    if (state.diagnostics.hasErrors()) {
        return CompileResult<RoutedGraph>::failure(
            std::move(state.diagnostics));
    }
    return CompileResult<RoutedGraph>::success(
        RoutedGraph(
            std::move(state.placed),
            capabilities.host(),
            std::move(state.transferReplicas),
            std::move(state.dependencies),
            std::move(state.routes)),
        std::move(state.diagnostics));
}

}  // namespace vrt::graph
