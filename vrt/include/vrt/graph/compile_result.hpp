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
 * @file compile_result.hpp
 * @brief Result carrier shared by concrete graph compiler stages.
 */

#ifndef VRT_GRAPH_COMPILE_RESULT_HPP
#define VRT_GRAPH_COMPILE_RESULT_HPP

#include <optional>
#include <utility>

#include <vrt/graph/diagnostics.hpp>

namespace vrt::graph {

template <class T>
struct CompileResult {
    std::optional<T> output;
    Diagnostics      diagnostics;

    bool ok() const {
        return output.has_value() && !diagnostics.hasErrors();
    }

    static CompileResult success(T value, Diagnostics diagnostics = {}) {
        return {std::move(value), std::move(diagnostics)};
    }

    static CompileResult failure(Diagnostics diagnostics) {
        return {std::nullopt, std::move(diagnostics)};
    }
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_COMPILE_RESULT_HPP
