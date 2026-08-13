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

#ifndef VRT_GRAPH_PASS_RESOLVE_GRAPH_INTERNAL_HPP
#define VRT_GRAPH_PASS_RESOLVE_GRAPH_INTERNAL_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/ir/resolved_graph.hpp>

namespace vrt::graph::resolve_detail {

struct TokenKey {
    ValueKind       kind = ValueKind::Buffer;
    AuthoredScopeId scope;
    std::string     name;

    bool operator<(const TokenKey& other) const;
    bool operator==(const TokenKey& other) const;
};

struct TokenRef {
    TokenKey                   key;
    ValueType                  type;
    std::optional<GraphScalar> size;
    std::optional<GraphBuffer> buffer;
    std::optional<GraphScalar> scalar;
    std::string                port;
    ValueAccess                access = ValueAccess::Input;
};

using OutputKey = std::pair<NodeId, TokenKey>;

struct ParameterLink {
    ValueId  source;
    TokenRef target;
};

struct RegionContext {
    std::optional<NodeId>             control;
    ControlArm                        arm = ControlArm::LoopBackedge;
    std::map<TokenKey, ParameterLink> parameters;
    std::map<TokenKey, ValueId>       resultTargets;
};

struct RegionResolution {
    std::shared_ptr<const ResolvedRegion> region;
    std::map<TokenKey, ValueId>           finalValues;
};

struct RegionValues {
    std::map<NodeId, std::set<TokenKey>>       carriedByControl;
    std::map<NodeId, std::vector<TokenRef>>    outputsByNode;
    std::map<TokenKey, std::vector<NodeId>>    producerCandidates;
    std::map<OutputKey, TokenRef>              outputDescriptions;
    std::map<TokenKey, ValueId>                initialValues;
    std::map<OutputKey, ValueId>               outputValues;
    std::map<TokenKey, ValueId>                finalValues;
    std::map<OutputKey, ValueId>               controlInitialValues;
};

TokenKey keyOf(const GraphBuffer& buffer);
TokenKey keyOf(const GraphScalar& scalar);
TokenRef inputRef(const GraphBuffer& buffer, std::string port,
                  ValueAccess access = ValueAccess::Input);
TokenRef outputRef(const GraphBuffer& buffer, std::string port,
                   ValueAccess access = ValueAccess::Output);
TokenRef inputRef(const GraphScalar& scalar, std::string port,
                  ValueAccess access = ValueAccess::Input);
TokenRef outputRef(const GraphScalar& scalar, std::string port,
                   ValueAccess access = ValueAccess::Output);

ResolvedOperationKind operationKind(const AuthoredOperation& operation);
const std::vector<AuthoredDependency>& operationAfter(
    const AuthoredOperation& operation);
const detail::PortBindings* operationIoMap(const AuthoredOperation& operation);
const IOTypeMap* operationIoType(const AuthoredOperation& operation);
std::vector<const AuthoredRegion*> childRegions(
    const AuthoredOperation& operation);
std::vector<const AuthoredBoundary*> boundaries(
    const AuthoredRegion& region, BoundarySide side);
std::vector<TokenRef> ioInputs(const detail::PortBindings& ioMap);
std::vector<TokenRef> ioOutputs(const detail::PortBindings& ioMap);
std::vector<TokenRef> controlBoundaryInputs(
    const AuthoredOperation& operation);
std::vector<TokenRef> controlBoundaryOutputs(
    const AuthoredOperation& operation);
std::vector<TokenRef> producedValues(
    const AuthoredOperation& operation);
std::set<TokenKey> loopCarriedValues(
    const AuthoredOperation& operation);
bool isControl(const AuthoredOperation& operation);

class ResolutionState {
   public:
    explicit ResolutionState(
        std::shared_ptr<const AuthoredGraph> authoredGraph);

    DiagnosticLocation location(
        const AuthoredRegion& region,
        const AuthoredOperation& operation,
        std::optional<std::string> port = std::nullopt) const;
    ValueId createValue(
        const TokenRef& token, RegionId region,
        ValueDefinitionKind definition,
        std::optional<NodeId> producer);
    void addDependency(
        ResolvedOperation& operation, ValueId value,
        DependencyReason reason = DependencyReason::Data);
    static void finishDependencies(ResolvedOperation& operation);
    ResolvedControlResult& controlResult(NodeId control, ValueId result);

    std::shared_ptr<const AuthoredGraph> authored;
    Diagnostics                           diagnostics;
    AuthoredScopeId                      rootSourceScope;
    std::uint64_t                        nextValue = 0;
    std::map<TokenKey, ValueId>          rootInputValues;
    std::map<ValueId, ResolvedValue>     values;
    std::map<NodeId, ResolvedOperation>  operations;
    std::vector<ResolvedInoutBinding>    inoutBindings;
    std::vector<BoundaryAlias>           boundaryAliases;
    std::vector<ResolvedControlResult>   controlResults;

   private:
    std::map<std::pair<NodeId, ValueId>, std::size_t>
        controlResultIndexes_;
};

class GraphValidator {
   public:
    explicit GraphValidator(ResolutionState& state) : state_(state) {}

    void validateRegion(const AuthoredRegion& region);

   private:
    void validateOperation(const AuthoredRegion& region,
                           const AuthoredOperation& operation);
    void validateIoPorts(const AuthoredRegion& region,
                         const AuthoredOperation& operation);
    void validateRequiredPorts(const AuthoredRegion& region,
                               const AuthoredOperation& operation,
                               const IOTypeMap& ioType,
                               const detail::PortBindings& ioMap);
    void validateBoundPorts(const AuthoredRegion& region,
                            const AuthoredOperation& operation,
                            const IOTypeMap& ioType,
                            const detail::PortBindings& ioMap);
    void validateBoundary(const AuthoredRegion& region,
                          const AuthoredOperation& operation,
                          const AuthoredBoundary& boundary);
    void validateBufferSize(const AuthoredRegion& region,
                            const AuthoredOperation& operation,
                            const GraphBuffer& buffer,
                            const std::string& port);
    void validateScopesAndSizes(const AuthoredRegion& region,
                                const AuthoredOperation& operation);
    void validateConditionScopes(
        const AuthoredRegion& region,
        const AuthoredOperation& operation);
    void validateImageSafety(const AuthoredRegion& region);

    ResolutionState& state_;
};

class ValueResolver {
   public:
    explicit ValueResolver(ResolutionState& state) : state_(state) {}

    RegionValues defineRegionValues(
        const AuthoredRegion& region, const RegionContext& context,
        bool rootRegion, ResolvedRegion& resolvedRegion);
    ResolvedOperation makeOperation(
        const AuthoredRegion& region,
        const AuthoredOperation& authoredOperation) const;
    std::optional<ValueId> valueForUse(
        const AuthoredRegion& region,
        const AuthoredOperation& operation,
        const TokenRef& token, const RegionValues& values,
        bool rootRegion);
    void bindDataOperation(
        const AuthoredRegion& region,
        const AuthoredOperation& authoredOperation,
        const RegionValues& values, bool rootRegion,
        ResolvedOperation& operation);
    void markRootOutputs(
        const AuthoredRegion& region,
        const std::map<TokenKey, ValueId>& finalValues);

   private:
    void collectProducerMetadata(const AuthoredRegion& region,
                                 RegionValues& values);
    void createRegionParameters(const AuthoredRegion& region,
                                const RegionContext& context,
                                ResolvedRegion& resolvedRegion,
                                RegionValues& values);
    void createRootInputs(const AuthoredRegion& region,
                          RegionValues& values);
    void createOperationOutputs(const AuthoredRegion& region,
                                RegionValues& values);
    void selectFinalProducers(RegionValues& values);
    void validateProducerTypes(const RegionValues& values);
    void bindInouts(const detail::PortBindings& ioMap,
                    ResolvedOperation& operation);

    ResolutionState& state_;
};

class ControlComposer {
   public:
    explicit ControlComposer(ResolutionState& state) : state_(state) {}

    void bindBoundary(
        const AuthoredRegion& region,
        const AuthoredOperation& authoredOperation,
        const AuthoredBoundary& boundary,
        const RegionContext& context, const RegionValues& values,
        ResolvedRegion& resolvedRegion,
        ResolvedOperation& operation);
    void bindControlOperation(
        const AuthoredRegion& region,
        const AuthoredOperation& authoredOperation,
        const RegionValues& values, bool rootRegion,
        ValueResolver& valueResolver,
        ResolvedOperation& operation);
    RegionContext makeChildContext(
        const AuthoredRegion& parent,
        const AuthoredOperation& operation,
        const AuthoredRegion& child, ControlArm arm,
        const RegionValues& values);
    void addControlIncoming(
        const AuthoredOperation& operation,
        const AuthoredRegion& child,
        const RegionResolution& childResolution,
        const RegionContext& context, ControlArm arm,
        const RegionValues& values);

   private:
    void bindStartBoundary(
        const AuthoredRegion& region,
        const AuthoredOperation& authoredOperation,
        const AuthoredBoundary& boundary,
        const RegionContext& context, const RegionValues& values,
        ResolvedOperation& operation);
    void bindEndBoundary(
        const AuthoredRegion& region,
        const AuthoredOperation& authoredOperation,
        const AuthoredBoundary& boundary,
        const RegionContext& context, const RegionValues& values,
        ResolvedRegion& resolvedRegion,
        ResolvedOperation& operation);
    void bindBoundaryParameter(
        const AuthoredRegion& region,
        const AuthoredOperation& authoredOperation,
        const AuthoredBoundary& boundary,
        const RegionContext& context, const RegionValues& values,
        const TokenKey& target, const std::string& port,
        ResolvedOperation& operation);
    void bindBoundaryResult(
        const AuthoredRegion& region,
        const AuthoredOperation& authoredOperation,
        const AuthoredBoundary& boundary,
        const RegionContext& context, const RegionValues& values,
        const TokenRef& source, const TokenKey& target,
        const std::string& port, ResolvedRegion& resolvedRegion,
        ResolvedOperation& operation);
    void addConditionBindings(
        const AuthoredRegion& region,
        const AuthoredOperation& authoredOperation,
        const Condition& condition, const RegionValues& values,
        bool rootRegion, ValueResolver& valueResolver,
        ResolvedOperation& operation);
    std::optional<ValueId> parentValue(
        const AuthoredRegion& parent,
        const AuthoredOperation& operation,
        const TokenRef& source, const RegionValues& values);
    void addStartBoundaryParameters(
        const AuthoredRegion& parent,
        const AuthoredOperation& operation,
        const AuthoredRegion& child,
        const RegionValues& values, RegionContext& context);
    void addResultTargets(
        const AuthoredOperation& operation,
        const AuthoredRegion& child,
        const RegionValues& values, RegionContext& context);
    void addExplicitIncoming(
        const AuthoredOperation& operation,
        const AuthoredRegion& child,
        const RegionResolution& childResolution,
        const RegionContext& context, ControlArm arm,
        const RegionValues& values);
    void addImplicitIncoming(
        const AuthoredOperation& operation,
        const AuthoredRegion& child,
        const RegionResolution& childResolution,
        const RegionContext& context, ControlArm arm);
    void addImplicitPortIncoming(
        NodeId control, const AuthoredRegion& child,
        const RegionResolution& childResolution,
        const RegionContext& context, ControlArm arm,
        const std::string& port, const TokenKey& target);

    ResolutionState& state_;
};

class DerivedOrdering {
   public:
    explicit DerivedOrdering(ResolutionState& state) : state_(state) {}

    void add(const AuthoredRegion& region);

   private:
    void addReprogramDrain(const AuthoredRegion& region);
    void addReadersBeforeMutator(const AuthoredRegion& region);

    ResolutionState& state_;
};

class DeterministicTopology {
   public:
    explicit DeterministicTopology(ResolutionState& state)
        : state_(state) {}

    std::vector<NodeId> order(const AuthoredRegion& region);

   private:
    ResolutionState& state_;
};

}  // namespace vrt::graph::resolve_detail

#endif  // VRT_GRAPH_PASS_RESOLVE_GRAPH_INTERNAL_HPP
