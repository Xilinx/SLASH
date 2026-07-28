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
 * @file scheduled_graph.hpp
 * @brief Routed graph expanded into queue programs and logical events.
 */

#ifndef VRT_GRAPH_IR_SCHEDULED_GRAPH_HPP
#define VRT_GRAPH_IR_SCHEDULED_GRAPH_HPP

#include <map>
#include <memory>
#include <vector>

#include <vrt/graph/compile_result.hpp>
#include <vrt/graph/ir/routed_graph.hpp>
#include <vrt/graph/schedule.hpp>

namespace vrt::graph {

class ScheduledGraph {
   public:
    ScheduledGraph(
        std::shared_ptr<const RoutedGraph> routed,
        std::vector<QueueProgram> queues,
        std::map<ScheduleStepId, ScheduledStep> steps,
        std::vector<LogicalRendezvous> rendezvous,
        std::vector<LogicalResourceRequirement> resources)
        : routed_(std::move(routed)),
          queues_(std::move(queues)),
          steps_(std::move(steps)),
          rendezvous_(std::move(rendezvous)),
          resources_(std::move(resources)) {}

    const RoutedGraph& routed() const { return *routed_; }
    const std::vector<QueueProgram>& queues() const { return queues_; }
    const std::map<ScheduleStepId, ScheduledStep>& steps() const {
        return steps_;
    }
    const std::vector<LogicalRendezvous>& rendezvous() const {
        return rendezvous_;
    }
    const std::vector<LogicalResourceRequirement>& resources() const {
        return resources_;
    }

   private:
    std::shared_ptr<const RoutedGraph>             routed_;
    std::vector<QueueProgram>                      queues_;
    std::map<ScheduleStepId, ScheduledStep>        steps_;
    std::vector<LogicalRendezvous>                 rendezvous_;
    std::vector<LogicalResourceRequirement>        resources_;
};

CompileResult<ScheduledGraph> scheduleGraph(
    const RoutedGraph& routed);

}  // namespace vrt::graph

#endif  // VRT_GRAPH_IR_SCHEDULED_GRAPH_HPP
