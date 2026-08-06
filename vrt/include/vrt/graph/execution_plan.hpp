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
 * @file execution_plan.hpp
 * @brief Final compiler output owning direct queue executables and resources.
 */

#ifndef VRT_GRAPH_EXECUTION_PLAN_HPP
#define VRT_GRAPH_EXECUTION_PLAN_HPP

#include <memory>
#include <utility>
#include <vector>

#include <vrt/graph/detail/executable_assembler.hpp>

namespace vrt::graph {

/*
 * ExecutionPlan is a single-use ownership handoff from compilation to
 * Execution. roots_ contains non-owning pointers into executables_; the
 * take* methods must move both sides together, along with every runtime pin
 * and lease needed by those executables.
 */
class ExecutionPlan {
   public:
    explicit ExecutionPlan(detail::AssembledExecutables assembled)
        : devicePins_(assembled.takeDevicePins()),
          resources_(assembled.takeResources()),
          runtimeState_(assembled.takeRuntimeState()),
          io_(assembled.takeIo()),
          executables_(assembled.takeExecutables()),
          roots_(assembled.takeRoots()) {}

    ExecutionPlan(const ExecutionPlan&) = delete;
    ExecutionPlan& operator=(const ExecutionPlan&) = delete;
    ExecutionPlan(ExecutionPlan&&) noexcept = default;
    ExecutionPlan& operator=(ExecutionPlan&&) = delete;

    BackendResourceBindings takeResources() {
        return std::move(resources_);
    }

    std::shared_ptr<BackendRuntimeState> takeRuntimeState() {
        return std::move(runtimeState_);
    }

    detail::ExecutionIoMetadata takeIo() {
        return std::move(io_);
    }

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
    /*
     * Reverse destruction drops root indexes and executables before runtime
     * state and resource leases. Device pins are last so lease destructors
     * can still safely refer to their owning devices.
     */
    std::vector<std::shared_ptr<IDevice>> devicePins_;
    BackendResourceBindings               resources_;
    std::shared_ptr<BackendRuntimeState>  runtimeState_;
    detail::ExecutionIoMetadata                  io_;
    std::vector<std::unique_ptr<IBackendExecutable>> executables_;
    std::vector<IBackendExecutable*>      roots_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_EXECUTION_PLAN_HPP
