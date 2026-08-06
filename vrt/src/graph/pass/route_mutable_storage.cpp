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

#include <vector>

namespace vrt::graph::route_detail {

namespace {

/*
 * Mutable aliases cannot change device. Memory regions conflict only when
 * both sides are concrete and unequal; an unspecified region is device-wide
 * placement evidence, not proof of different storage.
 */
bool incompatibleMutableStorage(
    const MemoryPlacement& input,
    const MemoryPlacement& output) {
    return input.device != output.device ||
           (input.region && output.region &&
            input.region != output.region);
}

std::vector<const ValueReplica*> inputReplicas(
    const PlacedGraph& placed, NodeId operation, ValueId value) {
    std::vector<const ValueReplica*> result;
    for (const PortPlacement& port : placed.portPlacements()) {
        if (port.operation != operation) continue;
        const ValueReplica* replica =
            placed.findReplica(port.replica);
        if (replica && replica->value == value) {
            result.push_back(replica);
        }
    }
    return result;
}

/*
 * Inout input and output ports are two views of one mutation, not values that
 * routing may reconcile with a copy. Every placed input replica must therefore
 * agree with the output's device and concrete memory region.
 */
void validateInouts(RoutingState& state) {
    for (const ResolvedInoutBinding& inout :
         state.placed->resolved().inouts()) {
        const ValueReplica* output =
            state.placed->primaryReplica(inout.output);
        if (!output) continue;
        for (const ValueReplica* input :
             inputReplicas(
                 *state.placed, inout.operation, inout.input)) {
            if (!incompatibleMutableStorage(
                    input->memory, output->memory)) {
                continue;
            }
            state.diagnostics.error(
                DiagCode::IncompatibleMemoryPlacement,
                "GraphCompiler: inout buffer ports require one "
                "device and memory region");
            break;
        }
    }
}

/*
 * A loop carry may cross devices through an explicit per-iteration route.
 * On one device, however, changing between two concrete HBM regions would
 * require rebinding mutable storage inside the loop, which is unsupported.
 * Missing arms or replicas are diagnosed by their owning passes.
 */
void validateLoopCarries(RoutingState& state) {
    for (const ResolvedControlResult& result :
         state.placed->resolved().controlResults()) {
        const ControlIncoming* initial = nullptr;
        const ControlIncoming* backedge = nullptr;

        /*
         * Only a true initial/backedge pair defines recurrent mutable storage;
         * conditional arms and incomplete results do not enter this check.
         */
        for (const ControlIncoming& incoming : result.incoming) {
            if (incoming.arm == ControlArm::LoopInitial) {
                initial = &incoming;
            } else if (
                incoming.arm == ControlArm::LoopBackedge) {
                backedge = &incoming;
            }
        }
        if (!initial || !backedge) continue;
        const ValueReplica* initialReplica =
            state.placed->primaryReplica(initial->value);
        const ValueReplica* backedgeReplica =
            state.placed->primaryReplica(backedge->value);
        if (!initialReplica || !backedgeReplica) continue;
        if (initialReplica->memory.device ==
                backedgeReplica->memory.device &&
            initialReplica->memory.region &&
            backedgeReplica->memory.region &&
            initialReplica->memory.region !=
                backedgeReplica->memory.region) {
            state.diagnostics.error(
                DiagCode::IncompatibleMemoryPlacement,
                "GraphCompiler: loop-carried buffers cannot cross "
                "memory regions");
        }
    }
}

}  // namespace

/*
 * Run identity checks before route planning. A transfer may materialize a
 * value elsewhere, but it must never be mistaken for preserving mutable
 * aliasing or repairing an invalid loop-carried HBM binding.
 */
void validateMutableStorage(RoutingState& state) {
    validateInouts(state);
    validateLoopCarries(state);
}

}  // namespace vrt::graph::route_detail
