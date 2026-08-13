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
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace vrt::graph {

namespace {

struct ResourceGroups {
    std::map<RendezvousId, BoundRendezvous> hostEvents;
    std::map<DeviceId, std::vector<RendezvousId>> rendezvous;
    std::map<DeviceId, std::vector<ScalarResourceId>> scalars;
    std::map<ScalarResourceId, LogicalScalarRequirement>
        scalarRequirements;
};

struct BoundResources {
    std::map<RendezvousId, BoundRendezvous> bindings;
    std::map<ScalarResourceId, BoundScalar> scalars;
    std::vector<std::shared_ptr<IDevice>> pins;
    std::set<const IDevice*> pinnedDevices;
    std::vector<std::unique_ptr<IDeviceExecutionLease>>
        executionLeases;
    std::vector<std::unique_ptr<IDeviceResourceLease>>
        resourceLeases;
    std::map<DeviceId, std::shared_ptr<IDeviceResourceAccess>>
        ownerAccess;
};

/*
 * Rendezvous requirements have four cases:
 * - multiple namespace owners are ambiguous;
 * - one namespace owner gets a device resource;
 * - no owner gets a host event on the graph-I/O or first participant;
 * - no participant is an invariant failure.
 * Scalars are leased only from their explicitly named namespace owner.
 */
void indexRequirements(
    const ScheduledGraph& scheduled,
    const std::map<DeviceId, DeviceCapabilities>& capabilities,
    ResourceGroups& groups, Diagnostics& diagnostics) {
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
            groups.rendezvous[owners.front()].push_back(
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
        groups.hostEvents[requirement.rendezvous] = {
            requirement.rendezvous,
            PhysicalRendezvousKind::HostEvent,
            owner,
            BackendResourceId(nextHostEvent++)};
    }

    /*
     * Scalar requirements without a physical namespace remain host-backed;
     * only device-owned entries need physical binding metadata.
     */
    for (const LogicalScalarRequirement& requirement :
         scheduled.scalarResources()) {
        auto owner = capabilities.find(requirement.owner);
        if (owner == capabilities.end() ||
            !owner->second.ownsRendezvousNamespace) {
            continue;
        }
        groups.scalars[requirement.owner].push_back(
            requirement.scalar);
        groups.scalarRequirements.emplace(
            requirement.scalar, requirement);
    }
}

std::set<DeviceId> resourceOwners(
    const ResourceGroups& groups) {
    std::set<DeviceId> resourceOwners;
    for (const auto& [owner, logical] : groups.rendezvous) {
        (void)logical;
        resourceOwners.insert(owner);
    }
    for (const auto& [owner, logical] : groups.scalars) {
        (void)logical;
        resourceOwners.insert(owner);
    }
    return resourceOwners;
}

template <class Id>
void sortUnique(std::vector<Id>& ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

/*
 * Several registry names may refer to one device object. Pin by object
 * identity so teardown retains exactly one owning reference per device.
 */
void pinDevice(
    const std::shared_ptr<IDevice>& device, BoundResources& bound) {
    if (bound.pinnedDevices.insert(device.get()).second) {
        bound.pins.push_back(device);
    }
}

std::set<DeviceId> participatingDevices(
    const ScheduledGraph& scheduled) {
    std::set<DeviceId> result;
    for (const QueueProgram& queue : scheduled.queues()) {
        result.insert(queue.device);
    }
    for (const LogicalResourceRequirement& requirement :
         scheduled.resources()) {
        result.insert(
            requirement.participants.begin(),
            requirement.participants.end());
    }
    for (const LogicalScalarRequirement& requirement :
         scheduled.scalarResources()) {
        result.insert(requirement.owner);
    }
    return result;
}

/*
 * Lease every device touched by queues or resource protocols before binding
 * physical slots. Device aliases are leased once by pointer identity.
 * Missing devices, a non-blocking nullptr result, and backend exceptions all
 * become diagnostics; successful leases are retained with a device pin.
 */
void leaseExecutions(
    const ScheduledGraph& scheduled,
    const std::map<DeviceId, std::shared_ptr<IDevice>>& devices,
    BoundResources& bound, Diagnostics& diagnostics) {
    std::set<const IDevice*> leasedDevices;
    for (const DeviceId& id : participatingDevices(scheduled)) {
        auto entry = devices.find(id);
        if (entry == devices.end() || !entry->second) {
            diagnostics.error(
                DiagCode::UnknownDevice,
                "GraphCompiler: scheduled device '" + id.value() +
                    "' is unavailable");
            continue;
        }
        if (!leasedDevices.insert(entry->second.get()).second) {
            continue;
        }
        try {
            std::unique_ptr<IDeviceExecutionLease> lease =
                entry->second->leaseExecution();
            if (!lease) {
                diagnostics.error(
                    DiagCode::ResourceExhausted,
                    "GraphCompiler: device '" + id.value() +
                        "' already has a live execution");
                continue;
            }
            pinDevice(entry->second, bound);
            bound.executionLeases.push_back(std::move(lease));
        } catch (const std::exception& error) {
            diagnostics.error(
                DiagCode::ResourceExhausted,
                "GraphCompiler: device '" + id.value() +
                    "' could not lease an execution: " + error.what());
        }
    }
}

/*
 * Binding one owner is transactional:
 * - normalize logical ids and request one combined resource lease;
 * - reject physical aliasing across rendezvous and scalar ids;
 * - build logical mappings and capture host access;
 * - retain the device pin before committing the lease.
 * Any exception destroys the local lease before reporting failure.
 */
void bindOwner(
    const DeviceId& owner, const std::shared_ptr<IDevice>& device,
    ResourceGroups& groups, BoundResources& bound,
    Diagnostics& diagnostics) {
    auto& rendezvous = groups.rendezvous[owner];
    auto& scalars = groups.scalars[owner];
    sortUnique(rendezvous);
    sortUnique(scalars);
    try {
        std::unique_ptr<IDeviceResourceLease> lease =
            device->leaseResources(rendezvous, scalars);
        if (!lease) {
            diagnostics.error(
                DiagCode::ResourceExhausted,
                "GraphCompiler: device '" + owner.value() +
                    "' returned an empty resource lease");
            return;
        }
        /*
         * Backends may allocate both kinds from one physical namespace.
         * Check the combined set while translating logical ids.
         */
        std::set<std::uint32_t> physical;
        for (RendezvousId id : rendezvous) {
            const BackendResourceId resource =
                lease->rendezvousResource(id);
            const std::uint32_t index =
                static_cast<std::uint32_t>(resource.value());
            if (!physical.insert(index).second) {
                diagnostics.error(
                    DiagCode::InternalInvariant,
                    "GraphCompiler: device '" + owner.value() +
                        "' reused a live rendezvous slot");
                continue;
            }
            bound.bindings[id] = {
                id, PhysicalRendezvousKind::DeviceResource,
                owner, resource};
        }
        for (ScalarResourceId id : scalars) {
            const BackendScalarId resource =
                lease->scalarResource(id);
            const std::uint32_t index =
                static_cast<std::uint32_t>(resource.value());
            if (!physical.insert(index).second) {
                diagnostics.error(
                    DiagCode::InternalInvariant,
                    "GraphCompiler: device '" + owner.value() +
                        "' reused a live scalar/rendezvous slot");
                continue;
            }
            const LogicalScalarRequirement& requirement =
                groups.scalarRequirements.at(id);
            bound.scalars[id] = {
                id, requirement.value, owner,
                requirement.key, resource};
        }
        /*
         * Commit lifetime dependencies last. A later diagnostic destroys the
         * local BoundResources in safe access/lease/pin order.
         */
        pinDevice(device, bound);
        bound.ownerAccess[owner] = device->resourceAccess();
        bound.resourceLeases.push_back(std::move(lease));
    } catch (const std::exception& error) {
        diagnostics.error(
            DiagCode::ResourceExhausted,
            "GraphCompiler: device '" + owner.value() +
                "' could not lease resources: " + error.what());
    }
}

}  // namespace

/*
 * Binding proceeds in four phases:
 * - snapshot registered devices and capabilities;
 * - classify logical requirements and acquire execution-wide leases;
 * - bind each physical resource owner;
 * - move the complete maps, leases, access handles, and pins into the result.
 * No partial ownership escapes on a diagnostic.
 */
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
        capabilities[id] = device->compilerCapabilities();
    }

    /* Classify requirements before any backend resource allocation. */
    ResourceGroups groups;
    indexRequirements(
        scheduled, capabilities, groups, diagnostics);
    BoundResources bound;
    bound.bindings = std::move(groups.hostEvents);
    leaseExecutions(
        scheduled, devicesById, bound, diagnostics);
    if (diagnostics.hasErrors()) {
        return CompileResult<BackendResourceBindings>::failure(
            std::move(diagnostics));
    }
    /* Execution exclusivity is now held while owner slots are allocated. */
    for (const DeviceId& owner : resourceOwners(groups)) {
        auto device = devicesById.find(owner);
        if (device == devicesById.end() || !device->second) {
            diagnostics.error(
                DiagCode::UnknownDevice,
                "GraphCompiler: resource owner '" + owner.value() +
                    "' is unavailable");
            continue;
        }
        bindOwner(
            owner, device->second, groups, bound, diagnostics);
    }

    if (diagnostics.hasErrors()) {
        return CompileResult<BackendResourceBindings>::failure(
            std::move(diagnostics));
    }
    /* Success transfers the entire lifetime transaction to the caller. */
    return CompileResult<BackendResourceBindings>::success(
        BackendResourceBindings(
            std::move(bound.bindings), std::move(bound.scalars),
            std::move(bound.pins),
            std::move(bound.executionLeases),
            std::move(bound.resourceLeases),
            std::move(bound.ownerAccess)),
        std::move(diagnostics));
}

}  // namespace vrt::graph
