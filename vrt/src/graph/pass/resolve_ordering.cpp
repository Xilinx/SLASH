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
#include <variant>

namespace vrt::graph::resolve_detail {

namespace {

bool hasAfter(const AuthoredOperation& operation, NodeId dependency) {
    const auto& after = operationAfter(operation);
    return std::any_of(
        after.begin(), after.end(),
        [&](const AuthoredDependency& value) {
            return value.target == std::optional<NodeId>(dependency);
        });
}

}  // namespace

/*
 * Add safety edges that follow from operation semantics rather than authored
 * data flow. Both rules may rediscover an existing edge, so dependencies are
 * sorted and deduplicated only after all derived edges are present.
 */
void DerivedOrdering::add(const AuthoredRegion& region) {
    addReprogramDrain(region);
    addReadersBeforeMutator(region);
    for (const AuthoredOperation& operation : region.operations) {
        ResolutionState::finishDependencies(
            state_.operations[authoredNodeId(operation)]);
    }
}

/*
 * Reprogramming to a new image must wait for all work gated by the previous
 * reprogram, not merely for that reprogram command itself. When B explicitly
 * follows A, every other operation that also follows A is treated as prior
 * image work and becomes a drain dependency of B.
 */
void DerivedOrdering::addReprogramDrain(
    const AuthoredRegion& region) {
    for (const AuthoredOperation& operation : region.operations) {
        const auto* reprogram =
            std::get_if<AuthoredReprogram>(&operation);
        if (!reprogram) continue;
        for (const AuthoredDependency& dependency :
             reprogram->after) {
            if (!dependency.target) continue;
            const AuthoredOperation* prior =
                state_.authored->index().findOperation(
                    *dependency.target);
            if (!prior ||
                !std::holds_alternative<AuthoredReprogram>(
                    *prior)) {
                continue;
            }
            for (const AuthoredOperation& candidate :
                 region.operations) {
                if (authoredNodeId(candidate) == reprogram->id ||
                    !hasAfter(candidate, *dependency.target)) {
                    continue;
                }
                state_.operations[reprogram->id]
                    .dependencies.push_back(
                        {authoredNodeId(candidate),
                         DependencyReason::ReprogramDrain,
                         std::nullopt});
            }
        }
    }
}

/*
 * An inout operation may overwrite the storage represented by its input token.
 * Make it wait for every other operation that reads that token, preventing a
 * mutator from racing readers that otherwise have no producer dependency on
 * the new output value.
 */
void DerivedOrdering::addReadersBeforeMutator(
    const AuthoredRegion& region) {
    for (const AuthoredOperation& operation : region.operations) {
        const detail::PortBindings* ioMap = operationIoMap(operation);
        if (!ioMap || ioMap->inouts().empty()) continue;
        for (const detail::PortBindings::InoutBinding& inout : ioMap->inouts()) {
            const TokenKey mutated = keyOf(inout.in);
            for (const AuthoredOperation& candidate :
                 region.operations) {
                if (authoredNodeId(candidate) ==
                    authoredNodeId(operation)) {
                    continue;
                }
                const detail::PortBindings* candidateIo =
                    operationIoMap(candidate);
                if (!candidateIo) continue;
                const bool reads = std::any_of(
                    candidateIo->inputs().begin(),
                    candidateIo->inputs().end(),
                    [&](const auto& input) {
                        return keyOf(input.second) == mutated;
                    });
                if (reads) {
                    state_.operations[authoredNodeId(operation)]
                        .dependencies.push_back(
                            {authoredNodeId(candidate),
                             DependencyReason::
                                 ReadersBeforeMutator,
                             std::nullopt});
                }
            }
        }
    }
}

}  // namespace vrt::graph::resolve_detail
