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

#include "model_process.h"

#include "fixtures_paths.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

using namespace slash_emu;
using namespace std::chrono_literals;
namespace tf = slash_emu::test_fixtures;

namespace {

// Short timeouts so failure-path tests do not wait the 10s production default.
ModelProcessTimeouts fast_timeouts() {
    ModelProcessTimeouts t;
    t.request   = 500ms;
    t.exit_wait = 500ms;
    t.term_wait = 500ms;
    return t;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Successful launch
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelProcess, LaunchesDefaultModelAndClientWorks) {
    auto r = ModelProcess::launch(tf::kDefaultModelVbin, {}, fast_timeouts());
    ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
    auto& mp = *r.value();

    EXPECT_TRUE(mp.running());
    EXPECT_GT(mp.pid(), 0);
    EXPECT_EQ(mp.endpoint().rfind("ipc://", 0), 0u);

    // The one-time global start was already sent as the launch probe; the client
    // is live and further verbs work.
    EXPECT_TRUE(mp.client().reg_write(0x10, 0xabcd).has_value());
    auto sc = mp.client().fetch_scalar(0x10);
    ASSERT_TRUE(sc.has_value());
    EXPECT_EQ(sc.value(), 0xabcdu);
}

TEST(ModelProcess, ExposesParsedSystemMap) {
    auto r = ModelProcess::launch(tf::kDefaultModelVbin, {}, fast_timeouts());
    ASSERT_TRUE(r.has_value());
    const SystemMap& map = r.value()->system_map();
    EXPECT_EQ(map.platform, Platform::Simulation);
    // The default fixture map is 100 MHz with kernel "defaultk".
    EXPECT_EQ(map.clock_frequency_hz, 100000000u);
    ASSERT_FALSE(map.kernels.empty());
    EXPECT_EQ(map.kernels.front().name, "defaultk");
}

TEST(ModelProcess, StagingMapDistinguishableFromDefault) {
    auto r = ModelProcess::launch(tf::kStagingGoodVbin, {}, fast_timeouts());
    ASSERT_TRUE(r.has_value());
    const SystemMap& map = r.value()->system_map();
    EXPECT_EQ(map.clock_frequency_hz, 250000000u);
    ASSERT_FALSE(map.kernels.empty());
    EXPECT_EQ(map.kernels.front().name, "stagingk");
}

// ─────────────────────────────────────────────────────────────────────────────
// Teardown
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelProcess, IntentionalTeardownDoesNotFireDeathCallback) {
    std::atomic<int> deaths{0};
    {
        auto r = ModelProcess::launch(
            tf::kDefaultModelVbin, [&] { ++deaths; }, fast_timeouts());
        ASSERT_TRUE(r.has_value());
        // Explicit teardown: the fake model handles the `exit` verb and quits,
        // so this exercises the graceful path.
        r.value()->teardown();
        EXPECT_FALSE(r.value()->running());
    }
    // Give any (erroneous) callback a chance to run.
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(deaths.load(), 0);
}

TEST(ModelProcess, TeardownIsIdempotent) {
    auto r = ModelProcess::launch(tf::kDefaultModelVbin, {}, fast_timeouts());
    ASSERT_TRUE(r.has_value());
    r.value()->teardown();
    r.value()->teardown(); // second call must be a no-op, not a crash/double-reap
    SUCCEED();
}

TEST(ModelProcess, DestructorTearsDownCleanly) {
    std::atomic<int> deaths{0};
    {
        auto r = ModelProcess::launch(
            tf::kDefaultModelVbin, [&] { ++deaths; }, fast_timeouts());
        ASSERT_TRUE(r.has_value());
        // No explicit teardown: destructor must reap and not fire the callback.
    }
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(deaths.load(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Death detection
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelProcess, UnexpectedExitFiresDeathCallbackExactlyOnce) {
    std::mutex              mu;
    std::condition_variable cv;
    int                     deaths = 0;

    // Launch a model that serves normally, then exits on its own after 150ms.
    ModelProcessTimeouts t = fast_timeouts();
    auto r = ModelProcess::launch(
        tf::kDefaultModelVbin,
        [&] {
            std::lock_guard<std::mutex> g(mu);
            ++deaths;
            cv.notify_all();
        },
        t);
    ASSERT_TRUE(r.has_value());
    auto& mp = *r.value();

    // Ask the model to exit out-of-band (send the exit verb ourselves WITHOUT
    // marking teardown) to simulate an unexpected death.
    (void)mp.client().exit();

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 3s, [&] { return deaths >= 1; }))
            << "death callback did not fire";
    }
    // Ensure it fired exactly once (no double-fire from the monitor).
    std::this_thread::sleep_for(150ms);
    {
        std::lock_guard<std::mutex> g(mu);
        EXPECT_EQ(deaths, 1);
    }
    EXPECT_FALSE(mp.running());
    // The client now reports Transport (dead model).
    EXPECT_EQ(mp.client().start().error().kind, ErrorKind::Transport);
}

TEST(ModelProcess, HardKillMidRunFiresDeathCallback) {
    // Models a model process that crashes/dies abruptly mid-run (SIGKILL from
    // outside, no `exit` verb): death detection must fire the callback once.
    std::atomic<int> deaths{0};
    auto r = ModelProcess::launch(
        tf::kDefaultModelVbin, [&] { ++deaths; }, fast_timeouts());
    ASSERT_TRUE(r.has_value());
    ::kill(r.value()->pid(), SIGKILL); // hard kill: unexpected death
    for (int i = 0; i < 300 && deaths.load() == 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(deaths.load(), 1);
    EXPECT_FALSE(r.value()->running());
}

// Re-entrancy safety net: calling teardown() synchronously from inside on_death
// (which runs on the monitor thread) must NOT self-join → deadlock/hang.  The
// contract forbids this (Step 11 must dispatch off-thread), but teardown() must
// degrade to safe behavior (detach) rather than hang.  A watchdog thread bounds
// the test so a regression shows as a failure, not an indefinite hang.
TEST(ModelProcess, TeardownFromInsideDeathCallbackDoesNotHang) {
    std::atomic<bool> done{false};
    std::atomic<ModelProcess*> self_ptr{nullptr};
    std::thread worker([&] {
        auto r = ModelProcess::launch(
            tf::kDefaultModelVbin,
            [&] {
                // Forbidden-but-must-not-hang: tear down from within the callback.
                ModelProcess* p = self_ptr.load();
                if (p != nullptr) {
                    p->teardown();
                }
            },
            fast_timeouts());
        ASSERT_TRUE(r.has_value());
        self_ptr.store(r.value().get());
        // Trigger an unexpected death so the callback (which calls teardown) runs
        // on the monitor thread.
        ::kill(r.value()->pid(), SIGKILL);
        // Give the monitor time to reap + run the callback.
        std::this_thread::sleep_for(500ms);
        // Destroying r here also calls teardown() (call_once → no-op); must be
        // safe with the monitor detached.
        done.store(true);
    });

    // Watchdog: the whole thing must finish well within a few seconds.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(done.load()) << "teardown() from inside on_death hung (self-join)";
    worker.join();
}

// PROBE G1 — the DANGEROUS re-entrancy case the detach guard must survive:
// DESTROY the ModelProcess synchronously from inside on_death, on the monitor
// thread.  This runs ~ModelProcess (→ teardown() detach path → member
// destruction, including destroying on_death_ itself) WHILE the std::function
// invocation of on_death_ is still on the monitor stack.  Hammered in a loop
// under ASan/UBSan this exposes any use-after-free of members read by the
// detached monitor after on_death_() returns, or of the callable being
// destroyed mid-call.
// This probe isolates the GUARD/monitor path: the callback calls teardown()
// (detach path) on the monitor thread, signals completion via an EXTERNAL atomic
// that does NOT live inside the callback, and then a SEPARATE thread destroys the
// ModelProcess — maximizing the race between ~ModelProcess freeing members and
// the detached monitor thread unwinding after on_death_() returns.  Any member
// read by monitor_loop after on_death_() (mutex/condvar/atomics/reap_) once the
// object is freed would be caught by ASan here.
TEST(ModelProcess, TeardownInsideCallbackThenExternalDestroyRacesMonitorUnwind) {
    for (int iter = 0; iter < 30; ++iter) {
        // `entered` and `holder` are heap objects shared with the test; the
        // callback touches ONLY these, never a capture that ~ModelProcess frees,
        // so any ASan hit is a genuine monitor/guard member UAF — not the callback
        // reading its own freed captures.
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto holder  = std::make_shared<ModelProcess*>(nullptr);

        auto r = ModelProcess::launch(
            tf::kDefaultModelVbin,
            [entered, holder] {
                ModelProcess* p = *holder;
                if (p) p->teardown();  // detach path: teardown() on the monitor thread
                entered->store(true);  // signal via external heap flag
            },
            fast_timeouts());
        ASSERT_TRUE(r.has_value());
        *holder = r.value().get();

        ::kill(r.value()->pid(), SIGKILL); // unexpected death → callback on monitor thread

        for (int i = 0; i < 300 && !entered->load(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
        EXPECT_TRUE(entered->load()) << "callback did not run";

        // Destroy on the MAIN thread, racing the detached monitor's unwind after
        // on_death_() returned.  ~ModelProcess frees reap_/mutex/atomics; if the
        // detached monitor reads any of them post-return, ASan fires here.
        r.value().reset();
        EXPECT_FALSE(r.value());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Launch failures
// ─────────────────────────────────────────────────────────────────────────────

TEST(ModelProcess, CorruptVbinFailsToUnpack) {
    auto r = ModelProcess::launch(tf::kCorruptVbin, {}, fast_timeouts());
    ASSERT_FALSE(r.has_value());
    // Not an Io launch error — a container/parse failure from unpack_vbin.
    EXPECT_NE(r.error().kind, VbinErrorKind::Io);
}

TEST(ModelProcess, UnlaunchableModelExitsImmediatelyIsError) {
    auto r = ModelProcess::launch(tf::kUnlaunchableVbin, {}, fast_timeouts());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, VbinErrorKind::Io);
    // No zombie: nothing to assert directly, but ASan/leak checks + the reaper
    // guarantee the short-lived child was collected.
}

TEST(ModelProcess, HangingModelProbeTimesOutIsError) {
    auto r = ModelProcess::launch(tf::kHangModelVbin, {}, fast_timeouts());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, VbinErrorKind::Io);
}

TEST(ModelProcess, MissingVbinFileIsError) {
    auto r = ModelProcess::launch("/nonexistent/path/to.vbin", {}, fast_timeouts());
    ASSERT_FALSE(r.has_value());
}

// ── ADVERSARY PROBES (Step 6) ────────────────────────────────────────────────

// PROBE P1 — AF_UNIX sun_path length limit.
// sun_path is ~108 bytes.  The endpoint is
// ipc://<TMPDIR>/slash_emu_vbin_XXXXXX/zmq.socket.  Under a long TMPDIR the path
// exceeds the limit; the model's zmq_bind() then fails, so it never serves.  The
// launch MUST fail cleanly (Io error) via the probe timeout, NOT hang and NOT
// crash.  This probe sets a deliberately long TMPDIR and asserts a bounded, clean
// failure — and that a subsequent normal launch (default TMPDIR) still works.
TEST(ModelProcess, LongTmpdirEndpointFailsCleanlyNotHang) {
    const char* saved = ::getenv("TMPDIR");
    std::string saved_str = saved ? saved : "";

    // Build a long but VALID, existing temp root: enough that
    // <root>/slash_emu_vbin_XXXXXX/zmq.socket exceeds 107 chars.
    std::filesystem::path base = std::filesystem::temp_directory_path();
    std::string longseg(90, 'd');
    std::filesystem::path longroot = base / longseg;
    std::error_code ec;
    std::filesystem::create_directories(longroot, ec);
    ASSERT_FALSE(ec) << ec.message();

    ::setenv("TMPDIR", longroot.c_str(), 1);

    auto t0 = std::chrono::steady_clock::now();
    auto r  = ModelProcess::launch(tf::kDefaultModelVbin, {}, fast_timeouts());
    auto dt = std::chrono::steady_clock::now() - t0;

    // Restore TMPDIR before assertions.
    if (saved) {
        ::setenv("TMPDIR", saved_str.c_str(), 1);
    } else {
        ::unsetenv("TMPDIR");
    }
    std::filesystem::remove_all(longroot, ec);

    // The path (~128 bytes) exceeds the AF_UNIX sun_path limit (108), so the
    // launcher's early guard MUST reject it with a specific, clean Io error —
    // NOT hang waiting on the probe timeout, and NOT rely on the model's bind
    // failing.  Bound the time well under the probe timeout to prove the guard
    // short-circuited rather than the probe timing out.
    ASSERT_FALSE(r.has_value()) << "over-long ipc path unexpectedly launched";
    EXPECT_EQ(r.error().kind, VbinErrorKind::Io);
    EXPECT_NE(r.error().message.find("too long for AF_UNIX"), std::string::npos)
        << "expected the sun_path guard message, got: " << r.error().message;
    // The guard fires before any spawn/probe, so this returns near-instantly.
    EXPECT_LT(dt, 1s) << "launch did not short-circuit on the over-long path";

    // A subsequent normal launch (default TMPDIR restored) still works — proves
    // the env manipulation left no lasting damage.
    auto r2 = ModelProcess::launch(tf::kDefaultModelVbin, {}, fast_timeouts());
    EXPECT_TRUE(r2.has_value()) << (r2.has_value() ? "" : r2.error().message);
}

// PROBE P2 — no zombie / fd leak over many launch+teardown cycles.  Reaping and
// fd hygiene must hold across churn (Step 11 will reconfigure repeatedly).
TEST(ModelProcess, ManyLaunchTeardownCyclesNoLeak) {
    auto count_fds = [] {
        int n = 0;
        DIR* d = ::opendir("/proc/self/fd");
        if (!d) return -1;
        while (::readdir(d)) ++n;
        ::closedir(d);
        return n;
    };
    // Warm-up.
    for (int i = 0; i < 3; ++i) {
        auto r = ModelProcess::launch(tf::kDefaultModelVbin, {}, fast_timeouts());
        ASSERT_TRUE(r.has_value());
        r.value()->teardown();
    }
    int before = count_fds();
    for (int i = 0; i < 25; ++i) {
        auto r = ModelProcess::launch(tf::kDefaultModelVbin, {}, fast_timeouts());
        ASSERT_TRUE(r.has_value());
        r.value()->teardown();
    }
    int after = count_fds();
    ASSERT_GE(before, 0);
    EXPECT_LT(after - before, 20) << "fd leak: before=" << before << " after=" << after;
    // No zombies: reap any lingering child non-blockingly; there should be none.
    int reaped = 0;
    while (::waitpid(-1, nullptr, WNOHANG) > 0) ++reaped;
    EXPECT_EQ(reaped, 0) << "found " << reaped << " unreaped zombie children";
}

// PROBE P3 — teardown() concurrent with a spontaneous natural death: the death
// callback must fire AT MOST once and teardown must not deadlock or double-reap.
// We race an out-of-band exit with teardown() and assert coherence.
TEST(ModelProcess, TeardownRacingNaturalDeathIsSafe) {
    for (int iter = 0; iter < 10; ++iter) {
        std::atomic<int> deaths{0};
        auto r = ModelProcess::launch(
            tf::kDefaultModelVbin, [&] { ++deaths; }, fast_timeouts());
        ASSERT_TRUE(r.has_value());
        auto& mp = *r.value();
        pid_t pid = mp.pid();

        // Fire an async hard-kill and a teardown as close together as possible.
        std::thread killer([pid] { ::kill(pid, SIGKILL); });
        mp.teardown(); // marks intentional; races the kill-induced death
        killer.join();

        EXPECT_FALSE(mp.running());
        // If teardown won the race, intentional suppresses the callback (0).  If
        // the natural death was observed first, at most one callback fired.  Never
        // more than one, never a hang (we got here), never a crash.
        EXPECT_LE(deaths.load(), 1);
    }
}
