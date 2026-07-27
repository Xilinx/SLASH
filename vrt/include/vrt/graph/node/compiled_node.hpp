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
 * @file compiled_node.hpp
 * @brief Compiled graph node variants produced by GraphCompiler and consumed by device runtimes.
 */

#ifndef VRT_GRAPH_NODE_COMPILED_NODE_HPP
#define VRT_GRAPH_NODE_COMPILED_NODE_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <vrt/graph/control/condition.hpp>
#include <vrt/graph/crossdevice/bridge_op.hpp>
#include <vrt/graph/node/io_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>

namespace vrt::graph {

struct CompiledKernelNode {
    std::string id;
    KernelDescriptor kernel;
    std::string deviceId;
    IOMap ioMap;
    std::vector<std::string> dependsOn;
};

struct CompiledBridgeOpNode {
    enum class Side { Producer, Consumer };

    std::string id;
    std::string deviceId;
    std::shared_ptr<IBridgeOp> op;
    std::function<void()> action;
    Side side = Side::Producer;
    std::string pairedKernelId;
    std::vector<std::string> dependsOn;
    std::function<bool()> tryReady = []{ return true; };
};

struct CompiledDeviceCopyNode {
    std::string id;
    std::string deviceId;
    GraphBuffer source;
    GraphBuffer target;
    std::string sourceRegion;
    std::string targetRegion;
    std::string mechanism = "host_qdma";
    std::function<void()> action;
    std::vector<std::string> dependsOn;
};

struct CompiledSourceNode {
    std::string id;
    std::string deviceId;
    std::vector<std::string> inputBufferKeys;
    std::vector<std::string> inputScalarKeys;
    std::vector<std::string> dependsOn;
};

struct CompiledSinkNode {
    std::string id;
    std::string deviceId;
    std::vector<std::string> outputBufferKeys;
    std::vector<std::string> outputScalarKeys;
    std::vector<std::string> dependsOn;
};

struct CompiledReprogramNode {
    std::string id;
    std::string deviceId;
    std::string imageId;
    std::string pdiPath;
    uint32_t timeoutCycles = 0;
    std::vector<std::string> dependsOn;
};

struct CompiledScalarBoundaryCopy {
    std::string sourceName;
    uint64_t sourceScopeId = 0;
    std::string targetName;
    uint64_t targetScopeId = 0;
};

struct CompiledBufferBoundaryCopy {
    std::string sourceName;
    uint64_t sourceScopeId = 0;
    std::string targetName;
    uint64_t targetScopeId = 0;
};

struct CompiledBoundaryNode {
    enum class Side { Start, End };

    std::string id;
    std::string deviceId;
    Side side = Side::Start;
    std::vector<CompiledScalarBoundaryCopy> scalarCopies;
    std::vector<CompiledBufferBoundaryCopy> bufferCopies;
    std::vector<std::string> dependsOn;
};

enum class CompiledLoopKind {
    FixedCount,
    WhileCondition,
};

/// Describes a single buffer port a loop publishes back to its parent
/// region. The body produces one source token (per-iteration); after the
/// loop finishes, that token is exposed under @c parentTokenName in the
/// parent scope.
struct CompiledLoopBufferPublication {
    std::string portName;
    std::string parentTokenName;
    uint64_t    parentScopeId = 0;
    std::string sourceTokenName;
    uint64_t    sourceScopeId = 0;
    std::string sourceDeviceId;
};

/// Scalar counterpart to CompiledLoopBufferPublication.
struct CompiledLoopScalarPublication {
    std::string portName;
    std::string parentTokenName;
    uint64_t    parentScopeId = 0;
    std::string sourceTokenName;
    uint64_t    sourceScopeId = 0;
    std::string sourceDeviceId;
};

/// Describes a single buffer port a conditional publishes back to its parent
/// region. Unlike a loop, both branches may contribute a source token, so
/// the publication carries a then-side and an else-side source descriptor.
struct CompiledConditionalBufferPublication {
    std::string portName;
    std::string parentTokenName;
    uint64_t    parentScopeId = 0;
    std::string thenSourceTokenName;
    uint64_t    thenSourceScopeId = 0;
    std::string thenSourceDeviceId;
    std::string elseSourceTokenName;
    uint64_t    elseSourceScopeId = 0;
    std::string elseSourceDeviceId;
};

/// Scalar counterpart to CompiledConditionalBufferPublication.
struct CompiledConditionalScalarPublication {
    std::string portName;
    std::string parentTokenName;
    uint64_t    parentScopeId = 0;
    std::string thenSourceTokenName;
    uint64_t    thenSourceScopeId = 0;
    std::string thenSourceDeviceId;
    std::string elseSourceTokenName;
    uint64_t    elseSourceScopeId = 0;
    std::string elseSourceDeviceId;
};

/// Roles a replicated control node plays in a data-dependent cross-device split.
enum class SplitBroadcastRole {
    None,       ///< Not a data-dependent split replica.
    Authority,  ///< Evaluates the real condition and broadcasts the decision.
    Follower,   ///< Reads the broadcast decision as its exit predicate.
};

struct CompiledLoopNode {
    std::string                                 id;
    std::string                                 deviceId;
    std::vector<std::string>                    dependsOn;
    CompiledLoopKind                            loopKind = CompiledLoopKind::FixedCount;
    std::optional<LoopTripCount>                tripCount;
    std::optional<Condition>                    condition;
    /// Data-dependent split broadcast wiring.  When @c broadcastRole is not
    /// @c None, the loop runs as two rendezvousing queues whose continue/stop
    /// decision is broadcast over host-visible signal slots (do-while shape: the
    /// body runs, then the Authority decides whether to iterate again):
    ///
    ///  - @c conditionBroadcastSlot carries the decision (1 = stop, 0 = continue).
    ///  - @c broadcastReadySlot is raised by the Authority once the decision is
    ///    written (Authority -> Follower); the Follower's body tail awaits it
    ///    before re-arming the LOOP, so the next top-of-loop check sees a fresh
    ///    decision.
    ///  - @c broadcastAckSlot is raised by the Follower once it has consumed the
    ///    decision (Follower -> Authority), so the Authority does not outpace it.
    ///
    /// The Authority replica (a CPU queue) evaluates @c condition and drives the
    /// handshake; the Follower replica (an FPGA queue) reads
    /// @c conditionBroadcastSlot as its RP1 LOOP exit predicate.
    SplitBroadcastRole                          broadcastRole = SplitBroadcastRole::None;
    std::uint32_t                               conditionBroadcastSlot = 0;
    std::uint32_t                               broadcastReadySlot = 0;
    std::uint32_t                               broadcastAckSlot = 0;
    std::map<std::string, std::string>          outputBufferPlacements;
    std::map<std::string, std::string>          outputScalarPlacements;
    std::vector<CompiledLoopBufferPublication>  outputBufferPublications;
    std::vector<CompiledLoopScalarPublication>  outputScalarPublications;
};

struct CompiledConditionalNode {
    std::string                                       id;
    std::string                                       deviceId;
    std::vector<std::string>                          dependsOn;
    Condition                                         condition = Condition::alwaysFalse();
    std::map<std::string, std::string>                outputBufferPlacements;
    std::map<std::string, std::string>                outputScalarPlacements;
    std::vector<CompiledConditionalBufferPublication> outputBufferPublications;
    std::vector<CompiledConditionalScalarPublication> outputScalarPublications;
};

/// Cross-queue rendezvous: raise a signal slot for a peer queue.  Lowered to
/// RP1_OP_SIGNAL on FPGA, or a host BAR write on CPU (Phase E).  @c operation
/// and @c value carry the raw rp1_sigop_t / 32-bit value (the depth-1 handshake
/// uses SET 1 to produce and SET 0 to acknowledge/clear).
struct CompiledSignalNode {
    std::string              id;
    std::string              deviceId;
    std::vector<std::string> dependsOn;
    std::uint32_t            slot = 0;
    std::uint32_t            value = 0;
    std::uint16_t            operation = 0;  // rp1_sigop_t
};

/// Cross-queue rendezvous: block until a signal slot satisfies a comparison.
/// Lowered to RP1_OP_WAIT on FPGA, or a host BAR poll on CPU (Phase E).
/// @c conditionOp carries the raw rp1_condop_t; the depth-1 handshake uses
/// AND_NZ (slot != 0) to await a peer's produce/ack.
struct CompiledWaitNode {
    std::string              id;
    std::string              deviceId;
    std::vector<std::string> dependsOn;
    std::uint32_t            slot = 0;
    std::uint32_t            value = 0;
    std::uint16_t            conditionOp = 0;  // rp1_condop_t
};

using CompiledNode = std::variant<CompiledKernelNode,
                                  CompiledBridgeOpNode,
                                  CompiledDeviceCopyNode,
                                  CompiledSourceNode,
                                  CompiledSinkNode,
                                  CompiledReprogramNode,
                                  CompiledBoundaryNode,
                                  CompiledLoopNode,
                                  CompiledConditionalNode,
                                  CompiledSignalNode,
                                  CompiledWaitNode>;

inline const std::string& compiledNodeId(const CompiledNode& node) {
    return std::visit([](const auto& concrete) -> const std::string& {
        return concrete.id;
    }, node);
}

inline const std::string& compiledNodeDeviceId(const CompiledNode& node) {
    return std::visit([](const auto& concrete) -> const std::string& {
        return concrete.deviceId;
    }, node);
}

inline const std::vector<std::string>& compiledNodeDependsOn(const CompiledNode& node) {
    return std::visit([](const auto& concrete) -> const std::vector<std::string>& {
        return concrete.dependsOn;
    }, node);
}

inline bool isCompiledBridgeOp(const CompiledNode& node) {
    return std::holds_alternative<CompiledBridgeOpNode>(node);
}

}  // namespace vrt::graph

#endif  // VRT_GRAPH_NODE_COMPILED_NODE_HPP
