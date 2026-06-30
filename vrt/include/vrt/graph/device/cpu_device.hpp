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
 * @file cpu_device.hpp
 * @brief CpuDevice — naive single-core CPU implementation of IDevice.
 *
 * Storage ownership
 * -----------------
 * `CpuDevice` is split between two concerns:
 *   - The device itself owns *device-resident* state: the registered
 *     `CpuKernel` table and the per-buffer-name byte storage. Both are
 *     *shared across every plan compiled by this device*. Per-scope buffer
 *     keys (`scope:N:name`) keep different graph regions from colliding;
 *     two plans for the same Graph naturally observe the same logical
 *     buffer when they reference the same scoped name, which mirrors
 *     accelerator hardware (a buffer "lives on" the device).
 *   - Each `CpuDevicePlan` owns its own *graph-level scalar map*, taken
 *     from `DGraph::scalarValues` at compile time. Scalar state is owned
 *     by the `Graph` and threaded into every device plan, so two plans
 *     for the same Graph see one shared scalar map and two plans for
 *     different Graphs see independent maps.
 *
 * Execution model
 * ---------------
 * Nodes run sequentially on a worker thread owned by the compiled CPU plan.
 * launch() returns immediately; wait() joins the worker. The plan also
 * runs kernel dispatch, boundary copies, and control-flow execution; the
 * device is consulted only as a kernel registry and a buffer store.
 *
 * Buffer management
 * -----------------
 * Each GraphBuffer that appears as an output or RW-output in the graph is
 * backed by a heap-allocated byte array owned privately by this device.
 * Graph-level input buffers must be pre-populated by the user before
 * launch() via setInputBuffer().
 *
 * Kernel dispatch
 * ---------------
 * CPU kernels are CpuKernel objects registered before launch() via
 * registerKernel(). Each kernel owns its name, typed IO signature, and call
 * implementation.
 *
 * Cross-device synchronisation and data movement
 * ----------------------------------------------
 * CpuDevice has no built-in notion of either: the compiler synthesises
 * `CompiledBridgeOpNode` entries (each carrying an opaque `std::function<void()>`
 * closure produced by an `IBridge`) directly into the device's per-device
 * `DGraph::nodes`. CpuDevicePlan walks the node list with `std::visit` and
 * runs the closures inline. A typical CPU↔X bridge gives the CPU side a
 * closure that reads/writes the device's buffer storage via the public
 * setInputBuffer/getOutputBuffer/bufferSize accessors, capturing whatever
 * bridge-private staging and synchronisation primitives it owns.
 */

#ifndef VRT_GRAPH_DEVICE_CPU_DEVICE_HPP
#define VRT_GRAPH_DEVICE_CPU_DEVICE_HPP

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <vrt/graph/device/device.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/node/io_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>
#include <vrt/graph/node/compiled_node.hpp>
#include <vrt/graph/core/types.hpp>

namespace vrt::graph {

// ---------------------------------------------------------------------------
// CpuBufferView — typed view into a buffer passed to kernel functions
// ---------------------------------------------------------------------------

/**
 * @brief Non-owning view of a buffer argument as seen by a CPU kernel function.
 */
struct CpuBufferView {
    void*      data       = nullptr;
    size_t     sizeBytes  = 0;
    BufferType elementType;

    template <typename T>
    T* as() const { return static_cast<T*>(data); }

    size_t elementCount() const;  // sizeBytes / sizeof(element); defined in .cpp
};

// ---------------------------------------------------------------------------
// KernelSpan — minimal typed, bounded view passed to CpuKernel::run()
// ---------------------------------------------------------------------------

/**
 * @brief Non-owning, typed, bounded view over a kernel buffer argument.
 *
 * Provides size() / operator[] / begin() / end() / data() so kernels can write
 * idiomatic element loops without sizeBytes / sizeof arithmetic. C++17 has no
 * std::span, so this is the lightweight stand-in.
 */
template <typename T>
class KernelSpan {
   public:
    KernelSpan() = default;
    KernelSpan(T* data, std::size_t count) : data_(data), count_(count) {}

    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    T* data() const { return data_; }
    T& operator[](std::size_t i) const { return data_[i]; }
    T* begin() const { return data_; }
    T* end() const { return data_ + count_; }

   private:
    T* data_ = nullptr;
    std::size_t count_ = 0;
};

// ---------------------------------------------------------------------------
// CpuKernelArgs — context passed to every registered kernel function
// ---------------------------------------------------------------------------

/**
 * @brief Provides access to all bound arguments for a single kernel invocation.
 *
 * Buffer and scalar look-ups are by port name (matching the IOTypeMap
 * declaration).  Throws std::out_of_range on an unknown port name.
 */
class CpuKernelArgs {
   public:
    CpuKernelArgs(std::map<std::string, CpuBufferView> buffers,
                                    std::map<std::string, uint64_t>       scalars,
                                    std::map<std::string, uint64_t*>      writableScalars = {})
                : buffers_(std::move(buffers)),
                    scalars_(std::move(scalars)),
                    writableScalars_(std::move(writableScalars)) {}

    /**
     * @brief Get a view of a buffer argument by port name.
     */
    const CpuBufferView& buffer(const std::string& portName) const {
        auto it = buffers_.find(portName);
        if (it == buffers_.end()) {
            throw std::out_of_range("CpuKernelArgs: unknown buffer port '" + portName + "'");
        }
        return it->second;
    }

    /**
     * @brief Get a scalar argument value by port name.
     */
    uint64_t scalar(const std::string& portName) const {
        auto it = scalars_.find(portName);
        if (it != scalars_.end()) {
            return it->second;
        }

        auto writableIt = writableScalars_.find(portName);
        if (writableIt != writableScalars_.end()) {
            return *writableIt->second;
        }

        throw std::out_of_range("CpuKernelArgs: unknown scalar port '" + portName + "'");
    }

    /**
     * @brief Write an output scalar argument by port name (raw bits).
     */
    void setScalar(const std::string& portName, uint64_t value) const {
        auto it = writableScalars_.find(portName);
        if (it == writableScalars_.end()) {
            throw std::out_of_range(
                "CpuKernelArgs: unknown writable scalar port '" + portName + "'");
        }
        *it->second = value;
    }

    // --- Typed accessors (RFC run(Args&) surface) ------------------------

    /** @brief Typed read-only view of an input buffer port. */
    template <typename T>
    KernelSpan<const T> in(const std::string& portName) const {
        const CpuBufferView& v = buffer(portName);
        return KernelSpan<const T>(static_cast<const T*>(v.data), v.sizeBytes / sizeof(T));
    }

    /** @brief Typed writable view of an output buffer port. */
    template <typename T>
    KernelSpan<T> out(const std::string& portName) const {
        const CpuBufferView& v = buffer(portName);
        return KernelSpan<T>(static_cast<T*>(v.data), v.sizeBytes / sizeof(T));
    }

    /** @brief Typed writable view of an in-place (inout) buffer port. */
    template <typename T>
    KernelSpan<T> inout(const std::string& portName) const {
        const CpuBufferView& v = buffer(portName);
        return KernelSpan<T>(static_cast<T*>(v.data), v.sizeBytes / sizeof(T));
    }

    /** @brief Typed value of an input scalar port. */
    template <typename T>
    T scalarIn(const std::string& portName) const {
        uint64_t bits = scalar(portName);
        T value{};
        std::memcpy(&value, &bits, sizeof(T));
        return value;
    }

    /** @brief Write an output scalar port from a typed value. */
    template <typename T>
    void setScalarValue(const std::string& portName, T value) const {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(T));
        setScalar(portName, bits);
    }

   private:
    std::map<std::string, CpuBufferView> buffers_;
    std::map<std::string, uint64_t>      scalars_;
    std::map<std::string, uint64_t*>     writableScalars_;
};

// ---------------------------------------------------------------------------
// CpuKernel — polymorphic CPU kernel implementation
// ---------------------------------------------------------------------------

class CpuKernel {
   public:
    /// Argument context handed to run(); typed accessors live on CpuKernelArgs.
    using Args = CpuKernelArgs;

    explicit CpuKernel(std::string name) : name_(std::move(name)) {}
    virtual ~CpuKernel() = default;

    /** @brief Logical kernel name used to match KernelDescriptor::name. */
    const std::string& name() const { return name_; }

    /** @brief Typed I/O signature for this CPU kernel (declared once). */
    virtual IOTypeMap ioTypeMap() const = 0;

    /** @brief Execute one graph node invocation with typed argument access. */
    virtual void run(Args& args) = 0;

    /** @brief Convenience descriptor for the kernel's typed signature. */
    KernelDescriptor descriptor() const {
        return KernelDescriptor{name_, DeviceType::CPU, std::nullopt, ioTypeMap()};
    }

   private:
    std::string name_;
};

// ---------------------------------------------------------------------------
// ElementwiseCpuKernel — lambda-backed shorthand for the map-each-element case
// ---------------------------------------------------------------------------

/**
 * @brief One-input one-output elementwise CPU kernel built from a lambda.
 *
 * Backs Graph::cpu().elementwise<T>(name, fn): declares an `in`/`out` buffer
 * pair of element type T and applies `fn` to each element.
 */
template <typename T>
class ElementwiseCpuKernel : public CpuKernel {
   public:
    ElementwiseCpuKernel(std::string name, std::function<T(T)> fn)
        : CpuKernel(std::move(name)), fn_(std::move(fn)) {}

    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.template in<T>("in").template out<T>("out");
    }

    void run(Args& args) override {
        auto in = args.template in<T>("in");
        auto out = args.template out<T>("out");
        for (std::size_t i = 0; i < in.size(); ++i) out[i] = fn_(in[i]);
    }

   private:
    std::function<T(T)> fn_;
};

// ---------------------------------------------------------------------------
// CpuDevice
// ---------------------------------------------------------------------------

class CpuDevicePlan;

class CpuDevice : public IDevice {
   public:
    /**
     * @brief Construct a CpuDevice.
     *
     * @param id  Logical device id, e.g. "cpu" or "cpu:0".
     */
    explicit CpuDevice(std::string id);

    // --- Kernel registration (call before launch) ---

    /**
     * @brief Register a CPU kernel implementation.
     */
    void registerKernel(std::shared_ptr<CpuKernel> kernel);

    // --- Buffer accessors (also used by bridges) ---

    /**
     * @brief Supply data for a graph-level input buffer (no producer node).
     *
     * Also used by bridges as the consumer-side write path on the CPU.
     */
    void setInputBuffer(const std::string& bufferName, const void* data, size_t sizeBytes);

    /**
     * @brief Read back an output buffer.
     *
     * Also used by bridges as the producer-side read path on the CPU.
     */
    void getOutputBuffer(const std::string& bufferName, void* data, size_t sizeBytes) const;

    /**
     * @brief Returns the current size of @p bufferName, or 0 if not present.
     */
    size_t bufferSize(const std::string& bufferName) const;

    // --- Cross-queue rendezvous signal access (Phase E) ---

    /// Reads the 32-bit value of a host-visible signal slot.
    using SignalReadFn = std::function<std::uint32_t(std::uint32_t /*slot*/)>;
    /// Writes the 32-bit value of a host-visible signal slot.
    using SignalWriteFn = std::function<void(std::uint32_t /*slot*/, std::uint32_t /*value*/)>;

    /**
     * @brief Wire this CPU device to a peer queue's host-visible signal array.
     *
     * A split cross-device loop runs its CPU body slice concurrently with the
     * peer (FPGA) queue, rendezvousing per iteration through signal slots that
     * live in the peer's BAR-visible DDR window. The Graph supplies these
     * accessors before launch so the CPU's CompiledSignalNode / CompiledWaitNode
     * execution can SET and poll those slots over the BAR. Without them, a CPU
     * slice that contains rendezvous nodes throws at launch.
     */
    void setSignalAccessors(SignalReadFn reader, SignalWriteFn writer) {
        signalRead_  = std::move(reader);
        signalWrite_ = std::move(writer);
    }

    // --- IDevice ---

    DeviceType  type() const override { return DeviceType::CPU; }
    std::string id()   const override { return id_; }

    std::unique_ptr<IDevicePlan> compilePlan(const DGraph& dg) override;

   private:
    friend class CpuDevicePlan;

    std::string                                  id_;
    std::map<std::string, std::shared_ptr<CpuKernel>> kernels_;
    std::map<std::string, std::vector<uint8_t>>  buffers_;
    SignalReadFn                                 signalRead_;
    SignalWriteFn                                signalWrite_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DEVICE_CPU_DEVICE_HPP
