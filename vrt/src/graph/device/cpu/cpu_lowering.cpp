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

#include <vrt/graph/device/cpu/cpu_lowering.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include <vrt/graph/device/dgraph.hpp>

namespace vrt::graph {

CpuProgram CpuLowering::lower(const DGraph& dgraph) {
    CpuProgram program;
    program.nodes.reserve(dgraph.nodes.size());
    std::unordered_map<std::string, std::size_t> indexes;
    indexes.reserve(dgraph.nodes.size());

    for (const CompiledNode& node : dgraph.nodes) {
        CpuProgramNode runtime;
        std::visit(
            [&](const auto& concrete) {
                using T = std::decay_t<decltype(concrete)>;
                runtime.id = concrete.id;
                if constexpr (std::is_same_v<T, CompiledKernelNode>) {
                    runtime.kind = CpuProgramNodeKind::Kernel;
                    runtime.kernel = concrete;
                } else if constexpr (
                    std::is_same_v<T, CompiledBridgeOpNode>) {
                    runtime.kind =
                        concrete.side ==
                                CompiledBridgeOpNode::Side::Producer
                            ? CpuProgramNodeKind::ProducerOp
                            : CpuProgramNodeKind::ConsumerOp;
                    runtime.tryReady = concrete.tryReady;
                    runtime.action = concrete.action;
                } else if constexpr (
                    std::is_same_v<T, CompiledDeviceCopyNode>) {
                    runtime.kind = CpuProgramNodeKind::ProducerOp;
                    runtime.action = concrete.action;
                } else if constexpr (
                    std::is_same_v<T, CompiledSourceNode> ||
                    std::is_same_v<T, CompiledSinkNode>) {
                    runtime.kind = CpuProgramNodeKind::Noop;
                } else if constexpr (
                    std::is_same_v<T, CompiledBoundaryNode>) {
                    runtime.kind = CpuProgramNodeKind::Boundary;
                    runtime.boundary = concrete;
                } else if constexpr (
                    std::is_same_v<T, CompiledLoopNode>) {
                    runtime.kind = CpuProgramNodeKind::Loop;
                    runtime.loop = concrete;
                } else if constexpr (
                    std::is_same_v<T, CompiledConditionalNode>) {
                    runtime.kind = CpuProgramNodeKind::Conditional;
                    runtime.conditional = concrete;
                } else if constexpr (
                    std::is_same_v<T, CompiledReprogramNode>) {
                    throw std::runtime_error(
                        "CpuDevice: reprogram nodes must execute on "
                        "an FPGA device");
                } else if constexpr (
                    std::is_same_v<T, CompiledSignalNode>) {
                    runtime.kind = CpuProgramNodeKind::Signal;
                    runtime.signalSlot = concrete.slot;
                    runtime.signalValue = concrete.value;
                    runtime.signalOp = concrete.operation;
                } else if constexpr (
                    std::is_same_v<T, CompiledWaitNode>) {
                    runtime.kind = CpuProgramNodeKind::Wait;
                    runtime.signalSlot = concrete.slot;
                    runtime.signalValue = concrete.value;
                    runtime.conditionOp = concrete.conditionOp;
                }
            },
            node);
        indexes[runtime.id] = program.nodes.size();
        program.nodes.push_back(std::move(runtime));
    }

    for (std::size_t i = 0; i < dgraph.nodes.size(); ++i) {
        for (const std::string& dependency :
             compiledNodeDependsOn(dgraph.nodes[i])) {
            auto predecessor = indexes.find(dependency);
            if (predecessor == indexes.end()) continue;
            program.nodes[predecessor->second].successors.push_back(i);
            ++program.nodes[i].initialUnmet;
        }
    }
    return program;
}

}  // namespace vrt::graph
