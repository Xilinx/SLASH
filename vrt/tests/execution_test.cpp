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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <vrt/graph/graph.hpp>

#include "test_support/graph_internal.hpp"

using namespace vrt::graph;

namespace {

class Gate {
   public:
    void arriveAndWait() {
        std::unique_lock<std::mutex> lock(mutex_);
        started_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return released_; });
    }

    bool waitUntilStarted(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(
            lock, timeout, [&] { return started_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool started_ = false;
    bool released_ = false;
};

class GatedCopyKernel final : public CpuKernel {
   public:
    GatedCopyKernel(
        std::shared_ptr<Gate> gate, bool fail)
        : CpuKernel("gated_copy"),
          gate_(std::move(gate)), fail_(fail) {}

    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}
            .in<std::int32_t>("in")
            .out<std::int32_t>("out");
    }

    void run(Args& args) override {
        gate_->arriveAndWait();
        if (fail_) {
            throw std::runtime_error("planned execution failure");
        }
        auto input = args.in<std::int32_t>("in");
        auto output = args.out<std::int32_t>("out");
        std::copy(input.begin(), input.end(), output.begin());
    }

   private:
    std::shared_ptr<Gate> gate_;
    bool fail_;
};

class BlockingLaunchExecutable final : public IBackendExecutable {
   public:
    explicit BlockingLaunchExecutable(
        std::shared_ptr<Gate> gate)
        : gate_(std::move(gate)) {}

    QueueId queue() const override { return QueueId(0); }
    DeviceId device() const override {
        return DeviceId("blocking");
    }

    void prepareLaunch() override {
        gate_->arriveAndWait();
        throw std::runtime_error("planned launch failure");
    }

    void launch() override {
        throw std::logic_error(
            "BlockingLaunchExecutable::launch must not run");
    }

    void wait() override {}

   private:
    std::shared_ptr<Gate> gate_;
};

struct WaitReentryState {
    Execution* execution = nullptr;
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    std::string error;
};

class WaitReentryExecutable final : public IBackendExecutable {
   public:
    explicit WaitReentryExecutable(
        std::shared_ptr<WaitReentryState> state)
        : state_(std::move(state)) {}

    ~WaitReentryExecutable() override {
        if (worker_.joinable()) worker_.join();
    }

    QueueId queue() const override { return QueueId(0); }
    DeviceId device() const override {
        return DeviceId("reentrant");
    }

    void launch() override {
        worker_ = std::thread([this] {
            detail::BackendWorkerScope workerScope;
            try {
                state_->execution->wait();
            } catch (const std::exception& error) {
                state_->error = error.what();
            }
            state_->gate->arriveAndWait();
        });
    }

    void wait() override {
        if (worker_.joinable()) worker_.join();
    }

   private:
    std::shared_ptr<WaitReentryState> state_;
    std::thread worker_;
};

struct DestructionReentryState {
    std::unique_ptr<Execution>* execution = nullptr;
};

class DestructionReentryExecutable final
    : public IBackendExecutable {
   public:
    explicit DestructionReentryExecutable(
        std::shared_ptr<DestructionReentryState> state)
        : state_(std::move(state)) {}

    ~DestructionReentryExecutable() override {
        if (worker_.joinable()) worker_.detach();
    }

    QueueId queue() const override { return QueueId(0); }
    DeviceId device() const override {
        return DeviceId("destruction-reentrant");
    }

    void launch() override {
        worker_ = std::thread([this] {
            detail::BackendWorkerScope workerScope;
            state_->execution->reset();
        });
    }

    void wait() override {
        if (worker_.joinable()) worker_.join();
    }

   private:
    std::shared_ptr<DestructionReentryState> state_;
    std::thread worker_;
};

struct ScalarReentryState {
    Execution* execution = nullptr;
    std::optional<GraphScalar> input;
    std::atomic_bool rejected{false};
};

class ScalarReentryKernel final : public CpuKernel {
   public:
    explicit ScalarReentryKernel(
        std::shared_ptr<ScalarReentryState> state)
        : CpuKernel("scalar_reentry"),
          state_(std::move(state)) {}

    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}
            .scalarIn<std::uint32_t>("in")
            .scalarOut<std::uint32_t>("out");
    }

    void run(Args& args) override {
        try {
            state_->execution->writeScalar(
                *state_->input, std::uint32_t{99});
        } catch (const std::runtime_error&) {
            state_->rejected.store(
                true, std::memory_order_release);
        }
        args.scalarOut<std::uint32_t>("out") =
            args.scalarIn<std::uint32_t>("in") + 1u;
    }

   private:
    std::shared_ptr<ScalarReentryState> state_;
};

struct GatedExecution {
    Graph graph = Graph::withDefaults();
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    GraphScalar size =
        graph.scalarInput<std::uint64_t>("size");
    GraphBuffer input =
        graph.input<std::int32_t>("input", size);
    GraphBuffer output =
        graph.output<std::int32_t>("output", size);

    GatedExecution() {
        auto kernel =
            graph.cpu().add<GatedCopyKernel>(gate, false);
        graph.addKernelCall({
            .kernel = kernel,
            .inputs = {{"in", input}},
            .outputs = {{"out", output}},
        });
    }
};

}  // namespace

TEST(ExecutionStateTest, RejectsDuplicateLaunchAndHostIoWhileActive) {
    GatedExecution fixture;
    Execution execution = fixture.graph.compile();
    const std::vector<std::int32_t> values{1, 2, 3, 4};
    execution.writeScalar(
        fixture.size,
        static_cast<std::uint64_t>(values.size()));
    execution.write(fixture.input, values);

    execution.launch();
    ASSERT_TRUE(fixture.gate->waitUntilStarted(
        std::chrono::seconds(2)));

    EXPECT_THROW(execution.launch(), std::runtime_error);
    EXPECT_THROW(
        execution.writeScalar(
            fixture.size,
            static_cast<std::uint64_t>(values.size())),
        std::runtime_error);
    EXPECT_THROW(
        execution.write(fixture.input, values),
        std::runtime_error);
    std::vector<std::int32_t> activeRead(values.size());
    EXPECT_THROW(
        execution.read(fixture.output, activeRead),
        std::runtime_error);

    fixture.gate->release();
    ASSERT_NO_THROW(execution.wait());
    std::vector<std::int32_t> result(values.size());
    execution.read(fixture.output, result);
    EXPECT_EQ(result, values);
}

TEST(ExecutionStateTest, ConcurrentWaitersShareOneFailure) {
    Graph graph = Graph::withDefaults();
    auto gate = std::make_shared<Gate>();
    auto kernel =
        graph.cpu().add<GatedCopyKernel>(gate, true);
    GraphScalar size =
        graph.scalarInput<std::uint64_t>("size");
    GraphBuffer input =
        graph.input<std::int32_t>("input", size);
    GraphBuffer output =
        graph.output<std::int32_t>("output", size);
    graph.addKernelCall({
        .kernel = kernel,
        .inputs = {{"in", input}},
        .outputs = {{"out", output}},
    });

    Execution execution = graph.compile();
    execution.writeScalar(size, std::uint64_t{1});
    execution.write(
        input, std::vector<std::int32_t>{7});
    execution.launch();
    ASSERT_TRUE(gate->waitUntilStarted(
        std::chrono::seconds(2)));

    auto waitForFailure = [&] {
        try {
            execution.wait();
            return std::string{};
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
    };
    auto first = std::async(
        std::launch::async, waitForFailure);
    auto second = std::async(
        std::launch::async, waitForFailure);
    gate->release();

    EXPECT_EQ(first.get(), "planned execution failure");
    EXPECT_EQ(second.get(), "planned execution failure");
    EXPECT_THROW(execution.wait(), std::runtime_error);
}

TEST(ExecutionStateTest, LaunchingWaitersShareLaunchFailure) {
    auto gate = std::make_shared<Gate>();
    auto backend =
        std::make_unique<BlockingLaunchExecutable>(gate);
    IBackendExecutable* root = backend.get();
    std::vector<std::unique_ptr<IBackendExecutable>>
        executables;
    executables.push_back(std::move(backend));
    Execution execution =
        detail::ExecutionTestAccess::makeExecution(
            std::move(executables), {root});

    auto captureError = [](auto&& work) {
        try {
            work();
            return std::string{};
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
    };
    auto launching = std::async(
        std::launch::async, [&] {
            return captureError([&] { execution.launch(); });
        });
    ASSERT_TRUE(gate->waitUntilStarted(
        std::chrono::seconds(2)));
    EXPECT_THROW(execution.launch(), std::runtime_error);

    auto firstWaiter = std::async(
        std::launch::async, [&] {
            return captureError([&] { execution.wait(); });
        });
    auto secondWaiter = std::async(
        std::launch::async, [&] {
            return captureError([&] { execution.wait(); });
        });
    gate->release();

    EXPECT_EQ(launching.get(), "planned launch failure");
    EXPECT_EQ(firstWaiter.get(), "planned launch failure");
    EXPECT_EQ(secondWaiter.get(), "planned launch failure");
}

TEST(ExecutionStateTest,
     BackendWorkerReentrantWaitIsRejectedWithoutPublishingIdle) {
    auto state = std::make_shared<WaitReentryState>();
    auto backend =
        std::make_unique<WaitReentryExecutable>(state);
    IBackendExecutable* root = backend.get();
    std::vector<std::unique_ptr<IBackendExecutable>>
        executables;
    executables.push_back(std::move(backend));
    Execution execution =
        detail::ExecutionTestAccess::makeExecution(
            std::move(executables), {root});
    state->execution = &execution;

    execution.launch();
    ASSERT_TRUE(state->gate->waitUntilStarted(
        std::chrono::seconds(2)));
    EXPECT_EQ(
        state->error,
        "Execution::wait: reentrant wait from a backend worker "
        "is not allowed");

    auto externalWaiter = std::async(
        std::launch::async, [&] {
            try {
                execution.wait();
                return std::string{};
            } catch (const std::exception& error) {
                return std::string(error.what());
            }
        });
    EXPECT_EQ(
        externalWaiter.wait_for(
            std::chrono::milliseconds(20)),
        std::future_status::timeout);

    state->gate->release();
    EXPECT_EQ(externalWaiter.get(), "");
}

TEST(ExecutionStateDeathTest,
     BackendWorkerCannotDestroyActiveExecution) {
    EXPECT_DEATH(
        {
            auto state =
                std::make_shared<DestructionReentryState>();
            auto backend =
                std::make_unique<
                    DestructionReentryExecutable>(state);
            IBackendExecutable* root = backend.get();
            std::vector<
                std::unique_ptr<IBackendExecutable>>
                executables;
            executables.push_back(std::move(backend));
            auto execution = std::make_unique<Execution>(
                detail::ExecutionTestAccess::makeExecution(
                    std::move(executables), {root}));
            state->execution = &execution;
            execution->launch();
            std::this_thread::sleep_for(
                std::chrono::seconds(2));
        },
        "destruction from a backend worker");
}

TEST(ExecutionStateTest, DestructionJoinsRunningRoots) {
    GatedExecution fixture;
    auto execution = std::make_unique<Execution>(
        fixture.graph.compile());
    execution->writeScalar(fixture.size, std::uint64_t{1});
    execution->write(
        fixture.input, std::vector<std::int32_t>{11});
    execution->launch();
    ASSERT_TRUE(fixture.gate->waitUntilStarted(
        std::chrono::seconds(2)));

    std::promise<void> destroying;
    std::promise<void> destroyed;
    auto destroyingFuture = destroying.get_future();
    auto destroyedFuture = destroyed.get_future();
    std::thread destructor([&] {
        destroying.set_value();
        execution.reset();
        destroyed.set_value();
    });
    ASSERT_EQ(
        destroyingFuture.wait_for(std::chrono::seconds(1)),
        std::future_status::ready);
    EXPECT_EQ(
        destroyedFuture.wait_for(std::chrono::milliseconds(20)),
        std::future_status::timeout);

    fixture.gate->release();
    EXPECT_EQ(
        destroyedFuture.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);
    destructor.join();
}

TEST(ExecutionStateTest, CpuKernelScalarReentryRejectsWithoutDeadlock) {
    Graph graph = Graph::withDefaults();
    auto state = std::make_shared<ScalarReentryState>();
    auto kernel =
        graph.cpu().add<ScalarReentryKernel>(state);
    GraphScalar input =
        graph.scalarInput<std::uint32_t>("input");
    GraphScalar output =
        graph.outputScalar<std::uint32_t>("output");
    graph.addKernelCall({
        .kernel = kernel,
        .inputScalars = {{"in", input}},
        .outputScalars = {{"out", output}},
    });

    Execution execution = graph.compile();
    state->execution = &execution;
    state->input = input;
    execution.writeScalar(input, std::uint32_t{41});
    ASSERT_NO_THROW(execution.run());
    EXPECT_TRUE(state->rejected.load(
        std::memory_order_acquire));
    EXPECT_EQ(
        execution.readScalar<std::uint32_t>(output), 42u);
}
