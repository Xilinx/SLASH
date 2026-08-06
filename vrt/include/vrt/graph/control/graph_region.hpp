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
 * @file graph_region.hpp
 * @brief Public Verilog-like authoring view of a graph region.
 */

#ifndef VRT_GRAPH_CONTROL_GRAPH_REGION_HPP
#define VRT_GRAPH_CONTROL_GRAPH_REGION_HPP

#include <vrt/graph/detail/authoring_region.hpp>

namespace vrt::graph {

/**
 * @brief Scoped region used for raw netlist-style graph authoring.
 *
 * The implementation is shared with the compiler snapshot boundary so the
 * public raw surface and the struct-literal surface build exactly the same IR.
 */
using GraphRegion = detail::AuthoringRegion;

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CONTROL_GRAPH_REGION_HPP
