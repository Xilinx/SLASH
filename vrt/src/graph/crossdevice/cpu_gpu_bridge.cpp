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

#include <vrt/graph/crossdevice/cpu_gpu_bridge.hpp>

#include <memory>
#include <stdexcept>
#include <vector>

#include <vrt/graph/crossdevice/bridge_op.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/gpu_device.hpp>

namespace vrt::graph {

namespace {

struct CpuGpuBridgeOp : IBridgeOp {
    SemaphorePool*       pool;
    SemaphoreHandle      sem;
    std::vector<uint8_t> staging;

    std::string label() const override { return "cpu_gpu_xfer"; }
};

struct CpuGpuBarrierOp : IBridgeOp {
    SemaphorePool*  pool;
    SemaphoreHandle sem;
    std::string     label() const override { return "barrier"; }
};

}  // namespace

CpuGpuBridge::CpuGpuBridge(IDevice& src, IDevice& dst)
    : srcCpu_(dynamic_cast<CpuDevice*>(&src)),
      srcGpu_(dynamic_cast<GpuDevice*>(&src)),
      dstCpu_(dynamic_cast<CpuDevice*>(&dst)),
      dstGpu_(dynamic_cast<GpuDevice*>(&dst)) {
    if (!(srcCpu_ || srcGpu_) || !(dstCpu_ || dstGpu_)) {
        throw std::runtime_error(
            "CpuGpuBridge: endpoints must be CpuDevice or GpuDevice");
    }
}

BridgeStepPair CpuGpuBridge::makeTransfer(IDevice&            /*src*/,
                                           IDevice&            /*dst*/,
                                           const GraphBuffer&  buffer,
                                           uint64_t            /*sizeHintBytes*/,
                                           const std::string&  /*producerNodeId*/,
                                           const std::string&  /*consumerNodeId*/) {
    auto op  = std::make_shared<CpuGpuBridgeOp>();
    op->pool = &pool_;
    op->sem  = pool_.allocate();

    const std::string bufName = buffer.name();
    auto* srcCpu = srcCpu_;
    auto* srcGpu = srcGpu_;
    auto* dstCpu = dstCpu_;
    auto* dstGpu = dstGpu_;

    auto producerClosure = [op, srcCpu, srcGpu, bufName]() {
        size_t sz = srcCpu ? srcCpu->bufferSize(bufName)
                           : srcGpu->bufferSize(bufName);
        op->staging.resize(sz);
        if (sz > 0) {
            if (srcCpu) srcCpu->getOutputBuffer(bufName, op->staging.data(), sz);
            else        srcGpu->getOutputBuffer(bufName, op->staging.data(), sz);
        }
        op->pool->signal(op->sem);
    };

    auto tryReady = [op]() { return op->pool->tryAwait(op->sem); };
    auto consumerAction = [op, dstCpu, dstGpu, bufName]() {
        if (dstCpu) dstCpu->setInputBuffer(bufName, op->staging.data(), op->staging.size());
        else        dstGpu->setInputBuffer(bufName, op->staging.data(), op->staging.size());
    };

    return BridgeStepPair{op,
                          std::move(producerClosure),
                          std::move(tryReady),
                          std::move(consumerAction)};
}

BridgeStepPair CpuGpuBridge::makeBarrier(IDevice&            /*src*/,
                                          IDevice&            /*dst*/,
                                          const std::string&  /*producerNodeId*/,
                                          const std::string&  /*consumerNodeId*/) {
    auto op  = std::make_shared<CpuGpuBarrierOp>();
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
