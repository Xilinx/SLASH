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
 * @file compiled_graph.hpp
 * @brief CompiledGraph — executable graph snapshot produced from an authored Graph.
 *
 * Graph owns authoring state. CompiledGraph owns the lowered top-level DGraphs,
 * the scalar state visible to those DGraphs, bridge instances needed by compiled
 * bridge operations, and the device plans compiled from the DGraphs.
 */

#ifndef VRT_GRAPH_COMPILED_GRAPH_HPP
#define VRT_GRAPH_COMPILED_GRAPH_HPP

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/device/fpga_device.hpp>
#if defined(VRT_HAS_GPU) && (VRT_HAS_GPU == 1)
#include <vrt/graph/device/gpu_device.hpp>
#endif

namespace vrt::graph {

/**
 * @brief Executable compiled graph snapshot.
 *
 * This class is move-only because device plans are move-only. It owns the
 * top-level per-device plans; nested child plans remain owned by the device
 * plan implementations that compile those child DGraphs.
 */
class CompiledGraph {
   public:
    CompiledGraph(std::vector<DGraph> dgraphs,
                  std::shared_ptr<std::map<std::string, uint64_t>> scalarValues,
                  std::map<std::string, ScalarType> scalarTypes,
                  std::vector<std::shared_ptr<IBridge>> bridgePins)
        : dgraphs_(std::move(dgraphs)),
          scalarValues_(std::move(scalarValues)),
          scalarTypes_(std::move(scalarTypes)),
          bridgePins_(std::move(bridgePins)) {
        if (!scalarValues_) {
            scalarValues_ = std::make_shared<std::map<std::string, uint64_t>>();
        }
        for (const auto& [name, type] : scalarTypes_) {
            scopedScalarTypes_[scopedScalarKey(rootScopeId_, name)] = type;
        }
        collectSizeScalarKeys();
        plans_.reserve(dgraphs_.size());
        for (const auto& dg : dgraphs_) {
            plans_.push_back(dg.device->compilePlan(dg));
        }
    }

    CompiledGraph(const CompiledGraph&) = delete;
    CompiledGraph& operator=(const CompiledGraph&) = delete;
    CompiledGraph(CompiledGraph&&) noexcept = default;
    CompiledGraph& operator=(CompiledGraph&&) noexcept = default;
    ~CompiledGraph() = default;

    /**
     * @brief Returns the lowered per-device DGraphs.
     */
    const std::vector<DGraph>& dgraphs() const { return dgraphs_; }

    /**
     * @brief Set a root-scope scalar in this compiled snapshot from raw bits.
     */
    void setScalarBits(const std::string& name, uint64_t bits) {
        requireScalar(name, "CompiledGraph::setScalarBits");
        (*scalarValues_)[scopedScalarKey(rootScopeId_, name)] = bits;
    }

    void setScalarBits(const GraphScalar& scalar, uint64_t bits) {
        requireScalar(scalar, "CompiledGraph::setScalarBits");
        (*scalarValues_)[scopedScalarKey(scalar.scopeId(), scalar.varName())] = bits;
    }

    /**
     * @brief Read a root-scope scalar in this compiled snapshot as raw bits.
     */
    uint64_t scalarBits(const std::string& name) const {
        requireScalar(name, "CompiledGraph::scalarBits");
        const std::string key = scopedScalarKey(rootScopeId_, name);
        auto valueIt = scalarValues_->find(key);
        if (valueIt == scalarValues_->end()) {
            throw std::out_of_range(
                "CompiledGraph::scalarBits: scalar '" + name + "' has no value");
        }
        return valueIt->second;
    }

    uint64_t scalarBits(const GraphScalar& scalar) const {
        requireScalar(scalar, "CompiledGraph::scalarBits");
        const std::string key = scopedScalarKey(scalar.scopeId(), scalar.varName());
        auto valueIt = scalarValues_->find(key);
        if (valueIt == scalarValues_->end()) {
            throw std::out_of_range(
                "CompiledGraph::scalarBits: scalar '" + scalar.varName() + "' has no value");
        }
        return valueIt->second;
    }

    template <class T>
    void setScalar(const std::string& name, T value) {
        static_assert(std::is_arithmetic_v<T>,
                      "CompiledGraph::setScalar only supports arithmetic types");
        requireScalarType<T>(name, "CompiledGraph::setScalar");
        setScalarBits(name, detail::valueToBits(value));
    }

    template <class T>
    void setScalar(const GraphScalar& scalar, T value) {
        static_assert(std::is_arithmetic_v<T>,
                      "CompiledGraph::setScalar only supports arithmetic types");
        requireScalarType<T>(scalar, "CompiledGraph::setScalar");
        setScalarBits(scalar, detail::valueToBits(value));
    }

    template <class T>
    T getScalar() const = delete;

    template <class T>
    T getScalar(const std::string& name) const {
        static_assert(std::is_arithmetic_v<T>,
                      "CompiledGraph::getScalar only supports arithmetic types");
        requireScalarType<T>(name, "CompiledGraph::getScalar");
        uint64_t bits = scalarBits(name);
        T value{};
        std::memcpy(&value, &bits, sizeof(T));
        return value;
    }

    template <class T>
    T getScalar(const GraphScalar& scalar) const {
        static_assert(std::is_arithmetic_v<T>,
                      "CompiledGraph::getScalar only supports arithmetic types");
        requireScalarType<T>(scalar, "CompiledGraph::getScalar");
        uint64_t bits = scalarBits(scalar);
        T value{};
        std::memcpy(&value, &bits, sizeof(T));
        return value;
    }

    void write(const GraphBuffer& token, const void* data, std::size_t bytes) {
        validateBufferByteCount(token, bytes, "CompiledGraph::write");
        if (bytes > 0 && data == nullptr) {
            throw std::invalid_argument("CompiledGraph::write: data must not be null");
        }
        const std::string key = scopedBufferKey(token.scopeId(), token.name());
        bool wrote = false;
        for (const auto& dg : dgraphs_) {
            if (deviceUsesInput(dg, token)) {
                writeToDevice(*dg.device, key, data, bytes);
                wrote = true;
            }
        }
        if (!wrote) {
            throw std::runtime_error(
                "CompiledGraph::write: token '" + token.name() +
                "' is not consumed by this compiled graph");
        }
    }

    template <class T>
    void write(const GraphBuffer& token, const std::vector<T>& data) {
        write(token, data.data(), data.size() * sizeof(T));
    }

    void read(const GraphBuffer& token, void* data, std::size_t bytes) const {
        validateBufferByteCount(token, bytes, "CompiledGraph::read");
        if (bytes > 0 && data == nullptr) {
            throw std::invalid_argument("CompiledGraph::read: data must not be null");
        }
        const std::string key = scopedBufferKey(token.scopeId(), token.name());
        for (const auto& dg : dgraphs_) {
            if (readFromDevice(*dg.device, key, data, bytes)) {
                return;
            }
        }
        throw std::runtime_error(
            "CompiledGraph::read: token '" + token.name() +
            "' has no readable storage; did the graph run and produce it?");
    }

    template <class T>
    void read(const GraphBuffer& token, std::vector<T>& out) const {
        read(token, out.data(), out.size() * sizeof(T));
    }

    /**
     * @brief Start asynchronous execution on every top-level device plan.
     */
    void launch() {
        requireAllSizesSet();
        for (auto& plan : plans_) {
            plan->prepareLaunch();
        }
        for (auto& plan : plans_) {
            plan->launch();
        }
    }

    /**
     * @brief Wait for every top-level device plan to complete.
     */
    void wait() {
        for (auto& plan : plans_) {
            plan->wait();
        }
    }

    /**
     * @brief Execute synchronously. Equivalent to launch() followed by wait().
     */
    void run() {
        launch();
        wait();
    }

   private:
    void requireAllSizesSet() const {
        std::vector<std::string> missing;
        for (const auto& [key, name] : sizeScalarKeys_) {
            if (scalarValues_->find(key) == scalarValues_->end()) {
                missing.push_back(name);
            }
        }
        if (missing.empty()) return;
        std::ostringstream msg;
        msg << "CompiledGraph::launch: unset size scalar";
        if (missing.size() != 1) msg << "s";
        msg << ": ";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i != 0) msg << ", ";
            msg << missing[i];
        }
        throw std::runtime_error(msg.str());
    }

    void validateBufferByteCount(const GraphBuffer& token,
                                 std::size_t bytes,
                                 const char* method) const {
        const std::size_t expected =
            resolvedBufferSizeBytes(token, scalarValues_, method);
        if (bytes != expected) {
            throw std::invalid_argument(
                std::string(method) + ": buffer '" + token.name() +
                "' expects " + std::to_string(expected) +
                " byte(s), got " + std::to_string(bytes));
        }
    }

    void requireScalar(const std::string& name, const char* method) const {
        if (scalarTypes_.find(name) == scalarTypes_.end()) {
            throw std::out_of_range(
                std::string(method) + ": unknown scalar '" + name + "'");
        }
    }

    void requireScalar(const GraphScalar& scalar, const char* method) const {
        auto scopeIt = scopedScalarTypes_.find(scopedScalarKey(scalar.scopeId(), scalar.varName()));
        if (scopeIt == scopedScalarTypes_.end()) {
            throw std::out_of_range(
                std::string(method) + ": unknown scalar '" + scalar.varName() + "'");
        }
    }

    template <class T>
    void requireScalarType(const std::string& name, const char* method) const {
        auto declaredType = scalarTypes_.find(name);
        if (declaredType == scalarTypes_.end()) {
            throw std::out_of_range(
                std::string(method) + ": unknown scalar '" + name + "'");
        }
        if (declaredType->second != typeToScalarType<T>()) {
            throw std::invalid_argument(
                std::string(method) + ": type mismatch for scalar '" + name + "'");
        }
    }

    template <class T>
    void requireScalarType(const GraphScalar& scalar, const char* method) const {
        auto declaredType = scopedScalarTypes_.find(scopedScalarKey(scalar.scopeId(), scalar.varName()));
        if (declaredType == scopedScalarTypes_.end()) {
            throw std::out_of_range(
                std::string(method) + ": unknown scalar '" + scalar.varName() + "'");
        }
        if (declaredType->second != typeToScalarType<T>()) {
            throw std::invalid_argument(
                std::string(method) + ": type mismatch for scalar '" + scalar.varName() + "'");
        }
    }

    static bool nodeConsumesBuffer(const CompiledNode& node, const std::string& key) {
        if (const auto* kernel = std::get_if<CompiledKernelNode>(&node)) {
            for (const auto& [port, buffer] : kernel->ioMap.inputs()) {
                (void)port;
                if (scopedBufferKey(buffer.scopeId(), buffer.name()) == key) return true;
            }
            for (const auto& inout : kernel->ioMap.inouts()) {
                if (scopedBufferKey(inout.in.scopeId(), inout.in.name()) == key) return true;
            }
        }
        if (const auto* boundary = std::get_if<CompiledBoundaryNode>(&node)) {
            if (boundary->side == CompiledBoundaryNode::Side::Start) {
                for (const auto& copy : boundary->bufferCopies) {
                    if (scopedBufferKey(copy.sourceScopeId, copy.sourceName) == key) return true;
                }
            }
        }
        return false;
    }

    static bool childConsumesBuffer(const DGraph& dg, const std::string& key) {
        for (const auto& child : dg.childDGraphs) {
            for (const auto& childDg : child.dgraphs) {
                if (childDg && deviceUsesInput(*childDg, key)) return true;
            }
        }
        return false;
    }

    static bool deviceUsesInput(const DGraph& dg, const GraphBuffer& token) {
        return deviceUsesInput(dg, scopedBufferKey(token.scopeId(), token.name()));
    }

    static bool deviceUsesInput(const DGraph& dg, const std::string& key) {
        for (const auto& node : dg.nodes) {
            if (nodeConsumesBuffer(node, key)) return true;
        }
        return childConsumesBuffer(dg, key);
    }

    static void writeToDevice(IDevice& device, const std::string& key,
                              const void* data, std::size_t bytes) {
        if (auto* cpu = dynamic_cast<CpuDevice*>(&device)) {
            cpu->setInputBuffer(key, data, bytes);
            return;
        }
        if (auto* fpga = dynamic_cast<FpgaDevice*>(&device)) {
            fpga->setInputBuffer(key, data, bytes);
            return;
        }
#if defined(VRT_HAS_GPU) && (VRT_HAS_GPU == 1)
        if (auto* gpu = dynamic_cast<GpuDevice*>(&device)) {
            gpu->setInputBuffer(key, data, bytes);
            return;
        }
#endif
        throw std::runtime_error("CompiledGraph::write: unsupported device '" + device.id() + "'");
    }

    static bool readFromDevice(IDevice& device, const std::string& key,
                               void* data, std::size_t bytes) {
        if (auto* cpu = dynamic_cast<CpuDevice*>(&device)) {
            if (cpu->bufferSize(key) == 0) return false;
            cpu->getOutputBuffer(key, data, bytes);
            return true;
        }
        if (auto* fpga = dynamic_cast<FpgaDevice*>(&device)) {
            if (fpga->bufferSize(key) == 0) return false;
            fpga->getOutputBuffer(key, data, bytes);
            return true;
        }
#if defined(VRT_HAS_GPU) && (VRT_HAS_GPU == 1)
        if (auto* gpu = dynamic_cast<GpuDevice*>(&device)) {
            if (gpu->bufferSize(key) == 0) return false;
            gpu->getOutputBuffer(key, data, bytes);
            return true;
        }
#endif
        return false;
    }

    static constexpr uint64_t rootScopeId_ = 0;

    static void noteSizeScalar(const GraphBuffer& buffer,
                               std::map<std::string, std::string>& out) {
        if (!buffer.valid() || !buffer.hasSizeScalar()) return;
        const GraphScalar& scalar = buffer.sizeScalar();
        out[scopedScalarKey(scalar.scopeId(), scalar.varName())] = scalar.varName();
    }

    static void collectSizeScalarKeys(const DGraph& dg,
                                      std::map<std::string, std::string>& out) {
        for (const CompiledNode& node : dg.nodes) {
            if (const auto* kernel = std::get_if<CompiledKernelNode>(&node)) {
                for (const auto& [port, buffer] : kernel->ioMap.inputs()) {
                    (void)port;
                    noteSizeScalar(buffer, out);
                }
                for (const auto& [port, buffer] : kernel->ioMap.outputs()) {
                    (void)port;
                    noteSizeScalar(buffer, out);
                }
                for (const auto& rw : kernel->ioMap.inouts()) {
                    noteSizeScalar(rw.in, out);
                    noteSizeScalar(rw.out, out);
                }
            }
        }
        for (const DGraphChild& child : dg.childDGraphs) {
            for (const auto& childDg : child.dgraphs) {
                if (childDg) collectSizeScalarKeys(*childDg, out);
            }
        }
    }

    void collectSizeScalarKeys() {
        for (const DGraph& dg : dgraphs_) {
            collectSizeScalarKeys(dg, sizeScalarKeys_);
        }
    }

    std::vector<DGraph>                       dgraphs_;
    std::shared_ptr<std::map<std::string, uint64_t>> scalarValues_;
    std::map<std::string, ScalarType>         scalarTypes_;
    std::map<std::string, ScalarType>         scopedScalarTypes_;
    std::map<std::string, std::string>        sizeScalarKeys_;
    std::vector<std::shared_ptr<IBridge>>     bridgePins_;
    std::vector<std::unique_ptr<IDevicePlan>> plans_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_COMPILED_GRAPH_HPP
