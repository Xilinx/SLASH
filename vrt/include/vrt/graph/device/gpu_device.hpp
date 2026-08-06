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
 * @file gpu_device.hpp
 * @brief GpuDevice — ROCm/HIP Graph implementation of IDevice.
 *
 * Execution model
 * ---------------
 * Scheduled commands are lowered into a HIP Graph in a GpuDevicePlan. Each
 * kernel node becomes a hipGraphAddKernelNode.  Bridge-supplied opaque
 * plan-owned host action becomes a hipGraphAddHostNode callback.
 *
 * The HIP Graph is instantiated once per plan (hipGraphInstantiate) and
 * launched via hipGraphLaunch into a dedicated stream. Repeated launch()/wait()
 * cycles on the same plan reuse the instantiated executable graph.
 *
 * Buffer management
 * -----------------
 * Each GraphBuffer that appears as an output or input in the subgraph is
 * backed by a hipMalloc'd device allocation.  Graph-level input buffers are
 * staged via setInputBuffer() (host → device copy at launch time via graph
 * memcpy nodes).  Output buffers are read back via getOutputBuffer() after
 * wait().
 *
 * A private host-side buffer store is used for staging.  Bridges that need
 * cross-device data movement use the public setInputBuffer / getOutputBuffer
 * accessors from their closures.
 *
 * Kernel dispatch
 * ---------------
 * GPU kernels are registered before graph compilation. Each
 * registration provides:
 *   - A __global__ function pointer
 *   - A paramOrder vector mapping port names to kernel parameter positions
 *   - A grid/block configuration
 *
 * The compile step resolves detail::PortBindings bindings to device pointers and scalars,
 * packs them into a void** parameter array, and passes them to
 * hipGraphAddKernelNode.
 *
 * Cross-device synchronisation
 * ----------------------------
 * GpuDevice has no built-in sync primitives.  Bridges construct opaque
 * closures, returned to the compiler in a `BridgeStepPair` and spliced as
 * direct GPU commands into HIP host-node callbacks so they run in stream order.
 */

#ifndef VRT_GRAPH_DEVICE_GPU_DEVICE_HPP
#define VRT_GRAPH_DEVICE_GPU_DEVICE_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <hip/hip_runtime.h>

#include <vrt/graph/backend_runtime.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/detail/port_bindings.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>
#include <vrt/graph/core/types.hpp>

namespace vrt::graph {

// ---------------------------------------------------------------------------
// GpuKernelBinding — describes how to launch a registered GPU kernel
// ---------------------------------------------------------------------------

/**
 * @brief Binding between a graph kernel name and a HIP __global__ function.
 *
 * @p paramOrder maps detail::PortBindings port names (buffers and scalars) to positional
 * kernel parameters.  At launch time, each port name is resolved to either
 * a device pointer (for buffers) or a uint64_t value (for scalars), and
 * the resulting array is passed to hipGraphAddKernelNode.
 *
 * Example: for a kernel `__global__ void vadd(const float* a, const float* b, float* c, int n)`:
 * @code
 *   GpuKernelBinding binding;
 *   binding.func = reinterpret_cast<const void*>(&vadd);
 *   binding.paramOrder = {"a", "b", "c", "n"};  // port names in kernel arg order
 *   binding.block = dim3(256);
 *   binding.gridFn = [](size_t n) { return dim3((n + 255) / 256); };
 * @endcode
 */
struct GpuKernelBinding {
    /**
     * @brief Pointer to the __global__ function.
     */
    const void* func = nullptr;

    /**
     * @brief Port names in kernel parameter order.
     *
     * Each entry is looked up in the node's detail::PortBindings at compile time.  Buffer
     * ports resolve to device pointers; scalar ports resolve to uint64_t values.
     */
    std::vector<std::string> paramOrder;

    /**
     * @brief Thread block dimensions (fixed).
     */
    dim3 block{256};

    /**
     * @brief Compute grid dimensions from the element count of the first input buffer.
     *
     * If nullptr, a 1×1×1 grid is used.
     */
    std::function<dim3(size_t elemCount)> gridFn;
};

struct GpuKernelCommand {
    ScheduleStepId step;
    KernelDescriptor kernel;
    detail::PortBindings ioMap;
    std::vector<ScheduleStepId> dependencies;
};

struct GpuHostCommand {
    ScheduleStepId step;
    std::vector<ScheduleStepId> dependencies;
    std::vector<HostAction> actions;
};

using GpuCommand = std::variant<GpuKernelCommand, GpuHostCommand>;

struct GpuProgram {
    QueueId queue;
    DeviceId device;
    std::vector<GpuCommand> commands;
    std::shared_ptr<BackendRuntimeState> runtimeState;
};

// ---------------------------------------------------------------------------
// GpuDevice
// ---------------------------------------------------------------------------

class GpuDevice : public IDevice,
                  public std::enable_shared_from_this<GpuDevice> {
   public:
    /**
     * @brief Construct a GpuDevice.
     *
     * @param id            Logical device id, e.g. "gpu:0".
     * @param hipDeviceIdx  HIP device ordinal (passed to hipSetDevice).
     */
    explicit GpuDevice(std::string id,
                       int hipDeviceIdx = 0);

    ~GpuDevice() override;

    // Non-copyable, non-movable (owns HIP resources)
    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;
    GpuDevice(GpuDevice&&) = delete;
    GpuDevice& operator=(GpuDevice&&) = delete;

    // --- Kernel registration (call before compile) ---

    /**
     * @brief Register a GPU kernel implementation.
     *
     * @param kernelName  Must match KernelDescriptor::name for the node(s) that
     *                    should execute this function.
     * @param binding     Kernel function pointer, parameter mapping, and launch config.
     */
    void registerKernel(std::string kernelName, GpuKernelBinding binding);

    // --- Pre-populated input buffers ---

    /**
     * @brief Supply data for a graph-level input buffer (no producer node).
     *
     * Copies @p sizeBytes from @p data into the shared host buffer store.
     * During graph execution an H2D memcpy node transfers this data to the GPU.
     *
     * @param bufferName  Matches GraphBuffer::name() for the input token.
     * @param data        Source data pointer (host memory).
     * @param sizeBytes   Number of bytes to copy.
     */
    void setInputBuffer(const std::string& bufferName, const void* data, size_t sizeBytes);

    /**
     * @brief Read back an output buffer after wait() completes.
     *
     * Copies from the GPU device allocation to @p data (host memory).
     *
     * @param bufferName  GraphBuffer::name() of an output buffer.
     * @param data        Destination pointer (host memory).
     * @param sizeBytes   Number of bytes to copy out.
     */
    void getOutputBuffer(const std::string& bufferName, void* data, size_t sizeBytes) const;

    /**
     * @brief Returns the size of @p bufferName's host-staging copy, or 0 if absent.
     */
    size_t bufferSize(const std::string& bufferName) const;

    // --- IDevice ---

    DeviceType  type() const override { return DeviceType::GPU; }
    std::string id()   const override { return id_; }

    std::unique_ptr<IDeviceExecutionLease> leaseExecution() override;
    std::unique_ptr<IBackendExecutable> lowerQueue(
        const BackendLoweringContext& context) override;

   private:
    friend class GpuDevicePlan;

    // Device memory management
    void* ensureDeviceBuffer(const std::string& name, size_t sizeBytes);
    void  freeDeviceBuffers();

    std::string                                  id_;
    int                                          hipDeviceIdx_;
    std::map<std::string, std::vector<uint8_t>>  hostBuffers_;  // private host staging store

    std::map<std::string, GpuKernelBinding>      kernels_;
    std::map<std::string, void*>                 deviceBuffers_;  // name → hipMalloc'd ptr
    std::map<std::string, size_t>                deviceBufferSizes_;
    hipStream_t     stream_   = nullptr;
    std::atomic_bool executionLeased_{false};
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DEVICE_GPU_DEVICE_HPP
