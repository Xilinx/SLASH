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

#ifndef VRT_GRAPH_PASS_ROUTE_GRAPH_INTERNAL_HPP
#define VRT_GRAPH_PASS_ROUTE_GRAPH_INTERNAL_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/ir/routed_graph.hpp>

namespace vrt::graph::route_detail {

/*
 * target names an existing placed replica when one exists; targetMemory is
 * authoritative even for fallback uses. copyOnWrite requires storage private
 * to this consumer rather than a location-interned transfer target.
 */
struct ValueUse {
    NodeId                  consumer;
    ValueId                 value;
    std::optional<ReplicaId> target;
    MemoryPlacement         targetMemory;
    bool                    copyOnWrite = false;
};

struct SelectedTransferLeg {
    TransferMechanism mechanism = TransferMechanism::DirectBridge;
    DeviceId          source;
    DeviceId          destination;
    TransferExecutor  executor =
        DestinationQueueTransferExecutor{};
};

/*
 * A selection distinguishes no transfer from an unavailable transfer.
 * Successful motion is an ordered leg chain; an empty, error-free selection
 * means the endpoints already share storage.
 */
struct RouteSelection {
    bool                             transferRequired = false;
    std::vector<SelectedTransferLeg> legs;
    std::optional<DiagCode>          error;
    std::string                      message;
};

/*
 * Placement owns original replicas; this state owns only transfer targets.
 * Route signatures are interned, replica locations are unique unless
 * isolation is requested, and dataDependencies records ordering already
 * supplied by value flow.
 */
struct RoutingState {
    std::shared_ptr<const PlacedGraph> placed;
    const TransferCapabilityCatalog*  capabilities = nullptr;
    Diagnostics                       diagnostics;
    std::uint64_t                     nextRoute = 0;
    std::uint64_t                     nextReplica = 0;
    std::map<ReplicaId, ValueReplica> transferReplicas;
    std::map<std::pair<ValueId, MemoryPlacement>, ReplicaId>
        replicasByLocation;
    std::map<RouteSignature, RouteId> routeIds;
    std::vector<DependencyEdge> dependencies;
    std::vector<TransferRoute> routes;
    std::set<std::pair<NodeId, NodeId>> dataDependencies;

    const ValueReplica* findReplica(ReplicaId id) const;
};

std::vector<ValueUse> discoverValueUses(const PlacedGraph& placed);

/*
 * Data planners populate both routes and dependency evidence before authored
 * order dependencies are considered. Validation runs only on the closed set.
 */
void planValueRoutes(
    RoutingState& state, const std::vector<ValueUse>& uses);
void planControlPublicationRoutes(RoutingState& state);
void planGraphOutputRoutes(RoutingState& state);
void planOrderDependencies(RoutingState& state);
void validateMutableStorage(RoutingState& state);
void validateRoutes(RoutingState& state);

/*
 * Selection prefers no-op locality, a required same-device copy, one direct
 * bridge, then a two-leg host bounce; failure is explicit when none applies.
 */
RouteSelection selectRoute(
    const RouteSignature& signature,
    const TransferCapabilityCatalog& capabilities,
    bool forceCopy = false);

/*
 * Ordinary transfer targets are interned by value and memory so compatible
 * consumers can share materialization. Isolated mutable targets bypass this
 * helper and receive a fresh identity.
 */
ReplicaId ensureTransferReplica(
    RoutingState& state, ValueId value,
    const MemoryPlacement& memory,
    std::optional<NodeId> operation = std::nullopt);

std::optional<RouteId> planDataRoute(
    RoutingState& state, ReplicaId source, ReplicaId destination,
    TransferPayloadKind payload,
    std::optional<NodeId> producer,
    std::optional<NodeId> consumer,
    std::vector<RouteId> prerequisites = {},
    bool isolatedDestination = false);

std::optional<RouteId> planBarrierRoute(
    RoutingState& state, NodeId producer, NodeId consumer,
    const MemoryPlacement& source,
    const MemoryPlacement& destination);

TransferPhase transferPhase(
    const PlacedGraph& placed,
    std::optional<NodeId> producer,
    std::optional<NodeId> consumer);

TransferControlScope transferScope(
    const PlacedGraph& placed, TransferPhase phase,
    std::optional<NodeId> producer,
    std::optional<NodeId> consumer);

TransferControlAnchor sourceAnchor(
    const PlacedGraph& placed,
    std::optional<NodeId> producer);

TransferControlAnchor destinationAnchor(
    const PlacedGraph& placed, TransferPhase phase,
    std::optional<NodeId> consumer);

}  // namespace vrt::graph::route_detail

#endif  // VRT_GRAPH_PASS_ROUTE_GRAPH_INTERNAL_HPP
