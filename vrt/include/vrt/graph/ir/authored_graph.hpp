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
 * @brief Detached, immutable snapshot of an authored GraphRegion tree.
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

#include <vrt/graph/control/graph_region.hpp>
#include <vrt/graph/ids.hpp>

namespace vrt::graph {

struct AuthoredRegion;

struct AuthoredDependency {
    std::string           authoredId;
    std::optional<NodeId> target;
};

struct AuthoredPlacementHints {
    std::map<std::string, DeviceId> buffers;
    std::map<std::string, DeviceId> scalars;
};

struct AuthoredKernel {
    NodeId                          id;
    std::string                     authoredId;
    KernelDescriptor                kernel;
    DeviceId                        device;
    IOMap                           ioMap;
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
    std::uint64_t                     sourceParentScopeId = 0;
    std::uint64_t                     sourceLocalScopeId = 0;
    std::vector<ScalarBoundaryMapping> scalarMappings;
    std::vector<BufferBoundaryMapping> bufferMappings;
    std::vector<AuthoredDependency>    after;
};

struct AuthoredLoop {
    NodeId                                id;
    std::string                           authoredId;
    IOTypeMap                             ioType;
    IOMap                                 ioMap;
    LoopKind                              kind = LoopKind::FixedCount;
    std::optional<LoopTripCount>          tripCount;
    std::optional<Condition>              condition;
    std::shared_ptr<const AuthoredRegion> body;
    AuthoredPlacementHints                outputPlacement;
    std::map<std::string, GraphBuffer>    namedOutputBuffers;
    std::vector<AuthoredDependency>       after;
};

struct AuthoredConditional {
    NodeId                                id;
    std::string                           authoredId;
    IOTypeMap                             ioType;
    IOMap                                 ioMap;
    Condition                             condition = Condition::alwaysFalse();
    std::shared_ptr<const AuthoredRegion> thenRegion;
    std::shared_ptr<const AuthoredRegion> elseRegion;
    AuthoredPlacementHints                outputPlacement;
    std::map<std::string, GraphBuffer>    namedOutputBuffers;
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
    RegionId                         id;
    std::optional<RegionId>          parent;
    std::uint64_t                    sourceScopeId = 0;
    std::uint64_t                    sourceParentScopeId = 0;
    std::map<std::string, GraphBuffer> declaredInputBuffers;
    std::map<std::string, GraphBuffer> declaredOutputBuffers;
    std::map<std::string, ScalarType> declaredScalars;
    std::map<std::string, ScalarType> declaredInputScalars;
    std::map<std::string, ScalarType> declaredOutputScalars;
    std::vector<AuthoredOperation>   operations;
};

class AuthoredGraph {
   public:
    static AuthoredGraph snapshot(const GraphRegion& root);

    const AuthoredRegion& root() const { return *root_; }
    const std::shared_ptr<const AuthoredRegion>& rootPtr() const {
        return root_;
    }

   private:
    explicit AuthoredGraph(std::shared_ptr<const AuthoredRegion> root)
        : root_(std::move(root)) {}

    std::shared_ptr<const AuthoredRegion> root_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_IR_AUTHORED_GRAPH_HPP
