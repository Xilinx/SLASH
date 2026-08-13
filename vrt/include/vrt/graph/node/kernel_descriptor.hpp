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
 * @file kernel_descriptor.hpp
 * @brief KernelDescriptor — device-independent kernel identity and I/O signature.
 *
 * A KernelDescriptor describes a kernel at the type level: what kind of device
 * it runs on, which bitstream/binary image it belongs to (if applicable), and
 * the typed I/O port signature (IOTypeMap).  It does not carry any runtime
 * state or device binding.
 *
 * Multiple Nodes in a Graph may reference the same KernelDescriptor.
 */

#ifndef VRT_GRAPH_NODE_KERNEL_DESCRIPTOR_HPP
#define VRT_GRAPH_NODE_KERNEL_DESCRIPTOR_HPP

#include <optional>
#include <string>

#include <vrt/graph/node/io_type_map.hpp>
#include <vrt/graph/core/types.hpp>

namespace vrt::graph {

struct KernelDescriptor {
    /**
     * @brief Logical kernel name.
     *
     * For FPGA kernels this must match the name used in the system_map / bitstream.
     * For CPU/GPU kernels it is a user-assigned identifier.
     */
    std::string name;

    /**
     * @brief Class of device this kernel runs on.
     */
    DeviceType type;

    /**
     * @brief Bitstream or binary image that contains this kernel.
     *
     * Required for FPGA kernels.  nullopt for CPU kernels (linked directly)
     * and may be omitted for GPU kernels when the binary is implicit.
     */
    std::optional<std::string> image;

    /**
     * @brief Typed I/O port signature.
     */
    IOTypeMap ioType;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_NODE_KERNEL_DESCRIPTOR_HPP
