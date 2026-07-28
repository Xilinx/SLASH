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

#include <vrt/graph/backend_resource_binding.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace vrt::graph {

CompileResult<BackendResourceBindings> bindBackendResources(
    const ScheduledGraph& scheduled,
    const std::map<std::string, std::shared_ptr<IDevice>>& devices) {
    Diagnostics diagnostics;
    std::map<DeviceId, std::shared_ptr<IDevice>> devicesById;
    std::map<DeviceId, DeviceCapabilities> capabilities;
    for (const auto& [name, device] : devices) {
        if (!device) continue;
        const DeviceId id(name);
        devicesById[id] = device;
        DeviceCapabilities description =
            device->compilerCapabilities();
        description.device = id;
        capabilities[id] = std::move(description);
    }

    std::map<RendezvousId, BoundRendezvous> bindings;
    std::map<DeviceId, std::vector<RendezvousId>> byOwner;
    std::uint32_t nextHostEvent = 0;

    for (const LogicalResourceRequirement& requirement :
         scheduled.resources()) {
        std::vector<DeviceId> owners;
        std::optional<DeviceId> host;
        for (DeviceId participant : requirement.participants) {
            auto entry = capabilities.find(participant);
            if (entry == capabilities.end()) continue;
            if (entry->second.ownsRendezvousNamespace) {
                owners.push_back(participant);
            }
            if (entry->second.hostsGraphIo) host = participant;
        }
        if (owners.size() > 1) {
            diagnostics.error(
                DiagCode::AmbiguousPlacement,
                "GraphCompiler: logical rendezvous has multiple "
                "physical resource owners");
            continue;
        }
        if (owners.size() == 1) {
            byOwner[owners.front()].push_back(
                requirement.rendezvous);
            continue;
        }
        if (requirement.participants.empty()) {
            diagnostics.error(
                DiagCode::InternalInvariant,
                "GraphCompiler: logical rendezvous has no participants");
            continue;
        }
        const DeviceId owner =
            host.value_or(requirement.participants.front());
        bindings[requirement.rendezvous] = {
            requirement.rendezvous,
            PhysicalRendezvousKind::HostEvent,
            owner,
            nextHostEvent++};
    }

    std::vector<std::unique_ptr<IDeviceResourceLease>> leases;
    std::vector<std::shared_ptr<IDevice>> pins;
    for (auto& [owner, logical] : byOwner) {
        auto device = devicesById.find(owner);
        if (device == devicesById.end() || !device->second) {
            diagnostics.error(
                DiagCode::UnknownDevice,
                "GraphCompiler: rendezvous resource owner '" +
                    owner.value() + "' is unavailable");
            continue;
        }
        std::sort(logical.begin(), logical.end());
        logical.erase(std::unique(logical.begin(), logical.end()),
                      logical.end());
        try {
            std::unique_ptr<IDeviceResourceLease> lease =
                device->second->leaseRendezvousResources(logical);
            if (!lease) {
                diagnostics.error(
                    DiagCode::ResourceExhausted,
                    "GraphCompiler: device '" + owner.value() +
                        "' returned an empty rendezvous lease");
                continue;
            }
            std::set<std::uint32_t> physical;
            for (RendezvousId id : logical) {
                const std::uint32_t index =
                    lease->physicalIndex(id);
                if (!physical.insert(index).second) {
                    diagnostics.error(
                        DiagCode::InternalInvariant,
                        "GraphCompiler: device '" + owner.value() +
                            "' assigned one physical resource to "
                            "multiple live rendezvous");
                    continue;
                }
                bindings[id] = {
                    id, PhysicalRendezvousKind::DeviceResource,
                    owner, index};
            }
            pins.push_back(device->second);
            leases.push_back(std::move(lease));
        } catch (const std::exception& error) {
            diagnostics.error(
                DiagCode::ResourceExhausted,
                "GraphCompiler: device '" + owner.value() +
                    "' could not lease rendezvous resources: " +
                    error.what());
        }
    }

    if (diagnostics.hasErrors()) {
        return CompileResult<BackendResourceBindings>::failure(
            std::move(diagnostics));
    }
    return CompileResult<BackendResourceBindings>::success(
        BackendResourceBindings(
            std::move(bindings), std::move(leases),
            std::move(pins)),
        std::move(diagnostics));
}

}  // namespace vrt::graph
