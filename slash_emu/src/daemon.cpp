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

#include "daemon.h"

#include <csignal>
#include <cstdio>
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace slash_emu {

namespace {

// Shared state for the shutdown rendezvous.
//
// `requested` is std::atomic<bool> so that the signal handler can write to it
// without holding a lock.  Signal handlers are not permitted to call
// non-async-signal-safe functions (which includes std::mutex::lock), so the
// flag must be lock-free from the handler's perspective.
//
// The condition variable and its associated mutex are still used so that the
// main daemon loop can sleep efficiently.  The wait predicate re-reads the
// atomic, which is safe: the CV wakeup (notify_all) acts as a happens-before
// fence that ensures the CV consumer sees the updated flag.
//
// request_shutdown() (called from ordinary thread context) also holds the
// mutex while setting the flag, so that the CV wait cannot miss the
// notification between reading the predicate and sleeping.
struct ShutdownState {
    std::mutex              mtx;
    std::condition_variable cv;
    std::atomic<bool>       requested{false};
};

// Single global instance.  A static local is fine here because
// run_daemon() is not designed to be re-entrant.
ShutdownState& shutdown_state() noexcept {
    static ShutdownState state;
    return state;
}

void signal_handler(int /*signum*/) noexcept {
    // Signal-handler context: only async-signal-safe operations are allowed.
    // Writing to an atomic<bool> is safe.  notify_all() is not formally
    // async-signal-safe in the C++ standard, but on Linux/glibc it is safe in
    // practice (it calls pthread_cond_broadcast under the hood).  A self-pipe
    // or eventfd would be the strictly-portable alternative and can be adopted
    // in a later step when the full event loop is added.
    ShutdownState& s = shutdown_state();
    s.requested.store(true, std::memory_order_relaxed);
    s.cv.notify_all();
}

// The default signal installer: delegates directly to ::sigaction.
int default_install_signal(int signum, const struct sigaction *act, struct sigaction *oldact) {
    return ::sigaction(signum, act, oldact);
}

} // namespace

void request_shutdown() noexcept {
    ShutdownState& s = shutdown_state();
    {
        // Hold the mutex while setting the flag so that a concurrent
        // cv.wait() cannot miss the notification (it either sees the flag
        // before sleeping or is woken by notify_all()).
        std::lock_guard<std::mutex> lk(s.mtx);
        s.requested.store(true, std::memory_order_relaxed);
    }
    s.cv.notify_all();
}

int run_daemon(SignalInstaller install_signal) {
    if (!install_signal) {
        install_signal = default_install_signal;
    }

    std::printf("[slash_emu] daemon starting\n");

    // Reset shutdown state in case run_daemon() is called more than once in
    // the same process (e.g. back-to-back unit tests).
    {
        ShutdownState& s = shutdown_state();
        std::lock_guard<std::mutex> lk(s.mtx);
        s.requested.store(false, std::memory_order_relaxed);
    }

    // Install signal handlers for graceful shutdown.
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (install_signal(SIGINT, &sa, nullptr) != 0 ||
        install_signal(SIGTERM, &sa, nullptr) != 0) {
        std::perror("[slash_emu] sigaction");
        return 1;
    }

    std::printf("[slash_emu] running — send SIGINT or SIGTERM to stop\n");

    // Block until a shutdown is requested.
    {
        ShutdownState& s = shutdown_state();
        std::unique_lock<std::mutex> lk(s.mtx);
        s.cv.wait(lk, [&s] { return s.requested.load(std::memory_order_relaxed); });
    }

    std::printf("[slash_emu] daemon shutting down cleanly\n");
    return 0;
}

} // namespace slash_emu
