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

#include <set>
#include <utility>

namespace vrt::graph::schedule_detail {

/*
 * Same-queue synchronization needs no event. A cross-queue rendezvous records
 * both queue roles and the participating devices so resource binding can
 * choose a host event or a device-owned slot later.
 */
std::optional<RendezvousId> createRendezvous(
    SchedulingState& state, QueueId publisher, QueueId waiter,
    LogicalRendezvousPayload payload) {
    if (publisher == waiter) return std::nullopt;

    LogicalRendezvous value;
    value.id = RendezvousId(state.nextRendezvous++);
    value.publisher = publisher;
    value.waiter = waiter;
    value.payload = std::move(payload);
    state.rendezvous.push_back(value);

    std::set<DeviceId> participants;
    if (const QueueProgram* queue = findQueue(state, publisher)) {
        participants.insert(queue->device);
    }
    if (const QueueProgram* queue = findQueue(state, waiter)) {
        participants.insert(queue->device);
    }
    LogicalResourceRequirement requirement;
    requirement.rendezvous = value.id;
    requirement.participants.assign(
        participants.begin(), participants.end());
    state.resources.push_back(std::move(requirement));
    return value.id;
}

/*
 * Preserve a local dependency unchanged. Across queues, publish only after
 * the supplied producer step and make the receiving wait depend on that
 * publication; the wait becomes the receiver's dependency anchor.
 */
ScheduleStepId publishAndWait(
    SchedulingState& state, QueueId publisherQueue,
    RegionId publisherRegion, ScheduleStepId publisherDependency,
    QueueId waiterQueue, RegionId waiterRegion,
    LogicalRendezvousPayload payload) {
    if (publisherQueue == waiterQueue) {
        return publisherDependency;
    }
    const RendezvousId rendezvous = *createRendezvous(
        state, publisherQueue, waiterQueue, std::move(payload));
    const ScheduleStepId publish = createStep(
        state, publisherQueue, publisherRegion,
        ScheduledEventPublish{rendezvous});
    addDependency(state, publish, publisherDependency);
    const ScheduleStepId wait = createStep(
        state, waiterQueue, waiterRegion,
        ScheduledEventWait{rendezvous});
    addDependency(state, wait, publish);
    return wait;
}

}  // namespace vrt::graph::schedule_detail
