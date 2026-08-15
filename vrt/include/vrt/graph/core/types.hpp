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
 * @file types.hpp
 * @brief Shared enums for the VRT graph API.
 */

#ifndef VRT_GRAPH_CORE_TYPES_HPP
#define VRT_GRAPH_CORE_TYPES_HPP

#include <cstddef>
#include <type_traits>

namespace vrt::graph {

namespace detail {
template <class>
struct always_false : std::false_type {};
}  // namespace detail

/**
 * @brief Element type of a scalar kernel argument.
 */
enum class ScalarType {
    U8, U16, U32, U64,
    I8, I16, I32, I64,
    F32, F64,
};

template<class T>
constexpr ScalarType typeToScalarType() {
    if constexpr (std::is_same_v<T, uint8_t>)  return ScalarType::U8;
    else if constexpr (std::is_same_v<T, uint16_t>) return ScalarType::U16;
    else if constexpr (std::is_same_v<T, uint32_t>) return ScalarType::U32;
    else if constexpr (std::is_same_v<T, uint64_t>) return ScalarType::U64;
    else if constexpr (std::is_same_v<T, int8_t>)  return ScalarType::I8;
    else if constexpr (std::is_same_v<T, int16_t>) return ScalarType::I16;
    else if constexpr (std::is_same_v<T, int32_t>) return ScalarType::I32;
    else if constexpr (std::is_same_v<T, int64_t>) return ScalarType::I64;
    else if constexpr (std::is_same_v<T, float>)   return ScalarType::F32;
    else if constexpr (std::is_same_v<T, double>)  return ScalarType::F64;
    else static_assert(detail::always_false<T>::value, "Unsupported type for GraphScalar");
}

/**
 * @brief Element type of a buffer kernel argument.
 */
enum class BufferType {
    U8, U16, U32, U64,
    I8, I16, I32, I64,
    F32, F64,
};

template<class T>
constexpr BufferType typeToBufferType() {
    if constexpr (std::is_same_v<T, uint8_t>)  return BufferType::U8;
    else if constexpr (std::is_same_v<T, uint16_t>) return BufferType::U16;
    else if constexpr (std::is_same_v<T, uint32_t>) return BufferType::U32;
    else if constexpr (std::is_same_v<T, uint64_t>) return BufferType::U64;
    else if constexpr (std::is_same_v<T, int8_t>)  return BufferType::I8;
    else if constexpr (std::is_same_v<T, int16_t>) return BufferType::I16;
    else if constexpr (std::is_same_v<T, int32_t>) return BufferType::I32;
    else if constexpr (std::is_same_v<T, int64_t>) return BufferType::I64;
    else if constexpr (std::is_same_v<T, float>)   return BufferType::F32;
    else if constexpr (std::is_same_v<T, double>)  return BufferType::F64;
    else static_assert(detail::always_false<T>::value, "Unsupported type for GraphBuffer");
}

/**
 * @brief Size in bytes of one element of a BufferType.
 */
constexpr std::size_t bufferElementSize(BufferType type) {
    switch (type) {
        case BufferType::U8:  case BufferType::I8:  return 1;
        case BufferType::U16: case BufferType::I16: return 2;
        case BufferType::U32: case BufferType::I32: case BufferType::F32: return 4;
        case BufferType::U64: case BufferType::I64: case BufferType::F64: return 8;
    }
    return 1;
}

/**
 * @brief Class of device that a kernel runs on.
 */
enum class DeviceType {
    CPU,
    GPU,
    FPGA,

    // For testing only.
    MOCK_CPU,
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CORE_TYPES_HPP
