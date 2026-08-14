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
 * @file graph.hpp
 * @brief Hardware-style heterogeneous graph authoring API.
 */

#ifndef VRT_GRAPH_GRAPH_HPP
#define VRT_GRAPH_GRAPH_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/authoring/calls.hpp>
#include <vrt/graph/authoring/fpga.hpp>
#include <vrt/graph/authoring/region_builder.hpp>
#include <vrt/graph/compiler.hpp>
#include <vrt/graph/control/graph_region.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/fpga_device.hpp>
#include <vrt/graph/execution.hpp>
#include <vrt/graph/node/io_map.hpp>

namespace vrt::graph {

namespace detail {
class GraphTestAccess;
}

class CpuKernels {
   public:
    explicit CpuKernels(std::shared_ptr<CpuDevice> device)
        : device_(std::move(device)) {}

    template <class K, class... Args>
    KernelHandle add(Args&&... args) {
        auto implementation =
            std::make_shared<K>(std::forward<Args>(args)...);
        KernelHandle descriptor{
            implementation->name(), DeviceType::CPU, std::nullopt,
            implementation->ioTypeMap(), device_->id()};
        device_->registerKernel(std::move(implementation));
        return descriptor;
    }

    template <class T, class Fn>
    KernelHandle elementwise(std::string name, Fn fn) {
        auto implementation =
            std::make_shared<ElementwiseCpuKernel<T>>(
                name, std::function<T(T)>(std::move(fn)));
        KernelHandle descriptor{
            name, DeviceType::CPU, std::nullopt,
            implementation->ioTypeMap(), device_->id()};
        device_->registerKernel(std::move(implementation));
        return descriptor;
    }

   private:
    std::shared_ptr<CpuDevice> device_;
};

class Graph {
   public:
    Graph() = default;
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&) = default;
    Graph& operator=(Graph&&) = default;

    static Graph withDefaults();

    CpuKernels cpu() { return CpuKernels(cpuDevice()); }
    FpgaHandle addFpga(const FpgaSpec& spec);

    template <class T>
    GraphScalar scalarInput(std::string name) {
        return root_->inputScalar(
            typeToScalarType<T>(), std::move(name));
    }

    template <class T>
    GraphBuffer input(std::string name, GraphScalar size) {
        return root_->inputBuffer(
            typeToBufferType<T>(), std::move(name), std::move(size));
    }

    template <class T>
    GraphBuffer buffer(std::string name, GraphScalar size) {
        return root_->buffer(
            typeToBufferType<T>(), std::move(name), std::move(size));
    }

    template <class T>
    GraphScalar scalar(std::string name) {
        return root_->scalar(typeToScalarType<T>(), std::move(name));
    }

    template <class T>
    GraphBuffer output(std::string name, GraphScalar size) {
        return root_->outputBuffer(
            typeToBufferType<T>(), std::move(name), std::move(size));
    }

    template <class T>
    GraphScalar outputScalar(std::string name) {
        return root_->outputScalar(
            typeToScalarType<T>(), std::move(name));
    }

    GraphNode addKernelCall(const KernelCallSpec& spec) {
        return rootBuilder_.addKernelCall(spec);
    }

    GraphNode addReprogram(const ReprogramCallSpec& spec) {
        return rootBuilder_.addReprogram(spec);
    }

    RegionBuilder addLoop(const LoopBuildSpec& spec) {
        return rootBuilder_.addLoop(spec);
    }

    std::pair<RegionBuilder, RegionBuilder> addConditional(
        const ConditionalBuildSpec& spec) {
        return rootBuilder_.addConditional(spec);
    }

    GraphRegion& rootRegion() { return *root_; }
    const GraphRegion& rootRegion() const { return *root_; }

    GraphScalar globalScalar(ScalarType type, std::string name) {
        return root_->scalar(type, std::move(name));
    }

    GraphBuffer inputBuffer(
        BufferType type, std::string name,
        std::optional<GraphScalar> size = std::nullopt) {
        return root_->inputBuffer(
            type, std::move(name), std::move(size));
    }

    std::string addNode(
        KernelDescriptor kernel, IOMap ioMap, std::string device,
        std::vector<std::string> after = {}) {
        for (const auto& [port, output] : ioMap.outputs()) {
            (void)port;
            root_->markOutputBuffer(output.name(), output);
        }
        for (const auto& inout : ioMap.inouts()) {
            root_->markOutputBuffer(
                inout.out.name(), inout.out);
        }
        return root_->addKernel(
            std::move(kernel), std::move(ioMap), std::move(device),
            std::move(after));
    }

    std::string addReprogram(ReprogramSpec spec) {
        return root_->addReprogram(std::move(spec));
    }

    std::string addLoop(LoopSpec spec) {
        return root_->addLoop(std::move(spec));
    }

    std::string addConditional(ConditionalSpec spec) {
        return root_->addConditional(std::move(spec));
    }

    [[nodiscard]] Execution compile() {
        try {
            auto scalarValues =
                std::make_shared<std::map<std::string, std::uint64_t>>();
            std::map<std::pair<std::string, std::string>,
                     std::shared_ptr<IBridge>> bridgePins;
            auto bridgeLookup =
                [this, &bridgePins](const std::string& source,
                                    const std::string& destination) {
                    IBridge* bridge = bridgeFor(source, destination);
                    auto found = bridgeInstances_.find(
                        {source, destination});
                    if (found != bridgeInstances_.end()) {
                        bridgePins[{source, destination}] = found->second;
                    }
                    return bridge;
                };
            GraphCompiler compiler;
            CompileResult<ExecutionPlan> compiled = compiler.compile(
                AuthoredGraph::snapshot(*root_), devices_,
                bridgeFactories_, bridgeLookup,
                scalarValues);
            if (!compiled.ok()) {
                throw GraphCompileError(
                    std::move(compiled.diagnostics));
            }
            ExecutionPlan plan = std::move(*compiled.output);
            std::vector<std::shared_ptr<IBridge>> pinnedBridges;
            pinnedBridges.reserve(bridgePins.size());
            for (auto& [endpoints, bridge] : bridgePins) {
                (void)endpoints;
                pinnedBridges.push_back(std::move(bridge));
            }
            return Execution(
                plan.takeRuntimeState(), plan.takeIo(),
                plan.takeResources(), std::move(pinnedBridges),
                plan.takeDevicePins(), plan.takeExecutables(),
                plan.takeRoots());
        } catch (const GraphCompileError&) {
            throw;
        } catch (const std::runtime_error& error) {
            Diagnostics diagnostics;
            diagnostics.error(DiagCode::CompilerError, error.what());
            throw GraphCompileError(
                std::move(diagnostics), error.what());
        }
    }

   public:
    friend class detail::GraphTestAccess;

    void registerDevice(std::shared_ptr<IDevice> device) {
        const std::string id = device->id();
        if (devices_.count(id)) {
            throw std::invalid_argument(
                "Graph: duplicate device registration");
        }
        if (device->type() == DeviceType::CPU && cpuDevice()) {
            throw std::invalid_argument(
                "Graph: only one CPU device may be registered");
        }
        devices_[id] = std::move(device);
    }

    void registerBridgeFactory(
        DeviceType source, DeviceType destination,
        BridgeFactory factory) {
        auto key = std::make_pair(source, destination);
        if (bridgeFactories_.count(key)) {
            throw std::invalid_argument(
                "Graph: duplicate bridge factory");
        }
        bridgeFactories_[key] = std::move(factory);
    }

    IBridge* bridgeFor(
        const std::string& source,
        const std::string& destination) {
        auto key = std::make_pair(source, destination);
        auto cached = bridgeInstances_.find(key);
        if (cached != bridgeInstances_.end()) {
            return cached->second.get();
        }
        auto sourceDevice = devices_.find(source);
        auto destinationDevice = devices_.find(destination);
        if (sourceDevice == devices_.end() ||
            destinationDevice == devices_.end()) {
            throw std::runtime_error(
                "Graph: transfer references an unknown device");
        }
        auto factory = bridgeFactories_.find(
            {sourceDevice->second->type(),
             destinationDevice->second->type()});
        if (factory == bridgeFactories_.end()) return nullptr;
        std::shared_ptr<IBridge> bridge = factory->second(
            *sourceDevice->second, *destinationDevice->second);
        if (!bridge) {
            throw std::runtime_error(
                "Graph: bridge factory returned null");
        }
        IBridge* result = bridge.get();
        bridgeInstances_[std::move(key)] = std::move(bridge);
        return result;
    }

    std::shared_ptr<CpuDevice> cpuDevice() const {
        for (const auto& [id, device] : devices_) {
            (void)id;
            auto cpu = std::dynamic_pointer_cast<CpuDevice>(device);
            if (cpu) return cpu;
        }
        return nullptr;
    }

   private:
    std::shared_ptr<detail::AuthoringRegion> root_ = detail::AuthoringRegion::createRoot();
    RegionBuilder rootBuilder_{root_};
    std::map<std::string, std::shared_ptr<IDevice>> devices_;
    std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>
        bridgeFactories_;
    std::map<std::pair<std::string, std::string>,
             std::shared_ptr<IBridge>> bridgeInstances_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_GRAPH_HPP
