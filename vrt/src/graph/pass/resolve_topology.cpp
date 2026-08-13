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

#include "resolve_graph_internal.hpp"

#include <functional>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

namespace vrt::graph::resolve_detail {

/*
 * Topologically order only operations owned by this region; dependencies on
 * parent or child regions are represented by control boundaries instead.
 * Kahn's algorithm uses the smallest NodeId whenever several nodes are ready,
 * making equivalent graphs deterministic. A cycle leaves a partial order and
 * records an error, so callers can continue collecting diagnostics.
 */
std::vector<NodeId> DeterministicTopology::order(
    const AuthoredRegion& region) {
    /*
     * First isolate local nodes, then project resolved dependencies onto that
     * set to build the region-local indegree and successor tables.
     */
    std::set<NodeId> local;
    for (const AuthoredOperation& operation : region.operations) {
        local.insert(authoredNodeId(operation));
    }

    std::map<NodeId, std::size_t> indegree;
    std::map<NodeId, std::vector<NodeId>> successors;
    for (NodeId node : local) indegree[node] = 0;
    for (NodeId node : local) {
        const ResolvedOperation& operation =
            state_.operations.at(node);
        for (const ResolvedDependency& dependency :
             operation.dependencies) {
            if (!local.count(dependency.predecessor)) continue;
            successors[dependency.predecessor].push_back(node);
            ++indegree[node];
        }
    }

    /*
     * The min-heap is the tie-breaker: authored NodeIds, rather than map or
     * traversal accidents, decide among independent ready operations.
     */
    std::priority_queue<
        NodeId, std::vector<NodeId>, std::greater<NodeId>> ready;
    for (const auto& [node, degree] : indegree) {
        if (degree == 0) ready.push(node);
    }

    /*
     * Removing a node releases each successor exactly when its last local
     * predecessor has been emitted.
     */
    std::vector<NodeId> result;
    while (!ready.empty()) {
        const NodeId node = ready.top();
        ready.pop();
        result.push_back(node);
        for (NodeId successor : successors[node]) {
            if (--indegree[successor] == 0) {
                ready.push(successor);
            }
        }
    }

    /*
     * Any un-emitted local node belongs to, or is downstream of, a cycle.
     * Preserve the deterministic acyclic prefix while making the graph fail.
     */
    if (result.size() != local.size()) {
        state_.diagnostics.error(
            DiagCode::Cycle,
            "GraphCompiler: cycle detected in region " +
                std::to_string(region.id.value()));
    }
    return result;
}

}  // namespace vrt::graph::resolve_detail
