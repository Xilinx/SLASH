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
 * @file calls.hpp
 * @brief Struct-literal authoring vocabulary for the vrt::graph API.
 *
 * Defines the lightweight value types that make up a node authored as a C++20
 * designated-initializer literal: kernel handles (`KernelHandle`), port
 * bindings (`BufferArg`, `ScalarArg`, `InoutArg`), node references for
 * side-effect ordering (`GraphNode`), and the per-node spec structs
 * (`KernelCallSpec`, `ReprogramCallSpec`, `LoopBuildSpec`, `ConditionalBuildSpec`).
 *
 * None of these pull in device or firmware headers; the heavyweight FPGA
 * bring-up handles live in authoring/fpga.hpp.
 */

#ifndef VRT_GRAPH_AUTHORING_CALLS_HPP
#define VRT_GRAPH_AUTHORING_CALLS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <vrt/graph/control/condition.hpp>
#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/node/io_type_map.hpp>

namespace vrt::graph {

/**
 * @brief A callable kernel reference named in a call's `.kernel` field.
 *
 * Produced by `Graph::cpu().add<T>()` / `elementwise<T>()` (CPU) or by
 * `image.kernel(...).scalarIn().in().out()` (FPGA). Carries the kernel
 * identity, its typed I/O signature, and the required device it must run on.
 */
struct KernelHandle {
    std::string                name;
    DeviceType                 type = DeviceType::CPU;
    std::optional<std::string> image;
    IOTypeMap                  ioType;
    std::string                device;  ///< required target device ("cpu", "fpga:0", ...)
};

/**
 * @brief Opaque reference to an authored node, used only for `.after`.
 */
struct GraphNode {
    std::string id;
};

/**
 * @brief Lightweight, device-header-free reference to a loadable image.
 *
 * `FpgaImageHandle` (authoring/fpga.hpp) converts to this so reprogram specs
 * can be authored without the region builder depending on FPGA/firmware
 * headers.
 */
struct ImageRef {
    std::string imageId;
    std::string pdiPath;
    std::string device;
};

/** @brief An explicit reprogram (PDI_LOAD) node authored as a struct literal. */
struct ReprogramCallSpec {
    ImageRef               image;
    std::vector<GraphNode> after;
};

/** @brief Binds a buffer port name to a token. Brace-init: `{"in", token}`. */
struct BufferArg {
    std::string port;
    GraphBuffer buffer;
};

/** @brief Binds an in-place port to a consumed token and a produced token. */
struct InoutArg {
    std::string port;
    GraphBuffer in;
    GraphBuffer out;
};

/**
 * @brief Binds a scalar port to a scalar token.
 */
struct ScalarArg {
    std::string port;
    GraphScalar scalar;

    ScalarArg(std::string portName, GraphScalar value)
        : port(std::move(portName)), scalar(std::move(value)) {}
};

/**
 * @brief A loop trip count scalar reference.
 */
struct TripCount {
    LoopTripCount value;

    TripCount(const GraphScalar& scalar)
        : value(LoopTripCount::scalar(scalar)) {}
};

/**
 * @brief One node authored as a struct literal.
 *
 * Data dependencies are inferred from token use in `.inputs`/`.outputs`/
 * `.inputScalars`/`.outputScalars`/`.inouts`; `.after` carries only side-effect ordering edges
 * (reprogram gating / drains).
 */
struct KernelCallSpec {
    KernelHandle           kernel;
    std::vector<ScalarArg> inputScalars;
    std::vector<BufferArg> inputs;
    std::vector<BufferArg> outputs;
    std::vector<ScalarArg> outputScalars;
    std::vector<InoutArg>  inouts;
    std::vector<GraphNode> after;
};

/**
 * @brief A fixed- or data-dependent-count loop authored as a region.
 *
 * A port present in both `.inputs` and `.outputs` is loop-carried: each
 * iteration reads `loop.input(port)` and writes `loop.output(port)`, and the
 * final value is published to the `.outputs` token.
 */
struct LoopBuildSpec {
    TripCount              count;
    std::vector<BufferArg> inputs;
    std::vector<BufferArg> outputs;
    std::vector<GraphNode> after;
};

/**
 * @brief A two-way conditional authored as two branch regions.
 *
 * Both branches must produce every `.outputs` port; the predicate reads a
 * graph scalar (e.g. `(parity == 0)`).
 */
struct ConditionalBuildSpec {
    /// Required: the branch predicate (e.g. `(parity == 0)`). Authoring a
    /// conditional without a condition throws rather than silently defaulting
    /// to a never-taken branch.
    std::optional<Condition> condition;
    std::vector<BufferArg>   inputs;
    std::vector<BufferArg>   outputs;
    std::vector<GraphNode>   after;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_AUTHORING_CALLS_HPP
