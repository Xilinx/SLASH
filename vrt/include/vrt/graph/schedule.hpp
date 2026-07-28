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

/**
 * @file schedule.hpp
 * @brief Queue steps and backend-neutral logical rendezvous.
 */

#ifndef VRT_GRAPH_SCHEDULE_HPP
#define VRT_GRAPH_SCHEDULE_HPP

#include <optional>
#include <vector>

#include <vrt/graph/ids.hpp>

namespace vrt::graph {

enum class ScheduledStepKind {
    Operation,
    TransferProduce,
    TransferConsume,
    TransferAction,
    EventPublish,
    EventWait,
};

enum class ControlReplicaRole {
    None,
    Authority,
    Follower,
};

enum class RendezvousPurpose {
    DataReady,
    DataConsumed,
    ControlValue,
    ControlDecision,
    ControlAcknowledged,
};

enum class RendezvousScope {
    Once,
    PreLaunch,
    PerIteration,
};

struct ScheduledStep {
    ScheduleStepId                 id;
    ScheduledStepKind              kind = ScheduledStepKind::Operation;
    QueueId                        queue;
    RegionId                       region;
    std::optional<NodeId>          operation;
    std::optional<RouteId>         route;
    std::optional<RendezvousId>    rendezvous;
    ControlReplicaRole             controlRole = ControlReplicaRole::None;
    bool                           preLaunch = false;
    std::vector<ScheduleStepId>    dependencies;
};

struct QueueProgram {
    QueueId                       id;
    DeviceId                      device;
    RegionId                      region;
    std::optional<NodeId>         parentControl;
    std::vector<ScheduleStepId>   steps;
};

struct LogicalRendezvous {
    RendezvousId             id;
    RendezvousPurpose        purpose = RendezvousPurpose::DataReady;
    RendezvousScope          scope = RendezvousScope::Once;
    QueueId                  publisher;
    QueueId                  waiter;
    std::optional<RouteId>   route;
    std::optional<NodeId>    control;
};

struct LogicalResourceRequirement {
    RendezvousId         rendezvous;
    std::vector<DeviceId> participants;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_SCHEDULE_HPP
