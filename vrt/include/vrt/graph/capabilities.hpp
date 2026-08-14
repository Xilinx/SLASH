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
 * @file capabilities.hpp
 * @brief Backend-neutral device capabilities consumed by graph placement.
 */

#ifndef VRT_GRAPH_CAPABILITIES_HPP
#define VRT_GRAPH_CAPABILITIES_HPP

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <vrt/graph/control/condition.hpp>
#include <vrt/graph/control/control_node.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/ids.hpp>

namespace vrt::graph {

class IDevice;
struct KernelDescriptor;

enum class ControlKind {
    Loop,
    Conditional,
};

struct PlacementRejection {
    std::optional<DeviceId> device;
    std::string             reason;
};

struct DeviceCapabilities {
    DeviceId             device;
    std::string          backend;
    std::set<DeviceType> kernelTypes;
    bool                 hostsGraphIo = false;
    bool                 ownsFallbackControl = false;
    bool                 supportsReprogram = false;
    bool                 supportsAutonomousControl = false;
    bool                 supportsSplitAuthority = false;
    bool                 supportsSplitFollower = false;
    bool                 prefersSplitPrimary = false;
    bool                 supportsMemoryRegionCopies = false;
    bool                 ownsRendezvousNamespace = false;
};

struct ControlCapabilityRequest {
    ControlKind               kind = ControlKind::Loop;
    DeviceId                  candidate;
    std::optional<LoopKind>   loopKind;
    std::optional<Condition>  condition;
    std::vector<DeviceId>     childDevices;
    bool                      childHasWork = false;
    bool                      childHasNestedControl = false;
    bool                      childHasDataBoundaries = false;
    bool                      predicateAvailableOnCandidate = false;
};

struct CapabilityDecision {
    bool                            supported = false;
    std::vector<PlacementRejection> rejections;

    static CapabilityDecision accept() {
        CapabilityDecision result;
        result.supported = true;
        return result;
    }

    static CapabilityDecision reject(DeviceId device,
                                     std::string reason) {
        CapabilityDecision result;
        result.rejections.push_back(
            {std::move(device), std::move(reason)});
        return result;
    }
};

class DeviceCapabilityCatalog {
   public:
    static DeviceCapabilityCatalog fromDevices(
        const std::map<std::string, std::shared_ptr<IDevice>>& devices);

    const DeviceCapabilities* find(DeviceId id) const;
    const std::shared_ptr<IDevice>* findDevice(DeviceId id) const;
    std::vector<DeviceId> fallbackControlDevices() const;
    std::vector<DeviceId> graphIoHosts() const;

    CapabilityDecision evaluateControl(
        DeviceId device,
        const ControlCapabilityRequest& request) const;

    std::optional<MemoryRegionId> resolveMemoryRegion(
        DeviceId device, const KernelDescriptor& kernel,
        const std::string& port) const;

    const std::map<DeviceId, DeviceCapabilities>& entries() const {
        return capabilities_;
    }

   private:
    std::map<DeviceId, DeviceCapabilities>       capabilities_;
    std::map<DeviceId, std::shared_ptr<IDevice>> devices_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CAPABILITIES_HPP
