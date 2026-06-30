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
 * @file region_builder.hpp
 * @brief RegionBuilder — struct-literal authoring surface over a GraphRegion.
 *
 * A RegionBuilder wraps one authored GraphRegion (the root graph, a loop body,
 * or a conditional branch) and exposes the RFC authoring API:
 * `addKernelCall`, `addReprogram`, `addLoop`, `addConditional`, typed token
 * minting (`buffer<T>`), and loop/branch port access (`input` / `output`).
 *
 * It is pure sugar: every call lowers to the existing region IR (KernelOp /
 * ReprogramOp / LoopOp / ConditionalOp + IOMap + subgraph boundaries), so the
 * downstream compiler and device runtimes are unchanged.
 */

#ifndef VRT_GRAPH_AUTHORING_REGION_BUILDER_HPP
#define VRT_GRAPH_AUTHORING_REGION_BUILDER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/authoring/calls.hpp>
#include <vrt/graph/control/control_node.hpp>
#include <vrt/graph/control/graph_region.hpp>
#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/node/io_map.hpp>

namespace vrt::graph {

namespace detail {
inline std::string nextInternalToken(const std::string& base) {
    static std::atomic<uint64_t> counter{0};
    return "__" + base + "_" + std::to_string(counter.fetch_add(1));
}
}  // namespace detail

class RegionBuilder {
   public:
    RegionBuilder() = default;
    explicit RegionBuilder(std::shared_ptr<GraphRegion> region)
        : region_(std::move(region)) {}

    /** @brief The underlying authored region. */
    const std::shared_ptr<GraphRegion>& region() const { return region_; }
    uint64_t scopeId() const { return region_->scopeId(); }

    /** @brief Mint a fresh typed buffer token in this region's scope. */
    template <class T>
    GraphBuffer buffer(std::string name, GraphScalar size) {
        return GraphBuffer::make(typeToBufferType<T>(), std::move(name), region_->scopeId(),
                                 std::move(size));
    }

    /** @brief Per-iteration / per-branch input token bound to a named port. */
    GraphBuffer input(const std::string& port) const {
        auto it = inputs_.find(port);
        if (it == inputs_.end()) {
            throw std::invalid_argument("RegionBuilder::input: unknown port '" + port + "'");
        }
        return it->second;
    }

    /** @brief Per-iteration / per-branch output token bound to a named port. */
    GraphBuffer output(const std::string& port) const {
        auto it = outputs_.find(port);
        if (it == outputs_.end()) {
            throw std::invalid_argument("RegionBuilder::output: unknown port '" + port + "'");
        }
        return it->second;
    }

    /** @brief Author a kernel dispatch from a struct literal. */
    GraphNode addKernelCall(const KernelCallSpec& spec) {
        KernelDescriptor desc{spec.kernel.name, spec.kernel.type, spec.kernel.image,
                              spec.kernel.ioType};
        IOMap io;
        for (const auto& s : spec.inputScalars) io.bindInputScalar(s.port, s.scalar);
        for (const auto& s : spec.outputScalars) io.bindOutputScalar(s.port, s.scalar);
        for (const auto& b : spec.inputs) io.bindInput(b.port, b.buffer);
        for (const auto& b : spec.outputs) io.bindExistingOutput(b.port, b.buffer);
        for (const auto& rw : spec.inouts) {
            io.bindExistingInout(rw.port, rw.port, rw.in, rw.out);
        }
        std::string id = region_->addKernel(std::move(desc), std::move(io),
                                             spec.kernel.deviceId, idsOf(spec.after));
        return GraphNode{std::move(id)};
    }

    /** @brief Author an explicit reprogram (PDI_LOAD) node. */
    GraphNode addReprogram(const ReprogramCallSpec& spec) {
        ReprogramSpec rp;
        rp.imageId = spec.image.imageId;
        rp.pdiPath = spec.image.pdiPath;
        rp.deviceHint = spec.image.deviceId;
        rp.afterOps = idsOf(spec.after);
        return GraphNode{region_->addReprogram(std::move(rp))};
    }

    /**
     * @brief Author a loop as a nested region.
     *
     * Lowers to a child body region with start/end subgraph boundaries plus a
     * LoopOp on this region. A port present in both `.inputs` and `.outputs`
     * is loop-carried (carried through the input token across iterations and
     * published to the output token at the end).
     */
    RegionBuilder addLoop(const LoopBuildSpec& spec);

    /**
     * @brief Author a two-way conditional, returning [thenBranch, elseBranch].
     */
    std::pair<RegionBuilder, RegionBuilder> addConditional(const ConditionalBuildSpec& spec);

   protected:
    static std::vector<std::string> idsOf(const std::vector<GraphNode>& nodes) {
        std::vector<std::string> ids;
        ids.reserve(nodes.size());
        for (const auto& n : nodes) ids.push_back(n.id);
        return ids;
    }

    std::shared_ptr<GraphRegion>       region_;
    std::map<std::string, GraphBuffer> inputs_;
    std::map<std::string, GraphBuffer> outputs_;
};

inline RegionBuilder RegionBuilder::addLoop(const LoopBuildSpec& spec) {
    auto body = region_->createChild();

    std::vector<BufferBoundaryMapping> imports;
    std::vector<BufferBoundaryMapping> exports;

    RegionBuilder loop(body);

    // Carried ports appear in both inputs and outputs.
    auto isOutput = [&](const std::string& port) {
        for (const auto& o : spec.outputs) {
            if (o.port == port) return true;
        }
        return false;
    };

    for (const auto& in : spec.inputs) {
        GraphBuffer localIn = GraphBuffer::make(in.buffer.type(),
                                                detail::nextInternalToken("loop_in_" + in.port),
                                                body->scopeId(),
                                                in.buffer.maybeSizeScalar());
        imports.push_back({in.buffer, localIn});
        loop.inputs_[in.port] = localIn;
    }

    for (const auto& out : spec.outputs) {
        GraphBuffer localOut = GraphBuffer::make(out.buffer.type(),
                                                 detail::nextInternalToken("loop_out_" + out.port),
                                                 body->scopeId(),
                                                 out.buffer.maybeSizeScalar());
        loop.outputs_[out.port] = localOut;
        // Publish the produced value to the parent output token.
        exports.push_back({localOut, out.buffer});
        // If the port is also an input, carry it: the export back to the input
        // token is what the cross-iteration import reads next time around.
        for (const auto& in : spec.inputs) {
            if (in.port == out.port) {
                exports.push_back({localOut, in.buffer});
                break;
            }
        }
    }
    (void)isOutput;

    if (!imports.empty()) body->importFromParent(imports);
    if (!exports.empty()) body->exportToParent(exports);

    LoopSpec loopSpec;
    loopSpec.tripCount = spec.count.value;
    loopSpec.body = body;
    loopSpec.afterOps = idsOf(spec.after);
    region_->addLoop(std::move(loopSpec));

    return loop;
}

inline std::pair<RegionBuilder, RegionBuilder> RegionBuilder::addConditional(
    const ConditionalBuildSpec& spec) {
    auto thenRegion = region_->createChild();
    auto elseRegion = region_->createChild();

    RegionBuilder thenBuilder(thenRegion);
    RegionBuilder elseBuilder(elseRegion);

    auto wireInputs = [&](std::shared_ptr<GraphRegion>& branch, RegionBuilder& builder) {
        std::vector<BufferBoundaryMapping> imports;
        for (const auto& in : spec.inputs) {
            GraphBuffer local = GraphBuffer::make(in.buffer.type(),
                                                  detail::nextInternalToken("if_in_" + in.port),
                                                  branch->scopeId(),
                                                  in.buffer.maybeSizeScalar());
            imports.push_back({in.buffer, local});
            builder.inputs_[in.port] = local;
        }
        if (!imports.empty()) branch->importFromParent(imports);
    };

    auto wireOutputs = [&](std::shared_ptr<GraphRegion>& branch, RegionBuilder& builder) {
        std::vector<BufferBoundaryMapping> exports;
        for (const auto& out : spec.outputs) {
            GraphBuffer local = GraphBuffer::make(out.buffer.type(),
                                                  detail::nextInternalToken("if_out_" + out.port),
                                                  branch->scopeId(),
                                                  out.buffer.maybeSizeScalar());
            builder.outputs_[out.port] = local;
            exports.push_back({local, out.buffer});
        }
        if (!exports.empty()) branch->exportToParent(exports);
    };

    wireInputs(thenRegion, thenBuilder);
    wireInputs(elseRegion, elseBuilder);
    wireOutputs(thenRegion, thenBuilder);
    wireOutputs(elseRegion, elseBuilder);

    ConditionalSpec condSpec;
    condSpec.condition = spec.condition;
    condSpec.thenRegion = thenRegion;
    condSpec.elseRegion = elseRegion;
    condSpec.afterOps = idsOf(spec.after);
    region_->addConditional(std::move(condSpec));

    return {thenBuilder, elseBuilder};
}

}  // namespace vrt::graph

#endif  // VRT_GRAPH_AUTHORING_REGION_BUILDER_HPP
