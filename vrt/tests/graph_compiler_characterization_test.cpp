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

#include <map>
#include <memory>
#include <string>

#include <vrt/graph/detail/authoring_region.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/ir/placed_graph.hpp>
#include <vrt/graph/ir/resolved_graph.hpp>
#include <vrt/graph/ir/routed_graph.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>
#include <vrt/graph/semantic_plan.hpp>

using namespace vrt::graph;

namespace {

ScheduledGraph nestedLoopSchedule(const std::string& kernelName) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar count = root->inputScalar(ScalarType::U32, "count");
    auto body = root->createChild();
    body->addKernel(
        KernelDescriptor{kernelName, DeviceType::CPU, std::nullopt, {}},
        {}, "cpu");
    ::vrt::graph::detail::LoopRecord loop;
    loop.tripCount = LoopTripCount::scalar(count);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    CompileResult<ResolvedGraph> resolved =
        resolveGraph(AuthoredGraph::snapshot(*root));
    EXPECT_TRUE(resolved.ok());
    auto cpu = std::make_shared<CpuDevice>("cpu");
    std::map<std::string, std::shared_ptr<IDevice>> devices{{"cpu", cpu}};
    CompileResult<PlacedGraph> placed = placeGraph(
        *resolved.output, DeviceCapabilityCatalog::fromDevices(devices));
    EXPECT_TRUE(placed.ok());
    CompileResult<RoutedGraph> routed = routeGraph(
        *placed.output,
        TransferCapabilityCatalog::fromGraph(devices, {}));
    EXPECT_TRUE(routed.ok());
    CompileResult<ScheduledGraph> scheduled = scheduleGraph(*routed.output);
    EXPECT_TRUE(scheduled.ok());
    return std::move(*scheduled.output);
}

}  // namespace

TEST(GraphCompilerCharacterizationTest,
     ScheduledProjectionIgnoresGeneratedScopeIds) {
    const SemanticPlacementPlan first = normalizeOperationPlacements(
        nestedLoopSchedule("increment"));
    const SemanticPlacementPlan second = normalizeOperationPlacements(
        nestedLoopSchedule("increment"));
    EXPECT_EQ(first, second) << first.toString() << "\n---\n"
                             << second.toString();
    EXPECT_NE(first.toString().find("loop_body"), std::string::npos);
}

TEST(GraphCompilerCharacterizationTest,
     ScheduledProjectionPreservesKernelIdentity) {
    const SemanticPlacementPlan first = normalizeOperationPlacements(
        nestedLoopSchedule("increment"));
    const SemanticPlacementPlan second = normalizeOperationPlacements(
        nestedLoopSchedule("different"));
    EXPECT_NE(first, second);
}
