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
 * @file condition.hpp
 * @brief Control-flow predicates and loop trip counts for structured graph regions.
 */

#ifndef VRT_GRAPH_CONTROL_CONDITION_HPP
#define VRT_GRAPH_CONTROL_CONDITION_HPP

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/core/types.hpp>

namespace vrt::graph {

inline bool isIntegerScalarType(ScalarType type) {
    switch (type) {
        case ScalarType::U8:
        case ScalarType::U16:
        case ScalarType::U32:
        case ScalarType::U64:
        case ScalarType::I8:
        case ScalarType::I16:
        case ScalarType::I32:
        case ScalarType::I64:
            return true;
        case ScalarType::F32:
        case ScalarType::F64:
            return false;
    }
    return false;
}

inline bool isSignedIntegerScalarType(ScalarType type) {
    switch (type) {
        case ScalarType::I8:
        case ScalarType::I16:
        case ScalarType::I32:
        case ScalarType::I64:
            return true;
        case ScalarType::U8:
        case ScalarType::U16:
        case ScalarType::U32:
        case ScalarType::U64:
        case ScalarType::F32:
        case ScalarType::F64:
            return false;
    }
    return false;
}

inline bool isFloatingScalarType(ScalarType type) {
    return type == ScalarType::F32 || type == ScalarType::F64;
}

enum class CompareOp {
    AlwaysTrue,
    AlwaysFalse,
    LT,
    LE,
    EQ,
    GT,
    GE,
    NE,
    EQE,
    NEE,
};

class ConditionOperand {
   public:
    enum class Kind { Scalar, Constant };

    static ConditionOperand scalar(ScalarType type, std::string name, uint64_t scopeId = 0) {
        if (name.empty()) {
            throw std::invalid_argument("ConditionOperand::scalar: name must not be empty");
        }
        return ConditionOperand(Kind::Scalar, type, std::move(name), 0, scopeId);
    }

    static ConditionOperand constantFromBits(ScalarType type, uint64_t bits) {
        return ConditionOperand(Kind::Constant, type, "", bits, 0);
    }

    template <class T>
    static ConditionOperand constant(T value) {
        static_assert(std::is_arithmetic_v<T>,
                      "ConditionOperand::constant only supports arithmetic types");
        return constantFromBits(typeToScalarType<T>(), detail::valueToBits(value));
    }

    Kind kind() const { return kind_; }
    ScalarType type() const { return type_; }
    bool isScalar() const { return kind_ == Kind::Scalar; }
    bool isConstant() const { return kind_ == Kind::Constant; }
    const std::string& name() const { return name_; }
    uint64_t constantBits() const { return bits_; }
    uint64_t scopeId() const { return scopeId_; }

   private:
    ConditionOperand(Kind kind, ScalarType type, std::string name, uint64_t bits,
                     uint64_t scopeId)
        : kind_(kind), type_(type), name_(std::move(name)), bits_(bits), scopeId_(scopeId) {}

    Kind        kind_ = Kind::Constant;
    ScalarType  type_ = ScalarType::U64;
    std::string name_;
    uint64_t    bits_ = 0;
    uint64_t    scopeId_ = 0;
};

class Condition {
   public:
    static Condition alwaysTrue() { return Condition(CompareOp::AlwaysTrue); }
    static Condition alwaysFalse() { return Condition(CompareOp::AlwaysFalse); }

    static Condition compare(CompareOp op, ConditionOperand lhs, ConditionOperand rhs) {
        Condition cond(op);
        cond.lhs_ = std::move(lhs);
        cond.rhs_ = std::move(rhs);
        cond.validate();
        return cond;
    }

    static Condition compareWithEpsilon(CompareOp op, ConditionOperand lhs,
                                        ConditionOperand rhs, ConditionOperand epsilon) {
        Condition cond(op);
        cond.lhs_ = std::move(lhs);
        cond.rhs_ = std::move(rhs);
        cond.epsilon_ = std::move(epsilon);
        cond.validate();
        return cond;
    }

    CompareOp op() const { return op_; }
    const std::optional<ConditionOperand>& lhs() const { return lhs_; }
    const std::optional<ConditionOperand>& rhs() const { return rhs_; }
    const std::optional<ConditionOperand>& epsilon() const { return epsilon_; }

    bool isAlways() const {
        return op_ == CompareOp::AlwaysTrue || op_ == CompareOp::AlwaysFalse;
    }

    bool isEpsilonCompare() const {
        return op_ == CompareOp::EQE || op_ == CompareOp::NEE;
    }

    void validate() const {
        if (isAlways()) {
            if (lhs_ || rhs_ || epsilon_) {
                throw std::invalid_argument("Condition: always predicates must not carry operands");
            }
            return;
        }

        if (!lhs_ || !rhs_) {
            throw std::invalid_argument("Condition: comparison predicates require lhs and rhs");
        }
        if (lhs_->type() != rhs_->type()) {
            throw std::invalid_argument("Condition: operand scalar types must match exactly");
        }

        if (isEpsilonCompare()) {
            if (!isFloatingScalarType(lhs_->type())) {
                throw std::invalid_argument("Condition: epsilon comparisons require F32 or F64 operands");
            }
            if (!epsilon_) {
                throw std::invalid_argument("Condition: epsilon comparison requires an epsilon operand");
            }
            if (epsilon_->type() != lhs_->type()) {
                throw std::invalid_argument("Condition: epsilon type must match comparison operands");
            }
            return;
        }

        if (epsilon_) {
            throw std::invalid_argument("Condition: epsilon is only valid for EQE and NEE");
        }
    }

   private:
    explicit Condition(CompareOp op) : op_(op) {}

    CompareOp op_ = CompareOp::AlwaysFalse;
    std::optional<ConditionOperand> lhs_;
    std::optional<ConditionOperand> rhs_;
    std::optional<ConditionOperand> epsilon_;
};

class LoopTripCount {
   public:
    enum class Kind { Scalar };

    static LoopTripCount scalar(ScalarType type, std::string name, uint64_t scopeId = 0) {
        if (!isIntegerScalarType(type)) {
            throw std::invalid_argument("LoopTripCount::scalar: type must be an integer scalar type");
        }
        if (name.empty()) {
            throw std::invalid_argument("LoopTripCount::scalar: name must not be empty");
        }
        return LoopTripCount(type, std::move(name), scopeId);
    }

    static LoopTripCount scalar(const GraphScalar& scalar) {
        return LoopTripCount::scalar(scalar.type(), scalar.varName(), scalar.scopeId());
    }

    Kind kind() const { return Kind::Scalar; }
    ScalarType type() const { return type_; }
    const std::string& name() const { return name_; }
    uint64_t scopeId() const { return scopeId_; }

   private:
    LoopTripCount(ScalarType type, std::string name, uint64_t scopeId)
        : type_(type), name_(std::move(name)), scopeId_(scopeId) {
        if (!isIntegerScalarType(type_)) {
            throw std::invalid_argument("LoopTripCount: type must be an integer scalar type");
        }
    }

    ScalarType  type_ = ScalarType::U64;
    std::string name_;
    uint64_t    scopeId_ = 0;
};

// ---------------------------------------------------------------------------
// Fluent condition construction from scalar tokens
// ---------------------------------------------------------------------------
//
// Lets authors write `graph.addConditional({.condition = (parity == 0), ...})`.
// The named scalar becomes the lhs operand; the literal is coerced to the
// scalar's element type so Condition::validate()'s exact-type rule is met.

inline ConditionOperand conditionOperandOf(const GraphScalar& scalar) {
    return ConditionOperand::scalar(scalar.type(), scalar.varName(), scalar.scopeId());
}

template <class T>
ConditionOperand conditionConstantLike(const GraphScalar& scalar, T value) {
    static_assert(std::is_arithmetic_v<T>,
                  "condition literal must be an arithmetic type");
    uint64_t bits = 0;
    if (isFloatingScalarType(scalar.type())) {
        if (scalar.type() == ScalarType::F32) {
            float f = static_cast<float>(value);
            std::memcpy(&bits, &f, sizeof(f));
        } else {
            double d = static_cast<double>(value);
            std::memcpy(&bits, &d, sizeof(d));
        }
    } else {
        uint64_t u = static_cast<uint64_t>(value);
        std::memcpy(&bits, &u, sizeof(u));
    }
    return ConditionOperand::constantFromBits(scalar.type(), bits);
}

#define VRT_GRAPH_DEFINE_SCALAR_COMPARE(opSymbol, compareOp)                       \
    template <class T,                                                            \
              std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>                 \
    inline Condition operator opSymbol(const GraphScalar& lhs, T rhs) {           \
        return Condition::compare(CompareOp::compareOp, conditionOperandOf(lhs),  \
                                  conditionConstantLike(lhs, rhs));               \
    }                                                                             \
    inline Condition operator opSymbol(const GraphScalar& lhs,                    \
                                       const GraphScalar& rhs) {                  \
        return Condition::compare(CompareOp::compareOp, conditionOperandOf(lhs),  \
                                  conditionOperandOf(rhs));                       \
    }

VRT_GRAPH_DEFINE_SCALAR_COMPARE(==, EQ)
VRT_GRAPH_DEFINE_SCALAR_COMPARE(!=, NE)
VRT_GRAPH_DEFINE_SCALAR_COMPARE(<, LT)
VRT_GRAPH_DEFINE_SCALAR_COMPARE(<=, LE)
VRT_GRAPH_DEFINE_SCALAR_COMPARE(>, GT)
VRT_GRAPH_DEFINE_SCALAR_COMPARE(>=, GE)

#undef VRT_GRAPH_DEFINE_SCALAR_COMPARE

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CONTROL_CONDITION_HPP