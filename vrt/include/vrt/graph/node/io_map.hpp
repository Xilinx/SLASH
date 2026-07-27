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
 * @file io_map.hpp
 * @brief IOMap — binds concrete graph tokens to a kernel node's typed ports.
 *
 * IOMap is constructed by the user before calling Graph::addNode().  It maps
 * each port name declared in the kernel's IOTypeMap to a concrete value:
 *  - inputScalars   → GraphScalar (constant or global variable)
 *  - outputScalars  → GraphScalar (global variable name, written by the kernel)
 *  - inputs         → an existing GraphBuffer token (produced earlier in the graph)
 *  - outputs        → a new GraphBuffer token (captured by the caller)
 *  - inouts         → an existing input token + a new output token
 *
 * All bind*() methods return *this for method chaining.
 */

#ifndef VRT_GRAPH_NODE_IO_MAP_HPP
#define VRT_GRAPH_NODE_IO_MAP_HPP

#include <atomic>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/core/types.hpp>

namespace vrt::graph {

class IOMap {
   public:
    /**
     * @brief Bind an input scalar port to a GraphScalar.
     */
    IOMap& bindInputScalar(std::string portName, GraphScalar scalar) {
        if (inputScalars_.count(portName)) {
            throw std::invalid_argument(
                "bindInputScalar: port '" + portName + "' already bound");
        }
        inputScalars_.emplace(std::move(portName), std::move(scalar));
        return *this;
    }

    /**
     * @brief Bind an output scalar port to a writable GraphScalar.
     */
    IOMap& bindOutputScalar(std::string portName, GraphScalar scalar) {
        if (outputScalars_.count(portName)) {
            throw std::invalid_argument(
                "bindOutputScalar: port '" + portName + "' already bound");
        }
        outputScalars_.emplace(std::move(portName), std::move(scalar));
        return *this;
    }

    /**
     * @brief Bind an input buffer port to an existing GraphBuffer token.
     */
    IOMap& bindInput(std::string portName, GraphBuffer buf) {
        if (!buf.valid()) {
            throw std::invalid_argument("bindInput: invalid (default-constructed) GraphBuffer");
        }
        if (inputs_.count(portName)) {
            throw std::invalid_argument("bindInput: port '" + portName + "' already bound");
        }
        inputs_.emplace(std::move(portName), std::move(buf));
        return *this;
    }

    /**
     * @brief Bind an output buffer port; creates and returns a new GraphBuffer token.
     *
     * The caller must capture @p out before using it as an input to subsequent nodes.
     * The token's name is auto-generated; use the returned IOMap& for chaining.
     *
     * @param portName  Port name matching an outputs entry in the IOTypeMap.
     * @param type      Element type of the produced buffer.
     * @param out       Receives the newly created GraphBuffer token.
     * @param scopeId   Graph-region namespace for the produced token.
     */
    IOMap& bindOutput(std::string portName, BufferType type, GraphBuffer& out,
                      uint64_t scopeId = 0) {
        if (outputs_.count(portName)) {
            throw std::invalid_argument("bindOutput: port '" + portName + "' already bound");
        }
        std::string tokenName = nextTokenName(portName);
        std::optional<GraphScalar> size;
        if (!inputs_.empty()) {
            size = inputs_.begin()->second.maybeSizeScalar();
        }
        out = GraphBuffer::make(type, tokenName, scopeId, std::move(size));
        outputs_.emplace(std::move(portName), out);
        return *this;
    }

    /**
     * @brief Bind an output buffer port with an explicit symbolic size.
     */
    IOMap& bindOutput(std::string portName, BufferType type, GraphBuffer& out,
                      GraphScalar size, uint64_t scopeId = 0) {
        if (outputs_.count(portName)) {
            throw std::invalid_argument("bindOutput: port '" + portName + "' already bound");
        }
        std::string tokenName = nextTokenName(portName);
        out = GraphBuffer::make(type, tokenName, scopeId, std::move(size));
        outputs_.emplace(std::move(portName), out);
        return *this;
    }

    /**
     * @brief Bind an output buffer port to a pre-declared token.
     *
     * Unlike bindOutput(), this does not mint a new token: the caller
     * supplies the already-declared output token (e.g. from Graph::buffer()).
     * This is the binding path used by the struct-literal authoring API where
     * outputs bind to named tokens declared up front.
     */
    IOMap& bindExistingOutput(std::string portName, GraphBuffer out) {
        if (!out.valid()) {
            throw std::invalid_argument(
                "bindExistingOutput: invalid (default-constructed) GraphBuffer");
        }
        if (outputs_.count(portName)) {
            throw std::invalid_argument(
                "bindExistingOutput: port '" + portName + "' already bound");
        }
        outputs_.emplace(std::move(portName), std::move(out));
        return *this;
    }

    /**
     * @brief Bind an in-place RW buffer port pair to pre-declared tokens.
     *
     * Consumes @p in and produces @p out, both supplied by the caller (no
     * minting). @p out must match @p in's element type.
     */
    IOMap& bindExistingInout(std::string inPortName, std::string outPortName,
                             GraphBuffer in, GraphBuffer out) {
        if (!in.valid() || !out.valid()) {
            throw std::invalid_argument(
                "bindExistingInout: invalid (default-constructed) GraphBuffer");
        }
        if (in.type() != out.type()) {
            throw std::invalid_argument(
                "bindExistingInout: in/out element types differ");
        }
        for (const auto& existing : inouts_) {
            if (existing.inPort == inPortName) {
                throw std::invalid_argument(
                    "bindExistingInout: input port '" + inPortName + "' already bound");
            }
            if (existing.outPort == outPortName) {
                throw std::invalid_argument(
                    "bindExistingInout: output port '" + outPortName + "' already bound");
            }
        }
        inouts_.emplace_back(InoutBinding{std::move(inPortName), std::move(outPortName),
                                          std::move(in), std::move(out)});
        return *this;
    }

    /**
     * @brief Bind an RW buffer port pair: consume @p in, produce a new token into @p out.
     *
     * @p inPortName and @p outPortName must match the in/out sides of the same
     * RWBufferPort entry in the kernel's IOTypeMap.  The output token inherits
     * the input token's element type (RW buffers are in-place by definition).
     *
     * @param inPortName   Port name of the consumed (input) side.
     * @param outPortName  Port name of the produced (output) side.
     * @param in           Existing token to consume.
     * @param out          Receives the newly created output token.
     * @param scopeId      Graph-region namespace for the produced token.
     */
    IOMap& bindInout(std::string inPortName, std::string outPortName,
                     GraphBuffer in, GraphBuffer& out, uint64_t scopeId = 0) {
        if (!in.valid()) {
            throw std::invalid_argument("bindInout: invalid (default-constructed) input GraphBuffer");
        }
        for (const auto& existing : inouts_) {
            if (existing.inPort == inPortName) {
                throw std::invalid_argument("bindInout: input port '" + inPortName + "' already bound");
            }
            if (existing.outPort == outPortName) {
                throw std::invalid_argument("bindInout: output port '" + outPortName + "' already bound");
            }
        }
        std::string tokenName = nextTokenName(outPortName);
        out = GraphBuffer::make(in.type(), tokenName, scopeId,
                                in.maybeSizeScalar());
        inouts_.emplace_back(InoutBinding{std::move(inPortName), std::move(outPortName),
                                          std::move(in), out});
        return *this;
    }

    // --- Accessors used by GraphCompiler (not part of the public user API) ---

    const std::map<std::string, GraphScalar>& inputScalars() const { return inputScalars_; }
    const std::map<std::string, GraphScalar>& outputScalars() const { return outputScalars_; }
    std::map<std::string, GraphScalar> scalarBindings() const {
        std::map<std::string, GraphScalar> merged = inputScalars_;
        merged.insert(outputScalars_.begin(), outputScalars_.end());
        return merged;
    }
    const std::map<std::string, GraphBuffer>& inputs() const { return inputs_; }
    const std::map<std::string, GraphBuffer>& outputs() const { return outputs_; }

    void rebindInputForCompiler(const std::string& portName, GraphBuffer buf) {
        auto it = inputs_.find(portName);
        if (it == inputs_.end()) {
            throw std::invalid_argument(
                "rebindInputForCompiler: input port '" + portName + "' is not bound");
        }
        if (!buf.valid()) {
            throw std::invalid_argument(
                "rebindInputForCompiler: invalid (default-constructed) GraphBuffer");
        }
        it->second = std::move(buf);
    }

    struct InoutBinding {
        std::string inPort;
        std::string outPort;
        GraphBuffer in;
        GraphBuffer out;
    };
    const std::vector<InoutBinding>& inouts() const { return inouts_; }

   private:
    std::string nextTokenName(const std::string& portName) {
        static std::atomic<uint32_t> globalCounter{0};
        return portName + "_buf_" + std::to_string(globalCounter++);
    }

    std::map<std::string, GraphScalar> inputScalars_;
    std::map<std::string, GraphScalar> outputScalars_;
    std::map<std::string, GraphBuffer> inputs_;
    std::map<std::string, GraphBuffer> outputs_;
    std::vector<InoutBinding>          inouts_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_NODE_IO_MAP_HPP
