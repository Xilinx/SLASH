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

#include <variant>

namespace vrt::graph::schedule_detail {

const SplitControlProtocol* splitControl(
    const SchedulingState& state, NodeId control) {
    auto protocol = state.splitControlIndexes.find(control);
    return protocol == state.splitControlIndexes.end()
               ? nullptr
               : &state.splitControls[protocol->second];
}

void scheduleSplitControls(SchedulingState& state) {
    /*
     * Co-located controls need no protocol. For each split control:
     * - the authority sends the current value and its decision to each
     *   follower;
     * - each follower acknowledges the decision back to the authority;
     * - missing authority or follower operation steps are invariant errors.
     *
     * The operation steps already exist. Store their queues and rendezvous
     * IDs so backend lowerers can build the publish/wait handshake.
     */
    const PlacedGraph& placed = state.routed->placed();
    for (const auto& [control, placement] :
         placed.controlPlacements()) {
        const auto* split =
            std::get_if<SplitControlPlacement>(&placement);
        if (!split) continue;
        const ResolvedOperation* operation =
            placed.resolved().findOperation(control);
        if (!operation) continue;
        const auto authorityStep =
            operationStep(state, control, split->authority());
        if (!authorityStep) {
            state.diagnostics.error(
                DiagCode::InternalInvariant,
                "GraphCompiler: split control has no authority step");
            continue;
        }
        SplitControlProtocol protocol;
        protocol.control = control;
        protocol.authorityQueue = state.steps.at(*authorityStep).queue;
        protocol.authorityStep = *authorityStep;
        protocol.primaryQueue =
            queueFor(state, operation->region, split->primary());

        /* Allocate one independent three-event handshake per follower. */
        for (const DeviceId& follower : split->followers()) {
            const auto followerStep =
                operationStep(state, control, follower);
            if (!followerStep) {
                state.diagnostics.error(
                    DiagCode::InternalInvariant,
                    "GraphCompiler: split control has no follower step");
                continue;
            }
            const QueueId followerQueue =
                state.steps.at(*followerStep).queue;
            const RendezvousId value = *createRendezvous(
                state, protocol.authorityQueue, followerQueue,
                ControlValueRendezvous{control});
            const RendezvousId decision = *createRendezvous(
                state, protocol.authorityQueue, followerQueue,
                ControlDecisionRendezvous{control});
            const RendezvousId acknowledgement = *createRendezvous(
                state, followerQueue, protocol.authorityQueue,
                ControlAcknowledgedRendezvous{control});
            protocol.followers.push_back(
                {followerQueue, *followerStep, value, decision,
                 acknowledgement});
        }
        state.splitControlIndexes[control] =
            state.splitControls.size();
        state.splitControls.push_back(std::move(protocol));
    }
}

}  // namespace vrt::graph::schedule_detail
