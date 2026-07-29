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
 * @file transfer.hpp
 * @brief Declarative transfer requirements, routes, and capability catalog.
 */

#ifndef VRT_GRAPH_TRANSFER_HPP
#define VRT_GRAPH_TRANSFER_HPP

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/capabilities.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/ids.hpp>

namespace vrt::graph {

class IDevice;

enum class TransferPayloadKind {
    Buffer,
    Scalar,
    Barrier,
};

enum class TransferMechanism {
    DirectBridge,
    HostBounce,
    HostMediatedDeviceCopy,
};

struct TransferEndpoint {
    std::optional<NodeId>         operation;
    DeviceId                      device;
    std::optional<MemoryRegionId> region;
};

struct TransferRequirement {
    RouteId                       id;
    std::optional<ValueId>        value;
    TransferPayloadKind           payload = TransferPayloadKind::Buffer;
    TransferEndpoint              source;
    TransferEndpoint              destination;
    std::optional<RouteId>        prerequisite;
};

struct TransferLeg {
    TransferMechanism       mechanism = TransferMechanism::DirectBridge;
    DeviceId                source;
    DeviceId                destination;
    std::optional<DeviceId> executor;
};

struct TransferRoute {
    TransferRequirement     requirement;
    std::vector<TransferLeg> legs;
};

class TransferCapabilityCatalog {
   public:
    static TransferCapabilityCatalog fromGraph(
        const std::map<std::string, std::shared_ptr<IDevice>>& devices,
        const std::map<std::pair<DeviceType, DeviceType>,
                       BridgeFactory>& bridgeFactories);

    bool hasDirect(DeviceId source, DeviceId destination) const;
    bool supportsMemoryRegionCopies(DeviceId device) const;
    const std::optional<DeviceId>& host() const { return host_; }

   private:
    std::set<std::pair<DeviceId, DeviceId>> direct_;
    std::set<DeviceId>                      memoryRegionCopyDevices_;
    std::optional<DeviceId>                 host_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_TRANSFER_HPP
