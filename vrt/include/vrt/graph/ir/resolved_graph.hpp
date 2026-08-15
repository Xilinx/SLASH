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
 * @file resolved_graph.hpp
 * @brief Validated graph IR with explicit values and dependencies.
 */

#ifndef VRT_GRAPH_IR_RESOLVED_GRAPH_HPP
#define VRT_GRAPH_IR_RESOLVED_GRAPH_HPP

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <vrt/graph/compile_result.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/ids.hpp>
#include <vrt/graph/ir/authored_graph.hpp>

namespace vrt::graph {

enum class ValueKind {
    Buffer,
    Scalar,
};

struct ValueType {
    ValueKind  kind = ValueKind::Buffer;
    BufferType buffer = BufferType::U8;
    ScalarType scalar = ScalarType::U8;

    static ValueType bufferType(BufferType type) {
        ValueType result;
        result.kind = ValueKind::Buffer;
        result.buffer = type;
        return result;
    }

    static ValueType scalarType(ScalarType type) {
        ValueType result;
        result.kind = ValueKind::Scalar;
        result.scalar = type;
        return result;
    }

    bool operator==(const ValueType& other) const {
        if (kind != other.kind) return false;
        return kind == ValueKind::Buffer ? buffer == other.buffer
                                         : scalar == other.scalar;
    }
};

enum class ValueDefinitionKind {
    GraphInput,
    OperationOutput,
    RegionParameter,
    ControlResult,
};

using ResolvedToken = std::variant<GraphBuffer, GraphScalar>;

struct GraphInputOrigin {
    ResolvedToken token;
};

struct OperationOutputOrigin {
    NodeId        operation;
    PortName      port;
    ResolvedToken token;
};

struct RegionParameterOrigin {
    RegionId      region;
    PortName      port;
    ResolvedToken token;
};

struct ControlResultOrigin {
    NodeId        control;
    PortName      port;
    ResolvedToken token;
};

using ValueOrigin =
    std::variant<GraphInputOrigin, OperationOutputOrigin,
                 RegionParameterOrigin, ControlResultOrigin>;

struct ResolvedValue {
    ValueId                id;
    RegionId               region;
    ValueType              type;
    std::string            sourceName;
    ValueOrigin            origin;
    std::optional<ValueId> size;
    bool                   graphOutput = false;
};

ValueDefinitionKind valueDefinition(const ResolvedValue& value);
std::optional<NodeId> valueProducer(const ResolvedValue& value);
const GraphBuffer* resolvedBufferToken(const ResolvedValue& value);
const GraphScalar* resolvedScalarToken(const ResolvedValue& value);

enum class ValueAccess {
    Input,
    Output,
    InoutInput,
    InoutOutput,
    Condition,
    TripCount,
    BoundarySource,
    BoundaryTarget,
};

struct ResolvedBinding {
    PortName    port;
    PortName    localPort;
    ValueId     value;
    ValueAccess access = ValueAccess::Input;

    ResolvedBinding() = default;
    ResolvedBinding(std::string portValue, ValueId valueId,
                    ValueAccess valueAccess)
        : port(PortName(portValue)),
          localPort(PortName(std::move(portValue))),
          value(valueId),
          access(valueAccess) {}
    ResolvedBinding(PortName portValue, ValueId valueId,
                    ValueAccess valueAccess)
        : port(portValue),
          localPort(std::move(portValue)),
          value(valueId),
          access(valueAccess) {}
    ResolvedBinding(PortName portValue, PortName localPortValue,
                    ValueId valueId,
                    ValueAccess valueAccess)
        : port(std::move(portValue)),
          localPort(std::move(localPortValue)),
          value(valueId),
          access(valueAccess) {}
};

struct ResolvedInoutBinding {
    NodeId   operation;
    PortName port;
    ValueId  input;
    ValueId  output;
};

enum class DependencyReason {
    Data,
    UserOrdering,
    ReprogramDrain,
    ReadersBeforeMutator,
    BoundaryOrdering,
};

struct ResolvedDependency {
    NodeId                 predecessor;
    DependencyReason       reason = DependencyReason::Data;
    std::optional<ValueId> value;

    friend bool operator<(const ResolvedDependency& lhs,
                          const ResolvedDependency& rhs) {
        if (lhs.predecessor != rhs.predecessor) {
            return lhs.predecessor < rhs.predecessor;
        }
        if (lhs.reason != rhs.reason) {
            return lhs.reason < rhs.reason;
        }
        return lhs.value < rhs.value;
    }

    friend bool operator==(const ResolvedDependency& lhs,
                           const ResolvedDependency& rhs) {
        return lhs.predecessor == rhs.predecessor &&
               lhs.reason == rhs.reason && lhs.value == rhs.value;
    }
};

enum class ResolvedOperationKind {
    Kernel,
    Reprogram,
    Boundary,
    Loop,
    Conditional,
};

struct ResolvedOperation {
    NodeId                       id;
    RegionId                     region;
    ResolvedOperationKind        kind = ResolvedOperationKind::Kernel;
    bool                         structural = false;
    std::vector<ResolvedDependency> dependencies;
    std::vector<ResolvedBinding> bindings;
};

enum class ControlArm {
    LoopInitial,
    LoopBackedge,
    ThenBranch,
    ElseBranch,
};

struct ControlIncoming {
    ControlArm arm = ControlArm::LoopInitial;
    RegionId   region;
    ValueId    value;
};

struct ResolvedControlResult {
    NodeId                       control;
    PortName                     port;
    ValueId                      result;
    std::vector<ControlIncoming> incoming;
};

struct BoundaryAlias {
    NodeId   boundary;
    PortName port;
    ValueId  source;
    ValueId  target;
};

class BoundaryAliasTable {
   public:
    explicit BoundaryAliasTable(std::vector<BoundaryAlias> aliases = {});

    ValueId canonical(ValueId value) const;
    std::optional<ValueId> directSource(ValueId target) const;
    const std::vector<BoundaryAlias>& entries() const { return aliases_; }

   private:
    std::vector<BoundaryAlias> aliases_;
    std::map<ValueId, ValueId> sources_;
};

struct ResolvedRegion {
    RegionId                                   id;
    std::optional<RegionId>                    parent;
    std::vector<NodeId>                        topologicalOrder;
    std::vector<ValueId>                       parameters;
    std::vector<ValueId>                       results;
    std::vector<std::shared_ptr<const ResolvedRegion>> children;
};

class ResolvedGraph {
   public:
    ResolvedGraph(std::shared_ptr<const AuthoredGraph> authored,
                  std::shared_ptr<const ResolvedRegion> root,
                  std::map<ValueId, ResolvedValue> values,
                  std::map<NodeId, ResolvedOperation> operations,
                  std::vector<ResolvedInoutBinding> inouts,
                  BoundaryAliasTable aliases,
                  std::vector<ResolvedControlResult> controlResults)
        : data_(std::make_shared<Data>(
              std::move(authored), std::move(root),
              std::move(values), std::move(operations),
              std::move(inouts), std::move(aliases),
              std::move(controlResults))) {}

    const AuthoredGraph& authored() const { return *data_->authored; }
    const ResolvedRegion& root() const { return *data_->root; }
    const std::map<ValueId, ResolvedValue>& values() const {
        return data_->values;
    }
    const std::map<NodeId, ResolvedOperation>& operations() const {
        return data_->operations;
    }
    const std::vector<ResolvedInoutBinding>& inouts() const {
        return data_->inouts;
    }
    const BoundaryAliasTable& aliases() const { return data_->aliases; }
    const std::vector<ResolvedControlResult>& controlResults() const {
        return data_->controlResults;
    }

    const ResolvedValue* findValue(ValueId id) const {
        auto it = data_->values.find(id);
        return it == data_->values.end() ? nullptr : &it->second;
    }

    const ResolvedOperation* findOperation(NodeId id) const {
        auto it = data_->operations.find(id);
        return it == data_->operations.end() ? nullptr : &it->second;
    }

   private:
    struct Data {
        Data(std::shared_ptr<const AuthoredGraph> authoredValue,
             std::shared_ptr<const ResolvedRegion> rootValue,
             std::map<ValueId, ResolvedValue> valuesValue,
             std::map<NodeId, ResolvedOperation> operationsValue,
             std::vector<ResolvedInoutBinding> inoutsValue,
             BoundaryAliasTable aliasesValue,
             std::vector<ResolvedControlResult> controlResultsValue)
            : authored(std::move(authoredValue)),
              root(std::move(rootValue)),
              values(std::move(valuesValue)),
              operations(std::move(operationsValue)),
              inouts(std::move(inoutsValue)),
              aliases(std::move(aliasesValue)),
              controlResults(std::move(controlResultsValue)) {}

        std::shared_ptr<const AuthoredGraph>  authored;
        std::shared_ptr<const ResolvedRegion> root;
        std::map<ValueId, ResolvedValue>      values;
        std::map<NodeId, ResolvedOperation>   operations;
        std::vector<ResolvedInoutBinding>      inouts;
        BoundaryAliasTable                    aliases;
        std::vector<ResolvedControlResult>    controlResults;
    };

    std::shared_ptr<const Data> data_;
};

CompileResult<ResolvedGraph> resolveGraph(const AuthoredGraph& authored);

namespace detail {
CompileResult<ResolvedGraph> resolveValidatedGraph(
    const AuthoredGraph& authored);
}

}  // namespace vrt::graph

#endif  // VRT_GRAPH_IR_RESOLVED_GRAPH_HPP
