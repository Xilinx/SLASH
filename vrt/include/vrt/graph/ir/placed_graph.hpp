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

/**
 * @file placed_graph.hpp
 * @brief Graph IR with explicit operation, control, value, and port placement.
 */

#ifndef VRT_GRAPH_IR_PLACED_GRAPH_HPP
#define VRT_GRAPH_IR_PLACED_GRAPH_HPP

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

#include <vrt/graph/capabilities.hpp>
#include <vrt/graph/compile_result.hpp>
#include <vrt/graph/ids.hpp>
#include <vrt/graph/ir/resolved_graph.hpp>

namespace vrt::graph {

struct DevicePlacement {
    DeviceId device;
};

struct MemoryPlacement {
    DeviceId                     device;
    std::optional<MemoryRegionId> region;

    friend bool operator==(const MemoryPlacement& lhs,
                           const MemoryPlacement& rhs) {
        return lhs.device == rhs.device && lhs.region == rhs.region;
    }

    friend bool operator<(const MemoryPlacement& lhs,
                          const MemoryPlacement& rhs) {
        return std::tie(lhs.device, lhs.region) <
               std::tie(rhs.device, rhs.region);
    }
};

struct PortPlacement {
    NodeId    operation;
    PortName  port;
    ReplicaId replica;
};

class HostControlPlacement {
   public:
    explicit HostControlPlacement(
        DeviceId host, std::vector<PlacementRejection> rejections = {})
        : host_(std::move(host)),
          participants_{host_},
          rejections_(std::move(rejections)) {
        if (host_.empty()) {
            throw std::invalid_argument(
                "host control placement requires a device");
        }
    }

    const DeviceId& host() const { return host_; }
    const std::vector<DeviceId>& participants() const {
        return participants_;
    }
    const std::vector<PlacementRejection>& rejections() const {
        return rejections_;
    }

   private:
    DeviceId                       host_;
    std::vector<DeviceId>          participants_;
    std::vector<PlacementRejection> rejections_;
};

class AutonomousControlPlacement {
   public:
    explicit AutonomousControlPlacement(
        DeviceId device, std::vector<PlacementRejection> rejections = {})
        : device_(std::move(device)),
          participants_{device_},
          rejections_(std::move(rejections)) {
        if (device_.empty()) {
            throw std::invalid_argument(
                "autonomous control placement requires a device");
        }
    }

    const DeviceId& device() const { return device_; }
    const std::vector<DeviceId>& participants() const {
        return participants_;
    }
    const std::vector<PlacementRejection>& rejections() const {
        return rejections_;
    }

   private:
    DeviceId                       device_;
    std::vector<DeviceId>          participants_;
    std::vector<PlacementRejection> rejections_;
};

class SplitControlPlacement {
   public:
    SplitControlPlacement(
        DeviceId authority, std::vector<DeviceId> followers,
        DeviceId primary,
        std::vector<PlacementRejection> rejections = {})
        : authority_(std::move(authority)),
          followers_(std::move(followers)),
          primary_(std::move(primary)),
          rejections_(std::move(rejections)) {
        if (authority_.empty() || followers_.empty()) {
            throw std::invalid_argument(
                "split control placement requires authority and followers");
        }
        participants_.push_back(authority_);
        participants_.insert(
            participants_.end(), followers_.begin(), followers_.end());
        std::sort(participants_.begin(), participants_.end());
        if (std::any_of(
                followers_.begin(), followers_.end(),
                [&](const DeviceId& follower) {
                    return follower.empty() || follower == authority_;
                }) ||
            std::adjacent_find(
                participants_.begin(), participants_.end()) !=
                participants_.end() ||
            std::find(participants_.begin(), participants_.end(), primary_) ==
                participants_.end()) {
            throw std::invalid_argument(
                "split control placement has inconsistent participants");
        }
    }

    const DeviceId& authority() const { return authority_; }
    const std::vector<DeviceId>& followers() const { return followers_; }
    const std::vector<DeviceId>& participants() const {
        return participants_;
    }
    const DeviceId& primary() const { return primary_; }
    const std::vector<PlacementRejection>& rejections() const {
        return rejections_;
    }

   private:
    DeviceId                       authority_;
    std::vector<DeviceId>          followers_;
    std::vector<DeviceId>          participants_;
    DeviceId                       primary_;
    std::vector<PlacementRejection> rejections_;
};

using ControlPlacement =
    std::variant<HostControlPlacement, AutonomousControlPlacement,
                 SplitControlPlacement>;

inline const DeviceId& controlPrimary(const ControlPlacement& placement) {
    return std::visit(
        [](const auto& concrete) -> const DeviceId& {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, HostControlPlacement>) {
                return concrete.host();
            } else if constexpr (
                std::is_same_v<T, AutonomousControlPlacement>) {
                return concrete.device();
            } else {
                return concrete.primary();
            }
        },
        placement);
}

inline const std::vector<DeviceId>& controlParticipants(
    const ControlPlacement& placement) {
    return std::visit(
        [](const auto& concrete) -> const std::vector<DeviceId>& {
            return concrete.participants();
        },
        placement);
}

inline const std::vector<PlacementRejection>& controlRejections(
    const ControlPlacement& placement) {
    return std::visit(
        [](const auto& concrete)
            -> const std::vector<PlacementRejection>& {
            return concrete.rejections();
        },
        placement);
}

enum class ReplicaPurpose {
    ValueDefinition,
    OperationInput,
    BoundarySource,
    BoundaryTarget,
    ControlResult,
    TransferTarget,
};

struct ValueReplica {
    ReplicaId                id;
    ValueId                  value;
    MemoryPlacement          memory;
    ReplicaPurpose           purpose = ReplicaPurpose::ValueDefinition;
    std::optional<NodeId>    operation;
    std::optional<PortName>  port;
};

struct BoundaryMappingPlacement {
    NodeId    boundary;
    PortName  port;
    ReplicaId source;
    ReplicaId target;
    DeviceId  owner;
};

struct RegionPlacementSummary {
    RegionId            region;
    std::set<DeviceId>  devices;
    bool                 hasWork = false;
    bool                 hasNestedControl = false;
    bool                 hasDataBoundaries = false;
};

class PlacedGraph {
   public:
    PlacedGraph(
        std::shared_ptr<const ResolvedGraph> resolved,
        std::map<NodeId, DevicePlacement> operationPlacements,
        std::map<NodeId, ControlPlacement> controlPlacements,
        std::map<ReplicaId, ValueReplica> replicas,
        std::map<ValueId, ReplicaId> primaryReplicas,
        std::vector<PortPlacement> portPlacements,
        std::vector<BoundaryMappingPlacement> boundaryMappings,
        std::map<RegionId, RegionPlacementSummary> regionSummaries)
        : resolved_(std::move(resolved)),
          operationPlacements_(std::move(operationPlacements)),
          controlPlacements_(std::move(controlPlacements)),
          replicas_(std::move(replicas)),
          primaryReplicas_(std::move(primaryReplicas)),
          portPlacements_(std::move(portPlacements)),
          boundaryMappings_(std::move(boundaryMappings)),
          regionSummaries_(std::move(regionSummaries)) {}

    const ResolvedGraph& resolved() const { return *resolved_; }

    const std::map<NodeId, DevicePlacement>& operationPlacements() const {
        return operationPlacements_;
    }

    const std::map<NodeId, ControlPlacement>& controlPlacements() const {
        return controlPlacements_;
    }

    const std::map<ReplicaId, ValueReplica>& replicas() const {
        return replicas_;
    }

    const ValueReplica* findReplica(ReplicaId id) const {
        auto it = replicas_.find(id);
        return it == replicas_.end() ? nullptr : &it->second;
    }

    const ValueReplica* primaryReplica(ValueId value) const {
        auto primary = primaryReplicas_.find(value);
        return primary == primaryReplicas_.end()
                   ? nullptr
                   : findReplica(primary->second);
    }

    const std::vector<PortPlacement>& portPlacements() const {
        return portPlacements_;
    }

    const std::vector<BoundaryMappingPlacement>& boundaryMappings() const {
        return boundaryMappings_;
    }

    const std::map<RegionId, RegionPlacementSummary>& regionSummaries() const {
        return regionSummaries_;
    }

   private:
    std::shared_ptr<const ResolvedGraph>         resolved_;
    std::map<NodeId, DevicePlacement>            operationPlacements_;
    std::map<NodeId, ControlPlacement>           controlPlacements_;
    std::map<ReplicaId, ValueReplica>             replicas_;
    std::map<ValueId, ReplicaId>                  primaryReplicas_;
    std::vector<PortPlacement>                   portPlacements_;
    std::vector<BoundaryMappingPlacement>         boundaryMappings_;
    std::map<RegionId, RegionPlacementSummary>   regionSummaries_;
};

CompileResult<PlacedGraph> placeGraph(
    const ResolvedGraph& resolved,
    const DeviceCapabilityCatalog& capabilities);

}  // namespace vrt::graph

#endif  // VRT_GRAPH_IR_PLACED_GRAPH_HPP
