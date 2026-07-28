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
    std::uint32_t            physicalIndex = 0;
};

class BackendResourceBindings {
   public:
    BackendResourceBindings(
        std::map<RendezvousId, BoundRendezvous> rendezvous,
        std::vector<std::unique_ptr<IDeviceResourceLease>> leases,
        std::vector<std::shared_ptr<IDevice>> devicePins)
        : rendezvous_(std::move(rendezvous)),
          leases_(std::move(leases)),
          devicePins_(std::move(devicePins)) {}

    BackendResourceBindings(const BackendResourceBindings&) = delete;
    BackendResourceBindings& operator=(
        const BackendResourceBindings&) = delete;
    BackendResourceBindings(BackendResourceBindings&&) noexcept = default;
    BackendResourceBindings& operator=(
        BackendResourceBindings&&) noexcept = default;

    const std::map<RendezvousId, BoundRendezvous>& rendezvous() const {
        return rendezvous_;
    }

    const BoundRendezvous* find(RendezvousId logical) const {
        auto it = rendezvous_.find(logical);
        return it == rendezvous_.end() ? nullptr : &it->second;
    }

   private:
    std::map<RendezvousId, BoundRendezvous> rendezvous_;
    std::vector<std::unique_ptr<IDeviceResourceLease>> leases_;
    std::vector<std::shared_ptr<IDevice>> devicePins_;
};

CompileResult<BackendResourceBindings> bindBackendResources(
    const ScheduledGraph& scheduled,
    const std::map<std::string, std::shared_ptr<IDevice>>& devices);

}  // namespace vrt::graph

#endif  // VRT_GRAPH_BACKEND_RESOURCE_BINDING_HPP
