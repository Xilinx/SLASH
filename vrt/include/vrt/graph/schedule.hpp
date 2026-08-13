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
#include <string>
#include <variant>
#include <vector>

#include <vrt/graph/ids.hpp>
#include <vrt/graph/transfer.hpp>

namespace vrt::graph {

struct ScheduledOperation {
    NodeId operation;
};

struct ScheduledTransferProduce {
    RouteId       route;
    TransferLegId leg;
};

struct ScheduledTransferConsume {
    RouteId       route;
    TransferLegId leg;
};

struct ScheduledTransferAction {
    RouteId       route;
    TransferLegId leg;
};

struct ScheduledEventPublish {
    RendezvousId rendezvous;
};

struct ScheduledEventWait {
    RendezvousId rendezvous;
};

struct ScheduledGraphInput {
    RegionId             graph;
    std::vector<ValueId> values;
};

struct ScheduledGraphOutput {
    RegionId             graph;
    std::vector<ValueId> values;
};

struct ScheduledBoundaryMapping {
    PortName  port;
    ReplicaId source;
    ReplicaId target;
};

struct ScheduledBoundaryMaterialization {
    NodeId                                boundary;
    std::vector<ScheduledBoundaryMapping> mappings;
};

using ScheduledStepPayload =
    std::variant<ScheduledOperation, ScheduledTransferProduce,
                 ScheduledTransferConsume, ScheduledTransferAction,
                 ScheduledEventPublish, ScheduledEventWait,
                 ScheduledGraphInput, ScheduledGraphOutput,
                 ScheduledBoundaryMaterialization>;

struct ScheduledStep {
    ScheduleStepId              id;
    QueueId                     queue;
    RegionId                    region;
    ScheduledStepPayload        payload;
    std::vector<ScheduleStepId> dependencies;
};

struct QueueProgram {
    QueueId                       id;
    DeviceId                      device;
    RegionId                      region;
    std::optional<NodeId>         parentControl;
    std::vector<ScheduleStepId>   steps;
};

struct DataReadyRendezvous {
    RouteId               route;
    TransferPhase         phase = TransferPhase::Once;
    TransferControlScope  scope = GraphTransferScope{};
};

struct DataConsumedRendezvous {
    RouteId               route;
    TransferPhase         phase = TransferPhase::Once;
    TransferControlScope  scope = GraphTransferScope{};
};

struct ControlValueRendezvous {
    NodeId control;
};

struct ControlDecisionRendezvous {
    NodeId control;
};

struct ControlAcknowledgedRendezvous {
    NodeId control;
};

using LogicalRendezvousPayload =
    std::variant<DataReadyRendezvous, DataConsumedRendezvous,
                 ControlValueRendezvous, ControlDecisionRendezvous,
                 ControlAcknowledgedRendezvous>;

struct LogicalRendezvous {
    RendezvousId              id;
    QueueId                   publisher;
    QueueId                   waiter;
    LogicalRendezvousPayload  payload;
};

struct SplitControlFollowerProtocol {
    QueueId       queue;
    ScheduleStepId operationStep;
    RendezvousId  value;
    RendezvousId  decision;
    RendezvousId  acknowledgement;
};

struct SplitControlProtocol {
    NodeId                                    control;
    QueueId                                   authorityQueue;
    ScheduleStepId                            authorityStep;
    std::vector<SplitControlFollowerProtocol> followers;
    QueueId                                   primaryQueue;
};

struct LogicalResourceRequirement {
    RendezvousId         rendezvous;
    std::vector<DeviceId> participants;
};

struct LogicalScalarRequirement {
    ScalarResourceId scalar;
    ValueId          value;
    DeviceId         owner;
    std::string      key;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_SCHEDULE_HPP
