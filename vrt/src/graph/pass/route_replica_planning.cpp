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

#include "route_graph_internal.hpp"

#include <algorithm>
#include <utility>

namespace vrt::graph::route_detail {

namespace {

const TransferRoute* findRoute(
    const RoutingState& state, RouteId id) {
    auto route = std::find_if(
        state.routes.begin(), state.routes.end(),
        [&](const TransferRoute& candidate) {
            return candidate.id == id;
        });
    return route == state.routes.end() ? nullptr : &*route;
}

ReplicaId routeSource(
    const RoutingState& state, RouteId route,
    ReplicaId fallback) {
    const TransferRoute* planned = findRoute(state, route);
    if (!planned) return fallback;
    return transferReplica(
               planned->requirement.signature.source)
        .value_or(fallback);
}

/*
 * A consumer-facing boundary target is not the transfer ingress identity.
 * Route into its paired source replica at the same memory, then retain the
 * target on the dependency edge so region crossing remains explicit.
 */
ReplicaId boundarySourceFor(
    const RoutingState& state, ReplicaId target) {
    for (const BoundaryMappingPlacement& mapping :
         state.placed->boundaryMappings()) {
        if (mapping.target == target) return mapping.source;
    }
    return target;
}

/*
 * Graph-input buffers may fan out through one device materialization. Prefer
 * the exact requested replica; otherwise a replica on the destination device
 * can seed a later local HBM-region copy, ordered after its creating route.
 */
std::optional<std::pair<ReplicaId, RouteId>>
materializedReplica(
    const RoutingState& state, ValueId value,
    const DeviceId& device, ReplicaId preferred) {
    // ponytail: route counts are small; add a destination index if this
    // scan ever becomes measurable.
    std::optional<std::pair<ReplicaId, RouteId>> available;
    for (const TransferRoute& route : state.routes) {
        const std::optional<ReplicaId> destination =
            transferReplica(
                route.requirement.signature.destination);
        if (!destination) continue;
        if (*destination == preferred) {
            return std::make_pair(*destination, route.id);
        }
        const ReplicaId replicaId = *destination;
        const ValueReplica* replica = state.findReplica(replicaId);
        if (replica && replica->value == value &&
            replica->memory.device == device && !available) {
            available = std::make_pair(replicaId, route.id);
        }
    }
    return available;
}

TransferPayloadKind payloadFor(const ResolvedValue& value) {
    return value.type.kind == ValueKind::Buffer
               ? TransferPayloadKind::Buffer
               : TransferPayloadKind::Scalar;
}

/*
 * Mutable consumers require a private identity even when another replica has
 * the same value and memory. Deliberately bypass the location index so route
 * interning cannot merge copy-on-write destinations.
 */
ReplicaId createTransferReplica(
    RoutingState& state, ValueId value,
    const MemoryPlacement& memory, NodeId operation) {
    ValueReplica replica;
    replica.id = ReplicaId(state.nextReplica++);
    replica.value = value;
    replica.memory = memory;
    replica.purpose = ReplicaPurpose::TransferTarget;
    replica.operation = operation;
    const ReplicaId id = replica.id;
    state.transferReplicas.emplace(id, std::move(replica));
    return id;
}

/*
 * Same-location values still need a dependency even when no transfer route
 * exists. Recording producer/consumer pairs here also lets the later ordering
 * pass suppress barriers already implied by data flow.
 */
void addValueDependency(
    RoutingState& state, std::optional<NodeId> producer,
    std::optional<NodeId> consumer, ReplicaId source,
    ReplicaId target, std::optional<RouteId> route) {
    state.dependencies.push_back(
        ValueDependencyEdge{
            producer, consumer, source, target, route});
    if (producer && consumer) {
        state.dataDependencies.insert({*producer, *consumer});
    }
}

/*
 * Resolve each use from the canonical primary replica. Shared destinations
 * are interned, while non-host mutable inputs get private storage and may
 * force a same-device copy. Root scalars need dependency only; graph-input
 * buffers may reuse one device transfer and add a local HBM copy. Boundary
 * targets route through their paired source identity.
 */
void planValueUse(RoutingState& state, const ValueUse& use) {
    /*
     * Alias canonicalization must happen before source lookup; dependencies
     * may retain the authored use identity, but storage ownership may not.
     */
    const ValueId canonical =
        state.placed->resolved().aliases().canonical(use.value);
    const ResolvedValue* value =
        state.placed->resolved().findValue(canonical);
    const ValueReplica* primary =
        state.placed->primaryReplica(canonical);
    if (!value || !primary) {
        state.diagnostics.error(
            DiagCode::InternalInvariant,
            "GraphCompiler: value use has no source replica");
        return;
    }

    /*
     * Cross-device motion already creates an independent copy. On one device,
     * mutable inputs need copy support unless host storage owns the mutation.
     */
    const bool sameDevice =
        primary->memory.device == use.targetMemory.device;
    const bool hostOwnsCopyOnWrite =
        use.copyOnWrite && sameDevice &&
        state.capabilities->host() &&
        primary->memory.device == *state.capabilities->host();
    if (use.copyOnWrite && sameDevice &&
        !hostOwnsCopyOnWrite &&
        !state.capabilities->supportsMemoryRegionCopies(
            primary->memory.device)) {
        state.diagnostics.error(
            DiagCode::IncompatibleMemoryPlacement,
            "GraphCompiler: inout consumer on device '" +
                primary->memory.device.value() +
                "' requires an independent copy, but device-local "
                "copy-on-write is unsupported");
        return;
    }
    const bool isolateMutableInput =
        use.copyOnWrite && !hostOwnsCopyOnWrite;

    /*
     * Isolated targets are never interned. A boundary-facing target keeps its
     * consumer identity, while routing terminates at the paired ingress replica.
     */
    const ReplicaId consumerTarget =
        isolateMutableInput
            ? createTransferReplica(
                  state, use.value, use.targetMemory, use.consumer)
            : use.target.value_or(ensureTransferReplica(
                  state, use.value, use.targetMemory,
                  use.consumer));
    const ReplicaId routeTarget =
        boundarySourceFor(state, consumerTarget);
    ReplicaId source = primary->id;
    std::vector<RouteId> prerequisites;
    std::optional<RouteId> route;
    const bool sharedRootScalar =
        value->type.kind == ValueKind::Scalar &&
        valueDefinition(*value) ==
            ValueDefinitionKind::GraphInput;

    /*
     * Root scalars are launch metadata shared without a data route. Other
     * values must either reuse an existing materialization or plan new motion.
     */
    if (!sharedRootScalar) {
        const ValueReplica* destination =
            state.findReplica(routeTarget);
        const bool graphInputFanout =
            destination &&
            value->type.kind == ValueKind::Buffer &&
            valueDefinition(*value) ==
                ValueDefinitionKind::GraphInput &&
            primary->memory.device !=
                destination->memory.device;

        /*
         * Reuse an exact fanout route outright. A different replica on the
         * same device instead becomes the source of an ordered local copy.
         */
        if (graphInputFanout && !isolateMutableInput) {
            auto available = materializedReplica(
                state, canonical, destination->memory.device,
                routeTarget);
            if (available && available->first == routeTarget) {
                addValueDependency(
                    state, valueProducer(*value), use.consumer,
                    routeSource(
                        state, available->second, source),
                    consumerTarget, available->second);
                return;
            }
            if (available) {
                source = available->first;
                prerequisites.push_back(available->second);
            }
        }
        route = planDataRoute(
            state, source, routeTarget, payloadFor(*value),
            valueProducer(*value), use.consumer,
            std::move(prerequisites),
            isolateMutableInput);
    }
    addValueDependency(
        state, valueProducer(*value), use.consumer, source,
        consumerTarget, route);
}

}  // namespace

/*
 * Ordinary transfer targets are shared by value and memory. Callers that
 * require copy-on-write isolation create an unindexed replica instead.
 */
ReplicaId ensureTransferReplica(
    RoutingState& state, ValueId value,
    const MemoryPlacement& memory,
    std::optional<NodeId> operation) {
    const auto key = std::make_pair(value, memory);
    auto existing = state.replicasByLocation.find(key);
    if (existing != state.replicasByLocation.end()) {
        return existing->second;
    }
    ValueReplica replica;
    replica.id = ReplicaId(state.nextReplica++);
    replica.value = value;
    replica.memory = memory;
    replica.purpose = ReplicaPurpose::TransferTarget;
    replica.operation = operation;
    const ReplicaId id = replica.id;
    state.transferReplicas.emplace(id, std::move(replica));
    state.replicasByLocation.emplace(key, id);
    return id;
}

void planValueRoutes(
    RoutingState& state, const std::vector<ValueUse>& uses) {
    for (const ValueUse& use : uses) {
        planValueUse(state, use);
    }
}

/*
 * Each control arm publishes from its own primary replica into the single
 * placed result replica. The dependency remains even for same-memory arms,
 * while differing devices or HBM regions acquire a transfer route.
 */
void planControlPublicationRoutes(RoutingState& state) {
    for (const ResolvedControlResult& result :
         state.placed->resolved().controlResults()) {
        const ResolvedValue* destinationValue =
            state.placed->resolved().findValue(result.result);
        const ValueReplica* destination =
            state.placed->primaryReplica(result.result);
        if (!destinationValue || !destination) continue;
        for (const ControlIncoming& incoming : result.incoming) {
            const ValueId sourceId =
                state.placed->resolved().aliases().canonical(
                    incoming.value);
            const ResolvedValue* sourceValue =
                state.placed->resolved().findValue(sourceId);
            const ValueReplica* source =
                state.placed->primaryReplica(sourceId);
            if (!sourceValue || !source) continue;
            const std::optional<NodeId> producer =
                valueProducer(*sourceValue);
            const std::optional<RouteId> route = planDataRoute(
                state, source->id, destination->id,
                payloadFor(*destinationValue), producer,
                result.control);
            addValueDependency(
                state, producer, result.control, source->id,
                destination->id, route);
        }
    }
}

/*
 * Graph outputs are materialized in the declared host ownership domain.
 * A host-resident primary needs only a dependency; every other primary is
 * routed to one regionless host transfer target.
 */
void planGraphOutputRoutes(RoutingState& state) {
    if (!state.capabilities->host()) return;
    const MemoryPlacement host{
        *state.capabilities->host(), std::nullopt};
    for (const auto& [id, value] :
         state.placed->resolved().values()) {
        if (!value.graphOutput) continue;
        const ValueReplica* source =
            state.placed->primaryReplica(id);
        if (!source) continue;
        const ReplicaId target = ensureTransferReplica(
            state, id, host, std::nullopt);
        const std::optional<NodeId> producer = valueProducer(value);
        const std::optional<RouteId> route = planDataRoute(
            state, source->id, target, payloadFor(value), producer,
            std::nullopt);
        addValueDependency(
            state, producer, std::nullopt, source->id, target,
            route);
    }
}

}  // namespace vrt::graph::route_detail
