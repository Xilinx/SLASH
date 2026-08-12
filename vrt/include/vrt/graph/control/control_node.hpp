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
 * @brief Authored control-flow op variants stored inside a detail::AuthoringRegion.
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
#include <vrt/graph/detail/port_bindings.hpp>
#include <vrt/graph/node/io_type_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>

namespace vrt::graph {

namespace detail {
class AuthoringRegion;
}

/**
 * @brief Which side of a child-region boundary an import/export node models.
 */
enum class BoundarySide {
    /** Copy parent-scope tokens into child-scope tokens before the child runs. */
    Start,
    /** Copy child-scope tokens back to parent-scope tokens after the child runs. */
    End,
};

/**
 * @brief Mapping from one scalar token to another across a region boundary.
 */
struct ScalarBoundaryMapping {
    /** Token read by the boundary op. */
    GraphScalar source;
    /** Token written by the boundary op. */
    GraphScalar target;
};

/**
 * @brief Mapping from one buffer token to another across a region boundary.
 */
struct BufferBoundaryMapping {
    /** Token read by the boundary op. */
    GraphBuffer source;
    /** Token written by the boundary op. */
    GraphBuffer target;
};

/**
 * @brief Scalar and buffer mappings carried by a subgraph boundary op.
 */
struct BoundaryMappings {
    /** Scalar token copies performed by the boundary. */
    std::vector<ScalarBoundaryMapping> scalars;
    /** Buffer token copies performed by the boundary. */
    std::vector<BufferBoundaryMapping> buffers;
};

/**
 * @brief Authored kernel dispatch stored in a detail::AuthoringRegion.
 */
struct KernelOp {
    /** Stable authored operation id. */
    std::string id;
    /** Kernel identity and typed port signature. */
    KernelDescriptor kernel;
    /** Target device id, matching IDevice::id(). */
    std::string device;
    /** Concrete graph-token bindings for this dispatch. */
    detail::PortBindings ioMap;
    /** Explicit side-effect ordering dependencies by authored op id. */
    std::vector<std::string> afterOps;
};

/**
 * @brief User-authored request to load an FPGA image before later dispatches.
 */
namespace detail {
struct ReprogramRecord {
    /** User-facing image id registered on the FPGA device. */
    std::string imageId;
    /** PDI file path to stage and load. */
    std::string pdiPath;
    /** Target FPGA device id. */
    std::string device;
    /** Optional backend-specific timeout in RP1 cycles; 0 selects backend default. */
    uint32_t timeoutCycles = 0;
    /** Explicit side-effect ordering dependencies by authored op id. */
    std::vector<std::string> afterOps;
};
}  // namespace detail

/**
 * @brief Authored reprogram op stored in a detail::AuthoringRegion.
 */
struct ReprogramOp {
    /** Stable authored operation id. */
    std::string id;
    /** User-facing image id registered on the FPGA device. */
    std::string imageId;
    /** PDI file path to stage and load. */
    std::string pdiPath;
    /** Target FPGA device id. */
    std::string device;
    /** Optional backend-specific timeout in RP1 cycles; 0 selects backend default. */
    uint32_t timeoutCycles = 0;
    /** Reserved signature slot; reprogram ops currently have no user ports. */
    IOTypeMap ioType;
    /** Reserved binding slot; reprogram ops currently have no user ports. */
    detail::PortBindings ioMap;
    /** Explicit side-effect ordering dependencies by authored op id. */
    std::vector<std::string> afterOps;
};

/**
 * @brief Compiler-visible copy op at the start or end of a child region.
 */
struct SubgraphBoundaryOp {
    /** Stable authored operation id. */
    std::string id;
    /** Whether this boundary imports into or exports from the child region. */
    BoundarySide side = BoundarySide::Start;
    /** Parent scope id for boundary validation and scoped-token keys. */
    uint64_t parentScopeId = 0;
    /** Child/local scope id for boundary validation and scoped-token keys. */
    uint64_t localScopeId = 0;
    /** Reserved signature slot; concrete mappings carry the boundary I/O. */
    IOTypeMap ioType;
    /** Reserved binding slot; concrete mappings carry the boundary I/O. */
    detail::PortBindings ioMap;
    /** Scalar token copies performed by this boundary. */
    std::vector<ScalarBoundaryMapping> scalarMappings;
    /** Buffer token copies performed by this boundary. */
    std::vector<BufferBoundaryMapping> bufferMappings;
    /** Explicit side-effect ordering dependencies by authored op id. */
    std::vector<std::string> afterOps;
};

/**
 * @brief Loop execution mode.
 */
enum class LoopKind {
    /** Run a fixed number of iterations from a LoopTripCount. */
    FixedCount,
    /** Re-evaluate a Condition before each iteration. */
    WhileCondition,
};

/**
 * @brief Optional backend placement hints for values produced by control ops.
 */
struct ControlOutputPlacementHints {
    /** Preferred device id for each output buffer port. */
    std::map<std::string, std::string> buffers;
    /** Preferred device id for each output scalar port. */
    std::map<std::string, std::string> scalars;
};

/**
 * @brief User-authored loop region specification.
 */
namespace detail {
struct LoopRecord {
    /** Typed boundary ports exposed by the loop. */
    IOTypeMap ioType;
    /** Parent-scope inputs, outputs, and inouts bound to loop ports. */
    detail::PortBindings ioMap;
    /** Fixed-count or while-condition loop form. */
    LoopKind kind = LoopKind::FixedCount;
    /** Required when @ref kind is LoopKind::FixedCount. */
    std::optional<LoopTripCount> tripCount;
    /** Required when @ref kind is LoopKind::WhileCondition. */
    std::optional<Condition> condition;
    /** Child region containing the loop body. */
    std::shared_ptr<detail::AuthoringRegion> body;
    /** Optional backend placement hints for loop outputs. */
    ControlOutputPlacementHints outputPlacement;
    /** Preferred-authoring port names mapped to parent output tokens. */
    std::map<std::string, GraphBuffer> namedOutputBuffers;
    /** Explicit side-effect ordering dependencies by authored op id. */
    std::vector<std::string> afterOps;
};
}  // namespace detail

/**
 * @brief Authored loop op stored in a detail::AuthoringRegion.
 */
struct LoopOp {
    /** Stable authored operation id. */
    std::string id;
    /** Typed boundary ports exposed by the loop. */
    IOTypeMap ioType;
    /** Parent-scope inputs, outputs, and inouts bound to loop ports. */
    detail::PortBindings ioMap;
    /** Fixed-count or while-condition loop form. */
    LoopKind kind = LoopKind::FixedCount;
    /** Fixed iteration count, if this is a fixed-count loop. */
    std::optional<LoopTripCount> tripCount;
    /** Loop predicate, if this is a while-condition loop. */
    std::optional<Condition> condition;
    /** Child region containing the loop body. */
    std::shared_ptr<detail::AuthoringRegion> body;
    /** Optional backend placement hints for loop outputs. */
    ControlOutputPlacementHints outputPlacement;
    /** Preferred-authoring port names mapped to parent output tokens. */
    std::map<std::string, GraphBuffer> namedOutputBuffers;
    /** Explicit side-effect ordering dependencies by authored op id. */
    std::vector<std::string> afterOps;
};

/**
 * @brief User-authored two-branch conditional region specification.
 */
namespace detail {
struct ConditionalRecord {
    /** Typed boundary ports exposed by the conditional. */
    IOTypeMap ioType;
    /** Parent-scope inputs, outputs, and inouts bound to conditional ports. */
    detail::PortBindings ioMap;
    /// Required for ConditionalRecord; detail::AuthoringRegion::addConditional throws if absent.
    std::optional<Condition> condition;
    /** Branch region executed when @ref condition evaluates true. */
    std::shared_ptr<detail::AuthoringRegion> thenRegion;
    /** Branch region executed when @ref condition evaluates false. */
    std::shared_ptr<detail::AuthoringRegion> elseRegion;
    /** Optional backend placement hints for conditional outputs. */
    ControlOutputPlacementHints outputPlacement;
    /** Preferred-authoring port names mapped to parent output tokens. */
    std::map<std::string, GraphBuffer> namedOutputBuffers;
    /** Explicit side-effect ordering dependencies by authored op id. */
    std::vector<std::string> afterOps;
};
}  // namespace detail

/** @brief Public raw reprogram specification. */
using ReprogramSpec = detail::ReprogramRecord;

/** @brief Public raw loop-region specification. */
using LoopSpec = detail::LoopRecord;

/** @brief Public raw conditional-region specification. */
using ConditionalSpec = detail::ConditionalRecord;

/**
 * @brief Authored conditional op stored in a detail::AuthoringRegion.
 */
struct ConditionalOp {
    /** Stable authored operation id. */
    std::string id;
    /** Typed boundary ports exposed by the conditional. */
    IOTypeMap ioType;
    /** Parent-scope inputs, outputs, and inouts bound to conditional ports. */
    detail::PortBindings ioMap;
    /** Predicate evaluated to choose between @ref thenRegion and @ref elseRegion. */
    Condition condition = Condition::alwaysFalse();
    /** Branch region executed when @ref condition evaluates true. */
    std::shared_ptr<detail::AuthoringRegion> thenRegion;
    /** Branch region executed when @ref condition evaluates false. */
    std::shared_ptr<detail::AuthoringRegion> elseRegion;
    /** Optional backend placement hints for conditional outputs. */
    ControlOutputPlacementHints outputPlacement;
    /** Preferred-authoring port names mapped to parent output tokens. */
    std::map<std::string, GraphBuffer> namedOutputBuffers;
    /** Explicit side-effect ordering dependencies by authored op id. */
    std::vector<std::string> afterOps;
};

/**
 * @brief Any operation that can appear in an authored graph region.
 */
using RegionOp = std::variant<KernelOp, ReprogramOp, SubgraphBoundaryOp, LoopOp, ConditionalOp>;

/**
 * @brief Return the stable authored id of any RegionOp variant.
 */
inline const std::string& regionOpId(const RegionOp& op) {
    return std::visit(
        [](const auto& concrete) -> const std::string& {
            return concrete.id;
        },
        op);
}

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CONTROL_CONTROL_NODE_HPP
