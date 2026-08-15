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

#include <vrt/graph/pass/validate_authored_graph.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "resolve_graph_internal.hpp"

namespace vrt::graph {
namespace {

using resolve_detail::TokenKey;
using resolve_detail::TokenRef;

class AuthoredProducerValidator {
   public:
    explicit AuthoredProducerValidator(
        resolve_detail::ResolutionState& state)
        : state_(state) {}

    /*
     * Validate the value namespace of one region as a whole. Producers are
     * collected before consumers are checked, so authored order does not
     * matter and forward references remain legal.
     */
    void validate(const AuthoredRegion& region) {
        ProducerMap producers;
        std::map<TokenKey, std::set<NodeId>> carried;
        collectProducers(region, producers, carried);
        validateDuplicateProducers(region, producers, carried);
        validateGraphOutputScalars(region, producers);
        validateConsumers(region, producers);
    }

   private:
    using ProducerMap =
        std::map<TokenKey, std::vector<NodeId>>;

    void collectProducers(
        const AuthoredRegion& region, ProducerMap& producers,
        std::map<TokenKey, std::set<NodeId>>& carried) const {
        for (const AuthoredOperation& operation :
             region.operations) {
            const NodeId node = authoredNodeId(operation);
            for (const TokenRef& output :
                 resolve_detail::producedValues(operation)) {
                producers[output.key].push_back(node);
            }
            for (const TokenKey& key :
                 resolve_detail::loopCarriedValues(operation)) {
                carried[key].insert(node);
            }
        }
    }

    /*
     * A value normally has one defining operation. The only legal overlap is
     * a loop-carried value: either a root input re-emitted by its loop, or one
     * ordinary initial producer paired with exactly one carrying control.
     * All other graph-input redefinitions and producer sets are ambiguous.
     */
    void validateDuplicateProducers(
        const AuthoredRegion& region, ProducerMap& producers,
        const std::map<TokenKey, std::set<NodeId>>& carried) {
        /*
         * Only the root region owns graph inputs. Child-region parameters
         * enter through start boundaries and are handled below as ordinary
         * region-local values.
         */
        std::set<TokenKey> graphInputs;
        if (!region.parent) {
            for (const auto& [name, buffer] :
                 region.declaredInputBuffers) {
                (void)name;
                graphInputs.insert(resolve_detail::keyOf(buffer));
            }
            for (const auto& [name, type] :
                 region.declaredInputScalars) {
                graphInputs.insert(
                    {ValueKind::Scalar, region.sourceScope, name});
                (void)type;
            }
        }
        for (auto& [key, candidates] : producers) {
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(
                std::unique(candidates.begin(), candidates.end()),
                candidates.end());
            auto carriedIt = carried.find(key);
            const bool legalCarriedInput =
                graphInputs.count(key) != 0 &&
                candidates.size() == 1 &&
                carriedIt != carried.end() &&
                carriedIt->second.count(candidates.front()) != 0;
            if (graphInputs.count(key) != 0 &&
                !legalCarriedInput) {
                state_.diagnostics.error(
                    DiagCode::DuplicateProducer,
                    "GraphCompiler: operation redefines graph input '" +
                        key.name + "'",
                    DiagnosticLocation{region.id});
                continue;
            }
            if (candidates.size() <= 1) continue;
            const std::size_t carriedCount =
                carriedIt == carried.end()
                    ? 0
                    : carriedIt->second.size();
            if (candidates.size() == 2 && carriedCount == 1) {
                continue;
            }
            state_.diagnostics.error(
                DiagCode::DuplicateProducer,
                "GraphCompiler: multiple operations produce value '" +
                    key.name + "' in one region",
                DiagnosticLocation{region.id});
        }
    }

    /*
     * Region parameters are the values available without a local producer.
     * The root gets declared graph inputs; nested regions get targets of
     * start-boundary mappings, which are their explicit parent imports.
     */
    std::set<TokenKey> regionParameters(
        const AuthoredRegion& region) const {
        std::set<TokenKey> result;
        if (!region.parent) {
            for (const auto& [name, buffer] :
                 region.declaredInputBuffers) {
                (void)name;
                result.insert(resolve_detail::keyOf(buffer));
            }
            for (const auto& [name, type] :
                 region.declaredInputScalars) {
                result.insert(
                    {ValueKind::Scalar, region.sourceScope, name});
                (void)type;
            }
        }
        for (const AuthoredOperation& operation :
             region.operations) {
            const auto* boundary =
                std::get_if<AuthoredBoundary>(&operation);
            if (!boundary ||
                boundary->side != BoundarySide::Start) {
                continue;
            }
            for (const auto& mapping :
                 boundary->bufferMappings) {
                result.insert(
                    resolve_detail::keyOf(mapping.target));
            }
            for (const auto& mapping :
                 boundary->scalarMappings) {
                result.insert(
                    resolve_detail::keyOf(mapping.target));
            }
        }
        return result;
    }

    void validateGraphOutputScalars(
        const AuthoredRegion& region,
        const ProducerMap& producers) {
        if (region.parent) return;
        for (const auto& [name, type] :
             region.declaredOutputScalars) {
            (void)type;
            const TokenKey key{
                ValueKind::Scalar, region.sourceScope, name};
            auto found = producers.find(key);
            if (found != producers.end() && !found->second.empty()) {
                continue;
            }
            state_.diagnostics.error(
                DiagCode::MissingProducer,
                "GraphCompiler: graph output scalar '" + name +
                    "' has no producer",
                DiagnosticLocation{region.id});
        }
    }

    /*
     * Form a complete consumer list from ordinary ports and control syntax.
     * Control ops consume child start-boundary sources; end boundaries consume
     * their local sources; trip counts and scalar condition operands are also
     * dependencies even though they are not ordinary I/O ports.
     */
    std::vector<TokenRef> consumers(
        const AuthoredOperation& operation) const {
        std::vector<TokenRef> result;
        if (const detail::PortBindings* io =
                resolve_detail::operationIoMap(operation)) {
            result = resolve_detail::ioInputs(*io);
        }
        std::vector<TokenRef> control =
            resolve_detail::controlBoundaryInputs(operation);
        result.insert(
            result.end(), control.begin(), control.end());
        if (const auto* boundary =
                std::get_if<AuthoredBoundary>(&operation);
            boundary &&
            boundary->side == BoundarySide::End) {
            for (std::size_t i = 0;
                 i < boundary->bufferMappings.size(); ++i) {
                result.push_back(resolve_detail::inputRef(
                    boundary->bufferMappings[i].source,
                    "boundary.buffer." + std::to_string(i)));
            }
            for (std::size_t i = 0;
                 i < boundary->scalarMappings.size(); ++i) {
                result.push_back(resolve_detail::inputRef(
                    boundary->scalarMappings[i].source,
                    "boundary.scalar." + std::to_string(i)));
            }
        }
        appendControlScalars(operation, result);
        return result;
    }

    static void appendCondition(
        const Condition& condition,
        std::vector<TokenRef>& result) {
        auto append = [&](const std::optional<ConditionOperand>& value,
                          const char* port) {
            if (!value || !value->isScalar()) return;
            result.push_back(TokenRef{
                {ValueKind::Scalar,
                 AuthoredScopeId(value->scopeId()), value->name()},
                ValueType::scalarType(value->type()),
                std::nullopt, std::nullopt, std::nullopt, port,
                ValueAccess::Input});
        };
        append(condition.lhs(), "condition.lhs");
        append(condition.rhs(), "condition.rhs");
        append(condition.epsilon(), "condition.epsilon");
    }

    static void appendControlScalars(
        const AuthoredOperation& operation,
        std::vector<TokenRef>& result) {
        if (const auto* loop =
                std::get_if<AuthoredLoop>(&operation)) {
            if (loop->tripCount) {
                result.push_back(TokenRef{
                    {ValueKind::Scalar,
                     AuthoredScopeId(loop->tripCount->scopeId()),
                     loop->tripCount->name()},
                    ValueType::scalarType(loop->tripCount->type()),
                    std::nullopt, std::nullopt, std::nullopt,
                    "trip_count", ValueAccess::Input});
            }
            if (loop->condition) {
                appendCondition(*loop->condition, result);
            }
        } else if (const auto* conditional =
                       std::get_if<AuthoredConditional>(&operation)) {
            appendCondition(conditional->condition, result);
        }
    }

    /*
     * Resolve each use against any other producer in the same region, an
     * explicit region parameter, or a root scalar visible to nested control.
     * An operation's own output cannot satisfy its input, while a producer
     * appearing later in authored order is intentionally accepted.
     */
    void validateConsumers(
        const AuthoredRegion& region,
        const ProducerMap& producers) {
        const std::set<TokenKey> parameters =
            regionParameters(region);
        std::set<TokenKey> rootScalars;
        for (const auto& [name, type] :
             state_.authored->root().declaredScalars) {
            rootScalars.insert(
                {ValueKind::Scalar, state_.rootSourceScope, name});
            (void)type;
        }
        for (const AuthoredOperation& operation :
             region.operations) {
            const NodeId consumer = authoredNodeId(operation);
            for (const TokenRef& token : consumers(operation)) {
                bool hasProducer = false;
                auto found = producers.find(token.key);
                if (found != producers.end()) {
                    hasProducer = std::any_of(
                        found->second.begin(), found->second.end(),
                        [&](NodeId producer) {
                            return producer != consumer;
                        });
                }
                if (hasProducer ||
                    parameters.count(token.key) != 0 ||
                    rootScalars.count(token.key) != 0) {
                    continue;
                }
                state_.diagnostics.error(
                    DiagCode::MissingProducer,
                    "GraphCompiler: op '" +
                        authoredSourceId(operation) + "' consumes " +
                        (token.key.kind == ValueKind::Buffer
                             ? "buffer '"
                             : "scalar '") +
                        token.key.name + "' with no producer",
                    state_.location(
                        region, operation, token.port),
                    "Declare it as a graph input or bind an output "
                    "producer; forward references are allowed");
            }
        }
    }

    /*
     * Check the branch contract for control buffer results. Every child arm
     * must export each declared target through an end boundary; validating
     * arms separately prevents one branch from masking a missing result in
     * another.
     */
    void validateControlOutputs(
        const AuthoredRegion& region) {
        for (const AuthoredOperation& operation :
             region.operations) {
            std::map<TokenKey, std::string> outputs;
            std::vector<const AuthoredRegion*> branches;
            if (const auto* loop =
                    std::get_if<AuthoredLoop>(&operation)) {
                for (const auto& [port, output] :
                     loop->namedOutputBuffers) {
                    outputs.emplace(
                        resolve_detail::keyOf(output), port.value());
                }
                if (loop->body) branches.push_back(loop->body.get());
            } else if (const auto* conditional =
                           std::get_if<AuthoredConditional>(&operation)) {
                for (const auto& [port, output] :
                     conditional->namedOutputBuffers) {
                    outputs.emplace(
                        resolve_detail::keyOf(output), port.value());
                }
                if (conditional->thenRegion) {
                    branches.push_back(
                        conditional->thenRegion.get());
                }
                if (conditional->elseRegion) {
                    branches.push_back(
                        conditional->elseRegion.get());
                }
            }
            if (branches.empty()) continue;
            if (const detail::PortBindings* io =
                    resolve_detail::operationIoMap(operation)) {
                for (const auto& [port, output] : io->outputs()) {
                    outputs.emplace(
                        resolve_detail::keyOf(output), port);
                }
                for (const auto& inout : io->inouts()) {
                    outputs.emplace(
                        resolve_detail::keyOf(inout.out),
                        inout.outPort);
                }
            }
            for (const auto& [expected, port] : outputs) {
                for (const AuthoredRegion* branch : branches) {
                    bool exported = false;
                    for (const AuthoredBoundary* boundary :
                         resolve_detail::boundaries(
                             *branch, BoundarySide::End)) {
                        exported |= std::any_of(
                            boundary->bufferMappings.begin(),
                            boundary->bufferMappings.end(),
                            [&](const BufferBoundaryMapping& mapping) {
                                return resolve_detail::keyOf(
                                           mapping.target) == expected;
                            });
                    }
                    if (exported) continue;
                    state_.diagnostics.error(
                        DiagCode::InvalidControlResult,
                        "GraphCompiler: control op '" +
                            authoredSourceId(operation) +
                            "' branch does not produce output port '" +
                            port + "'",
                        state_.location(
                            region, operation, port));
                }
            }
        }
    }

    resolve_detail::ResolutionState& state_;
};

}  // namespace

/*
 * Run structural and producer/consumer validation over every indexed region
 * before resolution allocates value IDs. Diagnostics are accumulated across
 * the full tree so callers receive all authored-graph errors in one pass.
 */
CompileResult<AuthoredGraph> validateAuthoredGraph(
    const AuthoredGraph& authored) {
    auto snapshot = std::make_shared<AuthoredGraph>(authored);
    resolve_detail::ResolutionState state(snapshot);
    if (authored.root().operations.empty()) {
        state.diagnostics.error(
            DiagCode::EmptyGraph,
            "GraphCompiler::compile: graph has no ops");
    }
    resolve_detail::GraphValidator structural(state);
    AuthoredProducerValidator producers(state);
    for (const auto& [id, region] :
         authored.index().regions()) {
        (void)id;
        structural.validateRegion(*region);
        producers.validate(*region);
    }
    if (state.diagnostics.hasErrors()) {
        return CompileResult<AuthoredGraph>::failure(
            std::move(state.diagnostics));
    }
    return CompileResult<AuthoredGraph>::success(
        authored, std::move(state.diagnostics));
}

}  // namespace vrt::graph
