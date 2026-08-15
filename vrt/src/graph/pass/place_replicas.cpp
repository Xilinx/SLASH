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

#include "place_graph_internal.hpp"

#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace vrt::graph::place_detail {

/*
 * A value/location pair denotes one physical replica. Uses may discover it
 * before the defining port does; primary promotion therefore updates that
 * replica's role and provenance without changing its identity.
 */
ReplicaId ensureReplica(
    PlacementState& state, ValueId value,
    const MemoryPlacement& memory, ReplicaPurpose purpose,
    std::optional<NodeId> operation,
    std::optional<PortName> port, bool primary) {
    const ReplicaKey key{value, memory};
    auto existing = state.replicasByLocation.find(key);
    if (existing != state.replicasByLocation.end()) {
        if (primary) {
            ValueReplica& replica = state.replicas.at(existing->second);
            replica.purpose = purpose;
            replica.operation = std::move(operation);
            replica.port = std::move(port);
            state.primaryReplicas[value] = existing->second;
        }
        return existing->second;
    }

    ValueReplica replica;
    replica.id = ReplicaId(state.nextReplica++);
    replica.value = value;
    replica.memory = memory;
    replica.purpose = purpose;
    replica.operation = std::move(operation);
    replica.port = std::move(port);
    const ReplicaId id = replica.id;
    state.replicas.emplace(id, std::move(replica));
    state.replicasByLocation.emplace(key, id);
    if (primary) state.primaryReplicas[value] = id;
    return id;
}

const ValueReplica* primaryReplica(
    const PlacementState& state, ValueId value) {
    auto primary = state.primaryReplicas.find(value);
    if (primary == state.primaryReplicas.end()) return nullptr;
    auto replica = state.replicas.find(primary->second);
    return replica == state.replicas.end() ? nullptr : &replica->second;
}

namespace {

/*
 * Graph inputs and outputs share one host-visible ownership domain. Zero or
 * multiple candidates is ambiguous because silently choosing a host would
 * make graph I/O placement depend on catalog order.
 */
std::optional<DeviceId> graphIoHost(PlacementState& state) {
    const std::vector<DeviceId> hosts =
        state.capabilities->graphIoHosts();
    if (hosts.size() == 1) return hosts.front();
    state.diagnostics.error(
        DiagCode::AmbiguousPlacement,
        "GraphCompiler: graph values require exactly one graph I/O host");
    return std::nullopt;
}

void placeGraphInputs(
    PlacementState& state, std::optional<DeviceId> ioHost) {
    if (!ioHost) return;
    for (const auto& [id, value] : state.resolved->values()) {
        if (valueDefinition(value) != ValueDefinitionKind::GraphInput) {
            continue;
        }
        ensureReplica(
            state, id, {*ioHost, std::nullopt},
            ReplicaPurpose::ValueDefinition, std::nullopt,
            std::nullopt, true);
    }
}

/*
 * Buffer ports inherit the concrete memory region selected by the device
 * capability map, including an FPGA HBM bank. Scalars remain device-local
 * but regionless. Resolution failures are diagnosed and left unspecified so
 * the pass can continue collecting independent placement errors.
 */
MemoryPlacement portMemory(
    PlacementState& state, NodeId node,
    const AuthoredKernel& kernel, const ResolvedBinding& binding,
    const ResolvedValue& value) {
    const DeviceId device =
        state.operationPlacements.at(node).device;
    std::optional<MemoryRegionId> region;
    if (value.type.kind == ValueKind::Buffer) {
        try {
            region = state.capabilities->resolveMemoryRegion(
                device, kernel.kernel, binding.localPort.value());
        } catch (const std::runtime_error& error) {
            state.diagnostics.error(
                DiagCode::IncompatibleMemoryPlacement, error.what());
        }
    }
    return {device, std::move(region)};
}

bool definesValue(ValueAccess access) {
    return access == ValueAccess::Output ||
           access == ValueAccess::InoutOutput;
}

/*
 * Port placement is the authoritative source of buffer-bank locality.
 * Defining ports become primary replicas; consuming ports reuse an existing
 * value/location identity when they target the same HBM bank. Non-kernel
 * operations have no device port map and are handled by later fallbacks.
 */
void placeOperationPorts(PlacementState& state) {
    for (const auto& [node, operation] :
         state.resolved->operations()) {
        const AuthoredOperation* authored =
            state.resolved->authored().index().findOperation(node);
        auto device = state.operationPlacements.find(node);
        if (!authored || device == state.operationPlacements.end()) {
            continue;
        }
        const auto* kernel = std::get_if<AuthoredKernel>(authored);
        if (!kernel) continue;

        for (const ResolvedBinding& binding : operation.bindings) {
            const ResolvedValue* value =
                state.resolved->findValue(binding.value);
            if (!value) continue;
            const MemoryPlacement memory =
                portMemory(state, node, *kernel, binding, *value);
            const bool definition = definesValue(binding.access);
            const ReplicaId replica = ensureReplica(
                state, binding.value, memory,
                definition ? ReplicaPurpose::ValueDefinition
                           : ReplicaPurpose::OperationInput,
                node, binding.localPort, definition);
            state.portPlacements.push_back(
                {node, binding.localPort, replica});
        }
    }
}

/*
 * This fallback runs after ports so it cannot replace an exact HBM choice
 * with a regionless device placement. Region parameters and control results
 * are deferred to boundary and control-result placement, respectively.
 */
void placeRemainingDefinitions(PlacementState& state) {
    for (const auto& [id, value] : state.resolved->values()) {
        if (primaryReplica(state, id)) continue;
        const ValueDefinitionKind definition = valueDefinition(value);
        if (definition == ValueDefinitionKind::RegionParameter ||
            definition == ValueDefinitionKind::ControlResult) {
            continue;
        }
        const std::optional<NodeId> producer = valueProducer(value);
        if (!producer) continue;
        auto placement = state.operationPlacements.find(*producer);
        if (placement == state.operationPlacements.end()) continue;
        ensureReplica(
            state, id, {placement->second.device, std::nullopt},
            ReplicaPurpose::ValueDefinition, producer,
            std::nullopt, true);
    }
}

}  // namespace

/*
 * Seed graph inputs on the I/O host, refine kernel ports to device memory,
 * then fill only definitions still lacking a primary. The order preserves
 * exact port regions over coarse producer-device fallbacks.
 */
void placeReplicas(PlacementState& state) {
    const std::optional<DeviceId> ioHost = graphIoHost(state);
    placeGraphInputs(state, ioHost);
    placeOperationPorts(state);
    placeRemainingDefinitions(state);
}

}  // namespace vrt::graph::place_detail
