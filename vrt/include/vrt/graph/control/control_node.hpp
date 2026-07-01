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
 * @file control_node.hpp
 * @brief Authored control-flow op variants stored inside a GraphRegion.
 */

#ifndef VRT_GRAPH_CONTROL_CONTROL_NODE_HPP
#define VRT_GRAPH_CONTROL_CONTROL_NODE_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <vrt/graph/control/condition.hpp>
#include <vrt/graph/node/io_map.hpp>
#include <vrt/graph/node/io_type_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>

namespace vrt::graph {

class GraphRegion;

enum class BoundarySide {
    Start,
    End,
};

struct ScalarBoundaryMapping {
    GraphScalar source;
    GraphScalar target;
};

struct BufferBoundaryMapping {
    GraphBuffer source;
    GraphBuffer target;
};

struct BoundaryMappings {
    std::vector<ScalarBoundaryMapping> scalars;
    std::vector<BufferBoundaryMapping> buffers;
};

struct KernelOp {
    std::string id;
    KernelDescriptor kernel;
    std::string device;
    IOMap ioMap;
    std::vector<std::string> afterOps;
};

struct ReprogramSpec {
    std::string imageId;
    std::string pdiPath;
    std::string device;
    uint32_t timeoutCycles = 0;
    std::vector<std::string> afterOps;
};

struct ReprogramOp {
    std::string id;
    std::string imageId;
    std::string pdiPath;
    std::string device;
    uint32_t timeoutCycles = 0;
    IOTypeMap ioType;
    IOMap ioMap;
    std::vector<std::string> afterOps;
};

struct SubgraphBoundaryOp {
    std::string id;
    BoundarySide side = BoundarySide::Start;
    uint64_t parentScopeId = 0;
    uint64_t localScopeId = 0;
    IOTypeMap ioType;
    IOMap ioMap;
    std::vector<ScalarBoundaryMapping> scalarMappings;
    std::vector<BufferBoundaryMapping> bufferMappings;
    std::vector<std::string> afterOps;
};

enum class LoopKind {
    FixedCount,
    WhileCondition,
};

struct ControlOutputPlacementHints {
    std::map<std::string, std::string> buffers;
    std::map<std::string, std::string> scalars;
};

struct LoopSpec {
    IOTypeMap ioType;
    IOMap ioMap;
    LoopKind kind = LoopKind::FixedCount;
    std::optional<LoopTripCount> tripCount;
    std::optional<Condition> condition;
    std::shared_ptr<GraphRegion> body;
    ControlOutputPlacementHints outputPlacement;
    std::vector<std::string> afterOps;
};

struct LoopOp {
    std::string id;
    IOTypeMap ioType;
    IOMap ioMap;
    LoopKind kind = LoopKind::FixedCount;
    std::optional<LoopTripCount> tripCount;
    std::optional<Condition> condition;
    std::shared_ptr<GraphRegion> body;
    ControlOutputPlacementHints outputPlacement;
    std::vector<std::string> afterOps;
};

struct ConditionalSpec {
    IOTypeMap ioType;
    IOMap ioMap;
    /// Required for ConditionalSpec; GraphRegion::addConditional throws if absent.
    std::optional<Condition> condition;
    std::shared_ptr<GraphRegion> thenRegion;
    std::shared_ptr<GraphRegion> elseRegion;
    ControlOutputPlacementHints outputPlacement;
    std::vector<std::string> afterOps;
};

struct ConditionalOp {
    std::string id;
    IOTypeMap ioType;
    IOMap ioMap;
    Condition condition = Condition::alwaysFalse();
    std::shared_ptr<GraphRegion> thenRegion;
    std::shared_ptr<GraphRegion> elseRegion;
    ControlOutputPlacementHints outputPlacement;
    std::vector<std::string> afterOps;
};

using RegionOp = std::variant<KernelOp, ReprogramOp, SubgraphBoundaryOp, LoopOp, ConditionalOp>;

inline const std::string& regionOpId(const RegionOp& op) {
    return std::visit(
        [](const auto& concrete) -> const std::string& {
            return concrete.id;
        },
        op);
}

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CONTROL_CONTROL_NODE_HPP