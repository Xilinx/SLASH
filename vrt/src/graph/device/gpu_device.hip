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
 * @file gpu_device.hip
 * @brief GpuDevice implementation — ROCm/HIP Graph GPU device backend.
 */

#include <vrt/graph/device/gpu_device.hpp>

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>

namespace vrt::graph {

// ---------------------------------------------------------------------------
// HIP error checking helper
// ---------------------------------------------------------------------------

namespace {

void hipCheck(hipError_t err, const char* context) {
    if (err != hipSuccess) {
        throw std::runtime_error(
            std::string("GpuDevice HIP error in ") + context + ": " + hipGetErrorString(err));
    }
}

}  // namespace

class GpuDevicePlan : public IDevicePlan {
   public:
    struct HostCallbackData {
        std::function<bool()> tryReady;
        std::function<void()> action;
    };

    GpuDevicePlan(GpuDevice& device, const DGraph& dg);
    ~GpuDevicePlan() override;

    void launch() override;
    void wait() override;

   private:
    struct KernelParamStore {
        struct ScalarParam {
            std::size_t index;
            GraphScalar scalar;
            std::string portName;
        };
        std::vector<void*> argPtrs;
        std::vector<uint64_t> scalarVals;
        std::vector<ScalarParam> scalarParams;
        std::vector<void*> devicePtrVals;
    };

    void destroyHipGraph();

    hipGraphNode_t addKernelNode(const CompiledKernelNode& node,
                                 const std::vector<hipGraphNode_t>& deps);
    hipGraphNode_t addOpHostNode(std::function<bool()> tryReady,
                                 std::function<void()> action,
                                 const std::vector<hipGraphNode_t>& deps);
    hipGraphNode_t addMemcpyH2DNode(const std::string& bufferName, size_t sizeBytes,
                                    const std::vector<hipGraphNode_t>& deps);
    hipGraphNode_t addMemcpyD2HNode(const std::string& bufferName, size_t sizeBytes,
                                    const std::vector<hipGraphNode_t>& deps);

    GpuDevice& device_;
    hipGraph_t hipGraph_ = nullptr;
    hipGraphExec_t hipExec_ = nullptr;
    std::map<std::string, hipGraphNode_t> hipNodeById_;
    std::vector<std::unique_ptr<KernelParamStore>> paramStores_;
    std::vector<std::unique_ptr<HostCallbackData>> callbackDatas_;
    std::shared_ptr<std::map<std::string, uint64_t>> scalarValues_;

    uint64_t scalarBits(const GraphScalar& scalar, const std::string& diagnostic) const;
    void refreshScalarParams();
};

// ---------------------------------------------------------------------------
// GpuDevice construction / destruction
// ---------------------------------------------------------------------------

GpuDevice::GpuDevice(std::string id, int hipDeviceIdx)
    : id_(std::move(id)),
      hipDeviceIdx_(hipDeviceIdx) {
    hipCheck(hipSetDevice(hipDeviceIdx_), "hipSetDevice in constructor");
    hipCheck(hipStreamCreate(&stream_), "hipStreamCreate");
}

GpuDevice::~GpuDevice() {
    freeDeviceBuffers();
    if (stream_) {
        hipStreamDestroy(stream_);
        stream_ = nullptr;
    }
}

std::unique_ptr<IDevicePlan> GpuDevice::compilePlan(const DGraph& dg) {
    return std::make_unique<GpuDevicePlan>(*this, dg);
}

GpuDevicePlan::~GpuDevicePlan() {
    if (hipExec_ && device_.stream_) {
        hipSetDevice(device_.hipDeviceIdx_);
        hipStreamSynchronize(device_.stream_);
    }
    destroyHipGraph();
}

void GpuDevicePlan::destroyHipGraph() {
    if (hipExec_) {
        hipGraphExecDestroy(hipExec_);
        hipExec_ = nullptr;
    }
    if (hipGraph_) {
        hipGraphDestroy(hipGraph_);
        hipGraph_ = nullptr;
    }
    paramStores_.clear();
    callbackDatas_.clear();
    hipNodeById_.clear();
}

void GpuDevice::freeDeviceBuffers() {
    hipSetDevice(hipDeviceIdx_);
    for (auto& [name, ptr] : deviceBuffers_) {
        if (ptr) hipFree(ptr);
    }
    deviceBuffers_.clear();
    deviceBufferSizes_.clear();
}

// ---------------------------------------------------------------------------
// Kernel registration
// ---------------------------------------------------------------------------

void GpuDevice::registerKernel(std::string kernelName, GpuKernelBinding binding) {
    kernels_[std::move(kernelName)] = std::move(binding);
}

// ---------------------------------------------------------------------------
// Input / output buffer management
// ---------------------------------------------------------------------------

void GpuDevice::setInputBuffer(const std::string& bufferName,
                                const void*        data,
                                size_t             sizeBytes) {
    auto& buf = hostBuffers_[bufferName];
    buf.resize(sizeBytes);
    if (data && sizeBytes > 0) {
        std::memcpy(buf.data(), data, sizeBytes);
    }
}

void GpuDevice::getOutputBuffer(const std::string& bufferName,
                                 void*              data,
                                 size_t             sizeBytes) const {
    hipSetDevice(hipDeviceIdx_);
    auto it = deviceBuffers_.find(bufferName);
    if (it == deviceBuffers_.end()) {
        throw std::runtime_error("GpuDevice::getOutputBuffer: unknown buffer '" + bufferName + "'");
    }
    auto sit = deviceBufferSizes_.find(bufferName);
    if (sit != deviceBufferSizes_.end() && sizeBytes > sit->second) {
        throw std::out_of_range("GpuDevice::getOutputBuffer: requested " +
                                std::to_string(sizeBytes) + " bytes but buffer '" +
                                bufferName + "' holds " + std::to_string(sit->second));
    }
    hipCheck(hipMemcpy(data, it->second, sizeBytes, hipMemcpyDeviceToHost),
             "getOutputBuffer hipMemcpy D2H");
}

size_t GpuDevice::bufferSize(const std::string& bufferName) const {
    auto sit = deviceBufferSizes_.find(bufferName);
    if (sit != deviceBufferSizes_.end()) return sit->second;
    auto hit = hostBuffers_.find(bufferName);
    if (hit != hostBuffers_.end())       return hit->second.size();
    return 0;
}

// ---------------------------------------------------------------------------
// Device memory management
// ---------------------------------------------------------------------------

void* GpuDevice::ensureDeviceBuffer(const std::string& name, size_t sizeBytes) {
    auto it = deviceBuffers_.find(name);
    if (it != deviceBuffers_.end()) {
        if (deviceBufferSizes_[name] >= sizeBytes) {
            return it->second;
        }
        // Need to grow — free and re-allocate
        hipFree(it->second);
        deviceBuffers_.erase(it);
        deviceBufferSizes_.erase(name);
    }

    void* ptr = nullptr;
    hipCheck(hipMalloc(&ptr, sizeBytes), ("hipMalloc for buffer '" + name + "'").c_str());
    deviceBuffers_[name] = ptr;
    deviceBufferSizes_[name] = sizeBytes;
    return ptr;
}

// ---------------------------------------------------------------------------
// Host callback for semaphore signal/await
// ---------------------------------------------------------------------------

namespace {

void hipHostOpCallback(void* userData) {
    auto* cbd = static_cast<GpuDevicePlan::HostCallbackData*>(userData);
    // Poll the bridge readiness probe before firing the action.
    // tryReady defaults to always-true on producer ops.
    while (!cbd->tryReady()) {}
    cbd->action();
}

}  // namespace

// ---------------------------------------------------------------------------
// HIP Graph node builders
// ---------------------------------------------------------------------------

hipGraphNode_t GpuDevicePlan::addOpHostNode(std::function<bool()> tryReady,
                                            std::function<void()> action,
                                            const std::vector<hipGraphNode_t>& deps) {
    auto cbd = std::make_unique<HostCallbackData>();
    cbd->tryReady = std::move(tryReady);
    cbd->action   = std::move(action);

    hipHostNodeParams params{};
    params.fn       = hipHostOpCallback;
    params.userData = cbd.get();

    hipGraphNode_t node;
    hipCheck(hipGraphAddHostNode(&node, hipGraph_,
                                 deps.empty() ? nullptr : deps.data(),
                                 deps.size(), &params),
             "hipGraphAddHostNode (op)");
    callbackDatas_.push_back(std::move(cbd));
    return node;
}

hipGraphNode_t GpuDevicePlan::addMemcpyH2DNode(const std::string& bufferName, size_t sizeBytes,
                                               const std::vector<hipGraphNode_t>& deps) {
    // Ensure the device buffer exists
    void* dptr = device_.ensureDeviceBuffer(bufferName, sizeBytes);

    // Source is the private host store
    auto hostIt = device_.hostBuffers_.find(bufferName);
    if (hostIt == device_.hostBuffers_.end()) {
        throw std::runtime_error(
            "GpuDevice: H2D memcpy source buffer '" + bufferName + "' not found in host store");
    }

    hipGraphNode_t node;
    hipMemcpy3DParms copyParams{};
    copyParams.srcArray = nullptr;
    copyParams.srcPos   = make_hipPos(0, 0, 0);
    copyParams.srcPtr   = make_hipPitchedPtr(hostIt->second.data(), sizeBytes, sizeBytes, 1);
    copyParams.dstArray = nullptr;
    copyParams.dstPos   = make_hipPos(0, 0, 0);
    copyParams.dstPtr   = make_hipPitchedPtr(dptr, sizeBytes, sizeBytes, 1);
    copyParams.extent   = make_hipExtent(sizeBytes, 1, 1);
    copyParams.kind     = hipMemcpyHostToDevice;

    hipCheck(hipGraphAddMemcpyNode(&node, hipGraph_,
                                   deps.empty() ? nullptr : deps.data(),
                                   deps.size(), &copyParams),
             ("hipGraphAddMemcpyNode H2D '" + bufferName + "'").c_str());
    return node;
}

hipGraphNode_t GpuDevicePlan::addMemcpyD2HNode(const std::string& bufferName, size_t sizeBytes,
                                               const std::vector<hipGraphNode_t>& deps) {
    auto devIt = device_.deviceBuffers_.find(bufferName);
    if (devIt == device_.deviceBuffers_.end()) {
        throw std::runtime_error(
            "GpuDevice: D2H memcpy source device buffer '" + bufferName + "' not found");
    }

    // Ensure host-side destination exists
    auto& hostBuf = device_.hostBuffers_[bufferName];
    if (hostBuf.size() < sizeBytes) hostBuf.resize(sizeBytes);

    hipGraphNode_t node;
    hipMemcpy3DParms copyParams{};
    copyParams.srcArray = nullptr;
    copyParams.srcPos   = make_hipPos(0, 0, 0);
    copyParams.srcPtr   = make_hipPitchedPtr(devIt->second, sizeBytes, sizeBytes, 1);
    copyParams.dstArray = nullptr;
    copyParams.dstPos   = make_hipPos(0, 0, 0);
    copyParams.dstPtr   = make_hipPitchedPtr(hostBuf.data(), sizeBytes, sizeBytes, 1);
    copyParams.extent   = make_hipExtent(sizeBytes, 1, 1);
    copyParams.kind     = hipMemcpyDeviceToHost;

    hipCheck(hipGraphAddMemcpyNode(&node, hipGraph_,
                                   deps.empty() ? nullptr : deps.data(),
                                   deps.size(), &copyParams),
             ("hipGraphAddMemcpyNode D2H '" + bufferName + "'").c_str());
    return node;
}

hipGraphNode_t GpuDevicePlan::addKernelNode(const CompiledKernelNode& node,
                                            const std::vector<hipGraphNode_t>& deps) {
    const std::string& kname = node.kernel.name;
    auto it = device_.kernels_.find(kname);
    if (it == device_.kernels_.end()) {
        throw std::runtime_error("GpuDevice: no kernel registered for '" + kname + "'");
    }
    const GpuKernelBinding& binding = it->second;

    // Build per-kernel parameter storage
    auto store = std::make_unique<KernelParamStore>();

    // Determine the element count from the first input buffer (for grid sizing)
    size_t firstInputElements = 1;
    if (!node.ioMap.inputs().empty()) {
        const std::string& firstName = node.ioMap.inputs().begin()->second.name();
        auto sit = device_.deviceBufferSizes_.find(firstName);
        if (sit != device_.deviceBufferSizes_.end()) {
            // Default to sizeof(float) elements if unknown
            firstInputElements = sit->second / sizeof(float);
        }
    }

    // Resolve each param in paramOrder to either a device pointer or a scalar
    // Build maps: portName → device pointer, portName → scalar value
    std::map<std::string, void*>    bufferPtrs;
    std::map<std::string, uint64_t> scalarVals;

    for (const auto& [portName, gbuf] : node.ioMap.inputs()) {
        auto dit = device_.deviceBuffers_.find(gbuf.name());
        if (dit != device_.deviceBuffers_.end()) {
            bufferPtrs[portName] = dit->second;
        }
    }
    for (const auto& [portName, gbuf] : node.ioMap.outputs()) {
        auto dit = device_.deviceBuffers_.find(gbuf.name());
        if (dit != device_.deviceBuffers_.end()) {
            bufferPtrs[portName] = dit->second;
        }
    }
    for (const auto& rwb : node.ioMap.inouts()) {
        auto inIt = device_.deviceBuffers_.find(rwb.in.name());
        if (inIt != device_.deviceBuffers_.end()) {
            bufferPtrs[rwb.inPort] = inIt->second;
        }
        auto outIt = device_.deviceBuffers_.find(rwb.out.name());
        if (outIt != device_.deviceBuffers_.end()) {
            bufferPtrs[rwb.outPort] = outIt->second;
        }
    }
    for (const auto& [portName, gs] : node.ioMap.inputScalars()) {
        if (!scalarValues_) {
            throw std::runtime_error(
                "GpuDevicePlan: scalar input '" + portName + "' requires a scalar value map");
        }
        auto sit = scalarValues_->find(scopedScalarKey(gs.scopeId(), gs.varName()));
        if (sit == scalarValues_->end() && gs.scopeId() == 0) {
            sit = scalarValues_->find(gs.varName());
        }
        if (sit == scalarValues_->end()) {
            throw std::runtime_error(
                "GpuDevicePlan: scalar '" + gs.varName() + "' not set before compile");
        }
        scalarVals[portName] = sit->second;
    }

    // Pack params in the order specified by the binding
    store->argPtrs.resize(binding.paramOrder.size());
    store->scalarVals.reserve(binding.paramOrder.size());
    for (size_t i = 0; i < binding.paramOrder.size(); ++i) {
        const std::string& paramName = binding.paramOrder[i];

        auto bufIt = bufferPtrs.find(paramName);
        if (bufIt != bufferPtrs.end()) {
            // It's a device pointer — store the pointer value and point argPtrs at it
            store->devicePtrVals.push_back(bufIt->second);
            store->argPtrs[i] = &store->devicePtrVals.back();
            continue;
        }

        auto scIt = scalarVals.find(paramName);
        if (scIt != scalarVals.end()) {
            const auto scalarBinding = node.ioMap.inputScalars().find(paramName);
            if (scalarBinding == node.ioMap.inputScalars().end()) {
                throw std::logic_error(
                    "GpuDevice: scalar param '" + paramName + "' lacks an input scalar binding");
            }
            const std::size_t scalarIndex = store->scalarVals.size();
            store->scalarVals.push_back(scIt->second);
            store->scalarParams.push_back(
                KernelParamStore::ScalarParam{scalarIndex, scalarBinding->second, paramName});
            store->argPtrs[i] = &store->scalarVals[scalarIndex];
            continue;
        }

        throw std::runtime_error(
            "GpuDevice: kernel '" + kname + "' paramOrder entry '" + paramName +
            "' not found in IOMap bindings");
    }

    // Compute grid dimensions
    dim3 grid(1, 1, 1);
    if (binding.gridFn) {
        grid = binding.gridFn(firstInputElements);
    }

    // Build the kernel node params
    hipKernelNodeParams kernelParams{};
    kernelParams.func           = const_cast<void*>(binding.func);
    kernelParams.gridDim        = grid;
    kernelParams.blockDim       = binding.block;
    kernelParams.sharedMemBytes = 0;
    kernelParams.kernelParams   = store->argPtrs.data();
    kernelParams.extra          = nullptr;

    hipGraphNode_t graphNode;
    hipCheck(hipGraphAddKernelNode(&graphNode, hipGraph_,
                                   deps.empty() ? nullptr : deps.data(),
                                   deps.size(), &kernelParams),
             ("hipGraphAddKernelNode '" + kname + "'").c_str());
    paramStores_.push_back(std::move(store));
    return graphNode;
}

uint64_t GpuDevicePlan::scalarBits(const GraphScalar& scalar,
                                   const std::string& diagnostic) const {
    if (!scalarValues_) {
        throw std::runtime_error(
            "GpuDevicePlan: scalar input '" + diagnostic + "' requires a scalar value map");
    }
    auto sit = scalarValues_->find(scopedScalarKey(scalar.scopeId(), scalar.varName()));
    if (sit == scalarValues_->end() && scalar.scopeId() == 0) {
        sit = scalarValues_->find(scalar.varName());
    }
    if (sit == scalarValues_->end()) {
        throw std::runtime_error(
            "GpuDevicePlan: scalar '" + scalar.varName() + "' not set before launch");
    }
    return sit->second;
}

void GpuDevicePlan::refreshScalarParams() {
    for (auto& store : paramStores_) {
        for (const auto& param : store->scalarParams) {
            store->scalarVals[param.index] = scalarBits(param.scalar, param.portName);
        }
    }
}

// ---------------------------------------------------------------------------
// compile
// ---------------------------------------------------------------------------

GpuDevicePlan::GpuDevicePlan(GpuDevice& device, const DGraph& dg)
    : device_(device), scalarValues_(dg.scalarValues) {
    hipCheck(hipSetDevice(device_.hipDeviceIdx_), "hipSetDevice in compilePlan");

    // --- Pre-allocate device buffers based on IOMap analysis (kernels only) ---

    for (const CompiledNode& node : dg.nodes) {
        if (!std::holds_alternative<CompiledKernelNode>(node)) continue;
        const CompiledKernelNode& k = std::get<CompiledKernelNode>(node);
        size_t defaultSize = 0;

        for (const auto& [portName, gbuf] : k.ioMap.inputs()) {
            auto hostIt = device_.hostBuffers_.find(gbuf.name());
            if (hostIt != device_.hostBuffers_.end()) {
                size_t sz = hostIt->second.size();
                device_.ensureDeviceBuffer(gbuf.name(), sz);
                if (defaultSize == 0) defaultSize = sz;
            }
        }

        for (const auto& [portName, gbuf] : k.ioMap.outputs()) {
            size_t sz = (defaultSize > 0) ? defaultSize : 1024;
            device_.ensureDeviceBuffer(gbuf.name(), sz);
        }

        for (const auto& rwb : k.ioMap.inouts()) {
            auto hostIt = device_.hostBuffers_.find(rwb.in.name());
            size_t sz = (hostIt != device_.hostBuffers_.end()) ? hostIt->second.size() : defaultSize;
            device_.ensureDeviceBuffer(rwb.in.name(), sz);
            device_.ensureDeviceBuffer(rwb.out.name(), sz);
        }
    }

    // Classify buffers: which are produced (and which are consumed) by some
    // kernel on *this* DGraph. Used to decide whether to inject an H2D copy
    // for an input or a D2H copy for an output.
    std::set<std::string> producedHere;
    std::set<std::string> consumedHere;
    for (const CompiledNode& node : dg.nodes) {
        if (!std::holds_alternative<CompiledKernelNode>(node)) continue;
        const CompiledKernelNode& k = std::get<CompiledKernelNode>(node);
        for (const auto& [_, gbuf] : k.ioMap.outputs()) producedHere.insert(gbuf.name());
        for (const auto& rwb : k.ioMap.inouts())           producedHere.insert(rwb.out.name());
        for (const auto& [_, gbuf] : k.ioMap.inputs())  consumedHere.insert(gbuf.name());
        for (const auto& rwb : k.ioMap.inouts())           consumedHere.insert(rwb.in.name());
    }

    // --- Build HIP Graph ---

    hipCheck(hipGraphCreate(&hipGraph_, 0), "hipGraphCreate");

    // Iterate dg.nodes in topological order (the compiler emits them that
    // way). For every node, translate its `dependsOn` ids into HIP graph
    // node handles via hipNodeById_; ids not local to this DGraph are
    // ignored \u2014 cross-DGraph synchronisation is enforced by the bridge's
    // tryReady probe inside host-node callbacks (matches CpuDevice
    // semantics).
    auto translateDeps = [&](const std::vector<std::string>& depIds) {
        std::vector<hipGraphNode_t> out;
        out.reserve(depIds.size());
        for (const std::string& id : depIds) {
            auto it = hipNodeById_.find(id);
            if (it != hipNodeById_.end()) out.push_back(it->second);
        }
        return out;
    };

    // Track the last D2H per buffer so subsequent kernels reading the same
    // graph-level input share one H2D copy instead of re-staging it.
    std::map<std::string, hipGraphNode_t> h2dByBuffer;

    for (const CompiledNode& node : dg.nodes) {
        std::visit(
            [&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                std::vector<hipGraphNode_t> deps = translateDeps(n.dependsOn);

                if constexpr (std::is_same_v<T, CompiledKernelNode>) {
                    // For each input buffer NOT produced locally and present
                    // in the host store, inject (or reuse) an H2D copy and
                    // chain the kernel after it.
                    for (const auto& [_, gbuf] : n.ioMap.inputs()) {
                        const std::string& bn = gbuf.name();
                        if (producedHere.count(bn)) continue;
                        auto hostIt = device_.hostBuffers_.find(bn);
                        if (hostIt == device_.hostBuffers_.end() || hostIt->second.empty()) continue;
                        auto h2dIt = h2dByBuffer.find(bn);
                        if (h2dIt == h2dByBuffer.end()) {
                            hipGraphNode_t h2d = addMemcpyH2DNode(bn, hostIt->second.size(),
                                                                  /*deps=*/{});
                            h2dByBuffer[bn] = h2d;
                            deps.push_back(h2d);
                        } else {
                            deps.push_back(h2dIt->second);
                        }
                    }

                    hipGraphNode_t kn = addKernelNode(n, deps);
                    hipNodeById_[n.id] = kn;

                } else if constexpr (std::is_same_v<T, CompiledBridgeOpNode>) {
                    hipGraphNode_t hn = addOpHostNode(n.tryReady, n.action, deps);
                    hipNodeById_[n.id] = hn;
                } else {
                    throw std::runtime_error(
                        "GpuDevice: compiled control/boundary nodes are not executable yet");
                }
            },
            node);
    }

    // After all kernels, append D2H copies for every kernel output that is
    // not consumed by another local kernel. These leaf outputs are either
    // graph-level outputs (read back via getOutputBuffer) or producer-side
    // staging for a cross-device bridge (which uses host-side buffers).
    for (const CompiledNode& node : dg.nodes) {
        if (!std::holds_alternative<CompiledKernelNode>(node)) continue;
        const CompiledKernelNode& k = std::get<CompiledKernelNode>(node);
        auto knIt = hipNodeById_.find(k.id);
        if (knIt == hipNodeById_.end()) continue;
        for (const auto& [_, gbuf] : k.ioMap.outputs()) {
            const std::string& bn = gbuf.name();
            if (consumedHere.count(bn)) continue;
            auto sit = device_.deviceBufferSizes_.find(bn);
            if (sit == device_.deviceBufferSizes_.end()) continue;
            addMemcpyD2HNode(bn, sit->second, /*deps=*/{ knIt->second });
        }
    }

    // Instantiate the executable graph
    hipCheck(hipGraphInstantiate(&hipExec_, hipGraph_, nullptr, nullptr, 0),
             "hipGraphInstantiate");
}

// ---------------------------------------------------------------------------
// launch / wait
// ---------------------------------------------------------------------------

void GpuDevicePlan::launch() {
    hipCheck(hipSetDevice(device_.hipDeviceIdx_), "hipSetDevice in launch");
    refreshScalarParams();
    hipCheck(hipGraphLaunch(hipExec_, device_.stream_), "hipGraphLaunch");
}

void GpuDevicePlan::wait() {
    hipCheck(hipSetDevice(device_.hipDeviceIdx_), "hipSetDevice in wait");
    hipCheck(hipStreamSynchronize(device_.stream_), "hipStreamSynchronize");
}

}  // namespace vrt::graph
