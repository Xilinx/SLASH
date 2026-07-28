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

#include <vrt/graph/semantic_plan.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>
#include <vrt/graph/node/compiled_node.hpp>

namespace vrt::graph {

namespace {

const char* bufferTypeName(BufferType type) {
    switch (type) {
        case BufferType::U8:  return "u8";
        case BufferType::U16: return "u16";
        case BufferType::U32: return "u32";
        case BufferType::U64: return "u64";
        case BufferType::I8:  return "i8";
        case BufferType::I16: return "i16";
        case BufferType::I32: return "i32";
        case BufferType::I64: return "i64";
        case BufferType::F32: return "f32";
        case BufferType::F64: return "f64";
    }
    return "unknown";
}

const char* scalarTypeName(ScalarType type) {
    switch (type) {
        case ScalarType::U8:  return "u8";
        case ScalarType::U16: return "u16";
        case ScalarType::U32: return "u32";
        case ScalarType::U64: return "u64";
        case ScalarType::I8:  return "i8";
        case ScalarType::I16: return "i16";
        case ScalarType::I32: return "i32";
        case ScalarType::I64: return "i64";
        case ScalarType::F32: return "f32";
        case ScalarType::F64: return "f64";
    }
    return "unknown";
}

const char* deviceTypeName(DeviceType type) {
    switch (type) {
        case DeviceType::CPU:      return "cpu";
        case DeviceType::GPU:      return "gpu";
        case DeviceType::FPGA:     return "fpga";
        case DeviceType::MOCK_CPU: return "mock_cpu";
    }
    return "unknown";
}

const char* compareOpName(CompareOp op) {
    switch (op) {
        case CompareOp::AlwaysTrue:  return "always_true";
        case CompareOp::AlwaysFalse: return "always_false";
        case CompareOp::LT:          return "lt";
        case CompareOp::LE:          return "le";
        case CompareOp::EQ:          return "eq";
        case CompareOp::GT:          return "gt";
        case CompareOp::GE:          return "ge";
        case CompareOp::NE:          return "ne";
        case CompareOp::EQE:         return "eqe";
        case CompareOp::NEE:         return "nee";
    }
    return "unknown";
}

const char* childRoleName(DGraphChildRole role) {
    switch (role) {
        case DGraphChildRole::LoopBody:        return "loop_body";
        case DGraphChildRole::ConditionalThen: return "then";
        case DGraphChildRole::ConditionalElse: return "else";
    }
    return "unknown";
}

const char* splitRoleName(SplitBroadcastRole role) {
    switch (role) {
        case SplitBroadcastRole::None:      return "none";
        case SplitBroadcastRole::Authority: return "authority";
        case SplitBroadcastRole::Follower:  return "follower";
    }
    return "unknown";
}

struct ScopedName {
    std::uint64_t scope = 0;
    std::string   name;
    bool          parsed = false;
};

ScopedName parseScopedKey(const std::string& key) {
    constexpr const char* prefix = "scope:";
    if (key.compare(0, 6, prefix) != 0) return {0, key, false};
    const std::size_t separator = key.find(':', 6);
    if (separator == std::string::npos) return {0, key, false};
    try {
        const std::uint64_t scope = std::stoull(key.substr(6, separator - 6));
        return {scope, key.substr(separator + 1), true};
    } catch (const std::exception&) {
        return {0, key, false};
    }
}

class SemanticNormalizer {
   public:
    SemanticPlan normalize(const std::vector<DGraph>& dgraphs) {
        std::vector<const DGraph*> roots;
        roots.reserve(dgraphs.size());
        for (const DGraph& dgraph : dgraphs) roots.push_back(&dgraph);

        SemanticPlan plan;
        plan.root = normalizeRegion(std::move(roots), "root", "", "");
        return plan;
    }

   private:
    using NodeIdMap = std::map<std::string, std::string>;

    std::string scopeId(std::uint64_t raw) {
        auto it = scopes_.find(raw);
        if (it != scopes_.end()) return it->second;
        std::string id = "s" + std::to_string(scopes_.size());
        scopes_.emplace(raw, id);
        return id;
    }

    std::string valueId(char kind, std::uint64_t scope,
                        const std::string& name) {
        const auto key = std::make_tuple(kind, scope, name);
        auto it = values_.find(key);
        if (it != values_.end()) return it->second;
        scopeId(scope);
        std::string id = "v" + std::to_string(values_.size());
        values_.emplace(key, id);
        return id;
    }

    std::string bufferRef(const GraphBuffer& buffer) {
        if (!buffer.valid()) return "invalid";
        std::string result =
            valueId('b', buffer.scopeId(), buffer.name()) + ":" +
            bufferTypeName(buffer.type());
        if (buffer.hasSizeScalar()) {
            result += "[size=" + scalarRef(buffer.sizeScalar()) + "]";
        }
        return result;
    }

    std::string scalarRef(const GraphScalar& scalar) {
        return valueId('s', scalar.scopeId(), scalar.varName()) + ":" +
               scalarTypeName(scalar.type());
    }

    std::string keyRef(char kind, const std::string& key,
                       bool retainPublicName = false) {
        const ScopedName parsed = parseScopedKey(key);
        if (!parsed.parsed) return "unscoped:" + parsed.name;
        std::string result = valueId(kind, parsed.scope, parsed.name);
        if (retainPublicName) result += "(" + parsed.name + ")";
        return result;
    }

    std::string boundaryRef(char kind, std::uint64_t scope,
                            const std::string& name) {
        return valueId(kind, scope, name);
    }

    std::string eventId(std::uint32_t slot) {
        auto it = events_.find(slot);
        if (it != events_.end()) return it->second;
        std::string id = "e" + std::to_string(events_.size());
        events_.emplace(slot, id);
        return id;
    }

    static std::string nodeId(const NodeIdMap& ids,
                              const std::string& raw) {
        auto it = ids.find(raw);
        if (it != ids.end()) return it->second;
        return "external";
    }

    std::string operand(const ConditionOperand& value) {
        if (value.isScalar()) {
            return valueId('s', value.scopeId(), value.name()) + ":" +
                   scalarTypeName(value.type());
        }
        std::ostringstream out;
        out << scalarTypeName(value.type()) << ":0x" << std::hex
            << value.constantBits();
        return out.str();
    }

    std::string condition(const Condition& value) {
        std::ostringstream out;
        out << compareOpName(value.op());
        if (value.lhs()) out << "(" << operand(*value.lhs());
        if (value.rhs()) out << "," << operand(*value.rhs());
        if (value.epsilon()) out << "," << operand(*value.epsilon());
        if (value.lhs()) out << ")";
        return out.str();
    }

    void addIoAttributes(std::map<std::string, std::string>& attributes,
                         const IOMap& ioMap) {
        for (const auto& [port, scalar] : ioMap.inputScalars()) {
            attributes["scalar.input." + port] = scalarRef(scalar);
        }
        for (const auto& [port, scalar] : ioMap.outputScalars()) {
            attributes["scalar.output." + port] = scalarRef(scalar);
        }
        for (const auto& [port, buffer] : ioMap.inputs()) {
            attributes["buffer.input." + port] = bufferRef(buffer);
        }
        for (const auto& [port, buffer] : ioMap.outputs()) {
            attributes["buffer.output." + port] = bufferRef(buffer);
        }
        for (const auto& inout : ioMap.inouts()) {
            attributes["buffer.inout." + inout.inPort + "->" + inout.outPort] =
                bufferRef(inout.in) + "=>" + bufferRef(inout.out);
        }
    }

    SemanticNode normalizeNode(const CompiledNode& node,
                               const NodeIdMap& ids) {
        SemanticNode result;
        result.id = nodeId(ids, compiledNodeId(node));

        std::vector<std::string> rawDependencies =
            compiledNodeDependsOn(node);
        std::sort(rawDependencies.begin(), rawDependencies.end());
        for (const std::string& dependency : rawDependencies) {
            result.dependsOn.push_back(nodeId(ids, dependency));
        }
        std::sort(result.dependsOn.begin(), result.dependsOn.end());
        result.dependsOn.erase(
            std::unique(result.dependsOn.begin(), result.dependsOn.end()),
            result.dependsOn.end());

        std::visit(
            [&](const auto& concrete) {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<T, CompiledKernelNode>) {
                    result.kind = "kernel";
                    result.attributes["kernel"] = concrete.kernel.name;
                    result.attributes["kernel.device_type"] =
                        deviceTypeName(concrete.kernel.type);
                    result.attributes["kernel.image"] =
                        concrete.kernel.image.value_or("");
                    addIoAttributes(result.attributes, concrete.ioMap);
                } else if constexpr (std::is_same_v<T, CompiledBridgeOpNode>) {
                    result.kind =
                        concrete.side == CompiledBridgeOpNode::Side::Producer
                            ? "bridge_producer"
                            : "bridge_consumer";
                    result.attributes["operation"] =
                        concrete.op ? concrete.op->label() : "bridge";
                    result.attributes["peer"] =
                        nodeId(ids, concrete.pairedKernelId);
                } else if constexpr (std::is_same_v<T, CompiledDeviceCopyNode>) {
                    result.kind = "device_copy";
                    result.attributes["source"] = bufferRef(concrete.source);
                    result.attributes["target"] = bufferRef(concrete.target);
                    result.attributes["source_region"] = concrete.sourceRegion;
                    result.attributes["target_region"] = concrete.targetRegion;
                    result.attributes["mechanism"] = concrete.mechanism;
                } else if constexpr (std::is_same_v<T, CompiledSourceNode>) {
                    result.kind = "source";
                    for (std::size_t i = 0; i < concrete.inputBufferKeys.size(); ++i) {
                        result.attributes["buffer." + std::to_string(i)] =
                            keyRef('b', concrete.inputBufferKeys[i], true);
                    }
                    for (std::size_t i = 0; i < concrete.inputScalarKeys.size(); ++i) {
                        result.attributes["scalar." + std::to_string(i)] =
                            keyRef('s', concrete.inputScalarKeys[i], true);
                    }
                } else if constexpr (std::is_same_v<T, CompiledSinkNode>) {
                    result.kind = "sink";
                    for (std::size_t i = 0; i < concrete.outputBufferKeys.size(); ++i) {
                        result.attributes["buffer." + std::to_string(i)] =
                            keyRef('b', concrete.outputBufferKeys[i], true);
                    }
                    for (std::size_t i = 0; i < concrete.outputScalarKeys.size(); ++i) {
                        result.attributes["scalar." + std::to_string(i)] =
                            keyRef('s', concrete.outputScalarKeys[i], true);
                    }
                } else if constexpr (std::is_same_v<T, CompiledReprogramNode>) {
                    result.kind = "reprogram";
                    result.attributes["image"] = concrete.imageId;
                    result.attributes["pdi"] = concrete.pdiPath;
                    result.attributes["timeout_cycles"] =
                        std::to_string(concrete.timeoutCycles);
                } else if constexpr (std::is_same_v<T, CompiledBoundaryNode>) {
                    result.kind =
                        concrete.side == CompiledBoundaryNode::Side::Start
                            ? "boundary_start"
                            : "boundary_end";
                    for (std::size_t i = 0; i < concrete.bufferCopies.size(); ++i) {
                        const auto& copy = concrete.bufferCopies[i];
                        result.attributes["buffer." + std::to_string(i)] =
                            boundaryRef('b', copy.sourceScopeId, copy.sourceName) +
                            "=>" +
                            boundaryRef('b', copy.targetScopeId, copy.targetName);
                    }
                    for (std::size_t i = 0; i < concrete.scalarCopies.size(); ++i) {
                        const auto& copy = concrete.scalarCopies[i];
                        result.attributes["scalar." + std::to_string(i)] =
                            boundaryRef('s', copy.sourceScopeId, copy.sourceName) +
                            "=>" +
                            boundaryRef('s', copy.targetScopeId, copy.targetName);
                    }
                } else if constexpr (std::is_same_v<T, CompiledLoopNode>) {
                    result.kind = "loop";
                    result.attributes["loop_kind"] =
                        concrete.loopKind == CompiledLoopKind::FixedCount
                            ? "fixed"
                            : "while";
                    if (concrete.tripCount) {
                        result.attributes["trip_count"] =
                            valueId('s', concrete.tripCount->scopeId(),
                                    concrete.tripCount->name()) +
                            ":" + scalarTypeName(concrete.tripCount->type());
                    }
                    if (concrete.condition) {
                        result.attributes["condition"] =
                            condition(*concrete.condition);
                    }
                    result.attributes["split_role"] =
                        splitRoleName(concrete.broadcastRole);
                    if (concrete.broadcastRole != SplitBroadcastRole::None) {
                        result.attributes["condition_event"] =
                            eventId(concrete.conditionBroadcastSlot);
                        result.attributes["ready_event"] =
                            eventId(concrete.broadcastReadySlot);
                        result.attributes["ack_event"] =
                            eventId(concrete.broadcastAckSlot);
                    }
                    for (const auto& [port, device] :
                         concrete.outputBufferPlacements) {
                        result.attributes["placement.buffer." + port] = device;
                    }
                    for (const auto& [port, device] :
                         concrete.outputScalarPlacements) {
                        result.attributes["placement.scalar." + port] = device;
                    }
                    for (const auto& publication :
                         concrete.outputBufferPublications) {
                        result.attributes["publication.buffer." +
                                          publication.portName] =
                            boundaryRef('b', publication.parentScopeId,
                                        publication.parentTokenName) +
                            "<-" +
                            boundaryRef('b', publication.sourceScopeId,
                                        publication.sourceTokenName) +
                            "@" + publication.sourceDeviceId;
                    }
                    for (const auto& publication :
                         concrete.outputScalarPublications) {
                        result.attributes["publication.scalar." +
                                          publication.portName] =
                            boundaryRef('s', publication.parentScopeId,
                                        publication.parentTokenName) +
                            "<-" +
                            boundaryRef('s', publication.sourceScopeId,
                                        publication.sourceTokenName) +
                            "@" + publication.sourceDeviceId;
                    }
                } else if constexpr (std::is_same_v<T, CompiledConditionalNode>) {
                    result.kind = "conditional";
                    result.attributes["condition"] =
                        condition(concrete.condition);
                    for (const auto& [port, device] :
                         concrete.outputBufferPlacements) {
                        result.attributes["placement.buffer." + port] = device;
                    }
                    for (const auto& [port, device] :
                         concrete.outputScalarPlacements) {
                        result.attributes["placement.scalar." + port] = device;
                    }
                    for (const auto& publication :
                         concrete.outputBufferPublications) {
                        result.attributes["publication.buffer." +
                                          publication.portName] =
                            boundaryRef('b', publication.parentScopeId,
                                        publication.parentTokenName) +
                            "<-then:" +
                            boundaryRef('b', publication.thenSourceScopeId,
                                        publication.thenSourceTokenName) +
                            "@" + publication.thenSourceDeviceId +
                            ",else:" +
                            boundaryRef('b', publication.elseSourceScopeId,
                                        publication.elseSourceTokenName) +
                            "@" + publication.elseSourceDeviceId;
                    }
                    for (const auto& publication :
                         concrete.outputScalarPublications) {
                        result.attributes["publication.scalar." +
                                          publication.portName] =
                            boundaryRef('s', publication.parentScopeId,
                                        publication.parentTokenName) +
                            "<-then:" +
                            boundaryRef('s', publication.thenSourceScopeId,
                                        publication.thenSourceTokenName) +
                            "@" + publication.thenSourceDeviceId +
                            ",else:" +
                            boundaryRef('s', publication.elseSourceScopeId,
                                        publication.elseSourceTokenName) +
                            "@" + publication.elseSourceDeviceId;
                    }
                } else if constexpr (std::is_same_v<T, CompiledSignalNode>) {
                    result.kind = "signal";
                    result.attributes["event"] = eventId(concrete.slot);
                    result.attributes["value"] = std::to_string(concrete.value);
                    result.attributes["operation"] =
                        std::to_string(concrete.operation);
                } else if constexpr (std::is_same_v<T, CompiledWaitNode>) {
                    result.kind = "wait";
                    result.attributes["event"] = eventId(concrete.slot);
                    result.attributes["value"] = std::to_string(concrete.value);
                    result.attributes["condition"] =
                        std::to_string(concrete.conditionOp);
                    result.attributes["pre_launch"] =
                        concrete.preLaunch ? "true" : "false";
                }
            },
            node);
        return result;
    }

    SemanticRegion normalizeRegion(std::vector<const DGraph*> dgraphs,
                                   std::string path,
                                   std::string parentNode,
                                   std::string role) {
        std::stable_sort(dgraphs.begin(), dgraphs.end(),
                         [](const DGraph* lhs, const DGraph* rhs) {
                             return lhs->deviceId < rhs->deviceId;
                         });
        std::set<const DGraph*> seenDgraphs;
        dgraphs.erase(
            std::remove_if(dgraphs.begin(), dgraphs.end(),
                           [&](const DGraph* dgraph) {
                               return !seenDgraphs.insert(dgraph).second;
                           }),
            dgraphs.end());

        NodeIdMap ids;
        for (const DGraph* dgraph : dgraphs) {
            for (const CompiledNode& node : dgraph->nodes) {
                const std::string& raw = compiledNodeId(node);
                if (ids.count(raw) == 0) {
                    ids.emplace(raw, "n" + std::to_string(ids.size()));
                }
            }
        }

        SemanticRegion region;
        region.path = std::move(path);
        region.parentNode = std::move(parentNode);
        region.role = std::move(role);

        for (const DGraph* dgraph : dgraphs) {
            SemanticQueue queue;
            queue.deviceId = dgraph->deviceId;
            queue.nodes.reserve(dgraph->nodes.size());
            for (const CompiledNode& node : dgraph->nodes) {
                queue.nodes.push_back(normalizeNode(node, ids));
            }
            region.queues.push_back(std::move(queue));
        }

        using ChildKey = std::pair<std::string, DGraphChildRole>;
        std::map<ChildKey, std::vector<const DGraph*>> childGroups;
        for (const DGraph* dgraph : dgraphs) {
            for (const DGraphChild& child : dgraph->childDGraphs) {
                ChildKey key{nodeId(ids, child.parentNodeId), child.role};
                auto& children = childGroups[key];
                for (const auto& childDgraph : child.dgraphs) {
                    if (childDgraph) children.push_back(childDgraph.get());
                }
            }
        }

        for (auto& [key, children] : childGroups) {
            const std::string childRole = childRoleName(key.second);
            const std::string childPath =
                region.path + "/" + key.first + ":" + childRole;
            region.children.push_back(
                normalizeRegion(std::move(children), childPath,
                                key.first, childRole));
        }
        return region;
    }

    std::map<std::uint64_t, std::string> scopes_{{0, "s0"}};
    std::map<std::tuple<char, std::uint64_t, std::string>, std::string> values_;
    std::map<std::uint32_t, std::string> events_;
};

void writeRegion(std::ostringstream& out, const SemanticRegion& region,
                 const std::string& indent) {
    out << indent << "region " << std::quoted(region.path);
    if (!region.role.empty()) {
        out << " parent=" << std::quoted(region.parentNode)
            << " role=" << std::quoted(region.role);
    }
    out << '\n';

    for (const SemanticQueue& queue : region.queues) {
        out << indent << "  queue " << std::quoted(queue.deviceId) << '\n';
        for (const SemanticNode& node : queue.nodes) {
            out << indent << "    " << node.id << ' ' << node.kind;
            if (!node.dependsOn.empty()) {
                out << " depends=[";
                for (std::size_t i = 0; i < node.dependsOn.size(); ++i) {
                    if (i != 0) out << ',';
                    out << node.dependsOn[i];
                }
                out << ']';
            }
            for (const auto& [key, value] : node.attributes) {
                out << ' ' << key << '=' << std::quoted(value);
            }
            out << '\n';
        }
    }
    for (const SemanticRegion& child : region.children) {
        writeRegion(out, child, indent + "  ");
    }
}

}  // namespace

SemanticPlan normalizeSemanticPlan(const std::vector<DGraph>& dgraphs) {
    return SemanticNormalizer().normalize(dgraphs);
}

std::string SemanticPlan::toString() const {
    std::ostringstream out;
    writeRegion(out, root, "");
    return out.str();
}

namespace {

using PlacementKey =
    std::tuple<std::string, std::string, std::string>;
using PlacementDevices =
    std::map<PlacementKey, std::set<std::string>>;

void collectDGraphPlacements(
    std::vector<const DGraph*> dgraphs, const std::string& path,
    PlacementDevices& placements) {
    std::stable_sort(
        dgraphs.begin(), dgraphs.end(),
        [](const DGraph* lhs, const DGraph* rhs) {
            return lhs->deviceId < rhs->deviceId;
        });
    std::set<const DGraph*> seen;
    dgraphs.erase(
        std::remove_if(
            dgraphs.begin(), dgraphs.end(),
            [&](const DGraph* dgraph) {
                return !seen.insert(dgraph).second;
            }),
        dgraphs.end());

    using ChildKey = std::pair<std::string, DGraphChildRole>;
    std::map<ChildKey, std::vector<const DGraph*>> children;
    for (const DGraph* dgraph : dgraphs) {
        for (const CompiledNode& node : dgraph->nodes) {
            std::visit(
                [&](const auto& concrete) {
                    using T = std::decay_t<decltype(concrete)>;
                    const char* kind = nullptr;
                    if constexpr (
                        std::is_same_v<T, CompiledKernelNode>) {
                        kind = "kernel";
                    } else if constexpr (
                        std::is_same_v<T, CompiledReprogramNode>) {
                        kind = "reprogram";
                    } else if constexpr (
                        std::is_same_v<T, CompiledLoopNode>) {
                        kind = "loop";
                    } else if constexpr (
                        std::is_same_v<T,
                                       CompiledConditionalNode>) {
                        kind = "conditional";
                    }
                    if (kind) {
                        placements[{path, concrete.id, kind}].insert(
                            dgraph->deviceId);
                    }
                },
                node);
        }
        for (const DGraphChild& child : dgraph->childDGraphs) {
            auto& group =
                children[{child.parentNodeId, child.role}];
            for (const auto& childDgraph : child.dgraphs) {
                if (childDgraph) group.push_back(childDgraph.get());
            }
        }
    }

    for (auto& [key, childDgraphs] : children) {
        collectDGraphPlacements(
            std::move(childDgraphs),
            path + "/" + key.first + ":" +
                childRoleName(key.second),
            placements);
    }
}

void indexAuthoredPlacements(
    const AuthoredRegion& region, const std::string& path,
    std::map<NodeId, PlacementKey>& placements) {
    for (const AuthoredOperation& operation : region.operations) {
        const std::string& authoredId =
            authoredSourceId(operation);
        std::visit(
            [&](const auto& concrete) {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<T, AuthoredKernel>) {
                    placements[concrete.id] =
                        {path, authoredId, "kernel"};
                } else if constexpr (
                    std::is_same_v<T, AuthoredReprogram>) {
                    placements[concrete.id] =
                        {path, authoredId, "reprogram"};
                } else if constexpr (
                    std::is_same_v<T, AuthoredLoop>) {
                    placements[concrete.id] =
                        {path, authoredId, "loop"};
                    if (concrete.body) {
                        indexAuthoredPlacements(
                            *concrete.body,
                            path + "/" + authoredId + ":loop_body",
                            placements);
                    }
                } else if constexpr (
                    std::is_same_v<T, AuthoredConditional>) {
                    placements[concrete.id] =
                        {path, authoredId, "conditional"};
                    if (concrete.thenRegion) {
                        indexAuthoredPlacements(
                            *concrete.thenRegion,
                            path + "/" + authoredId + ":then",
                            placements);
                    }
                    if (concrete.elseRegion) {
                        indexAuthoredPlacements(
                            *concrete.elseRegion,
                            path + "/" + authoredId + ":else",
                            placements);
                    }
                }
            },
            operation);
    }
}

SemanticPlacementPlan makePlacementPlan(
    const PlacementDevices& placements) {
    SemanticPlacementPlan result;
    for (const auto& [key, devices] : placements) {
        SemanticOperationPlacement operation;
        operation.regionPath = std::get<0>(key);
        operation.authoredId = std::get<1>(key);
        operation.kind = std::get<2>(key);
        operation.devices.assign(devices.begin(), devices.end());
        result.operations.push_back(std::move(operation));
    }
    return result;
}

}  // namespace

SemanticPlacementPlan normalizeOperationPlacements(
    const std::vector<DGraph>& dgraphs) {
    std::vector<const DGraph*> roots;
    roots.reserve(dgraphs.size());
    for (const DGraph& dgraph : dgraphs) roots.push_back(&dgraph);
    PlacementDevices placements;
    collectDGraphPlacements(std::move(roots), "root", placements);
    return makePlacementPlan(placements);
}

SemanticPlacementPlan normalizeOperationPlacements(
    const ScheduledGraph& scheduled) {
    std::map<NodeId, PlacementKey> authored;
    indexAuthoredPlacements(
        scheduled.routed().placed().resolved().authored().root(),
        "root", authored);
    std::map<QueueId, DeviceId> queueDevices;
    for (const QueueProgram& queue : scheduled.queues()) {
        queueDevices[queue.id] = queue.device;
    }

    PlacementDevices placements;
    for (const auto& [id, step] : scheduled.steps()) {
        (void)id;
        if (step.kind != ScheduledStepKind::Operation ||
            !step.operation) {
            continue;
        }
        auto authoredPlacement = authored.find(*step.operation);
        auto device = queueDevices.find(step.queue);
        if (authoredPlacement == authored.end() ||
            device == queueDevices.end()) {
            continue;
        }
        placements[authoredPlacement->second].insert(
            device->second.value());
    }
    return makePlacementPlan(placements);
}

std::string SemanticPlacementPlan::toString() const {
    std::ostringstream out;
    for (const SemanticOperationPlacement& operation : operations) {
        out << operation.regionPath << ' ' << operation.authoredId
            << ' ' << operation.kind << " [";
        for (std::size_t i = 0; i < operation.devices.size(); ++i) {
            if (i != 0) out << ',';
            out << operation.devices[i];
        }
        out << "]\n";
    }
    return out.str();
}

}  // namespace vrt::graph
