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
 * A bridge produces the final runtime adapter: producer/consumer closures plus
 * an opaque `IBridgeOp` that owns their shared state. Backend lowering stores
 * those closures in the execution plan's host-action table.
 */

#ifndef VRT_GRAPH_CROSSDEVICE_BRIDGE_HPP
#define VRT_GRAPH_CROSSDEVICE_BRIDGE_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
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
/*
 * The producer snapshots payload state before signalling. A successful probe
 * consumes that signal and grants exactly one consumer action, which publishes
 * the snapshot at the destination. op is the explicit pin tying all three
 * callbacks to the same per-transfer state.
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

/*
 * Scalar callbacks route through execution-owned runtime state rather than
 * bridge-global storage: readScalar takes the producer snapshot and
 * writeScalar publishes it to the destination's host/device binding.
 */
struct BridgeRuntimeContext {
    std::function<std::uint64_t(IDevice&, const std::string&)>
        readScalar;
    std::function<void(IDevice&, const std::string&, std::uint64_t)>
        writeScalar;
};

class IBridge {
   public:
    /*
     * Graph pins each bridge instance for the whole Execution. This matters
     * for implementations whose operation objects point back to a bridge-owned
     * semaphore pool while queue executables retain only per-operation pins.
     */
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
     * Backend lowering stores the returned closures in plan-owned host actions.
     */
    virtual BridgeStepPair makeTransfer(IDevice&            src,
                                         IDevice&            dst,
                                         const GraphBuffer&  buffer,
                                         uint64_t            sizeHintBytes,
                                         const std::string&  producerNodeId,
                                         const std::string&  consumerNodeId) = 0;

    virtual BridgeStepPair makeTransfer(
        IDevice& src, IDevice& dst, const GraphBuffer& source,
        const GraphBuffer& destination, uint64_t sizeHintBytes,
        const std::string& producerNodeId,
        const std::string& consumerNodeId) {
        if (scopedBufferKey(source.scopeId(), source.name()) !=
            scopedBufferKey(
                destination.scopeId(), destination.name())) {
            throw std::logic_error(
                "IBridge: distinct transfer destinations are not supported");
        }
        return makeTransfer(
            src, dst, source, sizeHintBytes, producerNodeId,
            consumerNodeId);
    }

    /**
     * @brief Build a producer/consumer closure pair for a cross-device
     *        transfer of a scalar value identified by @p scalarKey.
     *
     * The scalar key is the fully-scoped key (`scope:N:name`). Concrete
     * devices expose scalar values through their native per-key storage
     * (CPU scalar map, FPGA signal slot, etc.); the bridge owns only the
     * synchronization/staging needed to move the 64-bit value.
     */
    virtual BridgeStepPair makeScalarTransfer(IDevice&            src,
                                               IDevice&            dst,
                                               const std::string&  scalarKey,
                                               const std::string&  producerNodeId,
                                               const std::string&  consumerNodeId) = 0;

    virtual BridgeStepPair makeScalarTransfer(
        IDevice& src, IDevice& dst, const std::string& scalarKey,
        const std::string& producerNodeId,
        const std::string& consumerNodeId,
        const BridgeRuntimeContext& runtime) {
        (void)runtime;
        return makeScalarTransfer(
            src, dst, scalarKey, producerNodeId, consumerNodeId);
    }

    virtual BridgeStepPair makeScalarTransfer(
        IDevice& src, IDevice& dst, const std::string& sourceKey,
        const std::string& destinationKey,
        const std::string& producerNodeId,
        const std::string& consumerNodeId,
        const BridgeRuntimeContext& runtime) {
        if (sourceKey != destinationKey) {
            throw std::logic_error(
                "IBridge: distinct scalar transfer destinations are not supported");
        }
        return makeScalarTransfer(
            src, dst, sourceKey, producerNodeId, consumerNodeId,
            runtime);
    }

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
