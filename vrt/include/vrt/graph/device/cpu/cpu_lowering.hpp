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
 * @file cpu_lowering.hpp
 * @brief Direct ScheduledGraph queue to CPU runtime-program lowering.
 */

#ifndef VRT_GRAPH_DEVICE_CPU_CPU_LOWERING_HPP
#define VRT_GRAPH_DEVICE_CPU_CPU_LOWERING_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <vrt/graph/backend_executable.hpp>
#include <vrt/graph/control/condition.hpp>
#include <vrt/graph/control/control_node.hpp>
#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/detail/port_bindings.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>

namespace vrt::graph {

class IBridgeOp;

/*
 * Local controls need no protocol. In a split loop the CPU authority
 * evaluates the condition and publishes each decision; the device follower
 * consumes that decision and acknowledges it before either side advances.
 * Current placement uses CPU as authority and FPGA firmware as follower.
 */
enum class CpuControlRole {
    None,
    Authority,
    Follower,
};

enum class CpuProgramNodeKind {
    Kernel,
    ProducerOp,
    ConsumerOp,
    Noop,
    Boundary,
    Loop,
    Conditional,
    Signal,
    Wait,
};

struct CpuKernelCommand {
    KernelDescriptor kernel;
    detail::PortBindings            ioMap;
};

struct CpuBoundaryBufferCopy {
    GraphBuffer source;
    GraphBuffer target;
};

struct CpuBoundaryScalarCopy {
    GraphScalar source;
    GraphScalar target;
};

struct CpuBoundaryCommand {
    std::vector<CpuBoundaryBufferCopy> bufferCopies;
    std::vector<CpuBoundaryScalarCopy> scalarCopies;
};

using CpuControlValue = std::variant<GraphBuffer, GraphScalar>;

struct CpuControlPublication {
    CpuControlValue target;
    CpuControlValue first;
    CpuControlValue second;
    DeviceId        targetDevice;
    DeviceId        firstDevice;
    DeviceId        secondDevice;
};

struct CpuLoopCommand {
    NodeId                                control;
    std::string                           diagnosticId;
    LoopKind                              kind = LoopKind::FixedCount;
    std::optional<LoopTripCount>          tripCount;
    std::optional<Condition>              condition;
    CpuControlRole                        role = CpuControlRole::None;
    /*
     * A split participant uses one owner and three leased slots: value carries
     * continue/stop, decision marks it ready, and acknowledgement proves the
     * follower consumed it. All three must belong to the same owner.
     */
    DeviceId                              resourceOwner;
    BackendResourceId                     value;
    BackendResourceId                     decision;
    BackendResourceId                     acknowledgement;
    std::vector<CpuControlPublication>    publications;
};

struct CpuConditionalCommand {
    NodeId                             control;
    std::string                        diagnosticId;
    std::optional<Condition>           condition;
    std::vector<CpuControlPublication> publications;
};

enum class CpuSignalOperation {
    Set,
    Add,
    Or,
    And,
};

enum class CpuWaitCondition {
    Equal,
    NotEqual,
    Less,
    GreaterEqual,
    AndNonzero,
    AndZero,
};

struct CpuProgramNode {
    ScheduleStepId               step;
    std::string                  diagnosticId;
    CpuProgramNodeKind           kind = CpuProgramNodeKind::Boundary;
    std::size_t                  initialUnmet = 0;
    std::vector<std::size_t>     successors;
    CpuKernelCommand             kernel;
    CpuBoundaryCommand           boundary;
    CpuLoopCommand               loop;
    CpuConditionalCommand        conditional;
    /*
     * HostActionTable does not survive lowering. Keep each bridge operation
     * beside the copied callbacks so staging and semaphore state outlive every
     * readiness probe and action invocation.
     */
    std::vector<std::shared_ptr<IBridgeOp>> actionPins;
    std::function<bool()>        tryReady;
    std::function<void()>        action;
    DeviceId                     resourceOwner;
    BackendResourceId            signalResource;
    std::uint32_t                signalSlot = 0;
    std::uint32_t                signalValue = 0;
    CpuSignalOperation           signalOp = CpuSignalOperation::Set;
    CpuWaitCondition             conditionOp = CpuWaitCondition::AndNonzero;
};

/*
 * initialUnmet and successors encode only dependencies within this queue.
 * Cross-queue ordering is represented by explicit producer/consumer or
 * signal/wait nodes, allowing the CPU scheduler to poll without blocking its
 * only worker on a peer queue.
 */
struct CpuProgram {
    QueueId                     queue;
    DeviceId                    device;
    std::vector<CpuProgramNode> nodes;
};

/*
 * Lowering snapshots every scheduled step into an owned CPU command:
 * operations become kernels or controls, boundaries become local copies, and
 * runtime steps become host actions or rendezvous operations. A final pass
 * wires queue-local dependency counters after all step indexes are known.
 */
class CpuLowering {
   public:
    static CpuProgram lower(const BackendLoweringContext& context);
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DEVICE_CPU_CPU_LOWERING_HPP
