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
 * @file cpu_fpga_bridge.hpp
 * @brief CpuFpgaBridge — per-pair bridge for CPU ↔ FPGA transfers.
 *
 * One instance is constructed per concrete `(srcDevice, dstDevice)` pair
 * by the Graph's bridge-factory machinery. The bridge owns its private
 * `SemaphorePool` plus per-transfer staging buffers, and exposes its
 * primitives only as opaque closures returned in a `BridgeStepPair`.
 */

#ifndef VRT_GRAPH_CROSSDEVICE_CPU_FPGA_BRIDGE_HPP
#define VRT_GRAPH_CROSSDEVICE_CPU_FPGA_BRIDGE_HPP

#include <memory>

#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/crossdevice/semaphore_pool.hpp>

namespace vrt::graph {

class CpuDevice;
class FpgaDevice;

class CpuFpgaBridge : public IBridge {
   public:
    /**
     * @brief Construct a bridge bound to a specific (src, dst) device pair.
     *
     * Exactly one of the endpoints must be a CpuDevice; the other must be
     * an FpgaDevice.
     */
    CpuFpgaBridge(IDevice& src, IDevice& dst);

    BridgeStepPair makeTransfer(IDevice&            src,
                                IDevice&            dst,
                                const GraphBuffer&  buffer,
                                uint64_t            sizeHintBytes,
                                const std::string&  producerNodeId,
                                const std::string&  consumerNodeId) override;

    BridgeStepPair makeBarrier(IDevice&            src,
                                IDevice&            dst,
                                const std::string&  producerNodeId,
                                const std::string&  consumerNodeId) override;

   private:
    SemaphorePool pool_;
    CpuDevice*    srcCpu_ = nullptr;
    FpgaDevice*   srcFpga_ = nullptr;
    CpuDevice*    dstCpu_ = nullptr;
    FpgaDevice*   dstFpga_ = nullptr;
};

/**
 * @brief Convenience factory for `Graph::registerBridgeFactory`.
 *
 * Usage:
 *   g.registerBridgeFactory(DeviceType::CPU,  DeviceType::FPGA, CpuFpgaBridgeFactory());
 *   g.registerBridgeFactory(DeviceType::FPGA, DeviceType::CPU,  CpuFpgaBridgeFactory());
 */
inline BridgeFactory CpuFpgaBridgeFactory() {
    return [](IDevice& src, IDevice& dst) -> std::shared_ptr<IBridge> {
        return std::make_shared<CpuFpgaBridge>(src, dst);
    };
}

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CROSSDEVICE_CPU_FPGA_BRIDGE_HPP
