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
    const bool srcOk = (srcCpu_ != nullptr) || (srcFpga_ != nullptr);
    const bool dstOk = (dstCpu_ != nullptr) || (dstFpga_ != nullptr);
    const bool hasCpu = (srcCpu_ != nullptr) || (dstCpu_ != nullptr);
    const bool hasFpga = (srcFpga_ != nullptr) || (dstFpga_ != nullptr);
    if (!srcOk || !dstOk || !hasCpu || !hasFpga) {
        throw std::runtime_error(
            "CpuFpgaBridge: endpoints must be one CpuDevice and one FpgaDevice");
    }
}

BridgeStepPair CpuFpgaBridge::makeTransfer(IDevice&            /*src*/,
                                            IDevice&            /*dst*/,
                                            const GraphBuffer&  buffer,
                                            uint64_t            /*sizeHintBytes*/,
                                            const std::string&  /*producerNodeId*/,
                                            const std::string&  /*consumerNodeId*/) {
    auto op  = std::make_shared<CpuFpgaBridgeOp>();
    op->pool = &pool_;
    op->sem  = pool_.allocate();

    const std::string bufName = scopedBufferKey(buffer.scopeId(), buffer.name());
    auto* srcCpu  = srcCpu_;
    auto* srcFpga = srcFpga_;
    auto* dstCpu  = dstCpu_;
    auto* dstFpga = dstFpga_;

    // Producer: snapshot the source buffer into staging and signal.
    auto producerClosure = [op, srcCpu, srcFpga, bufName]() {
        const bool exists = srcCpu ? srcCpu->hasBuffer(bufName) : srcFpga->hasBuffer(bufName);
        if (std::getenv("VRT_FPGA_BUFFER_TRACE")) {
            std::cerr << "[fpga-buffer] bridge producer "
                      << (srcCpu ? "CPU" : "FPGA")
                      << " key=" << bufName
                      << " exists=" << exists << std::endl;
        }
        if (!exists) {
            throw std::runtime_error(
                "CpuFpgaBridge: producer-side buffer '" + bufName +
                "' does not exist at transfer time");
        }
        size_t sz = 0;
        if (srcCpu) {
            sz = srcCpu->bufferSize(bufName);
        } else {
            sz = srcFpga->bufferSize(bufName);
        }
        op->staging.resize(sz);
        if (sz > 0) {
            if (srcCpu) srcCpu->getOutputBuffer(bufName, op->staging.data(), sz);
            else        srcFpga->getOutputBuffer(bufName, op->staging.data(), sz);
        }
        op->pool->signal(op->sem);
    };

    // Split consumer into a non-blocking probe + the actual copy.
    auto tryReady = [op]() -> bool {
        return op->pool->tryAwait(op->sem);
    };
    auto consumerAction = [op, dstCpu, dstFpga, bufName]() {
        if (dstCpu) {
            dstCpu->setInputBuffer(bufName, op->staging.data(), op->staging.size());
        } else {
            dstFpga->setInputBuffer(bufName, op->staging.data(), op->staging.size());
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
    auto op  = std::make_shared<CpuFpgaScalarBridgeOp>();
    op->pool = &pool_;
    op->sem  = pool_.allocate();

    auto* srcCpu  = srcCpu_;
    auto* srcFpga = srcFpga_;
    auto* dstCpu  = dstCpu_;
    auto* dstFpga = dstFpga_;

    auto producerClosure = [op, srcCpu, srcFpga, scalarKey]() {
        op->bits = srcCpu ? srcCpu->getOutputScalar(scalarKey)
                          : srcFpga->getOutputScalar(scalarKey);
        op->pool->signal(op->sem);
    };

    auto tryReady = [op]() -> bool {
        return op->pool->tryAwait(op->sem);
    };
    auto consumerAction = [op, dstCpu, dstFpga, scalarKey]() {
        if (dstCpu) {
            dstCpu->setInputScalar(scalarKey, op->bits);
        } else {
            dstFpga->setInputScalar(scalarKey, op->bits);
        }
    };

    return BridgeStepPair{op,
                          std::move(producerClosure),
                          std::move(tryReady),
                          std::move(consumerAction)};
}

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
