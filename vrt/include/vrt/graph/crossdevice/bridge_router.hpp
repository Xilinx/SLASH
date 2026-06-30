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
 * @file bridge_router.hpp
 * @brief BridgeRouter — routes cross-device transfers through direct
 *        bridges or bounces via the CPU when no direct path exists.
 *
 * `routeTransfer` returns one or two `RoutedLeg`s describing where each
 * CompiledBridgeOpNode should be spliced into the per-device DGraphs. The compiler
 * does the actual splicing; this header is purely policy.
 */

#ifndef VRT_GRAPH_CROSSDEVICE_BRIDGE_ROUTER_HPP
#define VRT_GRAPH_CROSSDEVICE_BRIDGE_ROUTER_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/core/types.hpp>

namespace vrt::graph {

/**
 * @brief One leg of a routed cross-device transfer.
 *
 * The compiler splices the producer-side CompiledBridgeOpNode immediately AFTER
 * `producerKernelId` in `srcDeviceId`'s DGraph, and the consumer-side
 * CompiledBridgeOpNode immediately BEFORE `consumerKernelId` in `dstDeviceId`'s.
 */
struct RoutedLeg {
    std::string    srcDeviceId;
    std::string    dstDeviceId;
    std::string    producerKernelId;  // anchor on src side
    std::string    consumerKernelId;  // anchor on dst side
    BridgeStepPair pair;
};

class BridgeRouter {
   public:
    /**
     * @brief Lookup callback that returns the bridge instance for a
     *        concrete (srcDeviceId, dstDeviceId) pair, lazily
     *        instantiating it from a registered factory if necessary.
     *
     * Returns @c nullptr if no factory is registered for the pair's
     * underlying device-type combination so callers can branch between
     * direct and bounce paths without exception-as-control-flow. Throws
     * for genuine errors (unknown device id, factory returning null, ...).
     */
    using BridgeFor =
        std::function<IBridge*(const std::string& srcDevId,
                               const std::string& dstDevId)>;

    /**
     * @brief Resolve a cross-device buffer transfer to one or more
     *        BridgeStepPair legs.
     *
     * If a bridge exists for {src.id(), dst.id()} (via the factory for
     * that DeviceType pair), returns a single direct leg using it.
     * Otherwise the transfer is split into two legs (src → cpu, then
     * cpu → dst) using the mandatory CPU↔T factories.
     */
    static std::vector<RoutedLeg> routeTransfer(
        IDevice&            src,
        IDevice&            dst,
        const GraphBuffer&  buffer,
        uint64_t            sizeHintBytes,
        const BridgeFor&    bridgeFor,
        IDevice&            cpuDevice,
        const std::string&  producerKernelId,
        const std::string&  consumerKernelId)
    {
        std::vector<RoutedLeg> legs;

        if (IBridge* direct = bridgeFor(src.id(), dst.id())) {
            auto pair = direct->makeTransfer(
                src, dst, buffer, sizeHintBytes,
                producerKernelId, consumerKernelId);
            legs.push_back(RoutedLeg{
                src.id(), dst.id(),
                producerKernelId, consumerKernelId,
                std::move(pair)});
            return legs;
        }

        // Bounce via CPU: src → cpu, then cpu → dst.
        IBridge* srcCpuBridge = bridgeFor(src.id(), cpuDevice.id());
        if (!srcCpuBridge) {
            throw std::runtime_error(
                "BridgeRouter: no bridge factory for {" + src.id() +
                ", cpu} — cannot bounce transfer of buffer '" +
                buffer.name() + "'");
        }

        IBridge* cpuDstBridge = bridgeFor(cpuDevice.id(), dst.id());
        if (!cpuDstBridge) {
            throw std::runtime_error(
                "BridgeRouter: no bridge factory for {cpu, " + dst.id() +
                "} — cannot bounce transfer of buffer '" +
                buffer.name() + "'");
        }

        // Leg 1: src → cpu
        auto leg1 = srcCpuBridge->makeTransfer(
            src, cpuDevice, buffer, sizeHintBytes,
            producerKernelId, consumerKernelId);
        legs.push_back(RoutedLeg{
            src.id(), cpuDevice.id(),
            producerKernelId, consumerKernelId,
            std::move(leg1)});

        // Leg 2: cpu → dst
        auto leg2 = cpuDstBridge->makeTransfer(
            cpuDevice, dst, buffer, sizeHintBytes,
            producerKernelId, consumerKernelId);
        legs.push_back(RoutedLeg{
            cpuDevice.id(), dst.id(),
            producerKernelId, consumerKernelId,
            std::move(leg2)});

        return legs;
    }
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CROSSDEVICE_BRIDGE_ROUTER_HPP
