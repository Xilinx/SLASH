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

#include <vrt/graph/crossdevice/cpu_fpga_bridge.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include <vrt/graph/crossdevice/bridge_op.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/fpga_device.hpp>

namespace vrt::graph {

namespace {

/**
 * @brief Bridge-private state for a single CPU↔FPGA transfer.
 */
/*
 * Queue callbacks share each operation through shared_ptr. The pool pointer is
 * borrowed from the bridge, which Execution keeps pinned until executable
 * callbacks and their operation state have been destroyed.
 */
struct CpuFpgaBridgeOp : IBridgeOp {
    SemaphorePool*       pool;
    SemaphoreHandle      sem;
    std::vector<uint8_t> staging;

    std::string label() const override { return "cpu_fpga_xfer"; }
};

struct CpuFpgaScalarBridgeOp : IBridgeOp {
    SemaphorePool*  pool;
    SemaphoreHandle sem;
    std::uint64_t   bits = 0;

    std::string label() const override { return "cpu_fpga_scalar_xfer"; }
};

struct CpuFpgaBarrierOp : IBridgeOp {
    SemaphorePool*  pool;
    SemaphoreHandle sem;
    std::string     label() const override { return "barrier"; }
};

}  // namespace

CpuFpgaBridge::CpuFpgaBridge(IDevice& src, IDevice& dst)
    : srcCpu_(dynamic_cast<CpuDevice*>(&src)),
      srcFpga_(dynamic_cast<FpgaDevice*>(&src)),
      dstCpu_(dynamic_cast<CpuDevice*>(&dst)),
      dstFpga_(dynamic_cast<FpgaDevice*>(&dst)) {
    /*
     * The only valid direction pairs are CPU -> FPGA and FPGA -> CPU.
     * Cache both typed views once so buffer callbacks need no RTTI and retain
     * the direction selected by the bridge factory.
     */
    const bool srcOk = (srcCpu_ != nullptr) || (srcFpga_ != nullptr);
    const bool dstOk = (dstCpu_ != nullptr) || (dstFpga_ != nullptr);
    const bool hasCpu = (srcCpu_ != nullptr) || (dstCpu_ != nullptr);
    const bool hasFpga = (srcFpga_ != nullptr) || (dstFpga_ != nullptr);
    if (!srcOk || !dstOk || !hasCpu || !hasFpga) {
        throw std::runtime_error(
            "CpuFpgaBridge: endpoints must be one CpuDevice and one FpgaDevice");
    }
}

BridgeStepPair CpuFpgaBridge::makeTransfer(
    IDevice& src, IDevice& dst, const GraphBuffer& buffer,
    uint64_t sizeHintBytes, const std::string& producerNodeId,
    const std::string& consumerNodeId) {
    return makeTransfer(
        src, dst, buffer, buffer, sizeHintBytes, producerNodeId,
        consumerNodeId);
}

/*
 * A buffer operation has three ordered phases:
 * - producer snapshots the source's current scoped storage into staging;
 * - the non-blocking probe consumes exactly one semaphore publication;
 * - consumer replaces the destination storage from that frozen snapshot.
 * The live storage size, not a compile-time hint, is authoritative here.
 */
BridgeStepPair CpuFpgaBridge::makeTransfer(
    IDevice& /*src*/, IDevice& /*dst*/, const GraphBuffer& source,
    const GraphBuffer& destination, uint64_t /*sizeHintBytes*/,
    const std::string& /*producerNodeId*/,
    const std::string& /*consumerNodeId*/) {
    auto op  = std::make_shared<CpuFpgaBridgeOp>();
    op->pool = &pool_;
    op->sem  = pool_.allocate();

    const std::string sourceKey =
        scopedBufferKey(source.scopeId(), source.name());
    const std::string destinationKey =
        scopedBufferKey(destination.scopeId(), destination.name());
    auto* srcCpu  = srcCpu_;
    auto* srcFpga = srcFpga_;
    auto* dstCpu  = dstCpu_;
    auto* dstFpga = dstFpga_;

    /*
     * Snapshot before signalling so the destination never observes source
     * storage while its owning backend may continue or replace it.
     */
    auto producerClosure = [op, srcCpu, srcFpga, sourceKey]() {
        const bool exists =
            srcCpu ? srcCpu->hasBuffer(sourceKey)
                   : srcFpga->hasBuffer(sourceKey);
        if (std::getenv("VRT_FPGA_BUFFER_TRACE")) {
            std::cerr << "[fpga-buffer] bridge producer "
                      << (srcCpu ? "CPU" : "FPGA")
                      << " key=" << sourceKey
                      << " exists=" << exists << std::endl;
        }
        if (!exists) {
            throw std::runtime_error(
                "CpuFpgaBridge: producer-side buffer '" + sourceKey +
                "' does not exist at transfer time");
        }
        size_t sz = 0;
        if (srcCpu) {
            sz = srcCpu->bufferSize(sourceKey);
        } else {
            sz = srcFpga->bufferSize(sourceKey);
        }
        op->staging.resize(sz);
        if (sz > 0) {
            if (srcCpu) {
                srcCpu->getOutputBuffer(
                    sourceKey, op->staging.data(), sz);
            } else {
                srcFpga->getOutputBuffer(
                    sourceKey, op->staging.data(), sz);
            }
        }
        op->pool->signal(op->sem);
    };

    /*
     * Keep readiness separate from publication: the CPU scheduler can poll
     * several blocked transfers, and only the successful probe may perform
     * the one destination copy for this signal.
     */
    auto tryReady = [op]() -> bool {
        return op->pool->tryAwait(op->sem);
    };
    auto consumerAction = [op, dstCpu, dstFpga, destinationKey]() {
        if (dstCpu) {
            dstCpu->setInputBuffer(
                destinationKey, op->staging.data(),
                op->staging.size());
        } else {
            dstFpga->setInputBuffer(
                destinationKey, op->staging.data(),
                op->staging.size());
        }
    };

    return BridgeStepPair{op,
                          std::move(producerClosure),
                          std::move(tryReady),
                          std::move(consumerAction)};
}

BridgeStepPair CpuFpgaBridge::makeScalarTransfer(IDevice&            /*src*/,
                                                  IDevice&            /*dst*/,
                                                  const std::string&  scalarKey,
                                                  const std::string&  /*producerNodeId*/,
                                                  const std::string&  /*consumerNodeId*/) {
    throw std::runtime_error(
        "CpuFpgaBridge::makeScalarTransfer: scalar transfers require "
        "execution-plan runtime state for '" + scalarKey + "'");
}

/*
 * Scalar transfer mirrors the buffer protocol without sharing scalar storage:
 * producer snapshots 64 raw bits through the source binding, the probe
 * consumes readiness, and consumer publishes those bits through the
 * destination binding. Scoped source and destination keys may differ.
 */
BridgeStepPair CpuFpgaBridge::makeScalarTransfer(
    IDevice& src, IDevice& dst, const std::string& scalarKey,
    const std::string& producerNodeId,
    const std::string& consumerNodeId,
    const BridgeRuntimeContext& runtime) {
    return makeScalarTransfer(
        src, dst, scalarKey, scalarKey, producerNodeId,
        consumerNodeId, runtime);
}

BridgeStepPair CpuFpgaBridge::makeScalarTransfer(
    IDevice& src, IDevice& dst, const std::string& sourceKey,
    const std::string& destinationKey,
    const std::string& /*producerNodeId*/,
    const std::string& /*consumerNodeId*/,
    const BridgeRuntimeContext& runtime) {
    if (!runtime.readScalar || !runtime.writeScalar) {
        throw std::invalid_argument(
            "CpuFpgaBridge::makeScalarTransfer: runtime scalar access is missing");
    }
    auto op  = std::make_shared<CpuFpgaScalarBridgeOp>();
    op->pool = &pool_;
    op->sem  = pool_.allocate();
    IDevice* source = &src;
    IDevice* destination = &dst;

    /* Freeze the producer value before making this transfer ready. */
    auto producerClosure =
        [op, source, sourceKey, read = runtime.readScalar]() {
        op->bits = read(*source, sourceKey);
        op->pool->signal(op->sem);
    };

    auto tryReady = [op]() -> bool {
        return op->pool->tryAwait(op->sem);
    };
    /*
     * Publication is deferred until readiness is consumed, preserving the
     * destination's previous value while the transfer is still pending.
     */
    auto consumerAction =
        [op, destination, destinationKey, write = runtime.writeScalar]() {
        write(*destination, destinationKey, op->bits);
    };

    return BridgeStepPair{op,
                          std::move(producerClosure),
                          std::move(tryReady),
                          std::move(consumerAction)};
}

/*
 * A barrier uses the same one-shot producer/probe handshake but carries no
 * payload. Its consumer action is intentionally empty: consuming the signal
 * is itself the ordering event.
 */
BridgeStepPair CpuFpgaBridge::makeBarrier(IDevice&            /*src*/,
                                           IDevice&            /*dst*/,
                                           const std::string&  /*producerNodeId*/,
                                           const std::string&  /*consumerNodeId*/) {
    auto op  = std::make_shared<CpuFpgaBarrierOp>();
    op->pool = &pool_;
    op->sem  = pool_.allocate();

    auto producer = [op]() { op->pool->signal(op->sem); };
    auto tryReady = [op]() { return op->pool->tryAwait(op->sem); };
    auto consumer = []() {};

    return BridgeStepPair{op,
                          std::move(producer),
                          std::move(tryReady),
                          std::move(consumer)};
}

}  // namespace vrt::graph
