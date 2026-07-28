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
 * @file device.hpp
 * @brief IDevice — abstract execution interface for a single device instance.
 *
 * Cross-device synchronisation and data movement are realised as
 * CompiledBridgeOpNode entries in `DGraph::nodes` (synthesised by the compiler from
 * each registered IBridge). Devices compile those DGraphs into explicit
 * IDevicePlan objects so multiple plans can coexist for one device.
 */

#ifndef VRT_GRAPH_DEVICE_DEVICE_HPP
#define VRT_GRAPH_DEVICE_DEVICE_HPP

#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <vrt/graph/capabilities.hpp>
#include <vrt/graph/core/types.hpp>

namespace vrt::graph {

struct DGraph;
class GraphBuffer;
struct KernelDescriptor;

class IDeviceResourceLease {
   public:
    virtual ~IDeviceResourceLease() = default;
    virtual std::uint32_t physicalIndex(
        RendezvousId logical) const = 0;
};

class IDevicePlan {
   public:
    virtual ~IDevicePlan() = default;

    /** @brief Optional synchronous pre-launch preparation before any device starts. */
    virtual void prepareLaunch() {}

    /** @brief Start asynchronous execution of the compiled plan. */
    virtual void launch() = 0;

    /** @brief Block until the plan has completed. */
    virtual void wait() = 0;
};

class IDevice {
   public:
    virtual ~IDevice() = default;

    /** @brief Returns the device type (CPU, GPU, FPGA, …). */
    virtual DeviceType type() const = 0;

    /**
     * @brief Returns the unique device identifier, e.g. `"fpga:0"`.
     *
     * Matched against authored kernel placement during compilation.
     */
    virtual std::string id() const = 0;

    /**
     * @brief Return backend-neutral capabilities used by graph placement.
     */
    virtual DeviceCapabilities compilerCapabilities() const;

    /**
     * @brief Evaluate whether this device can own a concrete control shape.
     */
    virtual CapabilityDecision evaluateControlCapability(
        const ControlCapabilityRequest& request) const;

    /**
     * @brief Lease backend-owned physical resources for logical rendezvous.
     */
    virtual std::unique_ptr<IDeviceResourceLease>
    leaseRendezvousResources(
        const std::vector<RendezvousId>& logical);

    /**
     * @brief Optional memory-region identity for a kernel buffer port.
     *
     * Devices with banked local memories (for example FPGA HBM ports) can
     * return a stable region tag. Devices with a flat address space keep the
     * default `std::nullopt`, which disables same-device region routing.
     */
    virtual std::optional<std::string> resolveMemoryRegion(
        const KernelDescriptor& /*kernel*/, const std::string& /*portName*/) const {
        return std::nullopt;
    }

    /**
     * @brief Build an action that copies one device-local buffer replica to another.
     */
    virtual std::function<void()> makeDeviceCopyAction(
        const GraphBuffer& /*source*/, const GraphBuffer& /*target*/,
        BufferType /*type*/, const std::string& /*sourceRegion*/,
        const std::string& /*targetRegion*/) {
        throw std::logic_error("IDevice: device-local buffer copies are not supported");
    }

    /**
     * @brief Compile the per-device subgraph into an executable plan.
     *
     * `dg.nodes` is an ordered list of `CompiledNode` variants. The returned
     * plan owns the device-specific compiled execution state.
     */
    virtual std::unique_ptr<IDevicePlan> compilePlan(const DGraph& dg) = 0;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DEVICE_DEVICE_HPP
