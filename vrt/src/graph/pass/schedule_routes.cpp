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

#include "schedule_graph_internal.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace vrt::graph::schedule_detail {

namespace {

struct LegScheduleSite {
    RegionId sourceRegion;
    RegionId destinationRegion;
    RegionId actionRegion;
    QueueId  sourceQueue;
    QueueId  destinationQueue;
    QueueId  actionQueue;
};

LegScheduleSite legSite(
    SchedulingState& state, const TransferRoute&,
    const TransferLeg& leg) {
    const DeviceId executor = transferExecutorDevice(
        leg.executor, leg.source, leg.destination);
    return {
        leg.sourceRegion, leg.destinationRegion,
        leg.executorRegion,
        queueFor(state, leg.sourceRegion, leg.source),
        queueFor(
            state, leg.destinationRegion, leg.destination),
        queueFor(state, leg.executorRegion, executor)};
}

LogicalRendezvousPayload dataReady(const TransferRoute& route) {
    return DataReadyRendezvous{
        route.id, route.requirement.signature.phase,
        route.requirement.signature.scope};
}

LogicalRendezvousPayload dataConsumed(
    const TransferRoute& route) {
    return DataConsumedRendezvous{
        route.id, route.requirement.signature.phase,
        route.requirement.signature.scope};
}

/*
 * A route prerequisite names one of two completion points:
 * - ExecutorSignalsReady waits only for the prerequisite's action;
 * - ProducerConsumerAcknowledged waits for terminal consumption.
 *
 * Routes are expected in prerequisite order. A missing milestone means the
 * routed graph violated that contract.
 */
std::optional<std::vector<ScheduleStepId>> prerequisiteSteps(
    SchedulingState& state, const TransferRoute& route) {
    const bool executorCompletion =
        route.requirement.completion ==
        TransferCompletionProtocol::ExecutorSignalsReady;
    const auto& completions =
        executorCompletion ? state.routeActionCompletion
                           : state.routeCompletion;
    std::vector<ScheduleStepId> result;
    for (RouteId id : route.requirement.prerequisites) {
        auto prerequisite = completions.find(id);
        if (prerequisite == completions.end()) {
            state.diagnostics.error(
                DiagCode::InternalInvariant,
                "GraphCompiler: transfer prerequisite was not "
                "scheduled first");
            return std::nullopt;
        }
        result.push_back(prerequisite->second);
    }
    return result;
}

void addPrerequisites(
    SchedulingState& state, ScheduleStepId step,
    const std::vector<ScheduleStepId>& prerequisites) {
    for (ScheduleStepId prerequisite : prerequisites) {
        addDependency(state, step, prerequisite);
    }
}

/*
 * Host events used by control dependencies, or delivered directly to a
 * control operation, complete on the executor queue. Other host events and
 * all device rendezvous complete on the destination queue.
 */
bool completeHostEventOnExecutor(
    const SchedulingState& state, const TransferRoute& route) {
    if (!std::holds_alternative<HostEventSynchronization>(
            route.requirement.synchronization)) {
        return false;
    }
    if (!route.requirement.controlPrerequisites.empty()) {
        return true;
    }
    const std::optional<NodeId> destination =
        route.requirement.destinationAnchor.operation();
    return destination &&
           state.routed->placed().controlPlacements().count(
               *destination) != 0;
}

/*
 * publishAndWait() returns the receiver's wait for a cross-queue delivery.
 * The launch milestone needed by the publishing queue is its publish step,
 * which is the wait's dependency. A local delivery already returns that
 * publisher-side milestone.
 */
ScheduleStepId deliveryPublisher(
    const SchedulingState& state, QueueId publisher,
    QueueId waiter, ScheduleStepId delivered) {
    if (publisher == waiter) return delivered;
    const std::vector<ScheduleStepId>& dependencies =
        state.steps.at(delivered).dependencies;
    return dependencies.empty() ? delivered : dependencies.front();
}

/*
 * Choose the latest route milestone which is safe to reference from a given
 * queue: terminal completion when local, launch completion on the launch
 * queue, and terminal completion everywhere else.
 */
ScheduleStepId localRouteCompletion(
    const SchedulingState& state, RouteId route,
    ScheduleStepId step, ScheduleStepId completion) {
    if (state.steps.at(step).queue ==
        state.steps.at(completion).queue) {
        return completion;
    }
    auto launch = state.routeLaunchCompletion.find(route);
    return launch != state.routeLaunchCompletion.end() &&
                   state.steps.at(step).queue ==
                       state.steps.at(launch->second).queue
               ? launch->second
               : completion;
}

/*
 * ExecutorSignalsReady has no producer/acknowledgement round trip:
 * - run the transfer action after route prerequisites or graph input;
 * - publish data-ready to the destination;
 * - consume there and use that step as terminal completion.
 */
ScheduleStepId scheduleDeliveryOnlyLeg(
    SchedulingState& state, const TransferRoute& route,
    const TransferLeg& leg, const LegScheduleSite& site,
    const std::vector<ScheduleStepId>& prerequisites) {
    const ScheduleStepId action = createStep(
        state, site.actionQueue, site.actionRegion,
        ScheduledTransferAction{route.id, leg.id});
    addPrerequisites(state, action, prerequisites);
    if (prerequisites.empty() && state.graphInput &&
        route.requirement.sourceAnchor.kind() ==
            TransferAnchorKind::GraphInput) {
        addDependency(state, action, *state.graphInput);
    }
    state.routeActionCompletion[route.id] = action;
    const ScheduleStepId delivered = publishAndWait(
        state, site.actionQueue, site.actionRegion, action,
        site.destinationQueue, site.destinationRegion,
        dataReady(route));
    state.routeLaunchCompletion[route.id] = deliveryPublisher(
        state, site.actionQueue, site.destinationQueue,
        delivered);
    const ScheduleStepId consume = createStep(
        state, site.destinationQueue, site.destinationRegion,
        ScheduledTransferConsume{route.id, leg.id});
    addDependency(state, consume, delivered);
    return consume;
}

/*
 * ProducerConsumerAcknowledged uses the full transfer protocol:
 * - source produce publishes data-ready to the executor;
 * - the executor action publishes delivery to the receiving queue;
 * - receive consumes, then acknowledges consumption back to the source.
 *
 * With control-related host events the executor is also the receiving queue;
 * otherwise the destination receives the delivery.
 */
ScheduleStepId scheduleAcknowledgedLeg(
    SchedulingState& state, const TransferRoute& route,
    const TransferLeg& leg, const LegScheduleSite& site,
    const std::vector<ScheduleStepId>& prerequisites) {
    const ScheduleStepId produce = createStep(
        state, site.sourceQueue, site.sourceRegion,
        ScheduledTransferProduce{route.id, leg.id});
    addPrerequisites(state, produce, prerequisites);
    if (prerequisites.empty()) {
        if (auto source = operationStep(
                state,
                route.requirement.sourceAnchor.operation(),
                leg.source)) {
            addDependency(state, produce, *source);
        } else if (
            state.graphInput &&
            route.requirement.sourceAnchor.kind() ==
                TransferAnchorKind::GraphInput) {
            addDependency(state, produce, *state.graphInput);
        }
    }
    const ScheduleStepId ready = publishAndWait(
        state, site.sourceQueue, site.sourceRegion, produce,
        site.actionQueue, site.actionRegion, dataReady(route));
    const ScheduleStepId action = createStep(
        state, site.actionQueue, site.actionRegion,
        ScheduledTransferAction{route.id, leg.id});
    addDependency(state, action, ready);
    state.routeActionCompletion[route.id] = action;
    const bool completeOnExecutor =
        completeHostEventOnExecutor(state, route);
    const QueueId deliveryQueue =
        completeOnExecutor ? site.actionQueue : site.destinationQueue;
    const RegionId deliveryRegion =
        completeOnExecutor ? site.actionRegion
                           : site.destinationRegion;
    const ScheduleStepId delivered = publishAndWait(
        state, site.actionQueue, site.actionRegion, action,
        deliveryQueue, deliveryRegion,
        dataReady(route));
    state.routeLaunchCompletion[route.id] = deliveryPublisher(
        state, site.actionQueue, deliveryQueue, delivered);
    const ScheduleStepId consume = createStep(
        state, deliveryQueue, deliveryRegion,
        ScheduledTransferConsume{route.id, leg.id});
    addDependency(state, consume, delivered);
    (void)publishAndWait(
        state, deliveryQueue, deliveryRegion,
        consume, site.sourceQueue, site.sourceRegion,
        dataConsumed(route));
    return consume;
}

/*
 * Transfer-backed dependency edges have two consumer cases:
 * - controls gate every participant step at a queue-local route milestone;
 * - ordinary operations gate the instance on the destination device at
 *   terminal completion.
 */
void connectRouteConsumers(
    SchedulingState& state, const TransferRoute& route,
    ScheduleStepId completion) {
    const DeviceId& destination =
        route.requirement.signature.destinationLocation.device;
    for (const DependencyEdge& edge :
         state.routed->dependencies()) {
        if (dependencyRoute(edge) !=
            std::optional<RouteId>(route.id)) {
            continue;
        }
        const std::optional<NodeId> consumer =
            dependencyConsumer(edge);
        if (!consumer) continue;
        if (state.routed->placed().controlPlacements().count(
                *consumer) != 0) {
            for (ScheduleStepId step :
                 state.operationStepLists[*consumer]) {
                addDependency(
                    state, step,
                    localRouteCompletion(
                        state, route.id, step, completion));
            }
            continue;
        }
        if (auto step =
                operationStep(state, consumer, destination)) {
            addDependency(state, *step, completion);
        }
    }
}

/*
 * Schedule legs in route order, carrying each leg's completion into the next
 * leg. The route completion protocol selects delivery-only or acknowledged
 * transfer steps; the final leg becomes the route and consumer milestone.
 */
void scheduleRoute(
    SchedulingState& state, const TransferRoute& route) {
    auto prerequisites = prerequisiteSteps(state, route);
    if (!prerequisites) return;
    std::vector<ScheduleStepId> previous =
        std::move(*prerequisites);
    for (const TransferLeg& leg : route.legs) {
        const LegScheduleSite site = legSite(state, route, leg);
        const ScheduleStepId completion =
            route.requirement.completion ==
                    TransferCompletionProtocol::ExecutorSignalsReady
                ? scheduleDeliveryOnlyLeg(
                      state, route, leg, site, previous)
                : scheduleAcknowledgedLeg(
                      state, route, leg, site, previous);
        previous = {completion};
    }
    if (previous.empty()) return;
    state.routeCompletion[route.id] = previous.back();
    connectRouteConsumers(state, route, previous.back());
}

}  // namespace

void scheduleRoutes(SchedulingState& state) {
    /*
     * The router emits prerequisites before their users. scheduleRoute()
     * checks that ordering while turning each route into queue steps.
     */
    for (const TransferRoute& route : state.routed->routes()) {
        scheduleRoute(state, route);
    }
}

void wireRouteControlDependencies(SchedulingState& state) {
    /*
     * A route used to decide control must finish before that control runs.
     * Split controls gate the authority step; ordinary controls gate their
     * placed operation step. Both use a queue-local route milestone where
     * possible to avoid routing a dependency back through its launch queue.
     */
    for (const TransferRoute& route : state.routed->routes()) {
        auto completion = state.routeCompletion.find(route.id);
        if (completion == state.routeCompletion.end()) continue;
        for (NodeId control :
             route.requirement.controlPrerequisites) {
            if (const SplitControlProtocol* protocol =
                    splitControl(state, control)) {
                addDependency(
                    state, protocol->authorityStep,
                    localRouteCompletion(
                        state, route.id,
                        protocol->authorityStep,
                        completion->second));
                continue;
            }
            auto placement = state.routed->placed()
                                 .operationPlacements()
                                 .find(control);
            if (placement == state.routed->placed()
                                 .operationPlacements()
                                 .end()) {
                continue;
            }
            if (auto step = operationStep(
                    state, control, placement->second.device)) {
                addDependency(
                    state, *step,
                    localRouteCompletion(
                        state, route.id, *step,
                        completion->second));
            }
        }
    }
}

}  // namespace vrt::graph::schedule_detail
