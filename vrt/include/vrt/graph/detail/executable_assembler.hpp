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
 * @file detail/executable_assembler.hpp
 * @brief Bind resources and assemble direct scheduled-queue executables.
 */

#ifndef VRT_GRAPH_DETAIL_EXECUTABLE_ASSEMBLER_HPP
#define VRT_GRAPH_DETAIL_EXECUTABLE_ASSEMBLER_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <vrt/graph/backend_executable.hpp>
#include <vrt/graph/backend_resource_binding.hpp>
#include <vrt/graph/backend_runtime.hpp>
#include <vrt/graph/compile_result.hpp>
#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/device/device.hpp>

namespace vrt::graph::detail {

using BridgeLookup =
    std::function<IBridge*(const std::string&, const std::string&)>;

struct ExecutionIoMetadata {
    std::set<std::string>              inputBuffers;
    std::set<std::string>              outputBuffers;
    std::set<std::string>              inputScalars;
    std::set<std::string>              outputScalars;
    std::map<std::string, std::string> sizeScalars;
};

class AssembledExecutables {
   public:
    AssembledExecutables(
        BackendResourceBindings resources,
        std::shared_ptr<BackendRuntimeState> runtimeState,
        ExecutionIoMetadata io,
        std::vector<std::shared_ptr<IDevice>> devicePins,
        std::vector<std::unique_ptr<IBackendExecutable>> executables,
        std::vector<IBackendExecutable*> roots)
        : devicePins_(std::move(devicePins)),
          resources_(std::move(resources)),
          runtimeState_(std::move(runtimeState)),
          io_(std::move(io)),
          executables_(std::move(executables)),
          roots_(std::move(roots)) {}

    AssembledExecutables(const AssembledExecutables&) = delete;
    AssembledExecutables& operator=(const AssembledExecutables&) = delete;
    AssembledExecutables(AssembledExecutables&&) noexcept = default;
    AssembledExecutables& operator=(AssembledExecutables&&) = delete;

    BackendResourceBindings takeResources() {
        return std::move(resources_);
    }
    std::shared_ptr<BackendRuntimeState> takeRuntimeState() {
        return std::move(runtimeState_);
    }
    ExecutionIoMetadata takeIo() { return std::move(io_); }
    std::vector<std::shared_ptr<IDevice>> takeDevicePins() {
        return std::move(devicePins_);
    }
    std::vector<std::unique_ptr<IBackendExecutable>> takeExecutables() {
        return std::move(executables_);
    }
    std::vector<IBackendExecutable*> takeRoots() {
        return std::move(roots_);
    }

   private:
    std::vector<std::shared_ptr<IDevice>> devicePins_;
    BackendResourceBindings resources_;
    std::shared_ptr<BackendRuntimeState> runtimeState_;
    ExecutionIoMetadata io_;
    std::vector<std::unique_ptr<IBackendExecutable>> executables_;
    std::vector<IBackendExecutable*> roots_;
};

CompileResult<AssembledExecutables> assembleExecutables(
    const ScheduledGraph& scheduled,
    const std::map<std::string, std::shared_ptr<IDevice>>& devices,
    const BridgeLookup& bridgeFor,
    const std::shared_ptr<std::map<std::string, std::uint64_t>>& scalarValues);

}  // namespace vrt::graph::detail

#endif  // VRT_GRAPH_DETAIL_EXECUTABLE_ASSEMBLER_HPP
