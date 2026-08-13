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

#include <vrt/graph/ir/placed_graph.hpp>

#include <memory>
#include <utility>

#include "place_graph_internal.hpp"

namespace vrt::graph {

/*
 * Placement is a monotonic pipeline: operations establish device ownership,
 * replicas refine it to memory regions, and structural mappings consume both.
 * Reordering these stages would make later inference observe incomplete
 * ownership or discard an already selected HBM region.
 */
CompileResult<PlacedGraph> placeGraph(
    const ResolvedGraph& resolved,
    const DeviceCapabilityCatalog& capabilities) {
    place_detail::PlacementState state;
    state.resolved = std::make_shared<ResolvedGraph>(resolved);
    state.capabilities = &capabilities;

    /*
     * First settle operation and control devices, then derive the replicas
     * attached to their ports.
     */
    place_detail::placeRegions(state);
    place_detail::placeReplicas(state);

    /*
     * Control outputs depend on producer replicas. Boundaries come last so
     * they can project the final replica choices across region edges.
     */
    place_detail::placeControlResults(state);
    place_detail::placeBoundaryMappings(state);

    if (state.diagnostics.hasErrors()) {
        return CompileResult<PlacedGraph>::failure(
            std::move(state.diagnostics));
    }
    return CompileResult<PlacedGraph>::success(
        PlacedGraph(
            std::move(state.resolved),
            std::move(state.operationPlacements),
            std::move(state.controlPlacements),
            std::move(state.replicas),
            std::move(state.primaryReplicas),
            std::move(state.portPlacements),
            std::move(state.boundaryMappings),
            std::move(state.regionSummaries)),
        std::move(state.diagnostics));
}

}  // namespace vrt::graph
