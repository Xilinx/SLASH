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

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace vrt::graph::place_detail {

namespace {

struct BoundaryPair {
    PortName port;
    ValueId  source;
    ValueId  target;
};

bool consumes(ValueAccess access) {
    return access == ValueAccess::Input ||
           access == ValueAccess::InoutInput ||
           access == ValueAccess::Condition ||
           access == ValueAccess::TripCount ||
           access == ValueAccess::BoundarySource;
}

/*
 * A resolved boundary is a set of same-port source/target pairs. Prefer the
 * resolver's direct alias when present; otherwise the explicit source
 * binding supplies the value that crosses into the target identity.
 */
std::vector<BoundaryPair> boundaryPairs(
    const PlacementState& state,
    const ResolvedOperation& operation) {
    std::map<PortName, ValueId> sources;
    std::vector<BoundaryPair> result;
    for (const ResolvedBinding& binding : operation.bindings) {
        if (binding.access == ValueAccess::BoundarySource) {
            sources[binding.port] = binding.value;
        } else if (binding.access == ValueAccess::BoundaryTarget) {
            auto source = sources.find(binding.port);
            std::optional<ValueId> alias =
                state.resolved->aliases().directSource(binding.value);
            if (alias || source != sources.end()) {
                result.push_back({
                    binding.port,
                    alias.value_or(source->second),
                    binding.value});
            }
        }
    }
    return result;
}

/*
 * Exact port replicas carry memory-region information such as an HBM bank
 * and are added whenever present. Coarse participant-device fallback is used
 * only while the aggregate has no placement evidence, avoiding extra guesses
 * once a consumer has supplied a concrete location.
 */
void addOperationMemories(
    const PlacementState& state, NodeId operation, ValueId value,
    std::set<MemoryPlacement>& memories) {
    for (const PortPlacement& port : state.portPlacements) {
        if (port.operation != operation) continue;
        auto replica = state.replicas.find(port.replica);
        if (replica != state.replicas.end() &&
            replica->second.value == value) {
            memories.insert(replica->second.memory);
        }
    }
    if (!memories.empty()) return;
    for (const DeviceId& device :
         operationDevices(state, operation)) {
        memories.insert({device, std::nullopt});
    }
}

/*
 * A start boundary must materialize where its target is consumed inside the
 * entered region. Multiple consumers may require mappings on several devices
 * or memory regions, so all distinct locations are retained.
 */
std::set<MemoryPlacement> startMemories(
    const PlacementState& state, const ResolvedOperation& boundary,
    ValueId target) {
    std::set<MemoryPlacement> result;
    for (const auto& [node, operation] :
         state.resolved->operations()) {
        if (operation.region != boundary.region ||
            operation.structural) {
            continue;
        }
        const bool usesTarget = std::any_of(
            operation.bindings.begin(), operation.bindings.end(),
            [&](const ResolvedBinding& binding) {
                return binding.value == target &&
                       consumes(binding.access);
            });
        if (usesTarget) {
            addOperationMemories(state, node, target, result);
        }
    }
    return result;
}

/*
 * An end boundary follows the source's primary replica when available. If
 * the source has only structural provenance, producer participants provide
 * conservative regionless locations instead.
 */
std::set<MemoryPlacement> endMemories(
    const PlacementState& state, ValueId source) {
    std::set<MemoryPlacement> result;
    if (const ValueReplica* replica = primaryReplica(state, source)) {
        result.insert(replica->memory);
        return result;
    }
    const ResolvedValue* value = state.resolved->findValue(source);
    if (!value) return result;
    const std::optional<NodeId> producer = valueProducer(*value);
    if (!producer) return result;
    for (const DeviceId& device : operationDevices(state, *producer)) {
        result.insert({device, std::nullopt});
    }
    return result;
}

/*
 * Empty regions expose no consumer or producer memory to infer. In that case
 * the enclosing control primary is the sole stable owner for the structural
 * handoff; a discovered location is never overridden by this fallback.
 */
void addFallbackOwner(
    const PlacementState& state, RegionId region,
    std::set<MemoryPlacement>& memories) {
    if (!memories.empty()) return;
    const std::optional<NodeId> parent =
        state.resolved->authored().index().parentControl(region);
    if (!parent) return;
    auto placement = state.controlPlacements.find(*parent);
    if (placement != state.controlPlacements.end()) {
        memories.insert(
            {controlPrimary(placement->second), std::nullopt});
    }
}

/*
 * Both sides of a boundary mapping denote the same device memory, but retain
 * distinct value identities. A target becomes primary only when no earlier
 * placement established a more authoritative definition.
 */
void placePair(
    PlacementState& state, NodeId boundary,
    const BoundaryPair& pair, const MemoryPlacement& memory) {
    const ReplicaId source = ensureReplica(
        state, pair.source, memory, ReplicaPurpose::BoundarySource,
        boundary, pair.port);
    const bool targetNeedsPrimary =
        primaryReplica(state, pair.target) == nullptr;
    const ReplicaId target = ensureReplica(
        state, pair.target, memory, ReplicaPurpose::BoundaryTarget,
        boundary, pair.port, targetNeedsPrimary);
    state.boundaryMappings.push_back(
        {boundary, pair.port, source, target, memory.device});
}

}  // namespace

/*
 * Start boundaries follow target consumers; end boundaries follow source
 * producers. Either side may fan out to several memories, with the enclosing
 * control used only when an otherwise empty region offers no placement
 * evidence.
 */
void placeBoundaryMappings(PlacementState& state) {
    for (const auto& [node, operation] :
         state.resolved->operations()) {
        if (!operation.structural) continue;
        const AuthoredOperation* authored =
            state.resolved->authored().index().findOperation(node);
        const auto* boundary =
            authored ? std::get_if<AuthoredBoundary>(authored) : nullptr;
        if (!boundary) continue;

        /*
         * Pair identities first, then derive locations from the appropriate
         * side of the boundary; this avoids treating structure as execution.
         */
        for (const BoundaryPair& pair :
             boundaryPairs(state, operation)) {
            std::set<MemoryPlacement> memories =
                boundary->side == BoundarySide::Start
                    ? startMemories(state, operation, pair.target)
                    : endMemories(state, pair.source);

            /*
             * Only an evidence-free pair needs control ownership. Every
             * selected memory receives a symmetric source/target mapping.
             */
            addFallbackOwner(state, operation.region, memories);
            for (const MemoryPlacement& memory : memories) {
                placePair(state, node, pair, memory);
            }
        }
    }
}

}  // namespace vrt::graph::place_detail
