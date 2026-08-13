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
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace vrt::graph::resolve_detail {

namespace {

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

bool hasDeclaredPorts(const IOTypeMap& ioType) {
    return !ioType.inputScalars.empty() ||
           !ioType.outputScalars.empty() ||
           !ioType.inputs.empty() ||
           !ioType.outputs.empty() ||
           !ioType.inouts.empty();
}

template <class Ports>
const typename Ports::value_type* findPort(
    const Ports& ports, const std::string& name) {
    auto it = std::find_if(
        ports.begin(), ports.end(),
        [&](const auto& port) { return port.name == name; });
    return it == ports.end() ? nullptr : &*it;
}

}  // namespace

/*
 * Validate constraints that need the complete region before checking each
 * operation's local ports, scopes, boundaries, and explicit dependencies.
 * Child regions are validated separately by the outer indexed-tree walk.
 */
void GraphValidator::validateRegion(const AuthoredRegion& region) {
    validateImageSafety(region);
    for (const AuthoredOperation& operation : region.operations) {
        validateOperation(region, operation);
    }
}

/*
 * Keep validation facets independent so one malformed binding does not hide
 * other useful diagnostics. Dependencies must name another operation, and
 * child control regions must have been authored directly under this region's
 * source scope.
 */
void GraphValidator::validateOperation(
    const AuthoredRegion& region,
    const AuthoredOperation& operation) {
    validateIoPorts(region, operation);
    validateScopesAndSizes(region, operation);
    if (const auto* boundary =
            std::get_if<AuthoredBoundary>(&operation)) {
        validateBoundary(region, operation, *boundary);
    }
    for (const AuthoredDependency& dependency :
         operationAfter(operation)) {
        if (!dependency.target) {
            state_.diagnostics.error(
                DiagCode::UnknownDependency,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' references unknown afterOps id '" +
                    dependency.authoredId + "'",
                state_.location(region, operation));
        } else if (*dependency.target == authoredNodeId(operation)) {
            state_.diagnostics.error(
                DiagCode::Cycle,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' cannot depend on itself",
                state_.location(region, operation));
        }
    }
    for (const AuthoredRegion* child : childRegions(operation)) {
        if (child->sourceParentScope != region.sourceScope) {
            state_.diagnostics.error(
                DiagCode::InvalidBoundary,
                "GraphCompiler: control op '" +
                    authoredSourceId(operation) +
                    "' references a child with the wrong parent scope",
                state_.location(region, operation));
        }
    }
}

void GraphValidator::validateIoPorts(
    const AuthoredRegion& region,
    const AuthoredOperation& operation) {
    const IOTypeMap* ioType = operationIoType(operation);
    const detail::PortBindings* ioMap = operationIoMap(operation);
    if (!ioType || !ioMap || !hasDeclaredPorts(*ioType)) return;
    validateRequiredPorts(region, operation, *ioType, *ioMap);
    validateBoundPorts(region, operation, *ioType, *ioMap);
}

/*
 * Walk the declared interface to prove every mandatory port is bound with the
 * declared type. This direction catches omissions; validateBoundPorts walks
 * the bindings in reverse to catch names the interface never declared.
 */
void GraphValidator::validateRequiredPorts(
    const AuthoredRegion& region,
    const AuthoredOperation& operation, const IOTypeMap& ioType,
    const detail::PortBindings& ioMap) {
    auto missing = [&](const std::string& port,
                       const std::string& description) {
        state_.diagnostics.error(
            DiagCode::UnboundPort,
            "GraphCompiler: op '" + authoredSourceId(operation) +
                "' missing mandatory " + description + " port '" +
                port + "'",
            state_.location(region, operation, port));
    };
    auto mismatch = [&](const std::string& port,
                        const std::string& description,
                        const std::string& expected,
                        const std::string& actual) {
        state_.diagnostics.error(
            DiagCode::TypeMismatch,
            "GraphCompiler: op '" + authoredSourceId(operation) +
                "' " + description + " '" + port +
                "' type mismatch: declared " + expected +
                ", bound " + actual,
            state_.location(region, operation, port));
    };

    /*
     * Scalar inputs and outputs use separate namespaces, so a name on one
     * side cannot satisfy a declaration on the other.
     */
    for (const auto& expected : ioType.inputScalars) {
        auto it = ioMap.inputScalars().find(expected.name);
        if (it == ioMap.inputScalars().end()) {
            missing(expected.name, "input scalar");
        } else if (it->second.type() != expected.type) {
            mismatch(expected.name, "input scalar",
                     scalarTypeName(expected.type),
                     scalarTypeName(it->second.type()));
        }
    }
    for (const auto& expected : ioType.outputScalars) {
        auto it = ioMap.outputScalars().find(expected.name);
        if (it == ioMap.outputScalars().end()) {
            missing(expected.name, "output scalar");
        } else if (it->second.type() != expected.type) {
            mismatch(expected.name, "output scalar",
                     scalarTypeName(expected.type),
                     scalarTypeName(it->second.type()));
        }
    }

    /*
     * Buffer ports follow the same presence and type rules, but remain
     * separate from scalars even when their authored names match.
     */
    for (const auto& expected : ioType.inputs) {
        auto it = ioMap.inputs().find(expected.name);
        if (it == ioMap.inputs().end()) {
            missing(expected.name, "input buffer");
        } else if (it->second.type() != expected.type) {
            mismatch(expected.name, "input buffer",
                     bufferTypeName(expected.type),
                     bufferTypeName(it->second.type()));
        }
    }
    for (const auto& expected : ioType.outputs) {
        auto it = ioMap.outputs().find(expected.name);
        if (it == ioMap.outputs().end()) {
            missing(expected.name, "output buffer");
        } else if (it->second.type() != expected.type) {
            mismatch(expected.name, "output buffer",
                     bufferTypeName(expected.type),
                     bufferTypeName(it->second.type()));
        }
    }

    /*
     * Read-write buffers are one semantic port pair. Match both endpoint
     * names together before checking each side's element type.
     */
    for (const auto& expected : ioType.inouts) {
        auto it = std::find_if(
            ioMap.inouts().begin(), ioMap.inouts().end(),
            [&](const detail::PortBindings::InoutBinding& binding) {
                return binding.inPort == expected.in.name &&
                       binding.outPort == expected.out.name;
            });
        if (it == ioMap.inouts().end()) {
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
}

/*
 * Walk authored bindings back against the declared interface. Unknown names
 * are rejected even when all mandatory ports were present, and known scalar
 * and buffer ports are type-checked again from the concrete binding side.
 */
void GraphValidator::validateBoundPorts(
    const AuthoredRegion& region,
    const AuthoredOperation& operation, const IOTypeMap& ioType,
    const detail::PortBindings& ioMap) {
    auto mismatch = [&](const std::string& port,
                        const std::string& description,
                        const std::string& expected,
                        const std::string& actual) {
        state_.diagnostics.error(
            DiagCode::TypeMismatch,
            "GraphCompiler: op '" + authoredSourceId(operation) +
                "' " + description + " '" + port +
                "' type mismatch: declared " + expected +
                ", bound " + actual,
            state_.location(region, operation, port));
    };

    /*
     * Check scalar bindings in their direction-specific declaration lists;
     * input and output ports may legally reuse a spelling.
     */
    for (const auto& [port, scalar] : ioMap.inputScalars()) {
        const auto* expected = findPort(ioType.inputScalars, port);
        if (!expected) {
            state_.diagnostics.error(
                DiagCode::UnboundPort,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' binds unknown input scalar port '" + port + "'",
                state_.location(region, operation, port));
        } else if (expected->type != scalar.type()) {
            mismatch(port, "input scalar",
                     scalarTypeName(expected->type),
                     scalarTypeName(scalar.type()));
        }
    }
    for (const auto& [port, scalar] : ioMap.outputScalars()) {
        const auto* expected = findPort(ioType.outputScalars, port);
        if (!expected) {
            state_.diagnostics.error(
                DiagCode::UnboundPort,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' binds unknown output scalar port '" + port + "'",
                state_.location(region, operation, port));
        } else if (expected->type != scalar.type()) {
            mismatch(port, "output scalar",
                     scalarTypeName(expected->type),
                     scalarTypeName(scalar.type()));
        }
    }

    /*
     * Buffer bindings use the same reverse lookup, while read-write bindings
     * below must match their input/output names as one declared pair.
     */
    for (const auto& [port, buffer] : ioMap.inputs()) {
        const auto* expected = findPort(ioType.inputs, port);
        if (!expected) {
            state_.diagnostics.error(
                DiagCode::UnboundPort,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' binds unknown input buffer port '" + port + "'",
                state_.location(region, operation, port));
        } else if (expected->type != buffer.type()) {
            mismatch(port, "input buffer",
                     bufferTypeName(expected->type),
                     bufferTypeName(buffer.type()));
        }
    }
    for (const auto& [port, buffer] : ioMap.outputs()) {
        const auto* expected = findPort(ioType.outputs, port);
        if (!expected) {
            state_.diagnostics.error(
                DiagCode::UnboundPort,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' binds unknown output buffer port '" + port + "'",
                state_.location(region, operation, port));
        } else if (expected->type != buffer.type()) {
            mismatch(port, "output buffer",
                     bufferTypeName(expected->type),
                     bufferTypeName(buffer.type()));
        }
    }

    /*
     * Required-port validation already checked read-write element types.
     * This reverse pass only needs to reject extra or mismatched endpoint
     * pairs that were present in the authored binding list.
     */
    for (const auto& binding : ioMap.inouts()) {
        auto expected = std::find_if(
            ioType.inouts.begin(), ioType.inouts.end(),
            [&](const RWBufferPort& port) {
                return port.in.name == binding.inPort &&
                       port.out.name == binding.outPort;
            });
        if (expected == ioType.inouts.end()) {
            state_.diagnostics.error(
                DiagCode::UnboundPort,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' binds unknown RW buffer ports '" +
                    binding.inPort + "'/'" + binding.outPort + "'",
                state_.location(region, operation, binding.inPort));
        }
    }
}

/*
 * A start boundary maps parent tokens into the child scope; an end boundary
 * reverses that direction. Every mapping must stay in the same graph, preserve
 * its value type, and use those exact source/target scopes. Buffers also need
 * a usable size token on at least one side of the alias.
 */
void GraphValidator::validateBoundary(
    const AuthoredRegion& region,
    const AuthoredOperation& operation,
    const AuthoredBoundary& boundary) {
    const AuthoredScopeId expectedSource =
        boundary.side == BoundarySide::Start
            ? boundary.sourceParentScope
            : boundary.sourceLocalScope;
    const AuthoredScopeId expectedTarget =
        boundary.side == BoundarySide::Start
            ? boundary.sourceLocalScope
            : boundary.sourceParentScope;

    auto validateScopes =
        [&](std::uint64_t source, std::uint64_t target,
            const std::string& kind, const std::string& name) {
            if (AuthoredScopeId(source) != expectedSource ||
                AuthoredScopeId(target) != expectedTarget) {
                state_.diagnostics.error(
                    DiagCode::InvalidBoundary,
                    "GraphCompiler: boundary op '" +
                        boundary.authoredId + "' " + kind +
                        " mapping '" + name +
                        "' has invalid source/target scopes",
                    state_.location(region, operation));
            }
        };

    /*
     * Scalars have no size metadata, so graph ownership, type, and directed
     * scope are the complete boundary contract for them.
     */
    for (const auto& mapping : boundary.scalarMappings) {
        if ((mapping.source.graphId() != 0 &&
             mapping.source.graphId() != region.sourceGraph) ||
            (mapping.target.graphId() != 0 &&
             mapping.target.graphId() != region.sourceGraph)) {
            state_.diagnostics.error(
                DiagCode::InvalidScope,
                "GraphCompiler: boundary op '" + boundary.authoredId +
                    "' scalar mapping uses a token from a different graph",
                state_.location(region, operation));
        }
        if (mapping.source.type() != mapping.target.type()) {
            state_.diagnostics.error(
                DiagCode::TypeMismatch,
                "GraphCompiler: boundary op '" + boundary.authoredId +
                    "' scalar mapping type mismatch",
                state_.location(region, operation));
        }
        validateScopes(mapping.source.scopeId(),
                       mapping.target.scopeId(), "scalar",
                       mapping.source.varName());
    }

    /*
     * Buffers additionally require valid tokens and enough size information
     * to build the resolved alias without inventing a new size value.
     */
    for (const auto& mapping : boundary.bufferMappings) {
        if ((mapping.source.graphId() != 0 &&
             mapping.source.graphId() != region.sourceGraph) ||
            (mapping.target.graphId() != 0 &&
             mapping.target.graphId() != region.sourceGraph)) {
            state_.diagnostics.error(
                DiagCode::InvalidScope,
                "GraphCompiler: boundary op '" + boundary.authoredId +
                    "' buffer mapping uses a token from a different graph",
                state_.location(region, operation));
        }
        if (!mapping.source.valid() || !mapping.target.valid()) {
            state_.diagnostics.error(
                DiagCode::InvalidBoundary,
                "GraphCompiler: boundary op '" + boundary.authoredId +
                    "' buffer mapping contains an invalid token",
                state_.location(region, operation));
            continue;
        }
        if (mapping.source.type() != mapping.target.type()) {
            state_.diagnostics.error(
                DiagCode::TypeMismatch,
                "GraphCompiler: boundary op '" + boundary.authoredId +
                    "' buffer mapping type mismatch",
                state_.location(region, operation));
        }
        if (!mapping.source.hasSizeScalar() &&
            !mapping.target.hasSizeScalar()) {
            state_.diagnostics.error(
                DiagCode::SizeMismatch,
                "GraphCompiler: boundary op '" + boundary.authoredId +
                    "' buffer mapping has no size scalar on either side",
                state_.location(region, operation));
        }
        validateScopes(mapping.source.scopeId(),
                       mapping.target.scopeId(), "buffer",
                       mapping.source.name());
    }
}

/*
 * Every buffer size is represented by a root U64 graph input. Keeping size
 * values global lets aliases across nested regions share one ValueId and keeps
 * allocation shape independent of control flow.
 */
void GraphValidator::validateBufferSize(
    const AuthoredRegion& region,
    const AuthoredOperation& operation, const GraphBuffer& buffer,
    const std::string& port) {
    if (!buffer.valid()) return;
    if (!buffer.hasSizeScalar()) {
        state_.diagnostics.error(
            DiagCode::SizeMismatch,
            "GraphCompiler: op '" + authoredSourceId(operation) +
                "' buffer '" + buffer.name() +
                "' has no size scalar",
            state_.location(region, operation, port));
        return;
    }
    const GraphScalar& size = buffer.sizeScalar();
    const auto& declared =
        state_.authored->root().declaredInputScalars;
    auto it = declared.find(size.varName());
    if (AuthoredScopeId(size.scopeId()) != state_.rootSourceScope ||
        it == declared.end() || it->second != ScalarType::U64) {
        state_.diagnostics.error(
            DiagCode::SizeMismatch,
            "GraphCompiler: op '" + authoredSourceId(operation) +
                "' buffer '" + buffer.name() +
                "' size scalar must be a root U64 graph input",
            state_.location(region, operation, port));
    }
}

/*
 * Ordinary bindings may use values local to their region; nested operations
 * may also read root scalars, but cannot write them. Buffers never bypass
 * region boundaries and are checked for root-backed size metadata after their
 * graph and scope are verified.
 */
void GraphValidator::validateScopesAndSizes(
    const AuthoredRegion& region,
    const AuthoredOperation& operation) {
    const detail::PortBindings* ioMap = operationIoMap(operation);
    if (!ioMap) return;

    /*
     * A root scalar is visible read-only in every nested region. Local scalars
     * must be declared in the root registry when they use the root scope.
     */
    auto scalarScopeAllowed = [&](const GraphScalar& scalar,
                                  bool output,
                                  const std::string& port) {
        if (scalar.graphId() != 0 &&
            scalar.graphId() != region.sourceGraph) {
            state_.diagnostics.error(
                DiagCode::InvalidScope,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' uses scalar '" + scalar.varName() +
                    "' from a different graph",
                state_.location(region, operation, port));
        }
        const bool allowed =
            AuthoredScopeId(scalar.scopeId()) == region.sourceScope ||
            (!output &&
             AuthoredScopeId(scalar.scopeId()) ==
                 state_.rootSourceScope);
        if (!allowed) {
            state_.diagnostics.error(
                DiagCode::InvalidScope,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' uses scalar '" + scalar.varName() +
                    "' from an invalid scope",
                state_.location(region, operation, port));
        }
        if (AuthoredScopeId(scalar.scopeId()) ==
            state_.rootSourceScope) {
            auto it = state_.authored->root().declaredScalars.find(
                scalar.varName());
            if (it ==
                state_.authored->root().declaredScalars.end()) {
                state_.diagnostics.error(
                    DiagCode::InvalidScope,
                    "GraphCompiler: global scalar '" +
                        scalar.varName() + "' is not declared",
                    state_.location(region, operation, port));
            } else if (it->second != scalar.type()) {
                state_.diagnostics.error(
                    DiagCode::TypeMismatch,
                    "GraphCompiler: global scalar '" +
                        scalar.varName() +
                        "' does not match its declared type",
                    state_.location(region, operation, port));
            }
        }
    };

    /*
     * Buffers are strictly region-local; movement across control boundaries
     * must be explicit so alias and result metadata can be constructed.
     */
    auto bufferScopeAllowed = [&](const GraphBuffer& buffer,
                                  const std::string& port) {
        if (buffer.graphId() != 0 &&
            buffer.graphId() != region.sourceGraph) {
            state_.diagnostics.error(
                DiagCode::InvalidScope,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' uses buffer '" + buffer.name() +
                    "' from a different graph",
                state_.location(region, operation, port));
        }
        if (AuthoredScopeId(buffer.scopeId()) !=
            region.sourceScope) {
            state_.diagnostics.error(
                DiagCode::InvalidScope,
                "GraphCompiler: op '" + authoredSourceId(operation) +
                    "' uses buffer '" + buffer.name() +
                    "' from an invalid scope",
                state_.location(region, operation, port));
        }
        validateBufferSize(region, operation, buffer, port);
    };

    /*
     * Validate all concrete binding categories before control-only operands,
     * which have their own declaration and integer trip-count rules.
     */
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
    validateConditionScopes(region, operation);
}

/*
 * Control-only scalars are not present in the normal port map, so validate
 * them explicitly against the control region's declarations. Literal
 * operands need no lookup; scalar operands must share graph, scope, and type,
 * and loop trip counts have the additional integer-type requirement.
 */
void GraphValidator::validateConditionScopes(
    const AuthoredRegion& region,
    const AuthoredOperation& operation) {
    /*
     * Use one check for trip counts and condition operands so graph ownership,
     * declaration, and type diagnostics stay consistent.
     */
    auto validateScalar =
        [&](const std::string& name, ScalarType type,
            std::uint64_t scopeId, std::uint64_t graphId,
            const std::string& port, const std::string& description) {
            if (graphId != region.sourceGraph) {
                state_.diagnostics.error(
                    DiagCode::InvalidScope,
                    "GraphCompiler: op '" +
                        authoredSourceId(operation) + "' " +
                        description + " '" + name +
                        "' belongs to a different graph",
                    state_.location(region, operation, port));
            }
            if (AuthoredScopeId(scopeId) != region.sourceScope) {
                state_.diagnostics.error(
                    DiagCode::InvalidScope,
                    "GraphCompiler: op '" +
                        authoredSourceId(operation) + "' " +
                        description + " '" + name +
                        "' is not in the control region",
                    state_.location(region, operation, port));
                return;
            }
            auto declared = region.declaredScalars.find(name);
            if (declared == region.declaredScalars.end()) {
                state_.diagnostics.error(
                    DiagCode::InvalidScope,
                    "GraphCompiler: op '" +
                        authoredSourceId(operation) + "' " +
                        description + " '" + name +
                        "' is not declared",
                    state_.location(region, operation, port));
            } else if (declared->second != type) {
                state_.diagnostics.error(
                    DiagCode::TypeMismatch,
                    "GraphCompiler: op '" +
                        authoredSourceId(operation) + "' " +
                        description + " '" + name +
                        "' does not match its declared type",
                    state_.location(region, operation, port));
            }
        };

    /*
     * Conditions can contain up to three operands. Only scalar-backed
     * operands become data dependencies; immediate values are self-contained.
     */
    auto conditionScope = [&](const Condition& condition) {
        auto check =
            [&](const std::optional<ConditionOperand>& operand) {
                if (!operand || !operand->isScalar()) return;
                validateScalar(
                    operand->name(), operand->type(),
                    operand->scopeId(), operand->graphId(),
                    "condition", "condition scalar");
            };
        check(condition.lhs());
        check(condition.rhs());
        check(condition.epsilon());
    };

    /*
     * Loops may bind both a trip count and a predicate, while conditionals
     * always bind exactly their authored predicate.
     */
    if (const auto* loop = std::get_if<AuthoredLoop>(&operation)) {
        if (loop->tripCount) {
            if (!isIntegerScalarType(loop->tripCount->type())) {
                state_.diagnostics.error(
                    DiagCode::TypeMismatch,
                    "GraphCompiler: loop op '" + loop->authoredId +
                        "' trip-count scalar must have an integer type",
                    state_.location(
                        region, operation, "trip_count"));
            }
            validateScalar(
                loop->tripCount->name(), loop->tripCount->type(),
                loop->tripCount->scopeId(),
                loop->tripCount->graphId(), "trip_count",
                "trip-count scalar");
        }
        if (loop->condition) conditionScope(*loop->condition);
    } else if (const auto* conditional =
                   std::get_if<AuthoredConditional>(&operation)) {
        conditionScope(conditional->condition);
    }
}

/*
 * An FPGA kernel tied to an image may run only after an explicit reprogram of
 * that same image in its region. Requiring the direct authored dependency
 * gives ordering derivation a concrete gate and prevents a similarly named
 * image or unrelated prior reprogram from being treated as sufficient.
 */
void GraphValidator::validateImageSafety(
    const AuthoredRegion& region) {
    for (const AuthoredOperation& operation : region.operations) {
        const auto* kernel = std::get_if<AuthoredKernel>(&operation);
        if (!kernel || kernel->kernel.type != DeviceType::FPGA ||
            !kernel->kernel.image) {
            continue;
        }
        bool gated = false;
        for (const AuthoredDependency& dependency : kernel->after) {
            if (!dependency.target) continue;
            const AuthoredOperation* prior =
                state_.authored->index().findOperation(
                    *dependency.target);
            if (!prior) continue;
            const auto* reprogram =
                std::get_if<AuthoredReprogram>(prior);
            if (reprogram &&
                reprogram->imageId == *kernel->kernel.image) {
                gated = true;
                break;
            }
        }
        if (!gated) {
            state_.diagnostics.error(
                DiagCode::ImageSafetyViolation,
                "GraphCompiler: FPGA kernel '" + kernel->kernel.name +
                    "' (op '" + kernel->authoredId +
                    "') is not gated behind reprogram of image '" +
                    *kernel->kernel.image + "'",
                state_.location(region, operation));
        }
    }
}

}  // namespace vrt::graph::resolve_detail
