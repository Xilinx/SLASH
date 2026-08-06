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
#include <vrt/graph/render/dot.hpp>

using namespace vrt::graph;

namespace {

ScheduledGraph renderedSchedule() {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar count = root->inputScalar(ScalarType::U32, "count");
    auto body = root->createChild();
    body->addKernel(
        KernelDescriptor{"work", DeviceType::CPU, std::nullopt, {}},
        {}, "cpu");
    ::vrt::graph::detail::LoopRecord loop;
    loop.tripCount = LoopTripCount::scalar(count);
    loop.body = std::move(body);
    root->addLoop(std::move(loop));

    auto resolved = resolveGraph(AuthoredGraph::snapshot(*root));
    EXPECT_TRUE(resolved.ok());
    auto cpu = std::make_shared<CpuDevice>("cpu");
    std::map<std::string, std::shared_ptr<IDevice>> devices{{"cpu", cpu}};
    auto placed = placeGraph(
        *resolved.output, DeviceCapabilityCatalog::fromDevices(devices));
    EXPECT_TRUE(placed.ok());
    auto routed = routeGraph(
        *placed.output, TransferCapabilityCatalog::fromGraph(devices, {}));
    EXPECT_TRUE(routed.ok());
    auto scheduled = scheduleGraph(*routed.output);
    EXPECT_TRUE(scheduled.ok());
    return std::move(*scheduled.output);
}

}  // namespace

TEST(RenderTest, ResolvedProjectionShowsOperationsAndValues) {
    ScheduledGraph scheduled = renderedSchedule();
    const std::string dot = render::renderToDot(
        scheduled.routed().placed().resolved());
    EXPECT_NE(dot.find("digraph"), std::string::npos);
    EXPECT_NE(dot.find("work"), std::string::npos);
}

TEST(RenderTest, PlacedProjectionShowsDeviceAssignments) {
    ScheduledGraph scheduled = renderedSchedule();
    const std::string dot = render::renderToDot(
        scheduled.routed().placed());
    EXPECT_NE(dot.find("cpu"), std::string::npos);
    EXPECT_NE(dot.find("loop"), std::string::npos);
}

TEST(RenderTest, RoutedProjectionShowsDependencyLayer) {
    ScheduledGraph scheduled = renderedSchedule();
    const std::string dot = render::renderToDot(scheduled.routed());
    EXPECT_NE(dot.find("digraph"), std::string::npos);
    EXPECT_NE(dot.find("RoutedGraph"), std::string::npos);
}

TEST(RenderTest, ScheduledProjectionShowsQueuesAndSteps) {
    ScheduledGraph scheduled = renderedSchedule();
    const std::string dot = render::renderToDot(scheduled);
    EXPECT_NE(dot.find("cluster_q"), std::string::npos);
    EXPECT_NE(dot.find("\"s"), std::string::npos);
}
