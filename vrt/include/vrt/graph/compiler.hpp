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
 * @file compiler.hpp
 * @brief GraphCompiler — translates a Graph into a list of per-device DGraphs.
 *
 * GraphCompiler is an internal component; users interact with Graph::compile(),
 * which returns a CompiledGraph executable snapshot.
 *
 * Compilation steps:
 *  1. Topologically sort authored operations within each GraphRegion using
 *     data-dependency edges (derived from IOMap tokens) and explicit afterOps
 *     edges. Nested control-flow bodies are compiled as child regions instead
 *     of participating in the parent region's sort.
 *  2. Group compiled nodes by their resolved deviceId → one DGraph per
 *     device. Loop and conditional operations become parent-level
 *     CompiledLoopNodes / CompiledConditionalNodes with child DGraphs for
 *     their bodies / branches.
 *  3. For each cross-device buffer edge inside one region: ask BridgeRouter for
 *     a `RoutedLeg` (one direct hop or two via the CPU bounce). Each leg's
 *     `BridgeStepPair` is materialised as producer-side and consumer-side
 *     `CompiledBridgeOpNode`s and spliced into the corresponding DGraphs.
 *  4. Return the top-level per-device DGraphs. CompiledGraph owns the follow-up
 *     conversion from each top-level DGraph into a backend-specific
 *     IDevicePlan; control-flow execution is implemented by device runtimes.
 */

#ifndef VRT_GRAPH_COMPILER_HPP
#define VRT_GRAPH_COMPILER_HPP

#include <cstddef>
#include <map>
#include <memory>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/crossdevice/bridge_router.hpp>
#include <vrt/graph/control/graph_region.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/node/compiled_node.hpp>

namespace vrt::graph {

class Graph;  // forward; compiler reads graph internals via a friend accessor

class GraphCompiler {
   public:
    /**
     * @brief Lookup callback used by the compiler to obtain a bridge
     *        instance for a concrete (srcDeviceId, dstDeviceId) pair.
     *
     * Implementations lazily instantiate one bridge per pair from
     * registered factories and cache the result. Returns @c nullptr when no
     * factory is registered for the underlying device-type pair so callers
     * can branch between direct and fall-back paths without
     * exception-as-control-flow. `Graph` wraps its `bridgeFor()` method as
     * this callback.
     */
    using BridgeFor =
        std::function<IBridge*(const std::string& srcDevId,
                               const std::string& dstDevId)>;

    /**
     * @brief Compile a Graph given the registered backends.
     *
     * The compiler is the single validator: it runs all structural checks
     * (graph not empty, devices registered, bridge factories present, root-
     * scope buffer/scalar references resolvable, port bindings well-typed,
     * scopes valid, no cycles) before lowering each region into per-device
     * DGraphs. Callers should treat any thrown error as a fatal authoring
     * problem.
     *
     * @param rootRegion The authored root region of the graph. Nested control
     *                   regions are compiled recursively.
     * @param devices   Map of device-id → device, as registered with Graph.
     * @param bridgeFactories Map of `(srcType, dstType)` → factory, as
     *                  registered with Graph. Used to verify that every non-
     *                  CPU device has matching `{CPU, T}` and `{T, CPU}`
     *                  factories.
     * @param bridgeFor Lookup that returns (lazily creating if needed) the
     *                  bridge instance handling a concrete pair of device ids.
     *                  The compiler invokes this once per cross-device edge it
     *                  materialises.
     * @param scalarValues Shared scalar value store threaded through every
     *                     compiled DGraph for runtime scalar bookkeeping.
     * @return          One DGraph per device, each with its nodes (kernel +
     *                  bridge ops) in execution order.
     *
     * @throws std::runtime_error  If the graph is empty, has no registered
     *                             devices, is missing a required bridge
     *                             factory, contains a cycle, has an unbound
     *                             mandatory port, has a deviceHint with no
     *                             matching device, or `bridgeFor` rejects a
     *                             needed pair.
     */
    std::vector<DGraph> compile(
        const GraphRegion&                                      rootRegion,
        const std::map<std::string, std::shared_ptr<IDevice>>& devices,
        const std::map<std::pair<DeviceType, DeviceType>,
                       BridgeFactory>&                         bridgeFactories,
        const BridgeFor&                                       bridgeFor,
        const std::shared_ptr<std::map<std::string, uint64_t>>& scalarValues);

   private:
    void validateRegionScopes(const GraphRegion& region,
                              const std::set<std::string>& rootProducedScalars = {}) const;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_COMPILER_HPP
