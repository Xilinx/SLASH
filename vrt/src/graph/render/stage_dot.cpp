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

#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <vrt/graph/ir/placed_graph.hpp>
#include <vrt/graph/ir/resolved_graph.hpp>
#include <vrt/graph/ir/routed_graph.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>

namespace vrt::graph::render {

namespace {

struct OperationDescription {
    std::string name;
    std::string kind;
};

std::string escapeStage(const std::string& value) {
    std::string result;
    for (char c : value) {
        if (c == '"' || c == '\\') result += '\\';
        if (c == '\n') {
            result += "\\n";
        } else if (c != '\r') {
            result += c;
        }
    }
    return result;
}

void indexOperations(
    const AuthoredRegion& region,
    std::map<NodeId, OperationDescription>& descriptions) {
    for (const AuthoredOperation& operation : region.operations) {
        std::visit(
            [&](const auto& concrete) {
                using T = std::decay_t<decltype(concrete)>;
                std::string kind;
                if constexpr (std::is_same_v<T, AuthoredKernel>) {
                    kind = "Kernel";
                } else if constexpr (
                    std::is_same_v<T, AuthoredReprogram>) {
                    kind = "Reprogram";
                } else if constexpr (
                    std::is_same_v<T, AuthoredBoundary>) {
                    kind = "Boundary";
                } else if constexpr (
                    std::is_same_v<T, AuthoredLoop>) {
                    kind = "Loop";
                } else {
                    kind = "Conditional";
                }
                descriptions[concrete.id] = {
                    concrete.authoredId, std::move(kind)};
                if constexpr (std::is_same_v<T, AuthoredLoop>) {
                    if (concrete.body) {
                        indexOperations(*concrete.body, descriptions);
                    }
                } else if constexpr (
                    std::is_same_v<T, AuthoredConditional>) {
                    if (concrete.thenRegion) {
                        indexOperations(*concrete.thenRegion,
                                        descriptions);
                    }
                    if (concrete.elseRegion) {
                        indexOperations(*concrete.elseRegion,
                                        descriptions);
                    }
                }
            },
            operation);
    }
}

const char* mechanismName(TransferMechanism mechanism) {
    switch (mechanism) {
        case TransferMechanism::DirectBridge:
            return "DirectBridge";
        case TransferMechanism::HostBounce:
            return "HostBounce";
        case TransferMechanism::HostMediatedDeviceCopy:
            return "HostMediatedDeviceCopy";
    }
    return "?";
}

std::string stepDescription(const ScheduledStepPayload& payload) {
    return std::visit(
        [](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, ScheduledOperation>) {
                return std::string("Operation\nnode ") +
                       std::to_string(concrete.operation.value());
            } else if constexpr (
                std::is_same_v<T, ScheduledTransferProduce>) {
                return std::string("TransferProduce\nroute ") +
                       std::to_string(concrete.route.value());
            } else if constexpr (
                std::is_same_v<T, ScheduledTransferConsume>) {
                return std::string("TransferConsume\nroute ") +
                       std::to_string(concrete.route.value());
            } else if constexpr (
                std::is_same_v<T, ScheduledTransferAction>) {
                return std::string("TransferAction\nroute ") +
                       std::to_string(concrete.route.value());
            } else if constexpr (
                std::is_same_v<T, ScheduledEventPublish>) {
                return std::string("EventPublish\nevent ") +
                       std::to_string(concrete.rendezvous.value());
            } else if constexpr (
                std::is_same_v<T, ScheduledEventWait>) {
                return std::string("EventWait\nevent ") +
                       std::to_string(concrete.rendezvous.value());
            } else if constexpr (
                std::is_same_v<T, ScheduledGraphInput>) {
                return std::string("GraphInput");
            } else if constexpr (
                std::is_same_v<T, ScheduledGraphOutput>) {
                return std::string("GraphOutput");
            } else {
                return std::string("Boundary\nnode ") +
                       std::to_string(concrete.boundary.value());
            }
        },
        payload);
}

std::map<NodeId, OperationDescription> descriptions(
    const ResolvedGraph& graph) {
    std::map<NodeId, OperationDescription> result;
    indexOperations(graph.authored().root(), result);
    return result;
}

void emitResolvedNodes(
    std::ostringstream& out, const ResolvedGraph& graph,
    const std::map<NodeId, OperationDescription>& names,
    const std::map<NodeId, DevicePlacement>* placements = nullptr) {
    for (const auto& [id, operation] : graph.operations()) {
        auto name = names.find(id);
        std::string label =
            name == names.end()
                ? "node"
                : name->second.name + "\n[" + name->second.kind + "]";
        if (placements) {
            auto placement = placements->find(id);
            if (placement != placements->end()) {
                label += "\n@" + placement->second.device.value();
            }
        }
        out << "  \"n" << id.value() << "\" [label=\""
            << escapeStage(label) << "\"";
        if (operation.structural) out << ",style=dashed";
        out << "];\n";
    }
    for (const auto& [id, operation] : graph.operations()) {
        for (const ResolvedDependency& dependency :
             operation.dependencies) {
            if (graph.operations().count(dependency.predecessor) == 0) {
                continue;
            }
            out << "  \"n" << dependency.predecessor.value()
                << "\" -> \"n"
                << id.value() << "\";\n";
        }
    }
}

}  // namespace

std::string renderToDot(const ResolvedGraph& graph) {
    std::ostringstream out;
    out << "digraph ResolvedGraph {\n";
    emitResolvedNodes(out, graph, descriptions(graph));
    out << "}\n";
    return out.str();
}

std::string renderToDot(const PlacedGraph& graph) {
    std::ostringstream out;
    out << "digraph PlacedGraph {\n";
    emitResolvedNodes(out, graph.resolved(),
                      descriptions(graph.resolved()),
                      &graph.operationPlacements());
    for (const PortPlacement& port : graph.portPlacements()) {
        const ValueReplica* replica =
            graph.findReplica(port.replica);
        if (!replica || !replica->memory.region) continue;
        out << "  \"n" << port.operation.value()
            << "\" [xlabel=\"" << escapeStage(port.port.value() + ": " +
                   replica->memory.region->value()) << "\"];\n";
    }
    out << "}\n";
    return out.str();
}

std::string renderToDot(const RoutedGraph& graph) {
    std::ostringstream out;
    out << "digraph RoutedGraph {\n";
    emitResolvedNodes(
        out, graph.placed().resolved(),
        descriptions(graph.placed().resolved()),
        &graph.placed().operationPlacements());
    for (const TransferRoute& route : graph.routes()) {
        std::string label = "Route " +
            std::to_string(route.id.value());
        for (const TransferLeg& leg : route.legs) {
            label += "\n" + std::string(mechanismName(leg.mechanism)) +
                     ": " + leg.source.value() + " -> " +
                     leg.destination.value();
        }
        out << "  \"route" << route.id.value()
            << "\" [shape=diamond,label=\"" << escapeStage(label)
            << "\"];\n";
        const std::optional<NodeId> source =
            route.requirement.sourceAnchor.operation();
        const std::optional<NodeId> destination =
            route.requirement.destinationAnchor.operation();
        if (source) {
            out << "  \"n"
                << source->value()
                << "\" -> \"route"
                << route.id.value() << "\";\n";
        }
        if (destination) {
            out << "  \"route" << route.id.value()
                << "\" -> \"n"
                << destination->value()
                << "\";\n";
        }
    }
    out << "}\n";
    return out.str();
}

std::string renderToDot(const ScheduledGraph& graph) {
    std::ostringstream out;
    out << "digraph ScheduledGraph {\n";
    for (const QueueProgram& queue : graph.queues()) {
        out << "  subgraph \"cluster_q" << queue.id.value()
            << "\" {\n    label=\""
            << escapeStage(queue.device.value()) << "\";\n";
        for (ScheduleStepId id : queue.steps) {
            const ScheduledStep& step = graph.steps().at(id);
            const std::string label =
                stepDescription(step.payload);
            out << "    \"s" << id.value() << "\" [label=\""
                << escapeStage(label) << "\"];\n";
        }
        out << "  }\n";
    }
    for (const auto& [id, step] : graph.steps()) {
        for (ScheduleStepId dependency : step.dependencies) {
            out << "  \"s" << dependency.value() << "\" -> \"s"
                << id.value() << "\";\n";
        }
    }
    for (const LogicalRendezvous& rendezvous : graph.rendezvous()) {
        out << "  \"q" << rendezvous.publisher.value()
            << "\" -> \"q" << rendezvous.waiter.value()
            << "\" [style=dashed,label=\"event "
            << rendezvous.id.value() << "\"];\n";
    }
    out << "}\n";
    return out.str();
}

}  // namespace vrt::graph::render
