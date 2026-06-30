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
 * @file graph_scalar.hpp
 * @brief GraphScalar — a typed reference to a scalar value slot.
 *
 * Scalar values live in the graph / compiled-graph scalar store keyed by
 * scope and name. Constants are represented by scalar slots with initial
 * values, not by a separate token kind.
 */

#ifndef VRT_GRAPH_CORE_GRAPH_SCALAR_HPP
#define VRT_GRAPH_CORE_GRAPH_SCALAR_HPP

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <vrt/graph/core/types.hpp>

namespace vrt::graph {

inline std::string scopedScalarKey(uint64_t scopeId, const std::string& name) {
    return "scope:" + std::to_string(scopeId) + ":" + name;
}

namespace detail {

/**
 * @brief Reinterpret an arithmetic value as raw uint64_t bits.
 *
 * Bit patterns shorter than 64 bits are zero-extended.
 */
template <class T>
inline uint64_t valueToBits(T value) {
    static_assert(std::is_arithmetic_v<T>, "valueToBits: T must be arithmetic");
    static_assert(sizeof(T) <= sizeof(uint64_t), "valueToBits: T larger than uint64_t");
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(T));
    return bits;
}

}  // namespace detail

class GraphScalar {
   public:
    /**
     * @brief Creates a reference to a named scalar value slot.
     */
    static GraphScalar ref(ScalarType type, std::string varName, uint64_t scopeId = 0) {
        if (varName.empty()) {
            throw std::invalid_argument("GraphScalar::ref: varName must not be empty");
        }
        return GraphScalar(type, std::move(varName), scopeId);
    }

    /**
     * @brief Returns the element type.
     */
    ScalarType type() const { return type_; }

    /**
     * @brief Returns the scalar value slot name.
     */
    const std::string& varName() const { return varName_; }

    /**
     * @brief Returns the graph-region namespace that owns this scalar.
     *
     */
    uint64_t scopeId() const { return scopeId_; }

   private:
    GraphScalar(ScalarType type, std::string varName, uint64_t scopeId)
        : type_(type), varName_(std::move(varName)), scopeId_(scopeId) {}

    ScalarType  type_;
    std::string varName_;
    uint64_t    scopeId_ = 0;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CORE_GRAPH_SCALAR_HPP
