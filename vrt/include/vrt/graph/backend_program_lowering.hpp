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
 * @file backend_program_lowering.hpp
 * @brief Materialize scheduled queues into executable backend programs.
 */

#ifndef VRT_GRAPH_BACKEND_PROGRAM_LOWERING_HPP
#define VRT_GRAPH_BACKEND_PROGRAM_LOWERING_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <vrt/graph/backend_resource_binding.hpp>
#include <vrt/graph/compile_result.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>

namespace vrt::graph {

using BridgeLookup =
    std::function<IBridge*(const std::string&, const std::string&)>;

class BackendPrograms {
   public:
    BackendPrograms(
        std::vector<DGraph> dgraphs,
        BackendResourceBindings resources)
        : dgraphs_(std::move(dgraphs)),
          resources_(std::move(resources)) {}

    BackendPrograms(const BackendPrograms&) = delete;
    BackendPrograms& operator=(const BackendPrograms&) = delete;
    BackendPrograms(BackendPrograms&&) noexcept = default;
    BackendPrograms& operator=(BackendPrograms&&) noexcept = default;

    std::vector<DGraph> takeDGraphs() {
        return std::move(dgraphs_);
    }

    BackendResourceBindings takeResources() {
        return std::move(resources_);
    }

   private:
    std::vector<DGraph>       dgraphs_;
    BackendResourceBindings   resources_;
};

CompileResult<BackendPrograms> lowerBackendPrograms(
    const ScheduledGraph& scheduled,
    const std::map<std::string, std::shared_ptr<IDevice>>& devices,
    const BridgeLookup& bridgeFor,
    const std::shared_ptr<std::map<std::string, std::uint64_t>>&
        scalarValues);

}  // namespace vrt::graph

#endif  // VRT_GRAPH_BACKEND_PROGRAM_LOWERING_HPP
