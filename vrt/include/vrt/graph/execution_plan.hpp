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
 * @brief Final compiler output with scheduled and backend programs.
 */

#ifndef VRT_GRAPH_EXECUTION_PLAN_HPP
#define VRT_GRAPH_EXECUTION_PLAN_HPP

#include <memory>
#include <utility>
#include <vector>

#include <vrt/graph/backend_program_lowering.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>

namespace vrt::graph {

class ExecutionPlan {
   public:
    ExecutionPlan(std::shared_ptr<const ScheduledGraph> scheduled,
                  BackendPrograms programs)
        : scheduled_(std::move(scheduled)),
          dgraphs_(programs.takeDGraphs()),
          resources_(programs.takeResources()) {}

    ExecutionPlan(const ExecutionPlan&) = delete;
    ExecutionPlan& operator=(const ExecutionPlan&) = delete;
    ExecutionPlan(ExecutionPlan&&) noexcept = default;
    ExecutionPlan& operator=(ExecutionPlan&&) noexcept = default;

    const ScheduledGraph& scheduled() const { return *scheduled_; }

    const std::shared_ptr<const ScheduledGraph>& scheduledPtr() const {
        return scheduled_;
    }

    std::vector<DGraph> takeDGraphs() {
        return std::move(dgraphs_);
    }

    BackendResourceBindings takeResources() {
        return std::move(resources_);
    }

   private:
    std::shared_ptr<const ScheduledGraph> scheduled_;
    std::vector<DGraph>                   dgraphs_;
    BackendResourceBindings               resources_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_EXECUTION_PLAN_HPP
