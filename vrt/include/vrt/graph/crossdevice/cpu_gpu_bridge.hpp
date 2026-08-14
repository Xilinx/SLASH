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
 * @file cpu_gpu_bridge.hpp
 * @brief CpuGpuBridge — bridge for CPU ↔ GPU transfers.
 *
 * Only available when the VRT GPU backend is built (VRT_HAS_GPU).
 *
 * The bridge owns its own primitive state (a private `SemaphorePool` plus a
 * host-side staging buffer per transfer) and exposes that state to the
 * participating devices only as opaque `std::function<void()>` closures via
 * a `BridgeStepPair` returned to the compiler. The closures use the public
 * `setInputBuffer` /
 * `getOutputBuffer` accessors of the concrete device types to move data, so
 * no part of the GPU-specific machinery leaks into the device interface.
 */

#ifndef VRT_GRAPH_CROSSDEVICE_CPU_GPU_BRIDGE_HPP
#define VRT_GRAPH_CROSSDEVICE_CPU_GPU_BRIDGE_HPP

#if !defined(VRT_HAS_GPU) || (VRT_HAS_GPU == 0)
#error "cpu_gpu_bridge.hpp requires VRT_HAS_GPU; build VRT with VRT_ENABLE_GPU=ON."
#endif

#include <memory>

#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/crossdevice/semaphore_pool.hpp>

namespace vrt::graph {

class CpuDevice;
class GpuDevice;

class CpuGpuBridge : public IBridge {
   public:
    /**
     * @brief Construct a bridge bound to a specific (src, dst) pair where
     *        each endpoint is either a CpuDevice or a GpuDevice.
     *
     * @throws std::runtime_error if neither endpoint is CPU or GPU.
     */
    CpuGpuBridge(IDevice& src, IDevice& dst);

    BridgeStepPair makeTransfer(IDevice&            src,
                                IDevice&            dst,
                                const GraphBuffer&  buffer,
                                uint64_t            sizeHintBytes,
                                const std::string&  producerNodeId,
                                const std::string&  consumerNodeId) override;

    BridgeStepPair makeScalarTransfer(IDevice&            src,
                                      IDevice&            dst,
                                      const std::string&  scalarKey,
                                      const std::string&  producerNodeId,
                                      const std::string&  consumerNodeId) override;

    BridgeStepPair makeBarrier(IDevice&            src,
                                IDevice&            dst,
                                const std::string&  producerNodeId,
                                const std::string&  consumerNodeId) override;

   private:
    SemaphorePool pool_;
    CpuDevice*    srcCpu_ = nullptr;
    GpuDevice*    srcGpu_ = nullptr;
    CpuDevice*    dstCpu_ = nullptr;
    GpuDevice*    dstGpu_ = nullptr;
};

/**
 * @brief Convenience factory for `Graph::registerBridgeFactory`.
 */
inline BridgeFactory CpuGpuBridgeFactory() {
    return [](IDevice& src, IDevice& dst) -> std::shared_ptr<IBridge> {
        return std::make_shared<CpuGpuBridge>(src, dst);
    };
}

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CROSSDEVICE_CPU_GPU_BRIDGE_HPP
