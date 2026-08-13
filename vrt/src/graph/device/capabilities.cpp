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

namespace detail {
namespace {
thread_local bool backendWorkerActive = false;
}

BackendWorkerScope::BackendWorkerScope() noexcept
    : previous_(backendWorkerActive) {
    backendWorkerActive = true;
}

BackendWorkerScope::~BackendWorkerScope() {
    backendWorkerActive = previous_;
}

bool BackendWorkerScope::active() noexcept {
    return backendWorkerActive;
}

}  // namespace detail

namespace {

std::mutex defaultLeaseMutex;
std::map<const IDevice*, std::set<std::uint32_t>> defaultLeaseSlots;

class DefaultExecutionLease final : public IDeviceExecutionLease {};

class ExclusiveExecutionLease final : public IDeviceExecutionLease {
   public:
    explicit ExclusiveExecutionLease(std::atomic_bool& leased)
        : leased_(&leased) {}

    ~ExclusiveExecutionLease() override {
        leased_->store(false, std::memory_order_release);
    }

   private:
    std::atomic_bool* leased_;
};

class DefaultResourceLease : public IDeviceResourceLease {
   public:
    DefaultResourceLease(
        const IDevice* device,
        std::map<RendezvousId, BackendResourceId> rendezvous,
        std::map<ScalarResourceId, BackendScalarId> scalars)
        : device_(device),
          rendezvous_(std::move(rendezvous)),
          scalars_(std::move(scalars)) {}

    ~DefaultResourceLease() override {
        std::lock_guard<std::mutex> lock(defaultLeaseMutex);
        auto state = defaultLeaseSlots.find(device_);
        if (state == defaultLeaseSlots.end()) return;
        for (const auto& [logical, physical] : rendezvous_) {
            (void)logical;
            state->second.erase(
                static_cast<std::uint32_t>(physical.value()));
        }
        for (const auto& [logical, physical] : scalars_) {
            (void)logical;
            state->second.erase(
                static_cast<std::uint32_t>(physical.value()));
        }
    }

    BackendResourceId rendezvousResource(
        RendezvousId logical) const override {
        return rendezvous_.at(logical);
    }

    BackendScalarId scalarResource(
        ScalarResourceId logical) const override {
        return scalars_.at(logical);
    }

   private:
    const IDevice* device_ = nullptr;
    std::map<RendezvousId, BackendResourceId> rendezvous_;
    std::map<ScalarResourceId, BackendScalarId> scalars_;
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

std::unique_ptr<IDeviceExecutionLease> IDevice::leaseExecution() {
    return std::make_unique<DefaultExecutionLease>();
}

std::unique_ptr<IDeviceExecutionLease>
IDevice::tryAcquireExclusiveExecutionLease(std::atomic_bool& leased) {
    bool expected = false;
    if (!leased.compare_exchange_strong(
            expected, true, std::memory_order_acquire,
            std::memory_order_relaxed)) {
        return nullptr;
    }
    try {
        return std::make_unique<ExclusiveExecutionLease>(leased);
    } catch (...) {
        leased.store(false, std::memory_order_release);
        throw;
    }
}

std::unique_ptr<IDeviceResourceLease>
IDevice::leaseResources(
    const std::vector<RendezvousId>& rendezvous,
    const std::vector<ScalarResourceId>& scalars) {
    std::map<RendezvousId, BackendResourceId> rendezvousResources;
    std::map<ScalarResourceId, BackendScalarId> scalarResources;
    std::lock_guard<std::mutex> lock(defaultLeaseMutex);
    auto& used = defaultLeaseSlots[this];
    for (RendezvousId id : rendezvous) {
        std::uint32_t physical = 0;
        while (used.count(physical) != 0) ++physical;
        used.insert(physical);
        rendezvousResources[id] = BackendResourceId(physical);
    }
    for (ScalarResourceId id : scalars) {
        std::uint32_t physical = 0;
        while (used.count(physical) != 0) ++physical;
        used.insert(physical);
        scalarResources[id] = BackendScalarId(physical);
    }
    return std::make_unique<DefaultResourceLease>(
        this, std::move(rendezvousResources),
        std::move(scalarResources));
}

std::shared_ptr<IDeviceResourceAccess> IDevice::resourceAccess() const {
    return nullptr;
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
