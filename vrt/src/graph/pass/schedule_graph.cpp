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

#include <vrt/graph/ir/scheduled_graph.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace vrt::graph {

namespace {

class GraphScheduler {
   public:
    CompileResult<ScheduledGraph> schedule(
        const RoutedGraph& routed) {
        routed_ = std::make_shared<RoutedGraph>(routed);
        rootRegion_ = routed_->placed().resolved().root().id;
        indexRegions(
            routed_->placed().resolved().authored().root(),
            std::nullopt);
        createOperationSteps();
        createTransferSteps();
        wireRoutedConsumers();
        wireUnroutedDependencies();
        createSplitControlRendezvous();
        validateAndOrder();

        if (diagnostics_.hasErrors()) {
            return CompileResult<ScheduledGraph>::failure(
                std::move(diagnostics_));
        }
        return CompileResult<ScheduledGraph>::success(
            ScheduledGraph(
                routed_, std::move(queues_),
                std::move(steps_), std::move(rendezvous_),
                std::move(resources_)),
            std::move(diagnostics_));
    }

   private:
    using QueueKey = std::pair<RegionId, DeviceId>;
    using OperationQueueKey = std::pair<NodeId, DeviceId>;

    void indexRegions(const AuthoredRegion& region,
                      std::optional<NodeId> parentControl) {
        parentControlByRegion_[region.id] = parentControl;
        for (const AuthoredOperation& operation : region.operations) {
            authoredOperations_[authoredNodeId(operation)] = &operation;
            if (const auto* loop =
                    std::get_if<AuthoredLoop>(&operation)) {
                if (loop->body) {
                    indexRegions(*loop->body, loop->id);
                }
            } else if (const auto* conditional =
                           std::get_if<AuthoredConditional>(&operation)) {
                if (conditional->thenRegion) {
                    indexRegions(*conditional->thenRegion,
                                 conditional->id);
                }
                if (conditional->elseRegion) {
                    indexRegions(*conditional->elseRegion,
                                 conditional->id);
                }
            }
        }
    }

    QueueId queueFor(RegionId region, DeviceId device) {
        const QueueKey key{region, device};
        auto existing = queueIds_.find(key);
        if (existing != queueIds_.end()) return existing->second;

        QueueProgram queue;
        queue.id = QueueId(queues_.size());
        queue.device = std::move(device);
        queue.region = region;
        auto parent = parentControlByRegion_.find(region);
        if (parent != parentControlByRegion_.end()) {
            queue.parentControl = parent->second;
        }
        queueIds_[key] = queue.id;
        queueIndexes_[queue.id] = queues_.size();
        queues_.push_back(std::move(queue));
        return queues_.back().id;
    }

    const QueueProgram* findQueue(QueueId id) const {
        auto index = queueIndexes_.find(id);
        return index == queueIndexes_.end()
                   ? nullptr
                   : &queues_[index->second];
    }

    QueueProgram& mutableQueue(QueueId id) {
        return queues_.at(queueIndexes_.at(id));
    }

    ScheduleStepId createStep(ScheduledStepKind kind,
                              QueueId queue,
                              RegionId region) {
        ScheduledStep step;
        step.id = ScheduleStepId(nextStep_++);
        step.kind = kind;
        step.queue = queue;
        step.region = region;
        const ScheduleStepId id = step.id;
        steps_.emplace(id, std::move(step));
        mutableQueue(queue).steps.push_back(id);
        return id;
    }

    void addDependency(ScheduleStepId step,
                       ScheduleStepId dependency) {
        if (step == dependency) {
            return;
        }
        steps_.at(step).dependencies.push_back(dependency);
    }

    static ControlReplicaRole replicaRole(
        const ControlPlacement& placement, DeviceId device) {
        if (placement.authority &&
            *placement.authority == device) {
            return ControlReplicaRole::Authority;
        }
        if (std::find(placement.followers.begin(),
                      placement.followers.end(),
                      device) != placement.followers.end()) {
            return ControlReplicaRole::Follower;
        }
        return ControlReplicaRole::None;
    }

    void createOperationSteps() {
        const PlacedGraph& placed = routed_->placed();
        for (const auto& [node, operation] :
             placed.resolved().operations()) {
            if (operation.structural) continue;
            auto control =
                placed.controlPlacements().find(node);
            if (control != placed.controlPlacements().end() &&
                control->second.mode ==
                    ControlMode::SplitAcrossDevices) {
                for (DeviceId device :
                     control->second.participants) {
                    const QueueId queue =
                        queueFor(operation.region, device);
                    const ScheduleStepId step =
                        createStep(ScheduledStepKind::Operation,
                                   queue, operation.region);
                    ScheduledStep& scheduled = steps_.at(step);
                    scheduled.operation = node;
                    scheduled.controlRole =
                        replicaRole(control->second, device);
                    operationSteps_[{node, device}] = step;
                    operationStepLists_[node].push_back(step);
                }
                continue;
            }

            auto placement =
                placed.operationPlacements().find(node);
            if (placement ==
                placed.operationPlacements().end()) {
                diagnostics_.error(
                    DiagCode::InternalInvariant,
                    "GraphCompiler: operation has no queue placement");
                continue;
            }
            const QueueId queue =
                queueFor(operation.region,
                         placement->second.device);
            const ScheduleStepId step =
                createStep(ScheduledStepKind::Operation,
                           queue, operation.region);
            steps_.at(step).operation = node;
            operationSteps_[{node, placement->second.device}] =
                step;
            operationStepLists_[node].push_back(step);
        }
    }

    RegionId operationRegion(
        std::optional<NodeId> operation) const {
        if (!operation) return rootRegion_;
        const ResolvedOperation* resolved =
            routed_->placed().resolved().findOperation(*operation);
        return resolved ? resolved->region : rootRegion_;
    }

    std::optional<NodeId> nearestSplitControl(
        RegionId region) const {
        auto parent = parentControlByRegion_.find(region);
        while (parent != parentControlByRegion_.end() &&
               parent->second) {
            auto placement =
                routed_->placed().controlPlacements().find(
                    *parent->second);
            if (placement !=
                    routed_->placed().controlPlacements().end() &&
                placement->second.mode ==
                    ControlMode::SplitAcrossDevices) {
                return parent->second;
            }
            const ResolvedOperation* control =
                routed_->placed().resolved().findOperation(
                    *parent->second);
            if (!control) break;
            parent =
                parentControlByRegion_.find(control->region);
        }
        return std::nullopt;
    }

    RegionId outerControlRegion(
        std::optional<NodeId> consumer) const {
        if (!consumer) return rootRegion_;
        const ResolvedOperation* operation =
            routed_->placed().resolved().findOperation(*consumer);
        if (!operation) return rootRegion_;
        RegionId region = operation->region;
        auto parent = parentControlByRegion_.find(region);
        while (parent != parentControlByRegion_.end() &&
               parent->second) {
            const ResolvedOperation* control =
                routed_->placed().resolved().findOperation(
                    *parent->second);
            if (!control) break;
            region = control->region;
            parent = parentControlByRegion_.find(region);
        }
        return region;
    }

    RendezvousScope routeScope(
        const TransferRoute& route) const {
        if (!route.requirement.source.operation) {
            return RendezvousScope::PreLaunch;
        }
        const RegionId source =
            operationRegion(route.requirement.source.operation);
        const RegionId destination =
            operationRegion(
                route.requirement.destination.operation);
        if (nearestSplitControl(source) ||
            nearestSplitControl(destination)) {
            return RendezvousScope::PerIteration;
        }
        return RendezvousScope::Once;
    }

    std::optional<RendezvousId> createRendezvous(
        RendezvousPurpose purpose, RendezvousScope scope,
        QueueId publisher, QueueId waiter,
        std::optional<RouteId> route,
        std::optional<NodeId> control = std::nullopt) {
        if (publisher == waiter) return std::nullopt;
        LogicalRendezvous value;
        value.id = RendezvousId(nextRendezvous_++);
        value.purpose = purpose;
        value.scope = scope;
        value.publisher = publisher;
        value.waiter = waiter;
        value.route = route;
        value.control = control;
        rendezvous_.push_back(value);

        std::set<DeviceId> participantSet;
        if (const QueueProgram* queue = findQueue(publisher)) {
            participantSet.insert(queue->device);
        }
        if (const QueueProgram* queue = findQueue(waiter)) {
            participantSet.insert(queue->device);
        }
        LogicalResourceRequirement requirement;
        requirement.rendezvous = value.id;
        requirement.participants.assign(
            participantSet.begin(), participantSet.end());
        resources_.push_back(std::move(requirement));
        return value.id;
    }

    ScheduleStepId publishAndWait(
        RendezvousPurpose purpose, RendezvousScope scope,
        QueueId publisherQueue, RegionId publisherRegion,
        ScheduleStepId publisherDependency,
        QueueId waiterQueue, RegionId waiterRegion,
        std::optional<RouteId> route,
        bool preLaunch) {
        if (publisherQueue == waiterQueue) {
            return publisherDependency;
        }
        const std::optional<RendezvousId> rendezvous =
            createRendezvous(
                purpose, scope, publisherQueue, waiterQueue,
                route);
        const ScheduleStepId publish =
            createStep(ScheduledStepKind::EventPublish,
                       publisherQueue, publisherRegion);
        steps_.at(publish).rendezvous = rendezvous;
        steps_.at(publish).preLaunch = preLaunch;
        addDependency(publish, publisherDependency);

        const ScheduleStepId wait =
            createStep(ScheduledStepKind::EventWait,
                       waiterQueue, waiterRegion);
        steps_.at(wait).rendezvous = rendezvous;
        steps_.at(wait).preLaunch = preLaunch;
        addDependency(wait, publish);
        return wait;
    }

    std::optional<ScheduleStepId> operationStep(
        std::optional<NodeId> operation,
        DeviceId device) const {
        if (!operation) return std::nullopt;
        auto exact =
            operationSteps_.find({*operation, device});
        if (exact != operationSteps_.end()) {
            return exact->second;
        }
        auto all = operationStepLists_.find(*operation);
        if (all == operationStepLists_.end() ||
            all->second.empty()) {
            return std::nullopt;
        }
        return all->second.front();
    }

    void gateAncestorControls(NodeId consumer,
                              ScheduleStepId ready) {
        const ResolvedOperation* operation =
            routed_->placed().resolved().findOperation(consumer);
        if (!operation) return;
        auto parent =
            parentControlByRegion_.find(operation->region);
        while (parent != parentControlByRegion_.end() &&
               parent->second) {
            auto steps = operationStepLists_.find(*parent->second);
            if (steps != operationStepLists_.end()) {
                const auto placement =
                    routed_->placed().controlPlacements().find(
                        *parent->second);
                for (ScheduleStepId controlStep :
                     steps->second) {
                    if (placement !=
                            routed_->placed().controlPlacements().end()) {
                        if (placement->second.mode ==
                            ControlMode::AutonomousOnDevice) {
                            continue;
                        }
                        if (placement->second.mode ==
                                ControlMode::SplitAcrossDevices &&
                            placement->second.authority) {
                            const QueueProgram* queue =
                                findQueue(steps_.at(controlStep).queue);
                            if (!queue ||
                                queue->device !=
                                    *placement->second.authority) {
                                continue;
                            }
                        }
                    }
                    addDependency(controlStep, ready);
                }
            }
            const ResolvedOperation* control =
                routed_->placed().resolved().findOperation(
                    *parent->second);
            if (!control) break;
            parent =
                parentControlByRegion_.find(control->region);
        }
    }

    void createTransferSteps() {
        for (const TransferRoute& route :
             routed_->routes()) {
            const RendezvousScope scope = routeScope(route);
            routeScopes_[route.requirement.id] = scope;
            const bool preLaunch =
                scope == RendezvousScope::PreLaunch;
            const bool hostPreLaunchCopy =
                preLaunch && route.requirement.prerequisite &&
                !route.legs.empty() &&
                route.legs.front().mechanism ==
                    TransferMechanism::HostMediatedDeviceCopy;
            std::optional<ScheduleStepId> previous;
            if (route.requirement.prerequisite) {
                const auto& completions =
                    hostPreLaunchCopy ? routeActionCompletion_
                                      : routeCompletion_;
                auto prerequisite = completions.find(
                    *route.requirement.prerequisite);
                if (prerequisite == completions.end()) {
                    diagnostics_.error(
                        DiagCode::InternalInvariant,
                        "GraphCompiler: transfer prerequisite was not "
                        "scheduled first");
                    continue;
                }
                previous = prerequisite->second;
            }

            for (const TransferLeg& leg : route.legs) {
                const RegionId sourceRegion =
                    leg.source ==
                            route.requirement.source.device
                        ? operationRegion(
                              route.requirement.source.operation)
                        : rootRegion_;
                const RegionId destinationRegion =
                    scope != RendezvousScope::PerIteration
                        ? outerControlRegion(
                              route.requirement.destination.operation)
                        :
                    leg.destination ==
                            route.requirement.destination.device
                        ? operationRegion(
                              route.requirement.destination.operation)
                        : rootRegion_;
                const QueueId sourceQueue =
                    queueFor(sourceRegion, leg.source);
                const QueueId destinationQueue =
                    queueFor(destinationRegion,
                             leg.destination);
                const RegionId actionRegion =
                    !leg.executor
                        ? destinationRegion
                        : (*leg.executor == leg.source
                               ? sourceRegion
                               : (*leg.executor == leg.destination
                                      ? destinationRegion
                                      : rootRegion_));
                const QueueId actionQueue =
                    leg.executor
                        ? queueFor(actionRegion, *leg.executor)
                        : destinationQueue;

                if (hostPreLaunchCopy) {
                    const ScheduleStepId action =
                        createStep(
                            ScheduledStepKind::TransferAction,
                            actionQueue, actionRegion);
                    ScheduledStep& actionStep = steps_.at(action);
                    actionStep.route = route.requirement.id;
                    actionStep.preLaunch = true;
                    if (previous) addDependency(action, *previous);
                    routeActionCompletion_[route.requirement.id] = action;

                    ScheduleStepId delivered = action;
                    if (actionQueue != destinationQueue) {
                        delivered = publishAndWait(
                            RendezvousPurpose::DataReady,
                            RendezvousScope::PreLaunch,
                            actionQueue, actionRegion, action,
                            destinationQueue, destinationRegion,
                            route.requirement.id, true);
                    }
                    const ScheduleStepId consume =
                        createStep(
                            ScheduledStepKind::TransferConsume,
                            destinationQueue, destinationRegion);
                    ScheduledStep& consumeStep = steps_.at(consume);
                    consumeStep.route = route.requirement.id;
                    consumeStep.preLaunch = true;
                    addDependency(consume, delivered);
                    previous = consume;
                    continue;
                }

                const ScheduleStepId produce =
                    createStep(
                        ScheduledStepKind::TransferProduce,
                        sourceQueue, sourceRegion);
                ScheduledStep& produceStep = steps_.at(produce);
                produceStep.route = route.requirement.id;
                produceStep.preLaunch = preLaunch;
                if (previous) {
                    addDependency(produce, *previous);
                } else if (auto source =
                               operationStep(
                                   route.requirement.source.operation,
                                   leg.source)) {
                    addDependency(produce, *source);
                }

                const ScheduleStepId ready =
                    publishAndWait(
                        RendezvousPurpose::DataReady, scope,
                        sourceQueue, sourceRegion, produce,
                        actionQueue, actionRegion,
                        route.requirement.id, preLaunch);

                const ScheduleStepId action =
                    createStep(
                        ScheduledStepKind::TransferAction,
                        actionQueue, actionRegion);
                ScheduledStep& actionStep = steps_.at(action);
                actionStep.route = route.requirement.id;
                actionStep.preLaunch = preLaunch;
                addDependency(action, ready);
                routeActionCompletion_[route.requirement.id] = action;

                ScheduleStepId delivered = action;
                if (actionQueue != destinationQueue) {
                    delivered = publishAndWait(
                        RendezvousPurpose::DataReady, scope,
                        actionQueue, actionRegion, action,
                        destinationQueue, destinationRegion,
                        route.requirement.id, preLaunch);
                }

                const ScheduleStepId consume =
                    createStep(
                        ScheduledStepKind::TransferConsume,
                        destinationQueue, destinationRegion);
                ScheduledStep& consumeStep = steps_.at(consume);
                consumeStep.route = route.requirement.id;
                consumeStep.preLaunch = preLaunch;
                addDependency(consume, delivered);

                const ScheduleStepId acknowledged =
                    publishAndWait(
                        RendezvousPurpose::DataConsumed, scope,
                        destinationQueue, destinationRegion,
                        consume, sourceQueue, sourceRegion,
                        route.requirement.id, preLaunch);
                (void)acknowledged;
                previous = consume;
            }

            if (!previous) continue;
            routeCompletion_[route.requirement.id] = *previous;
            if (route.requirement.destination.operation) {
                const NodeId consumer =
                    *route.requirement.destination.operation;
                if (auto consumerStep = operationStep(
                        consumer,
                        route.requirement.destination.device)) {
                    addDependency(*consumerStep, *previous);
                } else {
                    auto all = operationStepLists_.find(consumer);
                    if (all != operationStepLists_.end()) {
                        for (ScheduleStepId step : all->second) {
                            addDependency(step, *previous);
                        }
                    }
                }
                if (preLaunch) {
                    gateAncestorControls(consumer, *previous);
                }
            }
        }
    }

    void wireUnroutedDependencies() {
        for (const DependencyEdge& edge :
             routed_->dependencies()) {
            if (edge.route || !edge.producer || !edge.consumer) {
                continue;
            }
            auto consumers =
                operationStepLists_.find(*edge.consumer);
            auto producers =
                operationStepLists_.find(*edge.producer);
            if (consumers == operationStepLists_.end() ||
                producers == operationStepLists_.end()) {
                continue;
            }
            for (ScheduleStepId consumer : consumers->second) {
                const QueueProgram* consumerQueue =
                    findQueue(steps_.at(consumer).queue);
                if (!consumerQueue) continue;
                auto exact = operationSteps_.find(
                    {*edge.producer, consumerQueue->device});
                addDependency(
                    consumer,
                    exact != operationSteps_.end()
                        ? exact->second
                        : producers->second.front());
            }
        }
    }

    void wireRoutedConsumers() {
        for (const DependencyEdge& edge :
             routed_->dependencies()) {
            if (!edge.route || !edge.consumer) continue;
            auto completion = routeCompletion_.find(*edge.route);
            auto consumers =
                operationStepLists_.find(*edge.consumer);
            if (completion == routeCompletion_.end() ||
                consumers == operationStepLists_.end()) {
                continue;
            }
            for (ScheduleStepId consumer : consumers->second) {
                addDependency(consumer, completion->second);
            }
            auto scope = routeScopes_.find(*edge.route);
            if (scope != routeScopes_.end() &&
                scope->second != RendezvousScope::PerIteration) {
                gateAncestorControls(
                    *edge.consumer, completion->second);
            }
        }
    }

    void createSplitControlRendezvous() {
        for (const auto& [control, placement] :
             routed_->placed().controlPlacements()) {
            if (placement.mode !=
                    ControlMode::SplitAcrossDevices ||
                !placement.authority) {
                continue;
            }
            auto authored =
                authoredOperations_.find(control);
            if (authored == authoredOperations_.end()) continue;
            const auto* loop =
                std::get_if<AuthoredLoop>(authored->second);
            if (!loop) continue;
            const ResolvedOperation* resolved =
                routed_->placed().resolved().findOperation(control);
            if (!resolved) continue;
            const QueueId authorityQueue =
                queueFor(resolved->region, *placement.authority);
            for (DeviceId follower : placement.followers) {
                const QueueId followerQueue =
                    queueFor(resolved->region, follower);
                createRendezvous(
                    RendezvousPurpose::ControlValue,
                    RendezvousScope::PerIteration,
                    authorityQueue, followerQueue,
                    std::nullopt, control);
                createRendezvous(
                    RendezvousPurpose::ControlDecision,
                    RendezvousScope::PerIteration,
                    authorityQueue, followerQueue,
                    std::nullopt, control);
                createRendezvous(
                    RendezvousPurpose::ControlAcknowledged,
                    RendezvousScope::PerIteration,
                    followerQueue, authorityQueue,
                    std::nullopt, control);
            }
        }
    }

    void validateAndOrder() {
        std::map<ScheduleStepId, std::size_t> indegree;
        std::map<ScheduleStepId,
                 std::vector<ScheduleStepId>> successors;
        for (auto& [id, step] : steps_) {
            std::sort(step.dependencies.begin(),
                      step.dependencies.end());
            step.dependencies.erase(
                std::unique(step.dependencies.begin(),
                            step.dependencies.end()),
                step.dependencies.end());
            indegree[id] = 0;
        }
        for (const auto& [id, step] : steps_) {
            for (ScheduleStepId dependency :
                 step.dependencies) {
                if (steps_.count(dependency) == 0) {
                    diagnostics_.error(
                        DiagCode::InternalInvariant,
                        "GraphCompiler: schedule references an "
                        "unknown step");
                    continue;
                }
                successors[dependency].push_back(id);
                ++indegree[id];
            }
        }

        for (const LogicalRendezvous& value : rendezvous_) {
            if (!findQueue(value.publisher) ||
                !findQueue(value.waiter)) {
                diagnostics_.error(
                    DiagCode::InternalInvariant,
                    "GraphCompiler: rendezvous references an "
                    "unknown queue");
            }
        }

        std::priority_queue<
            ScheduleStepId, std::vector<ScheduleStepId>,
            std::greater<ScheduleStepId>> ready;
        for (const auto& [id, degree] : indegree) {
            if (degree == 0) ready.push(id);
        }
        std::vector<ScheduleStepId> order;
        while (!ready.empty()) {
            const ScheduleStepId id = ready.top();
            ready.pop();
            order.push_back(id);
            for (ScheduleStepId successor : successors[id]) {
                if (--indegree[successor] == 0) {
                    ready.push(successor);
                }
            }
        }
        if (order.size() != steps_.size()) {
            diagnostics_.error(
                DiagCode::Cycle,
                "GraphCompiler: queue schedule contains a cycle");
            return;
        }

        std::map<ScheduleStepId, std::size_t> rank;
        for (std::size_t i = 0; i < order.size(); ++i) {
            rank[order[i]] = i;
        }
        for (QueueProgram& queue : queues_) {
            std::sort(
                queue.steps.begin(), queue.steps.end(),
                [&](ScheduleStepId lhs, ScheduleStepId rhs) {
                    return rank[lhs] < rank[rhs];
                });
        }
    }

    std::shared_ptr<const RoutedGraph> routed_;
    Diagnostics diagnostics_;
    RegionId rootRegion_;
    std::uint64_t nextStep_ = 0;
    std::uint64_t nextRendezvous_ = 0;
    std::map<NodeId, const AuthoredOperation*> authoredOperations_;
    std::map<RegionId, std::optional<NodeId>> parentControlByRegion_;
    std::map<QueueKey, QueueId> queueIds_;
    std::map<QueueId, std::size_t> queueIndexes_;
    std::vector<QueueProgram> queues_;
    std::map<ScheduleStepId, ScheduledStep> steps_;
    std::map<OperationQueueKey, ScheduleStepId> operationSteps_;
    std::map<NodeId, std::vector<ScheduleStepId>>
        operationStepLists_;
    std::map<RouteId, ScheduleStepId> routeCompletion_;
    std::map<RouteId, ScheduleStepId> routeActionCompletion_;
    std::map<RouteId, RendezvousScope> routeScopes_;
    std::vector<LogicalRendezvous> rendezvous_;
    std::vector<LogicalResourceRequirement> resources_;
};

}  // namespace

CompileResult<ScheduledGraph> scheduleGraph(
    const RoutedGraph& routed) {
    return GraphScheduler().schedule(routed);
}

}  // namespace vrt::graph
