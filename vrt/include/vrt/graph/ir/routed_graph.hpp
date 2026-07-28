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

/**
 * @file routed_graph.hpp
 * @brief Placed graph with explicit dependency edges and transfer routes.
 */

#ifndef VRT_GRAPH_IR_ROUTED_GRAPH_HPP
#define VRT_GRAPH_IR_ROUTED_GRAPH_HPP

#include <memory>
#include <optional>
#include <vector>

#include <vrt/graph/compile_result.hpp>
#include <vrt/graph/ids.hpp>
#include <vrt/graph/ir/placed_graph.hpp>
#include <vrt/graph/transfer.hpp>

namespace vrt::graph {

enum class DependencyKind {
    Value,
    Order,
};

struct DependencyEdge {
    std::optional<NodeId>  producer;
    std::optional<NodeId>  consumer;
    DependencyKind         kind = DependencyKind::Value;
    std::optional<ValueId> value;
    std::optional<RouteId> route;
};

class RoutedGraph {
   public:
    RoutedGraph(std::shared_ptr<const PlacedGraph> placed,
                std::vector<DependencyEdge> dependencies,
                std::vector<TransferRoute> routes)
        : placed_(std::move(placed)),
          dependencies_(std::move(dependencies)),
          routes_(std::move(routes)) {}

    const PlacedGraph& placed() const { return *placed_; }
    const std::vector<DependencyEdge>& dependencies() const {
        return dependencies_;
    }
    const std::vector<TransferRoute>& routes() const {
        return routes_;
    }

   private:
    std::shared_ptr<const PlacedGraph> placed_;
    std::vector<DependencyEdge>        dependencies_;
    std::vector<TransferRoute>         routes_;
};

CompileResult<RoutedGraph> routeGraph(
    const PlacedGraph& placed,
    const TransferCapabilityCatalog& capabilities);

}  // namespace vrt::graph

#endif  // VRT_GRAPH_IR_ROUTED_GRAPH_HPP
