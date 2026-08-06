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
 * @file graph_buffer.hpp
 * @brief GraphBuffer — typed, opaque buffer token used in graph construction.
 *
 * A GraphBuffer is a first-class value in the graph: a (scope, name,
 * element-type, size-scalar) tuple that the compiler resolves to a concrete,
 * device-allocated buffer in a compiled execution.
 *
 * Tokens are minted by the compiler-private factory:
 *   ::vrt::graph::detail::makeGraphBuffer(BufferType, std::string, scopeId)
 *
 * Public typed authoring methods invoke this factory indirectly; applications
 * cannot forge GraphBuffer tokens.
 *
 * A default-constructed GraphBuffer is invalid (valid() == false).
 */

#ifndef VRT_GRAPH_CORE_GRAPH_BUFFER_HPP
#define VRT_GRAPH_CORE_GRAPH_BUFFER_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/core/types.hpp>

namespace vrt::graph {

inline std::string scopedBufferKey(uint64_t scopeId, const std::string& name) {
    return "scope:" + std::to_string(scopeId) + ":" + name;
}

class GraphBuffer;

namespace detail {
GraphBuffer makeGraphBuffer(
    BufferType type, std::string name, std::uint64_t scopeId = 0,
    std::optional<GraphScalar> size = std::nullopt,
    std::uint64_t graphId = 0);
}

class GraphBuffer {
   public:
    GraphBuffer() = default;

    /**
     * @brief Returns the logical name of this buffer (unique within its Graph).
     */
    const std::string& name() const { return name_; }

    /**
     * @brief Returns the graph-region namespace that owns this token.
     */
    uint64_t scopeId() const { return scopeId_; }

    /**
     * @brief Identity of the Graph that minted this token.
     */
    uint64_t graphId() const { return graphId_; }

    /**
     * @brief Returns the element type of this buffer.
     *
     * For a default-constructed (invalid) token the returned value is
     * unspecified and should not be relied upon.
     */
    BufferType type() const { return type_; }

    /**
     * @brief Returns true when this token carries a symbolic size scalar.
     */
    bool hasSizeScalar() const { return size_.has_value(); }

    /**
     * @brief Symbolic element count used for allocation.
     *
     * Throws if this token is intentionally unsized. Unsized tokens are only
     * valid when the compiler can prove they are pure aliases of sized tokens.
     */
    const GraphScalar& sizeScalar() const {
        if (!size_) {
            throw std::runtime_error(
                "GraphBuffer::sizeScalar: buffer '" + name_ +
                "' has no size scalar");
        }
        return *size_;
    }

    /**
     * @brief Optional symbolic element count used for allocation.
     */
    const std::optional<GraphScalar>& maybeSizeScalar() const { return size_; }

    /**
     * @brief Returns false for default-constructed (unbound) tokens.
     */
    bool valid() const { return !name_.empty(); }

   private:
    friend GraphBuffer detail::makeGraphBuffer(
        BufferType, std::string, std::uint64_t,
        std::optional<GraphScalar>, std::uint64_t);

    GraphBuffer(BufferType type, std::string name, uint64_t scopeId,
                std::optional<GraphScalar> size, uint64_t graphId)
        : type_(type),
          name_(std::move(name)),
          scopeId_(scopeId),
          size_(std::move(size)),
          graphId_(graphId) {}

    BufferType  type_ = BufferType::U8;  // placeholder for default-constructed tokens
    std::string name_;
    uint64_t    scopeId_ = 0;
    std::optional<GraphScalar> size_;
    uint64_t    graphId_ = 0;
};

namespace detail {

inline GraphBuffer makeGraphBuffer(
    BufferType type, std::string name, std::uint64_t scopeId,
    std::optional<GraphScalar> size, std::uint64_t graphId) {
    if (name.empty()) {
        throw std::invalid_argument(
            "makeGraphBuffer: name must not be empty");
    }
    return GraphBuffer(
        type, std::move(name), scopeId, std::move(size), graphId);
}

}  // namespace detail

inline std::size_t resolvedBufferElements(
    const GraphBuffer& buffer,
    const std::shared_ptr<std::map<std::string, std::uint64_t>>& scalarValues,
    const char* diagnostic) {
    if (!buffer.valid()) {
        throw std::runtime_error(
            std::string(diagnostic) + ": invalid GraphBuffer");
    }
    if (!buffer.hasSizeScalar()) {
        throw std::runtime_error(
            std::string(diagnostic) + ": buffer '" + buffer.name() +
            "' has no size scalar");
    }
    if (!scalarValues) {
        throw std::runtime_error(
            std::string(diagnostic) + ": no scalar map is available");
    }
    const GraphScalar& size = buffer.sizeScalar();
    const std::string key = scopedScalarKey(size.scopeId(), size.varName());
    auto it = scalarValues->find(key);
    if (it == scalarValues->end()) {
        throw std::runtime_error(
            std::string(diagnostic) + ": size scalar '" + size.varName() +
            "' is not set");
    }
    return static_cast<std::size_t>(it->second);
}

inline std::size_t resolvedBufferSizeBytes(
    const GraphBuffer& buffer,
    const std::shared_ptr<std::map<std::string, std::uint64_t>>& scalarValues,
    const char* diagnostic) {
    return resolvedBufferElements(buffer, scalarValues, diagnostic) *
           bufferElementSize(buffer.type());
}

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CORE_GRAPH_BUFFER_HPP
