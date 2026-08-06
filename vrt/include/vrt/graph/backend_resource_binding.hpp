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
 * @file backend_resource_binding.hpp
 * @brief Device-owned resource leases for scheduled logical rendezvous.
 */

#ifndef VRT_GRAPH_BACKEND_RESOURCE_BINDING_HPP
#define VRT_GRAPH_BACKEND_RESOURCE_BINDING_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/compile_result.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/ids.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>

namespace vrt::graph {

enum class PhysicalRendezvousKind {
    HostEvent,
    DeviceResource,
};

struct BoundRendezvous {
    RendezvousId             logical;
    PhysicalRendezvousKind   kind =
        PhysicalRendezvousKind::HostEvent;
    DeviceId                 owner;
    BackendResourceId        physical;
};

struct BoundScalar {
    ScalarResourceId logical;
    ValueId          value;
    DeviceId         owner;
    std::string      key;
    BackendScalarId  physical;
};

/*
 * Resource binding is an ownership transaction, not just an id map.
 * Execution leases exclude incompatible live plans; resource leases reserve
 * physical slots; device pins keep both lease implementations valid.
 * Member order makes teardown run access handles -> resource leases ->
 * execution leases -> device pins, after backend executables are gone.
 */
class BackendResourceBindings {
   public:
    BackendResourceBindings(
        std::map<RendezvousId, BoundRendezvous> rendezvous,
        std::map<ScalarResourceId, BoundScalar> scalars,
        std::vector<std::shared_ptr<IDevice>> devicePins,
        std::vector<std::unique_ptr<IDeviceExecutionLease>>
            executionLeases,
        std::vector<std::unique_ptr<IDeviceResourceLease>>
            resourceLeases,
        std::map<DeviceId, std::shared_ptr<IDeviceResourceAccess>>
            ownerAccess)
        : rendezvous_(std::move(rendezvous)),
          scalars_(std::move(scalars)),
          devicePins_(std::move(devicePins)),
          executionLeases_(std::move(executionLeases)),
          resourceLeases_(std::move(resourceLeases)),
          ownerAccess_(std::move(ownerAccess)) {}

    BackendResourceBindings(const BackendResourceBindings&) = delete;
    BackendResourceBindings& operator=(
        const BackendResourceBindings&) = delete;
    BackendResourceBindings(BackendResourceBindings&&) noexcept = default;
    BackendResourceBindings& operator=(
        BackendResourceBindings&&) = delete;

    const std::map<RendezvousId, BoundRendezvous>& rendezvous() const {
        return rendezvous_;
    }

    const BoundRendezvous* find(RendezvousId logical) const {
        auto it = rendezvous_.find(logical);
        return it == rendezvous_.end() ? nullptr : &it->second;
    }

    const std::map<ScalarResourceId, BoundScalar>& scalars() const {
        return scalars_;
    }

    const BoundScalar* findScalar(
        DeviceId owner, const std::string& key) const {
        for (const auto& [logical, scalar] : scalars_) {
            (void)logical;
            if (scalar.owner == owner && scalar.key == key) return &scalar;
        }
        return nullptr;
    }

    const std::map<DeviceId, std::shared_ptr<IDeviceResourceAccess>>&
    ownerAccess() const {
        return ownerAccess_;
    }

   private:
    std::map<RendezvousId, BoundRendezvous> rendezvous_;
    std::map<ScalarResourceId, BoundScalar> scalars_;
    std::vector<std::shared_ptr<IDevice>> devicePins_;
    std::vector<std::unique_ptr<IDeviceExecutionLease>>
        executionLeases_;
    std::vector<std::unique_ptr<IDeviceResourceLease>>
        resourceLeases_;
    std::map<DeviceId, std::shared_ptr<IDeviceResourceAccess>>
        ownerAccess_;
};

CompileResult<BackendResourceBindings> bindBackendResources(
    const ScheduledGraph& scheduled,
    const std::map<std::string, std::shared_ptr<IDevice>>& devices);

}  // namespace vrt::graph

#endif  // VRT_GRAPH_BACKEND_RESOURCE_BINDING_HPP
