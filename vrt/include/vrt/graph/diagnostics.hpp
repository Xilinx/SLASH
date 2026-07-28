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
 * @file diagnostics.hpp
 * @brief Structured graph compiler diagnostics and public compile exception.
 */

#ifndef VRT_GRAPH_DIAGNOSTICS_HPP
#define VRT_GRAPH_DIAGNOSTICS_HPP

#include <algorithm>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vrt/graph/ids.hpp>

namespace vrt::graph {

enum class Severity {
    Note,
    Warning,
    Error,
};

enum class DiagCode {
    CompilerError,
    EmptyGraph,
    UnknownDevice,
    UnknownDependency,
    MissingTransferRoute,
    Cycle,
    DuplicateProducer,
    TypeMismatch,
    SizeMismatch,
    UnboundPort,
    InvalidScope,
    InvalidBoundary,
    InvalidControlResult,
    AmbiguousPlacement,
    UnsupportedOperation,
    UnsupportedControl,
    IncompatibleMemoryPlacement,
    UnsupportedNestedCopy,
    ImageSafetyViolation,
    ResourceExhausted,
    InternalInvariant,
};

struct DiagnosticLocation {
    std::optional<RegionId>   region;
    std::optional<NodeId>     node;
    std::optional<std::string> authoredId;
    std::optional<std::string> port;
    std::optional<std::string> backend;
};

struct Diagnostic {
    DiagCode                       code = DiagCode::InternalInvariant;
    Severity                       severity = Severity::Error;
    std::string                    message;
    std::optional<DiagnosticLocation> location;
    std::optional<std::string>     suggestion;
};

class Diagnostics {
   public:
    void add(Diagnostic diagnostic) {
        entries_.push_back(std::move(diagnostic));
    }

    void error(DiagCode code, std::string message,
               std::optional<DiagnosticLocation> location = std::nullopt,
               std::optional<std::string> suggestion = std::nullopt) {
        add(Diagnostic{code, Severity::Error, std::move(message),
                       std::move(location), std::move(suggestion)});
    }

    void warning(DiagCode code, std::string message,
                 std::optional<DiagnosticLocation> location = std::nullopt,
                 std::optional<std::string> suggestion = std::nullopt) {
        add(Diagnostic{code, Severity::Warning, std::move(message),
                       std::move(location), std::move(suggestion)});
    }

    void append(Diagnostics other) {
        entries_.insert(entries_.end(),
                        std::make_move_iterator(other.entries_.begin()),
                        std::make_move_iterator(other.entries_.end()));
    }

    bool empty() const { return entries_.empty(); }

    bool hasErrors() const {
        return std::any_of(entries_.begin(), entries_.end(),
                           [](const Diagnostic& diagnostic) {
                               return diagnostic.severity == Severity::Error;
                           });
    }

    const Diagnostic* firstError() const {
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [](const Diagnostic& diagnostic) {
                                   return diagnostic.severity == Severity::Error;
                               });
        return it == entries_.end() ? nullptr : &*it;
    }

    const std::vector<Diagnostic>& entries() const { return entries_; }

   private:
    std::vector<Diagnostic> entries_;
};

class GraphCompileError : public std::runtime_error {
   public:
    explicit GraphCompileError(Diagnostics diagnostics,
                               std::string preservedMessage = {})
        : std::runtime_error(messageFor(diagnostics, preservedMessage)),
          diagnostics_(std::move(diagnostics)) {}

    const Diagnostics& diagnostics() const { return diagnostics_; }

   private:
    static std::string messageFor(const Diagnostics& diagnostics,
                                  const std::string& preservedMessage) {
        if (!preservedMessage.empty()) return preservedMessage;
        if (const Diagnostic* error = diagnostics.firstError()) {
            return error->message;
        }
        return "Graph compilation failed";
    }

    Diagnostics diagnostics_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_DIAGNOSTICS_HPP
