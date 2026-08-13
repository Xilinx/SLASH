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
 * @file execution.hpp
 * @brief Move-only executable graph owning direct backend queue programs.
 */

#ifndef VRT_GRAPH_EXECUTION_HPP
#define VRT_GRAPH_EXECUTION_HPP

#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <vrt/graph/backend_resource_binding.hpp>
#include <vrt/graph/backend_runtime.hpp>
#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/detail/executable_assembler.hpp>

namespace vrt::graph {

class IBridge;

namespace detail {
class ExecutionTestAccess;
}

/*
 * An Execution is the lifetime boundary for one lowered graph instance.
 * Root pointers borrow from executables_; backend objects in turn borrow
 * bridge, resource, and device state pinned by later members. Destruction
 * therefore joins all work before reverse member destruction begins.
 */
class Execution {
   private:
    friend class Graph;
    friend class detail::ExecutionTestAccess;

    Execution(
        std::shared_ptr<BackendRuntimeState> runtimeState,
        detail::ExecutionIoMetadata io,
        BackendResourceBindings resources,
        std::vector<std::shared_ptr<IBridge>> bridgePins,
        std::vector<std::shared_ptr<IDevice>> devicePins,
        std::vector<std::unique_ptr<IBackendExecutable>> executables,
        std::vector<IBackendExecutable*> roots)
        : runtimeState_(std::move(runtimeState)),
          scalarValues_(runtimeState_->scalarValues()),
          io_(std::move(io)),
          devicePins_(std::move(devicePins)),
          resources_(std::move(resources)),
          bridgePins_(std::move(bridgePins)),
          executables_(std::move(executables)),
          roots_(std::move(roots)) {
        for (const auto& device : devicePins_) {
            auto cpu = std::dynamic_pointer_cast<CpuDevice>(device);
            if (cpu) {
                cpuDevice_ = std::move(cpu);
                break;
            }
        }
    }

   public:
    Execution(const Execution&) = delete;
    Execution& operator=(const Execution&) = delete;
    Execution(Execution&&) noexcept = default;
    Execution& operator=(Execution&&) = delete;
    ~Execution() {
        /*
         * A backend worker cannot join itself, so fail closed instead of
         * tearing down storage still in use. Every other path joins first,
         * including stack unwinding after a backend error.
         */
        if (detail::BackendWorkerScope::active() &&
            hasIncompleteRun()) {
            std::fputs(
                "Execution::~Execution: destruction from a backend worker "
                "is not allowed\n",
                stderr);
            std::fflush(stderr);
            std::terminate();
        }
        waitNoThrow();
    }

    template <class T>
    void writeScalar(const GraphScalar& scalar, T value) {
        static_assert(
            std::is_arithmetic_v<T>,
            "Execution::writeScalar supports arithmetic scalar values");
        HostIoGuard hostIo(state_, "Execution::writeScalar");
        requireScalarType<T>(scalar);
        const std::string key = scalarKey(scalar);
        requireMember(io_.inputScalars, key, "scalar", "input");
        std::lock_guard<std::mutex> lock(runtimeState_->scalarMutex());
        (*scalarValues_)[key] = detail::valueToBits(value);
    }

    template <class T>
    T readScalar(const GraphScalar& scalar) const {
        static_assert(
            std::is_arithmetic_v<T>,
            "Execution::readScalar supports arithmetic scalar values");
        HostIoGuard hostIo(state_, "Execution::readScalar");
        requireScalarType<T>(scalar);
        const std::string key = scalarKey(scalar);
        requireMember(io_.outputScalars, key, "scalar", "output");
        std::lock_guard<std::mutex> lock(runtimeState_->scalarMutex());
        auto value = scalarValues_->find(key);
        if (value == scalarValues_->end()) {
            throw std::out_of_range(
                "Execution::read: scalar has no value");
        }
        T result{};
        std::memcpy(&result, &value->second, sizeof(T));
        return result;
    }

    void write(
        const GraphBuffer& token, const void* data,
        std::size_t bytes) {
        HostIoGuard hostIo(state_, "Execution::write");
        validateBufferByteCount(
            token, bytes, "Execution::write");
        if (bytes != 0 && data == nullptr) {
            throw std::invalid_argument(
                "Execution::write: data must not be null");
        }
        const std::string key = bufferKey(token);
        requireMember(io_.inputBuffers, key, "buffer", "input");
        requireCpuDevice("Execution::write");
        cpuDevice_->setInputBuffer(key, data, bytes);
    }

    template <class T>
    void write(
        const GraphBuffer& token,
        const std::vector<T>& data) {
        requireBufferType<T>(token);
        write(token, data.data(), data.size() * sizeof(T));
    }

    void read(
        const GraphBuffer& token, void* data,
        std::size_t bytes) const {
        HostIoGuard hostIo(state_, "Execution::read");
        validateBufferByteCount(
            token, bytes, "Execution::read");
        if (bytes != 0 && data == nullptr) {
            throw std::invalid_argument(
                "Execution::read: data must not be null");
        }
        const std::string key = bufferKey(token);
        requireMember(io_.outputBuffers, key, "buffer", "output");
        requireCpuDevice("Execution::read");
        cpuDevice_->getOutputBuffer(key, data, bytes);
    }

    template <class T>
    void read(
        const GraphBuffer& token,
        std::vector<T>& data) const {
        requireBufferType<T>(token);
        read(token, data.data(), data.size() * sizeof(T));
    }

    /*
     * Launch has three outcomes:
     * - Idle becomes Launching only after host I/O has drained;
     * - a prepare/launch failure joins every root and publishes completion;
     * - successful submission becomes Running for a waiter to join.
     *
     * Backend calls stay outside the state lock because they may block.
     */
    void launch() {
        const std::shared_ptr<State> state = requireState();
        std::shared_ptr<Completion> completion;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->phase != Phase::Idle) {
                throw std::runtime_error(
                    "Execution::launch: execution is already active");
            }
            if (state->hostIoCount != 0) {
                throw std::runtime_error(
                    "Execution::launch: host I/O is in progress");
            }
            completion = std::make_shared<Completion>();
            state->completion = completion;
            state->phase = Phase::Launching;
        }

        std::exception_ptr launchError;
        try {
            requireAllSizesSet();
            for (IBackendExecutable* executable : roots_) {
                executable->prepareLaunch();
            }
            for (IBackendExecutable* executable : roots_) {
                executable->launch();
            }
        } catch (...) {
            launchError = std::current_exception();
        }

        if (launchError) {
            (void)waitRoots();
            publishCompletion(state, completion, launchError);
            std::rethrow_exception(launchError);
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->phase = Phase::Running;
        }
        state->changed.notify_all();
    }

    /*
     * Waiters have four cases:
     * - no completion means no run has been launched;
     * - Launching waits until launch either fails or reaches Running;
     * - the first Running waiter becomes the sole Joining thread;
     * - later waiters sleep until that thread publishes ready.
     * Every waiter observes the same saved backend exception.
     */
    void wait() {
        const std::shared_ptr<State> state = requireState();
        std::shared_ptr<Completion> completion;
        bool join = false;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            completion = state->completion;
            if (!completion) return;
            if (!completion->ready &&
                detail::BackendWorkerScope::active()) {
                throw std::logic_error(
                    "Execution::wait: reentrant wait from a backend worker "
                    "is not allowed");
            }

            while (!completion->ready &&
                   state->phase == Phase::Launching) {
                state->changed.wait(lock);
            }
            if (!completion->ready &&
                state->phase == Phase::Running) {
                state->phase = Phase::Joining;
                join = true;
            } else if (!completion->ready) {
                state->changed.wait(
                    lock, [&] { return completion->ready; });
            }
        }

        if (join) {
            publishCompletion(
                state, completion, waitRoots());
        }

        std::exception_ptr error;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            error = completion->error;
        }
        if (error) std::rethrow_exception(error);
    }

    void run() {
        launch();
        wait();
    }

   private:
    /*
     * The only forward transitions are Idle -> Launching -> Running ->
     * Joining -> Idle. Launch failure skips Running/Joining but still
     * publishes ready before returning to Idle. completion identifies the
     * run whose result concurrent waiters must observe.
     */
    enum class Phase {
        Idle,
        Launching,
        Running,
        Joining,
    };

    struct Completion {
        bool ready = false;
        std::exception_ptr error;
    };

    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        Phase phase = Phase::Idle;
        std::size_t hostIoCount = 0;
        std::shared_ptr<Completion> completion;
    };

    /*
     * Host I/O is a counted lease on the Idle phase. The shared State pin
     * keeps the decrement valid through exceptions, while launch checks the
     * count under the same lock so inputs cannot change during submission.
     */
    class HostIoGuard {
       public:
        HostIoGuard(
            const std::shared_ptr<State>& state,
            const char* method)
            : state_(state) {
            if (!state_) {
                throw std::logic_error(
                    std::string(method) +
                    ": execution was moved from");
            }
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->phase != Phase::Idle) {
                throw std::runtime_error(
                    std::string(method) +
                    ": execution is active");
            }
            ++state_->hostIoCount;
        }

        HostIoGuard(const HostIoGuard&) = delete;
        HostIoGuard& operator=(const HostIoGuard&) = delete;

        ~HostIoGuard() {
            if (!state_) return;
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                --state_->hostIoCount;
            }
            state_->changed.notify_all();
        }

       private:
        std::shared_ptr<State> state_;
    };

    std::shared_ptr<State> requireState() const {
        if (!state_) {
            throw std::logic_error(
                "Execution: operation on moved-from execution");
        }
        return state_;
    }

    bool hasIncompleteRun() const {
        if (!state_) return false;
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->completion &&
               !state_->completion->ready;
    }

    /*
     * Joining is best-effort across all roots: the first failure is retained,
     * but every root is waited so no sibling worker survives into resource
     * or device teardown.
     */
    std::exception_ptr waitRoots() noexcept {
        std::exception_ptr firstError;
        for (IBackendExecutable* executable : roots_) {
            try {
                executable->wait();
            } catch (...) {
                if (!firstError) {
                    firstError = std::current_exception();
                }
            }
        }
        return firstError;
    }

    static void publishCompletion(
        const std::shared_ptr<State>& state,
        const std::shared_ptr<Completion>& completion,
        std::exception_ptr error) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            completion->error = std::move(error);
            completion->ready = true;
            if (state->completion == completion) {
                state->phase = Phase::Idle;
            }
        }
        state->changed.notify_all();
    }

    void waitNoThrow() noexcept {
        if (!state_) return;
        try {
            wait();
        } catch (...) {
        }
    }

    static std::string scalarKey(const GraphScalar& scalar) {
        return scopedScalarKey(
            scalar.scopeId(), scalar.varName());
    }

    static std::string bufferKey(const GraphBuffer& buffer) {
        return scopedBufferKey(buffer.scopeId(), buffer.name());
    }

    static void requireMember(
        const std::set<std::string>& values,
        const std::string& key,
        const char* kind, const char* direction) {
        if (values.count(key) == 0) {
            throw std::runtime_error(
                "Execution: " + std::string(kind) + " '" + key +
                "' is not a graph " + direction);
        }
    }

    template <class T>
    static void requireScalarType(const GraphScalar& scalar) {
        if (scalar.type() != typeToScalarType<T>()) {
            throw std::invalid_argument(
                "Execution: scalar handle type mismatch");
        }
    }

    template <class T>
    static void requireBufferType(const GraphBuffer& buffer) {
        if (buffer.type() != typeToBufferType<T>()) {
            throw std::invalid_argument(
                "Execution: buffer handle type mismatch");
        }
    }

    void validateBufferByteCount(
        const GraphBuffer& buffer,
        std::size_t bytes,
        const char* method) const {
        std::lock_guard<std::mutex> lock(
            runtimeState_->scalarMutex());
        const std::size_t expected =
            resolvedBufferSizeBytes(buffer, scalarValues_, method);
        if (bytes != expected) {
            throw std::invalid_argument(
                std::string(method) + ": expected " +
                std::to_string(expected) + " byte(s), got " +
                std::to_string(bytes));
        }
    }

    void requireAllSizesSet() const {
        std::vector<std::string> missing;
        {
            std::lock_guard<std::mutex> lock(
                runtimeState_->scalarMutex());
            for (const auto& [key, name] : io_.sizeScalars) {
                if (scalarValues_->count(key) == 0) {
                    missing.push_back(name);
                }
            }
        }
        if (missing.empty()) return;
        std::ostringstream message;
        message << "Execution::launch: unset size scalar";
        if (missing.size() != 1) message << "s";
        message << ": ";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i != 0) message << ", ";
            message << missing[i];
        }
        throw std::runtime_error(message.str());
    }

    void requireCpuDevice(const char* method) const {
        if (!cpuDevice_) {
            throw std::runtime_error(
                std::string(method) +
                ": execution has no graph-I/O CPU device");
        }
    }

    /*
     * Reverse destruction order is deliberate: borrowed roots and owned
     * executables disappear before bridge instances, resource leases, and
     * devices. resources_ releases its leases while devicePins_ still keeps
     * every lease owner alive; runtime state outlives all backend objects.
     */
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::shared_ptr<BackendRuntimeState> runtimeState_;
    std::shared_ptr<std::map<std::string, std::uint64_t>>
        scalarValues_;
    detail::ExecutionIoMetadata io_;
    std::vector<std::shared_ptr<IDevice>> devicePins_;
    BackendResourceBindings resources_;
    std::vector<std::shared_ptr<IBridge>> bridgePins_;
    std::shared_ptr<CpuDevice> cpuDevice_;
    std::vector<std::unique_ptr<IBackendExecutable>> executables_;
    std::vector<IBackendExecutable*> roots_;
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_EXECUTION_HPP
