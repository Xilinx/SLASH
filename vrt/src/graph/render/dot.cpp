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

#include <vrt/graph/render/dot.hpp>

#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/graph.hpp>
#include <vrt/graph/node/io_map.hpp>
#include <vrt/graph/node/compiled_node.hpp>

namespace vrt::graph::render {

namespace {

/// Escape a string for inclusion inside a double-quoted DOT identifier or label.
std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': break;
            default:   out += c;      break;
        }
    }
    return out;
}

const char* deviceTypeShortName(DeviceType dt) {
    switch (dt) {
        case DeviceType::CPU:      return "CPU";
        case DeviceType::GPU:      return "GPU";
        case DeviceType::FPGA:     return "FPGA";
        case DeviceType::MOCK_CPU: return "MOCK_CPU";
    }
    return "?";
}

/// Qualify an authored op id with its enclosing region's scope so it remains
/// unique across the single full-Graph DOT document. Op ids minted by
/// GraphRegion::nextOpId are only unique within their region — different
/// regions independently reset their op counters, so identically-shaped
/// boundary or kernel ids collide once nested clusters are emitted into a
/// single `digraph`. Root-region ids (scope 0) keep their bare form.
std::string qualifiedNodeId(uint64_t scopeId, const std::string& opId) {
    if (scopeId == 0) return opId;
    std::ostringstream os;
    os << "scope" << scopeId << "__" << opId;
    return os.str();
}

/// Build a map: scoped-buffer-key → producing-kernel-node-id.
///
/// Keying on the scoped key (not just the buffer name) keeps the renderer
/// correct when a single DGraph contains buffers with the same name in
/// different scopes - which can happen after Phase 7C output-placement
/// materialisation, where a child DGraph hosts both its body's local
/// buffer and a parent-scope token bridged in for publication.
template <typename KernelT>
std::unordered_map<std::string, std::string> buildProducerMap(
    const std::vector<KernelT>& nodes) {
    std::unordered_map<std::string, std::string> producers;
    for (const auto& n : nodes) {
        for (const auto& [port, buf] : n.ioMap.outputs()) {
            (void)port;
            producers[scopedBufferKey(buf.scopeId(), buf.name())] = n.id;
        }
        for (const auto& rw : n.ioMap.inouts()) {
            producers[scopedBufferKey(rw.out.scopeId(), rw.out.name())] = n.id;
        }
    }
    return producers;
}

/// A buffer this kernel reads, with both the scoped lookup key (used to
/// match against `buildProducerMap`) and the bare buffer name (used for
/// human-readable edge labels).
struct ConsumedBufferRef {
    std::string key;
    std::string name;
};

/// Collect the unique consumer-buffer references a kernel node depends on.
template <typename KernelT>
std::vector<ConsumedBufferRef> consumedBuffers(const KernelT& n) {
    std::vector<ConsumedBufferRef> refs;
    refs.reserve(n.ioMap.inputs().size() + n.ioMap.inouts().size());
    for (const auto& [port, buf] : n.ioMap.inputs()) {
        (void)port;
        refs.push_back({scopedBufferKey(buf.scopeId(), buf.name()), buf.name()});
    }
    for (const auto& rw : n.ioMap.inouts()) {
        refs.push_back({scopedBufferKey(rw.in.scopeId(), rw.in.name()), rw.in.name()});
    }
    return refs;
}

/// Emit a kernel node as a rounded box. `scopeId` is the enclosing region's
/// scope id, used to namespace the Graphviz node id (the visible label keeps
/// the bare authored id).
template <typename KernelT>
void emitKernelNode(std::ostringstream& os, const KernelT& n,
                    uint64_t scopeId, const std::string& indent) {
    os << indent << "\"" << escape(qualifiedNodeId(scopeId, n.id)) << "\""
       << " [label=\"" << escape(n.id) << "\\n[" << escape(n.kernel.name) << "]\"];\n";
}

/// Convenience overload for the per-device DGraph path, which lives in its
/// own `digraph` and therefore needs no scope qualification.
template <typename KernelT>
void emitKernelNode(std::ostringstream& os, const KernelT& n,
                    const std::string& indent) {
    emitKernelNode(os, n, /*scopeId=*/0, indent);
}

/// Emit a bridge-op node as a dashed blue ellipse.
void emitBridgeOpNode(std::ostringstream& os, const CompiledBridgeOpNode& b,
                      const std::string& indent) {
    const char* sideLabel =
        (b.side == CompiledBridgeOpNode::Side::Producer) ? "Producer" : "Consumer";
    std::string opLabel = b.op ? b.op->label() : std::string{"bridge_op"};
    os << indent << "\"" << escape(b.id) << "\""
       << " [shape=ellipse, style=dashed, color=blue, label=\""
       << escape(b.id) << "\\n[" << escape(opLabel) << "]\\n("
       << sideLabel << ")\"];\n";
}

const char* compiledLoopKindLabel(CompiledLoopKind kind) {
    switch (kind) {
        case CompiledLoopKind::FixedCount:     return "FixedCount";
        case CompiledLoopKind::WhileCondition: return "WhileCondition";
    }
    return "?";
}

const char* compareOpLabel(CompareOp op) {
    switch (op) {
        case CompareOp::AlwaysTrue:  return "always true";
        case CompareOp::AlwaysFalse: return "always false";
        case CompareOp::LT:          return "LT";
        case CompareOp::LE:          return "LE";
        case CompareOp::EQ:          return "EQ";
        case CompareOp::GT:          return "GT";
        case CompareOp::GE:          return "GE";
        case CompareOp::NE:          return "NE";
        case CompareOp::EQE:         return "EQE";
        case CompareOp::NEE:         return "NEE";
    }
    return "?";
}

std::string pluralized(size_t count, const char* singular) {
    std::ostringstream os;
    os << count << ' ' << singular;
    if (count != 1) os << 's';
    return os.str();
}

std::string bufferScalarCounts(const char* label, size_t bufferCount, size_t scalarCount) {
    std::ostringstream os;
    os << label << ": " << pluralized(bufferCount, "buffer")
       << ", " << pluralized(scalarCount, "scalar");
    return os.str();
}

std::string scopedName(uint64_t scopeId, const std::string& name) {
    if (scopeId == 0) return name;
    std::ostringstream os;
    os << "scope" << scopeId << ':' << name;
    return os.str();
}

std::string tripCountSummary(const LoopTripCount& tripCount) {
    return "trip: scalar " + scopedName(tripCount.scopeId(), tripCount.name());
}

std::string conditionSummary(const Condition& condition) {
    std::ostringstream os;
    os << "condition: " << compareOpLabel(condition.op());
    if (!condition.isAlways()) {
        size_t scalarCount = 0;
        auto countScalar = [&](const std::optional<ConditionOperand>& operand) {
            if (operand && operand->isScalar()) ++scalarCount;
        };
        countScalar(condition.lhs());
        countScalar(condition.rhs());
        countScalar(condition.epsilon());
        os << " (" << pluralized(scalarCount, "scalar") << ')';
    }
    return os.str();
}

void emitBoundaryNode(std::ostringstream& os, const CompiledBoundaryNode& b,
                      const std::string& indent) {
    const char* sideLabel = (b.side == CompiledBoundaryNode::Side::Start) ? "Start" : "End";
    os << indent << "\"" << escape(b.id) << "\""
       << " [shape=diamond, style=dashed, color=gray, label=\""
       << escape(b.id) << "\\n[Boundary]\\n(" << sideLabel << ")\\n"
       << escape(bufferScalarCounts("copies", b.bufferCopies.size(),
                                    b.scalarCopies.size()))
       << "\"];\n";
}

void emitLoopNode(std::ostringstream& os, const CompiledLoopNode& c,
                  const std::string& indent) {
    std::ostringstream label;
    label << c.id << "\n[Loop]";
    label << "\n(" << compiledLoopKindLabel(c.loopKind) << ')';
    if (c.tripCount) label << "\n" << tripCountSummary(*c.tripCount);
    if (c.condition) label << "\n" << conditionSummary(*c.condition);
    if (!c.outputBufferPublications.empty() || !c.outputScalarPublications.empty()) {
        label << "\n" << bufferScalarCounts("outputs", c.outputBufferPublications.size(),
                                             c.outputScalarPublications.size());
    }
    if (!c.outputBufferPlacements.empty() || !c.outputScalarPlacements.empty()) {
        label << "\n" << bufferScalarCounts("placements", c.outputBufferPlacements.size(),
                                             c.outputScalarPlacements.size());
    }

    os << indent << "\"" << escape(c.id) << "\""
       << " [shape=octagon, style=dashed, color=gray, label=\""
       << escape(label.str()) << "\"];\n";
}

void emitConditionalNode(std::ostringstream& os, const CompiledConditionalNode& c,
                         const std::string& indent) {
    std::ostringstream label;
    label << c.id << "\n[Conditional]";
    label << "\n" << conditionSummary(c.condition);
    if (!c.outputBufferPublications.empty() || !c.outputScalarPublications.empty()) {
        label << "\n" << bufferScalarCounts("outputs", c.outputBufferPublications.size(),
                                             c.outputScalarPublications.size());
    }
    if (!c.outputBufferPlacements.empty() || !c.outputScalarPlacements.empty()) {
        label << "\n" << bufferScalarCounts("placements", c.outputBufferPlacements.size(),
                                             c.outputScalarPlacements.size());
    }

    os << indent << "\"" << escape(c.id) << "\""
       << " [shape=octagon, style=dashed, color=gray, label=\""
       << escape(label.str()) << "\"];\n";
}

void emitDataEdge(std::ostringstream& os,
                  const std::string&  from,
                  const std::string&  to,
                  const std::string&  bufName,
                  const std::string&  indent) {
    os << indent << "\"" << escape(from) << "\" -> \"" << escape(to) << "\""
       << " [label=\"" << escape(bufName) << "\"];\n";
}

void emitScalarEdge(std::ostringstream& os,
                    const std::string&  from,
                    const std::string&  to,
                    const std::string&  scalarName,
                    const std::string&  indent) {
    os << indent << "\"" << escape(from) << "\" -> \"" << escape(to) << "\""
       << " [style=dashed, color=purple, label=\"scalar: "
       << escape(scalarName) << "\"];\n";
}

void emitAfterEdge(std::ostringstream& os,
                   const std::string&  from,
                   const std::string&  to,
                   const std::string&  indent) {
    os << indent << "\"" << escape(from) << "\" -> \"" << escape(to) << "\""
       << " [style=dashed, label=\"after\"];\n";
}

void emitSeqEdge(std::ostringstream& os,
                 const std::string&  from,
                 const std::string&  to,
                 const std::string&  indent) {
    os << indent << "\"" << escape(from) << "\" -> \"" << escape(to) << "\""
       << " [style=dotted, color=gray];\n";
}

const char* boundarySideLabel(BoundarySide side) {
    switch (side) {
        case BoundarySide::Start: return "Start";
        case BoundarySide::End:   return "End";
    }
    return "?";
}

const char* loopKindLabel(LoopKind kind) {
    switch (kind) {
        case LoopKind::FixedCount:     return "FixedCount";
        case LoopKind::WhileCondition: return "WhileCondition";
    }
    return "?";
}

void emitAuthoredBoundaryNode(std::ostringstream& os, const SubgraphBoundaryOp& b,
                              uint64_t scopeId, const std::string& indent) {
    os << indent << "\"" << escape(qualifiedNodeId(scopeId, b.id)) << "\""
       << " [shape=diamond, style=dashed, color=gray, label=\""
       << escape(b.id) << "\\n[Boundary]\\n(" << boundarySideLabel(b.side) << ")\"];\n";
}

void emitAuthoredControlNode(std::ostringstream& os, const LoopOp& loop,
                             uint64_t scopeId, const std::string& indent) {
    os << indent << "\"" << escape(qualifiedNodeId(scopeId, loop.id)) << "\""
       << " [shape=octagon, style=dashed, color=gray, label=\""
       << escape(loop.id) << "\\n[Loop]\\n(" << loopKindLabel(loop.kind) << ")\"];\n";
}

void emitAuthoredControlNode(std::ostringstream& os, const ConditionalOp& conditional,
                             uint64_t scopeId, const std::string& indent) {
    os << indent << "\"" << escape(qualifiedNodeId(scopeId, conditional.id)) << "\""
       << " [shape=octagon, style=dashed, color=gray, label=\""
       << escape(conditional.id) << "\\n[Conditional]\"];\n";
}

struct RenderEdge {
    enum class Kind { Data, Scalar, After };
    Kind kind = Kind::Data;
    std::string from;
    std::string to;
    std::string label;
};

struct AuthoredRenderContext {
    const std::map<std::string, std::shared_ptr<IDevice>>& devices;
    int clusterIdx = 0;
    std::vector<RenderEdge> edges;
    std::map<std::string, std::string> scalarInputs;
};

std::string nextClusterName(AuthoredRenderContext& ctx) {
    return "cluster_" + std::to_string(ctx.clusterIdx++);
}

const IOMap& authoredIoMap(const RegionOp& op) {
    return std::visit(
        [](const auto& concrete) -> const IOMap& {
            return concrete.ioMap;
        },
        op);
}

const std::vector<std::string>& authoredAfterOps(const RegionOp& op) {
    return std::visit(
        [](const auto& concrete) -> const std::vector<std::string>& {
            return concrete.afterOps;
        },
        op);
}

std::string authoredBufferKey(const GraphBuffer& buffer) {
    return scopedBufferKey(buffer.scopeId(), buffer.name());
}

std::string authoredScalarKey(const GraphScalar& scalar) {
    return scopedScalarKey(scalar.scopeId(), scalar.varName());
}

std::string scalarInputNodeId(const GraphScalar& scalar) {
    return "__scalar_input_" + std::to_string(scalar.scopeId()) + "_" + scalar.varName();
}

std::vector<GraphBuffer> authoredConsumedBuffers(const RegionOp& op) {
    const IOMap& ioMap = authoredIoMap(op);
    std::vector<GraphBuffer> buffers;
    buffers.reserve(ioMap.inputs().size() + ioMap.inouts().size());
    for (const auto& [port, buffer] : ioMap.inputs()) {
        (void)port;
        buffers.push_back(buffer);
    }
    for (const auto& rw : ioMap.inouts()) {
        buffers.push_back(rw.in);
    }
    auto appendStartBoundarySources = [&](const std::shared_ptr<GraphRegion>& child) {
        if (!child) return;
        for (const RegionOp& childOp : child->ops()) {
            const auto* boundary = std::get_if<SubgraphBoundaryOp>(&childOp);
            if (!boundary || boundary->side != BoundarySide::Start) continue;
            for (const auto& mapping : boundary->bufferMappings) {
                buffers.push_back(mapping.source);
            }
        }
    };
    if (const auto* loop = std::get_if<LoopOp>(&op)) {
        appendStartBoundarySources(loop->body);
    } else if (const auto* cond = std::get_if<ConditionalOp>(&op)) {
        appendStartBoundarySources(cond->thenRegion);
        appendStartBoundarySources(cond->elseRegion);
    }
    return buffers;
}

std::vector<GraphScalar> conditionScalars(const Condition& condition) {
    std::vector<GraphScalar> scalars;
    auto append = [&](const std::optional<ConditionOperand>& operand) {
        if (operand && operand->isScalar()) {
            scalars.push_back(GraphScalar::ref(operand->type(), operand->name(),
                                               operand->scopeId()));
        }
    };
    append(condition.lhs());
    append(condition.rhs());
    append(condition.epsilon());
    return scalars;
}

std::vector<GraphScalar> authoredConsumedScalars(const RegionOp& op) {
    const IOMap& ioMap = authoredIoMap(op);
    std::vector<GraphScalar> scalars;
    scalars.reserve(ioMap.inputScalars().size() + 2);
    for (const auto& [port, scalar] : ioMap.inputScalars()) {
        (void)port;
        scalars.push_back(scalar);
    }
    if (const auto* loop = std::get_if<LoopOp>(&op)) {
        if (loop->tripCount) {
            scalars.push_back(GraphScalar::ref(loop->tripCount->type(),
                                               loop->tripCount->name(),
                                               loop->tripCount->scopeId()));
        }
        if (loop->condition) {
            auto condScalars = conditionScalars(*loop->condition);
            scalars.insert(scalars.end(), condScalars.begin(), condScalars.end());
        }
    } else if (const auto* cond = std::get_if<ConditionalOp>(&op)) {
        auto condScalars = conditionScalars(cond->condition);
        scalars.insert(scalars.end(), condScalars.begin(), condScalars.end());
    }
    return scalars;
}

/// Identifies the kernel/control op (by qualified Graphviz id) that last writes
/// a particular scoped buffer token. Keyed by `authoredBufferKey(buffer)` so a
/// single map can describe producers across the whole region tree.
struct Producer {
    std::string id;     // qualified Graphviz id of the producing op
    std::string label;  // bare buffer name, kept for symmetry with edge labels
};
using ProducerMap = std::unordered_map<std::string, Producer>;

/// Walk the region tree and register every authored producer in `producers`.
///
/// This must run as a global pre-pass before any edges are emitted, because
/// boundary edges need to resolve producers in scopes other than the boundary's
/// own region (e.g. a parent-region kernel producing a token that's imported
/// into a child region via `SubgraphBoundaryOp::bufferMappings`).
///
/// For each op we register:
///   - `ioMap.outputs` and `ioMap.inouts.out` (the regular kernel /
///     control-op outputs).
///   - For `SubgraphBoundaryOp`: each `bufferMappings[i].target`, with the
///     boundary itself as the producer. This makes local consumers of the
///     boundary's target tokens resolve naturally to a `boundary → consumer`
///     edge in the per-region pass, without that pass having to know about
///     boundaries.
void recordIoMapProducers(const IOMap& ioMap,
                          const std::string& qualifiedId,
                          ProducerMap& producers) {
    for (const auto& [port, buffer] : ioMap.outputs()) {
        (void)port;
        producers[authoredBufferKey(buffer)] =
            Producer{qualifiedId, buffer.name()};
    }
    for (const auto& rw : ioMap.inouts()) {
        producers[authoredBufferKey(rw.out)] =
            Producer{qualifiedId, rw.out.name()};
    }
}

void recordIoMapScalarProducers(const IOMap& ioMap,
                                const std::string& qualifiedId,
                                ProducerMap& producers) {
    for (const auto& [port, scalar] : ioMap.outputScalars()) {
        (void)port;
        producers[authoredScalarKey(scalar)] =
            Producer{qualifiedId, scalar.varName()};
    }
}

void recordControlPublications(const std::string& controlId,
                               const GraphRegion* child,
                               uint64_t parentScopeId,
                               ProducerMap& producers) {
    if (!child) return;
    for (const RegionOp& childOp : child->ops()) {
        const auto* boundary = std::get_if<SubgraphBoundaryOp>(&childOp);
        if (!boundary || boundary->side != BoundarySide::End) continue;
        for (const auto& bm : boundary->bufferMappings) {
            if (bm.target.scopeId() == parentScopeId) {
                producers.emplace(authoredBufferKey(bm.target),
                                  Producer{controlId, bm.target.name()});
            }
        }
    }
}

void collectLogicalProducers(const GraphRegion& region, ProducerMap& producers) {
    const uint64_t scopeId = region.scopeId();
    for (const RegionOp& op : region.ops()) {
        const std::string qualifiedId = qualifiedNodeId(scopeId, regionOpId(op));

        if (const auto* boundary = std::get_if<SubgraphBoundaryOp>(&op)) {
            for (const auto& bm : boundary->bufferMappings) {
                if (boundary->side == BoundarySide::Start ||
                    bm.target.scopeId() == scopeId) {
                    producers[authoredBufferKey(bm.target)] =
                        Producer{qualifiedId, bm.target.name()};
                }
            }
        } else {
            recordIoMapProducers(authoredIoMap(op), qualifiedId, producers);
        }
        recordIoMapScalarProducers(authoredIoMap(op), qualifiedId, producers);

        if (const auto* loop = std::get_if<LoopOp>(&op)) {
            recordControlPublications(qualifiedId, loop->body.get(), scopeId, producers);
            if (loop->body) collectLogicalProducers(*loop->body, producers);
        }
        if (const auto* cond = std::get_if<ConditionalOp>(&op)) {
            recordControlPublications(qualifiedId, cond->thenRegion.get(), scopeId, producers);
            recordControlPublications(qualifiedId, cond->elseRegion.get(), scopeId, producers);
            if (cond->thenRegion) collectLogicalProducers(*cond->thenRegion, producers);
            if (cond->elseRegion) collectLogicalProducers(*cond->elseRegion, producers);
        }
    }
}

void collectBoundaryProducers(const GraphRegion& region, ProducerMap& producers) {
    const uint64_t scopeId = region.scopeId();
    for (const RegionOp& op : region.ops()) {
        const std::string qualifiedId = qualifiedNodeId(scopeId, regionOpId(op));
        const IOMap& ioMap = authoredIoMap(op);
        recordIoMapProducers(ioMap, qualifiedId, producers);
        recordIoMapScalarProducers(ioMap, qualifiedId, producers);

        if (const auto* boundary = std::get_if<SubgraphBoundaryOp>(&op)) {
            for (const auto& bm : boundary->bufferMappings) {
                producers[authoredBufferKey(bm.target)] =
                    Producer{qualifiedId, bm.target.name()};
            }
        }
        if (const auto* loop = std::get_if<LoopOp>(&op)) {
            if (loop->body) collectBoundaryProducers(*loop->body, producers);
        }
        if (const auto* cond = std::get_if<ConditionalOp>(&op)) {
            if (cond->thenRegion) collectBoundaryProducers(*cond->thenRegion, producers);
            if (cond->elseRegion) collectBoundaryProducers(*cond->elseRegion, producers);
        }
    }
}

/// Emit data/after edges for a single region using the pre-built global
/// producer map. Consumers in this region resolve their incoming edges
/// against `producers`, which already contains every authored writer across
/// the whole region tree (including boundary publications).
///
/// In addition to the regular consumer-side edges, every `SubgraphBoundaryOp`
/// emits one edge per `bufferMappings[i]` from the producer of the mapping's
/// *source* token to the boundary itself, labelled with the source name. This
/// renders the data flow into / out of a boundary that was previously
/// invisible in the authored DOT output.
void collectRegionEdges(const GraphRegion& region,
                        const ProducerMap& logicalProducers,
                        const ProducerMap& boundaryProducers,
                        AuthoredRenderContext& ctx) {
    const uint64_t scopeId = region.scopeId();
    std::unordered_set<std::string> idSet;  // bare authored ids local to this region
    idSet.reserve(region.ops().size());
    for (const RegionOp& op : region.ops()) idSet.insert(regionOpId(op));

    for (const RegionOp& op : region.ops()) {
        const std::string toId = qualifiedNodeId(scopeId, regionOpId(op));

        for (const GraphBuffer& buffer : authoredConsumedBuffers(op)) {
            auto pit = logicalProducers.find(authoredBufferKey(buffer));
            if (pit == logicalProducers.end()) continue;
            if (pit->second.id == toId) continue;
            ctx.edges.push_back(RenderEdge{RenderEdge::Kind::Data,
                                           pit->second.id, toId, buffer.name()});
        }

        for (const GraphScalar& scalar : authoredConsumedScalars(op)) {
            auto pit = logicalProducers.find(authoredScalarKey(scalar));
            if (pit == logicalProducers.end()) {
                const std::string id = scalarInputNodeId(scalar);
                ctx.scalarInputs.emplace(id, scopedName(scalar.scopeId(), scalar.varName()));
                ctx.edges.push_back(RenderEdge{RenderEdge::Kind::Scalar,
                                               id, toId, scalar.varName()});
                continue;
            }
            if (pit->second.id == toId) continue;
            ctx.edges.push_back(RenderEdge{RenderEdge::Kind::Scalar,
                                           pit->second.id, toId, scalar.varName()});
        }

        if (const auto* boundary = std::get_if<SubgraphBoundaryOp>(&op)) {
            if (boundary->side == BoundarySide::End) {
                for (const auto& bm : boundary->bufferMappings) {
                    auto pit = boundaryProducers.find(authoredBufferKey(bm.source));
                    if (pit == boundaryProducers.end()) continue;
                    if (pit->second.id == toId) continue;
                    ctx.edges.push_back(RenderEdge{RenderEdge::Kind::Data,
                                                   pit->second.id, toId,
                                                   bm.source.name()});
                }
            }
        }

        for (const std::string& after : authoredAfterOps(op)) {
            if (idSet.count(after)) {
                ctx.edges.push_back(RenderEdge{RenderEdge::Kind::After,
                                               qualifiedNodeId(scopeId, after),
                                               toId, {}});
            }
        }
    }
}

/// Recursively collect edges for every region in the tree using the shared
/// global producer map. Edge collection is intentionally separated from
/// cluster/visual-node emission so that the per-region edge pass has access
/// to producers in *all* regions (not just its own).
void collectAllRegionEdges(const GraphRegion& region,
                           const ProducerMap& logicalProducers,
                           const ProducerMap& boundaryProducers,
                           AuthoredRenderContext& ctx) {
    collectRegionEdges(region, logicalProducers, boundaryProducers, ctx);
    for (const RegionOp& op : region.ops()) {
        if (const auto* loop = std::get_if<LoopOp>(&op)) {
            if (loop->body) collectAllRegionEdges(*loop->body, logicalProducers,
                                                  boundaryProducers, ctx);
        }
        if (const auto* cond = std::get_if<ConditionalOp>(&op)) {
            if (cond->thenRegion) {
                collectAllRegionEdges(*cond->thenRegion, logicalProducers,
                                      boundaryProducers, ctx);
            }
            if (cond->elseRegion) {
                collectAllRegionEdges(*cond->elseRegion, logicalProducers,
                                      boundaryProducers, ctx);
            }
        }
    }
}

std::string deviceClusterLabel(
    const std::string& devId,
    const std::map<std::string, std::shared_ptr<IDevice>>& devices) {
    std::string label = devId;
    auto it = devices.find(devId);
    if (it != devices.end()) {
        label += " [";
        label += deviceTypeShortName(it->second->type());
        label += "]";
    } else if (devId == "_unassigned") {
        label = "(unassigned)";
    }
    return label;
}

void emitAuthoredDeviceClusters(std::ostringstream& os,
                                const GraphRegion& region,
                                AuthoredRenderContext& ctx,
                                const std::string& indent) {
    const uint64_t scopeId = region.scopeId();
    std::map<std::string, std::vector<const KernelOp*>> byDevice;
    for (const RegionOp& op : region.ops()) {
        if (const auto* kernel = std::get_if<KernelOp>(&op)) {
            const std::string key = kernel->deviceHint.empty()
                ? std::string{"_unassigned"}
                : kernel->deviceHint;
            byDevice[key].push_back(kernel);
        }
    }

    for (const auto& [devId, nodePtrs] : byDevice) {
        os << indent << "subgraph " << nextClusterName(ctx) << " {\n";
        os << indent << "  label=\"" << escape(deviceClusterLabel(devId, ctx.devices)) << "\";\n";
        os << indent << "  style=rounded;\n";
        os << indent << "  color=gray;\n";
        for (const KernelOp* node : nodePtrs) {
            emitKernelNode(os, *node, scopeId, indent + "  ");
        }
        os << indent << "}\n";
    }
}

void emitRegionCluster(std::ostringstream& os,
                       const GraphRegion& region,
                       const std::string& label,
                       AuthoredRenderContext& ctx,
                       const std::string& indent) {
    const uint64_t scopeId = region.scopeId();
    os << indent << "subgraph " << nextClusterName(ctx) << " {\n";
    os << indent << "  label=\"" << escape(label) << "\";\n";
    os << indent << "  style=rounded;\n";
    os << indent << "  color=gray;\n";

    emitAuthoredDeviceClusters(os, region, ctx, indent + "  ");

    for (const RegionOp& op : region.ops()) {
        std::visit(
            [&](const auto& concrete) {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<T, KernelOp>) {
                    return;
                } else if constexpr (std::is_same_v<T, SubgraphBoundaryOp>) {
                    emitAuthoredBoundaryNode(os, concrete, scopeId, indent + "  ");
                } else if constexpr (std::is_same_v<T, LoopOp>) {
                    emitAuthoredControlNode(os, concrete, scopeId, indent + "  ");
                    if (concrete.body) {
                        emitRegionCluster(os, *concrete.body, concrete.id + " loop body",
                                          ctx, indent + "  ");
                    }
                } else if constexpr (std::is_same_v<T, ConditionalOp>) {
                    emitAuthoredControlNode(os, concrete, scopeId, indent + "  ");
                    if (concrete.thenRegion) {
                        emitRegionCluster(os, *concrete.thenRegion, concrete.id + " then",
                                          ctx, indent + "  ");
                    }
                    if (concrete.elseRegion) {
                        emitRegionCluster(os, *concrete.elseRegion, concrete.id + " else",
                                          ctx, indent + "  ");
                    }
                }
            },
            op);
    }

    os << indent << "}\n";
}

}  // namespace

std::string renderToDot(const Graph& graph) {
    std::ostringstream os;
    os << "digraph G {\n";
    os << "  rankdir=LR;\n";
    os << "  node [shape=box, style=rounded];\n";

    AuthoredRenderContext ctx{graph.devices()};

    // Phase 1: build producer maps across the whole region tree. Logical
    // producers make control nodes represent parent-visible loop/conditional
    // outputs; boundary producers keep child-region import/export details
    // visible inside nested clusters.
    ProducerMap logicalProducers;
    ProducerMap boundaryProducers;
    collectLogicalProducers(graph.rootRegion(), logicalProducers);
    collectBoundaryProducers(graph.rootRegion(), boundaryProducers);

    // Phase 2: walk every region and emit edges (regular consumers, boundary
    // mappings, and intra-region `afterOps`) into `ctx.edges`.
    collectAllRegionEdges(graph.rootRegion(), logicalProducers, boundaryProducers, ctx);

    // Phase 3: emit the cluster/visual-node tree. Edges are deferred to the
    // top level so Graphviz routes them through cluster boundaries correctly.
    emitRegionCluster(os, graph.rootRegion(), "root region", ctx, "  ");

    for (const auto& [id, name] : ctx.scalarInputs) {
        os << "  \"" << escape(id) << "\""
           << " [shape=note, style=dashed, color=purple, label=\"scalar input\\n"
           << escape(name) << "\"];\n";
    }

    std::unordered_set<std::string> emittedEdges;
    for (const RenderEdge& edge : ctx.edges) {
        const std::string edgeKey =
            std::to_string(static_cast<int>(edge.kind)) + "\n" +
            edge.from + "\n" + edge.to + "\n" + edge.label;
        if (!emittedEdges.insert(edgeKey).second) continue;
        if (edge.kind == RenderEdge::Kind::Data) {
            emitDataEdge(os, edge.from, edge.to, edge.label, "  ");
        } else if (edge.kind == RenderEdge::Kind::Scalar) {
            emitScalarEdge(os, edge.from, edge.to, edge.label, "  ");
        } else {
            emitAfterEdge(os, edge.from, edge.to, "  ");
        }
    }

    os << "}\n";
    return os.str();
}

std::string renderToDot(const DGraph& dgraph) {
    std::ostringstream os;
    os << "digraph \"" << escape(dgraph.deviceId) << "\" {\n";
    os << "  rankdir=LR;\n";
    os << "  node [shape=box, style=rounded];\n";

    std::string typeLabel;
    if (dgraph.device) {
        typeLabel = std::string{" ["} + deviceTypeShortName(dgraph.device->type()) + "]";
    }
    os << "  label=\"" << escape(dgraph.deviceId) << escape(typeLabel) << "\";\n";

    // Collect kernels and bridge ops, and emit their visual nodes.
    std::vector<CompiledKernelNode>  kernels;
    std::unordered_set<std::string>  idSet;
    kernels.reserve(dgraph.nodes.size());
    idSet.reserve(dgraph.nodes.size());

    for (const CompiledNode& node : dgraph.nodes) {
        std::visit(
            [&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                idSet.insert(n.id);
                if constexpr (std::is_same_v<T, CompiledKernelNode>) {
                    emitKernelNode(os, n, "  ");
                    kernels.push_back(n);
                } else if constexpr (std::is_same_v<T, CompiledBridgeOpNode>) {
                    emitBridgeOpNode(os, n, "  ");
                } else if constexpr (std::is_same_v<T, CompiledBoundaryNode>) {
                    emitBoundaryNode(os, n, "  ");
                } else if constexpr (std::is_same_v<T, CompiledLoopNode>) {
                    emitLoopNode(os, n, "  ");
                } else if constexpr (std::is_same_v<T, CompiledConditionalNode>) {
                    emitConditionalNode(os, n, "  ");
                }
            },
            node);
    }

    // Dependency edges, derived directly from each node's `dependsOn`
    // (populated by the compiler). Each entry produces one edge.
    //
    // Edge style:
    //   - Both endpoints are CompiledKernelNodes → solid; labelled with the
    //     shared buffer name when identifiable from IOMap, else
    //     unlabelled.
    //   - Otherwise (any bridge-op endpoint) → dotted gray.

    auto producers = buildProducerMap(kernels);

    std::unordered_set<std::string> kernelIdSet;
    kernelIdSet.reserve(kernels.size());
    for (const auto& k : kernels) kernelIdSet.insert(k.id);

    auto sharedBufferName = [&](const CompiledKernelNode& consumer,
                                const std::string& producerId) -> std::string {
        for (const auto& ref : consumedBuffers(consumer)) {
            auto pit = producers.find(ref.key);
            if (pit != producers.end() && pit->second == producerId) return ref.name;
        }
        return {};
    };

    auto emitForNode = [&](const std::string&  toId,
                           const std::vector<std::string>& deps,
                           const CompiledKernelNode* consumerKernel /*nullable*/) {
        for (const auto& depId : deps) {
            if (!idSet.count(depId)) continue;
            const bool bothKernels =
                kernelIdSet.count(depId) && kernelIdSet.count(toId);
            if (bothKernels) {
                std::string buf;
                if (consumerKernel) buf = sharedBufferName(*consumerKernel, depId);
                if (!buf.empty()) {
                    emitDataEdge(os, depId, toId, buf, "  ");
                } else {
                    os << "  \"" << escape(depId) << "\" -> \"" << escape(toId) << "\";\n";
                }
            } else {
                emitSeqEdge(os, depId, toId, "  ");
            }
        }
    };

    for (const CompiledNode& node : dgraph.nodes) {
        const auto* kernel = std::get_if<CompiledKernelNode>(&node);
        emitForNode(compiledNodeId(node), compiledNodeDependsOn(node), kernel);
    }

    os << "}\n";
    return os.str();
}

namespace {

void writeDotString(const std::string& dot, const std::string& path) {
    std::ofstream ofs(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!ofs) {
        throw std::runtime_error("render::writeToDotFile: cannot open '" + path +
                                 "' for writing");
    }
    ofs.write(dot.data(), static_cast<std::streamsize>(dot.size()));
    if (!ofs) {
        throw std::runtime_error("render::writeToDotFile: write failed for '" + path + "'");
    }
}

}  // namespace

void writeToDotFile(const Graph& graph, const std::string& path) {
    writeDotString(renderToDot(graph), path);
}

void writeToDotFile(const DGraph& dgraph, const std::string& path) {
    writeDotString(renderToDot(dgraph), path);
}

}  // namespace vrt::graph::render
