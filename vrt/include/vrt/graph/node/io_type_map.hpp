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
 * @file io_type_map.hpp
 * @brief IOTypeMap — typed port declarations for a KernelDescriptor.
 *
 * IOTypeMap describes the abstract I/O signature of a kernel: which scalar
 * and buffer ports it exposes, their types, and their directions.
 * detail::PortBindings (a separate class) binds concrete graph values to these ports for a
 * specific node instantiation.
 */

#ifndef VRT_GRAPH_NODE_IO_TYPE_MAP_HPP
#define VRT_GRAPH_NODE_IO_TYPE_MAP_HPP

#include <string>
#include <vector>

#include <vrt/graph/core/types.hpp>

namespace vrt::graph {

/** @brief A single typed scalar port (input or output). */
struct ScalarPort {
    std::string name;
    ScalarType  type;
};

/** @brief A single typed buffer port (input or output). */
struct BufferPort {
    std::string name;
    BufferType  type;
};

/**
 * @brief A read-write buffer port pair.
 *
 * From the graph's perspective an inout buffer "consumes" one GraphBuffer token
 * and "produces" a new one (even though physically it may be the same
 * allocation).  The in/out port names are distinct so that the detail::PortBindings can bind
 * each side independently.
 */
struct RWBufferPort {
    BufferPort in;   ///< Consumed (input) side
    BufferPort out;  ///< Produced (output) side
};

/**
 * @brief Abstract I/O type signature of a kernel.
 *
 * All fields are optional; leave a vector empty if the kernel has no ports
 * of that category.
 */
struct IOTypeMap {
    std::vector<ScalarPort>   inputScalars;   ///< Read-only scalar arguments
    std::vector<ScalarPort>   outputScalars;  ///< Written scalar results
    std::vector<BufferPort>   inputs;         ///< Read-only buffer arguments
    std::vector<BufferPort>   outputs;        ///< Write-only buffer results
    std::vector<RWBufferPort> inouts;         ///< In-place read-write buffers

    // --- Fluent typed builders -------------------------------------------
    //
    // Declare a kernel's signature once, types spelled out explicitly:
    //   IOTypeMap{}.scalarIn<uint64_t>("n").in<int32_t>("in").out<int32_t>("out");
    // Each returns *this for chaining.

    template <class T>
    IOTypeMap& scalarIn(std::string name) {
        inputScalars.push_back({std::move(name), typeToScalarType<T>()});
        return *this;
    }

    template <class T>
    IOTypeMap& scalarOut(std::string name) {
        outputScalars.push_back({std::move(name), typeToScalarType<T>()});
        return *this;
    }

    template <class T>
    IOTypeMap& in(std::string name) {
        inputs.push_back({std::move(name), typeToBufferType<T>()});
        return *this;
    }

    template <class T>
    IOTypeMap& out(std::string name) {
        outputs.push_back({std::move(name), typeToBufferType<T>()});
        return *this;
    }

    /**
     * @brief Declare a single in-place read-write port; the in and out sides
     *        share the same port name.
     */
    template <class T>
    IOTypeMap& inout(std::string name) {
        inouts.push_back(RWBufferPort{BufferPort{name, typeToBufferType<T>()},
                                      BufferPort{name, typeToBufferType<T>()}});
        return *this;
    }
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_NODE_IO_TYPE_MAP_HPP
