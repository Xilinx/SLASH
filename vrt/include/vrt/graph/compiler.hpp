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
 * @brief Staged graph compiler entry point.
 *
 * Compilation validates an authored snapshot, resolves typed values, places
 * work, routes transfers, schedules queues, binds resources, and lowers
 * backend programs.
 */

#ifndef VRT_GRAPH_COMPILER_HPP
#define VRT_GRAPH_COMPILER_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <vrt/graph/compile_result.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/execution_plan.hpp>
#include <vrt/graph/ir/authored_graph.hpp>

namespace vrt::graph {

class GraphCompiler {
   public:
    using BridgeFor =
        std::function<IBridge*(const std::string& srcDevId,
                               const std::string& dstDevId)>;

    CompileResult<ExecutionPlan> compile(
        const AuthoredGraph&                                   authored,
        const std::map<std::string, std::shared_ptr<IDevice>>& devices,
        const std::map<std::pair<DeviceType, DeviceType>,
                       BridgeFactory>&                         bridgeFactories,
        const BridgeFor&                                       bridgeFor,
        const std::shared_ptr<std::map<std::string, std::uint64_t>>&
            scalarValues) const;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_COMPILER_HPP
