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
 * @brief Scoped authored graph region used by root graphs and nested control-flow bodies.
 */

#ifndef VRT_GRAPH_CONTROL_GRAPH_REGION_HPP
#define VRT_GRAPH_CONTROL_GRAPH_REGION_HPP

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/control/control_node.hpp>
#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/core/graph_scalar.hpp>

namespace vrt::graph {

class GraphRegion : public std::enable_shared_from_this<GraphRegion> {
   public:
    static std::shared_ptr<GraphRegion> createRoot() {
        return std::shared_ptr<GraphRegion>(new GraphRegion(0, 0));
    }

    std::shared_ptr<GraphRegion> createChild() const {
        return std::shared_ptr<GraphRegion>(new GraphRegion(nextScopeId(), scopeId_));
    }

    uint64_t scopeId() const { return scopeId_; }
    uint64_t parentScopeId() const { return parentScopeId_; }

    GraphBuffer inputBuffer(BufferType type, std::string name,
                            std::optional<GraphScalar> size = std::nullopt) {
        if (bufferNames_.count(name)) {
            throw std::invalid_argument("GraphRegion::inputBuffer: name '" + name + "' already used");
        }
        bufferNames_.insert(name);
        return GraphBuffer::make(type, std::move(name), scopeId_, std::move(size));
    }

    GraphBuffer inputBuffer(BufferType type, std::string name, GraphScalar size) {
        return inputBuffer(type, std::move(name),
                           std::optional<GraphScalar>(std::move(size)));
    }

    GraphScalar scalar(ScalarType type, std::string name) {
        if (scalarTypes_.count(name)) {
            throw std::invalid_argument("GraphRegion::scalar: name '" + name + "' already used");
        }
        scalarTypes_.emplace(name, type);
        return GraphScalar::ref(type, std::move(name), scopeId_);
    }

    GraphScalar inputScalar(ScalarType type, std::string name) {
        if (scalarTypes_.count(name)) {
            throw std::invalid_argument(
                "GraphRegion::inputScalar: name '" + name + "' already used");
        }
        scalarTypes_.emplace(name, type);
        inputScalarTypes_.emplace(name, type);
        return GraphScalar::ref(type, std::move(name), scopeId_);
    }

    std::string addKernel(KernelDescriptor kernel, IOMap ioMap, std::string deviceHint = "",
                          std::vector<std::string> afterOps = {}) {
        std::string id = nextOpId(kernel.name.empty() ? std::string{"kernel"} : kernel.name);
        KernelOp op{std::move(id), std::move(kernel), std::move(deviceHint),
                    std::move(ioMap), std::move(afterOps)};
        const std::string nodeId = op.id;
        addOp(std::move(op));
        return nodeId;
    }

    std::string addReprogram(ReprogramSpec spec) {
        if (spec.imageId.empty()) {
            throw std::invalid_argument("GraphRegion::addReprogram: image id must not be empty");
        }
        if (spec.pdiPath.empty()) {
            throw std::invalid_argument("GraphRegion::addReprogram: PDI path must not be empty");
        }
        ReprogramOp op;
        op.id = nextOpId("reprogram");
        op.imageId = std::move(spec.imageId);
        op.pdiPath = std::move(spec.pdiPath);
        op.deviceHint = std::move(spec.deviceHint);
        op.timeoutCycles = spec.timeoutCycles;
        op.afterOps = std::move(spec.afterOps);
        const std::string id = op.id;
        addOp(std::move(op));
        return id;
    }

    /**
     * @brief Import parent-scope tokens into this region at its entry.
     *
     * Authored on the child region; the resulting boundary op lands in the
     * child's own op list and copies parent-scope sources into local-scope
     * targets when the child runs. Required before any kernel inside the
     * region can read parent-scope tokens.
     *
     * @throws std::invalid_argument  If called on the root region.
     */
    std::string importFromParent(BoundaryMappings mappings,
                                 std::vector<std::string> afterOps = {}) {
        requireNonRoot("importFromParent");
        return addParentBoundary(BoundarySide::Start, std::move(mappings),
                                 std::move(afterOps));
    }

    std::string importFromParent(std::vector<ScalarBoundaryMapping> scalarMappings,
                                 std::vector<std::string> afterOps = {}) {
        requireNonRoot("importFromParent");
        BoundaryMappings mappings;
        mappings.scalars = std::move(scalarMappings);
        return addParentBoundary(BoundarySide::Start, std::move(mappings),
                                 std::move(afterOps));
    }

    std::string importFromParent(std::vector<BufferBoundaryMapping> bufferMappings,
                                 std::vector<std::string> afterOps = {}) {
        requireNonRoot("importFromParent");
        BoundaryMappings mappings;
        mappings.buffers = std::move(bufferMappings);
        return addParentBoundary(BoundarySide::Start, std::move(mappings),
                                 std::move(afterOps));
    }

    /**
     * @brief Export local-scope tokens back to the parent at this region's
     *        exit.
     *
     * Authored on the child region; the resulting boundary op lands in the
     * child's own op list and copies local-scope sources into parent-scope
     * targets when the child finishes. The parent region's compiler treats
     * the surrounding control op (loop / conditional) as the producer of any
     * parent-scope target written here, so subsequent parent-scope readers
     * are wired with a `dependsOn` edge to the control op.
     *
     * @throws std::invalid_argument  If called on the root region.
     */
    std::string exportToParent(BoundaryMappings mappings,
                               std::vector<std::string> afterOps = {}) {
        requireNonRoot("exportToParent");
        return addParentBoundary(BoundarySide::End, std::move(mappings),
                                 std::move(afterOps));
    }

    std::string exportToParent(std::vector<ScalarBoundaryMapping> scalarMappings,
                               std::vector<std::string> afterOps = {}) {
        requireNonRoot("exportToParent");
        BoundaryMappings mappings;
        mappings.scalars = std::move(scalarMappings);
        return addParentBoundary(BoundarySide::End, std::move(mappings),
                                 std::move(afterOps));
    }

    std::string exportToParent(std::vector<BufferBoundaryMapping> bufferMappings,
                               std::vector<std::string> afterOps = {}) {
        requireNonRoot("exportToParent");
        BoundaryMappings mappings;
        mappings.buffers = std::move(bufferMappings);
        return addParentBoundary(BoundarySide::End, std::move(mappings),
                                 std::move(afterOps));
    }

    std::string addLoop(LoopSpec spec) {
        if (!spec.body) {
            throw std::invalid_argument("GraphRegion::addLoop: body region must not be null");
        }
        if (spec.kind == LoopKind::FixedCount && !spec.tripCount) {
            throw std::invalid_argument("GraphRegion::addLoop: fixed-count loop requires a trip count");
        }
        if (spec.kind == LoopKind::WhileCondition && !spec.condition) {
            throw std::invalid_argument("GraphRegion::addLoop: while loop requires a condition");
        }
        LoopOp op;
        op.id = nextOpId(spec.kind == LoopKind::WhileCondition ? "while" : "loop");
        op.ioType = std::move(spec.ioType);
        op.ioMap = std::move(spec.ioMap);
        op.kind = spec.kind;
        op.tripCount = std::move(spec.tripCount);
        op.condition = std::move(spec.condition);
        op.body = std::move(spec.body);
        op.outputPlacement = std::move(spec.outputPlacement);
        op.afterOps = std::move(spec.afterOps);
        const std::string id = op.id;
        addOp(std::move(op));
        return id;
    }

    std::string addConditional(ConditionalSpec spec) {
        if (!spec.condition) {
            throw std::invalid_argument(
                "GraphRegion::addConditional: spec.condition must be set");
        }
        if (!spec.thenRegion || !spec.elseRegion) {
            throw std::invalid_argument(
                "GraphRegion::addConditional: then and else regions must not be null");
        }
        ConditionalOp op;
        op.id = nextOpId("if");
        op.ioType = std::move(spec.ioType);
        op.ioMap = std::move(spec.ioMap);
        op.condition = std::move(*spec.condition);
        op.thenRegion = std::move(spec.thenRegion);
        op.elseRegion = std::move(spec.elseRegion);
        op.outputPlacement = std::move(spec.outputPlacement);
        op.afterOps = std::move(spec.afterOps);
        const std::string id = op.id;
        addOp(std::move(op));
        return id;
    }

    const std::vector<RegionOp>& ops() const { return ops_; }

    /**
     * @brief Returns the set of names registered via inputBuffer() on this region.
     */
    const std::set<std::string>& declaredInputBufferNames() const { return bufferNames_; }

    /**
     * @brief Returns the scalars registered via scalar() on this region keyed
     *        by name with their declared element type.
     */
    const std::map<std::string, ScalarType>& declaredScalars() const { return scalarTypes_; }

    const std::map<std::string, ScalarType>& declaredInputScalars() const {
        return inputScalarTypes_;
    }

    /**
     * @brief Returns the declared element type of @p name on this region, or
     *        std::nullopt if the name was never registered via scalar().
     */
    std::optional<ScalarType> scalarType(const std::string& name) const {
        auto it = scalarTypes_.find(name);
        if (it == scalarTypes_.end()) return std::nullopt;
        return it->second;
    }

   private:
    GraphRegion(uint64_t scopeId, uint64_t parentScopeId)
        : scopeId_(scopeId), parentScopeId_(parentScopeId) {}

    static uint64_t nextScopeId() {
        static std::atomic<uint64_t> next{1};
        return next++;
    }

    std::string nextOpId(const std::string& base) {
        return base + "_" + std::to_string(opCounter_++);
    }

    void addOp(RegionOp op) {
        const std::string id = regionOpId(op);
        if (opIds_.count(id)) {
            throw std::invalid_argument("GraphRegion: duplicate op id '" + id + "'");
        }
        opIds_.insert(id);
        ops_.push_back(std::move(op));
    }

    void requireNonRoot(const char* method) const {
        if (scopeId_ == parentScopeId_) {
            throw std::invalid_argument(
                std::string("GraphRegion::") + method +
                ": must be authored in a child region (root region has no parent)");
        }
    }

    std::string addParentBoundary(BoundarySide side,
                                  BoundaryMappings mappings,
                                  std::vector<std::string> afterOps) {
        SubgraphBoundaryOp op;
        op.id = nextOpId(side == BoundarySide::Start ? "subgraph_start" : "subgraph_end");
        op.side = side;
        op.parentScopeId = parentScopeId_;
        op.localScopeId = scopeId_;
        op.scalarMappings = std::move(mappings.scalars);
        op.bufferMappings = std::move(mappings.buffers);
        op.afterOps = std::move(afterOps);
        const std::string id = op.id;
        addOp(std::move(op));
        return id;
    }

    uint64_t scopeId_ = 0;
    uint64_t parentScopeId_ = 0;
    uint32_t opCounter_ = 0;
    std::set<std::string> bufferNames_;
    std::map<std::string, ScalarType> scalarTypes_;
    std::map<std::string, ScalarType> inputScalarTypes_;
    std::set<std::string> opIds_;
    std::vector<RegionOp> ops_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CONTROL_GRAPH_REGION_HPP