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

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vrt::graph::place_detail {

DiagnosticLocation location(
    const AuthoredRegion& region,
    const AuthoredOperation& operation,
    std::optional<std::string> port) {
    DiagnosticLocation result;
    result.region = region.id;
    result.node = authoredNodeId(operation);
    result.authoredId = authoredSourceId(operation);
    result.port = std::move(port);
    return result;
}

namespace {

std::vector<const AuthoredRegion*> controlChildren(
    const PlacementState& state, const AuthoredRegion& region,
    NodeId control) {
    std::vector<const AuthoredRegion*> result;
    for (const AuthoredChildRegion& child :
         state.resolved->authored().index().children(region.id)) {
        if (child.control != control) continue;
        if (const AuthoredRegion* authored =
                state.resolved->authored().index().findRegion(child.region)) {
            result.push_back(authored);
        }
    }
    return result;
}

/*
 * Child summaries are available because regions are placed post-order.
 * Their union describes every device and structural constraint that the
 * parent control must coordinate, not merely its eventual primary device.
 */
RegionPlacementSummary combineChildren(
    const PlacementState& state,
    const std::vector<const AuthoredRegion*>& children) {
    RegionPlacementSummary result;
    for (const AuthoredRegion* child : children) {
        auto it = state.regionSummaries.find(child->id);
        if (it == state.regionSummaries.end()) continue;
        const RegionPlacementSummary& summary = it->second;
        result.devices.insert(
            summary.devices.begin(), summary.devices.end());
        result.hasWork |= summary.hasWork;
        result.hasNestedControl |= summary.hasNestedControl;
        result.hasDataBoundaries |= summary.hasDataBoundaries;
    }
    return result;
}

bool placeKernel(
    PlacementState& state, const AuthoredRegion& region,
    const AuthoredOperation& operation, const AuthoredKernel& kernel,
    RegionPlacementSummary& summary) {
    const DeviceCapabilities* capabilities =
        state.capabilities->find(kernel.device);
    if (!capabilities) {
        state.diagnostics.error(
            DiagCode::UnknownDevice,
            "GraphCompiler: op '" + kernel.authoredId +
                "' requests unknown device '" + kernel.device.value() + "'",
            location(region, operation));
        return false;
    }
    if (capabilities->kernelTypes.count(kernel.kernel.type) == 0) {
        state.diagnostics.error(
            DiagCode::UnsupportedOperation,
            "GraphCompiler: device '" + kernel.device.value() +
                "' does not support the kernel type requested by op '" +
                kernel.authoredId + "'",
            location(region, operation));
        return false;
    }
    state.operationPlacements[kernel.id] = {kernel.device};
    summary.devices.insert(kernel.device);
    summary.hasWork = true;
    return true;
}

bool placeReprogram(
    PlacementState& state, const AuthoredRegion& region,
    const AuthoredOperation& operation,
    const AuthoredReprogram& reprogram,
    RegionPlacementSummary& summary) {
    const DeviceCapabilities* capabilities =
        state.capabilities->find(reprogram.device);
    if (!capabilities) {
        state.diagnostics.error(
            DiagCode::UnknownDevice,
            "GraphCompiler: reprogram op '" + reprogram.authoredId +
                "' requests unknown device '" +
                reprogram.device.value() + "'",
            location(region, operation));
        return false;
    }
    if (!capabilities->supportsReprogram) {
        state.diagnostics.error(
            DiagCode::UnsupportedOperation,
            "GraphCompiler: device '" + reprogram.device.value() +
                "' does not support reprogram operations",
            location(region, operation));
        return false;
    }
    state.operationPlacements[reprogram.id] = {reprogram.device};
    summary.devices.insert(reprogram.device);
    summary.hasWork = true;
    return true;
}

/*
 * A split loop requires useful work on exactly two devices, with no nested
 * controller to coordinate recursively. The devices must supply one distinct
 * authority and follower; a preferred primary only chooses which participant
 * represents the control to generic placement consumers.
 */
std::optional<ControlPlacement> splitLoopPlacement(
    const PlacementState& state,
    const RegionPlacementSummary& child,
    std::vector<PlacementRejection> rejections) {
    if (child.devices.size() != 2 || child.hasNestedControl ||
        !child.hasWork) {
        return std::nullopt;
    }
    std::optional<DeviceId> authority;
    std::optional<DeviceId> follower;
    std::optional<DeviceId> primary;
    for (DeviceId device : child.devices) {
        const DeviceCapabilities* capabilities =
            state.capabilities->find(device);
        if (!capabilities) return std::nullopt;
        if (capabilities->supportsSplitAuthority) {
            if (authority) return std::nullopt;
            authority = device;
        }
        if (capabilities->supportsSplitFollower) {
            if (follower) return std::nullopt;
            follower = device;
        }
        if (capabilities->prefersSplitPrimary) primary = device;
    }
    if (!authority || !follower || *authority == *follower) {
        rejections.push_back(
            {std::nullopt,
             "child devices do not provide one split authority "
             "and one split follower"});
        return std::nullopt;
    }
    return SplitControlPlacement(
        *authority, {*follower},
        primary.value_or(*authority), std::move(rejections));
}

/*
 * Prefer the most local valid controller: autonomous control on the sole
 * child device, then a two-party split for eligible loops, then host control.
 * Capability rejections follow a successful placement for diagnostics. If
 * fallback discovery yields zero or multiple hosts, placement is ambiguous
 * rather than choosing one by enumeration order.
 */
std::optional<ControlPlacement> placeControl(
    PlacementState& state, const AuthoredRegion& region,
    const AuthoredOperation& operation,
    const RegionPlacementSummary& child) {
    std::vector<PlacementRejection> rejections;
    if (child.devices.size() == 1) {
        const DeviceId candidate = *child.devices.begin();
        const DeviceCapabilities* capabilities =
            state.capabilities->find(candidate);
        if (capabilities && capabilities->supportsAutonomousControl) {
            CapabilityDecision decision =
                state.capabilities->evaluateControl(
                    candidate,
                    controlRequest(state, operation, candidate, child));
            rejections.insert(
                rejections.end(), decision.rejections.begin(),
                decision.rejections.end());
            if (decision.supported) {
                return AutonomousControlPlacement(
                    candidate, std::move(rejections));
            }
        }
    }
    if (std::holds_alternative<AuthoredLoop>(operation)) {
        if (auto split =
                splitLoopPlacement(state, child, rejections)) {
            return split;
        }
    }
    const std::vector<DeviceId> hosts =
        state.capabilities->fallbackControlDevices();
    if (hosts.size() == 1) {
        return HostControlPlacement(
            hosts.front(), std::move(rejections));
    }
    state.diagnostics.error(
        DiagCode::AmbiguousPlacement,
        "GraphCompiler: control op '" + authoredSourceId(operation) +
            "' requires exactly one fallback control device",
        location(region, operation));
    return std::nullopt;
}

/*
 * Regions are placed post-order so each control sees complete child
 * summaries. Leaf work is placed before controls in the same region because
 * a parent's summary must include both ordinary operations and every control
 * participant selected from its descendants.
 */
RegionPlacementSummary placeRegion(
    PlacementState& state, const AuthoredRegion& region) {
    /*
     * Descend first; control placement below consumes these summaries as a
     * closed description of each child region.
     */
    for (const AuthoredChildRegion& child :
         state.resolved->authored().index().children(region.id)) {
        if (const AuthoredRegion* authored =
                state.resolved->authored().index().findRegion(child.region)) {
            placeRegion(state, *authored);
        }
    }

    /*
     * Place fixed-device work and record boundary constraints before asking
     * whether enclosing control can execute autonomously.
     */
    RegionPlacementSummary summary;
    summary.region = region.id;
    for (const AuthoredOperation& operation : region.operations) {
        if (const auto* kernel = std::get_if<AuthoredKernel>(&operation)) {
            placeKernel(state, region, operation, *kernel, summary);
        } else if (const auto* reprogram =
                       std::get_if<AuthoredReprogram>(&operation)) {
            placeReprogram(
                state, region, operation, *reprogram, summary);
        } else if (const auto* boundary =
                       std::get_if<AuthoredBoundary>(&operation)) {
            summary.hasDataBoundaries |=
                !boundary->scalarMappings.empty() ||
                !boundary->bufferMappings.empty();
        }
    }

    /*
     * Controls are a second phase: their candidates and capability requests
     * depend on the already settled child summaries.
     */
    for (const AuthoredOperation& operation : region.operations) {
        if (!std::holds_alternative<AuthoredLoop>(operation) &&
            !std::holds_alternative<AuthoredConditional>(operation)) {
            continue;
        }
        const RegionPlacementSummary children = combineChildren(
            state, controlChildren(
                       state, region, authoredNodeId(operation)));
        auto placement =
            placeControl(state, region, operation, children);
        if (!placement) continue;
        const NodeId node = authoredNodeId(operation);
        state.operationPlacements[node] = {controlPrimary(*placement)};
        summary.devices.insert(
            controlParticipants(*placement).begin(),
            controlParticipants(*placement).end());
        state.controlPlacements.emplace(node, std::move(*placement));
        summary.hasWork = true;
        summary.hasNestedControl = true;
    }
    state.regionSummaries[region.id] = summary;
    return summary;
}

}  // namespace

void placeRegions(PlacementState& state) {
    placeRegion(state, state.resolved->authored().root());
}

/*
 * Ordinary operations occupy one device, but split control is owned by all
 * participants. Boundary fallback and structural placement must preserve
 * that full set instead of collapsing it to the control primary.
 */
std::set<DeviceId> operationDevices(
    const PlacementState& state, NodeId operation) {
    std::set<DeviceId> result;
    auto control = state.controlPlacements.find(operation);
    if (control != state.controlPlacements.end()) {
        result.insert(
            controlParticipants(control->second).begin(),
            controlParticipants(control->second).end());
        return result;
    }
    auto placement = state.operationPlacements.find(operation);
    if (placement != state.operationPlacements.end()) {
        result.insert(placement->second.device);
    }
    return result;
}

}  // namespace vrt::graph::place_detail
