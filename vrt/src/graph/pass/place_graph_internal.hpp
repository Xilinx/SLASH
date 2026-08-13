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

#ifndef VRT_GRAPH_PASS_PLACE_GRAPH_INTERNAL_HPP
#define VRT_GRAPH_PASS_PLACE_GRAPH_INTERNAL_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <vrt/graph/ir/placed_graph.hpp>

namespace vrt::graph::place_detail {

struct ReplicaKey {
    ValueId        value;
    MemoryPlacement memory;

    bool operator<(const ReplicaKey& other) const {
        return std::tie(value, memory) <
               std::tie(other.value, other.memory);
    }
};

/*
 * Placement grows monotonically in this state. A value has at most one
 * replica at a given memory location, while primaryReplicas names the
 * definition selected for later routing. Region summaries are complete
 * before their parent control is considered.
 */
struct PlacementState {
    std::shared_ptr<const ResolvedGraph>         resolved;
    const DeviceCapabilityCatalog*               capabilities = nullptr;
    Diagnostics                                  diagnostics;
    std::map<NodeId, DevicePlacement>            operationPlacements;
    std::map<NodeId, ControlPlacement>           controlPlacements;
    std::map<ReplicaId, ValueReplica>             replicas;
    std::map<ValueId, ReplicaId>                  primaryReplicas;
    std::map<ReplicaKey, ReplicaId>               replicasByLocation;
    std::vector<PortPlacement>                   portPlacements;
    std::vector<BoundaryMappingPlacement>         boundaryMappings;
    std::map<RegionId, RegionPlacementSummary>   regionSummaries;
    std::uint64_t                                 nextReplica = 0;
};

DiagnosticLocation location(
    const AuthoredRegion& region,
    const AuthoredOperation& operation,
    std::optional<std::string> port = std::nullopt);

bool predicateAvailableOnCandidate(
    const PlacementState& state,
    const AuthoredOperation& operation, DeviceId candidate);

ControlCapabilityRequest controlRequest(
    const PlacementState& state,
    const AuthoredOperation& operation, DeviceId candidate,
    const RegionPlacementSummary& child);

/*
 * These stages are order-sensitive: region ownership precedes port replicas,
 * control results consume those replicas, and boundaries consume all prior
 * placement decisions.
 */
void placeRegions(PlacementState& state);
void placeReplicas(PlacementState& state);
void placeControlResults(PlacementState& state);
void placeBoundaryMappings(PlacementState& state);

/*
 * Replica identity is interned by value and memory. Marking an existing
 * replica primary promotes its definition metadata instead of creating a
 * second identity for the same storage.
 */
ReplicaId ensureReplica(
    PlacementState& state, ValueId value,
    const MemoryPlacement& memory, ReplicaPurpose purpose,
    std::optional<NodeId> operation = std::nullopt,
    std::optional<PortName> port = std::nullopt,
    bool primary = false);

const ValueReplica* primaryReplica(
    const PlacementState& state, ValueId value);

std::set<DeviceId> operationDevices(
    const PlacementState& state, NodeId operation);

}  // namespace vrt::graph::place_detail

#endif  // VRT_GRAPH_PASS_PLACE_GRAPH_INTERNAL_HPP
