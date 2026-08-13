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
 * @file rp1_lowering.hpp
 * @brief Scheduled FPGA queue lowering boundary.
 */

#ifndef VRT_GRAPH_DEVICE_FPGA_RP1_LOWERING_HPP
#define VRT_GRAPH_DEVICE_FPGA_RP1_LOWERING_HPP

#include <memory>
#include <utility>
#include <vector>

#include <vrt/graph/backend_executable.hpp>
#include <vrt/graph/device/fpga/rp1_program.hpp>

namespace vrt::graph {

class FpgaDevicePlan;

/**
 * @brief Backend-owned description of one scheduled RP1 queue.
 *
 * Queue, device, and step identity remain visible to the scheduler. The typed
 * command program stays private until child control queues have been attached
 * and `FpgaDevicePlan` can build one complete RP1 image.
 */
class Rp1Program {
   public:
    QueueId queue() const { return queue_; }
    DeviceId device() const { return device_; }
    const std::vector<ScheduleStepId>& steps() const {
        return steps_;
    }

   private:
    friend class Rp1Lowering;
    friend class FpgaDevicePlan;

    Rp1Program(
        QueueId queue, DeviceId device,
        std::vector<ScheduleStepId> steps,
        std::shared_ptr<Rp1QueueProgram> program)
        : queue_(queue), device_(std::move(device)),
          steps_(std::move(steps)),
          program_(std::move(program)) {}

    QueueId queue_;
    DeviceId device_;
    std::vector<ScheduleStepId> steps_;
    std::shared_ptr<Rp1QueueProgram> program_;
};

/**
 * @brief Pure queue-slice translation into an RP1 backend program.
 *
 * Operations become typed kernel, reprogram, loop, or conditional commands;
 * device rendezvous become signal/wait commands; control boundaries preserve
 * scoped publications. Host-only steps are elided and dependencies are wired
 * through them to the nearest concrete RP1 predecessor.
 *
 * This pass performs no BAR access, packet packing, barrier allocation, or
 * device-memory allocation. Those require the finalized control topology and
 * are owned by `FpgaDevicePlan`.
 */
class Rp1Lowering {
   public:
    static Rp1Program lower(
        const BackendLoweringContext& context);
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DEVICE_FPGA_RP1_LOWERING_HPP
