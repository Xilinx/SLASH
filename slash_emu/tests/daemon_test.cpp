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
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "daemon.h"
#include "fixtures_paths.h"

namespace slash_emu {
namespace {

namespace fs = std::filesystem;
namespace tf = slash_emu::test_fixtures;

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

// run_daemon() is re-invocable in one process: reset_shutdown_state() closes the
// stale self-pipe and creates a fresh one each run (no fd leak, no stale wake).
TEST(DaemonTest, ReinvocableAcrossRuns) {
    for (int i = 0; i < 3; ++i) {
        int rc = run_with_trigger([]() { slash_emu::request_shutdown(); });
        EXPECT_EQ(0, rc) << "run " << i;
    }
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

// The config-taking overload brings accelerators up on startup (auto-RESCAN),
// serves them, and tears everything down cleanly on shutdown — leaving no
// leftover sockets or VBIN directories (cold reboot).
TEST(DaemonTest, RunWithConfigBringsUpAndTearsDown) {
    fs::path base = fs::temp_directory_path() /
                    ("slash_emu_daemon_test_" + std::to_string(::getpid()));
    fs::create_directories(base);

    std::string cfg_path = (base / "slash_emu.ini").string();
    { std::ofstream ofs(cfg_path, std::ios::trunc); ofs << "[device.0000:61:00]\n"; }

    DaemonConfig cfg;
    cfg.base_dir          = base.string();
    cfg.uid               = ::getuid();
    cfg.gid               = ::getgid();
    cfg.mode              = 0600;
    cfg.config_file       = cfg_path;
    cfg.default_vbin_path = std::string(tf::kDefaultModelVbin);
    cfg.accelerators.push_back(AcceleratorConfig{*BoardBdf::parse("0000:61:00"), std::nullopt});

    int result = -1;
    std::thread daemon_thread([&] { result = slash_emu::run_daemon(cfg); });

    // Wait until the hotplug socket + the accelerator's sockets are up.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    auto exists = [](const std::string& p) {
        struct stat st{};
        return ::stat(p.c_str(), &st) == 0;
    };
    while (std::chrono::steady_clock::now() < deadline &&
           !(exists((base / "slash_hotplug").string()) &&
             exists((base / "slash_ctl0").string()) &&
             exists((base / "slash_qdma_ctl0").string()))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(exists((base / "slash_hotplug").string()));
    EXPECT_TRUE(exists((base / "slash_ctl0").string()));
    EXPECT_TRUE(exists((base / "slash_qdma_ctl0").string()));

    slash_emu::request_shutdown();
    daemon_thread.join();
    EXPECT_EQ(0, result);

    // Everything torn down: no sockets, and the VBIN dir was cold-reboot-cleaned.
    EXPECT_FALSE(exists((base / "slash_hotplug").string()));
    EXPECT_FALSE(exists((base / "slash_ctl0").string()));
    EXPECT_FALSE(exists((base / "slash_qdma_ctl0").string()));
    EXPECT_FALSE(fs::exists(base / "0000:61:00"));

    std::error_code ec;
    fs::remove_all(base, ec);
}

// ═════════════════════════════════════════════════════════════════════════════
// ADVERSARY PROBES (Step 11) — self-pipe signal handling.
// ═════════════════════════════════════════════════════════════════════════════

// Count open fds via /proc/self/fd.
static int daemon_open_fd_count() {
    int count = 0;
    try {
        for (auto& e : fs::directory_iterator("/proc/self/fd")) { (void)e; ++count; }
    } catch (...) {}
    return count;
}

// ── Probe: re-invoking run_daemon() many times leaks no fds (the self-pipe is
//    recreated + closed each run — reset_shutdown_state closes the stale pipe). ──
TEST(DaemonTest, ReinvocationDoesNotLeakSelfPipeFds) {
    // Warm up once so any one-time statics are allocated before counting.
    (void)run_with_trigger([]() { slash_emu::request_shutdown(); });
    int before = daemon_open_fd_count();
    for (int i = 0; i < 30; ++i) {
        int rc = run_with_trigger([]() { slash_emu::request_shutdown(); });
        ASSERT_EQ(0, rc) << "run " << i;
    }
    int after = daemon_open_fd_count();
    EXPECT_LE(after, before + 2) << "self-pipe fds leaked: before=" << before
                                 << " after=" << after;
}

// ── Probe: a double shutdown request (storm) is idempotent — the CAS-once gate
//    writes at most one byte, the pipe never fills, and the daemon still exits 0. ─
TEST(DaemonTest, DoubleShutdownRequestIsIdempotent) {
    int rc = run_with_trigger([]() {
        for (int i = 0; i < 1000; ++i) slash_emu::request_shutdown();
    });
    EXPECT_EQ(0, rc);
    // A subsequent clean run still works (state was reset).
    EXPECT_EQ(0, run_with_trigger([]() { slash_emu::request_shutdown(); }));
}

// ── Probe: a shutdown requested BEFORE run_daemon() reaches reset_shutdown_state()
//    must not be silently lost.  This pins the "request racing the reset" ordering
//    the implementer dropped as "unreachable": request_shutdown() is a documented
//    thread-safe API (daemon.h) that may be called any time.  If the request
//    completes before reset_shutdown_state() clears `requested` and installs a
//    fresh empty pipe, the wake is lost and wait_for_shutdown() blocks forever.
//    The probe is watchdog-guarded so a defect surfaces as a FAILURE, not a suite
//    hang: if the daemon does not exit within the deadline, the wake was lost.
TEST(DaemonTest, ShutdownRequestedBeforeStartIsNotLost) {
    // Ensure a clean starting state (a prior run left `requested` false + a pipe).
    (void)run_with_trigger([]() { slash_emu::request_shutdown(); });

    std::atomic<int> result{-2};
    std::atomic<bool> done{false};
    std::thread th([&] {
        // Fire the shutdown request FIRST, fully ordered before run_daemon() begins
        // its reset_shutdown_state().  A correct implementation must still stop.
        slash_emu::request_shutdown();
        result.store(slash_emu::run_daemon());
        done.store(true);
    });

    // Watchdog: a lost wake manifests as run_daemon() never returning.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool exited = done.load();
    if (!exited) {
        // Rescue the hung daemon so the process can exit, then fail loudly.
        slash_emu::request_shutdown();
    }
    th.join();
    EXPECT_TRUE(exited)
        << "shutdown requested just before run_daemon() start was LOST "
           "(reset_shutdown_state cleared it) → wait_for_shutdown() hung";
    EXPECT_EQ(0, result.load());
}

// ── Probe: SIGINT and SIGTERM delivered together (storm of both) still exits 0. ─
TEST(DaemonTest, BothSignalsStormStillExitsCleanly) {
    int rc = run_with_trigger([]() {
        for (int i = 0; i < 100; ++i) { std::raise(SIGINT); std::raise(SIGTERM); }
    });
    EXPECT_EQ(0, rc);
}

} // namespace
} // namespace slash_emu
