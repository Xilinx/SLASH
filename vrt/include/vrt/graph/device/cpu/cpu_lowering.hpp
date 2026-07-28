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
 * @brief Pure DGraph to CPU runtime-program lowering.
 */

#ifndef VRT_GRAPH_DEVICE_CPU_CPU_LOWERING_HPP
#define VRT_GRAPH_DEVICE_CPU_CPU_LOWERING_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <vrt/graph/node/compiled_node.hpp>

namespace vrt::graph {

struct DGraph;

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

struct CpuProgramNode {
    std::string                  id;
    CpuProgramNodeKind           kind = CpuProgramNodeKind::Boundary;
    std::size_t                  initialUnmet = 0;
    std::vector<std::size_t>     successors;
    CompiledKernelNode           kernel;
    CompiledBoundaryNode         boundary;
    CompiledLoopNode             loop;
    CompiledConditionalNode      conditional;
    std::function<bool()>        tryReady;
    std::function<void()>        action;
    std::uint32_t                signalSlot = 0;
    std::uint32_t                signalValue = 0;
    std::uint16_t                signalOp = 0;
    std::uint16_t                conditionOp = 0;
};

struct CpuProgram {
    std::vector<CpuProgramNode> nodes;
};

class CpuLowering {
   public:
    static CpuProgram lower(const DGraph& dgraph);
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DEVICE_CPU_CPU_LOWERING_HPP
