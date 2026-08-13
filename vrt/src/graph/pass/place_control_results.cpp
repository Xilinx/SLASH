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
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace vrt::graph::place_detail {

namespace {

const AuthoredPlacementHints* outputHints(
    const PlacementState& state, NodeId control) {
    const AuthoredOperation* authored =
        state.resolved->authored().index().findOperation(control);
    if (!authored) return nullptr;
    return std::visit(
        [](const auto& concrete) -> const AuthoredPlacementHints* {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, AuthoredLoop> ||
                          std::is_same_v<T, AuthoredConditional>) {
                return &concrete.outputPlacement;
            }
            return nullptr;
        },
        *authored);
}

std::optional<DeviceId> hintFor(
    const PlacementState& state,
    const ResolvedControlResult& result) {
    const AuthoredPlacementHints* hints =
        outputHints(state, result.control);
    const ResolvedValue* value =
        state.resolved->findValue(result.result);
    if (!hints || !value) return std::nullopt;
    const auto& placements =
        value->type.kind == ValueKind::Buffer
            ? hints->buffers
            : hints->scalars;
    auto placement = placements.find(result.port);
    return placement == placements.end()
               ? std::nullopt
               : std::optional<DeviceId>(placement->second);
}

/*
 * Infer a result only when every publishing arm agrees on one concrete
 * memory; the loop's initial seed is not a publication. Conflicting producer
 * locations require an explicit hint, while a present hint defers the choice
 * to the override phase below.
 */
void inferControlResult(
    PlacementState& state, const ResolvedControlResult& result) {
    std::set<MemoryPlacement> incoming;
    for (const ControlIncoming& value : result.incoming) {
        if (value.arm == ControlArm::LoopInitial) continue;
        if (const ValueReplica* replica =
                primaryReplica(state, value.value)) {
            incoming.insert(replica->memory);
        }
    }
    if (incoming.size() == 1) {
        ensureReplica(
            state, result.result, *incoming.begin(),
            ReplicaPurpose::ControlResult, result.control,
            result.port, true);
    } else if (incoming.size() > 1 && !hintFor(state, result)) {
        state.diagnostics.error(
            DiagCode::AmbiguousPlacement,
            "GraphCompiler: control result has producers on "
            "different devices and no output placement hint");
    }
}

const ResolvedControlResult* findResult(
    const PlacementState& state, NodeId control,
    const PortName& port, ValueKind kind) {
    for (const ResolvedControlResult& result :
         state.resolved->controlResults()) {
        const ResolvedValue* value =
            state.resolved->findValue(result.result);
        if (result.control == control && result.port == port &&
            value && value->type.kind == kind) {
            return &result;
        }
    }
    return nullptr;
}

/*
 * Hints must name both a known device and a resolved result port. A hint on
 * the inferred device preserves its exact HBM region; moving the result to
 * another device drops that device-local region and promotes the hinted
 * replica to primary.
 */
void applyHints(
    PlacementState& state, NodeId control,
    const std::map<PortName, DeviceId>& hints, ValueKind kind) {
    for (const auto& [port, device] : hints) {
        if (!state.capabilities->find(device)) {
            state.diagnostics.error(
                DiagCode::UnknownDevice,
                "GraphCompiler: output placement for port '" +
                    port.value() + "' names unknown device '" +
                    device.value() + "'");
            continue;
        }
        const ResolvedControlResult* result =
            findResult(state, control, port, kind);
        if (!result) {
            state.diagnostics.error(
                DiagCode::InvalidControlResult,
                "GraphCompiler: output placement refers to "
                "unknown port '" + port.value() + "'");
            continue;
        }
        MemoryPlacement memory{device, std::nullopt};
        if (const ValueReplica* inferred =
                primaryReplica(state, result->result);
            inferred && inferred->memory.device == device) {
            memory = inferred->memory;
        }
        ensureReplica(
            state, result->result, memory,
            ReplicaPurpose::ControlResult, control, port, true);
    }
}

}  // namespace

/*
 * Inference deliberately precedes explicit hints. This lets same-device
 * hints retain an inferred memory region while still allowing authored
 * placement to override producer consensus.
 */
void placeControlResults(PlacementState& state) {
    /*
     * First establish every unambiguous producer-derived location so hints
     * can refine rather than erase exact memory information.
     */
    for (const ResolvedControlResult& result :
         state.resolved->controlResults()) {
        inferControlResult(state, result);
    }

    /*
     * Apply authored choices only after the inference set is complete;
     * buffer and scalar namespaces are checked independently.
     */
    for (const auto& [node, operation] :
         state.resolved->authored().index().operations()) {
        (void)operation;
        const AuthoredPlacementHints* hints = outputHints(state, node);
        if (!hints) continue;
        applyHints(
            state, node, hints->buffers, ValueKind::Buffer);
        applyHints(
            state, node, hints->scalars, ValueKind::Scalar);
    }
}

}  // namespace vrt::graph::place_detail
