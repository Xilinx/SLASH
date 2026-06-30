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
 * @file dot.hpp
 * @brief Graphviz DOT serialisation for Graph and DGraph.
 *
 * Free functions in `vrt::graph::render` that convert a Graph (or one of its
 * compiled per-device DGraphs) into a Graphviz `.dot` source string.  Edges
 * are derived from IOMap data flow (input/output buffer tokens, RW pairs)
 * and explicit authored `KernelOp::afterOps` ordering constraints.
 */

#ifndef VRT_GRAPH_RENDER_DOT_HPP
#define VRT_GRAPH_RENDER_DOT_HPP

#include <string>

namespace vrt::graph {

class Graph;
struct DGraph;

namespace render {

/**
 * @brief Render a full Graph as Graphviz DOT.
 *
 * Authored regions are rendered as nested clusters. Kernel nodes are grouped
 * per-device inside their region; control-flow bodies and branches appear as
 * child clusters. Data dependencies are drawn as solid edges; explicit
 * `afterOps` ordering is drawn as dashed edges.
 */
std::string renderToDot(const Graph& graph);

/**
 * @brief Render a single per-device DGraph as Graphviz DOT.
 *
 * Only nodes assigned to this device are emitted.  Edges are restricted to
 * pairs both present in the DGraph; cross-device flows are realised as
 * bridge-injected ops inside the device and are not represented at the
 * DGraph level.
 */
std::string renderToDot(const DGraph& dgraph);

/**
 * @brief Write `renderToDot(graph)` to @p path.
 * @throws std::runtime_error if the file cannot be opened for writing.
 */
void writeToDotFile(const Graph& graph, const std::string& path);

/**
 * @brief Write `renderToDot(dgraph)` to @p path.
 * @throws std::runtime_error if the file cannot be opened for writing.
 */
void writeToDotFile(const DGraph& dgraph, const std::string& path);

}  // namespace render
}  // namespace vrt::graph

#endif  // VRT_GRAPH_RENDER_DOT_HPP
