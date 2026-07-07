// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//  and associated documentation files (the "Software"), to deal in the Software without
//  restriction, including without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ################################################################################################

#pragma once

#include "bar_memfd.h"
#include "system_map.h"
#include "transport.h"
#include "worker_controller.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace slash_sysemu {

class ModelClient;

// ─────────────────────────────────────────────────────────────────────────────
// ModelControlWorkers — the concrete WorkerController (Step 8)
// ─────────────────────────────────────────────────────────────────────────────
//
// Orchestrates the execution of the compute kernels described in the system map,
// plus the clock wizard (architecture: "Model control worker subsystem").  One
// worker thread per kernel drives the idle→busy→idle handshake between the
// user-region BAR memfd (where VRT writes parameters and ap_start via direct
// MMIO) and the model process (driven over the ZeroMQ ModelClient).  A single
// clock-wizard worker keeps the two clocking-wizard lock bits asserted so the
// vrtd clock driver never times out.
//
// Lifetime and ownership:
//   * The BAR memfds (user region, clock wizard) are owned elsewhere (Step 9/11)
//     and PERSIST across model-process restarts, so they are injected by
//     reference, not owned here.  The architecture requires the workers to run
//     "for the entire lifetime of the model process, independently of the BAR
//     subsystem" — the BAR subsystem may be REMOVE'd and restored while the
//     kernels keep running, hence the memfds outlive any single worker set.
//   * The ModelClient and SystemMap passed to start() are owned by the
//     ModelProcess and remain valid until the matching stop() (WorkerController
//     contract).
//
// This object is a WorkerController: Step 6's reconfiguration calls start() on a
// freshly launched model and stop() before tearing an old one down.  start()
// MUST NOT send the global `start` verb — the ModelProcess already issued that
// one-time sim-clock start as its launch probe (see worker_controller.h).

/**
 * @brief The lifetime state of a single compute kernel's worker.
 *
 * Tracked for the whole lifetime of the model process (architecture: "the daemon
 * never loses track of a kernel's state").  Exposed for tests via
 * kernel_state().
 */
enum class KernelState {
    Idle, /**< Polling the control register, waiting for ap_start (bit0). */
    Busy, /**< ap_start seen; polling the model for ap_done (bit1). */
    Dead, /**< The model was assumed dead (Transport error); the worker exited. */
};

/** @brief Human-readable name for a KernelState value. */
const char* kernel_state_name(KernelState s) noexcept;

class ModelControlWorkers : public WorkerController {
public:
    /** Control-register bit masks (architecture control-register-bits table). */
    static constexpr uint32_t kApStart = 0x1u; /**< bit0 */
    static constexpr uint32_t kApDone  = 0x2u; /**< bit1 */

    /** Clock-wizard lock-bit register offsets within BAR 4 (both windows). */
    static constexpr std::size_t kClockLockUserOffset    = 0x0033Cu; /**< user window REG4. */
    static constexpr std::size_t kClockLockServiceOffset = 0x1033Cu; /**< service window REG4. */
    static constexpr uint32_t    kClockLockBit           = 0x1u;     /**< the lock bit. */

    /** Default worker poll interval (short enough to feel responsive). */
    static constexpr std::chrono::milliseconds kDefaultPollInterval{1};

    /**
     * @brief Construct the worker controller bound to its BAR memfds.
     *
     * @param user_region   The 128 MiB user-region BAR memfd holding every
     *                      kernel's register window.  Shared by all kernel
     *                      workers (BarMemfd is internally thread-safe).
     * @param clock_wizard  The 512 KiB clock-wizard BAR memfd.
     * @param poll_interval Polling period for every worker loop.  Injectable so
     *                      tests can drive fast cycles.
     */
    ModelControlWorkers(BarMemfd&                 user_region,
                        BarMemfd&                 clock_wizard,
                        std::chrono::milliseconds poll_interval = kDefaultPollInterval);

    ModelControlWorkers(const ModelControlWorkers&)            = delete;
    ModelControlWorkers& operator=(const ModelControlWorkers&) = delete;

    /** The destructor stops and joins all workers (calls stop()). */
    ~ModelControlWorkers() override;

    // ── WorkerController interface ───────────────────────────────────────────

    /**
     * @brief Spawn one worker thread per kernel plus the clock-wizard worker.
     *
     * @p client and @p map must outlive the workers (until stop()).  Does NOT
     * send the global `start` verb.  Idempotent-ish: calling start() while
     * already running is a programming error and returns a Protocol error rather
     * than double-spawning.
     */
    Result<void> start(ModelClient& client, const SystemMap& map) override;

    /**
     * @brief Signal all workers to exit and join them.  Idempotent; safe if
     * start() never ran or already failed.  Also invoked by the destructor.
     */
    void stop() override;

    // ── Introspection (for tests) ────────────────────────────────────────────

    /** True while workers are running (between a successful start and stop). */
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    /**
     * @brief The current state of the worker for kernel index @p i.
     * @return the kernel's state, or KernelState::Dead if @p i is out of range.
     */
    [[nodiscard]] KernelState kernel_state(std::size_t i) const;

    /** Number of kernel workers (== number of kernels in the map). */
    [[nodiscard]] std::size_t kernel_count() const;

private:
    // Per-kernel worker context.  The state is an atomic so tests can observe it
    // without locking; the worker thread is the sole writer.
    struct KernelWorker {
        Kernel                   kernel;               // a copy of the kernel model
        std::size_t              control_offset = 0;   // base_address + 0
        bool                     has_control    = false;
        std::atomic<KernelState> state{KernelState::Idle};
        std::thread              thread;
    };

    void kernel_loop(KernelWorker& kw, ModelClient& client);
    void clock_loop();

    // Sleep for the poll interval, or wake early if a stop was requested.  Returns
    // false if a stop was requested (the caller should exit its loop).
    bool wait_or_stop();

    BarMemfd&                 user_region_;
    BarMemfd&                 clock_wizard_;
    std::chrono::milliseconds poll_interval_;

    std::atomic<bool>                          running_{false};
    std::atomic<bool>                          stop_requested_{false};

    // Guards the kernels_ VECTOR's lifetime (not the per-worker state, which is an
    // atomic written only by its own thread).  start()/stop() mutate the vector
    // (push_back / clear) under a UNIQUE lock; kernel_state()/kernel_count() read
    // it under a SHARED lock.  Without this, stop()'s kernels_.clear() frees the
    // KernelWorker objects while a concurrent introspection reader dereferences
    // kernels_[i] — a heap-use-after-free.  A reader/writer lock keeps the common
    // path (many concurrent state reads) cheap while making teardown safe.
    mutable std::shared_mutex                  kernels_mu_;
    std::vector<std::unique_ptr<KernelWorker>> kernels_;
    std::thread                                clock_thread_;

    // Interruptible sleep: workers wait on this cv for the poll interval; stop()
    // sets stop_requested_ and notifies so they wake immediately rather than
    // sleeping out a full interval on teardown.
    mutable std::mutex      sleep_mu_;
    std::condition_variable sleep_cv_;
};

} // namespace slash_sysemu
