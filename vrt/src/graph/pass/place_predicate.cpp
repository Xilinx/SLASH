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
#include <string>
#include <utility>
#include <variant>

namespace vrt::graph::place_detail {

namespace {

/*
 * Either operand may carry the runtime scalar that gives a predicate
 * locality. Literal-only conditions provide no producer location.
 */
std::optional<std::pair<std::string, std::uint64_t>> conditionScalar(
    const Condition& condition) {
    auto scalar =
        [](const std::optional<ConditionOperand>& operand)
            -> std::optional<std::pair<std::string, std::uint64_t>> {
        if (!operand || !operand->isScalar()) return std::nullopt;
        return std::make_pair(operand->name(), operand->scopeId());
    };
    if (auto lhs = scalar(condition.lhs())) return lhs;
    return scalar(condition.rhs());
}

bool producedOn(
    const PlacementState& state, ValueId value, DeviceId candidate) {
    const ResolvedValue* resolved = state.resolved->findValue(value);
    if (!resolved) return false;
    const std::optional<NodeId> producer = valueProducer(*resolved);
    if (!producer) return false;
    auto placement = state.operationPlacements.find(*producer);
    return placement != state.operationPlacements.end() &&
           placement->second.device == candidate;
}

}  // namespace

/*
 * Autonomous control requires the changing predicate to be produced on the
 * candidate device. Loops use the backedge value that drives the next
 * iteration; conditionals use their condition binding. Missing metadata,
 * literal-only predicates, and remotely produced scalars all fail closed.
 */
bool predicateAvailableOnCandidate(
    const PlacementState& state,
    const AuthoredOperation& operation, DeviceId candidate) {
    const NodeId control = authoredNodeId(operation);
    const Condition* condition = nullptr;
    const auto* loop = std::get_if<AuthoredLoop>(&operation);
    if (loop && loop->condition) {
        condition = &*loop->condition;
    } else if (const auto* conditional =
                   std::get_if<AuthoredConditional>(&operation)) {
        condition = &conditional->condition;
    }
    if (!condition) return false;
    const auto scalar = conditionScalar(*condition);
    if (!scalar) return false;

    /*
     * The initial loop value only seeds iteration zero. Local autonomy is
     * determined by the backedge that must be re-evaluated repeatedly.
     */
    if (loop) {
        for (const ResolvedControlResult& result :
             state.resolved->controlResults()) {
            if (result.control != control) continue;
            const ResolvedValue* resultValue =
                state.resolved->findValue(result.result);
            if (!resultValue ||
                resultValue->type.kind != ValueKind::Scalar ||
                resultValue->sourceName != scalar->first) {
                continue;
            }
            for (const ControlIncoming& incoming : result.incoming) {
                if (incoming.arm == ControlArm::LoopBackedge &&
                    producedOn(state, incoming.value, candidate)) {
                    return true;
                }
            }
        }
        return false;
    }

    /*
     * A conditional has no recurrence, so its resolved condition binding is
     * the authoritative link from predicate name to producer placement.
     */
    const ResolvedOperation* resolvedControl =
        state.resolved->findOperation(control);
    if (!resolvedControl) return false;
    for (const ResolvedBinding& binding : resolvedControl->bindings) {
        const ResolvedValue* value =
            state.resolved->findValue(binding.value);
        if (binding.access == ValueAccess::Condition && value &&
            value->sourceName == scalar->first &&
            producedOn(state, binding.value, candidate)) {
            return true;
        }
    }
    return false;
}

/*
 * Keep capability policy outside the placement pass: this request carries
 * child topology, control kind, and predicate locality as evidence for the
 * candidate device's evaluator.
 */
ControlCapabilityRequest controlRequest(
    const PlacementState& state,
    const AuthoredOperation& operation, DeviceId candidate,
    const RegionPlacementSummary& child) {
    ControlCapabilityRequest request;
    request.candidate = candidate;
    request.childDevices.assign(
        child.devices.begin(), child.devices.end());
    request.childHasWork = child.hasWork;
    request.childHasNestedControl = child.hasNestedControl;
    request.childHasDataBoundaries = child.hasDataBoundaries;
    request.predicateAvailableOnCandidate =
        predicateAvailableOnCandidate(state, operation, candidate);
    if (const auto* loop = std::get_if<AuthoredLoop>(&operation)) {
        request.kind = ControlKind::Loop;
        request.loopKind = loop->kind;
        request.condition = loop->condition;
    } else {
        request.kind = ControlKind::Conditional;
        request.condition =
            std::get<AuthoredConditional>(operation).condition;
    }
    return request;
}

}  // namespace vrt::graph::place_detail
