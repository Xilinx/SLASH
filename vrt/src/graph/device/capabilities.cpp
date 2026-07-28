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

#include <vrt/graph/capabilities.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/device/device.hpp>
#include <vrt/graph/device/fpga/control_lowering.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>

namespace vrt::graph {

namespace {

std::mutex defaultLeaseMutex;
std::map<const IDevice*, std::set<std::uint32_t>> defaultLeaseSlots;

class DefaultResourceLease : public IDeviceResourceLease {
   public:
    DefaultResourceLease(
        const IDevice* device,
        std::map<RendezvousId, std::uint32_t> resources)
        : device_(device), resources_(std::move(resources)) {}

    ~DefaultResourceLease() override {
        std::lock_guard<std::mutex> lock(defaultLeaseMutex);
        auto state = defaultLeaseSlots.find(device_);
        if (state == defaultLeaseSlots.end()) return;
        for (const auto& [logical, physical] : resources_) {
            (void)logical;
            state->second.erase(physical);
        }
    }

    std::uint32_t physicalIndex(
        RendezvousId logical) const override {
        return resources_.at(logical);
    }

   private:
    const IDevice*                           device_ = nullptr;
    std::map<RendezvousId, std::uint32_t> resources_;
};

}  // namespace

DeviceCapabilities IDevice::compilerCapabilities() const {
    DeviceCapabilities result;
    result.device = DeviceId(id());
    switch (type()) {
        case DeviceType::CPU:
            result.backend = "cpu";
            result.kernelTypes.insert(DeviceType::CPU);
            result.hostsGraphIo = true;
            result.ownsFallbackControl = true;
            result.supportsSplitAuthority = true;
            break;
        case DeviceType::FPGA:
            result.backend = "fpga";
            result.kernelTypes.insert(DeviceType::FPGA);
            result.supportsReprogram = true;
            result.supportsAutonomousControl = true;
            result.supportsSplitFollower = true;
            result.prefersSplitPrimary = true;
            result.supportsMemoryRegionCopies = true;
            result.ownsRendezvousNamespace = true;
            break;
        case DeviceType::GPU:
            result.backend = "gpu";
            result.kernelTypes.insert(DeviceType::GPU);
            break;
        case DeviceType::MOCK_CPU:
            result.backend = "mock_cpu";
            result.kernelTypes.insert(DeviceType::MOCK_CPU);
            break;
    }
    return result;
}

CapabilityDecision IDevice::evaluateControlCapability(
    const ControlCapabilityRequest& request) const {
    if (type() == DeviceType::FPGA) {
        const DeviceId device(id());
        if (!request.childHasWork) {
            return CapabilityDecision::reject(
                device, "control body has no executable work");
        }
        if (request.childHasNestedControl) {
            return CapabilityDecision::reject(
                device, "nested control is not autonomous");
        }
        if (request.childDevices.size() != 1 ||
            request.childDevices.front() != device) {
            return CapabilityDecision::reject(
                device, "control body spans multiple devices");
        }
        if (request.kind == ControlKind::Loop &&
            request.loopKind == LoopKind::FixedCount) {
            return CapabilityDecision::accept();
        }
        if (!request.condition ||
            !fpga::isRp1EvaluableCondition(*request.condition)) {
            return CapabilityDecision::reject(
                device, "control predicate is not representable by RP1");
        }
        if (!request.predicateAvailableOnCandidate) {
            return CapabilityDecision::reject(
                device, "control predicate is unavailable on this device");
        }
        if (request.kind == ControlKind::Conditional &&
            request.childHasDataBoundaries) {
            return CapabilityDecision::reject(
                device, "conditional branches carry boundary data");
        }
        return CapabilityDecision::accept();
    }
    return CapabilityDecision::reject(
        DeviceId(id()),
        compilerCapabilities().supportsAutonomousControl
            ? "device has not implemented control capability evaluation"
            : "device does not support autonomous control");
}

std::unique_ptr<IDeviceResourceLease>
IDevice::leaseRendezvousResources(
    const std::vector<RendezvousId>& logical) {
    std::map<RendezvousId, std::uint32_t> resources;
    std::lock_guard<std::mutex> lock(defaultLeaseMutex);
    auto& used = defaultLeaseSlots[this];
    for (RendezvousId id : logical) {
        std::uint32_t physical = 0;
        while (used.count(physical) != 0) ++physical;
        used.insert(physical);
        resources[id] = physical;
    }
    return std::make_unique<DefaultResourceLease>(
        this, std::move(resources));
}

DeviceCapabilityCatalog DeviceCapabilityCatalog::fromDevices(
    const std::map<std::string, std::shared_ptr<IDevice>>& devices) {
    DeviceCapabilityCatalog result;
    for (const auto& [name, device] : devices) {
        if (!device) continue;
        const DeviceId id(name);
        DeviceCapabilities capabilities =
            device->compilerCapabilities();
        capabilities.device = id;
        result.capabilities_.emplace(id, std::move(capabilities));
        result.devices_.emplace(id, device);
    }
    return result;
}

const DeviceCapabilities* DeviceCapabilityCatalog::find(
    DeviceId id) const {
    auto it = capabilities_.find(id);
    return it == capabilities_.end() ? nullptr : &it->second;
}

const std::shared_ptr<IDevice>* DeviceCapabilityCatalog::findDevice(
    DeviceId id) const {
    auto it = devices_.find(id);
    return it == devices_.end() ? nullptr : &it->second;
}

std::vector<DeviceId>
DeviceCapabilityCatalog::fallbackControlDevices() const {
    std::vector<DeviceId> result;
    for (const auto& [id, capabilities] : capabilities_) {
        if (capabilities.ownsFallbackControl) result.push_back(id);
    }
    return result;
}

std::vector<DeviceId> DeviceCapabilityCatalog::graphIoHosts() const {
    std::vector<DeviceId> result;
    for (const auto& [id, capabilities] : capabilities_) {
        if (capabilities.hostsGraphIo) result.push_back(id);
    }
    return result;
}

CapabilityDecision DeviceCapabilityCatalog::evaluateControl(
    DeviceId device,
    const ControlCapabilityRequest& request) const {
    const std::shared_ptr<IDevice>* backend = findDevice(device);
    if (!backend || !*backend) {
        return CapabilityDecision::reject(
            std::move(device), "unknown device");
    }
    return (*backend)->evaluateControlCapability(request);
}

std::optional<MemoryRegionId>
DeviceCapabilityCatalog::resolveMemoryRegion(
    DeviceId device, const KernelDescriptor& kernel,
    const std::string& port) const {
    const std::shared_ptr<IDevice>* backend = findDevice(device);
    if (!backend || !*backend) return std::nullopt;
    std::optional<std::string> region =
        (*backend)->resolveMemoryRegion(kernel, port);
    if (!region || region->empty()) return std::nullopt;
    return MemoryRegionId(std::move(*region));
}

}  // namespace vrt::graph
