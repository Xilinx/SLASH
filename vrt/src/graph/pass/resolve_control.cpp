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

#include "resolve_graph_internal.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace vrt::graph::resolve_detail {

/*
 * Boundary direction determines which side already owns a resolved value.
 * Start boundaries alias parent sources to child parameters; end boundaries
 * consume child finals and connect them to parent control-result targets.
 */
void ControlComposer::bindBoundary(
    const AuthoredRegion& region,
    const AuthoredOperation& authoredOperation,
    const AuthoredBoundary& boundary,
    const RegionContext& context, const RegionValues& values,
    ResolvedRegion& resolvedRegion,
    ResolvedOperation& operation) {
    if (boundary.side == BoundarySide::Start) {
        bindStartBoundary(
            region, authoredOperation, boundary, context, values,
            operation);
    } else {
        bindEndBoundary(
            region, authoredOperation, boundary, context, values,
            resolvedRegion, operation);
    }
}

void ControlComposer::bindStartBoundary(
    const AuthoredRegion& region,
    const AuthoredOperation& authoredOperation,
    const AuthoredBoundary& boundary,
    const RegionContext& context, const RegionValues& values,
    ResolvedOperation& operation) {
    for (std::size_t i = 0;
         i < boundary.scalarMappings.size(); ++i) {
        bindBoundaryParameter(
            region, authoredOperation, boundary, context, values,
            keyOf(boundary.scalarMappings[i].target),
            "scalar." + std::to_string(i), operation);
    }
    for (std::size_t i = 0;
         i < boundary.bufferMappings.size(); ++i) {
        bindBoundaryParameter(
            region, authoredOperation, boundary, context, values,
            keyOf(boundary.bufferMappings[i].target),
            "buffer." + std::to_string(i), operation);
    }
}

/*
 * Bind both sides of a start-boundary alias at the same structural port.
 * The parent source comes from RegionContext and the child target from this
 * region's initial values; both must exist or the boundary cannot represent
 * the parent-to-child transfer.
 */
void ControlComposer::bindBoundaryParameter(
    const AuthoredRegion& region,
    const AuthoredOperation& authoredOperation,
    const AuthoredBoundary& boundary,
    const RegionContext& context, const RegionValues& values,
    const TokenKey& target, const std::string& port,
    ResolvedOperation& operation) {
    auto link = context.parameters.find(target);
    auto value = values.initialValues.find(target);
    if (link == context.parameters.end() ||
        value == values.initialValues.end()) {
        state_.diagnostics.error(
            DiagCode::InvalidBoundary,
            "GraphCompiler: boundary op '" + boundary.authoredId +
                "' has no parent parameter binding",
            state_.location(region, authoredOperation));
        return;
    }
    operation.bindings.push_back(
        {port, link->second.source, ValueAccess::BoundarySource});
    operation.bindings.push_back(
        {port, value->second, ValueAccess::BoundaryTarget});
    state_.boundaryAliases.push_back(
        {operation.id, PortName(port),
         link->second.source, value->second});
}

void ControlComposer::bindEndBoundary(
    const AuthoredRegion& region,
    const AuthoredOperation& authoredOperation,
    const AuthoredBoundary& boundary,
    const RegionContext& context, const RegionValues& values,
    ResolvedRegion& resolvedRegion,
    ResolvedOperation& operation) {
    for (std::size_t i = 0;
         i < boundary.scalarMappings.size(); ++i) {
        const auto& mapping = boundary.scalarMappings[i];
        bindBoundaryResult(
            region, authoredOperation, boundary, context, values,
            inputRef(mapping.source, ""), keyOf(mapping.target),
            "scalar." + std::to_string(i), resolvedRegion,
            operation);
    }
    for (std::size_t i = 0;
         i < boundary.bufferMappings.size(); ++i) {
        const auto& mapping = boundary.bufferMappings[i];
        bindBoundaryResult(
            region, authoredOperation, boundary, context, values,
            inputRef(mapping.source, ""), keyOf(mapping.target),
            "buffer." + std::to_string(i), resolvedRegion,
            operation);
    }
}

/*
 * Resolve an end-boundary mapping from the child's final value to the result
 * ValueId preallocated on the parent control. The boundary operation depends
 * on the child producer and exposes the source as a region result; control-arm
 * incoming records are attached after the child recursion returns.
 */
void ControlComposer::bindBoundaryResult(
    const AuthoredRegion& region,
    const AuthoredOperation& authoredOperation,
    const AuthoredBoundary& boundary,
    const RegionContext& context, const RegionValues& values,
    const TokenRef& source, const TokenKey& target,
    const std::string& port, ResolvedRegion& resolvedRegion,
    ResolvedOperation& operation) {
    auto sourceValue = values.finalValues.find(source.key);
    auto targetValue = context.resultTargets.find(target);
    if (sourceValue == values.finalValues.end() ||
        targetValue == context.resultTargets.end()) {
        state_.diagnostics.error(
            DiagCode::InvalidControlResult,
            "GraphCompiler: boundary op '" + boundary.authoredId +
                "' cannot resolve its result mapping",
            state_.location(region, authoredOperation, port));
        return;
    }
    operation.bindings.push_back(
        {port, sourceValue->second, ValueAccess::BoundarySource});
    operation.bindings.push_back(
        {port, targetValue->second, ValueAccess::BoundaryTarget});
    state_.addDependency(
        operation, sourceValue->second,
        DependencyReason::BoundaryOrdering);
    resolvedRegion.results.push_back(sourceValue->second);
}

/*
 * Bind the parent-facing interface of a loop or conditional before resolving
 * its children. Boundary imports/exports become control ports, then loop trip
 * count and scalar predicate operands are added as ordinary value dependencies.
 * Literal predicate operands require no binding.
 */
void ControlComposer::bindControlOperation(
    const AuthoredRegion& region,
    const AuthoredOperation& authoredOperation,
    const RegionValues& values, bool rootRegion,
    ValueResolver& valueResolver, ResolvedOperation& operation) {
    for (const TokenRef& input :
         controlBoundaryInputs(authoredOperation)) {
        if (auto value = valueResolver.valueForUse(
                region, authoredOperation, input, values,
                rootRegion)) {
            operation.bindings.push_back(
                {input.port, *value, ValueAccess::BoundarySource});
            state_.addDependency(operation, *value);
        }
    }
    for (const TokenRef& output :
         controlBoundaryOutputs(authoredOperation)) {
        auto value =
            values.outputValues.find({operation.id, output.key});
        if (value != values.outputValues.end()) {
            operation.bindings.push_back(
                {output.port, value->second,
                 ValueAccess::BoundaryTarget});
        }
    }

    if (const auto* loop =
            std::get_if<AuthoredLoop>(&authoredOperation)) {
        if (loop->tripCount) {
            TokenRef trip = inputRef(
                ::vrt::graph::detail::makeGraphScalar(
                    loop->tripCount->type(),
                    loop->tripCount->name(),
                    loop->tripCount->scopeId(),
                    loop->tripCount->graphId()),
                "trip_count", ValueAccess::TripCount);
            if (auto value = valueResolver.valueForUse(
                    region, authoredOperation, trip, values,
                    rootRegion)) {
                operation.bindings.push_back(
                    {trip.port, *value, trip.access});
                state_.addDependency(operation, *value);
            }
        }
        if (loop->condition) {
            addConditionBindings(
                region, authoredOperation, *loop->condition, values,
                rootRegion, valueResolver, operation);
        }
    } else if (const auto* conditional =
                   std::get_if<AuthoredConditional>(
                       &authoredOperation)) {
        addConditionBindings(
            region, authoredOperation, conditional->condition,
            values, rootRegion, valueResolver, operation);
    }
}

/*
 * Convert only scalar-backed condition operands into resolved bindings.
 * The port spelling identifies lhs, rhs, or epsilon, while valueForUse applies
 * the same carried, local, and root-scalar precedence as ordinary data inputs.
 */
void ControlComposer::addConditionBindings(
    const AuthoredRegion& region,
    const AuthoredOperation& authoredOperation,
    const Condition& condition, const RegionValues& values,
    bool rootRegion, ValueResolver& valueResolver,
    ResolvedOperation& operation) {
    auto addOperand =
        [&](const std::optional<ConditionOperand>& operand,
            const std::string& port) {
            if (!operand || !operand->isScalar()) return;
            TokenRef token = inputRef(
                ::vrt::graph::detail::makeGraphScalar(
                    operand->type(), operand->name(),
                    operand->scopeId(), operand->graphId()),
                port, ValueAccess::Condition);
            if (auto value = valueResolver.valueForUse(
                    region, authoredOperation, token, values,
                    rootRegion)) {
                operation.bindings.push_back(
                    {port, *value, ValueAccess::Condition});
                state_.addDependency(operation, *value);
            }
        };
    addOperand(condition.lhs(), "condition.lhs");
    addOperand(condition.rhs(), "condition.rhs");
    addOperand(condition.epsilon(), "condition.epsilon");
}

/*
 * Prepare everything a child can know about its parent before recursion.
 * Start mappings become parameter links and end mappings become result targets;
 * the control node and arm travel with both tables for later incoming records.
 */
RegionContext ControlComposer::makeChildContext(
    const AuthoredRegion& parent,
    const AuthoredOperation& operation,
    const AuthoredRegion& child, ControlArm arm,
    const RegionValues& values) {
    RegionContext context;
    context.control = authoredNodeId(operation);
    context.arm = arm;
    addStartBoundaryParameters(
        parent, operation, child, values, context);
    addResultTargets(operation, child, values, context);
    return context;
}

/*
 * Resolve a child import in the parent namespace. A loop-carried token must
 * use its saved seed, otherwise the region-final producer wins; root scalars
 * are the only values implicitly visible across nested scopes.
 */
std::optional<ValueId> ControlComposer::parentValue(
    const AuthoredRegion& parent,
    const AuthoredOperation& operation, const TokenRef& source,
    const RegionValues& values) {
    const NodeId control = authoredNodeId(operation);
    if (values.carriedByControl.at(control).count(source.key)) {
        auto initial =
            values.controlInitialValues.find({control, source.key});
        if (initial != values.controlInitialValues.end()) {
            return initial->second;
        }
    }
    auto value = values.finalValues.find(source.key);
    if (value != values.finalValues.end()) return value->second;
    if (source.key.kind == ValueKind::Scalar &&
        source.key.scope == state_.rootSourceScope) {
        auto root = state_.rootInputValues.find(source.key);
        if (root != state_.rootInputValues.end()) {
            return root->second;
        }
    }
    state_.diagnostics.error(
        DiagCode::InvalidScope,
        "GraphCompiler: control op '" +
            authoredSourceId(operation) +
            "' cannot resolve child input '" + source.key.name + "'",
        state_.location(parent, operation, source.port));
    return std::nullopt;
}

/*
 * Translate each child start mapping into a parent source and a typed
 * child-local target description. Failed parent lookups are omitted after
 * diagnostics so independent mappings can still be resolved.
 */
void ControlComposer::addStartBoundaryParameters(
    const AuthoredRegion& parent,
    const AuthoredOperation& operation,
    const AuthoredRegion& child, const RegionValues& values,
    RegionContext& context) {
    for (const AuthoredBoundary* boundary :
         boundaries(child, BoundarySide::Start)) {
        for (std::size_t i = 0;
             i < boundary->scalarMappings.size(); ++i) {
            const auto& mapping = boundary->scalarMappings[i];
            TokenRef source = inputRef(
                mapping.source,
                "boundary.scalar." + std::to_string(i));
            if (auto value =
                    parentValue(parent, operation, source, values)) {
                context.parameters[keyOf(mapping.target)] =
                    ParameterLink{
                        *value,
                        outputRef(
                            mapping.target, source.port,
                            ValueAccess::BoundaryTarget)};
            }
        }
        for (std::size_t i = 0;
             i < boundary->bufferMappings.size(); ++i) {
            const auto& mapping = boundary->bufferMappings[i];
            TokenRef source = inputRef(
                mapping.source,
                "boundary.buffer." + std::to_string(i));
            if (auto value =
                    parentValue(parent, operation, source, values)) {
                context.parameters[keyOf(mapping.target)] =
                    ParameterLink{
                        *value,
                        outputRef(
                            mapping.target, source.port,
                            ValueAccess::BoundaryTarget)};
            }
        }
    }
}

/*
 * Map every child export target to the parent control's preallocated result.
 * Explicit end-boundary targets and implicit control I/O share this table, so
 * both styles converge on the same ValueId and later incoming merge record.
 */
void ControlComposer::addResultTargets(
    const AuthoredOperation& operation,
    const AuthoredRegion& child, const RegionValues& values,
    RegionContext& context) {
    const NodeId control = authoredNodeId(operation);
    auto addTarget = [&](const TokenKey& target) {
        auto result = values.outputValues.find({control, target});
        if (result != values.outputValues.end()) {
            context.resultTargets[target] = result->second;
        }
    };
    for (const AuthoredBoundary* boundary :
         boundaries(child, BoundarySide::End)) {
        for (const auto& mapping : boundary->scalarMappings) {
            addTarget(keyOf(mapping.target));
        }
        for (const auto& mapping : boundary->bufferMappings) {
            addTarget(keyOf(mapping.target));
        }
    }
    if (const detail::PortBindings* ioMap = operationIoMap(operation)) {
        for (const auto& [port, scalar] :
             ioMap->outputScalars()) {
            (void)port;
            addTarget(keyOf(scalar));
        }
        for (const auto& [port, buffer] : ioMap->outputs()) {
            (void)port;
            addTarget(keyOf(buffer));
        }
        for (const auto& inout : ioMap->inouts()) {
            addTarget(keyOf(inout.out));
        }
    }
}

/*
 * Add explicit boundary mappings first, then infer control-port results not
 * represented by those mappings. Both paths use the shared control-result
 * registry, and the implicit path suppresses an arm already supplied
 * explicitly.
 */
void ControlComposer::addControlIncoming(
    const AuthoredOperation& operation,
    const AuthoredRegion& child,
    const RegionResolution& childResolution,
    const RegionContext& context, ControlArm arm,
    const RegionValues& values) {
    addExplicitIncoming(
        operation, child, childResolution, context, arm, values);
    addImplicitIncoming(
        operation, child, childResolution, context, arm);
}

/*
 * Turn explicit child end mappings into incoming values for the parent control
 * result. Conditional arms contribute one branch value. A loop backedge also
 * needs one LoopInitial value; prefer the target's direct seed, then reuse the
 * seed of another exported target that aliases the same child source. This
 * handles multiple parent result names for one carried body value.
 */
void ControlComposer::addExplicitIncoming(
    const AuthoredOperation& operation,
    const AuthoredRegion& child,
    const RegionResolution& childResolution,
    const RegionContext& context, ControlArm arm,
    const RegionValues& values) {
    const NodeId control = authoredNodeId(operation);

    /*
     * Initial seeds are indexed by parent result target. Search sibling
     * mappings only when this target has no direct entry, and require the
     * child source key to match before borrowing that seed.
     */
    auto initialFor =
        [&](const TokenKey& source,
            const TokenKey& target) -> std::optional<ValueId> {
        auto direct =
            values.controlInitialValues.find({control, target});
        if (direct != values.controlInitialValues.end()) {
            return direct->second;
        }
        for (const AuthoredBoundary* candidate :
             boundaries(child, BoundarySide::End)) {
            auto sharedSourceInitial =
                [&](const auto& mapping) -> std::optional<ValueId> {
                if (!(keyOf(mapping.source) == source)) {
                    return std::nullopt;
                }
                auto initial = values.controlInitialValues.find(
                    {control, keyOf(mapping.target)});
                return initial == values.controlInitialValues.end()
                           ? std::nullopt
                           : std::optional<ValueId>(initial->second);
            };
            if (source.kind == ValueKind::Scalar) {
                for (const ScalarBoundaryMapping& mapping :
                     candidate->scalarMappings) {
                    if (auto initial = sharedSourceInitial(mapping)) {
                        return initial;
                    }
                }
            } else {
                for (const BufferBoundaryMapping& mapping :
                     candidate->bufferMappings) {
                    if (auto initial = sharedSourceInitial(mapping)) {
                        return initial;
                    }
                }
            }
        }
        return std::nullopt;
    };

    /*
     * Record each arm's child-final value. Loop initial values are inserted
     * once per control result after the backedge confirms that result exists.
     */
    for (const AuthoredBoundary* boundary :
         boundaries(child, BoundarySide::End)) {
        auto add = [&](const TokenKey& source,
                       const TokenKey& target) {
            auto sourceValue =
                childResolution.finalValues.find(source);
            auto resultValue = context.resultTargets.find(target);
            if (sourceValue == childResolution.finalValues.end() ||
                resultValue == context.resultTargets.end()) {
                return;
            }
            ResolvedControlResult& result =
                state_.controlResult(control, resultValue->second);
            result.incoming.push_back(
                {arm, child.id, sourceValue->second});
            if (arm != ControlArm::LoopBackedge) return;
            const bool hasInitial = std::any_of(
                result.incoming.begin(), result.incoming.end(),
                [](const ControlIncoming& incoming) {
                    return incoming.arm == ControlArm::LoopInitial;
                });
            const std::optional<ValueId> initial =
                initialFor(source, target);
            if (initial && !hasInitial) {
                result.incoming.push_back(
                    {ControlArm::LoopInitial,
                     state_.values.at(*initial).region, *initial});
            }
        };
        for (const auto& mapping : boundary->scalarMappings) {
            add(keyOf(mapping.source), keyOf(mapping.target));
        }
        for (const auto& mapping : boundary->bufferMappings) {
            add(keyOf(mapping.source), keyOf(mapping.target));
        }
    }
}

/*
 * Control I/O may expose child results without an explicit end-boundary
 * mapping. Probe each output port by value kind and feed the unique child
 * producer into the same result records used by explicit mappings.
 */
void ControlComposer::addImplicitIncoming(
    const AuthoredOperation& operation,
    const AuthoredRegion& child,
    const RegionResolution& childResolution,
    const RegionContext& context, ControlArm arm) {
    const detail::PortBindings* controlIo = operationIoMap(operation);
    if (!controlIo) return;
    const NodeId control = authoredNodeId(operation);
    for (const auto& [port, scalar] :
         controlIo->outputScalars()) {
        addImplicitPortIncoming(
            control, child, childResolution, context, arm, port,
            keyOf(scalar));
    }
    for (const auto& [port, buffer] : controlIo->outputs()) {
        addImplicitPortIncoming(
            control, child, childResolution, context, arm, port,
            keyOf(buffer));
    }
    for (const auto& inout : controlIo->inouts()) {
        addImplicitPortIncoming(
            control, child, childResolution, context, arm,
            inout.outPort, keyOf(inout.out));
    }
}

/*
 * Resolve an implicit control output by authored port name inside one child
 * arm. Exactly one distinct child token may produce that port; none is a
 * missing branch result and more than one is ambiguous. The selected token
 * must also survive as the child's final value and have a parent result target.
 */
void ControlComposer::addImplicitPortIncoming(
    NodeId control, const AuthoredRegion& child,
    const RegionResolution& childResolution,
    const RegionContext& context, ControlArm arm,
    const std::string& port, const TokenKey& target) {
    /*
     * Gather tokens rather than operation IDs because several descriptions
     * can refer to the same logical child value.
     */
    std::vector<TokenKey> candidates;
    for (const AuthoredOperation& childOperation :
         child.operations) {
        const detail::PortBindings* childIo = operationIoMap(childOperation);
        if (!childIo) continue;
        if (target.kind == ValueKind::Buffer) {
            auto output = childIo->outputs().find(port);
            if (output != childIo->outputs().end()) {
                candidates.push_back(keyOf(output->second));
            }
            for (const auto& inout : childIo->inouts()) {
                if (inout.outPort == port) {
                    candidates.push_back(keyOf(inout.out));
                }
            }
        } else {
            auto output = childIo->outputScalars().find(port);
            if (output != childIo->outputScalars().end()) {
                candidates.push_back(keyOf(output->second));
            }
        }
    }

    /*
     * Deduplicate aliases before enforcing the one-producer branch contract.
     * The diagnostic deliberately distinguishes absence from ambiguity.
     */
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
        std::unique(candidates.begin(), candidates.end()),
        candidates.end());
    if (candidates.size() != 1) {
        state_.diagnostics.error(
            DiagCode::InvalidControlResult,
            "GraphCompiler: control output port '" + port +
                "' has " +
                (candidates.empty()
                     ? "no body producer"
                     : "multiple body producers"));
        return;
    }

    /*
     * Explicit mappings are processed first. If they already supplied this
     * arm, keep that authoritative source instead of adding a duplicate.
     */
    auto source =
        childResolution.finalValues.find(candidates.front());
    auto result = context.resultTargets.find(target);
    if (source == childResolution.finalValues.end() ||
        result == context.resultTargets.end()) {
        return;
    }
    ResolvedControlResult& controlResultValue =
        state_.controlResult(control, result->second);
    const bool alreadyPresent = std::any_of(
        controlResultValue.incoming.begin(),
        controlResultValue.incoming.end(),
        [&](const ControlIncoming& incoming) {
            return incoming.arm == arm;
        });
    if (!alreadyPresent) {
        controlResultValue.incoming.push_back(
            {arm, child.id, source->second});
    }
}

}  // namespace vrt::graph::resolve_detail
