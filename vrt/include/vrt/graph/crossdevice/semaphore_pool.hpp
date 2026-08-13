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
 * @file semaphore_pool.hpp
 * @brief SemaphorePool — thread-safe pool of binary semaphores for bridges.
 *
 * Bridges that need host-side cross-device synchronisation can use this
 * naive pool internally. Each bridge owns a private SemaphorePool and
 * allocates ids out of a private counter; semaphore ids are NOT shared
 * across bridges.
 *
 * `await()` busy-waits — this is intentionally simple. Bridges that need a
 * less wasteful primitive (condition variables, futexes, doorbells, HSA
 * signals, …) are expected to roll their own.
 */

#ifndef VRT_GRAPH_CROSSDEVICE_SEMAPHORE_POOL_HPP
#define VRT_GRAPH_CROSSDEVICE_SEMAPHORE_POOL_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace vrt::graph {

/**
 * @brief Opaque handle to a semaphore inside a SemaphorePool.
 *
 * Ids are unique only within the owning pool; do not mix handles across pools.
 */
struct SemaphoreHandle {
    uint32_t id;
};

class SemaphorePool {
   public:
    /**
     * @brief Allocate a fresh semaphore handle.
     */
    SemaphoreHandle allocate() {
        return SemaphoreHandle{nextId_.fetch_add(1, std::memory_order_relaxed)};
    }

    /**
     * @brief Release the semaphore at @p sem.id (set the flag).
     */
    void signal(SemaphoreHandle sem) {
        getFlag(sem.id).store(true, std::memory_order_release);
    }

    /**
     * @brief Block until the semaphore at @p sem.id has been signalled, then
     *        reset it for reuse.
     */
    void await(SemaphoreHandle sem) {
        auto& flag = getFlag(sem.id);
        while (!flag.load(std::memory_order_acquire)) {
            // busy-wait — naive but correct
        }
        flag.store(false, std::memory_order_relaxed);
    }

    /**
     * @brief Non-blocking poll. If the semaphore is signalled, atomically
     *        consume the signal and return `true`. Otherwise return `false`
     *        immediately.
     *
     * Race-free against a concurrent `signal()` via `compare_exchange_strong`:
     * exactly one caller observes a `true` per `signal`. Calling `tryAwait`
     * a second time after success returns `false` until another `signal`.
     */
    bool tryAwait(SemaphoreHandle sem) {
        auto& flag = getFlag(sem.id);
        bool expected = true;
        return flag.compare_exchange_strong(expected, false,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed);
    }

   private:
    std::atomic<bool>& getFlag(uint32_t id) {
        std::lock_guard<std::mutex> lk(mutex_);
        while (id >= flags_.size()) {
            flags_.emplace_back(std::make_unique<std::atomic<bool>>(false));
        }
        return *flags_[id];
    }

    std::mutex                                       mutex_;
    std::vector<std::unique_ptr<std::atomic<bool>>>  flags_;
    std::atomic<uint32_t>                            nextId_{0};
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CROSSDEVICE_SEMAPHORE_POOL_HPP
