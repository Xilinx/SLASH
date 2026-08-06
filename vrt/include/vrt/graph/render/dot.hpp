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

/** @file dot.hpp @brief Graphviz projections of compiler stage IR. */
#ifndef VRT_GRAPH_RENDER_DOT_HPP
#define VRT_GRAPH_RENDER_DOT_HPP

#include <string>

namespace vrt::graph {
class ResolvedGraph;
class PlacedGraph;
class RoutedGraph;
class ScheduledGraph;

namespace render {
std::string renderToDot(const ResolvedGraph& graph);
std::string renderToDot(const PlacedGraph& graph);
std::string renderToDot(const RoutedGraph& graph);
std::string renderToDot(const ScheduledGraph& graph);
}  // namespace render
}  // namespace vrt::graph

#endif  // VRT_GRAPH_RENDER_DOT_HPP
