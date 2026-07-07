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

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "config.h"
#include "daemon.h"
#include "fixtures_paths.h"

namespace slash_emu {
namespace {

namespace fs = std::filesystem;
namespace tf = slash_emu::test_fixtures;

// The daemon blocks SIGTERM/SIGINT (pthread_sigmask) so sd-event's signalfd is the
// sole consumer.  For a process-directed kill() to reach that signalfd rather than
// killing the test binary, the signals must be blocked in EVERY thread — so we
// block them in the main thread at load time, before any thread is spawned.
const bool g_signals_blocked = [] {
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGTERM);
    sigaddset(&s, SIGINT);
    ::pthread_sigmask(SIG_BLOCK, &s, nullptr);
    return true;
}();

bool socket_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fixture: a private base directory (systemd's RuntimeDirectory in production; the
// daemon no longer creates it, so the test must) + a one-device config file.
// ─────────────────────────────────────────────────────────────────────────────

class DaemonTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<int> counter{0};
        base_ = fs::temp_directory_path() /
                ("slash_emu_daemon_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter.fetch_add(1)));
        fs::create_directories(base_);
        cfg_path_ = (base_ / "slash_emu.ini").string();
        std::ofstream(cfg_path_, std::ios::trunc) << "[device.0000:61:00]\n";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base_, ec);
    }

    // A config with one device.  With @p with_vbin the accelerator can actually
    // instantiate (model launch → ctl/qdma sockets); without it the RESCAN fails
    // fast (no model launched) — the daemon still runs and serves slash_hotplug.
    DaemonConfig make_config(bool with_vbin) {
        DaemonConfig cfg;
        cfg.base_dir    = base_.string();
        cfg.config_file = cfg_path_;
        if (with_vbin) {
            cfg.default_vbin_path = std::string(tf::kDefaultModelVbin);
        }
        cfg.accelerators.push_back(
            AcceleratorConfig{*BoardBdf::parse("0000:61:00"), std::nullopt});
        return cfg;
    }

    std::string hotplug_path() const { return (base_ / "slash_hotplug").string(); }

    // Run the daemon on a background thread; wait for slash_hotplug to appear,
    // invoke @p trigger, join, and return the daemon exit code.
    template <typename Fn>
    int run_and_stop(const DaemonConfig& cfg, Fn trigger) {
        int result = -1;
        std::thread th([&] { result = run_daemon(cfg); });
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline &&
               !socket_exists(hotplug_path())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        trigger();
        th.join();
        return result;
    }

    fs::path    base_;
    std::string cfg_path_;
};

// ── Shutdown paths ────────────────────────────────────────────────────────────

TEST_F(DaemonTest, ShutdownViaRequestShutdown) {
    EXPECT_EQ(0, run_and_stop(make_config(false), [] { request_shutdown(); }));
}

TEST_F(DaemonTest, ShutdownViaSigterm) {
    EXPECT_EQ(0, run_and_stop(make_config(false), [] { ::kill(::getpid(), SIGTERM); }));
}

TEST_F(DaemonTest, ShutdownViaSigint) {
    EXPECT_EQ(0, run_and_stop(make_config(false), [] { ::kill(::getpid(), SIGINT); }));
}

TEST_F(DaemonTest, ReinvocableAcrossRuns) {
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(0, run_and_stop(make_config(false), [] { request_shutdown(); }))
            << "run " << i;
    }
}

// Full bring-up: with a real VBIN the accelerator instantiates, so the hotplug +
// per-PF sockets all appear; shutdown unlinks every socket.
TEST_F(DaemonTest, RunWithConfigBringsUpAndTearsDown) {
    DaemonConfig cfg = make_config(/*with_vbin=*/true);

    int result = -1;
    std::thread th([&] { result = run_daemon(cfg); });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    auto all_up = [&] {
        return socket_exists(hotplug_path()) &&
               socket_exists((base_ / "slash_ctl0").string()) &&
               socket_exists((base_ / "slash_qdma_ctl0").string());
    };
    while (std::chrono::steady_clock::now() < deadline && !all_up()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(socket_exists(hotplug_path()));
    EXPECT_TRUE(socket_exists((base_ / "slash_ctl0").string()));
    EXPECT_TRUE(socket_exists((base_ / "slash_qdma_ctl0").string()));

    request_shutdown();
    th.join();
    EXPECT_EQ(0, result);

    // Teardown unlinks every socket (systemd wipes the RuntimeDirectory — including
    // the per-BDF VBIN staging dir — so the daemon itself no longer cold-cleans).
    EXPECT_FALSE(socket_exists(hotplug_path()));
    EXPECT_FALSE(socket_exists((base_ / "slash_ctl0").string()));
    EXPECT_FALSE(socket_exists((base_ / "slash_qdma_ctl0").string()));
}

// ── sd_notify: readiness, stopping, watchdog ─────────────────────────────────

// Collect datagrams sent to a NOTIFY_SOCKET the test owns.
class NotifyListener {
public:
    explicit NotifyListener(const fs::path& path) : path_(path.string()) {
        fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path_.c_str(), path_.size());
        ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::setenv("NOTIFY_SOCKET", path_.c_str(), 1);
    }
    ~NotifyListener() {
        ::unsetenv("NOTIFY_SOCKET");
        if (fd_ >= 0) ::close(fd_);
    }

    // Drain all currently-pending datagrams into the accumulated log.
    void drain() {
        char buf[512];
        for (;;) {
            ssize_t n = ::recv(fd_, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            log_ += buf;
            log_ += '\n';
        }
    }

    bool saw(const std::string& needle) {
        drain();
        return log_.find(needle) != std::string::npos;
    }

    int fd() const { return fd_; }

private:
    std::string path_;
    int         fd_{-1};
    std::string log_;
};

TEST_F(DaemonTest, NotifiesReadyThenStopping) {
    NotifyListener notify(base_ / "notify.sock");
    ASSERT_GE(notify.fd(), 0);

    DaemonConfig cfg = make_config(false);
    int result = -1;
    std::thread th([&] { result = run_daemon(cfg); });

    // READY=1 must arrive, and only after the hotplug socket is bound.
    bool ready = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !ready) {
        ready = notify.saw("READY=1");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(ready);
    EXPECT_TRUE(socket_exists(hotplug_path())) << "READY implies sockets are up";

    request_shutdown();
    th.join();
    EXPECT_EQ(0, result);

    EXPECT_TRUE(notify.saw("STOPPING=1"));
}

TEST_F(DaemonTest, WatchdogPingsWhenEnabled) {
    NotifyListener notify(base_ / "notify.sock");
    ASSERT_GE(notify.fd(), 0);
    // 200 ms deadline → daemon pings every ~100 ms; expect at least one within 5 s.
    ::setenv("WATCHDOG_USEC", "200000", 1);

    DaemonConfig cfg = make_config(false);
    int result = -1;
    std::thread th([&] { result = run_daemon(cfg); });

    bool pinged = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !pinged) {
        pinged = notify.saw("WATCHDOG=1");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    request_shutdown();
    th.join();
    ::unsetenv("WATCHDOG_USEC");

    EXPECT_EQ(0, result);
    EXPECT_TRUE(pinged) << "health-gated watchdog keepalive never arrived";
}

// ── Adversary probes ─────────────────────────────────────────────────────────

static int daemon_open_fd_count() {
    int count = 0;
    try {
        for (auto& e : fs::directory_iterator("/proc/self/fd")) { (void)e; ++count; }
    } catch (...) {}
    return count;
}

// Re-invoking run_daemon() many times leaks no fds: the eventfd + every sd-event
// source (epoll/signalfd/timerfd) are released on each sd_event_unref().
TEST_F(DaemonTest, ReinvocationDoesNotLeakFds) {
    (void)run_and_stop(make_config(false), [] { request_shutdown(); }); // warm up
    int before = daemon_open_fd_count();
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(0, run_and_stop(make_config(false), [] { request_shutdown(); }))
            << "run " << i;
    }
    int after = daemon_open_fd_count();
    EXPECT_LE(after, before + 2) << "fds leaked: before=" << before
                                 << " after=" << after;
}

// A storm of shutdown requests is idempotent — the eventfd counter saturates
// harmlessly and the daemon still exits 0; a fresh run afterwards still works.
TEST_F(DaemonTest, DoubleShutdownRequestIsIdempotent) {
    EXPECT_EQ(0, run_and_stop(make_config(false), [] {
        for (int i = 0; i < 1000; ++i) request_shutdown();
    }));
    EXPECT_EQ(0, run_and_stop(make_config(false), [] { request_shutdown(); }));
}

// A shutdown requested BEFORE run_daemon() creates its eventfd must not be lost:
// the `pending` latch is checked right after the eventfd is registered and
// self-triggers.  Watchdog-guarded so a regression surfaces as a FAILURE, not a
// suite hang.
TEST_F(DaemonTest, ShutdownRequestedBeforeStartIsNotLost) {
    (void)run_and_stop(make_config(false), [] { request_shutdown(); }); // clean slate

    DaemonConfig cfg = make_config(false);
    std::atomic<int>  result{-2};
    std::atomic<bool> done{false};
    std::thread th([&] {
        request_shutdown();          // fully ordered before run_daemon() begins
        result.store(run_daemon(cfg));
        done.store(true);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool exited = done.load();
    if (!exited) { request_shutdown(); } // rescue so the process can exit
    th.join();
    EXPECT_TRUE(exited) << "pre-start shutdown request was LOST → sd_event_loop hung";
    EXPECT_EQ(0, result.load());
}

// SIGINT and SIGTERM delivered together (storm of both) still exits 0.
TEST_F(DaemonTest, BothSignalsStormStillExitsCleanly) {
    EXPECT_EQ(0, run_and_stop(make_config(false), [] {
        for (int i = 0; i < 100; ++i) { ::kill(::getpid(), SIGINT); ::kill(::getpid(), SIGTERM); }
    }));
}

} // namespace
} // namespace slash_emu
