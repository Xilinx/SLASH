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

#include <vrt/graph/ir/resolved_graph.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vrt::graph {

namespace {

struct TokenKey {
    ValueKind    kind = ValueKind::Buffer;
    std::uint64_t scope = 0;
    std::string  name;

    bool operator<(const TokenKey& other) const {
        return std::tie(kind, scope, name) <
               std::tie(other.kind, other.scope, other.name);
    }

    bool operator==(const TokenKey& other) const {
        return kind == other.kind && scope == other.scope &&
               name == other.name;
    }
};

struct TokenRef {
    TokenKey                  key;
    ValueType                 type;
    std::optional<GraphScalar> size;
    std::optional<GraphBuffer> buffer;
    std::optional<GraphScalar> scalar;
    std::string               port;
    ValueAccess               access = ValueAccess::Input;
};

struct ParameterLink {
    ValueId  source;
    TokenRef target;
};

struct RegionContext {
    std::optional<NodeId>              control;
    ControlArm                         arm = ControlArm::LoopBackedge;
    std::map<TokenKey, ParameterLink>  parameters;
    std::map<TokenKey, ValueId>        resultTargets;
};

struct RegionResolution {
    std::shared_ptr<const ResolvedRegion> region;
    std::map<TokenKey, ValueId>           finalValues;
};

TokenKey keyOf(const GraphBuffer& buffer) {
    return {ValueKind::Buffer, buffer.scopeId(), buffer.name()};
}

TokenKey keyOf(const GraphScalar& scalar) {
    return {ValueKind::Scalar, scalar.scopeId(), scalar.varName()};
}

TokenRef inputRef(const GraphBuffer& buffer, std::string port,
                  ValueAccess access = ValueAccess::Input) {
    return {keyOf(buffer), ValueType::bufferType(buffer.type()),
            buffer.maybeSizeScalar(), buffer, std::nullopt,
            std::move(port), access};
}

TokenRef outputRef(const GraphBuffer& buffer, std::string port,
                   ValueAccess access = ValueAccess::Output) {
    return {keyOf(buffer), ValueType::bufferType(buffer.type()),
            buffer.maybeSizeScalar(), buffer, std::nullopt,
            std::move(port), access};
}

TokenRef inputRef(const GraphScalar& scalar, std::string port,
                  ValueAccess access = ValueAccess::Input) {
    return {keyOf(scalar), ValueType::scalarType(scalar.type()),
            std::nullopt, std::nullopt, scalar,
            std::move(port), access};
}

TokenRef outputRef(const GraphScalar& scalar, std::string port,
                   ValueAccess access = ValueAccess::Output) {
    return {keyOf(scalar), ValueType::scalarType(scalar.type()),
            std::nullopt, std::nullopt, scalar,
            std::move(port), access};
}

const char* scalarTypeName(ScalarType type) {
    switch (type) {
        case ScalarType::U8:  return "U8";
        case ScalarType::U16: return "U16";
        case ScalarType::U32: return "U32";
        case ScalarType::U64: return "U64";
        case ScalarType::I8:  return "I8";
        case ScalarType::I16: return "I16";
        case ScalarType::I32: return "I32";
        case ScalarType::I64: return "I64";
        case ScalarType::F32: return "F32";
        case ScalarType::F64: return "F64";
    }
    return "unknown";
}

const char* bufferTypeName(BufferType type) {
    switch (type) {
        case BufferType::U8:  return "U8";
        case BufferType::U16: return "U16";
        case BufferType::U32: return "U32";
        case BufferType::U64: return "U64";
        case BufferType::I8:  return "I8";
        case BufferType::I16: return "I16";
        case BufferType::I32: return "I32";
        case BufferType::I64: return "I64";
        case BufferType::F32: return "F32";
        case BufferType::F64: return "F64";
    }
    return "unknown";
}

ResolvedOperationKind operationKind(const AuthoredOperation& operation) {
    return std::visit(
        [](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, AuthoredKernel>) {
                return ResolvedOperationKind::Kernel;
            } else if constexpr (std::is_same_v<T, AuthoredReprogram>) {
                return ResolvedOperationKind::Reprogram;
            } else if constexpr (std::is_same_v<T, AuthoredBoundary>) {
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

const IOMap* operationIoMap(const AuthoredOperation& operation) {
    return std::visit(
        [](const auto& concrete) -> const IOMap* {
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

const IOTypeMap* operationIoType(const AuthoredOperation& operation) {
    return std::visit(
        [](const auto& concrete) -> const IOTypeMap* {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, AuthoredKernel>) {
                return &concrete.kernel.ioType;
            } else if constexpr (std::is_same_v<T, AuthoredLoop> ||
                                 std::is_same_v<T, AuthoredConditional>) {
                return &concrete.ioType;
            } else {
                return nullptr;
            }
        },
        operation);
}

bool hasDeclaredPorts(const IOTypeMap& ioType) {
    return !ioType.inputScalars.empty() ||
           !ioType.outputScalars.empty() ||
           !ioType.inputs.empty() ||
           !ioType.outputs.empty() ||
           !ioType.inouts.empty();
}

template <class Ports>
const typename Ports::value_type* findPort(const Ports& ports,
                                           const std::string& name) {
    auto it = std::find_if(
        ports.begin(), ports.end(),
        [&](const auto& port) { return port.name == name; });
    return it == ports.end() ? nullptr : &*it;
}

class GraphResolver {
   public:
    CompileResult<ResolvedGraph> resolve(const AuthoredGraph& authored) {
        authored_ = std::make_shared<AuthoredGraph>(authored);
        registerTree(authored_->root());
        rootRegionId_ = authored_->root().id;
        rootSourceScope_ = authored_->root().sourceScopeId;
        if (authored_->root().operations.empty()) {
            diagnostics_.error(
                DiagCode::EmptyGraph,
                "GraphCompiler::compile: graph has no ops");
        }

        RegionResolution root =
            resolveRegion(authored_->root(), RegionContext{}, true);
        if (diagnostics_.hasErrors()) {
            return CompileResult<ResolvedGraph>::failure(
                std::move(diagnostics_));
        }
        return CompileResult<ResolvedGraph>::success(
            ResolvedGraph(authored_, std::move(root.region),
                          std::move(values_), std::move(operations_),
                          std::move(controlResults_)),
            std::move(diagnostics_));
    }

   private:
    using OutputKey = std::pair<NodeId, TokenKey>;

    DiagnosticLocation location(const AuthoredRegion& region,
                                const AuthoredOperation& operation,
                                std::optional<std::string> port =
                                    std::nullopt) const {
        DiagnosticLocation result;
        result.region = region.id;
        result.node = authoredNodeId(operation);
        result.authoredId = authoredSourceId(operation);
        result.port = std::move(port);
        return result;
    }

    void registerTree(const AuthoredRegion& region) {
        regionsBySourceScope_[region.sourceScopeId] = region.id;
        for (const AuthoredOperation& operation : region.operations) {
            authoredOperations_[authoredNodeId(operation)] = &operation;
            std::visit(
                [&](const auto& concrete) {
                    using T = std::decay_t<decltype(concrete)>;
                    if constexpr (std::is_same_v<T, AuthoredLoop>) {
                        if (concrete.body) registerTree(*concrete.body);
                    } else if constexpr (
                        std::is_same_v<T, AuthoredConditional>) {
                        if (concrete.thenRegion) {
                            registerTree(*concrete.thenRegion);
                        }
                        if (concrete.elseRegion) {
                            registerTree(*concrete.elseRegion);
                        }
                    }
                },
                operation);
        }
    }

    static std::vector<const AuthoredRegion*> childRegions(
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

    static std::vector<const AuthoredBoundary*> boundaries(
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

    static std::vector<TokenRef> ioInputs(const IOMap& ioMap) {
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

    static std::vector<TokenRef> ioOutputs(const IOMap& ioMap) {
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

    static std::vector<TokenRef> controlBoundaryInputs(
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

    static std::vector<TokenRef> controlBoundaryOutputs(
        const AuthoredOperation& operation) {
        std::vector<TokenRef> result;
        const auto namedPort =
            [&](const GraphBuffer& target,
                const std::string& fallback) {
                const std::map<std::string, GraphBuffer>* named =
                    std::visit(
                        [](const auto& concrete)
                            -> const std::map<std::string,
                                              GraphBuffer>* {
                            using T =
                                std::decay_t<decltype(concrete)>;
                            if constexpr (
                                std::is_same_v<T, AuthoredLoop> ||
                                std::is_same_v<T,
                                               AuthoredConditional>) {
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
                            return "buffer." + port;
                        }
                    }
                }
                return fallback;
            };
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
                        namedPort(
                            boundary->bufferMappings[i].target,
                            "boundary.buffer." +
                                std::to_string(i))));
                }
            }
        }
        return result;
    }

    static std::vector<TokenRef> producedValues(
        const AuthoredOperation& operation) {
        std::vector<TokenRef> result;
        if (const IOMap* ioMap = operationIoMap(operation)) {
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

    static std::set<TokenKey> loopCarriedValues(
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

    void validateIoPorts(const AuthoredRegion& region,
                         const AuthoredOperation& operation) {
        const IOTypeMap* ioType = operationIoType(operation);
        const IOMap* ioMap = operationIoMap(operation);
        if (!ioType || !ioMap || !hasDeclaredPorts(*ioType)) return;

        auto missing = [&](const std::string& port,
                           const std::string& description) {
            diagnostics_.error(
                DiagCode::UnboundPort,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' missing mandatory " + description + " port '" +
                    port + "'",
                location(region, operation, port));
        };
        auto mismatch = [&](const std::string& port,
                            const std::string& description,
                            const std::string& expected,
                            const std::string& actual) {
            diagnostics_.error(
                DiagCode::TypeMismatch,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' " + description + " '" + port +
                    "' type mismatch: declared " + expected +
                    ", bound " + actual,
                location(region, operation, port));
        };

        for (const auto& expected : ioType->inputScalars) {
            auto it = ioMap->inputScalars().find(expected.name);
            if (it == ioMap->inputScalars().end()) {
                missing(expected.name, "input scalar");
            } else if (it->second.type() != expected.type) {
                mismatch(expected.name, "input scalar",
                         scalarTypeName(expected.type),
                         scalarTypeName(it->second.type()));
            }
        }
        for (const auto& expected : ioType->outputScalars) {
            auto it = ioMap->outputScalars().find(expected.name);
            if (it == ioMap->outputScalars().end()) {
                missing(expected.name, "output scalar");
            } else if (it->second.type() != expected.type) {
                mismatch(expected.name, "output scalar",
                         scalarTypeName(expected.type),
                         scalarTypeName(it->second.type()));
            }
        }
        for (const auto& expected : ioType->inputs) {
            auto it = ioMap->inputs().find(expected.name);
            if (it == ioMap->inputs().end()) {
                missing(expected.name, "input buffer");
            } else if (it->second.type() != expected.type) {
                mismatch(expected.name, "input buffer",
                         bufferTypeName(expected.type),
                         bufferTypeName(it->second.type()));
            }
        }
        for (const auto& expected : ioType->outputs) {
            auto it = ioMap->outputs().find(expected.name);
            if (it == ioMap->outputs().end()) {
                missing(expected.name, "output buffer");
            } else if (it->second.type() != expected.type) {
                mismatch(expected.name, "output buffer",
                         bufferTypeName(expected.type),
                         bufferTypeName(it->second.type()));
            }
        }
        for (const auto& expected : ioType->inouts) {
            auto it = std::find_if(
                ioMap->inouts().begin(), ioMap->inouts().end(),
                [&](const IOMap::InoutBinding& binding) {
                    return binding.inPort == expected.in.name &&
                           binding.outPort == expected.out.name;
                });
            if (it == ioMap->inouts().end()) {
                missing(expected.in.name + "/" + expected.out.name,
                        "RW buffer");
            } else {
                if (it->in.type() != expected.in.type) {
                    mismatch(expected.in.name, "RW input buffer",
                             bufferTypeName(expected.in.type),
                             bufferTypeName(it->in.type()));
                }
                if (it->out.type() != expected.out.type) {
                    mismatch(expected.out.name, "RW output buffer",
                             bufferTypeName(expected.out.type),
                             bufferTypeName(it->out.type()));
                }
            }
        }

        for (const auto& [port, scalar] : ioMap->inputScalars()) {
            const auto* expected = findPort(ioType->inputScalars, port);
            if (!expected) {
                diagnostics_.error(
                    DiagCode::UnboundPort,
                    "GraphCompiler: op '" + authoredSourceId(operation) +
                        "' binds unknown input scalar port '" + port + "'",
                    location(region, operation, port));
            } else if (expected->type != scalar.type()) {
                mismatch(port, "input scalar",
                         scalarTypeName(expected->type),
                         scalarTypeName(scalar.type()));
            }
        }
        for (const auto& [port, scalar] : ioMap->outputScalars()) {
            const auto* expected = findPort(ioType->outputScalars, port);
            if (!expected) {
                diagnostics_.error(
                    DiagCode::UnboundPort,
                    "GraphCompiler: op '" + authoredSourceId(operation) +
                        "' binds unknown output scalar port '" + port + "'",
                    location(region, operation, port));
            } else if (expected->type != scalar.type()) {
                mismatch(port, "output scalar",
                         scalarTypeName(expected->type),
                         scalarTypeName(scalar.type()));
            }
        }
        for (const auto& [port, buffer] : ioMap->inputs()) {
            const auto* expected = findPort(ioType->inputs, port);
            if (!expected) {
                diagnostics_.error(
                    DiagCode::UnboundPort,
                    "GraphCompiler: op '" + authoredSourceId(operation) +
                        "' binds unknown input buffer port '" + port + "'",
                    location(region, operation, port));
            } else if (expected->type != buffer.type()) {
                mismatch(port, "input buffer",
                         bufferTypeName(expected->type),
                         bufferTypeName(buffer.type()));
            }
        }
        for (const auto& [port, buffer] : ioMap->outputs()) {
            const auto* expected = findPort(ioType->outputs, port);
            if (!expected) {
                diagnostics_.error(
                    DiagCode::UnboundPort,
                    "GraphCompiler: op '" + authoredSourceId(operation) +
                        "' binds unknown output buffer port '" + port + "'",
                    location(region, operation, port));
            } else if (expected->type != buffer.type()) {
                mismatch(port, "output buffer",
                         bufferTypeName(expected->type),
                         bufferTypeName(buffer.type()));
            }
        }
        for (const auto& binding : ioMap->inouts()) {
            auto expected = std::find_if(
                ioType->inouts.begin(), ioType->inouts.end(),
                [&](const RWBufferPort& port) {
                    return port.in.name == binding.inPort &&
                           port.out.name == binding.outPort;
                });
            if (expected == ioType->inouts.end()) {
                diagnostics_.error(
                    DiagCode::UnboundPort,
                    "GraphCompiler: op '" + authoredSourceId(operation) +
                        "' binds unknown RW buffer ports '" +
                        binding.inPort + "'/'" + binding.outPort + "'",
                    location(region, operation, binding.inPort));
            }
        }
    }

    void validateBoundary(const AuthoredRegion& region,
                          const AuthoredOperation& operation,
                          const AuthoredBoundary& boundary) {
        const std::uint64_t expectedSource =
            boundary.side == BoundarySide::Start
                ? boundary.sourceParentScopeId
                : boundary.sourceLocalScopeId;
        const std::uint64_t expectedTarget =
            boundary.side == BoundarySide::Start
                ? boundary.sourceLocalScopeId
                : boundary.sourceParentScopeId;

        auto validateScopes =
            [&](std::uint64_t source, std::uint64_t target,
                const std::string& kind, const std::string& name) {
                if (source != expectedSource || target != expectedTarget) {
                    diagnostics_.error(
                        DiagCode::InvalidBoundary,
                        "GraphCompiler: boundary op '" +
                            boundary.authoredId + "' " + kind +
                            " mapping '" + name +
                            "' has invalid source/target scopes",
                        location(region, operation));
                }
            };
        for (const auto& mapping : boundary.scalarMappings) {
            if (mapping.source.type() != mapping.target.type()) {
                diagnostics_.error(
                    DiagCode::TypeMismatch,
                    "GraphCompiler: boundary op '" +
                        boundary.authoredId +
                        "' scalar mapping type mismatch",
                    location(region, operation));
            }
            validateScopes(mapping.source.scopeId(),
                           mapping.target.scopeId(), "scalar",
                           mapping.source.varName());
        }
        for (const auto& mapping : boundary.bufferMappings) {
            if (!mapping.source.valid() || !mapping.target.valid()) {
                diagnostics_.error(
                    DiagCode::InvalidBoundary,
                    "GraphCompiler: boundary op '" +
                        boundary.authoredId +
                        "' buffer mapping contains an invalid token",
                    location(region, operation));
                continue;
            }
            if (mapping.source.type() != mapping.target.type()) {
                diagnostics_.error(
                    DiagCode::TypeMismatch,
                    "GraphCompiler: boundary op '" +
                        boundary.authoredId +
                        "' buffer mapping type mismatch",
                    location(region, operation));
            }
            if (!mapping.source.hasSizeScalar() &&
                !mapping.target.hasSizeScalar()) {
                diagnostics_.error(
                    DiagCode::SizeMismatch,
                    "GraphCompiler: boundary op '" +
                        boundary.authoredId +
                        "' buffer mapping has no size scalar on either side",
                    location(region, operation));
            }
            validateScopes(mapping.source.scopeId(),
                           mapping.target.scopeId(), "buffer",
                           mapping.source.name());
        }
    }

    void validateBufferSize(const AuthoredRegion& region,
                            const AuthoredOperation& operation,
                            const GraphBuffer& buffer,
                            const std::string& port) {
        if (!buffer.valid()) return;
        if (!buffer.hasSizeScalar()) {
            diagnostics_.error(
                DiagCode::SizeMismatch,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' buffer '" + buffer.name() +
                    "' has no size scalar",
                location(region, operation, port));
            return;
        }
        const GraphScalar& size = buffer.sizeScalar();
        const auto& declared = authored_->root().declaredInputScalars;
        auto it = declared.find(size.varName());
        if (size.scopeId() != rootSourceScope_ ||
            it == declared.end() || it->second != ScalarType::U64) {
            diagnostics_.error(
                DiagCode::SizeMismatch,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' buffer '" + buffer.name() +
                    "' size scalar must be a root U64 graph input",
                location(region, operation, port));
        }
    }

    void validateScopesAndSizes(const AuthoredRegion& region,
                                const AuthoredOperation& operation) {
        const IOMap* ioMap = operationIoMap(operation);
        if (!ioMap) return;

        auto scalarScopeAllowed = [&](const GraphScalar& scalar,
                                      bool output,
                                      const std::string& port) {
            const bool allowed =
                scalar.scopeId() == region.sourceScopeId ||
                (!output && scalar.scopeId() == rootSourceScope_);
            if (!allowed) {
                diagnostics_.error(
                    DiagCode::InvalidScope,
                    "GraphCompiler: op '" +
                        authoredSourceId(operation) + "' uses scalar '" +
                        scalar.varName() + "' from an invalid scope",
                    location(region, operation, port));
            }
            if (scalar.scopeId() == rootSourceScope_) {
                auto it = authored_->root().declaredScalars.find(
                    scalar.varName());
                if (it == authored_->root().declaredScalars.end()) {
                    diagnostics_.error(
                        DiagCode::InvalidScope,
                        "GraphCompiler: global scalar '" +
                            scalar.varName() + "' is not declared",
                        location(region, operation, port));
                } else if (it->second != scalar.type()) {
                    diagnostics_.error(
                        DiagCode::TypeMismatch,
                        "GraphCompiler: global scalar '" +
                            scalar.varName() +
                            "' does not match its declared type",
                        location(region, operation, port));
                }
            }
        };
        auto bufferScopeAllowed = [&](const GraphBuffer& buffer,
                                      const std::string& port) {
            if (buffer.scopeId() != region.sourceScopeId) {
                diagnostics_.error(
                    DiagCode::InvalidScope,
                    "GraphCompiler: op '" +
                        authoredSourceId(operation) + "' uses buffer '" +
                        buffer.name() + "' from an invalid scope",
                    location(region, operation, port));
            }
            validateBufferSize(region, operation, buffer, port);
        };

        for (const auto& [port, scalar] : ioMap->inputScalars()) {
            scalarScopeAllowed(scalar, false, port);
        }
        for (const auto& [port, scalar] : ioMap->outputScalars()) {
            scalarScopeAllowed(scalar, true, port);
        }
        for (const auto& [port, buffer] : ioMap->inputs()) {
            bufferScopeAllowed(buffer, port);
        }
        for (const auto& [port, buffer] : ioMap->outputs()) {
            bufferScopeAllowed(buffer, port);
        }
        for (const auto& binding : ioMap->inouts()) {
            bufferScopeAllowed(binding.in, binding.inPort);
            bufferScopeAllowed(binding.out, binding.outPort);
        }

        auto conditionScope = [&](const Condition& condition) {
            auto check = [&](const std::optional<ConditionOperand>& operand) {
                if (!operand || !operand->isScalar()) return;
                if (operand->scopeId() != region.sourceScopeId) {
                    diagnostics_.error(
                        DiagCode::InvalidScope,
                        "GraphCompiler: op '" +
                            authoredSourceId(operation) +
                            "' condition scalar '" + operand->name() +
                            "' is not in the control region",
                        location(region, operation, "condition"));
                }
            };
            check(condition.lhs());
            check(condition.rhs());
            check(condition.epsilon());
        };

        if (const auto* loop = std::get_if<AuthoredLoop>(&operation)) {
            if (loop->tripCount &&
                loop->tripCount->scopeId() != region.sourceScopeId) {
                diagnostics_.error(
                    DiagCode::InvalidScope,
                    "GraphCompiler: loop op '" + loop->authoredId +
                        "' trip-count scalar is not in the loop region",
                    location(region, operation, "trip_count"));
            }
            if (loop->condition) conditionScope(*loop->condition);
        } else if (const auto* conditional =
                       std::get_if<AuthoredConditional>(&operation)) {
            conditionScope(conditional->condition);
        }
    }

    void validateImageSafety(const AuthoredRegion& region) {
        std::map<NodeId, const AuthoredOperation*> byId;
        for (const AuthoredOperation& operation : region.operations) {
            byId[authoredNodeId(operation)] = &operation;
        }
        for (const AuthoredOperation& operation : region.operations) {
            const auto* kernel = std::get_if<AuthoredKernel>(&operation);
            if (!kernel || kernel->kernel.type != DeviceType::FPGA ||
                !kernel->kernel.image) {
                continue;
            }
            bool gated = false;
            for (const AuthoredDependency& dependency : kernel->after) {
                if (!dependency.target) continue;
                auto it = byId.find(*dependency.target);
                if (it == byId.end()) continue;
                const auto* reprogram =
                    std::get_if<AuthoredReprogram>(it->second);
                if (reprogram &&
                    reprogram->imageId == *kernel->kernel.image) {
                    gated = true;
                    break;
                }
            }
            if (!gated) {
                diagnostics_.error(
                    DiagCode::ImageSafetyViolation,
                    "GraphCompiler: FPGA kernel '" + kernel->kernel.name +
                        "' (op '" + kernel->authoredId +
                        "') is not gated behind reprogram of image '" +
                        *kernel->kernel.image + "'",
                    location(region, operation));
            }
        }
    }

    ValueId createValue(const TokenRef& token, RegionId region,
                        ValueDefinitionKind definition,
                        std::optional<NodeId> producer) {
        const ValueId id(nextValue_++);
        ResolvedValue value;
        value.id = id;
        value.region = region;
        value.type = token.type;
        value.sourceName = token.key.name;
        value.definition = definition;
        value.producer = producer;
        value.bufferToken = token.buffer;
        value.scalarToken = token.scalar;
        if (token.size) {
            const TokenKey sizeKey = keyOf(*token.size);
            auto it = rootInputValues_.find(sizeKey);
            if (it != rootInputValues_.end()) value.size = it->second;
        }
        values_.emplace(id, std::move(value));
        return id;
    }

    static bool isControl(const AuthoredOperation& operation) {
        return std::holds_alternative<AuthoredLoop>(operation) ||
               std::holds_alternative<AuthoredConditional>(operation);
    }

    RegionResolution resolveRegion(const AuthoredRegion& region,
                                   const RegionContext& context,
                                   bool rootRegion) {
        auto resolvedRegion = std::make_shared<ResolvedRegion>();
        resolvedRegion->id = region.id;
        resolvedRegion->parent = region.parent;

        validateImageSafety(region);
        for (const AuthoredOperation& operation : region.operations) {
            validateIoPorts(region, operation);
            validateScopesAndSizes(region, operation);
            if (const auto* boundary =
                    std::get_if<AuthoredBoundary>(&operation)) {
                validateBoundary(region, operation, *boundary);
            }
            for (const AuthoredDependency& dependency :
                 operationAfter(operation)) {
                if (!dependency.target) {
                    diagnostics_.error(
                        DiagCode::UnknownDependency,
                        "GraphCompiler: op '" +
                            authoredSourceId(operation) +
                            "' references unknown afterOps id '" +
                            dependency.authoredId + "'",
                        location(region, operation));
                } else if (*dependency.target ==
                           authoredNodeId(operation)) {
                    diagnostics_.error(
                        DiagCode::Cycle,
                        "GraphCompiler: op '" +
                            authoredSourceId(operation) +
                            "' cannot depend on itself",
                        location(region, operation));
                }
            }
            for (const AuthoredRegion* child :
                 childRegions(operation)) {
                if (child->sourceParentScopeId !=
                    region.sourceScopeId) {
                    diagnostics_.error(
                        DiagCode::InvalidBoundary,
                        "GraphCompiler: control op '" +
                            authoredSourceId(operation) +
                            "' references a child with the wrong parent scope",
                        location(region, operation));
                }
            }
        }

        std::map<NodeId, std::set<TokenKey>> carriedByControl;
        std::map<NodeId, std::vector<TokenRef>> outputsByNode;
        std::map<TokenKey, std::vector<NodeId>> producerCandidates;
        std::map<OutputKey, TokenRef> outputDescriptions;

        for (const AuthoredOperation& operation : region.operations) {
            const NodeId node = authoredNodeId(operation);
            carriedByControl[node] = loopCarriedValues(operation);
            outputsByNode[node] = producedValues(operation);
            for (const TokenRef& output : outputsByNode[node]) {
                producerCandidates[output.key].push_back(node);
                outputDescriptions.emplace(
                    OutputKey{node, output.key}, output);
            }
        }

        std::map<TokenKey, ValueId> initialValues;
        for (const auto& [targetKey, link] : context.parameters) {
            const ValueId value =
                createValue(link.target, region.id,
                            ValueDefinitionKind::RegionParameter,
                            std::nullopt);
            initialValues[targetKey] = value;
            resolvedRegion->parameters.push_back(value);
        }

        if (rootRegion) {
            for (const auto& [name, type] : region.declaredScalars) {
                const TokenKey key{
                    ValueKind::Scalar, region.sourceScopeId, name};
                bool carried = false;
                auto producers = producerCandidates.find(key);
                if (producers != producerCandidates.end()) {
                    carried = std::any_of(
                        producers->second.begin(), producers->second.end(),
                        [&](NodeId producer) {
                            return carriedByControl[producer].count(key) != 0;
                        });
                }
                const bool explicitInput =
                    region.declaredInputScalars.count(name) != 0;
                if (producers == producerCandidates.end() ||
                    explicitInput || carried) {
                    const TokenRef token{
                        key, ValueType::scalarType(type), std::nullopt,
                        std::nullopt,
                        GraphScalar::ref(
                            type, name, region.sourceScopeId),
                        name, ValueAccess::Input};
                    const ValueId value =
                        createValue(token, region.id,
                                    ValueDefinitionKind::GraphInput,
                                    std::nullopt);
                    initialValues[key] = value;
                    rootInputValues_[key] = value;
                }
            }
            for (const auto& [name, buffer] :
                 region.declaredInputBuffers) {
                const TokenRef token =
                    inputRef(buffer, name, ValueAccess::Input);
                const ValueId value =
                    createValue(token, region.id,
                                ValueDefinitionKind::GraphInput,
                                std::nullopt);
                initialValues[token.key] = value;
                rootInputValues_[token.key] = value;
            }
        }

        std::map<OutputKey, ValueId> outputValues;
        for (const AuthoredOperation& operation : region.operations) {
            const NodeId node = authoredNodeId(operation);
            for (const TokenRef& output : outputsByNode[node]) {
                const ValueDefinitionKind definition =
                    isControl(operation)
                        ? ValueDefinitionKind::ControlResult
                        : ValueDefinitionKind::OperationOutput;
                outputValues[{node, output.key}] =
                    createValue(output, region.id, definition, node);
            }
        }

        std::map<TokenKey, ValueId> finalValues = initialValues;
        std::map<OutputKey, ValueId> controlInitialValues;
        for (auto& [key, candidates] : producerCandidates) {
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(
                std::unique(candidates.begin(), candidates.end()),
                candidates.end());

            std::vector<NodeId> carried;
            for (NodeId candidate : candidates) {
                if (carriedByControl[candidate].count(key)) {
                    carried.push_back(candidate);
                }
            }

            NodeId finalProducer = candidates.front();
            if (candidates.size() > 1) {
                if (candidates.size() == 2 && carried.size() == 1) {
                    finalProducer = carried.front();
                    const NodeId initialProducer =
                        candidates[0] == finalProducer ? candidates[1]
                                                       : candidates[0];
                    controlInitialValues[{finalProducer, key}] =
                        outputValues.at({initialProducer, key});
                } else {
                    diagnostics_.error(
                        DiagCode::DuplicateProducer,
                        "GraphCompiler: multiple operations produce value '" +
                            key.name + "' in one region");
                }
            } else if (carried.size() == 1) {
                auto initial = initialValues.find(key);
                if (initial != initialValues.end()) {
                    controlInitialValues[{finalProducer, key}] =
                        initial->second;
                }
            }
            finalValues[key] =
                outputValues.at({finalProducer, key});
        }

        for (const auto& [key, candidates] : producerCandidates) {
            if (candidates.empty()) continue;
            const TokenRef& expected =
                outputDescriptions.at({candidates.front(), key});
            for (NodeId candidate : candidates) {
                const TokenRef& actual =
                    outputDescriptions.at({candidate, key});
                if (!(actual.type == expected.type)) {
                    diagnostics_.error(
                        DiagCode::TypeMismatch,
                        "GraphCompiler: producers of value '" + key.name +
                            "' disagree on its type");
                }
            }
        }

        auto valueForUse =
            [&](const AuthoredOperation& operation,
                const TokenRef& token) -> std::optional<ValueId> {
                const NodeId node = authoredNodeId(operation);
                if (carriedByControl[node].count(token.key)) {
                    auto initial =
                        controlInitialValues.find({node, token.key});
                    if (initial != controlInitialValues.end()) {
                        return initial->second;
                    }
                    diagnostics_.error(
                        DiagCode::InvalidControlResult,
                        "GraphCompiler: loop op '" +
                            authoredSourceId(operation) +
                            "' has no initial value for carried token '" +
                            token.key.name + "'",
                        location(region, operation, token.port));
                    return std::nullopt;
                }
                auto ownOutput =
                    outputValues.find({node, token.key});
                if (token.access == ValueAccess::InoutInput &&
                    ownOutput != outputValues.end()) {
                    auto initial = initialValues.find(token.key);
                    if (initial != initialValues.end()) {
                        return initial->second;
                    }
                }
                if (rootRegion) {
                    const bool declaredBufferInput =
                        token.key.kind == ValueKind::Buffer &&
                        region.declaredInputBuffers.count(
                            token.key.name) != 0;
                    const bool declaredScalarInput =
                        token.key.kind == ValueKind::Scalar &&
                        region.declaredInputScalars.count(
                            token.key.name) != 0;
                    if (declaredBufferInput || declaredScalarInput) {
                        auto initial = initialValues.find(token.key);
                        if (initial != initialValues.end()) {
                            return initial->second;
                        }
                    }
                }
                auto value = finalValues.find(token.key);
                if (value != finalValues.end()) return value->second;
                if (token.key.kind == ValueKind::Scalar &&
                    token.key.scope == rootSourceScope_) {
                    auto rootValue = rootInputValues_.find(token.key);
                    if (rootValue != rootInputValues_.end()) {
                        return rootValue->second;
                    }
                }
                diagnostics_.error(
                    DiagCode::InvalidScope,
                    "GraphCompiler: op '" +
                        authoredSourceId(operation) + "' consumes " +
                        std::string(token.key.kind == ValueKind::Buffer
                                        ? "buffer '"
                                        : "scalar '") +
                        token.key.name + "' with no producer",
                    location(region, operation, token.port));
                return std::nullopt;
            };

        auto addDependency = [&](ResolvedOperation& operation,
                                 ValueId value) {
            const ResolvedValue& resolved = values_.at(value);
            if (resolved.producer &&
                *resolved.producer != operation.id) {
                operation.dependencies.push_back(*resolved.producer);
            }
        };

        for (const AuthoredOperation& authoredOperation :
             region.operations) {
            ResolvedOperation operation;
            operation.id = authoredNodeId(authoredOperation);
            operation.region = region.id;
            operation.kind = operationKind(authoredOperation);
            operation.structural =
                operation.kind == ResolvedOperationKind::Boundary;

            for (const AuthoredDependency& dependency :
                 operationAfter(authoredOperation)) {
                if (dependency.target) {
                    operation.dependencies.push_back(*dependency.target);
                }
            }

            if (const auto* boundary =
                    std::get_if<AuthoredBoundary>(&authoredOperation)) {
                if (boundary->side == BoundarySide::Start) {
                    for (std::size_t i = 0;
                         i < boundary->scalarMappings.size(); ++i) {
                        const auto& mapping =
                            boundary->scalarMappings[i];
                        const TokenKey target = keyOf(mapping.target);
                        auto link = context.parameters.find(target);
                        auto value = initialValues.find(target);
                        if (link == context.parameters.end() ||
                            value == initialValues.end()) {
                            diagnostics_.error(
                                DiagCode::InvalidBoundary,
                                "GraphCompiler: boundary op '" +
                                    boundary->authoredId +
                                    "' has no parent parameter binding",
                                location(region, authoredOperation));
                            continue;
                        }
                        operation.bindings.push_back(
                            {"scalar." + std::to_string(i),
                             link->second.source,
                             ValueAccess::BoundarySource});
                        operation.bindings.push_back(
                            {"scalar." + std::to_string(i),
                             value->second,
                             ValueAccess::BoundaryTarget});
                    }
                    for (std::size_t i = 0;
                         i < boundary->bufferMappings.size(); ++i) {
                        const auto& mapping =
                            boundary->bufferMappings[i];
                        const TokenKey target = keyOf(mapping.target);
                        auto link = context.parameters.find(target);
                        auto value = initialValues.find(target);
                        if (link == context.parameters.end() ||
                            value == initialValues.end()) {
                            diagnostics_.error(
                                DiagCode::InvalidBoundary,
                                "GraphCompiler: boundary op '" +
                                    boundary->authoredId +
                                    "' has no parent parameter binding",
                                location(region, authoredOperation));
                            continue;
                        }
                        operation.bindings.push_back(
                            {"buffer." + std::to_string(i),
                             link->second.source,
                             ValueAccess::BoundarySource});
                        operation.bindings.push_back(
                            {"buffer." + std::to_string(i),
                             value->second,
                             ValueAccess::BoundaryTarget});
                    }
                } else {
                    auto addResult =
                        [&](const TokenRef& source,
                            const TokenKey& target,
                            const std::string& port) {
                            auto sourceValue =
                                finalValues.find(source.key);
                            auto targetValue =
                                context.resultTargets.find(target);
                            if (sourceValue == finalValues.end() ||
                                targetValue ==
                                    context.resultTargets.end()) {
                                diagnostics_.error(
                                    DiagCode::InvalidControlResult,
                                    "GraphCompiler: boundary op '" +
                                        boundary->authoredId +
                                        "' cannot resolve its result mapping",
                                    location(region, authoredOperation,
                                             port));
                                return;
                            }
                            operation.bindings.push_back(
                                {port, sourceValue->second,
                                 ValueAccess::BoundarySource});
                            operation.bindings.push_back(
                                {port, targetValue->second,
                                 ValueAccess::BoundaryTarget});
                            addDependency(operation,
                                          sourceValue->second);
                            resolvedRegion->results.push_back(
                                sourceValue->second);
                        };
                    for (std::size_t i = 0;
                         i < boundary->scalarMappings.size(); ++i) {
                        const auto& mapping =
                            boundary->scalarMappings[i];
                        addResult(
                            inputRef(mapping.source, ""),
                            keyOf(mapping.target),
                            "scalar." + std::to_string(i));
                    }
                    for (std::size_t i = 0;
                         i < boundary->bufferMappings.size(); ++i) {
                        const auto& mapping =
                            boundary->bufferMappings[i];
                        addResult(
                            inputRef(mapping.source, ""),
                            keyOf(mapping.target),
                            "buffer." + std::to_string(i));
                    }
                }
            } else {
                if (const IOMap* ioMap =
                        operationIoMap(authoredOperation)) {
                    for (const TokenRef& input : ioInputs(*ioMap)) {
                        if (auto value =
                                valueForUse(authoredOperation, input)) {
                            operation.bindings.push_back(
                                {input.port, *value, input.access});
                            addDependency(operation, *value);
                        }
                    }
                    for (const TokenRef& output :
                         ioOutputs(*ioMap)) {
                        auto value = outputValues.find(
                            {operation.id, output.key});
                        if (value != outputValues.end()) {
                            operation.bindings.push_back(
                                {output.port, value->second,
                                 output.access});
                        }
                    }
                }

                for (const TokenRef& input :
                     controlBoundaryInputs(authoredOperation)) {
                    if (auto value =
                            valueForUse(authoredOperation, input)) {
                        operation.bindings.push_back(
                            {input.port, *value,
                             ValueAccess::BoundarySource});
                        addDependency(operation, *value);
                    }
                }
                for (const TokenRef& output :
                     controlBoundaryOutputs(authoredOperation)) {
                    auto value = outputValues.find(
                        {operation.id, output.key});
                    if (value != outputValues.end()) {
                        operation.bindings.push_back(
                            {output.port, value->second,
                             ValueAccess::BoundaryTarget});
                    }
                }

                if (const auto* loop =
                        std::get_if<AuthoredLoop>(&authoredOperation)) {
                    if (loop->tripCount) {
                        TokenRef trip = inputRef(
                            GraphScalar::ref(
                                loop->tripCount->type(),
                                loop->tripCount->name(),
                                loop->tripCount->scopeId()),
                            "trip_count", ValueAccess::TripCount);
                        if (auto value =
                                valueForUse(authoredOperation, trip)) {
                            operation.bindings.push_back(
                                {trip.port, *value, trip.access});
                            addDependency(operation, *value);
                        }
                    }
                    if (loop->condition) {
                        addConditionBindings(
                            region, authoredOperation,
                            *loop->condition, operation,
                            valueForUse, addDependency);
                    }
                } else if (const auto* conditional =
                               std::get_if<AuthoredConditional>(
                                   &authoredOperation)) {
                    addConditionBindings(
                        region, authoredOperation,
                        conditional->condition, operation,
                        valueForUse, addDependency);
                }
            }

            std::sort(operation.dependencies.begin(),
                      operation.dependencies.end());
            operation.dependencies.erase(
                std::unique(operation.dependencies.begin(),
                            operation.dependencies.end()),
                operation.dependencies.end());
            operations_[operation.id] = std::move(operation);
        }

        resolvedRegion->topologicalOrder =
            topologicalOrder(region);

        for (const AuthoredOperation& operation :
             region.operations) {
            const NodeId control = authoredNodeId(operation);
            if (const auto* loop =
                    std::get_if<AuthoredLoop>(&operation)) {
                if (!loop->body) continue;
                RegionContext childContext =
                    makeChildContext(region, operation, *loop->body,
                                     ControlArm::LoopBackedge,
                                     finalValues,
                                     controlInitialValues,
                                     outputValues,
                                     carriedByControl);
                RegionResolution child =
                    resolveRegion(*loop->body, childContext, false);
                addControlIncoming(
                    operation, *loop->body, child, childContext,
                    ControlArm::LoopBackedge,
                    controlInitialValues);
                resolvedRegion->children.push_back(
                    std::move(child.region));
            } else if (const auto* conditional =
                           std::get_if<AuthoredConditional>(&operation)) {
                if (conditional->thenRegion) {
                    RegionContext thenContext =
                        makeChildContext(
                            region, operation,
                            *conditional->thenRegion,
                            ControlArm::ThenBranch, finalValues,
                            controlInitialValues, outputValues,
                            carriedByControl);
                    RegionResolution thenResolution =
                        resolveRegion(*conditional->thenRegion,
                                      thenContext, false);
                    addControlIncoming(
                        operation, *conditional->thenRegion,
                        thenResolution, thenContext,
                        ControlArm::ThenBranch,
                        controlInitialValues);
                    resolvedRegion->children.push_back(
                        std::move(thenResolution.region));
                }
                if (conditional->elseRegion) {
                    RegionContext elseContext =
                        makeChildContext(
                            region, operation,
                            *conditional->elseRegion,
                            ControlArm::ElseBranch, finalValues,
                            controlInitialValues, outputValues,
                            carriedByControl);
                    RegionResolution elseResolution =
                        resolveRegion(*conditional->elseRegion,
                                      elseContext, false);
                    addControlIncoming(
                        operation, *conditional->elseRegion,
                        elseResolution, elseContext,
                        ControlArm::ElseBranch,
                        controlInitialValues);
                    resolvedRegion->children.push_back(
                        std::move(elseResolution.region));
                }
            }
        }

        if (rootRegion) {
            auto markOutput = [&](const TokenKey& key,
                                  const std::string& kind) {
                auto value = finalValues.find(key);
                if (value == finalValues.end()) {
                    diagnostics_.error(
                        DiagCode::InvalidControlResult,
                        "GraphCompiler: graph output " + kind + " '" +
                            key.name + "' has no producer");
                    return;
                }
                values_.at(value->second).graphOutput = true;
            };
            for (const auto& [name, buffer] :
                 region.declaredOutputBuffers) {
                (void)name;
                markOutput(keyOf(buffer), "buffer");
            }
            for (const auto& [name, type] :
                 region.declaredOutputScalars) {
                markOutput(
                    TokenKey{ValueKind::Scalar,
                             region.sourceScopeId, name},
                    "scalar");
                (void)type;
            }
        }

        return {std::move(resolvedRegion), std::move(finalValues)};
    }

    template <class ValueLookup, class DependencyAdder>
    void addConditionBindings(
        const AuthoredRegion& region,
        const AuthoredOperation& authoredOperation,
        const Condition& condition,
        ResolvedOperation& operation,
        ValueLookup&& valueForUse,
        DependencyAdder&& addDependency) {
        auto addOperand =
            [&](const std::optional<ConditionOperand>& operand,
                const std::string& port) {
                if (!operand || !operand->isScalar()) return;
                TokenRef token = inputRef(
                    GraphScalar::ref(operand->type(), operand->name(),
                                     operand->scopeId()),
                    port, ValueAccess::Condition);
                if (auto value =
                        valueForUse(authoredOperation, token)) {
                    operation.bindings.push_back(
                        {port, *value, ValueAccess::Condition});
                    addDependency(operation, *value);
                }
            };
        addOperand(condition.lhs(), "condition.lhs");
        addOperand(condition.rhs(), "condition.rhs");
        addOperand(condition.epsilon(), "condition.epsilon");
        (void)region;
    }

    RegionContext makeChildContext(
        const AuthoredRegion& parent,
        const AuthoredOperation& operation,
        const AuthoredRegion& child,
        ControlArm arm,
        const std::map<TokenKey, ValueId>& finalValues,
        const std::map<OutputKey, ValueId>& controlInitialValues,
        const std::map<OutputKey, ValueId>& outputValues,
        const std::map<NodeId, std::set<TokenKey>>& carriedByControl) {
        RegionContext context;
        context.control = authoredNodeId(operation);
        context.arm = arm;
        const NodeId control = *context.control;

        auto parentValue = [&](const TokenRef& source)
            -> std::optional<ValueId> {
            if (carriedByControl.at(control).count(source.key)) {
                auto initial =
                    controlInitialValues.find({control, source.key});
                if (initial != controlInitialValues.end()) {
                    return initial->second;
                }
            }
            auto value = finalValues.find(source.key);
            if (value != finalValues.end()) return value->second;
            if (source.key.kind == ValueKind::Scalar &&
                source.key.scope == rootSourceScope_) {
                auto root = rootInputValues_.find(source.key);
                if (root != rootInputValues_.end()) {
                    return root->second;
                }
            }
            diagnostics_.error(
                DiagCode::InvalidScope,
                "GraphCompiler: control op '" +
                    authoredSourceId(operation) +
                    "' cannot resolve child input '" +
                    source.key.name + "'",
                location(parent, operation, source.port));
            return std::nullopt;
        };

        for (const AuthoredBoundary* boundary :
             boundaries(child, BoundarySide::Start)) {
            for (std::size_t i = 0;
                 i < boundary->scalarMappings.size(); ++i) {
                const auto& mapping =
                    boundary->scalarMappings[i];
                TokenRef source = inputRef(
                    mapping.source,
                    "boundary.scalar." + std::to_string(i));
                if (auto value = parentValue(source)) {
                    context.parameters[keyOf(mapping.target)] =
                        ParameterLink{
                            *value,
                            outputRef(mapping.target,
                                      source.port,
                                      ValueAccess::BoundaryTarget)};
                }
            }
            for (std::size_t i = 0;
                 i < boundary->bufferMappings.size(); ++i) {
                const auto& mapping =
                    boundary->bufferMappings[i];
                TokenRef source = inputRef(
                    mapping.source,
                    "boundary.buffer." + std::to_string(i));
                if (auto value = parentValue(source)) {
                    context.parameters[keyOf(mapping.target)] =
                        ParameterLink{
                            *value,
                            outputRef(mapping.target,
                                      source.port,
                                      ValueAccess::BoundaryTarget)};
                }
            }
        }

        for (const AuthoredBoundary* boundary :
             boundaries(child, BoundarySide::End)) {
            for (const auto& mapping : boundary->scalarMappings) {
                const TokenKey target = keyOf(mapping.target);
                auto result = outputValues.find({control, target});
                if (result != outputValues.end()) {
                    context.resultTargets[target] = result->second;
                }
            }
            for (const auto& mapping : boundary->bufferMappings) {
                const TokenKey target = keyOf(mapping.target);
                auto result = outputValues.find({control, target});
                if (result != outputValues.end()) {
                    context.resultTargets[target] = result->second;
                }
            }
        }
        if (const IOMap* ioMap = operationIoMap(operation)) {
            for (const auto& [port, scalar] : ioMap->outputScalars()) {
                (void)port;
                const TokenKey target = keyOf(scalar);
                auto result = outputValues.find({control, target});
                if (result != outputValues.end()) {
                    context.resultTargets[target] = result->second;
                }
            }
            for (const auto& [port, buffer] : ioMap->outputs()) {
                (void)port;
                const TokenKey target = keyOf(buffer);
                auto result = outputValues.find({control, target});
                if (result != outputValues.end()) {
                    context.resultTargets[target] = result->second;
                }
            }
            for (const auto& inout : ioMap->inouts()) {
                const TokenKey target = keyOf(inout.out);
                auto result = outputValues.find({control, target});
                if (result != outputValues.end()) {
                    context.resultTargets[target] = result->second;
                }
            }
        }
        return context;
    }

    void addControlIncoming(
        const AuthoredOperation& operation,
        const AuthoredRegion& child,
        const RegionResolution& childResolution,
        const RegionContext& context,
        ControlArm arm,
        const std::map<OutputKey, ValueId>& controlInitialValues) {
        const NodeId control = authoredNodeId(operation);
        for (const AuthoredBoundary* boundary :
             boundaries(child, BoundarySide::End)) {
            auto add = [&](const TokenKey& source,
                           const TokenKey& target) {
                auto sourceValue =
                    childResolution.finalValues.find(source);
                auto resultValue =
                    context.resultTargets.find(target);
                if (sourceValue == childResolution.finalValues.end() ||
                    resultValue == context.resultTargets.end()) {
                    return;
                }
                ResolvedControlResult& result =
                    controlResult(control, resultValue->second);
                result.incoming.push_back(
                    {arm, child.id, sourceValue->second});
                if (arm == ControlArm::LoopBackedge) {
                    auto initial =
                        controlInitialValues.find({control, target});
                    if (initial != controlInitialValues.end() &&
                        std::none_of(
                            result.incoming.begin(),
                            result.incoming.end(),
                            [](const ControlIncoming& incoming) {
                                return incoming.arm ==
                                       ControlArm::LoopInitial;
                            })) {
                        result.incoming.push_back(
                            {ControlArm::LoopInitial,
                             values_.at(initial->second).region,
                             initial->second});
                    }
                }
            };
            for (const auto& mapping : boundary->scalarMappings) {
                add(keyOf(mapping.source), keyOf(mapping.target));
            }
            for (const auto& mapping : boundary->bufferMappings) {
                add(keyOf(mapping.source), keyOf(mapping.target));
            }
        }

        const IOMap* controlIo = operationIoMap(operation);
        if (!controlIo) return;
        auto addImplicit =
            [&](const std::string& port, const TokenKey& target) {
                std::vector<TokenKey> candidates;
                for (const AuthoredOperation& childOperation :
                     child.operations) {
                    const IOMap* childIo =
                        operationIoMap(childOperation);
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
                        auto output =
                            childIo->outputScalars().find(port);
                        if (output !=
                            childIo->outputScalars().end()) {
                            candidates.push_back(keyOf(output->second));
                        }
                    }
                }
                std::sort(candidates.begin(), candidates.end());
                candidates.erase(
                    std::unique(candidates.begin(),
                                candidates.end()),
                    candidates.end());
                if (candidates.size() != 1) {
                    diagnostics_.error(
                        DiagCode::InvalidControlResult,
                        "GraphCompiler: control output port '" + port +
                            "' has " +
                            (candidates.empty()
                                 ? "no body producer"
                                 : "multiple body producers"));
                    return;
                }
                auto source =
                    childResolution.finalValues.find(candidates.front());
                auto result = context.resultTargets.find(target);
                if (source == childResolution.finalValues.end() ||
                    result == context.resultTargets.end()) {
                    return;
                }
                ResolvedControlResult& controlResultValue =
                    controlResult(control, result->second);
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
            };
        for (const auto& [port, scalar] :
             controlIo->outputScalars()) {
            addImplicit(port, keyOf(scalar));
        }
        for (const auto& [port, buffer] : controlIo->outputs()) {
            addImplicit(port, keyOf(buffer));
        }
        for (const auto& inout : controlIo->inouts()) {
            addImplicit(inout.outPort, keyOf(inout.out));
        }
    }

    ResolvedControlResult& controlResult(NodeId control,
                                         ValueId result) {
        const auto key = std::make_pair(control, result);
        auto it = controlResultIndexes_.find(key);
        if (it != controlResultIndexes_.end()) {
            return controlResults_[it->second];
        }
        const std::size_t index = controlResults_.size();
        controlResultIndexes_[key] = index;
        controlResults_.push_back(
            ResolvedControlResult{control, result, {}});
        return controlResults_.back();
    }

    std::vector<NodeId> topologicalOrder(
        const AuthoredRegion& region) {
        std::set<NodeId> local;
        for (const AuthoredOperation& operation : region.operations) {
            local.insert(authoredNodeId(operation));
        }

        std::map<NodeId, std::size_t> indegree;
        std::map<NodeId, std::vector<NodeId>> successors;
        for (NodeId node : local) indegree[node] = 0;
        for (NodeId node : local) {
            const ResolvedOperation& operation =
                operations_.at(node);
            for (NodeId dependency : operation.dependencies) {
                if (!local.count(dependency)) continue;
                successors[dependency].push_back(node);
                ++indegree[node];
            }
        }

        std::priority_queue<NodeId, std::vector<NodeId>,
                            std::greater<NodeId>> ready;
        for (const auto& [node, degree] : indegree) {
            if (degree == 0) ready.push(node);
        }

        std::vector<NodeId> result;
        while (!ready.empty()) {
            const NodeId node = ready.top();
            ready.pop();
            result.push_back(node);
            for (NodeId successor : successors[node]) {
                if (--indegree[successor] == 0) {
                    ready.push(successor);
                }
            }
        }
        if (result.size() != local.size()) {
            diagnostics_.error(
                DiagCode::Cycle,
                "GraphCompiler: cycle detected in region " +
                    std::to_string(region.id.value()));
        }
        return result;
    }

    std::shared_ptr<const AuthoredGraph> authored_;
    Diagnostics diagnostics_;
    RegionId rootRegionId_;
    std::uint64_t rootSourceScope_ = 0;
    std::uint64_t nextValue_ = 0;
    std::map<std::uint64_t, RegionId> regionsBySourceScope_;
    std::map<NodeId, const AuthoredOperation*> authoredOperations_;
    std::map<TokenKey, ValueId> rootInputValues_;
    std::map<ValueId, ResolvedValue> values_;
    std::map<NodeId, ResolvedOperation> operations_;
    std::vector<ResolvedControlResult> controlResults_;
    std::map<std::pair<NodeId, ValueId>, std::size_t>
        controlResultIndexes_;
};

}  // namespace

CompileResult<ResolvedGraph> resolveGraph(
    const AuthoredGraph& authored) {
    return GraphResolver().resolve(authored);
}

}  // namespace vrt::graph
