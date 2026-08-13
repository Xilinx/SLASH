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
#include <tuple>
#include <utility>
#include <variant>

namespace vrt::graph::route_detail {

namespace {

bool consumesValue(ValueAccess access) {
    return access == ValueAccess::Input ||
           access == ValueAccess::InoutInput ||
           access == ValueAccess::Condition ||
           access == ValueAccess::TripCount ||
           access == ValueAccess::BoundarySource;
}

/*
 * Port replicas are the precise destination evidence for an operation/value
 * pair. Keeping every placement here preserves distinct device memory regions
 * before the caller removes duplicate locations.
 */
std::vector<ValueUse> placedUses(
    const PlacedGraph& placed, NodeId operation, ValueId value) {
    std::vector<ValueUse> result;
    for (const PortPlacement& port : placed.portPlacements()) {
        const ValueReplica* replica =
            placed.findReplica(port.replica);
        if (port.operation == operation && replica &&
            replica->value == value) {
            result.push_back(
                {operation, value, port.replica, replica->memory});
        }
    }
    return result;
}

/*
 * Controls and non-kernel operations may have no port replica. Split control
 * consumes on every participant; ordinary placement contributes its single
 * regionless device. This fallback is used only when precise ports are absent.
 */
void addFallbackUses(
    const PlacedGraph& placed, NodeId operation, ValueId value,
    std::vector<ValueUse>& result) {
    auto control = placed.controlPlacements().find(operation);
    const auto* split =
        control == placed.controlPlacements().end()
            ? nullptr
            : std::get_if<SplitControlPlacement>(&control->second);
    if (split) {
        for (const DeviceId& participant : split->participants()) {
            result.push_back(
                {operation, value, std::nullopt,
                 {participant, std::nullopt}});
        }
        return;
    }
    auto placement = placed.operationPlacements().find(operation);
    if (placement != placed.operationPlacements().end()) {
        result.push_back(
            {operation, value, std::nullopt,
             {placement->second.device, std::nullopt}});
    }
}

/*
 * Prefer exact port memories, then fall back to operation ownership. Sorting
 * makes the chosen replica deterministic, while deduplication by memory avoids
 * planning multiple routes to aliases of the same physical location.
 */
std::vector<ValueUse> usesForBinding(
    const PlacedGraph& placed, NodeId operation, ValueId value) {
    std::vector<ValueUse> result =
        placedUses(placed, operation, value);
    if (result.empty()) {
        addFallbackUses(placed, operation, value, result);
    }
    std::sort(
        result.begin(), result.end(),
        [](const ValueUse& lhs, const ValueUse& rhs) {
            return std::tie(lhs.targetMemory, lhs.target) <
                   std::tie(rhs.targetMemory, rhs.target);
        });
    result.erase(
        std::unique(
            result.begin(), result.end(),
            [](const ValueUse& lhs, const ValueUse& rhs) {
                return lhs.targetMemory == rhs.targetMemory;
            }),
        result.end());
    return result;
}

}  // namespace

/*
 * Discover only executable consumers. Structural operations are represented
 * by boundary mappings, and control boundary-source bindings are published by
 * the dedicated control-result planner. Inout inputs retain a separate
 * copy-on-write bit because equal value/location uses are not interchangeable.
 */
std::vector<ValueUse> discoverValueUses(const PlacedGraph& placed) {
    std::vector<ValueUse> result;
    std::set<std::tuple<NodeId, ValueId, MemoryPlacement, bool>> seen;

    /*
     * Walk resolved bindings to retain semantic access kind, then expand each
     * binding into the concrete destination memories chosen by placement.
     */
    for (const auto& [node, operation] :
         placed.resolved().operations()) {
        if (operation.structural) continue;
        for (const ResolvedBinding& binding : operation.bindings) {
            if (!consumesValue(binding.access)) continue;
            if (binding.access == ValueAccess::BoundarySource &&
                (operation.kind == ResolvedOperationKind::Loop ||
                 operation.kind ==
                     ResolvedOperationKind::Conditional)) {
                continue;
            }

            /*
             * Deduplicate only after copy-on-write is known; mutable and
             * shared uses at one location require different routing policy.
             */
            for (ValueUse use :
                 usesForBinding(placed, node, binding.value)) {
                use.copyOnWrite =
                    binding.access == ValueAccess::InoutInput;
                if (seen
                        .insert(
                            {node, binding.value, use.targetMemory,
                             use.copyOnWrite})
                        .second) {
                    result.push_back(std::move(use));
                }
            }
        }
    }
    return result;
}

}  // namespace vrt::graph::route_detail
