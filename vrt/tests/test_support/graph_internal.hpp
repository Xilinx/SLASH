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

#ifndef VRT_TEST_SUPPORT_GRAPH_INTERNAL_HPP
#define VRT_TEST_SUPPORT_GRAPH_INTERNAL_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/graph.hpp>

namespace vrt::graph::detail {

class GraphTestAccess {
   public:
    static Graph make() { return Graph(); }

    static void registerDevice(
        Graph& graph, std::shared_ptr<IDevice> device) {
        graph.registerDevice(std::move(device));
    }

    static void registerBridgeFactory(
        Graph& graph, DeviceType source, DeviceType destination,
        BridgeFactory factory) {
        graph.registerBridgeFactory(
            source, destination, std::move(factory));
    }

    static AuthoringRegion& root(Graph& graph) {
        return *graph.root_;
    }

    static const AuthoringRegion& root(const Graph& graph) {
        return *graph.root_;
    }

    static AuthoredGraph snapshot(const Graph& graph) {
        return AuthoredGraph::snapshot(*graph.root_);
    }

    static const std::map<std::string, std::shared_ptr<IDevice>>&
    devices(const Graph& graph) {
        return graph.devices_;
    }

    static const std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>&
    bridgeFactories(const Graph& graph) {
        return graph.bridgeFactories_;
    }

    static std::shared_ptr<CpuDevice> cpuDevice(Graph& graph) {
        return graph.cpuDevice();
    }

    template <class T>
    static GraphScalar scalarInput(
        Graph& graph, std::string name) {
        return graph.root_->inputScalar(
            typeToScalarType<T>(), std::move(name));
    }

    static GraphScalar scalar(
        Graph& graph, ScalarType type, std::string name) {
        return graph.root_->scalar(type, std::move(name));
    }

    static GraphBuffer inputBuffer(
        Graph& graph, BufferType type, std::string name,
        GraphScalar size) {
        return graph.root_->inputBuffer(
            type, std::move(name), std::move(size));
    }

    static GraphBuffer inputBuffer(
        Graph& graph, BufferType type, std::string name) {
        return graph.root_->inputBuffer(type, std::move(name));
    }

    template <class T>
    static GraphBuffer buffer(
        Graph& graph, std::string name, GraphScalar size) {
        return graph.root_->buffer(
            typeToBufferType<T>(), std::move(name), std::move(size));
    }

    template <class T>
    static GraphBuffer outputBuffer(
        Graph& graph, std::string name, GraphScalar size) {
        return graph.root_->outputBuffer(
            typeToBufferType<T>(), std::move(name), std::move(size));
    }

    static std::string addNode(
        Graph& graph, KernelDescriptor kernel,
        PortBindings bindings, std::string device,
        std::vector<std::string> after = {}) {
        return graph.root_->addKernel(
            std::move(kernel), std::move(bindings),
            std::move(device), std::move(after));
    }

    static std::string addReprogram(
        Graph& graph, ::vrt::graph::detail::ReprogramRecord record) {
        return graph.root_->addReprogram(std::move(record));
    }

    static std::string addLoop(
        Graph& graph, ::vrt::graph::detail::LoopRecord record) {
        return graph.root_->addLoop(std::move(record));
    }

    static std::string addConditional(
        Graph& graph, ::vrt::graph::detail::ConditionalRecord record) {
        return graph.root_->addConditional(std::move(record));
    }
};

class ExecutionTestAccess {
   public:
    static Execution makeExecution(
        std::vector<std::unique_ptr<IBackendExecutable>> executables,
        std::vector<IBackendExecutable*> roots) {
        auto runtimeState =
            std::make_shared<BackendRuntimeState>(
                std::make_shared<
                    std::map<std::string, std::uint64_t>>());
        return Execution(
            std::move(runtimeState), {},
            BackendResourceBindings({}, {}, {}, {}, {}, {}),
            {}, {}, std::move(executables), std::move(roots));
    }

    template <class T>
    static void writeScalar(
        Execution& execution, const GraphScalar& scalar, T value) {
        std::lock_guard<std::mutex> lock(
            execution.runtimeState_->scalarMutex());
        (*execution.scalarValues_)[scopedScalarKey(
            scalar.scopeId(), scalar.varName())] =
            detail::valueToBits(value);
    }

    static void writeBuffer(
        Execution& execution, const GraphBuffer& buffer,
        const void* data, std::size_t bytes) {
        execution.requireCpuDevice("ExecutionTestAccess::writeBuffer");
        execution.cpuDevice_->setInputBuffer(
            scopedBufferKey(buffer.scopeId(), buffer.name()),
            data, bytes);
    }

    static void readBuffer(
        const Execution& execution, const GraphBuffer& buffer,
        void* data, std::size_t bytes) {
        execution.requireCpuDevice("ExecutionTestAccess::readBuffer");
        execution.cpuDevice_->getOutputBuffer(
            scopedBufferKey(buffer.scopeId(), buffer.name()),
            data, bytes);
    }
};

}  // namespace vrt::graph::detail

#endif  // VRT_TEST_SUPPORT_GRAPH_INTERNAL_HPP
