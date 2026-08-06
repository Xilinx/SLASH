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

#include <memory>
#include <utility>
#include <variant>

#include <vrt/graph/pass/validate_authored_graph.hpp>

namespace vrt::graph {

namespace resolve_detail {

class GraphResolver {
   public:
    explicit GraphResolver(const AuthoredGraph& authored)
        : state_(std::make_shared<AuthoredGraph>(authored)),
          values_(state_),
          control_(state_),
          ordering_(state_),
          topology_(state_) {}

    /*
     * Resolve the root recursively into one shared value and operation table.
     * No partially resolved graph escapes: any diagnostic error discards the
     * assembled regions, while successful resolution transfers all tables
     * into the immutable ResolvedGraph.
     */
    CompileResult<ResolvedGraph> resolve() {
        RegionResolution root = resolveRegion(
            state_.authored->root(), RegionContext{}, true);
        if (state_.diagnostics.hasErrors()) {
            return CompileResult<ResolvedGraph>::failure(
                std::move(state_.diagnostics));
        }
        return CompileResult<ResolvedGraph>::success(
            ResolvedGraph(
                state_.authored, std::move(root.region),
                std::move(state_.values),
                std::move(state_.operations),
                std::move(state_.inoutBindings),
                BoundaryAliasTable(
                    std::move(state_.boundaryAliases)),
                std::move(state_.controlResults)),
            std::move(state_.diagnostics));
    }

   private:
    RegionResolution resolveRegion(
        const AuthoredRegion& region,
        const RegionContext& context, bool rootRegion) {
        /*
         * Resolve a region in dependency order: establish its value namespace,
         * bind local operations, derive local ordering, then recurse into
         * children using the now-complete parent values. Child results are
         * folded back into their control operation after each recursive call.
         */
        auto resolvedRegion = std::make_shared<ResolvedRegion>();
        resolvedRegion->id = region.id;
        resolvedRegion->parent = region.parent;

        /*
         * Local topology must be complete before children are visited because
         * child operations live in separate regions and never participate in
         * the parent's topological sort.
         */
        RegionValues regionValues = values_.defineRegionValues(
            region, context, rootRegion, *resolvedRegion);
        resolveOperations(
            region, context, rootRegion, regionValues,
            *resolvedRegion);
        ordering_.add(region);
        resolvedRegion->topologicalOrder = topology_.order(region);

        /*
         * Child contexts translate parent values to region parameters and
         * child exports back to control results. Root outputs are marked only
         * after every nested arm has supplied its incoming values.
         */
        resolveChildren(region, regionValues, *resolvedRegion);
        if (rootRegion) {
            values_.markRootOutputs(
                region, regionValues.finalValues);
        }
        return {
            std::move(resolvedRegion),
            std::move(regionValues.finalValues)};
    }

    /*
     * Bind every operation against the region-wide value tables, so authored
     * order does not affect name resolution. Boundaries translate parent and
     * child values; ordinary and control operations bind data first and then
     * their control-only operands before dependencies are canonicalized.
     */
    void resolveOperations(
        const AuthoredRegion& region,
        const RegionContext& context, bool rootRegion,
        const RegionValues& regionValues,
        ResolvedRegion& resolvedRegion) {
        for (const AuthoredOperation& authoredOperation :
             region.operations) {
            ResolvedOperation operation =
                values_.makeOperation(region, authoredOperation);
            if (const auto* boundary =
                    std::get_if<AuthoredBoundary>(
                        &authoredOperation)) {
                control_.bindBoundary(
                    region, authoredOperation, *boundary, context,
                    regionValues, resolvedRegion, operation);
            } else {
                values_.bindDataOperation(
                    region, authoredOperation, regionValues,
                    rootRegion, operation);
                control_.bindControlOperation(
                    region, authoredOperation, regionValues,
                    rootRegion, values_, operation);
            }
            ResolutionState::finishDependencies(operation);
            state_.operations[operation.id] = std::move(operation);
        }
    }

    /*
     * Follow child entries from the snapshot index rather than rediscovering
     * them from variants. Missing entries can only follow earlier diagnostics,
     * so resolution skips them and continues collecting independent errors.
     */
    void resolveChildren(
        const AuthoredRegion& region,
        const RegionValues& regionValues,
        ResolvedRegion& resolvedRegion) {
        for (const AuthoredChildRegion& childEntry :
             state_.authored->index().children(region.id)) {
            const AuthoredOperation* operation =
                state_.authored->index().findOperation(
                    childEntry.control);
            const AuthoredRegion* child =
                state_.authored->index().findRegion(
                    childEntry.region);
            if (!operation || !child) continue;
            resolveChild(
                region, *operation, *child,
                armFor(childEntry.role), regionValues,
                resolvedRegion);
        }
    }

    /*
     * Construct the parent-to-child value context before recursion, then use
     * the child's final values to populate the owning control result. Appending
     * the resolved child last keeps the tree and incoming-value metadata in
     * the same arm order as the authored index.
     */
    void resolveChild(
        const AuthoredRegion& parent,
        const AuthoredOperation& operation,
        const AuthoredRegion& child, ControlArm arm,
        const RegionValues& regionValues,
        ResolvedRegion& resolvedRegion) {
        RegionContext childContext = control_.makeChildContext(
            parent, operation, child, arm, regionValues);
        RegionResolution childResolution =
            resolveRegion(child, childContext, false);
        control_.addControlIncoming(
            operation, child, childResolution, childContext, arm,
            regionValues);
        resolvedRegion.children.push_back(
            std::move(childResolution.region));
    }

    static ControlArm armFor(AuthoredChildRole role) {
        switch (role) {
            case AuthoredChildRole::LoopBody:
                return ControlArm::LoopBackedge;
            case AuthoredChildRole::ConditionalThen:
                return ControlArm::ThenBranch;
            case AuthoredChildRole::ConditionalElse:
                return ControlArm::ElseBranch;
        }
        return ControlArm::LoopBackedge;
    }

    ResolutionState       state_;
    ValueResolver         values_;
    ControlComposer       control_;
    DerivedOrdering       ordering_;
    DeterministicTopology topology_;
};

}  // namespace resolve_detail

/*
 * Public resolution always validates first because the resolver assumes
 * unique producers, valid scopes, and a well-formed region tree. Warnings from
 * validation are retained and appended to any diagnostics from resolution.
 */
CompileResult<ResolvedGraph> resolveGraph(
    const AuthoredGraph& authored) {
    CompileResult<AuthoredGraph> validated =
        validateAuthoredGraph(authored);
    if (!validated.ok()) {
        return CompileResult<ResolvedGraph>::failure(
            std::move(validated.diagnostics));
    }
    CompileResult<ResolvedGraph> resolved =
        detail::resolveValidatedGraph(*validated.output);
    resolved.diagnostics.append(
        std::move(validated.diagnostics));
    return resolved;
}

CompileResult<ResolvedGraph> detail::resolveValidatedGraph(
    const AuthoredGraph& authored) {
    return resolve_detail::GraphResolver(authored).resolve();
}

}  // namespace vrt::graph
