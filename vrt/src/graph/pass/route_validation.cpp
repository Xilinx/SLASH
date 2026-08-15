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
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

namespace vrt::graph::route_detail {

namespace {

/*
 * Graph endpoints have no operation region and therefore belong to the root.
 * Missing operation metadata uses the same conservative scope so route
 * planning never invents a nested lifetime.
 */
RegionId operationRegion(
    const PlacedGraph& placed, std::optional<NodeId> operation) {
    if (!operation) return placed.resolved().root().id;
    const ResolvedOperation* resolved =
        placed.resolved().findOperation(*operation);
    return resolved ? resolved->region : placed.resolved().root().id;
}

/*
 * The nearest split ancestor is the controller whose iteration can trigger a
 * transfer. Stopping at the first match prevents an outer split from claiming
 * traffic governed by a more deeply nested one.
 */
std::optional<NodeId> nearestSplitControl(
    const PlacedGraph& placed, RegionId region) {
    std::optional<NodeId> parent =
        placed.resolved().authored().index().parentControl(region);
    while (parent) {
        auto placement = placed.controlPlacements().find(*parent);
        if (placement != placed.controlPlacements().end() &&
            std::holds_alternative<SplitControlPlacement>(
                placement->second)) {
            return parent;
        }
        const ResolvedOperation* control =
            placed.resolved().findOperation(*parent);
        if (!control) break;
        parent = placed.resolved().authored().index().parentControl(
            control->region);
    }
    return std::nullopt;
}

/*
 * Non-iterative transfers live in the outer graph scope enclosing their
 * consumer. Climbing every parent control keeps one-time materialization from
 * being recreated with a nested region's shorter lifetime.
 */
RegionId graphScopeRegion(
    const PlacedGraph& placed, std::optional<NodeId> operation) {
    RegionId region = operationRegion(placed, operation);
    std::optional<NodeId> parent =
        placed.resolved().authored().index().parentControl(region);
    while (parent) {
        const ResolvedOperation* control =
            placed.resolved().findOperation(*parent);
        if (!control) break;
        region = control->region;
        parent = placed.resolved().authored().index().parentControl(
            region);
    }
    return region;
}

/*
 * Autonomous control cannot call back to an external executor for a local
 * memory-region copy. Any autonomous ancestor therefore makes such a copy
 * unsupported, not just the immediate parent.
 */
bool nestedAutonomousCopy(
    const PlacedGraph& placed, std::optional<NodeId> consumer) {
    if (!consumer) return false;
    const ResolvedOperation* operation =
        placed.resolved().findOperation(*consumer);
    if (!operation) return false;
    std::optional<NodeId> parent =
        placed.resolved().authored().index().parentControl(
            operation->region);
    while (parent) {
        auto placement = placed.controlPlacements().find(*parent);
        if (placement != placed.controlPlacements().end() &&
            std::holds_alternative<AutonomousControlPlacement>(
                placement->second)) {
            return true;
        }
        const ResolvedOperation* control =
            placed.resolved().findOperation(*parent);
        if (!control) break;
        parent = placed.resolved().authored().index().parentControl(
            control->region);
    }
    return false;
}

/*
 * Leg ids derive from route id and position so lowering can reproduce them
 * without another allocator. Validation later enforces this stability.
 */
TransferLegId legId(RouteId route, std::size_t index) {
    return TransferLegId(
        (route.value() << 32) |
        static_cast<std::uint64_t>(index));
}

/*
 * Device rendezvous is valid only when exactly one participant owns the
 * namespace shared by every leg endpoint and executor. Zero owners require no
 * device rendezvous; multiple owners would be ambiguous and are left unset.
 */
std::optional<DeviceId> rendezvousOwner(
    const RouteSelection& selection,
    const TransferCapabilityCatalog& capabilities) {
    std::set<DeviceId> owners;
    for (const SelectedTransferLeg& leg : selection.legs) {
        const DeviceId executor = transferExecutorDevice(
            leg.executor, leg.source, leg.destination);
        for (const DeviceId& participant :
             {leg.source, leg.destination, executor}) {
            if (capabilities.ownsRendezvousNamespace(participant)) {
                owners.insert(participant);
            }
        }
    }
    return owners.size() == 1
               ? std::optional<DeviceId>(*owners.begin())
               : std::nullopt;
}

RegionId scopeRegion(const TransferControlScope& scope) {
    return std::visit(
        [](const auto& concrete) { return concrete.region; },
        scope);
}

/*
 * A signature interns one logical transfer regardless of how many consumers
 * discover it. Selection first separates error, no-op, and physical motion.
 * Reuse unions control prerequisites; new routes canonicalize dependencies,
 * choose synchronization/completion policy, then lower ordered physical legs.
 * Prerequisite routes must already exist so ids encode topological order.
 */
std::optional<RouteId> internRoute(
    RoutingState& state, RouteSignature signature,
    TransferControlAnchor sourceAnchor,
    TransferControlAnchor destinationAnchor,
    std::vector<RouteId> prerequisites,
    std::vector<NodeId> controlPrerequisites,
    bool isolatedDestination = false) {
    /*
     * Isolation changes same-location reuse into a physical copy. Selection
     * errors are terminal, while a successful no-op deliberately has no id.
     */
    const bool forceCopy =
        isolatedDestination &&
        signature.sourceLocation.device ==
            signature.destinationLocation.device;
    const RouteSelection selection =
        selectRoute(signature, *state.capabilities, forceCopy);
    if (selection.error) {
        state.diagnostics.error(*selection.error, selection.message);
        return std::nullopt;
    }
    if (!selection.transferRequired) return std::nullopt;

    /*
     * Route identity excludes consumer ancestry. Merge those prerequisites
     * into an existing route so every use waits for its enclosing controls.
     */
    auto existing = state.routeIds.find(signature);
    if (existing != state.routeIds.end()) {
        auto route = std::find_if(
            state.routes.begin(), state.routes.end(),
            [&](const TransferRoute& candidate) {
                return candidate.id == existing->second;
            });
        if (route != state.routes.end()) {
            route->requirement.controlPrerequisites.insert(
                route->requirement.controlPrerequisites.end(),
                controlPrerequisites.begin(),
                controlPrerequisites.end());
            std::sort(
                route->requirement.controlPrerequisites.begin(),
                route->requirement.controlPrerequisites.end());
            route->requirement.controlPrerequisites.erase(
                std::unique(
                    route->requirement.controlPrerequisites.begin(),
                    route->requirement.controlPrerequisites.end()),
                route->requirement.controlPrerequisites.end());
        }
        return existing->second;
    }

    /*
     * Canonical prerequisite order makes construction deterministic. Since
     * all ids were allocated earlier, the vector is also topologically prior.
     */
    std::sort(prerequisites.begin(), prerequisites.end());
    prerequisites.erase(
        std::unique(prerequisites.begin(), prerequisites.end()),
        prerequisites.end());
    TransferRoute route;
    route.id = RouteId(state.nextRoute++);
    route.requirement.signature = std::move(signature);
    route.requirement.sourceAnchor = std::move(sourceAnchor);
    route.requirement.destinationAnchor =
        std::move(destinationAnchor);
    route.requirement.prerequisites = std::move(prerequisites);
    route.requirement.controlPrerequisites =
        std::move(controlPrerequisites);
    route.requirement.isolatedDestination =
        isolatedDestination;

    /*
     * Use device rendezvous only with one namespace owner. A pre-launch local
     * copy chained after another route instead needs the executor to publish
     * readiness, because launch cannot supply an operation completion event.
     */
    if (const std::optional<DeviceId> owner =
            rendezvousOwner(selection, *state.capabilities)) {
        route.requirement.synchronization =
            DeviceRendezvousSynchronization{*owner};
    }
    if (!route.requirement.prerequisites.empty() &&
        route.requirement.signature.phase ==
            TransferPhase::PreLaunch &&
        !selection.legs.empty() &&
        selection.legs.front().mechanism ==
            TransferMechanism::HostMediatedDeviceCopy) {
        route.requirement.completion =
            TransferCompletionProtocol::ExecutorSignalsReady;
    }

    /*
     * Endpoint legs inherit anchor regions; host-bounce intermediates live at
     * root. Non-iterative destinations use the route scope, while iterative
     * legs stay with their controlling child region. Executors follow the
     * endpoint they run on, or root when they are a third participant.
     */
    for (std::size_t i = 0; i < selection.legs.size(); ++i) {
        const SelectedTransferLeg& selected = selection.legs[i];
        const RouteSignature& signature =
            route.requirement.signature;
        const RegionId sourceRegion =
            selected.source == signature.sourceLocation.device
                ? route.requirement.sourceAnchor.region()
                : state.placed->resolved().root().id;
        RegionId destinationRegion =
            selected.destination ==
                    signature.destinationLocation.device
                ? route.requirement.destinationAnchor.region()
                : state.placed->resolved().root().id;
        if (signature.phase != TransferPhase::PerIteration) {
            destinationRegion = scopeRegion(signature.scope);
        } else if (
            const std::optional<NodeId> destination =
                route.requirement.destinationAnchor.operation();
            destination &&
            state.placed->resolved()
                    .authored()
                    .index()
                    .parentControl(sourceRegion) == destination) {
            destinationRegion = sourceRegion;
        }
        const DeviceId executor = transferExecutorDevice(
            selected.executor, selected.source,
            selected.destination);
        const RegionId executorRegion =
            executor == selected.source
                ? sourceRegion
                : (executor == selected.destination
                       ? destinationRegion
                       : state.placed->resolved().root().id);
        route.legs.push_back(
            {legId(route.id, i), selected.mechanism,
             selected.source, selected.destination,
             selected.executor, sourceRegion,
             destinationRegion, executorRegion});
    }
    const RouteId id = route.id;
    state.routeIds.emplace(route.requirement.signature, id);
    state.routes.push_back(std::move(route));
    return id;
}

}  // namespace

/*
 * Transfer replicas are layered over immutable placement replicas. Routing
 * ids therefore resolve locally first, then fall back to the placed graph.
 */
const ValueReplica* RoutingState::findReplica(ReplicaId id) const {
    auto transfer = transferReplicas.find(id);
    return transfer == transferReplicas.end()
               ? placed->findReplica(id)
               : &transfer->second;
}

/*
 * No producer means graph-input motion before launch. A producer and consumer
 * under the same nearest split move per iteration; a control result publishes
 * after that control; all other producer traffic happens once. This order
 * gives the narrowest valid recurring phase precedence over broader phases.
 */
TransferPhase transferPhase(
    const PlacedGraph& placed,
    std::optional<NodeId> producer,
    std::optional<NodeId> consumer) {
    if (!producer) return TransferPhase::PreLaunch;
    const std::optional<NodeId> producerSplit =
        nearestSplitControl(
            placed, operationRegion(placed, producer));
    const std::optional<NodeId> consumerSplit =
        nearestSplitControl(
            placed, operationRegion(placed, consumer));
    if (producerSplit && producerSplit == consumerSplit) {
        return TransferPhase::PerIteration;
    }
    const ResolvedOperation* source =
        placed.resolved().findOperation(*producer);
    if (source &&
        (source->kind == ResolvedOperationKind::Loop ||
         source->kind == ResolvedOperationKind::Conditional)) {
        return TransferPhase::AfterControl;
    }
    return TransferPhase::Once;
}

/*
 * Per-iteration routes belong to their nearest split controller, preferring
 * producer ancestry when both endpoints are structural. After-control routes
 * belong to the producing control. Pre-launch and once-only routes use the
 * consumer's outer graph scope so their materialization outlives nested work.
 */
TransferControlScope transferScope(
    const PlacedGraph& placed, TransferPhase phase,
    std::optional<NodeId> producer,
    std::optional<NodeId> consumer) {
    if (phase == TransferPhase::PerIteration) {
        std::optional<NodeId> control = nearestSplitControl(
            placed, operationRegion(placed, producer));
        if (!control) {
            control = nearestSplitControl(
                placed, operationRegion(placed, consumer));
        }
        if (control) {
            return ControlTransferScope{
                *control, operationRegion(placed, *control)};
        }
    } else if (phase == TransferPhase::AfterControl && producer) {
        return ControlTransferScope{
            *producer, operationRegion(placed, producer)};
    }
    return GraphTransferScope{
        graphScopeRegion(placed, consumer)};
}

TransferControlAnchor sourceAnchor(
    const PlacedGraph& placed,
    std::optional<NodeId> producer) {
    return producer
               ? TransferControlAnchor::operation(
                     *producer, operationRegion(placed, producer))
               : TransferControlAnchor::graphInput(
                     placed.resolved().root().id);
}

TransferControlAnchor destinationAnchor(
    const PlacedGraph& placed, TransferPhase,
    std::optional<NodeId> consumer) {
    if (!consumer) {
        return TransferControlAnchor::graphOutput(
            placed.resolved().root().id);
    }
    return TransferControlAnchor::operation(
        *consumer, operationRegion(placed, consumer));
}

/*
 * Per-iteration routes are already gated by their owning split, and graph
 * outputs have no consumer ancestry. Other routes wait for every enclosing
 * non-autonomous control; autonomous ancestors execute locally and need no
 * externally scheduled prerequisite.
 */
std::vector<NodeId> controlPrerequisites(
    const PlacedGraph& placed, TransferPhase phase,
    std::optional<NodeId> consumer) {
    if (!consumer || phase == TransferPhase::PerIteration) {
        return {};
    }
    std::vector<NodeId> controls;
    std::optional<NodeId> parent =
        placed.resolved().authored().index().parentControl(
            operationRegion(placed, consumer));
    while (parent) {
        auto placement =
            placed.controlPlacements().find(*parent);
        if (placement == placed.controlPlacements().end() ||
            !std::holds_alternative<
                AutonomousControlPlacement>(
                placement->second)) {
            controls.push_back(*parent);
        }
        const ResolvedOperation* operation =
            placed.resolved().findOperation(*parent);
        if (!operation) break;
        parent = placed.resolved().authored().index().parentControl(
            operation->region);
    }
    return controls;
}

/*
 * Data routing requires two known replicas and rejects local HBM copies under
 * autonomous control. Otherwise replica locations, producer/consumer phase,
 * scope, and anchors form the complete interned signature. Isolation is kept
 * outside the signature only to force physical copy selection for this target.
 */
std::optional<RouteId> planDataRoute(
    RoutingState& state, ReplicaId source, ReplicaId destination,
    TransferPayloadKind payload,
    std::optional<NodeId> producer,
    std::optional<NodeId> consumer,
    std::vector<RouteId> prerequisites,
    bool isolatedDestination) {
    /*
     * Validate storage endpoints before deriving control metadata; unknown
     * replicas cannot safely contribute a device, region, or scope.
     */
    const ValueReplica* sourceReplica = state.findReplica(source);
    const ValueReplica* targetReplica = state.findReplica(destination);
    if (!sourceReplica || !targetReplica) {
        state.diagnostics.error(
            DiagCode::InternalInvariant,
            "GraphCompiler: transfer references an unknown replica");
        return std::nullopt;
    }
    if (sourceReplica->memory.device ==
            targetReplica->memory.device &&
        sourceReplica->memory.region &&
        targetReplica->memory.region &&
        sourceReplica->memory.region !=
            targetReplica->memory.region &&
        nestedAutonomousCopy(*state.placed, consumer)) {
        state.diagnostics.error(
            DiagCode::UnsupportedNestedCopy,
            "GraphCompiler: same-device memory-region copy "
            "inside autonomous control is unsupported");
        return std::nullopt;
    }

    /*
     * Phase and scope are semantic route identity, not scheduling decoration:
     * equal endpoints in different control lifetimes must not be interned.
     */
    const TransferPhase phase =
        transferPhase(*state.placed, producer, consumer);
    RouteSignature signature;
    signature.payload = payload;
    signature.source = ReplicaTransferEndpoint{source};
    signature.destination =
        ReplicaTransferEndpoint{destination};
    signature.sourceLocation = {
        sourceReplica->memory.device, sourceReplica->memory.region};
    signature.destinationLocation = {
        targetReplica->memory.device, targetReplica->memory.region};
    signature.phase = phase;
    signature.scope =
        transferScope(*state.placed, phase, producer, consumer);
    return internRoute(
        state, std::move(signature),
        sourceAnchor(*state.placed, producer),
        destinationAnchor(*state.placed, phase, consumer),
        std::move(prerequisites),
        controlPrerequisites(*state.placed, phase, consumer),
        isolatedDestination);
}

/*
 * Ordering-only dependencies use device endpoints rather than replicas, but
 * share the same phase, scope, anchors, route selection, and interning rules
 * as data motion.
 */
std::optional<RouteId> planBarrierRoute(
    RoutingState& state, NodeId producer, NodeId consumer,
    const MemoryPlacement& source,
    const MemoryPlacement& destination) {
    const TransferPhase phase = transferPhase(
        *state.placed, producer, consumer);
    RouteSignature signature;
    signature.payload = TransferPayloadKind::Barrier;
    signature.source =
        BarrierTransferEndpoint{source.device, producer};
    signature.destination =
        BarrierTransferEndpoint{destination.device, consumer};
    signature.sourceLocation = {source.device, source.region};
    signature.destinationLocation = {
        destination.device, destination.region};
    signature.phase = phase;
    signature.scope = transferScope(
        *state.placed, phase, producer, consumer);
    return internRoute(
        state, std::move(signature),
        sourceAnchor(*state.placed, producer),
        destinationAnchor(*state.placed, phase, consumer), {},
        controlPrerequisites(*state.placed, phase, consumer));
}

/*
 * Validate the closed route graph in layers: endpoint kind and replica
 * existence, physical leg coverage and continuity, then control lifetime and
 * stable ids. Prerequisite ids must be smaller, proving routes were created
 * in executable topological order rather than relying on a later sort.
 */
void validateRoutes(RoutingState& state) {
    for (const TransferRoute& route : state.routes) {
        const RouteSignature& signature =
            route.requirement.signature;

        /*
         * Barrier endpoints carry devices; data endpoints carry replicas.
         * Payload kind must agree before either form can be lowered safely.
         */
        const bool data =
            signature.payload != TransferPayloadKind::Barrier;
        if (data != transferReplica(signature.source).has_value() ||
            data !=
                transferReplica(signature.destination).has_value()) {
            state.diagnostics.error(
                DiagCode::InternalInvariant,
                "GraphCompiler: route endpoint kind does not match "
                "its payload");
        }
        if (data &&
            (!state.findReplica(
                 *transferReplica(signature.source)) ||
             !state.findReplica(
                 *transferReplica(signature.destination)))) {
            state.diagnostics.error(
                DiagCode::InternalInvariant,
                "GraphCompiler: data route has an unknown replica");
        }

        /*
         * Legs must cover both logical endpoints in one continuous chain.
         * An interned transfer with no physical leg is never a valid no-op.
         */
        if (route.legs.empty()) {
            state.diagnostics.error(
                DiagCode::InternalInvariant,
                "GraphCompiler: transfer route has no legs");
        } else {
            if (route.legs.front().source !=
                    signature.sourceLocation.device ||
                route.legs.back().destination !=
                    signature.destinationLocation.device) {
                state.diagnostics.error(
                    DiagCode::InternalInvariant,
                    "GraphCompiler: transfer legs do not span the "
                    "route endpoints");
            }
            for (std::size_t i = 1; i < route.legs.size(); ++i) {
                if (route.legs[i - 1].destination !=
                    route.legs[i].source) {
                    state.diagnostics.error(
                        DiagCode::InternalInvariant,
                        "GraphCompiler: transfer legs are not "
                        "continuous");
                }
            }
        }

        /*
         * Recurring and post-control routes require control scope; one-time
         * routes require graph scope. Stable leg ids and earlier prerequisites
         * complete the scheduling invariants.
         */
        const bool controlScoped =
            signature.phase == TransferPhase::PerIteration ||
            signature.phase == TransferPhase::AfterControl;
        if (controlScoped !=
            std::holds_alternative<ControlTransferScope>(
                signature.scope)) {
            state.diagnostics.error(
                DiagCode::InternalInvariant,
                "GraphCompiler: transfer phase and control scope "
                "disagree");
        }
        for (std::size_t i = 0; i < route.legs.size(); ++i) {
            if (route.legs[i].id != legId(route.id, i)) {
                state.diagnostics.error(
                    DiagCode::InternalInvariant,
                    "GraphCompiler: transfer leg id is unstable");
            }
        }
        for (RouteId prerequisite :
             route.requirement.prerequisites) {
            if (prerequisite >= route.id) {
                state.diagnostics.error(
                    DiagCode::InternalInvariant,
                    "GraphCompiler: transfer prerequisite is not "
                    "ordered before its route");
            }
        }
    }
}

}  // namespace vrt::graph::route_detail
