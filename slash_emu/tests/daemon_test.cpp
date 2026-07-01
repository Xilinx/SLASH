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

#include <gtest/gtest.h>
#include <csignal>
#include <cerrno>
#include <thread>
#include <chrono>

#include "daemon.h"

namespace slash_emu {
namespace {

// Helper: start run_daemon() on a background thread, invoke `trigger` after a
// short delay, join, and return the daemon's exit code.
template <typename Fn>
int run_with_trigger(Fn trigger) {
    int result = -1;
    std::thread daemon_thread([&result]() {
        result = slash_emu::run_daemon();
    });
    // Give the daemon thread time to reach its wait point.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    trigger();
    daemon_thread.join();
    return result;
}

// --- Tests ---

// The daemon exits cleanly (returns 0) when request_shutdown() is called
// directly from another thread.  This is the primary testability hook.
TEST(DaemonTest, ShutdownViaRequestShutdown) {
    int rc = run_with_trigger([]() { slash_emu::request_shutdown(); });
    EXPECT_EQ(rc, 0);
}

// The daemon exits cleanly when SIGTERM is delivered to the process.
// std::raise() delivers the signal to the calling thread; the signal handler
// sets the flag and notifies the condition variable so the daemon thread wakes.
TEST(DaemonTest, ShutdownViaSigterm) {
    int rc = run_with_trigger([]() { std::raise(SIGTERM); });
    EXPECT_EQ(rc, 0);
}

// The daemon exits cleanly when SIGINT is delivered to the process.
TEST(DaemonTest, ShutdownViaSigint) {
    int rc = run_with_trigger([]() { std::raise(SIGINT); });
    EXPECT_EQ(rc, 0);
}

// The daemon returns non-zero when the signal installer fails.
//
// We inject a stub installer that always returns -1 (simulating a failed
// sigaction call, e.g. on an invalid or uncatchable signal number).  The
// daemon must detect the failure and return non-zero without hanging.
TEST(DaemonTest, SignalInstallFailureReturnsNonZero) {
    auto failing_installer = [](int /*signum*/, const struct sigaction * /*act*/,
                                struct sigaction * /*oldact*/) -> int {
        errno = EINVAL;
        return -1;
    };
    int rc = slash_emu::run_daemon(failing_installer);
    EXPECT_NE(rc, 0);
}

} // namespace
} // namespace slash_emu
