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

#include <vrt/graph/ir/placed_graph.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <vrt/graph/device/device.hpp>

namespace vrt::graph {

namespace {

class GraphPlacer {
   public:
    CompileResult<PlacedGraph> place(
        const ResolvedGraph& resolved,
        const DeviceCapabilityCatalog& capabilities) {
        resolved_ = std::make_shared<ResolvedGraph>(resolved);
        capabilities_ = &capabilities;
        indexRegion(resolved_->authored().root());
        placeRegion(resolved_->authored().root());
        placeValuesAndPorts();

        if (diagnostics_.hasErrors()) {
            return CompileResult<PlacedGraph>::failure(
                std::move(diagnostics_));
        }
        return CompileResult<PlacedGraph>::success(
            PlacedGraph(
                resolved_, std::move(operationPlacements_),
                std::move(controlPlacements_),
                std::move(valuePlacements_),
                std::move(portPlacements_),
                std::move(regionSummaries_)),
            std::move(diagnostics_));
    }

   private:
    void indexRegion(const AuthoredRegion& region) {
        authoredRegions_[region.id] = &region;
        for (const AuthoredOperation& operation : region.operations) {
            authoredOperations_[authoredNodeId(operation)] = &operation;
            if (const auto* loop = std::get_if<AuthoredLoop>(&operation)) {
                if (loop->body) indexRegion(*loop->body);
            } else if (const auto* conditional =
                           std::get_if<AuthoredConditional>(&operation)) {
                if (conditional->thenRegion) {
                    indexRegion(*conditional->thenRegion);
                }
                if (conditional->elseRegion) {
                    indexRegion(*conditional->elseRegion);
                }
            }
        }
    }

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

    bool placeKernel(const AuthoredRegion& region,
                     const AuthoredOperation& operation,
                     const AuthoredKernel& kernel,
                     RegionPlacementSummary& summary) {
        const DeviceCapabilities* capabilities =
            capabilities_->find(kernel.device);
        if (!capabilities) {
            diagnostics_.error(
                DiagCode::UnknownDevice,
                "GraphCompiler: op '" + kernel.authoredId +
                    "' requests unknown device '" +
                    kernel.device.value() + "'",
                location(region, operation));
            return false;
        }
        if (capabilities->kernelTypes.count(kernel.kernel.type) == 0) {
            diagnostics_.error(
                DiagCode::UnsupportedOperation,
                "GraphCompiler: device '" + kernel.device.value() +
                    "' does not support the kernel type requested by op '" +
                    kernel.authoredId + "'",
                location(region, operation));
            return false;
        }
        operationPlacements_[kernel.id] = {kernel.device};
        summary.devices.insert(kernel.device);
        summary.hasWork = true;
        return true;
    }

    bool placeReprogram(const AuthoredRegion& region,
                        const AuthoredOperation& operation,
                        const AuthoredReprogram& reprogram,
                        RegionPlacementSummary& summary) {
        const DeviceCapabilities* capabilities =
            capabilities_->find(reprogram.device);
        if (!capabilities) {
            diagnostics_.error(
                DiagCode::UnknownDevice,
                "GraphCompiler: reprogram op '" +
                    reprogram.authoredId +
                    "' requests unknown device '" +
                    reprogram.device.value() + "'",
                location(region, operation));
            return false;
        }
        if (!capabilities->supportsReprogram) {
            diagnostics_.error(
                DiagCode::UnsupportedOperation,
                "GraphCompiler: device '" +
                    reprogram.device.value() +
                    "' does not support reprogram operations",
                location(region, operation));
            return false;
        }
        operationPlacements_[reprogram.id] = {reprogram.device};
        summary.devices.insert(reprogram.device);
        summary.hasWork = true;
        return true;
    }

    RegionPlacementSummary combineChildren(
        const std::vector<const AuthoredRegion*>& children) const {
        RegionPlacementSummary result;
        for (const AuthoredRegion* child : children) {
            auto it = regionSummaries_.find(child->id);
            if (it == regionSummaries_.end()) continue;
            const RegionPlacementSummary& summary = it->second;
            result.devices.insert(summary.devices.begin(),
                                  summary.devices.end());
            result.hasWork |= summary.hasWork;
            result.hasNestedControl |= summary.hasNestedControl;
            result.hasDataBoundaries |= summary.hasDataBoundaries;
        }
        return result;
    }

    static std::vector<const AuthoredRegion*> childrenOf(
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

    static std::optional<std::pair<std::string, std::uint64_t>>
    conditionScalar(const Condition& condition) {
        auto scalar =
            [](const std::optional<ConditionOperand>& operand)
                -> std::optional<std::pair<std::string, std::uint64_t>> {
                if (!operand || !operand->isScalar()) {
                    return std::nullopt;
                }
                return std::make_pair(operand->name(),
                                      operand->scopeId());
            };
        if (auto lhs = scalar(condition.lhs())) return lhs;
        return scalar(condition.rhs());
    }

    bool predicateAvailableOnCandidate(
        const AuthoredOperation& operation,
        DeviceId candidate) const {
        const NodeId control = authoredNodeId(operation);
        const Condition* condition = nullptr;
        bool loop = false;
        if (const auto* authoredLoop =
                std::get_if<AuthoredLoop>(&operation)) {
            loop = true;
            if (authoredLoop->condition) {
                condition = &*authoredLoop->condition;
            }
        } else if (const auto* conditional =
                       std::get_if<AuthoredConditional>(&operation)) {
            condition = &conditional->condition;
        }
        if (!condition) return false;
        const auto scalar = conditionScalar(*condition);
        if (!scalar) return false;

        if (loop) {
            for (const ResolvedControlResult& result :
                 resolved_->controlResults()) {
                if (result.control != control) continue;
                const ResolvedValue* resultValue =
                    resolved_->findValue(result.result);
                if (!resultValue ||
                    resultValue->type.kind != ValueKind::Scalar ||
                    resultValue->sourceName != scalar->first) {
                    continue;
                }
                for (const ControlIncoming& incoming :
                     result.incoming) {
                    if (incoming.arm != ControlArm::LoopBackedge) {
                        continue;
                    }
                    const ResolvedValue* value =
                        resolved_->findValue(incoming.value);
                    if (!value || !value->producer) continue;
                    auto placement =
                        operationPlacements_.find(*value->producer);
                    if (placement != operationPlacements_.end() &&
                        placement->second.device == candidate) {
                        return true;
                    }
                }
            }
            return false;
        }

        const ResolvedOperation* resolvedControl =
            resolved_->findOperation(control);
        if (!resolvedControl) return false;
        for (const ResolvedBinding& binding :
             resolvedControl->bindings) {
            if (binding.access != ValueAccess::Condition) continue;
            const ResolvedValue* value =
                resolved_->findValue(binding.value);
            if (!value || value->sourceName != scalar->first ||
                !value->producer) {
                continue;
            }
            auto placement =
                operationPlacements_.find(*value->producer);
            if (placement != operationPlacements_.end() &&
                placement->second.device == candidate) {
                return true;
            }
        }
        return false;
    }

    ControlCapabilityRequest controlRequest(
        const AuthoredOperation& operation,
        DeviceId candidate,
        const RegionPlacementSummary& child) const {
        ControlCapabilityRequest request;
        request.candidate = candidate;
        request.childDevices.assign(child.devices.begin(),
                                    child.devices.end());
        request.childHasWork = child.hasWork;
        request.childHasNestedControl = child.hasNestedControl;
        request.childHasDataBoundaries =
            child.hasDataBoundaries;
        request.predicateAvailableOnCandidate =
            predicateAvailableOnCandidate(operation, candidate);

        if (const auto* loop = std::get_if<AuthoredLoop>(&operation)) {
            request.kind = ControlKind::Loop;
            request.loopKind = loop->kind;
            request.condition = loop->condition;
        } else {
            const auto& conditional =
                std::get<AuthoredConditional>(operation);
            request.kind = ControlKind::Conditional;
            request.condition = conditional.condition;
        }
        return request;
    }

    std::optional<ControlPlacement> splitLoopPlacement(
        const RegionPlacementSummary& child,
        std::vector<PlacementRejection>& rejections) const {
        if (child.devices.size() != 2 || child.hasNestedControl ||
            !child.hasWork) {
            return std::nullopt;
        }
        std::optional<DeviceId> authority;
        std::optional<DeviceId> follower;
        std::optional<DeviceId> primary;
        for (DeviceId device : child.devices) {
            const DeviceCapabilities* capabilities =
                capabilities_->find(device);
            if (!capabilities) return std::nullopt;
            if (capabilities->supportsSplitAuthority) {
                if (authority) return std::nullopt;
                authority = device;
            }
            if (capabilities->supportsSplitFollower) {
                if (follower) return std::nullopt;
                follower = device;
            }
            if (capabilities->prefersSplitPrimary) {
                primary = device;
            }
        }
        if (!authority || !follower || *authority == *follower) {
            rejections.push_back(
                {std::nullopt,
                 "child devices do not provide one split authority "
                 "and one split follower"});
            return std::nullopt;
        }

        ControlPlacement result;
        result.mode = ControlMode::SplitAcrossDevices;
        result.participants.assign(child.devices.begin(),
                                   child.devices.end());
        result.primary = primary.value_or(result.participants.front());
        result.authority = authority;
        result.followers = {*follower};
        result.rejections = rejections;
        return result;
    }

    ControlPlacement placeControl(
        const AuthoredRegion& region,
        const AuthoredOperation& operation,
        const RegionPlacementSummary& child) {
        std::vector<PlacementRejection> rejections;
        if (child.devices.size() == 1) {
            const DeviceId candidate = *child.devices.begin();
            const DeviceCapabilities* capabilities =
                capabilities_->find(candidate);
            if (capabilities &&
                capabilities->supportsAutonomousControl) {
                CapabilityDecision decision =
                    capabilities_->evaluateControl(
                        candidate,
                        controlRequest(operation, candidate, child));
                rejections.insert(
                    rejections.end(),
                    decision.rejections.begin(),
                    decision.rejections.end());
                if (decision.supported) {
                    ControlPlacement result;
                    result.mode =
                        ControlMode::AutonomousOnDevice;
                    result.participants = {candidate};
                    result.primary = candidate;
                    result.rejections = std::move(rejections);
                    return result;
                }
            }
        }

        if (std::holds_alternative<AuthoredLoop>(operation)) {
            if (auto split =
                    splitLoopPlacement(child, rejections)) {
                return *split;
            }
        }

        std::vector<DeviceId> hosts =
            capabilities_->fallbackControlDevices();
        if (hosts.size() != 1) {
            diagnostics_.error(
                DiagCode::AmbiguousPlacement,
                "GraphCompiler: control op '" +
                    authoredSourceId(operation) +
                    "' requires exactly one fallback control device",
                location(region, operation));
            ControlPlacement invalid;
            invalid.rejections = std::move(rejections);
            return invalid;
        }

        ControlPlacement result;
        result.mode = ControlMode::HostOwned;
        result.participants = hosts;
        result.primary = hosts.front();
        result.rejections = std::move(rejections);
        return result;
    }

    RegionPlacementSummary placeRegion(
        const AuthoredRegion& region) {
        for (const AuthoredOperation& operation : region.operations) {
            for (const AuthoredRegion* child : childrenOf(operation)) {
                placeRegion(*child);
            }
        }

        RegionPlacementSummary summary;
        summary.region = region.id;

        for (const AuthoredOperation& operation : region.operations) {
            if (const auto* kernel =
                    std::get_if<AuthoredKernel>(&operation)) {
                placeKernel(region, operation, *kernel, summary);
            } else if (const auto* reprogram =
                           std::get_if<AuthoredReprogram>(&operation)) {
                placeReprogram(region, operation, *reprogram, summary);
            } else if (const auto* boundary =
                           std::get_if<AuthoredBoundary>(&operation)) {
                summary.hasDataBoundaries |=
                    !boundary->scalarMappings.empty() ||
                    !boundary->bufferMappings.empty();
            }
        }

        for (const AuthoredOperation& operation : region.operations) {
            std::visit(
                [&](const auto& concrete) {
                    using T = std::decay_t<decltype(concrete)>;
                    if constexpr (std::is_same_v<T, AuthoredLoop> ||
                                  std::is_same_v<T, AuthoredConditional>) {
                        const RegionPlacementSummary children =
                            combineChildren(childrenOf(operation));
                        ControlPlacement placement =
                            placeControl(region, operation, children);
                        controlPlacements_[concrete.id] = placement;
                        operationPlacements_[concrete.id] = {
                            placement.primary};
                        summary.devices.insert(
                            placement.participants.begin(),
                            placement.participants.end());
                        summary.hasWork = true;
                        summary.hasNestedControl = true;
                    }
                },
                operation);
        }
        regionSummaries_[region.id] = summary;
        return summary;
    }

    static std::string backendPort(const std::string& bindingPort) {
        const std::size_t separator = bindingPort.find('.');
        return separator == std::string::npos
                   ? bindingPort
                   : bindingPort.substr(separator + 1);
    }

    void placeValuesAndPorts() {
        std::vector<DeviceId> ioHosts =
            capabilities_->graphIoHosts();
        std::optional<DeviceId> ioHost;
        if (ioHosts.size() == 1) {
            ioHost = ioHosts.front();
        } else {
            diagnostics_.error(
                DiagCode::AmbiguousPlacement,
                "GraphCompiler: graph values require exactly one graph I/O host");
        }

        for (const auto& [id, value] : resolved_->values()) {
            if (value.definition == ValueDefinitionKind::GraphInput) {
                if (ioHost) valuePlacements_[id] = {*ioHost, std::nullopt};
                continue;
            }
            if (!value.producer) continue;
            auto placement =
                operationPlacements_.find(*value.producer);
            if (placement != operationPlacements_.end()) {
                valuePlacements_[id] = {
                    placement->second.device, std::nullopt};
            }
        }

        for (const auto& [node, operation] :
             resolved_->operations()) {
            auto authored = authoredOperations_.find(node);
            auto placement = operationPlacements_.find(node);
            if (authored == authoredOperations_.end() ||
                placement == operationPlacements_.end()) {
                continue;
            }
            const auto* kernel =
                std::get_if<AuthoredKernel>(authored->second);
            if (!kernel) continue;

            for (const ResolvedBinding& binding :
                 operation.bindings) {
                const ResolvedValue* value =
                    resolved_->findValue(binding.value);
                if (!value ||
                    value->type.kind != ValueKind::Buffer) {
                    continue;
                }
                const std::string port =
                    backendPort(binding.port);
                std::optional<MemoryRegionId> region;
                try {
                    region = capabilities_->resolveMemoryRegion(
                        placement->second.device,
                        kernel->kernel, port);
                } catch (const std::runtime_error& error) {
                    diagnostics_.error(
                        DiagCode::IncompatibleMemoryPlacement,
                        error.what());
                }
                PortPlacement portPlacement;
                portPlacement.operation = node;
                portPlacement.port = port;
                portPlacement.value = binding.value;
                portPlacement.memory = {
                    placement->second.device, region};
                portPlacements_.push_back(portPlacement);

                if (binding.access == ValueAccess::Output ||
                    binding.access == ValueAccess::InoutOutput) {
                    valuePlacements_[binding.value] =
                        portPlacement.memory;
                }
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [node, operation] :
                 resolved_->operations()) {
                (void)node;
                if (!operation.structural) continue;
                std::map<std::string, ValueId> sources;
                for (const ResolvedBinding& binding :
                     operation.bindings) {
                    if (binding.access ==
                        ValueAccess::BoundarySource) {
                        sources[binding.port] = binding.value;
                    } else if (
                        binding.access ==
                        ValueAccess::BoundaryTarget) {
                        auto source = sources.find(binding.port);
                        if (source == sources.end()) continue;
                        auto placement =
                            valuePlacements_.find(source->second);
                        if (placement != valuePlacements_.end() &&
                            valuePlacements_.count(binding.value) == 0) {
                            valuePlacements_[binding.value] =
                                placement->second;
                            changed = true;
                        }
                    }
                }
            }
        }

        for (const ResolvedControlResult& result :
             resolved_->controlResults()) {
            std::set<MemoryPlacement, bool (*)(
                const MemoryPlacement&, const MemoryPlacement&)>
                incoming([](const MemoryPlacement& lhs,
                            const MemoryPlacement& rhs) {
                    return std::tie(lhs.device, lhs.region) <
                           std::tie(rhs.device, rhs.region);
                });
            for (const ControlIncoming& value : result.incoming) {
                if (value.arm == ControlArm::LoopInitial) continue;
                auto placement =
                    valuePlacements_.find(value.value);
                if (placement != valuePlacements_.end()) {
                    incoming.insert(placement->second);
                }
            }
            if (incoming.size() == 1) {
                valuePlacements_[result.result] = *incoming.begin();
            } else if (incoming.size() > 1 &&
                       !controlOutputHint(result.control,
                                          result.result)) {
                diagnostics_.error(
                    DiagCode::AmbiguousPlacement,
                    "GraphCompiler: control result has producers on "
                    "different devices and no output placement hint");
            }
        }

        applyControlOutputHints();
    }

    std::optional<DeviceId> controlOutputHint(
        NodeId control, ValueId result) const {
        auto authored = authoredOperations_.find(control);
        const ResolvedOperation* operation =
            resolved_->findOperation(control);
        const ResolvedValue* value = resolved_->findValue(result);
        if (authored == authoredOperations_.end() || !operation ||
            !value) {
            return std::nullopt;
        }
        const AuthoredPlacementHints* hints = std::visit(
            [](const auto& concrete)
                -> const AuthoredPlacementHints* {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<T, AuthoredLoop> ||
                              std::is_same_v<T,
                                             AuthoredConditional>) {
                    return &concrete.outputPlacement;
                } else {
                    return nullptr;
                }
            },
            *authored->second);
        if (!hints) return std::nullopt;
        const std::string prefix =
            value->type.kind == ValueKind::Buffer
                ? "buffer."
                : "scalar.";
        for (const ResolvedBinding& binding : operation->bindings) {
            if (binding.value != result ||
                binding.port.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            const std::string port = binding.port.substr(prefix.size());
            const auto& placements =
                value->type.kind == ValueKind::Buffer
                    ? hints->buffers
                    : hints->scalars;
            auto placement = placements.find(port);
            if (placement != placements.end()) {
                return placement->second;
            }
        }
        return std::nullopt;
    }

    void applyControlOutputHints() {
        for (const auto& [node, operation] :
             authoredOperations_) {
            const AuthoredPlacementHints* hints = std::visit(
                [](const auto& concrete)
                    -> const AuthoredPlacementHints* {
                    using T = std::decay_t<decltype(concrete)>;
                    if constexpr (std::is_same_v<T, AuthoredLoop> ||
                                  std::is_same_v<T, AuthoredConditional>) {
                        return &concrete.outputPlacement;
                    } else {
                        return nullptr;
                    }
                },
                *operation);
            if (!hints) continue;
            const ResolvedOperation* resolvedOperation =
                resolved_->findOperation(node);
            if (!resolvedOperation) continue;

            auto apply = [&](const std::string& prefix,
                             const std::map<std::string, DeviceId>& values) {
                for (const auto& [port, device] : values) {
                    if (!capabilities_->find(device)) {
                        diagnostics_.error(
                            DiagCode::UnknownDevice,
                            "GraphCompiler: output placement for port '" +
                                port + "' names unknown device '" +
                                device.value() + "'");
                        continue;
                    }
                    const std::string bindingPort = prefix + port;
                    auto binding = std::find_if(
                        resolvedOperation->bindings.begin(),
                        resolvedOperation->bindings.end(),
                        [&](const ResolvedBinding& candidate) {
                            return candidate.port == bindingPort &&
                                   (candidate.access ==
                                        ValueAccess::Output ||
                                    candidate.access ==
                                        ValueAccess::InoutOutput ||
                                    candidate.access ==
                                        ValueAccess::BoundaryTarget);
                        });
                    if (binding == resolvedOperation->bindings.end()) {
                        diagnostics_.error(
                            DiagCode::InvalidControlResult,
                            "GraphCompiler: output placement refers to "
                            "unknown port '" + port + "'");
                        continue;
                    }
                    valuePlacements_[binding->value] = {
                        device, std::nullopt};
                }
            };
            apply("buffer.", hints->buffers);
            apply("scalar.", hints->scalars);
        }
    }

    std::shared_ptr<const ResolvedGraph> resolved_;
    const DeviceCapabilityCatalog* capabilities_ = nullptr;
    Diagnostics diagnostics_;
    std::map<RegionId, const AuthoredRegion*> authoredRegions_;
    std::map<NodeId, const AuthoredOperation*> authoredOperations_;
    std::map<NodeId, DevicePlacement> operationPlacements_;
    std::map<NodeId, ControlPlacement> controlPlacements_;
    std::map<ValueId, MemoryPlacement> valuePlacements_;
    std::vector<PortPlacement> portPlacements_;
    std::map<RegionId, RegionPlacementSummary> regionSummaries_;
};

}  // namespace

CompileResult<PlacedGraph> placeGraph(
    const ResolvedGraph& resolved,
    const DeviceCapabilityCatalog& capabilities) {
    return GraphPlacer().place(resolved, capabilities);
}

}  // namespace vrt::graph
