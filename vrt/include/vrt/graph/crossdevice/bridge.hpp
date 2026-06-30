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
 * @file bridge.hpp
 * @brief IBridge — abstract interface for cross-device transfer and
 *        synchronisation between two devices.
 *
 * A bridge produces a pair of closures (producer-side + consumer-side) plus
 * an opaque `IBridgeOp` that owns whatever shared state the two closures
 * need. The compiler then synthesises a pair of `CompiledBridgeOpNode`s in the
 * relevant DGraphs from this returned data.
 */

#ifndef VRT_GRAPH_CROSSDEVICE_BRIDGE_HPP
#define VRT_GRAPH_CROSSDEVICE_BRIDGE_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <vrt/graph/crossdevice/bridge_op.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/core/types.hpp>

namespace vrt::graph {

/**
 * @brief Closure triple returned by `IBridge::makeTransfer` / `makeBarrier`.
 *
 * The consumer side is split into a non-blocking readiness probe
 * (`consumerTryReady`) and the actual copy (`consumerAction`) so that
 * dep-driven schedulers can poll many transfers without dedicating a
 * thread to each blocking await.
 */
struct BridgeStepPair {
    std::shared_ptr<IBridgeOp> op;
    std::function<void()>      producerAction;

    // TODO(review): consumerTryReady is conceptually two things — "is the
    //   semaphore signalled?" and "side-effect: reset the semaphore". For
    //   Phase 2 we let it do both (one closure = one poll attempt). Revisit
    //   once we have more bridges; we may want a separate `consumerCommit()`
    //   to make the reset explicit.
    /**
     * @brief Non-blocking readiness probe for the consumer side. Returns
     *        `true` exactly once per `signal` (it eats the signal); the
     *        caller may then invoke `consumerAction()` exactly once.
     *
     * Default = always-ready, suitable for bridges that have no
     * cross-device synchronisation.
     */
    std::function<bool()>      consumerTryReady = []{ return true; };

    /**
     * @brief The data-movement portion of the consumer side. Must be
     *        called only after a `consumerTryReady()` call returned `true`.
     */
    std::function<void()>      consumerAction;
};

class IBridge {
   public:
    virtual ~IBridge() = default;

    /**
     * @brief Build a producer/consumer closure pair for a cross-device
     *        transfer of @p buffer from @p src to @p dst.
     *
     * The bridge:
     *   1. Allocates whatever primitive state it needs (kept alive via a
     *      `shared_ptr<IBridgeOp>` subclass that overrides `label()`).
     *   2. Builds the producer closure, the consumer readiness probe, and
     *      the consumer copy closure, all capturing that state via the
     *      shared pointer.
     *   3. Returns the four pieces as a `BridgeStepPair`.
     *
     * The compiler is responsible for splicing the resulting closures into
     * the correct positions in the producer and consumer DGraphs as
     * CompiledBridgeOpNodes; the bridge does not touch the devices directly.
     */
    virtual BridgeStepPair makeTransfer(IDevice&            src,
                                         IDevice&            dst,
                                         const GraphBuffer&  buffer,
                                         uint64_t            sizeHintBytes,
                                         const std::string&  producerNodeId,
                                         const std::string&  consumerNodeId) = 0;

    /**
     * @brief Build a pure-synchronisation closure pair (no data movement).
     *
     * Used by the compiler to honour cross-device `afterNodes` constraints.
     * The expected pattern is:
     *   - `producerAction` signals a fresh semaphore from the bridge's pool.
     *   - `consumerTryReady` calls `tryAwait` on that semaphore.
     *   - `consumerAction` is a no-op.
     *
     * Every concrete IBridge MUST implement this; there is no
     * device-agnostic semaphore primitive we could fall back on.
     */
    virtual BridgeStepPair makeBarrier(IDevice&            src,
                                        IDevice&            dst,
                                        const std::string&  producerNodeId,
                                        const std::string&  consumerNodeId) = 0;

    /**
     * @brief Build the canonical key for a device-type pair (lower enum first).
     */
    static std::pair<DeviceType, DeviceType> makeKey(DeviceType a, DeviceType b) {
        if (static_cast<int>(a) <= static_cast<int>(b)) return {a, b};
        return {b, a};
    }
};

/**
 * @brief Factory used by `Graph::registerBridgeFactory` to lazily produce
 *        one bridge instance per concrete `(srcDevice, dstDevice)` pair.
 */
using BridgeFactory =
    std::function<std::shared_ptr<IBridge>(IDevice& src, IDevice& dst)>;

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CROSSDEVICE_BRIDGE_HPP
