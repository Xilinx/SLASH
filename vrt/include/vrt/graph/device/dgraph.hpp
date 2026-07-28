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
 * @file dgraph.hpp
 * @brief DGraph — per-device compiled subgraph produced by GraphCompiler.
 *
 * A DGraph is the output of the compilation step for a single device.  It
 * contains:
 *  - The ordered list of `CompiledNode`s assigned to that device.
 *    Bridge-synthesised ops are spliced inline among compiled kernels by the
 *    compiler.
 *  - Optional child DGraph groups owned by parent-level compiled control nodes.
 *    Device runtimes use these groups to execute nested loop bodies and
 *    conditional branches once structured control-flow execution is enabled.
 *  - A pointer to the IDevice responsible for executing the subgraph.
 *
 * DGraph is an internal compiler artifact; it is not part of the user-facing API.
 */

#ifndef VRT_GRAPH_DEVICE_DGRAPH_HPP
#define VRT_GRAPH_DEVICE_DGRAPH_HPP

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <vrt/graph/device/device.hpp>
#include <vrt/graph/node/compiled_node.hpp>

namespace vrt::graph {

struct DGraph;

enum class DGraphChildRole {
    LoopBody,
    ConditionalThen,
    ConditionalElse,
};

struct DGraphChild {
    std::string parentNodeId;
    DGraphChildRole role = DGraphChildRole::LoopBody;
    std::vector<std::shared_ptr<DGraph>> dgraphs;
};

struct DGraph {
    /**
     * @brief ID of the device this subgraph targets (matches IDevice::id()).
     */
    std::string deviceId;

    /**
    * @brief Compiled nodes assigned to this device, in topological order.
     */
    std::vector<CompiledNode> nodes;

    /**
     * @brief The device that will compile and execute this subgraph.
     */
    std::shared_ptr<IDevice> device;

    /**
     * @brief Shared graph-owned scalar state visible to this device runtime.
     */
    std::shared_ptr<std::map<std::string, uint64_t>> scalarValues;

    /**
     * @brief Physical synchronization resources are owned by ExecutionPlan.
     */
    bool resourcesLeased = false;

    /**
     * @brief Nested per-device DGraphs owned by compiled control nodes in this DGraph.
     */
    std::vector<DGraphChild> childDGraphs;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DEVICE_DGRAPH_HPP
