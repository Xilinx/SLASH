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

const char* stepKindName(ScheduledStepKind kind) {
    switch (kind) {
        case ScheduledStepKind::Operation:       return "Operation";
        case ScheduledStepKind::TransferProduce: return "TransferProduce";
        case ScheduledStepKind::TransferConsume: return "TransferConsume";
        case ScheduledStepKind::TransferAction:  return "TransferAction";
        case ScheduledStepKind::EventPublish:    return "EventPublish";
        case ScheduledStepKind::EventWait:       return "EventWait";
    }
    return "?";
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
        for (NodeId dependency : operation.dependencies) {
            if (graph.operations().count(dependency) == 0) continue;
            out << "  \"n" << dependency.value() << "\" -> \"n"
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
        if (!port.memory.region) continue;
        out << "  \"n" << port.operation.value()
            << "\" [xlabel=\"" << escapeStage(port.port + ": " +
                   port.memory.region->value()) << "\"];\n";
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
            std::to_string(route.requirement.id.value());
        for (const TransferLeg& leg : route.legs) {
            label += "\n" + std::string(mechanismName(leg.mechanism)) +
                     ": " + leg.source.value() + " -> " +
                     leg.destination.value();
        }
        out << "  \"route" << route.requirement.id.value()
            << "\" [shape=diamond,label=\"" << escapeStage(label)
            << "\"];\n";
        if (route.requirement.source.operation) {
            out << "  \"n"
                << route.requirement.source.operation->value()
                << "\" -> \"route"
                << route.requirement.id.value() << "\";\n";
        }
        if (route.requirement.destination.operation) {
            out << "  \"route" << route.requirement.id.value()
                << "\" -> \"n"
                << route.requirement.destination.operation->value()
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
            std::string label = stepKindName(step.kind);
            if (step.operation) {
                label += "\nnode " +
                         std::to_string(step.operation->value());
            }
            if (step.route) {
                label += "\nroute " +
                         std::to_string(step.route->value());
            }
            if (step.rendezvous) {
                label += "\nevent " +
                         std::to_string(step.rendezvous->value());
            }
            if (step.preLaunch) label += "\npre-launch";
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
