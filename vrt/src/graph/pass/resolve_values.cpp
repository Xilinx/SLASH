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

namespace vrt::graph::resolve_detail {

namespace {

PortName localPort(const TokenRef& token) {
    const std::string prefix =
        token.key.kind == ValueKind::Buffer ? "buffer." : "scalar.";
    return PortName(
        token.port.compare(0, prefix.size(), prefix) == 0
            ? token.port.substr(prefix.size())
            : token.port);
}

}  // namespace

/*
 * Build the complete value namespace for one region before binding any uses.
 * Parameters and graph inputs establish initial values, every producer gets a
 * distinct output ValueId, and finalValues selects the definition visible to
 * consumers after loop-carried cases are resolved.
 */
RegionValues ValueResolver::defineRegionValues(
    const AuthoredRegion& region, const RegionContext& context,
    bool rootRegion, ResolvedRegion& resolvedRegion) {
    RegionValues values;
    collectProducerMetadata(region, values);
    createRegionParameters(
        region, context, resolvedRegion, values);
    if (rootRegion) createRootInputs(region, values);
    createOperationOutputs(region, values);
    values.finalValues = values.initialValues;
    selectFinalProducers(values);
    validateProducerTypes(values);
    return values;
}

/*
 * Record all potential definitions without choosing a winner yet. Control
 * outputs may describe loop-carried values, so carry metadata is kept beside
 * producer candidates and the exact token description for each node.
 */
void ValueResolver::collectProducerMetadata(
    const AuthoredRegion& region, RegionValues& values) {
    for (const AuthoredOperation& operation : region.operations) {
        const NodeId node = authoredNodeId(operation);
        values.carriedByControl[node] =
            loopCarriedValues(operation);
        values.outputsByNode[node] = producedValues(operation);
        for (const TokenRef& output :
             values.outputsByNode[node]) {
            values.producerCandidates[output.key].push_back(node);
            values.outputDescriptions.emplace(
                OutputKey{node, output.key}, output);
        }
    }
}

/*
 * Materialize child parameters from the parent links prepared by control
 * composition. Each target gets a child-local ValueId while the link's source
 * remains available for boundary alias construction.
 */
void ValueResolver::createRegionParameters(
    const AuthoredRegion& region, const RegionContext& context,
    ResolvedRegion& resolvedRegion, RegionValues& values) {
    for (const auto& [targetKey, link] : context.parameters) {
        const ValueId value = state_.createValue(
            link.target, region.id,
            ValueDefinitionKind::RegionParameter, std::nullopt);
        values.initialValues[targetKey] = value;
        resolvedRegion.parameters.push_back(value);
    }
}

/*
 * Root scalars are inputs when explicitly declared, read-only globals, or
 * initial values for a carried loop result. A scalar declared only as an
 * output is left undefined for its producer; buffers use the explicit input
 * registry and always retain their authored size token.
 */
void ValueResolver::createRootInputs(
    const AuthoredRegion& region, RegionValues& values) {
    for (const auto& [name, type] : region.declaredScalars) {
        const TokenKey key{
            ValueKind::Scalar, region.sourceScope, name};
        bool carried = false;
        auto producers = values.producerCandidates.find(key);
        if (producers != values.producerCandidates.end()) {
            carried = std::any_of(
                producers->second.begin(), producers->second.end(),
                [&](NodeId producer) {
                    return values.carriedByControl[producer].count(key) !=
                           0;
                });
        }
        const bool explicitInput =
            region.declaredInputScalars.count(name) != 0;
        const bool declaredOutput =
            region.declaredOutputScalars.count(name) != 0;
        if ((!declaredOutput &&
             producers == values.producerCandidates.end()) ||
            explicitInput || carried) {
            const TokenRef token{
                key, ValueType::scalarType(type), std::nullopt,
                std::nullopt,
                ::vrt::graph::detail::makeGraphScalar(
                    type, name, region.sourceScope.value()),
                name, ValueAccess::Input};
            const ValueId value = state_.createValue(
                token, region.id,
                ValueDefinitionKind::GraphInput, std::nullopt);
            values.initialValues[key] = value;
            state_.rootInputValues[key] = value;
        }
    }

    /*
     * Buffer input declaration is unambiguous, so unlike scalar globals it
     * needs no producer or output-role classification.
     */
    for (const auto& [name, buffer] :
         region.declaredInputBuffers) {
        const TokenRef token =
            inputRef(buffer, name, ValueAccess::Input);
        const ValueId value = state_.createValue(
            token, region.id, ValueDefinitionKind::GraphInput,
            std::nullopt);
        values.initialValues[token.key] = value;
        state_.rootInputValues[token.key] = value;
    }
}

/*
 * Allocate a distinct value for every operation/token pair before selecting
 * visible producers. Leaf outputs use operation origins; loop and conditional
 * outputs use control-result origins that will later collect one value per arm.
 */
void ValueResolver::createOperationOutputs(
    const AuthoredRegion& region, RegionValues& values) {
    for (const AuthoredOperation& operation : region.operations) {
        const NodeId node = authoredNodeId(operation);
        for (const TokenRef& output :
             values.outputsByNode[node]) {
            const ValueDefinitionKind definition =
                isControl(operation)
                    ? ValueDefinitionKind::ControlResult
                    : ValueDefinitionKind::OperationOutput;
            values.outputValues[{node, output.key}] =
                state_.createValue(
                    output, region.id, definition, node);
        }
    }
}

/*
 * Choose the value visible under each token name after the region completes.
 * A single producer wins directly. The only legal two-producer form pairs one
 * initial definition with one carrying control, whose output becomes final
 * and whose initial incoming value is saved for control composition.
 */
void ValueResolver::selectFinalProducers(RegionValues& values) {
    for (auto& [key, candidates] : values.producerCandidates) {
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(
            std::unique(candidates.begin(), candidates.end()),
            candidates.end());

        std::vector<NodeId> carried;
        for (NodeId candidate : candidates) {
            if (values.carriedByControl[candidate].count(key)) {
                carried.push_back(candidate);
            }
        }

        /*
         * Seed a carried control from the other producer when present, or from
         * the region's initial parameter/input when the control is the sole
         * producer of that token.
         */
        NodeId finalProducer = candidates.front();
        if (candidates.size() > 1) {
            if (candidates.size() == 2 && carried.size() == 1) {
                finalProducer = carried.front();
                const NodeId initialProducer =
                    candidates[0] == finalProducer ? candidates[1]
                                                   : candidates[0];
                values.controlInitialValues[{finalProducer, key}] =
                    values.outputValues.at({initialProducer, key});
            } else {
                state_.diagnostics.error(
                    DiagCode::DuplicateProducer,
                    "GraphCompiler: multiple operations produce value '" +
                        key.name + "' in one region");
            }
        } else if (carried.size() == 1) {
            auto initial = values.initialValues.find(key);
            if (initial != values.initialValues.end()) {
                values.controlInitialValues[{finalProducer, key}] =
                    initial->second;
            }
        }
        values.finalValues[key] =
            values.outputValues.at({finalProducer, key});
    }
}

/*
 * Multiple legal producer descriptions of one logical token must still agree
 * on value kind and element type. Report every mismatch after producer
 * selection so ambiguity and type errors can be diagnosed together.
 */
void ValueResolver::validateProducerTypes(
    const RegionValues& values) {
    for (const auto& [key, candidates] :
         values.producerCandidates) {
        if (candidates.empty()) continue;
        const TokenRef& expected =
            values.outputDescriptions.at({candidates.front(), key});
        for (NodeId candidate : candidates) {
            const TokenRef& actual =
                values.outputDescriptions.at({candidate, key});
            if (!(actual.type == expected.type)) {
                state_.diagnostics.error(
                    DiagCode::TypeMismatch,
                    "GraphCompiler: producers of value '" + key.name +
                        "' disagree on its type");
            }
        }
    }
}

/*
 * Seed the resolved operation with structural identity and explicit authored
 * ordering. Data, boundary, control, and derived ordering dependencies are
 * appended by later binding stages before final deduplication.
 */
ResolvedOperation ValueResolver::makeOperation(
    const AuthoredRegion& region,
    const AuthoredOperation& authoredOperation) const {
    ResolvedOperation operation;
    operation.id = authoredNodeId(authoredOperation);
    operation.region = region.id;
    operation.kind = operationKind(authoredOperation);
    operation.structural =
        operation.kind == ResolvedOperationKind::Boundary;
    for (const AuthoredDependency& dependency :
         operationAfter(authoredOperation)) {
        if (dependency.target) {
            operation.dependencies.push_back(
                {*dependency.target,
                 DependencyReason::UserOrdering, std::nullopt});
        }
    }
    return operation;
}

/*
 * Select the definition that an input must observe. Precedence is deliberate:
 * a carried control reads its saved initial value; an inout reads the value
 * before its own write; declared root inputs beat same-name outputs; ordinary
 * uses see the region's final producer; nested control may finally read a
 * root scalar. Anything else is outside the region's value namespace.
 */
std::optional<ValueId> ValueResolver::valueForUse(
    const AuthoredRegion& region,
    const AuthoredOperation& operation, const TokenRef& token,
    const RegionValues& values, bool rootRegion) {
    const NodeId node = authoredNodeId(operation);

    /*
     * A loop's own output is the post-loop value, so using it as the initial
     * input would form a self-cycle. Read the separately recorded seed.
     */
    if (values.carriedByControl.at(node).count(token.key)) {
        auto initial =
            values.controlInitialValues.find({node, token.key});
        if (initial != values.controlInitialValues.end()) {
            return initial->second;
        }
        state_.diagnostics.error(
            DiagCode::InvalidControlResult,
            "GraphCompiler: loop op '" + authoredSourceId(operation) +
                "' has no initial value for carried token '" +
                token.key.name + "'",
            state_.location(region, operation, token.port));
        return std::nullopt;
    }

    /*
     * An inout binding describes both sides of one mutation. When its output
     * reuses the input token name, bind the read side to the region's initial
     * value rather than the newly allocated output.
     */
    auto ownOutput =
        values.outputValues.find({node, token.key});
    if (token.access == ValueAccess::InoutInput &&
        ownOutput != values.outputValues.end()) {
        auto initial = values.initialValues.find(token.key);
        if (initial != values.initialValues.end()) {
            return initial->second;
        }
    }

    /*
     * At the root, an explicitly declared input remains the source even if an
     * operation later produces the same carried token.
     */
    if (rootRegion) {
        const bool declaredBufferInput =
            token.key.kind == ValueKind::Buffer &&
            region.declaredInputBuffers.count(token.key.name) != 0;
        const bool declaredScalarInput =
            token.key.kind == ValueKind::Scalar &&
            region.declaredInputScalars.count(token.key.name) != 0;
        if (declaredBufferInput || declaredScalarInput) {
            auto initial = values.initialValues.find(token.key);
            if (initial != values.initialValues.end()) {
                return initial->second;
            }
        }
    }

    /*
     * Normal uses consume the selected region producer. Root scalars are the
     * one cross-region exception and are looked up in the shared input table.
     */
    auto value = values.finalValues.find(token.key);
    if (value != values.finalValues.end()) return value->second;
    if (token.key.kind == ValueKind::Scalar &&
        token.key.scope == state_.rootSourceScope) {
        auto root = state_.rootInputValues.find(token.key);
        if (root != state_.rootInputValues.end()) {
            return root->second;
        }
    }
    state_.diagnostics.error(
        DiagCode::InvalidScope,
        "GraphCompiler: op '" + authoredSourceId(operation) +
            "' consumes " +
            std::string(token.key.kind == ValueKind::Buffer
                            ? "buffer '"
                            : "scalar '") +
            token.key.name + "' with no producer",
        state_.location(region, operation, token.port));
    return std::nullopt;
}

/*
 * Bind read sides before write sides so dependencies refer only to consumed
 * values. Output bindings point at values preallocated for this operation,
 * then read-write pairs are recorded separately for backends that need the
 * mutation relationship rather than two unrelated ports.
 */
void ValueResolver::bindDataOperation(
    const AuthoredRegion& region,
    const AuthoredOperation& authoredOperation,
    const RegionValues& values, bool rootRegion,
    ResolvedOperation& operation) {
    const detail::PortBindings* ioMap = operationIoMap(authoredOperation);
    if (!ioMap) return;
    for (const TokenRef& input : ioInputs(*ioMap)) {
        if (auto value = valueForUse(
                region, authoredOperation, input, values,
                rootRegion)) {
            operation.bindings.push_back(
                {PortName(input.port), localPort(input),
                 *value, input.access});
            state_.addDependency(operation, *value);
        }
    }
    for (const TokenRef& output : ioOutputs(*ioMap)) {
        auto value =
            values.outputValues.find({operation.id, output.key});
        if (value != values.outputValues.end()) {
            operation.bindings.push_back(
                {PortName(output.port), localPort(output),
                 value->second, output.access});
        }
    }
    bindInouts(*ioMap, operation);
}

/*
 * Recover each authored read-write pair from the operation's resolved port
 * bindings. Only complete pairs enter the side table; missing halves already
 * produced validation or value-resolution diagnostics.
 */
void ValueResolver::bindInouts(
    const detail::PortBindings& ioMap, ResolvedOperation& operation) {
    for (const detail::PortBindings::InoutBinding& inout : ioMap.inouts()) {
        const PortName inputPort("buffer." + inout.inPort);
        const PortName outputPort("buffer." + inout.outPort);
        auto input = std::find_if(
            operation.bindings.begin(), operation.bindings.end(),
            [&](const ResolvedBinding& binding) {
                return binding.port == inputPort &&
                       binding.access == ValueAccess::InoutInput;
            });
        auto output = std::find_if(
            operation.bindings.begin(), operation.bindings.end(),
            [&](const ResolvedBinding& binding) {
                return binding.port == outputPort &&
                       binding.access == ValueAccess::InoutOutput;
            });
        if (input != operation.bindings.end() &&
            output != operation.bindings.end()) {
            state_.inoutBindings.push_back(
                {operation.id, PortName(inout.inPort),
                 input->value, output->value});
        }
    }
}

/*
 * Graph outputs are flags on the final visible ValueIds, not separate values.
 * This preserves aliases and control merges chosen earlier; a missing final
 * definition remains an error even if an intermediate value used that name.
 */
void ValueResolver::markRootOutputs(
    const AuthoredRegion& region,
    const std::map<TokenKey, ValueId>& finalValues) {
    auto markOutput = [&](const TokenKey& key,
                          const std::string& kind) {
        auto value = finalValues.find(key);
        if (value == finalValues.end()) {
            state_.diagnostics.error(
                DiagCode::MissingProducer,
                "GraphCompiler: graph output " + kind + " '" +
                    key.name + "' has no producer");
            return;
        }
        state_.values.at(value->second).graphOutput = true;
    };
    for (const auto& [name, buffer] :
         region.declaredOutputBuffers) {
        (void)name;
        markOutput(keyOf(buffer), "buffer");
    }
    for (const auto& [name, type] :
         region.declaredOutputScalars) {
        markOutput(
            TokenKey{ValueKind::Scalar, region.sourceScope, name},
            "scalar");
        (void)type;
    }
}

}  // namespace vrt::graph::resolve_detail
