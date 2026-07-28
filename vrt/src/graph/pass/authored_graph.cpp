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

#include <vrt/graph/ir/authored_graph.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vrt::graph {

namespace {

class AuthoredSnapshotBuilder {
   public:
    std::shared_ptr<const AuthoredRegion> snapshot(
        const GraphRegion& region,
        std::optional<RegionId> parent = std::nullopt) {
        auto result = std::make_shared<AuthoredRegion>();
        result->id = RegionId(nextRegion_++);
        result->parent = parent;
        result->sourceScopeId = region.scopeId();
        result->sourceParentScopeId = region.parentScopeId();
        result->declaredInputBuffers = region.declaredInputBuffers();
        result->declaredOutputBuffers = region.declaredOutputBuffers();
        result->declaredScalars = region.declaredScalars();
        result->declaredInputScalars = region.declaredInputScalars();
        result->declaredOutputScalars = region.declaredOutputScalars();

        const std::vector<RegionOp>& sourceOperations = region.ops();
        std::vector<NodeId> ids;
        ids.reserve(sourceOperations.size());
        std::map<std::string, NodeId> idsByAuthoredName;
        for (const RegionOp& operation : sourceOperations) {
            const NodeId id(nextNode_++);
            ids.push_back(id);
            idsByAuthoredName.emplace(regionOpId(operation), id);
        }

        result->operations.reserve(sourceOperations.size());
        for (std::size_t i = 0; i < sourceOperations.size(); ++i) {
            result->operations.push_back(snapshotOperation(
                sourceOperations[i], ids[i], result->id, idsByAuthoredName));
        }
        return result;
    }

   private:
    static std::vector<AuthoredDependency> dependencies(
        const std::vector<std::string>& authoredDependencies,
        const std::map<std::string, NodeId>& idsByAuthoredName) {
        std::vector<AuthoredDependency> result;
        result.reserve(authoredDependencies.size());
        for (const std::string& authoredId : authoredDependencies) {
            AuthoredDependency dependency;
            dependency.authoredId = authoredId;
            auto it = idsByAuthoredName.find(authoredId);
            if (it != idsByAuthoredName.end()) dependency.target = it->second;
            result.push_back(std::move(dependency));
        }
        return result;
    }

    static AuthoredPlacementHints placementHints(
        const ControlOutputPlacementHints& source) {
        AuthoredPlacementHints result;
        for (const auto& [port, device] : source.buffers) {
            result.buffers.emplace(port, DeviceId(device));
        }
        for (const auto& [port, device] : source.scalars) {
            result.scalars.emplace(port, DeviceId(device));
        }
        return result;
    }

    AuthoredOperation snapshotOperation(
        const RegionOp& operation, NodeId id, RegionId region,
        const std::map<std::string, NodeId>& idsByAuthoredName) {
        return std::visit(
            [&](const auto& source) -> AuthoredOperation {
                using T = std::decay_t<decltype(source)>;
                if constexpr (std::is_same_v<T, KernelOp>) {
                    AuthoredKernel result;
                    result.id = id;
                    result.authoredId = source.id;
                    result.kernel = source.kernel;
                    result.device = DeviceId(source.device);
                    result.ioMap = source.ioMap;
                    result.after = dependencies(source.afterOps,
                                                idsByAuthoredName);
                    return result;
                } else if constexpr (std::is_same_v<T, ReprogramOp>) {
                    AuthoredReprogram result;
                    result.id = id;
                    result.authoredId = source.id;
                    result.imageId = source.imageId;
                    result.pdiPath = source.pdiPath;
                    result.device = DeviceId(source.device);
                    result.timeoutCycles = source.timeoutCycles;
                    result.after = dependencies(source.afterOps,
                                                idsByAuthoredName);
                    return result;
                } else if constexpr (std::is_same_v<T, SubgraphBoundaryOp>) {
                    AuthoredBoundary result;
                    result.id = id;
                    result.authoredId = source.id;
                    result.side = source.side;
                    result.sourceParentScopeId = source.parentScopeId;
                    result.sourceLocalScopeId = source.localScopeId;
                    result.scalarMappings = source.scalarMappings;
                    result.bufferMappings = source.bufferMappings;
                    result.after = dependencies(source.afterOps,
                                                idsByAuthoredName);
                    return result;
                } else if constexpr (std::is_same_v<T, LoopOp>) {
                    AuthoredLoop result;
                    result.id = id;
                    result.authoredId = source.id;
                    result.ioType = source.ioType;
                    result.ioMap = source.ioMap;
                    result.kind = source.kind;
                    result.tripCount = source.tripCount;
                    result.condition = source.condition;
                    result.body = snapshot(*source.body, region);
                    result.outputPlacement =
                        placementHints(source.outputPlacement);
                    result.namedOutputBuffers =
                        source.namedOutputBuffers;
                    result.after = dependencies(source.afterOps,
                                                idsByAuthoredName);
                    return result;
                } else {
                    static_assert(std::is_same_v<T, ConditionalOp>);
                    AuthoredConditional result;
                    result.id = id;
                    result.authoredId = source.id;
                    result.ioType = source.ioType;
                    result.ioMap = source.ioMap;
                    result.condition = source.condition;
                    result.thenRegion = snapshot(*source.thenRegion, region);
                    result.elseRegion = snapshot(*source.elseRegion, region);
                    result.outputPlacement =
                        placementHints(source.outputPlacement);
                    result.namedOutputBuffers =
                        source.namedOutputBuffers;
                    result.after = dependencies(source.afterOps,
                                                idsByAuthoredName);
                    return result;
                }
            },
            operation);
    }

    std::uint64_t nextRegion_ = 0;
    std::uint64_t nextNode_ = 0;
};

}  // namespace

AuthoredGraph AuthoredGraph::snapshot(const GraphRegion& root) {
    AuthoredSnapshotBuilder builder;
    return AuthoredGraph(builder.snapshot(root));
}

}  // namespace vrt::graph
