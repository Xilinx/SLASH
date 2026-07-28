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

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
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
};

struct PortPlacement {
    NodeId          operation;
    std::string     port;
    ValueId         value;
    MemoryPlacement memory;
};

enum class ControlMode {
    HostOwned,
    AutonomousOnDevice,
    SplitAcrossDevices,
};

struct ControlPlacement {
    ControlMode                     mode = ControlMode::HostOwned;
    std::vector<DeviceId>           participants;
    DeviceId                        primary;
    std::optional<DeviceId>         authority;
    std::vector<DeviceId>           followers;
    std::vector<PlacementRejection> rejections;
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
        std::map<ValueId, MemoryPlacement> valuePlacements,
        std::vector<PortPlacement> portPlacements,
        std::map<RegionId, RegionPlacementSummary> regionSummaries)
        : resolved_(std::move(resolved)),
          operationPlacements_(std::move(operationPlacements)),
          controlPlacements_(std::move(controlPlacements)),
          valuePlacements_(std::move(valuePlacements)),
          portPlacements_(std::move(portPlacements)),
          regionSummaries_(std::move(regionSummaries)) {}

    const ResolvedGraph& resolved() const { return *resolved_; }

    const std::map<NodeId, DevicePlacement>& operationPlacements() const {
        return operationPlacements_;
    }

    const std::map<NodeId, ControlPlacement>& controlPlacements() const {
        return controlPlacements_;
    }

    const std::map<ValueId, MemoryPlacement>& valuePlacements() const {
        return valuePlacements_;
    }

    const std::vector<PortPlacement>& portPlacements() const {
        return portPlacements_;
    }

    const std::map<RegionId, RegionPlacementSummary>& regionSummaries() const {
        return regionSummaries_;
    }

   private:
    std::shared_ptr<const ResolvedGraph>         resolved_;
    std::map<NodeId, DevicePlacement>            operationPlacements_;
    std::map<NodeId, ControlPlacement>           controlPlacements_;
    std::map<ValueId, MemoryPlacement>           valuePlacements_;
    std::vector<PortPlacement>                   portPlacements_;
    std::map<RegionId, RegionPlacementSummary>   regionSummaries_;
};

CompileResult<PlacedGraph> placeGraph(
    const ResolvedGraph& resolved,
    const DeviceCapabilityCatalog& capabilities);

}  // namespace vrt::graph

#endif  // VRT_GRAPH_IR_PLACED_GRAPH_HPP
