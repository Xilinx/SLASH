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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vrt/graph/detail/authoring_region.hpp>
#include <vrt/graph/ir/resolved_graph.hpp>
#include <vrt/graph/pass/validate_authored_graph.hpp>

using namespace vrt::graph;

namespace {

IOTypeMap copyType() {
    return IOTypeMap{}
        .in<std::int32_t>("in")
        .out<std::int32_t>("out");
}

std::string addCopy(
    const std::shared_ptr<detail::AuthoringRegion>& region,
    const GraphBuffer& input, const GraphBuffer& output,
    std::string name) {
    detail::PortBindings io;
    io.bindInput("in", input)
        .bindExistingOutput("out", output);
    return region->addKernel(
        KernelDescriptor{
            std::move(name), DeviceType::CPU,
            std::nullopt, copyType()},
        std::move(io), "cpu");
}

bool hasDiagnostic(
    const Diagnostics& diagnostics, DiagCode code,
    const std::string& text = {}) {
    return std::any_of(
        diagnostics.entries().begin(),
        diagnostics.entries().end(),
        [&](const Diagnostic& diagnostic) {
            return diagnostic.code == code &&
                   (text.empty() ||
                    diagnostic.message.find(text) !=
                        std::string::npos);
        });
}

}  // namespace

TEST(AuthoredGraphValidationTest, ForwardUseIsLegal) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer future =
        root->buffer(BufferType::I32, "future", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    const std::string consumer =
        addCopy(root, future, output, "consumer");
    const std::string producer =
        addCopy(root, input, future, "producer");
    (void)consumer;
    (void)producer;

    AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    CompileResult<AuthoredGraph> validated =
        validateAuthoredGraph(authored);
    ASSERT_TRUE(validated.ok());

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(authored);
    ASSERT_TRUE(resolved.ok());
    ASSERT_EQ(resolved.output->root().topologicalOrder.size(), 2u);
    EXPECT_GT(
        resolved.output->root().topologicalOrder.front().value(),
        resolved.output->root().topologicalOrder.back().value());
}

TEST(AuthoredGraphValidationTest, MissingProducerFailsFirstPass) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer missing =
        root->buffer(BufferType::I32, "missing", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);
    addCopy(root, missing, output, "consumer");

    CompileResult<AuthoredGraph> result =
        validateAuthoredGraph(AuthoredGraph::snapshot(*root));
    ASSERT_FALSE(result.ok());
    EXPECT_TRUE(hasDiagnostic(
        result.diagnostics, DiagCode::MissingProducer,
        "buffer 'missing' with no producer"));
}

TEST(AuthoredGraphValidationTest,
     OutputScalarWithoutProducerIsNotAGraphInput) {
    auto root = detail::AuthoringRegion::createRoot();
    root->outputScalar(ScalarType::U32, "result");
    root->addKernel(
        KernelDescriptor{
            "side_effect", DeviceType::CPU, std::nullopt, {}},
        {}, "cpu");

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    CompileResult<AuthoredGraph> validated =
        validateAuthoredGraph(authored);
    ASSERT_FALSE(validated.ok());
    EXPECT_TRUE(hasDiagnostic(
        validated.diagnostics, DiagCode::MissingProducer,
        "graph output scalar 'result' has no producer"));

    CompileResult<ResolvedGraph> resolved = resolveGraph(authored);
    ASSERT_FALSE(resolved.ok());
    EXPECT_TRUE(hasDiagnostic(
        resolved.diagnostics, DiagCode::MissingProducer,
        "graph output scalar 'result' has no producer"));
}

TEST(AuthoredGraphValidationTest, DuplicateProducerFailsFirstPass) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer duplicate =
        root->outputBuffer(BufferType::I32, "duplicate", size);
    addCopy(root, input, duplicate, "first");
    addCopy(root, input, duplicate, "second");

    CompileResult<AuthoredGraph> result =
        validateAuthoredGraph(AuthoredGraph::snapshot(*root));
    ASSERT_FALSE(result.ok());
    EXPECT_TRUE(hasDiagnostic(
        result.diagnostics, DiagCode::DuplicateProducer,
        "multiple operations produce value 'duplicate'"));
}

TEST(AuthoredGraphValidationTest, CrossScopeTokenFailsFirstPass) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    auto child = root->createChild();
    GraphBuffer childToken =
        child->buffer(BufferType::I32, "child", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);
    addCopy(root, childToken, output, "consumer");

    CompileResult<AuthoredGraph> result =
        validateAuthoredGraph(AuthoredGraph::snapshot(*root));
    ASSERT_FALSE(result.ok());
    EXPECT_TRUE(hasDiagnostic(
        result.diagnostics, DiagCode::InvalidScope,
        "from an invalid scope"));
}

TEST(AuthoredGraphValidationTest, ForeignGraphTokenFailsFirstPass) {
    auto first = detail::AuthoringRegion::createRoot();
    GraphScalar firstSize =
        first->inputScalar(ScalarType::U64, "size");
    GraphBuffer foreign =
        first->inputBuffer(BufferType::I32, "input", firstSize);

    auto second = detail::AuthoringRegion::createRoot();
    GraphScalar secondSize =
        second->inputScalar(ScalarType::U64, "size");
    GraphBuffer output =
        second->outputBuffer(
            BufferType::I32, "output", secondSize);
    addCopy(second, foreign, output, "consumer");

    CompileResult<AuthoredGraph> result =
        validateAuthoredGraph(AuthoredGraph::snapshot(*second));
    ASSERT_FALSE(result.ok());
    EXPECT_TRUE(hasDiagnostic(
        result.diagnostics, DiagCode::InvalidScope,
        "different graph"));
}

TEST(AuthoredGraphValidationTest,
     ChildOwnershipRejectsCyclesReuseAndPartialClaims) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar trips =
        root->inputScalar(ScalarType::U32, "trips");
    auto loopWith = [&](std::shared_ptr<detail::AuthoringRegion> body) {
        detail::LoopRecord loop;
        loop.tripCount = LoopTripCount::scalar(trips);
        loop.body = std::move(body);
        return loop;
    };

    EXPECT_THROW(root->addLoop(loopWith(root)), std::invalid_argument);

    auto child = root->createChild();
    EXPECT_THROW(child->addLoop(loopWith(root)), std::invalid_argument);

    auto foreignRoot = detail::AuthoringRegion::createRoot();
    auto foreignChild = foreignRoot->createChild();
    EXPECT_THROW(
        root->addLoop(loopWith(foreignChild)),
        std::invalid_argument);

    auto claimed = root->createChild();
    EXPECT_NO_THROW(root->addLoop(loopWith(claimed)));
    EXPECT_THROW(
        root->addLoop(loopWith(claimed)), std::invalid_argument);

    auto repeatedArm = root->createChild();
    detail::ConditionalRecord repeated;
    repeated.condition = Condition::alwaysTrue();
    repeated.thenRegion = repeatedArm;
    repeated.elseRegion = repeatedArm;
    EXPECT_THROW(
        root->addConditional(std::move(repeated)),
        std::invalid_argument);

    auto stillFree = root->createChild();
    detail::ConditionalRecord partial;
    partial.condition = Condition::alwaysTrue();
    partial.thenRegion = stillFree;
    partial.elseRegion = claimed;
    EXPECT_THROW(
        root->addConditional(std::move(partial)),
        std::invalid_argument);
    EXPECT_NO_THROW(root->addLoop(loopWith(stillFree)));
}

TEST(AuthoredGraphValidationTest, SnapshotRejectsExcessiveNesting) {
    auto root = detail::AuthoringRegion::createRoot();
    auto parent = root;
    for (std::size_t depth = 0; depth < 256; ++depth) {
        GraphScalar trips =
            parent->inputScalar(ScalarType::U32, "trips");
        auto child = parent->createChild();
        detail::LoopRecord loop;
        loop.tripCount = LoopTripCount::scalar(trips);
        loop.body = child;
        parent->addLoop(std::move(loop));
        parent = std::move(child);
    }
    EXPECT_THROW(
        AuthoredGraph::snapshot(*root), std::invalid_argument);
}

TEST(AuthoredGraphValidationTest,
     ControlScalarsPreserveAndValidateGraphIdentity) {
    auto foreignRoot = detail::AuthoringRegion::createRoot();
    GraphScalar foreignPredicate =
        foreignRoot->inputScalar(ScalarType::U32, "predicate");
    GraphScalar foreignTrips =
        foreignRoot->inputScalar(ScalarType::U32, "trips");
    const Condition foreignCondition = foreignPredicate != 0u;
    const LoopTripCount foreignTripCount =
        LoopTripCount::scalar(foreignTrips);
    ASSERT_TRUE(foreignCondition.lhs().has_value());
    EXPECT_EQ(
        foreignCondition.lhs()->graphId(),
        foreignRoot->graphId());
    EXPECT_EQ(
        foreignTripCount.graphId(), foreignRoot->graphId());

    auto root = detail::AuthoringRegion::createRoot();
    root->inputScalar(ScalarType::U32, "predicate");
    root->inputScalar(ScalarType::U32, "trips");
    auto thenRegion = root->createChild();
    auto elseRegion = root->createChild();
    detail::ConditionalRecord conditional;
    conditional.condition = foreignCondition;
    conditional.thenRegion = std::move(thenRegion);
    conditional.elseRegion = std::move(elseRegion);
    root->addConditional(std::move(conditional));

    auto body = root->createChild();
    detail::LoopRecord loop;
    loop.tripCount = foreignTripCount;
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    CompileResult<AuthoredGraph> result =
        validateAuthoredGraph(AuthoredGraph::snapshot(*root));
    ASSERT_FALSE(result.ok());
    EXPECT_TRUE(hasDiagnostic(
        result.diagnostics, DiagCode::InvalidScope,
        "belongs to a different graph"));
}

TEST(AuthoredGraphValidationTest,
     ControlScalarsRequireDeclarationAndExactType) {
    auto root = detail::AuthoringRegion::createRoot();
    root->inputScalar(ScalarType::U32, "predicate");
    root->inputScalar(ScalarType::U32, "trips");

    detail::ConditionalRecord conditional;
    conditional.condition = Condition::compare(
        CompareOp::NE,
        ConditionOperand::scalar(
            ScalarType::I32, "predicate", root->scopeId(),
            root->graphId()),
        ConditionOperand::constant<std::int32_t>(0));
    conditional.thenRegion = root->createChild();
    conditional.elseRegion = root->createChild();
    root->addConditional(std::move(conditional));

    detail::LoopRecord loop;
    loop.tripCount = LoopTripCount::scalar(
        ScalarType::I32, "trips", root->scopeId(),
        root->graphId());
    loop.body = root->createChild();
    root->addLoop(std::move(loop));

    detail::ConditionalRecord undeclared;
    undeclared.condition = Condition::compare(
        CompareOp::NE,
        ConditionOperand::scalar(
            ScalarType::U32, "missing", root->scopeId(),
            root->graphId()),
        ConditionOperand::constant<std::uint32_t>(0));
    undeclared.thenRegion = root->createChild();
    undeclared.elseRegion = root->createChild();
    root->addConditional(std::move(undeclared));

    CompileResult<AuthoredGraph> result =
        validateAuthoredGraph(AuthoredGraph::snapshot(*root));
    ASSERT_FALSE(result.ok());
    EXPECT_TRUE(hasDiagnostic(
        result.diagnostics, DiagCode::TypeMismatch,
        "does not match its declared type"));
    EXPECT_TRUE(hasDiagnostic(
        result.diagnostics, DiagCode::InvalidScope,
        "is not declared"));
}

TEST(AuthoredGraphValidationTest,
     OrdinaryGraphInputRedefinitionIsRejected) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input =
        root->inputBuffer(BufferType::I32, "input", size);
    addCopy(root, input, input, "overwrite");

    CompileResult<AuthoredGraph> result =
        validateAuthoredGraph(AuthoredGraph::snapshot(*root));
    ASSERT_FALSE(result.ok());
    EXPECT_TRUE(hasDiagnostic(
        result.diagnostics, DiagCode::DuplicateProducer,
        "redefines graph input 'input'"));
}

TEST(AuthoredGraphValidationTest,
     LoopCarriedGraphInputRemainsLegal) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphScalar trips =
        root->inputScalar(ScalarType::U32, "trips");
    GraphBuffer state =
        root->inputBuffer(BufferType::I32, "state", size);

    auto body = root->createChild();
    GraphBuffer localInput =
        body->inputBuffer(BufferType::I32, "local_input", size);
    GraphBuffer localOutput =
        body->buffer(BufferType::I32, "local_output", size);
    body->importFromParent(
        std::vector<BufferBoundaryMapping>{{state, localInput}});
    const std::string copy =
        addCopy(body, localInput, localOutput, "body");
    body->exportToParent(
        std::vector<BufferBoundaryMapping>{{localOutput, state}},
        {copy});

    detail::LoopRecord loop;
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    CompileResult<AuthoredGraph> result =
        validateAuthoredGraph(AuthoredGraph::snapshot(*root));
    EXPECT_TRUE(result.ok());
}

TEST(AuthoredGraphValidationTest,
     RawConditionalOutputInferenceReachesResolver) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    IOTypeMap outputType =
        IOTypeMap{}.out<std::int32_t>("out");
    auto makeBranch = [&](const std::string& name) {
        auto branch = root->createChild();
        GraphBuffer local =
            branch->buffer(BufferType::I32, name, size);
        detail::PortBindings io;
        io.bindExistingOutput("out", local);
        branch->addKernel(
            KernelDescriptor{
                name, DeviceType::CPU, std::nullopt, outputType},
            std::move(io), "cpu");
        return branch;
    };

    detail::PortBindings controlIo;
    controlIo.bindExistingOutput("out", output);
    detail::ConditionalRecord conditional;
    conditional.ioType = outputType;
    conditional.ioMap = std::move(controlIo);
    conditional.condition = Condition::alwaysTrue();
    conditional.thenRegion = makeBranch("then_value");
    conditional.elseRegion = makeBranch("else_value");
    root->addConditional(std::move(conditional));

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    EXPECT_TRUE(validateAuthoredGraph(authored).ok());
    EXPECT_TRUE(resolveGraph(authored).ok());
}

TEST(AuthoredGraphValidationTest,
     RawLoopInoutOutputInferenceReachesResolver) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size =
        root->inputScalar(ScalarType::U64, "size");
    GraphScalar trips =
        root->inputScalar(ScalarType::U32, "trips");
    GraphBuffer initial =
        root->inputBuffer(BufferType::I32, "initial", size);
    GraphBuffer output =
        root->outputBuffer(BufferType::I32, "output", size);

    auto body = root->createChild();
    GraphBuffer local =
        body->buffer(BufferType::I32, "local", size);
    IOTypeMap bodyType =
        IOTypeMap{}.out<std::int32_t>("state");
    detail::PortBindings bodyIo;
    bodyIo.bindExistingOutput("state", local);
    body->addKernel(
        KernelDescriptor{
            "body", DeviceType::CPU, std::nullopt, bodyType},
        std::move(bodyIo), "cpu");

    IOTypeMap loopType =
        IOTypeMap{}.inout<std::int32_t>("state");
    detail::PortBindings loopIo;
    loopIo.bindExistingInout(
        "state", "state", initial, output);
    detail::LoopRecord loop;
    loop.ioType = std::move(loopType);
    loop.ioMap = std::move(loopIo);
    loop.tripCount = LoopTripCount::scalar(trips);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    const AuthoredGraph authored = AuthoredGraph::snapshot(*root);
    EXPECT_TRUE(validateAuthoredGraph(authored).ok());
    EXPECT_TRUE(resolveGraph(authored).ok());
}
