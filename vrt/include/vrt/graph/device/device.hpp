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
 * Devices lower scheduled queue slices directly into executable backend
 * programs. Cross-device actions are supplied through plan-owned action ids.
 */

#ifndef VRT_GRAPH_DEVICE_DEVICE_HPP
#define VRT_GRAPH_DEVICE_DEVICE_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <vrt/graph/backend_executable.hpp>
#include <vrt/graph/capabilities.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/ids.hpp>

namespace vrt::graph {

class GraphBuffer;
struct KernelDescriptor;

/*
 * Resource access is a non-owning view of physical slots. It is valid only
 * while the matching resource lease and device pin remain alive; retaining
 * the access object alone must not make a released slot usable.
 */
class IDeviceResourceAccess {
   public:
    virtual ~IDeviceResourceAccess() = default;
    virtual std::uint32_t readRendezvous(
        BackendResourceId resource) const = 0;
    virtual void writeRendezvous(
        BackendResourceId resource, std::uint32_t value) const = 0;
    virtual std::uint64_t readScalar(
        BackendScalarId scalar) const = 0;
    virtual void writeScalar(
        BackendScalarId scalar, std::uint64_t value) const = 0;
};

/*
 * An execution lease covers backend-wide mutable state, not one launch.
 * Exclusive backends keep it from lowering through final teardown so a
 * second live plan cannot alias queues, firmware state, or local buffers.
 */
class IDeviceExecutionLease {
   public:
    IDeviceExecutionLease() = default;
    IDeviceExecutionLease(const IDeviceExecutionLease&) = delete;
    IDeviceExecutionLease& operator=(const IDeviceExecutionLease&) = delete;
    IDeviceExecutionLease(IDeviceExecutionLease&&) noexcept = default;
    IDeviceExecutionLease& operator=(IDeviceExecutionLease&&) = delete;
    virtual ~IDeviceExecutionLease() = default;
};

/*
 * A resource lease owns the physical ids returned for one logical batch.
 * Rendezvous and scalar ids may share a backend namespace, so allocation and
 * release happen as one lease while the execution lease is still held.
 */
class IDeviceResourceLease {
   public:
    virtual ~IDeviceResourceLease() = default;
    virtual BackendResourceId rendezvousResource(
        RendezvousId logical) const = 0;
    virtual BackendScalarId scalarResource(
        ScalarResourceId logical) const = 0;
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
     * @brief Try to reserve this device for one live execution.
     *
     * The default is a no-op lease for simple and test devices. Backends with
     * execution-wide mutable state override this and return nullptr when an
     * execution is already live; implementations must not wait for release.
     */
    virtual std::unique_ptr<IDeviceExecutionLease> leaseExecution();

    /**
     * @brief Lease backend-owned physical rendezvous and scalar resources.
     *
     * The whole batch succeeds or fails together; callers retain the lease
     * for as long as any lowered command can address a returned physical id.
     */
    virtual std::unique_ptr<IDeviceResourceLease>
    leaseResources(
        const std::vector<RendezvousId>& rendezvous,
        const std::vector<ScalarResourceId>& scalars);

    std::unique_ptr<IDeviceResourceLease> leaseRendezvousResources(
        const std::vector<RendezvousId>& logical) {
        return leaseResources(logical, {});
    }

    /**
     * @brief Return plan-bindable host access to backend resources.
     */
    virtual std::shared_ptr<IDeviceResourceAccess> resourceAccess() const;

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
     * @brief Lower one complete scheduled queue into a direct executable.
     *
     * Production devices must return a queue-local executable.
     */
    virtual std::unique_ptr<IBackendExecutable> lowerQueue(
        const BackendLoweringContext&) {
        return nullptr;
    }

   protected:
    /*
     * Exclusive backends use a non-blocking compare/exchange: compilation
     * must report contention instead of waiting on an Execution whose
     * lifetime may itself depend on the caller.
     */
    static std::unique_ptr<IDeviceExecutionLease>
    tryAcquireExclusiveExecutionLease(std::atomic_bool& leased);
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DEVICE_DEVICE_HPP
