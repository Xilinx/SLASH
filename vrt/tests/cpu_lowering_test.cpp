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
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <vrt/graph/backend_resource_binding.hpp>
#include <vrt/graph/backend_runtime.hpp>
#include <vrt/graph/detail/authoring_region.hpp>
#include <vrt/graph/device/cpu/cpu_lowering.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/ir/placed_graph.hpp>
#include <vrt/graph/ir/resolved_graph.hpp>
#include <vrt/graph/ir/routed_graph.hpp>
#include <vrt/graph/ir/scheduled_graph.hpp>

using namespace vrt::graph;

TEST(CpuLoweringTest, LowersScheduledDependenciesDirectly) {
    auto root = detail::AuthoringRegion::createRoot();
    GraphScalar size = root->inputScalar(ScalarType::U64, "size");
    GraphBuffer input = root->inputBuffer(BufferType::I32, "input", size);
    GraphBuffer middle = root->buffer(BufferType::I32, "middle", size);
    GraphBuffer output = root->outputBuffer(BufferType::I32, "output", size);
    IOTypeMap type = IOTypeMap{}.in<std::int32_t>("in").out<std::int32_t>("out");
    detail::PortBindings first;
    first.bindInput("in", input).bindExistingOutput("out", middle);
    detail::PortBindings second;
    second.bindInput("in", middle).bindExistingOutput("out", output);
    root->addKernel(
        KernelDescriptor{"first", DeviceType::CPU, std::nullopt, type},
        std::move(first), "cpu");
    root->addKernel(
        KernelDescriptor{"second", DeviceType::CPU, std::nullopt, type},
        std::move(second), "cpu");

    auto resolved = resolveGraph(AuthoredGraph::snapshot(*root));
    ASSERT_TRUE(resolved.ok());
    auto cpu = std::make_shared<CpuDevice>("cpu");
    std::map<std::string, std::shared_ptr<IDevice>> devices{{"cpu", cpu}};
    auto placed = placeGraph(
        *resolved.output, DeviceCapabilityCatalog::fromDevices(devices));
    ASSERT_TRUE(placed.ok());
    auto routed = routeGraph(
        *placed.output, TransferCapabilityCatalog::fromGraph(devices, {}));
    ASSERT_TRUE(routed.ok());
    auto scheduled = scheduleGraph(*routed.output);
    ASSERT_TRUE(scheduled.ok());
    ASSERT_EQ(scheduled.output->queues().size(), 1u);
    auto resources = bindBackendResources(*scheduled.output, devices);
    ASSERT_TRUE(resources.ok());
    auto runtime = std::make_shared<BackendRuntimeState>(
        std::make_shared<std::map<std::string, std::uint64_t>>());
    HostActionTable actions;
    BackendLoweringContext context{
        *scheduled.output, scheduled.output->queues().front(),
        *resources.output, runtime, actions};

    const CpuProgram program = CpuLowering::lower(context);
    std::vector<const CpuProgramNode*> kernels;
    for (const CpuProgramNode& node : program.nodes) {
        if (node.kind == CpuProgramNodeKind::Kernel) kernels.push_back(&node);
    }
    ASSERT_EQ(kernels.size(), 2u);
    EXPECT_EQ(kernels[0]->kernel.kernel.name, "first");
    EXPECT_EQ(kernels[1]->kernel.kernel.name, "second");
    EXPECT_NE(
        std::find(kernels[0]->successors.begin(),
                  kernels[0]->successors.end(),
                  static_cast<std::size_t>(kernels[1] - &program.nodes[0])),
        kernels[0]->successors.end());
    EXPECT_GT(kernels[1]->initialUnmet, 0u);
}

TEST(CpuLoweringTest, RuntimeScalarAccessUsesOneSharedLock) {
    auto values =
        std::make_shared<std::map<std::string, std::uint64_t>>();
    BackendRuntimeState runtime(values);

    std::unique_lock<std::mutex> held(runtime.scalarMutex());
    std::promise<void> started;
    std::future<void> writer = std::async(
        std::launch::async,
        [&] {
            started.set_value();
            runtime.writeScalar(DeviceId("cpu"), "value", 42u);
        });
    started.get_future().wait();

    EXPECT_EQ(
        writer.wait_for(std::chrono::milliseconds(10)),
        std::future_status::timeout);
    held.unlock();
    EXPECT_EQ(
        writer.wait_for(std::chrono::seconds(1)),
        std::future_status::ready);
    writer.get();
    EXPECT_EQ(runtime.readScalar(DeviceId("cpu"), "value"), 42u);
}
