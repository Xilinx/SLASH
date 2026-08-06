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
#include <type_traits>
#include <variant>

#include <vrt/graph/backend_runtime.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>

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

using GpuAuthoredIndex =
    std::map<NodeId, const AuthoredOperation*>;

void indexGpuAuthored(
    const AuthoredRegion& region, GpuAuthoredIndex& result) {
    for (const AuthoredOperation& operation : region.operations) {
        result[authoredNodeId(operation)] = &operation;
        if (const auto* loop = std::get_if<AuthoredLoop>(&operation)) {
            if (loop->body) indexGpuAuthored(*loop->body, result);
        } else if (const auto* conditional =
                       std::get_if<AuthoredConditional>(&operation)) {
            if (conditional->thenRegion) {
                indexGpuAuthored(*conditional->thenRegion, result);
            }
            if (conditional->elseRegion) {
                indexGpuAuthored(*conditional->elseRegion, result);
            }
        }
    }
}

GpuProgram lowerGpuProgram(const BackendLoweringContext& context) {
    GpuProgram program;
    program.queue = context.queue.id;
    program.device = context.queue.device;
    program.runtimeState = context.runtimeState;
    GpuAuthoredIndex authored;
    indexGpuAuthored(
        context.scheduled.routed().placed().resolved()
            .authored().root(),
        authored);
    for (ScheduleStepId stepId : context.queue.steps) {
        const ScheduledStep& step =
            context.scheduled.steps().at(stepId);
        if (const auto* scheduled =
                std::get_if<ScheduledOperation>(&step.payload)) {
            const AuthoredOperation& operation =
                *authored.at(scheduled->operation);
            const auto* kernel = std::get_if<AuthoredKernel>(&operation);
            if (!kernel) {
                throw std::runtime_error(
                    "GpuDevice: scheduled control/reprogram operation "
                    "is not executable yet");
            }
            GpuKernelCommand command;
            command.step = stepId;
            command.kernel = kernel->kernel;
            command.ioMap = kernel->ioMap;
            command.dependencies = step.dependencies;
            program.commands.emplace_back(std::move(command));
            continue;
        }
        const std::vector<HostActionId>& actions =
            context.hostActions.find(stepId);
        if (!actions.empty()) {
            GpuHostCommand command;
            command.step = stepId;
            command.dependencies = step.dependencies;
            command.actions.reserve(actions.size());
            for (HostActionId id : actions) {
                command.actions.push_back(
                    context.hostActions.at(id));
            }
            program.commands.emplace_back(std::move(command));
        }
    }
    return program;
}

}  // namespace

class GpuDevicePlan : public IBackendExecutable {
   public:
    struct HostCallbackData {
        std::function<bool()> tryReady;
        std::function<void()> action;
    };

    GpuDevicePlan(
        std::shared_ptr<GpuDevice> device,
        GpuProgram program);
    ~GpuDevicePlan() override;

    void launch() override;
    void wait() override;
    QueueId queue() const override { return queue_; }
    DeviceId device() const override {
        return DeviceId(device_->id());
    }

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

    hipGraphNode_t addKernelNode(const GpuKernelCommand& node,
                                 const std::vector<hipGraphNode_t>& deps);
    hipGraphNode_t addOpHostNode(std::function<bool()> tryReady,
                                 std::function<void()> action,
                                 const std::vector<hipGraphNode_t>& deps);
    hipGraphNode_t addMemcpyH2DNode(const std::string& bufferName, size_t sizeBytes,
                                    const std::vector<hipGraphNode_t>& deps);
    hipGraphNode_t addMemcpyD2HNode(const std::string& bufferName, size_t sizeBytes,
                                    const std::vector<hipGraphNode_t>& deps);

    std::shared_ptr<GpuDevice> device_;
    hipGraph_t hipGraph_ = nullptr;
    hipGraphExec_t hipExec_ = nullptr;
    std::map<ScheduleStepId, hipGraphNode_t> hipNodeByStep_;
    std::vector<std::unique_ptr<KernelParamStore>> paramStores_;
    std::vector<std::unique_ptr<HostCallbackData>> callbackDatas_;
    std::shared_ptr<BackendRuntimeState> runtimeState_;
    std::shared_ptr<std::map<std::string, uint64_t>> scalarValues_;
    QueueId queue_;

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

std::unique_ptr<IDeviceExecutionLease>
GpuDevice::leaseExecution() {
    return tryAcquireExclusiveExecutionLease(
        executionLeased_);
}

std::unique_ptr<IBackendExecutable> GpuDevice::lowerQueue(
    const BackendLoweringContext& context) {
    try {
        return std::make_unique<GpuDevicePlan>(
            shared_from_this(), lowerGpuProgram(context));
    } catch (const std::bad_weak_ptr&) {
        throw std::runtime_error(
            "GpuDevice: executable plans require the device to be "
            "owned by std::shared_ptr");
    }
}

GpuDevicePlan::~GpuDevicePlan() {
    if (hipExec_ && device_->stream_) {
        hipSetDevice(device_->hipDeviceIdx_);
        hipStreamSynchronize(device_->stream_);
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
    hipNodeByStep_.clear();
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
    void* dptr = device_->ensureDeviceBuffer(bufferName, sizeBytes);

    // Source is the private host store
    auto hostIt = device_->hostBuffers_.find(bufferName);
    if (hostIt == device_->hostBuffers_.end()) {
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
    auto devIt = device_->deviceBuffers_.find(bufferName);
    if (devIt == device_->deviceBuffers_.end()) {
        throw std::runtime_error(
            "GpuDevice: D2H memcpy source device buffer '" + bufferName + "' not found");
    }

    // Ensure host-side destination exists
    auto& hostBuf = device_->hostBuffers_[bufferName];
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

hipGraphNode_t GpuDevicePlan::addKernelNode(const GpuKernelCommand& node,
                                            const std::vector<hipGraphNode_t>& deps) {
    const std::string& kname = node.kernel.name;
    auto it = device_->kernels_.find(kname);
    if (it == device_->kernels_.end()) {
        throw std::runtime_error("GpuDevice: no kernel registered for '" + kname + "'");
    }
    const GpuKernelBinding& binding = it->second;

    // Build per-kernel parameter storage
    auto store = std::make_unique<KernelParamStore>();

    // Determine the element count from the first input buffer (for grid sizing)
    size_t firstInputElements = 1;
    if (!node.ioMap.inputs().empty()) {
        const std::string& firstName = node.ioMap.inputs().begin()->second.name();
        auto sit = device_->deviceBufferSizes_.find(firstName);
        if (sit != device_->deviceBufferSizes_.end()) {
            // Default to sizeof(float) elements if unknown
            firstInputElements = sit->second / sizeof(float);
        }
    }

    // Resolve each param in paramOrder to either a device pointer or a scalar
    // Build maps: portName → device pointer, portName → scalar value
    std::map<std::string, void*>    bufferPtrs;
    std::map<std::string, uint64_t> scalarVals;

    for (const auto& [portName, gbuf] : node.ioMap.inputs()) {
        auto dit = device_->deviceBuffers_.find(gbuf.name());
        if (dit != device_->deviceBuffers_.end()) {
            bufferPtrs[portName] = dit->second;
        }
    }
    for (const auto& [portName, gbuf] : node.ioMap.outputs()) {
        auto dit = device_->deviceBuffers_.find(gbuf.name());
        if (dit != device_->deviceBuffers_.end()) {
            bufferPtrs[portName] = dit->second;
        }
    }
    for (const auto& rwb : node.ioMap.inouts()) {
        auto inIt = device_->deviceBuffers_.find(rwb.in.name());
        if (inIt != device_->deviceBuffers_.end()) {
            bufferPtrs[rwb.inPort] = inIt->second;
        }
        auto outIt = device_->deviceBuffers_.find(rwb.out.name());
        if (outIt != device_->deviceBuffers_.end()) {
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
            "' not found in detail::PortBindings bindings");
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

GpuDevicePlan::GpuDevicePlan(
    std::shared_ptr<GpuDevice> device, GpuProgram program)
    : device_(std::move(device)),
      runtimeState_(std::move(program.runtimeState)),
      scalarValues_(
          runtimeState_ ? runtimeState_->scalarValues()
                        : nullptr),
      queue_(program.queue) {
    if (!device_ || !runtimeState_) {
        throw std::invalid_argument(
            "GpuDevicePlan: device and runtime state must not be null");
    }
    hipCheck(hipSetDevice(device_->hipDeviceIdx_),
             "hipSetDevice in GPU program lowering");

    for (const GpuCommand& command : program.commands) {
        const auto* kernel = std::get_if<GpuKernelCommand>(&command);
        if (!kernel) continue;
        std::size_t defaultSize = 0;
        for (const auto& [port, buffer] : kernel->ioMap.inputs()) {
            (void)port;
            auto host = device_->hostBuffers_.find(buffer.name());
            if (host == device_->hostBuffers_.end()) continue;
            device_->ensureDeviceBuffer(buffer.name(), host->second.size());
            if (defaultSize == 0) defaultSize = host->second.size();
        }
        for (const auto& [port, buffer] : kernel->ioMap.outputs()) {
            (void)port;
            device_->ensureDeviceBuffer(
                buffer.name(), defaultSize > 0 ? defaultSize : 1024);
        }
        for (const auto& binding : kernel->ioMap.inouts()) {
            auto host = device_->hostBuffers_.find(binding.in.name());
            const std::size_t bytes =
                host != device_->hostBuffers_.end()
                    ? host->second.size() : defaultSize;
            device_->ensureDeviceBuffer(binding.in.name(), bytes);
            device_->ensureDeviceBuffer(binding.out.name(), bytes);
        }
    }

    std::set<std::string> producedHere;
    std::set<std::string> consumedHere;
    for (const GpuCommand& command : program.commands) {
        const auto* kernel = std::get_if<GpuKernelCommand>(&command);
        if (!kernel) continue;
        for (const auto& [port, buffer] : kernel->ioMap.outputs()) {
            (void)port;
            producedHere.insert(buffer.name());
        }
        for (const auto& binding : kernel->ioMap.inouts()) {
            producedHere.insert(binding.out.name());
            consumedHere.insert(binding.in.name());
        }
        for (const auto& [port, buffer] : kernel->ioMap.inputs()) {
            (void)port;
            consumedHere.insert(buffer.name());
        }
    }

    hipCheck(hipGraphCreate(&hipGraph_, 0), "hipGraphCreate");
    auto translateDependencies =
        [&](const std::vector<ScheduleStepId>& dependencies) {
            std::vector<hipGraphNode_t> result;
            result.reserve(dependencies.size());
            for (ScheduleStepId dependency : dependencies) {
                auto node = hipNodeByStep_.find(dependency);
                if (node != hipNodeByStep_.end()) {
                    result.push_back(node->second);
                }
            }
            return result;
        };
    std::map<std::string, hipGraphNode_t> h2dByBuffer;

    for (const GpuCommand& command : program.commands) {
        std::visit(
            [&](const auto& concrete) {
                using T = std::decay_t<decltype(concrete)>;
                std::vector<hipGraphNode_t> dependencies =
                    translateDependencies(concrete.dependencies);
                if constexpr (std::is_same_v<T, GpuKernelCommand>) {
                    for (const auto& [port, buffer] :
                         concrete.ioMap.inputs()) {
                        (void)port;
                        const std::string& name = buffer.name();
                        if (producedHere.count(name)) continue;
                        auto host = device_->hostBuffers_.find(name);
                        if (host == device_->hostBuffers_.end() ||
                            host->second.empty()) {
                            continue;
                        }
                        auto staged = h2dByBuffer.find(name);
                        if (staged == h2dByBuffer.end()) {
                            hipGraphNode_t h2d = addMemcpyH2DNode(
                                name, host->second.size(), {});
                            h2dByBuffer[name] = h2d;
                            dependencies.push_back(h2d);
                        } else {
                            dependencies.push_back(staged->second);
                        }
                    }
                    hipNodeByStep_[concrete.step] =
                        addKernelNode(concrete, dependencies);
                } else {
                    auto ready = [actions = concrete.actions] {
                        return std::all_of(
                            actions.begin(), actions.end(),
                            [](const HostAction& action) {
                                return !action.tryReady ||
                                       action.tryReady();
                            });
                    };
                    auto run = [actions = concrete.actions] {
                        for (const HostAction& action : actions) {
                            if (action.action) action.action();
                        }
                    };
                    hipNodeByStep_[concrete.step] = addOpHostNode(
                        std::move(ready), std::move(run), dependencies);
                }
            },
            command);
    }

    for (const GpuCommand& command : program.commands) {
        const auto* kernel = std::get_if<GpuKernelCommand>(&command);
        if (!kernel) continue;
        auto graphNode = hipNodeByStep_.find(kernel->step);
        if (graphNode == hipNodeByStep_.end()) continue;
        for (const auto& [port, buffer] : kernel->ioMap.outputs()) {
            (void)port;
            if (consumedHere.count(buffer.name())) continue;
            auto size = device_->deviceBufferSizes_.find(buffer.name());
            if (size == device_->deviceBufferSizes_.end()) continue;
            addMemcpyD2HNode(
                buffer.name(), size->second, {graphNode->second});
        }
    }
    hipCheck(hipGraphInstantiate(&hipExec_, hipGraph_, nullptr, nullptr, 0),
             "hipGraphInstantiate");
}

// ---------------------------------------------------------------------------
// launch / wait
// ---------------------------------------------------------------------------

void GpuDevicePlan::launch() {
    hipCheck(hipSetDevice(device_->hipDeviceIdx_), "hipSetDevice in launch");
    refreshScalarParams();
    hipCheck(hipGraphLaunch(hipExec_, device_->stream_), "hipGraphLaunch");
}

void GpuDevicePlan::wait() {
    hipCheck(hipSetDevice(device_->hipDeviceIdx_), "hipSetDevice in wait");
    hipCheck(hipStreamSynchronize(device_->stream_), "hipStreamSynchronize");
}

}  // namespace vrt::graph
