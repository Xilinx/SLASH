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
 * @file backend_executable.hpp
 * @brief Queue-local backend executable seam owned by an execution plan.
 */

#ifndef VRT_GRAPH_BACKEND_EXECUTABLE_HPP
#define VRT_GRAPH_BACKEND_EXECUTABLE_HPP

#include <memory>
#include <vector>

#include <vrt/graph/ids.hpp>

namespace vrt::graph {

class BackendResourceBindings;
class BackendRuntimeState;
class HostActionTable;
class ScheduledGraph;
struct QueueProgram;

namespace detail {

/**
 * @brief Marks code running on a backend-owned worker thread.
 *
 * Execution uses this marker to reject a worker recursively waiting on, or
 * destroying, the execution that owns it. Such reentry cannot join safely.
 */
class BackendWorkerScope {
   public:
    BackendWorkerScope() noexcept;

    BackendWorkerScope(const BackendWorkerScope&) = delete;
    BackendWorkerScope& operator=(const BackendWorkerScope&) = delete;

    ~BackendWorkerScope();

    static bool active() noexcept;

   private:
    bool previous_;
};

}  // namespace detail

/**
 * @brief Complete immutable input for lowering one scheduled queue.
 */
/*
 * scheduled, queue, resources, and hostActions are assembly-time borrows.
 * A lowerer must copy every command and callback it needs before returning.
 * runtimeState is shared because queue executables and bridge callbacks use
 * the same scalar snapshot after the compiler's temporary context is gone.
 */
struct BackendLoweringContext {
    const ScheduledGraph&          scheduled;
    const QueueProgram&            queue;
    const BackendResourceBindings& resources;
    std::shared_ptr<BackendRuntimeState> runtimeState;
    const HostActionTable&               hostActions;
};

enum class ControlChildRole {
    LoopBody,
    ConditionalThen,
    ConditionalElse,
};

class IBackendExecutable;

/**
 * @brief Typed non-owning reference to a lowered queue executable.
 */
struct QueueExecutableHandle {
    QueueId             queue;
    IBackendExecutable* executable = nullptr;

    explicit operator bool() const { return executable != nullptr; }
};

/**
 * @brief Typed reference to a control operation in a parent queue.
 */
struct ControlExecutableHandle {
    QueueId        queue;
    ScheduleStepId step;
    NodeId         control;
};

/**
 * @brief Prepared backend work for one ScheduledGraph queue slice.
 */
/*
 * Assembly first connects child queues and finalizes the executable tree.
 * For each run, prepareLaunch() completes fallible pre-submit work before
 * launch() starts anything; wait() must join or otherwise retire all work,
 * and may report the saved asynchronous failure.
 */
class IBackendExecutable {
   public:
    virtual ~IBackendExecutable() = default;

    virtual QueueId queue() const = 0;
    virtual DeviceId device() const = 0;

    virtual void connectControlChildren(
        ControlExecutableHandle,
        ControlChildRole,
        std::vector<QueueExecutableHandle>) {}

    virtual void finalize() {}
    virtual void prepareLaunch() {}
    virtual void launch() = 0;
    virtual void wait() = 0;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_BACKEND_EXECUTABLE_HPP
