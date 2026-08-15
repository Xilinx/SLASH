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
#include <iterator>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace vrt::graph {

namespace {

const ResolvedToken& originToken(const ValueOrigin& origin) {
    return std::visit(
        [](const auto& concrete) -> const ResolvedToken& {
            return concrete.token;
        },
        origin);
}

}  // namespace

ValueDefinitionKind valueDefinition(const ResolvedValue& value) {
    return std::visit(
        [](const auto& origin) {
            using T = std::decay_t<decltype(origin)>;
            if constexpr (std::is_same_v<T, GraphInputOrigin>) {
                return ValueDefinitionKind::GraphInput;
            } else if constexpr (
                std::is_same_v<T, OperationOutputOrigin>) {
                return ValueDefinitionKind::OperationOutput;
            } else if constexpr (
                std::is_same_v<T, RegionParameterOrigin>) {
                return ValueDefinitionKind::RegionParameter;
            } else {
                return ValueDefinitionKind::ControlResult;
            }
        },
        value.origin);
}

std::optional<NodeId> valueProducer(const ResolvedValue& value) {
    return std::visit(
        [](const auto& origin) -> std::optional<NodeId> {
            using T = std::decay_t<decltype(origin)>;
            if constexpr (std::is_same_v<T, OperationOutputOrigin>) {
                return origin.operation;
            } else if constexpr (
                std::is_same_v<T, ControlResultOrigin>) {
                return origin.control;
            } else {
                return std::nullopt;
            }
        },
        value.origin);
}

const GraphBuffer* resolvedBufferToken(const ResolvedValue& value) {
    return std::get_if<GraphBuffer>(&originToken(value.origin));
}

const GraphScalar* resolvedScalarToken(const ResolvedValue& value) {
    return std::get_if<GraphScalar>(&originToken(value.origin));
}

BoundaryAliasTable::BoundaryAliasTable(
    std::vector<BoundaryAlias> aliases)
    : aliases_(std::move(aliases)) {
    for (const BoundaryAlias& alias : aliases_) {
        sources_.emplace(alias.target, alias.source);
    }
}

/*
 * Follow boundary aliases until reaching the value allocated in the source
 * region. A malformed alias cycle is contained by seen and returns a stable
 * member of that cycle instead of looping during later lookups.
 */
ValueId BoundaryAliasTable::canonical(ValueId value) const {
    std::set<ValueId> seen;
    while (seen.insert(value).second) {
        auto source = sources_.find(value);
        if (source == sources_.end()) break;
        value = source->second;
    }
    return value;
}

std::optional<ValueId> BoundaryAliasTable::directSource(
    ValueId target) const {
    auto source = sources_.find(target);
    return source == sources_.end()
               ? std::nullopt
               : std::optional<ValueId>(source->second);
}

namespace resolve_detail {

bool TokenKey::operator<(const TokenKey& other) const {
    return std::tie(kind, scope, name) <
           std::tie(other.kind, other.scope, other.name);
}

bool TokenKey::operator==(const TokenKey& other) const {
    return kind == other.kind && scope == other.scope &&
           name == other.name;
}

TokenKey keyOf(const GraphBuffer& buffer) {
    return {ValueKind::Buffer, AuthoredScopeId(buffer.scopeId()),
            buffer.name()};
}

TokenKey keyOf(const GraphScalar& scalar) {
    return {ValueKind::Scalar, AuthoredScopeId(scalar.scopeId()),
            scalar.varName()};
}

TokenRef inputRef(const GraphBuffer& buffer, std::string port,
                  ValueAccess access) {
    return {keyOf(buffer), ValueType::bufferType(buffer.type()),
            buffer.maybeSizeScalar(), buffer, std::nullopt,
            std::move(port), access};
}

TokenRef outputRef(const GraphBuffer& buffer, std::string port,
                   ValueAccess access) {
    return {keyOf(buffer), ValueType::bufferType(buffer.type()),
            buffer.maybeSizeScalar(), buffer, std::nullopt,
            std::move(port), access};
}

TokenRef inputRef(const GraphScalar& scalar, std::string port,
                  ValueAccess access) {
    return {keyOf(scalar), ValueType::scalarType(scalar.type()),
            std::nullopt, std::nullopt, scalar,
            std::move(port), access};
}

TokenRef outputRef(const GraphScalar& scalar, std::string port,
                   ValueAccess access) {
    return {keyOf(scalar), ValueType::scalarType(scalar.type()),
            std::nullopt, std::nullopt, scalar,
            std::move(port), access};
}

ResolvedOperationKind operationKind(
    const AuthoredOperation& operation) {
    return std::visit(
        [](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, AuthoredKernel>) {
                return ResolvedOperationKind::Kernel;
            } else if constexpr (
                std::is_same_v<T, AuthoredReprogram>) {
                return ResolvedOperationKind::Reprogram;
            } else if constexpr (
                std::is_same_v<T, AuthoredBoundary>) {
                return ResolvedOperationKind::Boundary;
            } else if constexpr (std::is_same_v<T, AuthoredLoop>) {
                return ResolvedOperationKind::Loop;
            } else {
                return ResolvedOperationKind::Conditional;
            }
        },
        operation);
}

const std::vector<AuthoredDependency>& operationAfter(
    const AuthoredOperation& operation) {
    return std::visit(
        [](const auto& concrete)
            -> const std::vector<AuthoredDependency>& {
            return concrete.after;
        },
        operation);
}

const detail::PortBindings* operationIoMap(const AuthoredOperation& operation) {
    return std::visit(
        [](const auto& concrete) -> const detail::PortBindings* {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, AuthoredKernel> ||
                          std::is_same_v<T, AuthoredLoop> ||
                          std::is_same_v<T, AuthoredConditional>) {
                return &concrete.ioMap;
            } else {
                return nullptr;
            }
        },
        operation);
}

const IOTypeMap* operationIoType(
    const AuthoredOperation& operation) {
    return std::visit(
        [](const auto& concrete) -> const IOTypeMap* {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, AuthoredKernel>) {
                return &concrete.kernel.ioType;
            } else if constexpr (
                std::is_same_v<T, AuthoredLoop> ||
                std::is_same_v<T, AuthoredConditional>) {
                return &concrete.ioType;
            } else {
                return nullptr;
            }
        },
        operation);
}

std::vector<const AuthoredRegion*> childRegions(
    const AuthoredOperation& operation) {
    std::vector<const AuthoredRegion*> result;
    if (const auto* loop = std::get_if<AuthoredLoop>(&operation)) {
        if (loop->body) result.push_back(loop->body.get());
    } else if (const auto* conditional =
                   std::get_if<AuthoredConditional>(&operation)) {
        if (conditional->thenRegion) {
            result.push_back(conditional->thenRegion.get());
        }
        if (conditional->elseRegion) {
            result.push_back(conditional->elseRegion.get());
        }
    }
    return result;
}

std::vector<const AuthoredBoundary*> boundaries(
    const AuthoredRegion& region, BoundarySide side) {
    std::vector<const AuthoredBoundary*> result;
    for (const AuthoredOperation& operation : region.operations) {
        const auto* boundary =
            std::get_if<AuthoredBoundary>(&operation);
        if (boundary && boundary->side == side) {
            result.push_back(boundary);
        }
    }
    return result;
}

std::vector<TokenRef> ioInputs(const detail::PortBindings& ioMap) {
    std::vector<TokenRef> result;
    for (const auto& [port, scalar] : ioMap.inputScalars()) {
        result.push_back(inputRef(scalar, "scalar." + port));
    }
    for (const auto& [port, buffer] : ioMap.inputs()) {
        result.push_back(inputRef(buffer, "buffer." + port));
    }
    for (const auto& inout : ioMap.inouts()) {
        result.push_back(inputRef(
            inout.in, "buffer." + inout.inPort,
            ValueAccess::InoutInput));
    }
    return result;
}

std::vector<TokenRef> ioOutputs(const detail::PortBindings& ioMap) {
    std::vector<TokenRef> result;
    for (const auto& [port, scalar] : ioMap.outputScalars()) {
        result.push_back(outputRef(scalar, "scalar." + port));
    }
    for (const auto& [port, buffer] : ioMap.outputs()) {
        result.push_back(outputRef(buffer, "buffer." + port));
    }
    for (const auto& inout : ioMap.inouts()) {
        result.push_back(outputRef(
            inout.out, "buffer." + inout.outPort,
            ValueAccess::InoutOutput));
    }
    return result;
}

/*
 * A control operation consumes the parent-side values imported by each child
 * start boundary. Keep every arm's entries here; later deduplication happens
 * at dependency level because distinct boundary ports still need bindings.
 */
std::vector<TokenRef> controlBoundaryInputs(
    const AuthoredOperation& operation) {
    std::vector<TokenRef> result;
    for (const AuthoredRegion* child : childRegions(operation)) {
        for (const AuthoredBoundary* boundary :
             boundaries(*child, BoundarySide::Start)) {
            for (std::size_t i = 0;
                 i < boundary->scalarMappings.size(); ++i) {
                result.push_back(inputRef(
                    boundary->scalarMappings[i].source,
                    "boundary.scalar." + std::to_string(i)));
            }
            for (std::size_t i = 0;
                 i < boundary->bufferMappings.size(); ++i) {
                result.push_back(inputRef(
                    boundary->bufferMappings[i].source,
                    "boundary.buffer." + std::to_string(i)));
            }
        }
    }
    return result;
}

namespace {

/*
 * Prefer an authored control output name when the boundary target is exposed
 * through a named buffer port. Older or purely structural boundaries have no
 * such name, so their positional boundary port remains the stable fallback.
 */
std::string controlOutputPort(
    const AuthoredOperation& operation, const GraphBuffer& target,
    const std::string& fallback) {
    const std::map<PortName, GraphBuffer>* named =
        std::visit(
            [](const auto& concrete)
                -> const std::map<PortName, GraphBuffer>* {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (
                    std::is_same_v<T, AuthoredLoop> ||
                    std::is_same_v<T, AuthoredConditional>) {
                    return &concrete.namedOutputBuffers;
                } else {
                    return nullptr;
                }
            },
            operation);
    if (named) {
        for (const auto& [port, buffer] : *named) {
            if (buffer.scopeId() == target.scopeId() &&
                buffer.name() == target.name()) {
                return "buffer." + port.value();
            }
        }
    }
    return fallback;
}

}  // namespace

/*
 * Child end-boundary targets become outputs of the parent control node.
 * Scalar mappings remain positional; buffer mappings recover a named control
 * port when available so downstream lowering sees the authored interface.
 */
std::vector<TokenRef> controlBoundaryOutputs(
    const AuthoredOperation& operation) {
    std::vector<TokenRef> result;
    for (const AuthoredRegion* child : childRegions(operation)) {
        for (const AuthoredBoundary* boundary :
             boundaries(*child, BoundarySide::End)) {
            for (std::size_t i = 0;
                 i < boundary->scalarMappings.size(); ++i) {
                result.push_back(outputRef(
                    boundary->scalarMappings[i].target,
                    "boundary.scalar." + std::to_string(i)));
            }
            for (std::size_t i = 0;
                 i < boundary->bufferMappings.size(); ++i) {
                result.push_back(outputRef(
                    boundary->bufferMappings[i].target,
                    controlOutputPort(
                        operation,
                        boundary->bufferMappings[i].target,
                        "boundary.buffer." + std::to_string(i))));
            }
        }
    }
    return result;
}

/*
 * Combine ordinary operation outputs with values exported by control
 * boundaries. The same logical token can appear through both descriptions,
 * so collapse by TokenKey before allocating exactly one resolved value.
 */
std::vector<TokenRef> producedValues(
    const AuthoredOperation& operation) {
    std::vector<TokenRef> result;
    if (const detail::PortBindings* ioMap = operationIoMap(operation)) {
        result = ioOutputs(*ioMap);
    }
    std::vector<TokenRef> control =
        controlBoundaryOutputs(operation);
    result.insert(result.end(), control.begin(), control.end());

    std::map<TokenKey, TokenRef> unique;
    for (TokenRef& value : result) {
        unique.emplace(value.key, std::move(value));
    }
    result.clear();
    result.reserve(unique.size());
    for (auto& [key, value] : unique) {
        (void)key;
        result.push_back(std::move(value));
    }
    return result;
}

/*
 * A loop carries only values imported from its parent and exported back to
 * that same parent namespace. Intersecting start sources with end targets
 * excludes body-local temporaries and one-way parameters or results.
 */
std::set<TokenKey> loopCarriedValues(
    const AuthoredOperation& operation) {
    const auto* loop = std::get_if<AuthoredLoop>(&operation);
    if (!loop || !loop->body) return {};

    std::set<TokenKey> imported;
    std::set<TokenKey> exported;
    for (const AuthoredBoundary* boundary :
         boundaries(*loop->body, BoundarySide::Start)) {
        for (const auto& mapping : boundary->scalarMappings) {
            imported.insert(keyOf(mapping.source));
        }
        for (const auto& mapping : boundary->bufferMappings) {
            imported.insert(keyOf(mapping.source));
        }
    }
    for (const AuthoredBoundary* boundary :
         boundaries(*loop->body, BoundarySide::End)) {
        for (const auto& mapping : boundary->scalarMappings) {
            exported.insert(keyOf(mapping.target));
        }
        for (const auto& mapping : boundary->bufferMappings) {
            exported.insert(keyOf(mapping.target));
        }
    }

    std::set<TokenKey> result;
    std::set_intersection(imported.begin(), imported.end(),
                          exported.begin(), exported.end(),
                          std::inserter(result, result.end()));
    return result;
}

bool isControl(const AuthoredOperation& operation) {
    return std::holds_alternative<AuthoredLoop>(operation) ||
           std::holds_alternative<AuthoredConditional>(operation);
}

ResolutionState::ResolutionState(
    std::shared_ptr<const AuthoredGraph> authoredGraph)
    : authored(std::move(authoredGraph)),
      rootSourceScope(authored->root().sourceScope) {}

DiagnosticLocation ResolutionState::location(
    const AuthoredRegion& region,
    const AuthoredOperation& operation,
    std::optional<std::string> port) const {
    DiagnosticLocation result;
    result.region = region.id;
    result.node = authoredNodeId(operation);
    result.authoredId = authoredSourceId(operation);
    result.port = std::move(port);
    return result;
}

/*
 * Allocate one globally unique value and encode why it exists in its origin.
 * The caller supplies a concrete buffer or scalar token; operation and control
 * origins additionally require a producer. Buffer sizes always point at the
 * canonical root input value rather than creating a second size value.
 */
ValueId ResolutionState::createValue(
    const TokenRef& token, RegionId region,
    ValueDefinitionKind definition,
    std::optional<NodeId> producer) {
    const ValueId id(nextValue++);
    ResolvedValue value;
    value.id = id;
    value.region = region;
    value.type = token.type;
    value.sourceName = token.key.name;
    ResolvedToken resolvedToken =
        token.buffer
            ? ResolvedToken(*token.buffer)
            : ResolvedToken(*token.scalar);

    /*
     * Control boundary ports may be positional while the public result port
     * is named. Decode the stored spelling here so all later users see one
     * canonical ControlResultOrigin.
     */
    const PortName port(token.port);
    switch (definition) {
        case ValueDefinitionKind::GraphInput:
            value.origin = GraphInputOrigin{std::move(resolvedToken)};
            break;
        case ValueDefinitionKind::OperationOutput:
            value.origin = OperationOutputOrigin{
                *producer, port, std::move(resolvedToken)};
            break;
        case ValueDefinitionKind::RegionParameter:
            value.origin = RegionParameterOrigin{
                region, port, std::move(resolvedToken)};
            break;
        case ValueDefinitionKind::ControlResult:
            {
                const std::string& encoded = port.value();
                const std::size_t separator = encoded.find('.');
                const PortName resultPort(
                    encoded.compare(0, 9, "boundary.") == 0
                        ? token.key.name
                    : separator == std::string::npos
                        ? encoded
                        : encoded.substr(separator + 1));
                value.origin = ControlResultOrigin{
                    *producer, resultPort,
                    std::move(resolvedToken)};
            }
            break;
    }

    /*
     * Size scalars are required to be root U64 inputs by validation. Looking
     * them up after origin construction preserves that single shared value ID
     * for every buffer carrying the same size token.
     */
    if (token.size) {
        const TokenKey sizeKey = keyOf(*token.size);
        auto it = rootInputValues.find(sizeKey);
        if (it != rootInputValues.end()) value.size = it->second;
    }
    values.emplace(id, std::move(value));
    return id;
}

void ResolutionState::addDependency(
    ResolvedOperation& operation, ValueId value,
    DependencyReason reason) {
    const std::optional<NodeId> producer =
        valueProducer(values.at(value));
    if (producer && *producer != operation.id) {
        operation.dependencies.push_back(
            {*producer, reason, value});
    }
}

void ResolutionState::finishDependencies(
    ResolvedOperation& operation) {
    std::sort(operation.dependencies.begin(),
              operation.dependencies.end());
    operation.dependencies.erase(
        std::unique(operation.dependencies.begin(),
                    operation.dependencies.end()),
        operation.dependencies.end());
}

/*
 * Return the single merge record for a control node and result value.
 * Different child arms append to that shared record over separate recursive
 * visits; the index prevents duplicate records while preserving insertion
 * order in the public result table.
 */
ResolvedControlResult& ResolutionState::controlResult(
    NodeId control, ValueId result) {
    const auto key = std::make_pair(control, result);
    auto it = controlResultIndexes_.find(key);
    if (it != controlResultIndexes_.end()) {
        return controlResults[it->second];
    }
    const std::size_t index = controlResults.size();
    controlResultIndexes_[key] = index;
    PortName port;
    if (const auto* origin = std::get_if<ControlResultOrigin>(
            &values.at(result).origin)) {
        port = origin->port;
    }
    controlResults.push_back(
        ResolvedControlResult{control, std::move(port), result, {}});
    return controlResults.back();
}

}  // namespace resolve_detail
}  // namespace vrt::graph
