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
 * @file control_lowering.hpp
 * @brief Helpers for lowering structured control flow (loops/conditionals)
 *        onto the RP1 command processor's native LOOP/COND/RERUN opcodes.
 *
 * RP1 evaluates a loop/conditional predicate as `compare(signal, op, value)`
 * where @c signal is a 32-bit value in the signal array and @c op is one of a
 * small set of integer operators (@ref rp1_condop_t).  The host @ref Condition
 * type is richer (signed/float types, LE/GT, epsilon compares, scalar-vs-scalar)
 * so only a subset can be evaluated by RP1 directly.  These helpers decide
 * whether a condition is RP1-evaluable and, when it is, normalise it into the
 * `(op, value)` form RP1 expects, treating the single scalar operand as the
 * signal source and the constant operand as the comparison value.
 *
 * Conditions that are not RP1-evaluable (float/epsilon, scalar-vs-scalar,
 * 64-bit-wide values, signed ordering) must stay on the host-owned CPU control
 * path; callers use @ref isRp1EvaluableCondition to gate placement.
 */

#ifndef VRT_GRAPH_DEVICE_FPGA_CONTROL_LOWERING_HPP
#define VRT_GRAPH_DEVICE_FPGA_CONTROL_LOWERING_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <slash/uapi/rp1_protocol.h>

#include <vrt/graph/control/condition.hpp>

namespace vrt::graph::fpga {

/**
 * @brief Normalised RP1 comparison: `compare(signal[slot], op, value)`.
 *
 * @c scalarName / @c scalarScopeId / @c scalarType identify the scalar operand
 * whose 32-bit value RP1 reads from a signal slot (populated by a SCALAR_READ
 * of the producing kernel's output register, or by the host).  The constant
 * operand is folded into @c value.
 */
struct Rp1Compare {
    rp1_condop_t  op            = RP1_COP_EQ;
    std::uint32_t value         = 0;
    std::string   scalarName;
    std::uint64_t scalarScopeId = 0;
    ScalarType    scalarType    = ScalarType::U32;
};

/**
 * @brief True iff @p cond can be evaluated natively by RP1.
 *
 * Requirements: exactly one scalar operand and one constant operand; the scalar
 * is an integer type no wider than 32 bits; the operator is one of
 * EQ/NE/LT/LE/GT/GE; ordering operators (LT/LE/GT/GE) require an unsigned scalar
 * type; the constant fits the 32-bit signal slot (and any LE/GT +1 rewrite does
 * not overflow).  Always-true/false and epsilon/float comparisons are not
 * RP1-evaluable.
 */
bool isRp1EvaluableCondition(const Condition& cond);

/**
 * @brief Map @p cond so that `compare(signalValue, result.op, result.value)`
 *        equals the truth of @p cond, with the scalar operand as the signal.
 *
 * @throws std::logic_error if `!isRp1EvaluableCondition(cond)`.
 */
Rp1Compare mapRp1Condition(const Condition& cond);

/**
 * @brief Logical inverse operator: `compare(s, invert(op), v) == !compare(s, op, v)`.
 *
 * Defined for EQ/NE/LT/GE/AND_NZ/AND_Z (the closed set produced by
 * @ref mapRp1Condition; LE/GT are always rewritten to LT/GE before this).
 */
rp1_condop_t invertRp1Op(rp1_condop_t op);

/**
 * @brief A never-true RP1 compare: `compare(s, AND_NZ, 0)` is always false.
 *
 * Used as the condition for a pure fixed-count loop, where @c max_iterations
 * alone governs termination and the data-dependent predicate must never fire.
 */
constexpr rp1_condop_t  kNeverOp    = RP1_COP_AND_NZ;
constexpr std::uint32_t kNeverValue = 0u;

/**
 * @brief Allocates RP1 signal-array slots [0, RP1_MAX_SIGNALS), skipping any
 *        reserved slots (e.g. the sentinel).
 */
class SignalSlotAllocator {
   public:
    SignalSlotAllocator() : used_(RP1_MAX_SIGNALS, false) {}

    /// Mark @p slot as unavailable for future @ref alloc calls.
    void reserve(std::uint32_t slot) {
        if (slot < used_.size()) used_[slot] = true;
    }

    /// Return the lowest free slot and mark it used.
    /// @throws std::runtime_error when the signal array is exhausted.
    std::uint32_t alloc();

   private:
    std::vector<bool> used_;
};

/**
 * @brief Allocates RP1 loop ids [0, RP1_MAX_LOOPS).
 */
class LoopIdAllocator {
   public:
    /// Return the next loop id.
    /// @throws std::runtime_error when more than RP1_MAX_LOOPS loops are needed.
    std::uint8_t alloc();

   private:
    std::uint32_t next_ = 0;
};

struct BarrierPhysicalEvent {
    std::uint8_t  bucket = 0;
    std::uint32_t mask = 0;
};

struct BarrierBucketRange {
    std::uint8_t start = 0;
    std::uint8_t end = 0;
    bool empty = true;
};

struct BarrierEventSpec {
    std::string id;
    std::string domain;
    std::vector<std::string> depends;
};

struct BarrierResetDomainSpec {
    std::string id;
    std::optional<std::string> parent;
    std::vector<std::string> children;
};

struct BarrierSyntheticNode {
    enum class Kind {
        Collector,
        Transition,
    };

    Kind kind = Kind::Collector;
    std::string id;
    std::string domain;
    std::vector<std::string> depends;
    std::string set;
};

struct BarrierLoweringInput {
    std::string rootDomain;
    std::vector<BarrierResetDomainSpec> domains;
    std::vector<BarrierEventSpec> events;
    std::uint8_t bitsPerBucket = 31;
    std::uint8_t maxBuckets = RP1_MAX_BUCKETS;
};

struct BarrierLoweringResult {
    std::map<std::string, BarrierPhysicalEvent> events;
    std::map<std::string, BarrierBucketRange> domainRanges;
    std::vector<BarrierSyntheticNode> syntheticNodes;
};

BarrierLoweringResult lowerBarrierEvents(const BarrierLoweringInput& input);

}  // namespace vrt::graph::fpga

#endif  // VRT_GRAPH_DEVICE_FPGA_CONTROL_LOWERING_HPP
