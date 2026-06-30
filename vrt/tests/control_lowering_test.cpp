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

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include <vrt/graph/control/condition.hpp>
#include <vrt/graph/device/fpga/control_lowering.hpp>

using namespace vrt::graph;
using vrt::graph::fpga::isRp1EvaluableCondition;
using vrt::graph::fpga::invertRp1Op;
using vrt::graph::fpga::LoopIdAllocator;
using vrt::graph::fpga::mapRp1Condition;
using vrt::graph::fpga::SignalSlotAllocator;
using vrt::graph::fpga::BarrierLoweringInput;
using vrt::graph::fpga::BarrierResetDomainSpec;
using vrt::graph::fpga::BarrierSyntheticNode;
using vrt::graph::fpga::BarrierEventSpec;
using vrt::graph::fpga::lowerBarrierEvents;

namespace {

Condition cmp(CompareOp op, ScalarType type, const char* name, ConditionOperand rhs) {
    return Condition::compare(op, ConditionOperand::scalar(type, name), std::move(rhs));
}

Condition cmpConstLhs(CompareOp op, ConditionOperand lhs, ScalarType type, const char* name) {
    return Condition::compare(op, std::move(lhs), ConditionOperand::scalar(type, name));
}

}  // namespace

TEST(ControlLowering, EvaluableUnsignedComparisons) {
    EXPECT_TRUE(isRp1EvaluableCondition(
        cmp(CompareOp::EQ, ScalarType::U32, "c", ConditionOperand::constant<uint32_t>(0))));
    EXPECT_TRUE(isRp1EvaluableCondition(
        cmp(CompareOp::NE, ScalarType::U16, "c", ConditionOperand::constant<uint16_t>(7))));
    EXPECT_TRUE(isRp1EvaluableCondition(
        cmp(CompareOp::LT, ScalarType::U32, "c", ConditionOperand::constant<uint32_t>(10))));
    EXPECT_TRUE(isRp1EvaluableCondition(
        cmp(CompareOp::LE, ScalarType::U32, "c", ConditionOperand::constant<uint32_t>(10))));
    EXPECT_TRUE(isRp1EvaluableCondition(
        cmp(CompareOp::GT, ScalarType::U32, "c", ConditionOperand::constant<uint32_t>(10))));
    EXPECT_TRUE(isRp1EvaluableCondition(
        cmp(CompareOp::GE, ScalarType::U8, "c", ConditionOperand::constant<uint8_t>(3))));
}

TEST(ControlLowering, NonEvaluableConditions) {
    // Equality on a signed type is still fine (bit equality), but signed
    // ordering is not RP1-evaluable.
    EXPECT_TRUE(isRp1EvaluableCondition(
        cmp(CompareOp::EQ, ScalarType::I32, "c", ConditionOperand::constant<int32_t>(5))));
    EXPECT_FALSE(isRp1EvaluableCondition(
        cmp(CompareOp::LT, ScalarType::I32, "c", ConditionOperand::constant<int32_t>(5))));

    // Float and epsilon comparisons.
    EXPECT_FALSE(isRp1EvaluableCondition(
        cmp(CompareOp::LT, ScalarType::F32, "c", ConditionOperand::constant<float>(1.0f))));
    EXPECT_FALSE(isRp1EvaluableCondition(Condition::compareWithEpsilon(
        CompareOp::EQE, ConditionOperand::scalar(ScalarType::F32, "c"),
        ConditionOperand::constant<float>(1.0f), ConditionOperand::constant<float>(0.01f))));

    // 64-bit-wide scalar does not fit a 32-bit signal slot.
    EXPECT_FALSE(isRp1EvaluableCondition(
        cmp(CompareOp::EQ, ScalarType::U64, "c", ConditionOperand::constant<uint64_t>(0))));

    // Always predicates and scalar-vs-scalar.
    EXPECT_FALSE(isRp1EvaluableCondition(Condition::alwaysTrue()));
    EXPECT_FALSE(isRp1EvaluableCondition(Condition::compare(
        CompareOp::EQ, ConditionOperand::scalar(ScalarType::U32, "a"),
        ConditionOperand::scalar(ScalarType::U32, "b"))));
}

TEST(ControlLowering, MapsDirectOperators) {
    auto eq = mapRp1Condition(
        cmp(CompareOp::EQ, ScalarType::U32, "counter", ConditionOperand::constant<uint32_t>(0)));
    EXPECT_EQ(eq.op, RP1_COP_EQ);
    EXPECT_EQ(eq.value, 0u);
    EXPECT_EQ(eq.scalarName, "counter");

    auto ne = mapRp1Condition(
        cmp(CompareOp::NE, ScalarType::U32, "c", ConditionOperand::constant<uint32_t>(42)));
    EXPECT_EQ(ne.op, RP1_COP_NE);
    EXPECT_EQ(ne.value, 42u);

    auto lt = mapRp1Condition(
        cmp(CompareOp::LT, ScalarType::U32, "c", ConditionOperand::constant<uint32_t>(10)));
    EXPECT_EQ(lt.op, RP1_COP_LT);
    EXPECT_EQ(lt.value, 10u);

    auto ge = mapRp1Condition(
        cmp(CompareOp::GE, ScalarType::U32, "c", ConditionOperand::constant<uint32_t>(10)));
    EXPECT_EQ(ge.op, RP1_COP_GE);
    EXPECT_EQ(ge.value, 10u);
}

TEST(ControlLowering, RewritesLeAndGt) {
    auto le = mapRp1Condition(
        cmp(CompareOp::LE, ScalarType::U32, "c", ConditionOperand::constant<uint32_t>(10)));
    EXPECT_EQ(le.op, RP1_COP_LT);
    EXPECT_EQ(le.value, 11u);  // s <= 10  <=>  s < 11

    auto gt = mapRp1Condition(
        cmp(CompareOp::GT, ScalarType::U32, "c", ConditionOperand::constant<uint32_t>(10)));
    EXPECT_EQ(gt.op, RP1_COP_GE);
    EXPECT_EQ(gt.value, 11u);  // s > 10  <=>  s >= 11
}

TEST(ControlLowering, NormalisesConstantOnLeft) {
    // 10 < counter  <=>  counter > 10  <=>  counter >= 11
    auto m = mapRp1Condition(cmpConstLhs(CompareOp::LT, ConditionOperand::constant<uint32_t>(10),
                                         ScalarType::U32, "counter"));
    EXPECT_EQ(m.op, RP1_COP_GE);
    EXPECT_EQ(m.value, 11u);
    EXPECT_EQ(m.scalarName, "counter");
}

TEST(ControlLowering, InvertOp) {
    EXPECT_EQ(invertRp1Op(RP1_COP_EQ), RP1_COP_NE);
    EXPECT_EQ(invertRp1Op(RP1_COP_NE), RP1_COP_EQ);
    EXPECT_EQ(invertRp1Op(RP1_COP_LT), RP1_COP_GE);
    EXPECT_EQ(invertRp1Op(RP1_COP_GE), RP1_COP_LT);
    EXPECT_EQ(invertRp1Op(RP1_COP_AND_NZ), RP1_COP_AND_Z);
    EXPECT_EQ(invertRp1Op(RP1_COP_AND_Z), RP1_COP_AND_NZ);
}

TEST(ControlLowering, SignalSlotAllocatorSkipsReserved) {
    SignalSlotAllocator alloc;
    alloc.reserve(0);
    alloc.reserve(1);
    EXPECT_EQ(alloc.alloc(), 2u);
    EXPECT_EQ(alloc.alloc(), 3u);
}

TEST(ControlLowering, LoopIdAllocatorIncrementsAndBounds) {
    LoopIdAllocator alloc;
    EXPECT_EQ(alloc.alloc(), 0u);
    EXPECT_EQ(alloc.alloc(), 1u);
    for (int i = 2; i < RP1_MAX_LOOPS; ++i) {
        (void)alloc.alloc();
    }
    EXPECT_THROW(alloc.alloc(), std::runtime_error);
}

TEST(ControlLowering, BarrierLoweringRejectsChildToParentDependency) {
    BarrierLoweringInput input;
    input.rootDomain = "root";
    input.domains = {
        {"root", std::nullopt, {"body"}},
        {"body", std::string("root"), {}},
    };
    input.events = {
        {"child_done", "body", {}},
        {"parent_consumer", "root", {"child_done"}},
    };

    EXPECT_THROW(lowerBarrierEvents(input), std::runtime_error);
}

TEST(ControlLowering, BarrierLoweringInsertsAncestorToChildTransition) {
    BarrierLoweringInput input;
    input.rootDomain = "root";
    input.bitsPerBucket = 4;
    input.domains = {
        {"root", std::nullopt, {"body"}},
        {"body", std::string("root"), {}},
    };
    input.events = {
        {"root_done", "root", {}},
        {"body_consumer", "body", {"root_done"}},
    };

    auto out = lowerBarrierEvents(input);
    auto transitions = 0;
    for (const auto& node : out.syntheticNodes) {
        if (node.kind == BarrierSyntheticNode::Kind::Transition) {
            ++transitions;
            EXPECT_EQ(node.domain, "body");
            EXPECT_EQ(node.depends, std::vector<std::string>{"root_done"});
            EXPECT_TRUE(out.events.count(node.set));
        }
    }
    EXPECT_EQ(transitions, 1);
    EXPECT_LE(out.domainRanges["root"].start, out.domainRanges["body"].start);
    EXPECT_LE(out.domainRanges["body"].end, out.domainRanges["root"].end);
}

TEST(ControlLowering, BarrierLoweringCreatesCollectorsAtLowBucketRate) {
    BarrierLoweringInput input;
    input.rootDomain = "root";
    input.bitsPerBucket = 2;
    input.domains = {{"root", std::nullopt, {}}};
    input.events = {
        {"a", "root", {}},
        {"b", "root", {}},
        {"c", "root", {}},
        {"d", "root", {}},
        {"join", "root", {"a", "b", "c", "d"}},
    };

    auto out = lowerBarrierEvents(input);
    auto collectors = 0;
    for (const auto& node : out.syntheticNodes) {
        if (node.kind == BarrierSyntheticNode::Kind::Collector) {
            ++collectors;
            ASSERT_FALSE(node.depends.empty());
            EXPECT_TRUE(out.events.count(node.set));
        }
    }
    EXPECT_GT(collectors, 0);
}

TEST(ControlLowering, BarrierLoweringAssignsNestedContiguousRanges) {
    BarrierLoweringInput input;
    input.rootDomain = "root";
    input.bitsPerBucket = 2;
    input.domains = {
        {"root", std::nullopt, {"loop1"}},
        {"loop1", std::string("root"), {"loop2"}},
        {"loop2", std::string("loop1"), {}},
    };
    input.events = {
        {"r0", "root", {}},
        {"r1", "root", {}},
        {"l1", "loop1", {"r0"}},
        {"l2", "loop2", {"r1", "l1"}},
    };

    auto out = lowerBarrierEvents(input);
    const auto root = out.domainRanges.at("root");
    const auto loop1 = out.domainRanges.at("loop1");
    const auto loop2 = out.domainRanges.at("loop2");
    EXPECT_FALSE(root.empty);
    EXPECT_FALSE(loop1.empty);
    EXPECT_FALSE(loop2.empty);
    EXPECT_LE(root.start, loop1.start);
    EXPECT_LE(loop1.end, root.end);
    EXPECT_LE(loop1.start, loop2.start);
    EXPECT_LE(loop2.end, loop1.end);
}

TEST(ControlLowering, BarrierLoweringFailsWhenBucketsExhausted) {
    BarrierLoweringInput input;
    input.rootDomain = "root";
    input.bitsPerBucket = 2;
    input.maxBuckets = 1;
    input.domains = {{"root", std::nullopt, {}}};
    input.events = {
        {"a", "root", {}},
        {"b", "root", {}},
        {"c", "root", {}},
    };

    EXPECT_THROW(lowerBarrierEvents(input), std::runtime_error);
}
