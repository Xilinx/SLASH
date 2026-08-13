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
 * @file validate_authored_graph.hpp
 * @brief First compiler pass over the detached authored netlist.
 */

#ifndef VRT_GRAPH_PASS_VALIDATE_AUTHORED_GRAPH_HPP
#define VRT_GRAPH_PASS_VALIDATE_AUTHORED_GRAPH_HPP

#include <vrt/graph/compile_result.hpp>
#include <vrt/graph/ir/authored_graph.hpp>

namespace vrt::graph {

/**
 * @brief Validate declarations, producers, scopes, and named port bindings.
 *
 * The pass collects all declarations and producers in each region before it
 * checks any consumer. Forward references are therefore legal: textual
 * authoring order is not execution order.
 */
CompileResult<AuthoredGraph> validateAuthoredGraph(
    const AuthoredGraph& authored);

}  // namespace vrt::graph

#endif  // VRT_GRAPH_PASS_VALIDATE_AUTHORED_GRAPH_HPP
