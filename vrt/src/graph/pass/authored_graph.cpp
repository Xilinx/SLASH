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
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vrt::graph {

namespace {

class AuthoredSnapshotBuilder {
   public:
    /*
     * Freeze one mutable authoring region into the immutable tree used by
     * later passes. Region and node IDs are process-local but stable within
     * the snapshot; active_ and the depth limit reject malformed recursion
     * before following child control regions.
     */
    std::shared_ptr<const AuthoredRegion> snapshot(
        const detail::AuthoringRegion& region,
        std::optional<RegionId> parent = std::nullopt,
        std::size_t depth = 0) {
        // ponytail: authored control nesting is capped to keep the recursive
        // snapshot/index representation stack-safe; replace both with an
        // iterative walk if valid graphs ever need deeper nesting.
        constexpr std::size_t kMaximumNesting = 256;
        if (depth >= kMaximumNesting) {
            throw std::invalid_argument(
                "AuthoredGraph::snapshot: control nesting is too deep");
        }
        if (!active_.insert(&region).second) {
            throw std::invalid_argument(
                "AuthoredGraph::snapshot: recursive child-region cycle");
        }
        auto result = std::make_shared<AuthoredRegion>();
        result->id = RegionId(nextRegion_++);
        result->parent = parent;
        result->sourceGraph = region.graphId();
        result->sourceScope = AuthoredScopeId(region.scopeId());
        result->sourceParentScope =
            AuthoredScopeId(region.parentScopeId());
        result->declaredInputBuffers = region.declaredInputBuffers();
        result->declaredOutputBuffers = region.declaredOutputBuffers();
        result->declaredScalars = region.declaredScalars();
        result->declaredInputScalars = region.declaredInputScalars();
        result->declaredOutputScalars = region.declaredOutputScalars();

        /*
         * Reserve every sibling node ID before visiting any child region.
         * A nested control tree therefore cannot change how authored
         * dependencies between operations in this region are translated.
         */
        const std::vector<RegionOp>& sourceOperations = region.ops();
        std::vector<NodeId> ids;
        ids.reserve(sourceOperations.size());
        std::map<std::string, NodeId> idsByAuthoredName;
        for (const RegionOp& operation : sourceOperations) {
            const NodeId id(nextNode_++);
            ids.push_back(id);
            idsByAuthoredName.emplace(regionOpId(operation), id);
        }

        /*
         * Copy leaf operations directly and recurse only for loop and
         * conditional children. All name-based dependencies now resolve
         * against the complete sibling ID table built above.
         */
        result->operations.reserve(sourceOperations.size());
        for (std::size_t i = 0; i < sourceOperations.size(); ++i) {
            result->operations.push_back(snapshotOperation(
                sourceOperations[i], ids[i], result->id,
                idsByAuthoredName, depth));
        }
        active_.erase(&region);
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
            result.buffers.emplace(PortName(port), DeviceId(device));
        }
        for (const auto& [port, device] : source.scalars) {
            result.scalars.emplace(PortName(port), DeviceId(device));
        }
        return result;
    }

    /*
     * Convert the authoring variant without losing variant-specific data.
     * Kernels, reprograms, and boundaries are leaves; loops and conditionals
     * additionally snapshot their child regions while preserving output
     * placement and the parent region link.
     */
    AuthoredOperation snapshotOperation(
        const RegionOp& operation, NodeId id, RegionId region,
        const std::map<std::string, NodeId>& idsByAuthoredName,
        std::size_t depth) {
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
                    result.sourceParentScope =
                        AuthoredScopeId(source.parentScopeId);
                    result.sourceLocalScope =
                        AuthoredScopeId(source.localScopeId);
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
                    result.body =
                        snapshot(*source.body, region, depth + 1);
                    result.outputPlacement =
                        placementHints(source.outputPlacement);
                    for (const auto& [port, buffer] :
                         source.namedOutputBuffers) {
                        result.namedOutputBuffers.emplace(
                            PortName(port), buffer);
                    }
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
                    result.thenRegion =
                        snapshot(*source.thenRegion, region, depth + 1);
                    result.elseRegion =
                        snapshot(*source.elseRegion, region, depth + 1);
                    result.outputPlacement =
                        placementHints(source.outputPlacement);
                    for (const auto& [port, buffer] :
                         source.namedOutputBuffers) {
                        result.namedOutputBuffers.emplace(
                            PortName(port), buffer);
                    }
                    result.after = dependencies(source.afterOps,
                                                idsByAuthoredName);
                    return result;
                }
            },
            operation);
    }

    std::uint64_t nextRegion_ = 0;
    std::uint64_t nextNode_ = 0;
    std::set<const detail::AuthoringRegion*> active_;
};

}  // namespace

RegionTreeIndex::RegionTreeIndex(const AuthoredRegion& root) {
    indexRegion(root, std::nullopt);
}

/*
 * Build all reverse lookups in the same recursive walk over the snapshot.
 * Each operation belongs to exactly one region, while each child records
 * both its owning control node and its arm so later passes need not recover
 * tree relationships from operation variants.
 */
void RegionTreeIndex::indexRegion(
    const AuthoredRegion& region,
    std::optional<NodeId> parentControlValue) {
    regions_.emplace(region.id, &region);
    regionsByScope_.emplace(region.sourceScope, region.id);
    parentControls_.emplace(region.id, parentControlValue);
    auto& childEntries = children_[region.id];

    for (const AuthoredOperation& operation : region.operations) {
        const NodeId node = authoredNodeId(operation);
        operations_.emplace(node, &operation);
        operationRegions_.emplace(node, region.id);
        if (const auto* loop = std::get_if<AuthoredLoop>(&operation)) {
            if (!loop->body) continue;
            childEntries.push_back(
                {loop->body->id, node, AuthoredChildRole::LoopBody});
            indexRegion(*loop->body, node);
        } else if (const auto* conditional =
                       std::get_if<AuthoredConditional>(&operation)) {
            if (conditional->thenRegion) {
                childEntries.push_back(
                    {conditional->thenRegion->id, node,
                     AuthoredChildRole::ConditionalThen});
                indexRegion(*conditional->thenRegion, node);
            }
            if (conditional->elseRegion) {
                childEntries.push_back(
                    {conditional->elseRegion->id, node,
                     AuthoredChildRole::ConditionalElse});
                indexRegion(*conditional->elseRegion, node);
            }
        }
    }
}

const AuthoredRegion* RegionTreeIndex::findRegion(RegionId id) const {
    auto found = regions_.find(id);
    return found == regions_.end() ? nullptr : found->second;
}

const AuthoredOperation* RegionTreeIndex::findOperation(NodeId id) const {
    auto found = operations_.find(id);
    return found == operations_.end() ? nullptr : found->second;
}

std::optional<RegionId> RegionTreeIndex::regionForScope(
    AuthoredScopeId scope) const {
    auto found = regionsByScope_.find(scope);
    return found == regionsByScope_.end()
               ? std::nullopt
               : std::optional<RegionId>(found->second);
}

std::optional<RegionId> RegionTreeIndex::regionForOperation(
    NodeId operation) const {
    auto found = operationRegions_.find(operation);
    return found == operationRegions_.end()
               ? std::nullopt
               : std::optional<RegionId>(found->second);
}

std::optional<NodeId> RegionTreeIndex::parentControl(
    RegionId region) const {
    auto found = parentControls_.find(region);
    return found == parentControls_.end() ? std::nullopt
                                         : found->second;
}

const std::vector<AuthoredChildRegion>& RegionTreeIndex::children(
    RegionId region) const {
    static const std::vector<AuthoredChildRegion> empty;
    auto found = children_.find(region);
    return found == children_.end() ? empty : found->second;
}

AuthoredGraph AuthoredGraph::snapshot(const detail::AuthoringRegion& root) {
    AuthoredSnapshotBuilder builder;
    return AuthoredGraph(builder.snapshot(root));
}

}  // namespace vrt::graph
