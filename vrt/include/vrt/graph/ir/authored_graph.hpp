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
 * @file authored_graph.hpp
 * @brief Detached, immutable snapshot of an authored detail::AuthoringRegion tree.
 */

#ifndef VRT_GRAPH_IR_AUTHORED_GRAPH_HPP
#define VRT_GRAPH_IR_AUTHORED_GRAPH_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <vrt/graph/detail/authoring_region.hpp>
#include <vrt/graph/ids.hpp>

namespace vrt::graph {

struct AuthoredRegion;

struct AuthoredDependency {
    std::string           authoredId;
    std::optional<NodeId> target;
};

struct AuthoredPlacementHints {
    std::map<PortName, DeviceId> buffers;
    std::map<PortName, DeviceId> scalars;
};

struct AuthoredKernel {
    NodeId                          id;
    std::string                     authoredId;
    KernelDescriptor                kernel;
    DeviceId                        device;
    detail::PortBindings                           ioMap;
    std::vector<AuthoredDependency> after;
};

struct AuthoredReprogram {
    NodeId                          id;
    std::string                     authoredId;
    std::string                     imageId;
    std::string                     pdiPath;
    DeviceId                        device;
    std::uint32_t                   timeoutCycles = 0;
    std::vector<AuthoredDependency> after;
};

struct AuthoredBoundary {
    NodeId                            id;
    std::string                       authoredId;
    BoundarySide                      side = BoundarySide::Start;
    AuthoredScopeId                   sourceParentScope;
    AuthoredScopeId                   sourceLocalScope;
    std::vector<ScalarBoundaryMapping> scalarMappings;
    std::vector<BufferBoundaryMapping> bufferMappings;
    std::vector<AuthoredDependency>    after;
};

struct AuthoredLoop {
    NodeId                                id;
    std::string                           authoredId;
    IOTypeMap                             ioType;
    detail::PortBindings                                 ioMap;
    LoopKind                              kind = LoopKind::FixedCount;
    std::optional<LoopTripCount>          tripCount;
    std::optional<Condition>              condition;
    std::shared_ptr<const AuthoredRegion> body;
    AuthoredPlacementHints                outputPlacement;
    std::map<PortName, GraphBuffer>       namedOutputBuffers;
    std::vector<AuthoredDependency>       after;
};

struct AuthoredConditional {
    NodeId                                id;
    std::string                           authoredId;
    IOTypeMap                             ioType;
    detail::PortBindings                                 ioMap;
    Condition                             condition = Condition::alwaysFalse();
    std::shared_ptr<const AuthoredRegion> thenRegion;
    std::shared_ptr<const AuthoredRegion> elseRegion;
    AuthoredPlacementHints                outputPlacement;
    std::map<PortName, GraphBuffer>       namedOutputBuffers;
    std::vector<AuthoredDependency>       after;
};

using AuthoredOperation =
    std::variant<AuthoredKernel, AuthoredReprogram, AuthoredBoundary,
                 AuthoredLoop, AuthoredConditional>;

inline NodeId authoredNodeId(const AuthoredOperation& operation) {
    return std::visit([](const auto& value) { return value.id; }, operation);
}

inline const std::string& authoredSourceId(
    const AuthoredOperation& operation) {
    return std::visit(
        [](const auto& value) -> const std::string& {
            return value.authoredId;
        },
        operation);
}

struct AuthoredRegion {
    RegionId                           id;
    std::optional<RegionId>            parent;
    std::uint64_t                      sourceGraph = 0;
    AuthoredScopeId                    sourceScope;
    AuthoredScopeId                    sourceParentScope;
    std::map<std::string, GraphBuffer> declaredInputBuffers;
    std::map<std::string, GraphBuffer> declaredOutputBuffers;
    std::map<std::string, ScalarType>   declaredScalars;
    std::map<std::string, ScalarType>   declaredInputScalars;
    std::map<std::string, ScalarType>   declaredOutputScalars;
    std::vector<AuthoredOperation>      operations;
};

enum class AuthoredChildRole {
    LoopBody,
    ConditionalThen,
    ConditionalElse,
};

struct AuthoredChildRegion {
    RegionId          region;
    NodeId            control;
    AuthoredChildRole role = AuthoredChildRole::LoopBody;
};

/**
 * @brief Immutable lookup index for one detached authored region tree.
 *
 * Every compiler pass shares this index instead of recursively rebuilding
 * node, region, parent-control, and source-scope maps. Pointers returned by
 * the index remain valid for the lifetime of the owning AuthoredGraph.
 */
class RegionTreeIndex {
   public:
    const AuthoredRegion* findRegion(RegionId id) const;
    const AuthoredOperation* findOperation(NodeId id) const;
    std::optional<RegionId> regionForScope(AuthoredScopeId scope) const;
    std::optional<RegionId> regionForOperation(NodeId operation) const;
    std::optional<NodeId> parentControl(RegionId region) const;
    const std::vector<AuthoredChildRegion>& children(RegionId region) const;

    const std::map<RegionId, const AuthoredRegion*>& regions() const {
        return regions_;
    }
    const std::map<NodeId, const AuthoredOperation*>& operations() const {
        return operations_;
    }

   private:
    friend class AuthoredGraph;
    explicit RegionTreeIndex(const AuthoredRegion& root);

    void indexRegion(const AuthoredRegion& region,
                     std::optional<NodeId> parentControl);

    std::map<RegionId, const AuthoredRegion*> regions_;
    std::map<NodeId, const AuthoredOperation*> operations_;
    std::map<AuthoredScopeId, RegionId> regionsByScope_;
    std::map<NodeId, RegionId> operationRegions_;
    std::map<RegionId, std::optional<NodeId>> parentControls_;
    std::map<RegionId, std::vector<AuthoredChildRegion>> children_;
};

class AuthoredGraph {
   public:
    static AuthoredGraph snapshot(const detail::AuthoringRegion& root);

    const AuthoredRegion& root() const { return *data_->root; }
    const std::shared_ptr<const AuthoredRegion>& rootPtr() const {
        return data_->root;
    }
    const RegionTreeIndex& index() const { return data_->index; }

   private:
    explicit AuthoredGraph(std::shared_ptr<const AuthoredRegion> root)
        : data_(std::make_shared<Data>(std::move(root))) {}

    struct Data {
        explicit Data(std::shared_ptr<const AuthoredRegion> rootValue)
            : root(std::move(rootValue)), index(*root) {}

        std::shared_ptr<const AuthoredRegion> root;
        RegionTreeIndex                       index;
    };

    std::shared_ptr<const Data> data_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_IR_AUTHORED_GRAPH_HPP
