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
 * @file rp1_program.hpp
 * @brief FPGA-private queue program lowered directly from a scheduled queue.
 *
 * This is the typed boundary between scheduling and RP1 packet construction.
 * It retains graph tokens, scoped publications, and symbolic dependencies;
 * physical addresses, barriers, packet indices, and launch pins come later.
 */

#ifndef VRT_GRAPH_DEVICE_FPGA_RP1_PROGRAM_HPP
#define VRT_GRAPH_DEVICE_FPGA_RP1_PROGRAM_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <vrt/graph/control/condition.hpp>
#include <vrt/graph/ids.hpp>
#include <vrt/graph/detail/port_bindings.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>

namespace vrt::graph {

/*
 * Executable work commands retain enough graph metadata for late argument and
 * image resolution. Reprogram addresses are deliberately absent here because
 * PDI bytes are staged and pinned only when a launch is prepared.
 */
struct Rp1KernelCommand {
    std::string id;
    KernelDescriptor kernel;
    std::string deviceId;
    detail::PortBindings ioMap;
    std::vector<std::string> dependsOn;
};

struct Rp1ReprogramCommand {
    std::string id;
    std::string deviceId;
    std::string imageId;
    std::string pdiPath;
    std::uint32_t timeoutCycles = 0;
    std::vector<std::string> dependsOn;
};

struct Rp1ScalarBoundaryCopy {
    std::string sourceName;
    std::uint64_t sourceScopeId = 0;
    std::string targetName;
    std::uint64_t targetScopeId = 0;
};

struct Rp1BufferBoundaryCopy {
    std::string sourceName;
    std::uint64_t sourceScopeId = 0;
    std::string targetName;
    std::uint64_t targetScopeId = 0;
};

/*
 * A boundary command publishes token identity across a control scope. It never
 * becomes an RP1 packet directly: buffer copies lower to aliases, while scalar
 * copies select persistent signal slots for carried values.
 */
struct Rp1BoundaryCommand {
    enum class Side { Start, End };

    std::string id;
    std::string deviceId;
    Side side = Side::Start;
    std::vector<Rp1ScalarBoundaryCopy> scalarCopies;
    std::vector<Rp1BufferBoundaryCopy> bufferCopies;
    std::vector<std::string> dependsOn;
};

enum class Rp1LoopKind {
    FixedCount,
    WhileCondition,
};

/*
 * Control publications keep parent and child tokens distinct until all child
 * queues are attached. Placements identify the owning device; publication
 * records describe the alias or scalar value selected when control completes.
 */
struct Rp1LoopBufferPublication {
    std::string portName;
    std::string parentTokenName;
    std::uint64_t parentScopeId = 0;
    std::string sourceTokenName;
    std::uint64_t sourceScopeId = 0;
    std::string sourceDeviceId;
};

struct Rp1LoopScalarPublication {
    std::string portName;
    std::string parentTokenName;
    std::uint64_t parentScopeId = 0;
    std::string sourceTokenName;
    std::uint64_t sourceScopeId = 0;
    std::string sourceDeviceId;
};

struct Rp1ConditionalBufferPublication {
    std::string portName;
    std::string parentTokenName;
    std::uint64_t parentScopeId = 0;
    std::string thenSourceTokenName;
    std::uint64_t thenSourceScopeId = 0;
    std::string thenSourceDeviceId;
    std::string elseSourceTokenName;
    std::uint64_t elseSourceScopeId = 0;
    std::string elseSourceDeviceId;
};

struct Rp1ConditionalScalarPublication {
    std::string portName;
    std::string parentTokenName;
    std::uint64_t parentScopeId = 0;
    std::string thenSourceTokenName;
    std::uint64_t thenSourceScopeId = 0;
    std::string thenSourceDeviceId;
    std::string elseSourceTokenName;
    std::uint64_t elseSourceScopeId = 0;
    std::string elseSourceDeviceId;
};

enum class Rp1SplitRole {
    None,
    Authority,
    Follower,
};

/*
 * Split loops share a value/ready/acknowledgement slot protocol. The authority
 * supplies duration; followers must not apply a local iteration cap because
 * they leave the protocol only when the authority broadcasts termination.
 */
struct Rp1LoopCommand {
    std::string id;
    std::string deviceId;
    std::vector<std::string> dependsOn;
    Rp1LoopKind loopKind = Rp1LoopKind::FixedCount;
    std::optional<LoopTripCount> tripCount;
    std::optional<Condition> condition;
    Rp1SplitRole broadcastRole = Rp1SplitRole::None;
    std::uint32_t conditionBroadcastSlot = 0;
    std::uint32_t broadcastReadySlot = 0;
    std::uint32_t broadcastAckSlot = 0;
    DeviceId broadcastResourceOwner;
    std::map<std::string, std::string> outputBufferPlacements;
    std::map<std::string, std::string> outputScalarPlacements;
    std::vector<Rp1LoopBufferPublication> outputBufferPublications;
    std::vector<Rp1LoopScalarPublication> outputScalarPublications;
};

struct Rp1ConditionalCommand {
    std::string id;
    std::string deviceId;
    std::vector<std::string> dependsOn;
    Condition condition = Condition::alwaysFalse();
    std::map<std::string, std::string> outputBufferPlacements;
    std::map<std::string, std::string> outputScalarPlacements;
    std::vector<Rp1ConditionalBufferPublication>
        outputBufferPublications;
    std::vector<Rp1ConditionalScalarPublication>
        outputScalarPublications;
};

/*
 * Signal and wait commands refer to already leased physical slots. A
 * pre-launch wait is host-polled before submission so RP1 cannot occupy the
 * device while waiting for bridge input that the scheduler still must stage.
 */
struct Rp1SignalCommand {
    std::string id;
    std::string deviceId;
    std::vector<std::string> dependsOn;
    std::uint32_t slot = 0;
    DeviceId resourceOwner;
    std::uint32_t value = 0;
    std::uint16_t operation = 0;
};

struct Rp1WaitCommand {
    std::string id;
    std::string deviceId;
    std::vector<std::string> dependsOn;
    std::uint32_t slot = 0;
    DeviceId resourceOwner;
    std::uint32_t value = 0;
    std::uint16_t conditionOp = 0;
    bool preLaunch = false;
};

using Rp1Command =
    std::variant<Rp1KernelCommand, Rp1ReprogramCommand,
                 Rp1BoundaryCommand, Rp1LoopCommand,
                 Rp1ConditionalCommand, Rp1SignalCommand,
                 Rp1WaitCommand>;

/*
 * IDs and dependency lists are intentionally uniform across command variants.
 * Image builders use these accessors without losing the closed typed variant
 * needed for exhaustive packet lowering.
 */
inline const std::string& rp1CommandId(const Rp1Command& command) {
    return std::visit(
        [](const auto& concrete) -> const std::string& {
            return concrete.id;
        },
        command);
}

inline const std::vector<std::string>& rp1CommandDependsOn(
    const Rp1Command& command) {
    return std::visit(
        [](const auto& concrete)
            -> const std::vector<std::string>& {
            return concrete.dependsOn;
        },
        command);
}

enum class Rp1ChildRole {
    LoopBody,
    ConditionalThen,
    ConditionalElse,
};

struct Rp1QueueProgram;

/*
 * Child programs stay grouped by parent command and semantic arm. Finalization
 * flattens only the requested role, preserving start/work/end boundary order
 * across the scheduler's per-device queue slices.
 */
struct Rp1ChildProgram {
    std::string parentCommandId;
    Rp1ChildRole role = Rp1ChildRole::LoopBody;
    std::vector<std::shared_ptr<Rp1QueueProgram>> programs;
};

/*
 * One queue program shares launch-time scalar storage with sibling executables.
 * `resourcesLeased` distinguishes compiler-owned physical slots from legacy
 * direct programs that allocate locally. Children remain typed until control
 * topology is complete and one RP1 image can be built.
 */
struct Rp1QueueProgram {
    DeviceId device;
    std::vector<Rp1Command> commands;
    std::shared_ptr<std::map<std::string, std::uint64_t>>
        scalarValues;
    std::map<std::string, BackendScalarId> scalarResources;
    bool resourcesLeased = false;
    std::vector<Rp1ChildProgram> children;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DEVICE_FPGA_RP1_PROGRAM_HPP
