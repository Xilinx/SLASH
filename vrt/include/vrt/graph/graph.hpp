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
 * @brief Graph — user-facing heterogeneous work graph builder and executor.
 *
 * Usage overview:
 *
 *  1. Register one IDevice per physical device.
 *  2. Declare graph-level input buffers with inputBuffer() and graph-global
 *     mutable scalars with globalScalar().
 *  3. Add kernel nodes with addNode(), capturing output buffer tokens from IOMap.
 *  4. Call compile() to validate the structure and lower the graph into
 *     a CompiledGraph snapshot.
 *  5. Call run() (blocking) or launch() + wait() (async) on the CompiledGraph.
 *
 * Compilation is explicit and value-producing: each call to compile() returns
 * an independent executable snapshot. Authoring changes affect only future
 * snapshots; already-compiled snapshots keep executing the structure they were
 * built from.
 *
 * Nested authoring (kernels, scalars, and buffers inside a loop body or a
 * conditional branch) does NOT go through the `Graph` itself. Use the
 * `GraphRegion` returned by `rootRegion().createChild()` (or by an inner
 * region's `createChild()`) and call `addKernel()`, `inputBuffer()`,
 * `scalar()`, `addLoop()`, `addConditional()`, etc. on it directly. The
 * region pointer is then handed to `Graph::addLoop` / `Graph::addConditional`
 * (or `GraphRegion::addLoop` / `GraphRegion::addConditional` for deeper
 * nesting) so the compiler can wire it up as a child of the surrounding
 * control-flow op.
 *
 * # CPU device invariant
 *
 * A Graph hosts at most one CPU-typed `IDevice` (see `DeviceType::CPU`).
 * Production code obtains it from `Graph::withDefaults()`, which registers a
 * canonical `CpuDevice` under the id `"cpu"`; users should not register their
 * own `CpuDevice` on top of that. Heterogeneous host-side compute that wants
 * its own placement domain (e.g. NUMA-affine workers, an accelerator-side
 * helper CPU) should declare a separate `DeviceType` rather than a second
 * CPU device.
 *
 * The compiler relies on this invariant to:
 *   - own control-flow execution as the *fallback*: a loop / conditional runs
 *     on the CPU only when it is not eligible for autonomous FPGA execution or
 *     a cross-device split. An all-FPGA loop (fixed-count or data-dependent
 *     while) or conditional is lowered to run autonomously on the FPGA queue
 *     (RP1 LOOP/RERUN/COND), and a control op whose body spans FPGA + CPU is
 *     split into per-queue replicas that rendezvous over signal slots (an FPGA
 *     Follower driven by a CPU Authority). See assignDevices /
 *     fpgaAutonomousLoopDevice / fpgaAutonomousConditionalDevice /
 *     splitLoopParticipants in the compiler;
 *   - route cross-device transfers through a CPU bounce buffer when no
 *     direct `(srcType, dstType)` bridge factory is registered.
 *
 * @example
 * @code
 *   Graph g = Graph::withDefaults();
 *   g.registerDevice(std::make_shared<FpgaDevice>("fpga:0", device));
 *
 *   GraphBuffer raw = g.inputBuffer(BufferType::F32, "raw");
 *
 *   IOMap rootIo;
 *   GraphBuffer rootOut;
 *   rootIo.bindInput("in", raw)
 *         .bindOutput("out", BufferType::F32, rootOut);
 *   g.addNode(rootKernel, std::move(rootIo), "fpga:0");
 *
 *   // Nested authoring lives on the child region. The body cannot reference
 *   // a parent-scope token directly; route it through an explicit start
 *   // boundary so the compiler can prove the import.
 *   auto body = g.rootRegion().createChild();
 *   GraphBuffer bodyImported = body->inputBuffer(BufferType::F32, "imported");
 *   body->importFromParent(
 *       std::vector<BufferBoundaryMapping>{{rootOut, bodyImported}});
 *
 *   IOMap bodyIo;
 *   GraphBuffer bodyOut;
 *   bodyIo.bindInput("in", bodyImported)
 *         .bindOutput("out", BufferType::F32, bodyOut, body->scopeId());
 *   body->addKernel(bodyKernel, std::move(bodyIo), "fpga:0");
 *
 *   GraphScalar iterations = g.scalarInput<int32_t>("iterations");
 *   LoopSpec loop;
 *   loop.tripCount = LoopTripCount::scalar(iterations);
 *   loop.body = body;
 *   g.addLoop(std::move(loop));
 *
 *   CompiledGraph exec = g.compile();
 *   exec.writeScalar(iterations, 4);
 *   exec.run();
 * @endcode
 */

#ifndef VRT_GRAPH_GRAPH_HPP
#define VRT_GRAPH_GRAPH_HPP

#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <vrt/graph/authoring/calls.hpp>
#include <vrt/graph/authoring/fpga.hpp>
#include <vrt/graph/authoring/region_builder.hpp>
#include <vrt/graph/compiled_graph.hpp>
#include <vrt/graph/compiler.hpp>
#include <vrt/graph/control/graph_region.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/diagnostics.hpp>
#include <vrt/graph/device/fpga_device.hpp>
#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/ir/authored_graph.hpp>
#include <vrt/graph/ir/placed_graph.hpp>
#include <vrt/graph/ir/resolved_graph.hpp>
#include <vrt/graph/ir/routed_graph.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>
#include <vrt/graph/node/io_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>
#include <vrt/graph/semantic_plan.hpp>

namespace vrt::graph {

class CpuDevice;

/**
 * @brief Facade returned by Graph::cpu() for declaring CPU kernels.
 *
 * `add<K>(args...)` registers a CpuKernel subclass instance and returns a
 * handle; `elementwise<T>(name, fn)` is the map-each-element shorthand.
 */
class CpuKernels {
   public:
    explicit CpuKernels(std::shared_ptr<CpuDevice> device) : device_(std::move(device)) {}

    template <class K, class... Args>
    KernelHandle add(Args&&... args) {
        auto kernel = std::make_shared<K>(std::forward<Args>(args)...);
        KernelHandle handle{kernel->name(), DeviceType::CPU, std::nullopt,
                            kernel->ioTypeMap(), device_->id()};
        device_->registerKernel(std::move(kernel));
        return handle;
    }

    template <class T, class Fn>
    KernelHandle elementwise(std::string name, Fn fn) {
        auto kernel = std::make_shared<ElementwiseCpuKernel<T>>(
            name, std::function<T(T)>(std::move(fn)));
        KernelHandle handle{name, DeviceType::CPU, std::nullopt, kernel->ioTypeMap(),
                            device_->id()};
        device_->registerKernel(std::move(kernel));
        return handle;
    }

   private:
    std::shared_ptr<CpuDevice> device_;
};

class Graph {
   public:
    Graph() = default;

    // Non-copyable; move is fine.
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&) = default;
    Graph& operator=(Graph&&) = default;

    /**
     * @brief Build a graph preloaded with the canonical host CPU device and
     *        all production bridge factories available in the current build.
     *
     * The returned graph contains a CpuDevice registered under the canonical
     * id `"cpu"`. CPU↔FPGA bridges are always registered. CPU↔GPU bridges are
     * registered only when the library is built with GPU support.
     */
    static Graph withDefaults();

    // --- Setup ---

    /**
     * @brief Register a device.
     *
     * Must be called before compile.  Multiple devices of different types
     * and ids may be registered, but a Graph may host **at most one** CPU-typed
     * device. The CPU device is effectively a singleton owned by `Graph`:
     * production code should obtain it via `Graph::withDefaults()` (which
     * registers a canonical `CpuDevice` under the id `"cpu"`) and not call
     * `registerDevice` with another `CpuDevice` afterwards. Host-side compute
     * that wants its own placement domain (e.g. NUMA-affine workers) should
     * declare a separate `DeviceType` instead of a second CPU device.
     *
     * @param device  Shared-ownership device instance.
     * @throws std::invalid_argument  If a device with the same id is already
     *                                registered, or if a second CPU-typed
     *                                device is registered.
     */
    void registerDevice(std::shared_ptr<IDevice> device) {
        const std::string did = device->id();
        if (devices_.count(did)) {
            throw std::invalid_argument("Graph::registerDevice: duplicate device id '" + did + "'");
        }
        if (device->type() == DeviceType::CPU) {
            for (const auto& [existingId, existing] : devices_) {
                if (existing->type() == DeviceType::CPU) {
                    throw std::invalid_argument(
                        "Graph::registerDevice: a CPU device is already registered ('" +
                        existingId + "'); only one CPU-typed device is allowed");
                }
            }
        }
        devices_[did] = std::move(device);
    }

    /**
     * @brief Register a cross-device bridge **factory** for an ordered
     *        device-type pair.
     *
     * The compiler lazily invokes the factory once per concrete
     * `(srcDeviceId, dstDeviceId)` pair it encounters, constructing one
     * bridge instance bound to that specific pair. CompiledGraph snapshots pin
     * the bridge instances they use so compiled bridge closures outlive Graph
     * edits and destruction.
     *
     * Each non-CPU device type SHOULD register at least a `(CPU, T)` and
     * `(T, CPU)` factory; missing factories surface as runtime errors at
     * `compile()` time when a transfer needs them.
     *
     * @throws std::invalid_argument  If a factory for the same ordered
     *         pair is already registered, or both types are CPU.
     */
    void registerBridgeFactory(DeviceType    srcType,
                                DeviceType    dstType,
                                BridgeFactory factory) {
        if (srcType == DeviceType::CPU && dstType == DeviceType::CPU) {
            throw std::invalid_argument(
                "Graph::registerBridgeFactory: {CPU, CPU} factory is not allowed");
        }
        auto key = std::make_pair(srcType, dstType);
        if (bridgeFactories_.count(key)) {
            throw std::invalid_argument(
                "Graph::registerBridgeFactory: duplicate factory for device-type pair");
        }
        bridgeFactories_[key] = std::move(factory);
    }

    /**
     * @brief Look up (or lazily create) the bridge instance handling
     *        transfers from device id @p srcDevId to @p dstDevId.
     *
     * Returns @c nullptr when no factory is registered for the underlying
     * device-type pair (callers that need a fall-back path inspect the
     * pointer rather than catching an exception). The instance is cached;
     * subsequent calls with the same pair return the same `IBridge*`.
     *
     * @throws std::runtime_error If either device id is unknown, or the
     *         registered factory itself returns null.
     */
    IBridge* bridgeFor(const std::string& srcDevId,
                       const std::string& dstDevId) {
        auto cacheKey = std::make_pair(srcDevId, dstDevId);
        auto it = bridgeInstances_.find(cacheKey);
        if (it != bridgeInstances_.end()) return it->second.get();

        auto sIt = devices_.find(srcDevId);
        auto dIt = devices_.find(dstDevId);
        if (sIt == devices_.end()) {
            throw std::runtime_error(
                "Graph::bridgeFor: unknown source device id '" + srcDevId + "'");
        }
        if (dIt == devices_.end()) {
            throw std::runtime_error(
                "Graph::bridgeFor: unknown destination device id '" + dstDevId + "'");
        }

        auto typeKey = std::make_pair(sIt->second->type(), dIt->second->type());
        auto fIt = bridgeFactories_.find(typeKey);
        if (fIt == bridgeFactories_.end()) {
            return nullptr;  // No factory for this device-type pair.
        }

        auto inst = fIt->second(*sIt->second, *dIt->second);
        if (!inst) {
            throw std::runtime_error(
                "Graph::bridgeFor: factory returned null for '" + srcDevId +
                "' -> '" + dstDevId + "'");
        }
        auto* ptr = inst.get();
        bridgeInstances_[cacheKey] = std::move(inst);
        return ptr;
    }

    /**
     * @brief Returns whether a factory is registered for an ordered
     *        device-type pair.
     */
    bool hasBridgeFactory(DeviceType srcType, DeviceType dstType) const {
        return bridgeFactories_.count({srcType, dstType}) != 0;
    }

    /**
     * @brief Returns the registered bridge factories.
     */
    const std::map<std::pair<DeviceType, DeviceType>, BridgeFactory>&
    bridgeFactories() const {
        return bridgeFactories_;
    }

    /**
     * @brief Returns the lazily-instantiated per-pair bridge cache.
     */
    const std::map<std::pair<std::string, std::string>,
                   std::shared_ptr<IBridge>>& bridgeInstances() const {
        return bridgeInstances_;
    }

    /**
     * @brief Returns a non-owning view of the root-region kernel operations
     *        in insertion order.
     *
     * Intended for inspection and visualisation; structured control-flow ops
     * are available through rootRegion(). The returned vector references
     * `KernelOp`s held by `rootRegion()`'s op list, which is stable for the
     * region's lifetime; the view is invalidated by any subsequent op append
     * on the root region (`Graph::addNode`, `Graph::addLoop`,
     * `Graph::addConditional`, …). Callers should grab the view once and
     * stop using it before authoring more root-level ops.
     */
    std::vector<std::reference_wrapper<const KernelOp>> rootKernels() const {
        std::vector<std::reference_wrapper<const KernelOp>> out;
        for (const RegionOp& op : rootRegion_->ops()) {
            if (const auto* kernel = std::get_if<KernelOp>(&op)) {
                out.emplace_back(std::cref(*kernel));
            }
        }
        return out;
    }

    /**
     * @brief Returns the registered devices keyed by id().
     */
    const std::map<std::string, std::shared_ptr<IDevice>>& devices() const {
        return devices_;
    }

    /**
     * @brief Returns the root authored region of the structured graph.
     */
    GraphRegion& rootRegion() { return *rootRegion_; }
    const GraphRegion& rootRegion() const { return *rootRegion_; }

    /**
     * @brief Returns the registered CPU-typed device as a CpuDevice.
     *
     * registerDevice() enforces that a Graph hosts at most one CPU-typed
     * device, so this returns the singleton when present. Returns nullptr if
     * no CPU device is registered or the registered CPU is not a CpuDevice
     * instance.
     */
    std::shared_ptr<CpuDevice> cpuDevice() const;

    /**
     * @brief Declare a graph-global mutable scalar variable.
     *
     * The returned GraphScalar token may be bound as an input scalar or,
     * when the kernel IOTypeMap declares it as an output scalar, as a result
     * location written by the kernel.
     *
     * Delegates fully to the root region: the type is recorded on
     * `rootRegion()` (via `GraphRegion::scalar()`) and the runtime value is
     * `globalScalar()` and a direct `rootRegion().scalar()` call are therefore
     * interchangeable. Runtime values are supplied to the returned
     * CompiledGraph, not to Graph.
     */
    GraphScalar globalScalar(ScalarType type, std::string name) {
        return rootRegion_->scalar(type, std::move(name));
    }

    /**
     * @brief Declare a graph-level input buffer (no producer node).
     *
     * @param type  Element type of the buffer.
     * @param name  Logical name; must be unique among all graph buffers.
     * @return      A GraphBuffer token that may be passed to IOMap::bindInput().
     * @throws std::invalid_argument  If the name is already taken.
     *
     * Delegates name registration to the root region so that ::inputBuffer()
     * calls directly on rootRegion() and ::inputBuffer() calls on Graph share
     * the same name set.
     */
    GraphBuffer inputBuffer(BufferType type, std::string name) {
        GraphBuffer token = rootRegion_->inputBuffer(type, std::move(name));
        return token;
    }

    GraphBuffer inputBuffer(BufferType type, std::string name, GraphScalar size) {
        GraphBuffer token = rootRegion_->inputBuffer(type, std::move(name),
                                                     std::move(size));
        return token;
    }

    /**
     * @brief Add a kernel node to the graph.
     *
     * @param kernel      Kernel type and I/O signature.
     * @param ioMap       Concrete data bindings for this instantiation.
     * @param device      Required target device id (e.g. "fpga:0").
     * @param afterNodes  Additional ordering constraints (node ids).
     * @return            The new node's id; stable for the lifetime of the Graph.
     *
     * @throws std::invalid_argument  If a node id collision occurs (should not
     *                                happen with auto-assigned ids).
     */
    std::string addNode(KernelDescriptor         kernel,
                        IOMap                    ioMap,
                        std::string              device,
                        std::vector<std::string> afterNodes = {}) {
        const std::string nodeId = rootRegion_->addKernel(
            std::move(kernel), std::move(ioMap), std::move(device), std::move(afterNodes));
        return nodeId;
    }

    std::string addReprogram(ReprogramSpec spec) {
        const std::string id = rootRegion_->addReprogram(std::move(spec));
        return id;
    }

    std::string addLoop(LoopSpec spec) {
        const std::string id = rootRegion_->addLoop(std::move(spec));
        return id;
    }

    std::string addConditional(ConditionalSpec spec) {
        const std::string id = rootRegion_->addConditional(std::move(spec));
        return id;
    }

    // --- Struct-literal authoring API ------------------------------------

    /**
     * @brief Access the CPU kernel registry facade (declare CPU kernels).
     */
    CpuKernels cpu() { return CpuKernels(cpuDevice()); }

    /**
     * @brief Bring up an FPGA device in one call and register it.
     *
     * Folds QDMA PDI staging, vbin/image loading, the vrtd session + BAR
     * window, the RP1 readiness preflight, and FpgaDevice construction. The
     * returned handle owns those resources for the graph's lifetime. Defined
     * in graph.cpp (pulls in vrtd/vrt device plumbing).
     */
    FpgaHandle addFpga(const FpgaSpec& spec);

    /**
     * @brief Declare a graph-level typed input buffer token.
     */
    template <class T>
    GraphBuffer input(std::string name, GraphScalar size) {
        GraphBuffer token = rootRegion_->inputBuffer(typeToBufferType<T>(), std::move(name),
                                                      std::move(size));
        return token;
    }

    /**
     * @brief Mint a typed, single-assignment buffer token at root scope.
     */
    template <class T>
    GraphBuffer buffer(std::string name, GraphScalar size) {
        return rootRegion_->buffer(typeToBufferType<T>(), std::move(name),
                                   std::move(size));
    }

    /**
     * @brief Declare a graph-level typed output buffer token.
     */
    template <class T>
    GraphBuffer output(std::string name, GraphScalar size) {
        return rootRegion_->outputBuffer(typeToBufferType<T>(), std::move(name),
                                         std::move(size));
    }

    /**
     * @brief Declare a graph-level scalar input token.
     */
    template <class T>
    GraphScalar scalarInput(std::string name) {
        return rootRegion_->inputScalar(typeToScalarType<T>(), std::move(name));
    }

    /**
     * @brief Declare a graph-level scalar output token.
     */
    template <class T>
    GraphScalar outputScalar(std::string name) {
        return rootRegion_->outputScalar(typeToScalarType<T>(), std::move(name));
    }

    /**
     * @brief Declare a named scalar token at root scope.
     */
    template <class T>
    GraphScalar scalar(std::string name) {
        return globalScalar(typeToScalarType<T>(), std::move(name));
    }

    /** @brief Author a kernel dispatch at root scope. */
    GraphNode addKernelCall(const KernelCallSpec& spec) {
        GraphNode node = rootBuilder_.addKernelCall(spec);
        return node;
    }

    /** @brief Author an explicit reprogram (PDI_LOAD) node at root scope. */
    GraphNode addReprogram(const ReprogramCallSpec& spec) {
        GraphNode node = rootBuilder_.addReprogram(spec);
        return node;
    }

    /** @brief Author a loop region at root scope. */
    RegionBuilder addLoop(const LoopBuildSpec& spec) {
        RegionBuilder loop = rootBuilder_.addLoop(spec);
        return loop;
    }

    /** @brief Author a conditional at root scope; returns [then, else]. */
    std::pair<RegionBuilder, RegionBuilder> addConditional(const ConditionalBuildSpec& spec) {
        auto branches = rootBuilder_.addConditional(spec);
        return branches;
    }

    // --- Compilation ---

    /**
     * @brief Compile the authored graph into an executable CompiledGraph snapshot.
     *
     * Compilation is the single, explicit validation step. It validates the
     * graph structure (root-scope scalar/buffer references, port bindings,
     * scopes, cycles, bridge factory coverage), resolves placement, routes
     * transfers, schedules queues, and returns an executable snapshot that
     * owns backend device plans.
     *
     * Calling compile() again builds a new independent snapshot. Existing
     * CompiledGraph instances continue to execute the structure and scalar
     * state they were built with.
     *
     * @throws GraphCompileError On any structural violation. The exception
     *                           remains a std::runtime_error and carries
     *                           structured diagnostics.
     */
    [[nodiscard]] CompiledGraph compile() {
        try {
            auto snapshotScalars =
                std::make_shared<std::map<std::string, uint64_t>>();
            std::map<std::pair<std::string, std::string>,
                     std::shared_ptr<IBridge>> bridgePins;
            auto lookup = [this, &bridgePins](const std::string& s,
                                              const std::string& d) -> IBridge* {
                IBridge* bridge = this->bridgeFor(s, d);
                if (bridge != nullptr) {
                    auto key = std::make_pair(s, d);
                    auto it = bridgeInstances_.find(key);
                    if (it != bridgeInstances_.end()) {
                        bridgePins[key] = it->second;
                    }
                }
                return bridge;
            };
            GraphCompiler compiler;
            CompileResult<ExecutionPlan> compiled =
                compiler.compile(rootRegion(), devices_, bridgeFactories_,
                                 lookup, snapshotScalars);
            if (!compiled.ok()) {
                throw GraphCompileError(
                    std::move(compiled.diagnostics));
            }
            ExecutionPlan execution =
                std::move(*compiled.output);
            std::shared_ptr<const ScheduledGraph> scheduled =
                execution.scheduledPtr();
            std::vector<DGraph> dgraphs =
                execution.takeDGraphs();
            BackendResourceBindings resources =
                execution.takeResources();
            wireRendezvousAccessors();
            std::vector<std::shared_ptr<IBridge>> bridgePinList;
            bridgePinList.reserve(bridgePins.size());
            for (auto& [key, bridge] : bridgePins) {
                (void)key;
                bridgePinList.push_back(std::move(bridge));
            }
            return CompiledGraph(std::move(dgraphs),
                                 std::move(snapshotScalars),
                                 rootRegion_->declaredScalars(),
                                 std::move(bridgePinList),
                                 std::move(scheduled),
                                 std::move(resources));
        } catch (const GraphCompileError&) {
            throw;
        } catch (const std::runtime_error& error) {
            Diagnostics diagnostics;
            diagnostics.error(DiagCode::CompilerError, error.what());
            throw GraphCompileError(std::move(diagnostics), error.what());
        }
    }

   private:
    /**
     * @brief Give the CPU device read/write access to the FPGA's signal array.
     *
     * A split cross-device loop runs its CPU body slice concurrently with the
     * FPGA queue, rendezvousing per iteration through host-visible signal slots
     * that live in the FPGA's BAR window. The CPU device polls and SETs those
     * slots while executing its CompiledWaitNode / CompiledSignalNode halves;
     * this wires it to the FPGA window so the two queues share the same slots.
     *
     * The Phase D compiler restricts a split loop to exactly one FPGA and one
     * CPU device, so a single accessor pair (to the first FPGA window) suffices.
     */
    void wireRendezvousAccessors() {
        std::shared_ptr<CpuDevice> cpu = cpuDevice();
        if (!cpu) return;
        std::shared_ptr<FpgaDevice> fpga;
        for (const auto& [id, device] : devices_) {
            (void)id;
            if (auto f = std::dynamic_pointer_cast<FpgaDevice>(device)) {
                fpga = f;
                break;
            }
        }
        if (!fpga) return;
        std::shared_ptr<fpga::Rp1BarWindow> win = fpga->window();
        if (!win) return;
        cpu->setSignalAccessors(
            [win](std::uint32_t slot) -> std::uint32_t {
                rp1_signal_slot_t s{};
                win->readSignal(slot, s);
                return s.value;
            },
            [win](std::uint32_t slot, std::uint32_t value) {
                win->writeU32(static_cast<std::uint32_t>(
                                  RP1_DEFAULT_SIG_ARRAY_OFFSET +
                                  slot * sizeof(rp1_signal_slot_t) +
                                  offsetof(rp1_signal_slot_t, value)),
                              value);
            });
    }

    static const char* deviceTypeName(DeviceType dt) {
        switch (dt) {
            case DeviceType::CPU:      return "CPU";
            case DeviceType::GPU:      return "GPU";
            case DeviceType::FPGA:     return "FPGA";
            case DeviceType::MOCK_CPU: return "MOCK_CPU";
        }
        return "unknown";
    }

    std::shared_ptr<GraphRegion>                              rootRegion_ =
        GraphRegion::createRoot();
    RegionBuilder                                             rootBuilder_{rootRegion_};
    std::map<std::string, std::shared_ptr<IDevice>>           devices_;    // device-id → device
    std::map<std::pair<DeviceType, DeviceType>, BridgeFactory> bridgeFactories_;
    std::map<std::pair<std::string, std::string>,
             std::shared_ptr<IBridge>>                        bridgeInstances_;

};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_GRAPH_HPP
