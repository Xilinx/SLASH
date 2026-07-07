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

#include "config.h"
#include "fixtures_paths.h"
#include "hotplug_ioctls.h"
#include "hotplug_subsystem.h"
#include "transport.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace slash_sysemu {
namespace {

namespace fs = std::filesystem;
namespace tf = slash_sysemu::test_fixtures;
using namespace std::chrono_literals;

constexpr auto kIoTimeout = 5s;

void set_rcv_timeout(int fd, std::chrono::milliseconds ms) {
    struct timeval tv{};
    tv.tv_sec  = static_cast<time_t>(ms.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((ms.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

UniqueFd connect_client(const std::string& path) {
    auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        UniqueFd fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
        if (!fd) return {};
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.c_str(), path.size());
        if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            set_rcv_timeout(fd.get(), 3000ms);
            return fd;
        }
        std::this_thread::sleep_for(5ms);
    }
    return {};
}

int open_fd_count() {
    int count = 0;
    try {
        for (auto& e : fs::directory_iterator("/proc/self/fd")) { (void)e; ++count; }
    } catch (...) {}
    return count;
}

int32_t ret_of(const ReceivedMessage& m) { return static_cast<int32_t>(m.header.return_value); }

template <typename F>
bool wait_until(F&& pred, std::chrono::milliseconds timeout = 5000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

// ── Hotplug socket request builders ──────────────────────────────────────────

Result<ReceivedMessage> do_rescan(int fd, uint32_t seq) {
    slash_sysemu_socket_header h{kSlashHotplugIoctlRescan, seq, 0, 0};
    return send_request(fd, h, {}, {});
}

Result<ReceivedMessage> do_dev_op(int fd, uint32_t op, const std::string& bdf, uint32_t seq) {
    slash_hotplug_device_request req{};
    req.size = sizeof(req);
    std::memset(req.bdf, 0, sizeof(req.bdf));
    std::size_t n = std::min(bdf.size(), sizeof(req.bdf) - 1);
    std::memcpy(req.bdf, bdf.data(), n);
    slash_sysemu_socket_header h{op, seq, 0, 0};
    std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    return send_request(fd, h, p, {});
}

// ── Fixture: a HotplugSubsystem over a scratch base dir + generated config ───

class HotplugTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = fs::temp_directory_path() /
                ("slash_hotplug_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter_++));
        fs::create_directories(base_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base_, ec);
    }

    // Write an INI config with the given device BDFs; return its path.
    std::string write_config(const std::vector<std::string>& bdfs) {
        std::string path = (base_ / "slash_sysemu.ini").string();
        std::ofstream ofs(path, std::ios::trunc);
        for (const auto& b : bdfs) {
            ofs << "[device." << b << "]\n";
        }
        return path;
    }

    DaemonConfig make_config(const std::vector<std::string>& bdfs) {
        DaemonConfig cfg;
        cfg.base_dir          = base_.string();
        cfg.config_file       = write_config(bdfs);
        cfg.default_vbin_path = std::string(tf::kDefaultModelVbin);
        for (const auto& b : bdfs) {
            cfg.accelerators.push_back(AcceleratorConfig{*BoardBdf::parse(b), std::nullopt});
        }
        return cfg;
    }

    HotplugSubsystem::Options fast_opts() {
        HotplugSubsystem::Options o;
        o.sbr_delay = 0ms; // no real link-training wait in tests
        // Short model timeouts so a dead-model teardown doesn't wait the full 10s.
        o.model_timeouts.request   = 1000ms;
        o.model_timeouts.exit_wait = 500ms;
        o.model_timeouts.term_wait = 500ms;
        return o;
    }

    fs::path          base_;
    static inline int counter_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// setup / RESCAN instantiate
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(HotplugTest, SetupOpensSocket) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    EXPECT_TRUE(hp.is_active());
    struct stat st{};
    ASSERT_EQ(0, ::stat(hp.socket_path().c_str(), &st));
    EXPECT_TRUE(S_ISSOCK(st.st_mode));
    EXPECT_TRUE(static_cast<bool>(connect_client(hp.socket_path())));
}

// The watchdog liveness probe: try_lock on the idle lifecycle mutex succeeds, so
// healthy() is true.  (The wedged case — lock held across watchdog intervals —
// withholds the keepalive; that path is exercised by the daemon watchdog test.)
TEST_F(HotplugTest, HealthyWhenLifecycleIdle) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    EXPECT_EQ(0, hp.op_rescan());
    EXPECT_TRUE(hp.healthy());
}

TEST_F(HotplugTest, RescanInstantiatesAllConfigured) {
    HotplugSubsystem hp(make_config({"0000:61:00", "0000:62:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    EXPECT_EQ(0, hp.op_rescan());
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:62:00").value());
    EXPECT_EQ(2u, hp.accelerator_count());
}

TEST_F(HotplugTest, RescanOverSocket) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    UniqueFd c = connect_client(hp.socket_path());
    ASSERT_TRUE(static_cast<bool>(c));
    auto r = do_rescan(c.get(), 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(0, ret_of(r.value()));
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
}

// ─────────────────────────────────────────────────────────────────────────────
// REMOVE targeted PF → Partial; last PF → Inactive
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(HotplugTest, RemovePf2YieldsPartial) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    EXPECT_EQ(0, hp.op_remove("0000:61:00.2"));
    EXPECT_EQ(AccelState::Partial, hp.state_of("0000:61:00").value());
}

TEST_F(HotplugTest, RemoveAllPfsInclPf0ReachesInactive) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    EXPECT_EQ(0, hp.op_remove("0000:61:00.2"));
    EXPECT_EQ(0, hp.op_remove("0000:61:00.1"));
    EXPECT_EQ(AccelState::Partial, hp.state_of("0000:61:00").value()); // PF0 still up
    EXPECT_EQ(0, hp.op_remove("0000:61:00.0"));
    EXPECT_EQ(AccelState::Inactive, hp.state_of("0000:61:00").value());
}

TEST_F(HotplugTest, BoardLevelRemoveTearsDownAllPfs) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    // No function suffix → board-level → all PFs removed → Inactive.
    EXPECT_EQ(0, hp.op_remove("0000:61:00"));
    EXPECT_EQ(AccelState::Inactive, hp.state_of("0000:61:00").value());
}

TEST_F(HotplugTest, RemoveUnknownBdfEnodev) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    EXPECT_EQ(-ENODEV, hp.op_remove("0000:99:00.2"));
}

TEST_F(HotplugTest, RemoveOverSocketWithEchoedRequest) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    UniqueFd c = connect_client(hp.socket_path());
    ASSERT_TRUE(static_cast<bool>(c));
    auto r = do_dev_op(c.get(), kSlashHotplugIoctlRemove, "0000:61:00.2", 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(0, ret_of(r.value()));
    // The request struct is echoed back in the response payload.
    ASSERT_EQ(sizeof(slash_hotplug_device_request), r.value().payload.size());
    slash_hotplug_device_request echoed{};
    std::memcpy(&echoed, r.value().payload.data(), sizeof(echoed));
    EXPECT_STREQ("0000:61:00.2", echoed.bdf);
    EXPECT_EQ(AccelState::Partial, hp.state_of("0000:61:00").value());
}

// ─────────────────────────────────────────────────────────────────────────────
// RESCAN behaviours: skip active, restore partial with original config
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(HotplugTest, RescanRestoresPartialPf) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    ASSERT_EQ(0, hp.op_remove("0000:61:00.2"));
    ASSERT_EQ(AccelState::Partial, hp.state_of("0000:61:00").value());
    // RESCAN restores the removed PF2.
    EXPECT_EQ(0, hp.op_rescan());
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
}

TEST_F(HotplugTest, RescanLeavesActiveRunningWithoutConfig) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    Accelerator* a = hp.accelerator("0000:61:00");
    ASSERT_NE(nullptr, a);
    pid_t pid = a->model()->process()->pid();

    // Empty the config file the subsystem reloads on RESCAN, then RESCAN: the
    // active accelerator keeps running (same process) even without a config entry.
    { std::ofstream ofs((base_ / "slash_sysemu.ini").string(), std::ios::trunc); }
    EXPECT_EQ(0, hp.op_rescan());
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
    EXPECT_EQ(pid, hp.accelerator("0000:61:00")->model()->process()->pid());
}

// ─────────────────────────────────────────────────────────────────────────────
// HOTPLUG = REMOVE + RESCAN
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(HotplugTest, HotplugRemovesThenRescans) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    // HOTPLUG the PF2: removed then rescanned back → Active.
    EXPECT_EQ(0, hp.op_hotplug("0000:61:00.2"));
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
}

// ─────────────────────────────────────────────────────────────────────────────
// TOGGLE_SBR
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(HotplugTest, ToggleSbrResetsBusAndRescans) {
    // Two accelerators on the SAME bus (0x61) + one on a different bus (0x62).
    HotplugSubsystem hp(make_config({"0000:61:00", "0000:61:01", "0000:62:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    pid_t other_pid = hp.accelerator("0000:62:00")->model()->process()->pid();

    EXPECT_EQ(0, hp.op_toggle_sbr("0000:61:00.2"));
    // Both bus-0x61 accelerators are back Active (removed + rescanned).
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:01").value());
    // The different-bus accelerator was untouched (same process).
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:62:00").value());
    EXPECT_EQ(other_pid, hp.accelerator("0000:62:00")->model()->process()->pid());
}

TEST_F(HotplugTest, ToggleSbrDelayIsApplied) {
    HotplugSubsystem::Options o;
    o.sbr_delay = 60ms; // small but measurable
    HotplugSubsystem hp(make_config({"0000:61:00"}), o);
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(0, hp.op_toggle_sbr("0000:61:00"));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(elapsed, 55ms) << "SBR link-training delay was not applied";
}

// ─────────────────────────────────────────────────────────────────────────────
// Model death → posted teardown (no monitor-thread self-join, no UAF)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(HotplugTest, ModelDeathTearsDownAccelerator) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    Accelerator* a = hp.accelerator("0000:61:00");
    ASSERT_NE(nullptr, a);
    pid_t pid = a->model()->process()->pid();

    // Kill the model process out from under the daemon.  The monitor thread fires
    // the death callback → posts a teardown task → the accelerator goes Inactive
    // on the lifecycle thread (never a monitor-thread self-join).
    ASSERT_EQ(0, ::kill(pid, SIGKILL));
    EXPECT_TRUE(wait_until([&] {
        auto s = hp.state_of("0000:61:00");
        return s.has_value() && *s == AccelState::Inactive;
    }, 8000ms)) << "model death did not tear the accelerator down";
}

// ─────────────────────────────────────────────────────────────────────────────
// remove() teardown / hygiene
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(HotplugTest, RemoveTearsDownEverything) {
    int before = open_fd_count();
    {
        HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
        ASSERT_TRUE(hp.setup().has_value());
        ASSERT_EQ(0, hp.op_rescan());
        hp.remove();
        EXPECT_FALSE(hp.is_active());
        struct stat st{};
        EXPECT_NE(0, ::stat(hp.socket_path().c_str(), &st));
        EXPECT_NE(0, ::stat((base_ / "slash_ctl0").c_str(), &st));
        EXPECT_NE(0, ::stat((base_ / "slash_qdma_ctl0").c_str(), &st));
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 3) << "before=" << before << " after=" << after;
}

TEST_F(HotplugTest, DestructorWhileClientConnected) {
    auto hp = std::make_unique<HotplugSubsystem>(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp->setup().has_value());
    ASSERT_EQ(0, hp->op_rescan());
    UniqueFd c = connect_client(hp->socket_path());
    ASSERT_TRUE(static_cast<bool>(c));
    ASSERT_TRUE(do_rescan(c.get(), 1).has_value());
    hp.reset(); // destroy while the client is connected — must not hang/leak
    EXPECT_FALSE(do_rescan(c.get(), 2).has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Protocol adversary probes
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(HotplugTest, UnknownOpEnosys) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    UniqueFd c = connect_client(hp.socket_path());
    ASSERT_TRUE(static_cast<bool>(c));
    slash_sysemu_socket_header h{0xDEADBEEFu, 1, 0, 0};
    auto r = send_request(c.get(), h, {}, {});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-ENOSYS, ret_of(r.value()));
    // Worker survives.
    EXPECT_TRUE(do_rescan(c.get(), 2).has_value());
}

TEST_F(HotplugTest, ShortPayloadOnRemoveRejected) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    UniqueFd c = connect_client(hp.socket_path());
    ASSERT_TRUE(static_cast<bool>(c));
    std::array<uint8_t, 3> stub{1, 2, 3};
    slash_sysemu_socket_header h{kSlashHotplugIoctlRemove, 1, 0, 0};
    auto r = send_request(c.get(), h, std::span<const uint8_t>(stub), {});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EINVAL, ret_of(r.value()));
}

TEST_F(HotplugTest, BadBdfEinval) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    EXPECT_EQ(-EINVAL, hp.op_remove("not-a-bdf"));
    EXPECT_EQ(-EINVAL, hp.op_remove("0000:61:00.9")); // no function 9
}

TEST_F(HotplugTest, EmptyBdfSingleDeviceTargetsIt) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    // Empty bdf with exactly one tracked device → board-level remove → Inactive.
    EXPECT_EQ(0, hp.op_remove(""));
    EXPECT_EQ(AccelState::Inactive, hp.state_of("0000:61:00").value());
}

TEST_F(HotplugTest, EmptyBdfMultipleDevicesEopnotsupp) {
    HotplugSubsystem hp(make_config({"0000:61:00", "0000:62:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    EXPECT_EQ(-EOPNOTSUPP, hp.op_remove(""));
}

TEST_F(HotplugTest, EmptyBdfNoDevicesEnodev) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    // No RESCAN yet → registry empty → empty bdf → -ENODEV.
    EXPECT_EQ(-ENODEV, hp.op_remove(""));
}

// ── Coverage: the single-argument constructor (default Options, 1s SBR) ────────
TEST_F(HotplugTest, DefaultOptionsConstructor) {
    HotplugSubsystem hp(make_config({"0000:61:00"}));
    ASSERT_TRUE(hp.setup().has_value());
    EXPECT_TRUE(hp.is_active());
    // Do not exercise TOGGLE_SBR here (default 1s delay); just prove it constructs
    // and serves a RESCAN.
    EXPECT_EQ(0, hp.op_rescan());
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
}

// ── Coverage: state_of / accelerator return "unknown" for an untracked bdf ─────
TEST_F(HotplugTest, StateOfUnknownIsNullopt) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    EXPECT_FALSE(hp.state_of("0000:99:00").has_value());
    EXPECT_EQ(nullptr, hp.accelerator("0000:99:00"));
}

// ── Coverage: HOTPLUG + TOGGLE_SBR driven OVER THE SOCKET (dispatch paths) ─────
TEST_F(HotplugTest, HotplugAndToggleSbrOverSocket) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    UniqueFd c = connect_client(hp.socket_path());
    ASSERT_TRUE(static_cast<bool>(c));

    auto rh = do_dev_op(c.get(), kSlashHotplugIoctlHotplug, "0000:61:00.1", 1);
    ASSERT_TRUE(rh.has_value());
    EXPECT_EQ(0, ret_of(rh.value()));
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());

    auto rs = do_dev_op(c.get(), kSlashHotplugIoctlToggleSbr, "0000:61:00", 2);
    ASSERT_TRUE(rs.has_value());
    EXPECT_EQ(0, ret_of(rs.value()));
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
}

// ── Coverage: RESCAN restores PF0 and PF1 of a partial (not just PF2) ──────────
TEST_F(HotplugTest, RescanRestoresPf0AndPf1) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    // Remove PF0 and PF1 (PF2 stays up → still Partial, model alive).
    ASSERT_EQ(0, hp.op_remove("0000:61:00.0"));
    ASSERT_EQ(0, hp.op_remove("0000:61:00.1"));
    ASSERT_EQ(AccelState::Partial, hp.state_of("0000:61:00").value());
    // RESCAN restores both PF0 and PF1.
    EXPECT_EQ(0, hp.op_rescan());
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
}

// ── Coverage: connection reaper (many short connections then a fresh op) ──────
TEST_F(HotplugTest, ShortConnectionChurnReaped) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    int before = open_fd_count();
    for (int i = 0; i < 50; ++i) {
        UniqueFd c = connect_client(hp.socket_path());
        ASSERT_TRUE(static_cast<bool>(c));
        ASSERT_TRUE(do_rescan(c.get(), 1).has_value());
        // c closes here.
    }
    // A few more connections trigger the listener's reaper.
    for (int i = 0; i < 3; ++i) {
        UniqueFd c = connect_client(hp.socket_path());
        ASSERT_TRUE(static_cast<bool>(c));
        ASSERT_TRUE(do_rescan(c.get(), 1).has_value());
        std::this_thread::sleep_for(10ms);
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 15) << "before=" << before << " after=" << after;
}

// ═════════════════════════════════════════════════════════════════════════════
// ADVERSARY PROBES (Step 11)
//
// Probes hunting for lifecycle-lock races, death-callback-vs-lifecycle-op races,
// forced-disconnect hangs, stale-generation teardown, and socket/thread hygiene
// under concurrent hotplug activity.
// ═════════════════════════════════════════════════════════════════════════════

// ── Probe: concurrent lifecycle ops serialise under the single lock (no crash,
//    no corruption) — many threads hammering RESCAN/REMOVE/HOTPLUG at once. ─────
TEST_F(HotplugTest, ConcurrentLifecycleOpsSerialise) {
    HotplugSubsystem hp(make_config({"0000:61:00", "0000:62:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());

    std::atomic<bool> go{true};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&, t] {
            int i = 0;
            while (go.load()) {
                const char* bdf = (t % 2 == 0) ? "0000:61:00.2" : "0000:62:00.1";
                switch (i++ % 3) {
                case 0: (void)hp.op_remove(bdf); break;
                case 1: (void)hp.op_hotplug(bdf); break;
                default: (void)hp.op_rescan(); break;
                }
            }
        });
    }
    std::this_thread::sleep_for(200ms);
    go.store(false);
    for (auto& th : threads) th.join();
    // A final RESCAN restores everything to Active — the registry is not corrupt.
    EXPECT_EQ(0, hp.op_rescan());
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:62:00").value());
}

// ── Probe: a user connected to a PF socket sees a forced disconnect when that PF
//    is REMOVEd out from under it, and the op does not hang. ────────────────────
TEST_F(HotplugTest, RemovePfForcesUserDisconnect) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    // Connect a user to the PF2 (slash_ctl) socket.
    std::string ctl_path = hp.accelerator("0000:61:00")->params().ctl_socket_path;
    UniqueFd user = connect_client(ctl_path);
    ASSERT_TRUE(static_cast<bool>(user));

    EXPECT_EQ(0, hp.op_remove("0000:61:00.2")); // must not hang
    // The user's next recv fails (forced disconnect).
    auto r = recv_message(user.get());
    EXPECT_FALSE(r.has_value());
}

// ── Probe: model death racing a concurrent RESCAN — the stale death task must
//    not tear down a freshly restarted accelerator (generation guard). ──────────
TEST_F(HotplugTest, ModelDeathRacingRescanNoStaleTeardown) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    pid_t pid = hp.accelerator("0000:61:00")->model()->process()->pid();

    // Kill the model, then immediately drive RESCANs.  Eventually the accelerator
    // must settle Active (RESCAN re-instantiates a dead accelerator) — the death
    // task must not tear down the restarted one.
    ASSERT_EQ(0, ::kill(pid, SIGKILL));
    for (int i = 0; i < 20; ++i) {
        (void)hp.op_rescan();
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(wait_until([&] {
        auto s = hp.state_of("0000:61:00");
        return s.has_value() && *s == AccelState::Active;
    }, 8000ms)) << "accelerator did not settle Active after death+rescan";
    // The running process is a NEW one (not the killed pid).
    EXPECT_NE(pid, hp.accelerator("0000:61:00")->model()->process()->pid());
}

// ── Probe: remove() while the socket has busy clients + a pending op — no hang,
//    no leaked threads/fds. ─────────────────────────────────────────────────────
TEST_F(HotplugTest, RemoveWhileClientsBusyNoHang) {
    int before = open_fd_count();
    {
        HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
        ASSERT_TRUE(hp.setup().has_value());
        ASSERT_EQ(0, hp.op_rescan());

        std::atomic<bool> go{true};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                UniqueFd c = connect_client(hp.socket_path());
                if (!c) return;
                uint32_t seq = 1;
                while (go.load()) {
                    if (!do_rescan(c.get(), seq++).has_value()) break; // forced disconnect
                }
            });
        }
        std::this_thread::sleep_for(80ms);
        hp.remove(); // must not deadlock with busy clients + lifecycle thread
        go.store(false);
        for (auto& th : threads) th.join();
        EXPECT_FALSE(hp.is_active());
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 4) << "before=" << before << " after=" << after;
}

// ── Probe: RESCAN with an unparseable config keeps active accelerators running. ─
TEST_F(HotplugTest, RescanWithBrokenConfigKeepsActiveRunning) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    pid_t pid = hp.accelerator("0000:61:00")->model()->process()->pid();

    // Corrupt the config file, then RESCAN: parse fails → keep running.
    { std::ofstream ofs((base_ / "slash_sysemu.ini").string(), std::ios::trunc);
      ofs << "this is not a valid ini !!! [[["; }
    EXPECT_EQ(0, hp.op_rescan());
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
    EXPECT_EQ(pid, hp.accelerator("0000:61:00")->model()->process()->pid());
}

// ═════════════════════════════════════════════════════════════════════════════
// ADVERSARY PROBES (Step 11) — second wave: stale-death-task / generation guard,
// TOGGLE_SBR-lock-blocks-others, shutdown interrupts SBR, multi-accel death.
// ═════════════════════════════════════════════════════════════════════════════

// ── Probe: a STALE model-death task must not tear down a freshly-adopted process.
//
// Sequence attacked:
//   1. Model P1 is running (Active).  Kill P1 → its monitor thread posts a death
//      teardown task T1 to the lifecycle queue.
//   2. Before T1 drains, a board-level HOTPLUG runs on a socket thread: it tears
//      the (now-dead) accelerator down and RESCAN re-instantiates a NEW process P2
//      — all under the lifecycle lock, so the accelerator is Active on P2.
//   3. T1 finally drains.  If the death path only guards on model_running() (which
//      is TRUE for the healthy P2) and NOT on the model GENERATION that T1 was
//      posted for, it wrongly tears P2 down → the accelerator drops to Inactive
//      and a healthy child is killed.
//
// The architecture + accelerator.h promise a generation guard exactly for this.
// The probe repeats the race many times and asserts the accelerator always
// settles Active on a LIVE (reaped-free) process.  A single wrong teardown fails.
TEST_F(HotplugTest, StaleDeathTaskDoesNotTearDownReadoptedModel) {
    for (int iter = 0; iter < 12; ++iter) {
        HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
        ASSERT_TRUE(hp.setup().has_value());
        ASSERT_EQ(0, hp.op_rescan());
        Accelerator* a = hp.accelerator("0000:61:00");
        ASSERT_NE(nullptr, a);
        pid_t p1 = a->model()->process()->pid();

        // Kill P1, then IMMEDIATELY drive a board-level HOTPLUG that re-instantiates
        // a fresh P2.  The kill's death task races the HOTPLUG's re-adoption.
        ASSERT_EQ(0, ::kill(p1, SIGKILL));
        (void)hp.op_hotplug("0000:61:00"); // remove-all + rescan → new P2

        // After the dust settles the accelerator must be Active on a live process
        // whose pid is NOT the killed p1, and must STAY Active (the stale death
        // task, if it fires late, must be a no-op for the new generation).
        bool active = wait_until([&] {
            auto s = hp.state_of("0000:61:00");
            return s.has_value() && *s == AccelState::Active;
        }, 8000ms);
        ASSERT_TRUE(active) << "iter " << iter << ": accelerator not Active after death+hotplug";

        pid_t p2 = hp.accelerator("0000:61:00")->model()->process()->pid();
        EXPECT_NE(p1, p2) << "iter " << iter;

        // Give any late-draining stale death task a chance to (wrongly) run, then
        // re-assert the accelerator is STILL Active on the SAME p2.
        std::this_thread::sleep_for(60ms);
        auto s = hp.state_of("0000:61:00");
        ASSERT_TRUE(s.has_value());
        EXPECT_EQ(AccelState::Active, *s)
            << "iter " << iter << ": stale death task tore down the re-adopted model";
        EXPECT_EQ(p2, hp.accelerator("0000:61:00")->model()->process()->pid())
            << "iter " << iter << ": re-adopted process changed (stale teardown + restart)";
    }
}

// ── Probe: TOGGLE_SBR holds the lifecycle lock across its delay, so a concurrent
//    op genuinely BLOCKS until the SBR completes (proves the single-lock serialise
//    for the long op, not just fast ops). ───────────────────────────────────────
TEST_F(HotplugTest, ToggleSbrBlocksConcurrentOpForItsDuration) {
    HotplugSubsystem::Options o;
    o.sbr_delay = 150ms;
    o.model_timeouts.request = 1000ms;
    o.model_timeouts.exit_wait = 500ms;
    o.model_timeouts.term_wait = 500ms;
    HotplugSubsystem hp(make_config({"0000:61:00"}), o);
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());

    std::atomic<bool> sbr_started{false};
    auto t0 = std::chrono::steady_clock::now();
    std::thread sbr([&] {
        sbr_started.store(true);
        (void)hp.op_toggle_sbr("0000:61:00");
    });
    // Wait until the SBR thread has surely taken the lock (it sleeps ~150ms).
    while (!sbr_started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);
    // This REMOVE must block behind the SBR's held lock.
    (void)hp.op_remove("0000:61:00.2");
    auto elapsed = std::chrono::steady_clock::now() - t0;
    sbr.join();
    EXPECT_GE(elapsed, 120ms)
        << "concurrent op did not block behind TOGGLE_SBR's held lock";
}

// ── Probe: remove() during an in-flight TOGGLE_SBR delay wakes it early (shutdown
//    interrupts the ~1s link-training sleep) — no ~1s hang on shutdown. ──────────
TEST_F(HotplugTest, ShutdownInterruptsInflightSbrDelay) {
    HotplugSubsystem::Options o;
    o.sbr_delay = 5000ms; // long enough that a non-interrupted wait would hang
    o.model_timeouts.request = 1000ms;
    o.model_timeouts.exit_wait = 500ms;
    o.model_timeouts.term_wait = 500ms;
    auto hp = std::make_unique<HotplugSubsystem>(make_config({"0000:61:00"}), o);
    ASSERT_TRUE(hp->setup().has_value());
    ASSERT_EQ(0, hp->op_rescan());

    std::atomic<bool> sbr_done{false};
    std::thread sbr([&] {
        (void)hp->op_toggle_sbr("0000:61:00");
        sbr_done.store(true);
    });
    std::this_thread::sleep_for(80ms); // let the SBR reach its 5s sleep
    auto t0 = std::chrono::steady_clock::now();
    hp->remove();                       // must wake the SBR sleep early
    sbr.join();
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_TRUE(sbr_done.load());
    EXPECT_LT(elapsed, 3000ms) << "remove() did not interrupt the SBR link-training sleep";
    hp.reset();
}

// ── Probe: several accelerators' models die at once; every death task must be
//    handled without deadlock/UAF and each accelerator ends Inactive. ────────────
TEST_F(HotplugTest, MultipleModelsDyingAtOnce) {
    HotplugSubsystem hp(make_config({"0000:61:00", "0000:62:00", "0000:63:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());

    std::vector<std::string> bdfs{"0000:61:00", "0000:62:00", "0000:63:00"};
    std::vector<pid_t> pids;
    for (auto& b : bdfs) {
        Accelerator* a = hp.accelerator(b);
        ASSERT_NE(nullptr, a);
        pids.push_back(a->model()->process()->pid());
    }
    // Kill all three model processes as close together as possible.
    for (pid_t p : pids) ASSERT_EQ(0, ::kill(p, SIGKILL));

    for (auto& b : bdfs) {
        EXPECT_TRUE(wait_until([&] {
            auto s = hp.state_of(b);
            return s.has_value() && *s == AccelState::Inactive;
        }, 8000ms)) << b << " did not go Inactive after model death";
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// ADVERSARY PROBES (Step 11) — third wave: hotplug ABI / targeting hardening.
// Cross-checked against driver/libslash/include/slash/uapi/slash_hotplug.h.
// ═════════════════════════════════════════════════════════════════════════════

// ── Probe: RESCAN takes NO argument (driver: _IO('w',0x30)).  Sending it WITH a
//    bogus payload must still succeed and instantiate — the payload is ignored. ──
TEST_F(HotplugTest, RescanIgnoresAnyPayload) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    UniqueFd c = connect_client(hp.socket_path());
    ASSERT_TRUE(static_cast<bool>(c));
    std::array<uint8_t, 40> junk{};
    for (auto& b : junk) b = 0xAB;
    slash_sysemu_socket_header h{kSlashHotplugIoctlRescan, 1, 0, 0};
    auto r = send_request(c.get(), h, std::span<const uint8_t>(junk), {});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(0, ret_of(r.value()));
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
}

// ── Probe: an unknown/foreign ioctl (wrong magic 'v' instead of 'w') is rejected
//    with -ENOSYS and the worker survives to serve a following RESCAN. ───────────
TEST_F(HotplugTest, WrongMagicOpEnosysWorkerSurvives) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    UniqueFd c = connect_client(hp.socket_path());
    ASSERT_TRUE(static_cast<bool>(c));
    // A QDMA-magic ('v') op number should not be understood by the hotplug socket.
    slash_sysemu_socket_header h{0x40085650u /* arbitrary 'v'-ish */, 1, 0, 0};
    auto r = send_request(c.get(), h, {}, {});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-ENOSYS, ret_of(r.value()));
    EXPECT_TRUE(do_rescan(c.get(), 2).has_value());
}

// ── Probe: an OVERSIZED REMOVE payload (> struct) is tolerated: the first
//    sizeof(request) bytes are parsed, the extra bytes ignored (forward-compat). ─
TEST_F(HotplugTest, OversizedRemovePayloadTolerated) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    UniqueFd c = connect_client(hp.socket_path());
    ASSERT_TRUE(static_cast<bool>(c));

    std::vector<uint8_t> buf(sizeof(slash_hotplug_device_request) + 64, 0);
    slash_hotplug_device_request req{};
    req.size = sizeof(req);
    std::memcpy(req.bdf, "0000:61:00.2", sizeof("0000:61:00.2"));
    std::memcpy(buf.data(), &req, sizeof(req));
    for (std::size_t i = sizeof(req); i < buf.size(); ++i) buf[i] = 0xEE; // trailing junk
    slash_sysemu_socket_header h{kSlashHotplugIoctlRemove, 1, 0, 0};
    auto r = send_request(c.get(), h, std::span<const uint8_t>(buf), {});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(0, ret_of(r.value()));
    EXPECT_EQ(AccelState::Partial, hp.state_of("0000:61:00").value());
}

// ── Probe: a lying `size` field must not be trusted for parsing.  The daemon keys
//    off the actual datagram length (>= sizeof(request)), not req.size. ──────────
TEST_F(HotplugTest, SizeFieldLieDoesNotCorruptParsing) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    UniqueFd c = connect_client(hp.socket_path());
    ASSERT_TRUE(static_cast<bool>(c));
    slash_hotplug_device_request req{};
    req.size = 0xFFFFFFFFu; // absurd size claim
    std::memcpy(req.bdf, "0000:61:00.1", sizeof("0000:61:00.1"));
    slash_sysemu_socket_header h{kSlashHotplugIoctlRemove, 1, 0, 0};
    std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    auto r = send_request(c.get(), h, p, {});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(0, ret_of(r.value())); // parsed by real length, bdf honored
    EXPECT_FALSE(hp.accelerator("0000:61:00")->pf_present(Pf::Pf1));
}

// ── Probe: a bdf with an unterminated / fully-filled 32-byte field must not read
//    past the buffer (dispatch forces NUL at bdf[len-1]); a non-BDF blob → -EINVAL,
//    worker survives. ────────────────────────────────────────────────────────────
TEST_F(HotplugTest, UnterminatedBdfIsBoundedAndRejected) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    UniqueFd c = connect_client(hp.socket_path());
    ASSERT_TRUE(static_cast<bool>(c));
    slash_hotplug_device_request req{};
    req.size = sizeof(req);
    std::memset(req.bdf, 'A', sizeof(req.bdf)); // NO NUL anywhere in the field
    slash_sysemu_socket_header h{kSlashHotplugIoctlRemove, 1, 0, 0};
    std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    auto r = send_request(c.get(), h, p, {});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EINVAL, ret_of(r.value())); // "AAAA..." is not a valid BDF
    // Worker survives, accelerator untouched.
    EXPECT_TRUE(do_rescan(c.get(), 2).has_value());
    EXPECT_EQ(AccelState::Active, hp.state_of("0000:61:00").value());
}

// ── Probe: targeting every function suffix individually removes exactly that PF. ─
TEST_F(HotplugTest, PerFunctionSuffixTargetsExactlyThatPf) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    Accelerator* a = hp.accelerator("0000:61:00");
    ASSERT_NE(nullptr, a);

    EXPECT_EQ(0, hp.op_remove("0000:61:00.1")); // PF1 only
    EXPECT_TRUE(a->pf_present(Pf::Pf0));
    EXPECT_FALSE(a->pf_present(Pf::Pf1));
    EXPECT_TRUE(a->pf_present(Pf::Pf2));

    EXPECT_EQ(0, hp.op_rescan()); // restore
    EXPECT_EQ(0, hp.op_remove("0000:61:00.2")); // PF2 only
    EXPECT_TRUE(a->pf_present(Pf::Pf0));
    EXPECT_TRUE(a->pf_present(Pf::Pf1));
    EXPECT_FALSE(a->pf_present(Pf::Pf2));
}

// ── Probe: REMOVE/HOTPLUG/TOGGLE_SBR on a syntactically valid but UNKNOWN board
//    bdf → -ENODEV (targeting resolves, registry lookup misses). ─────────────────
TEST_F(HotplugTest, KnownFormatUnknownBoardEnodevAcrossOps) {
    HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
    ASSERT_TRUE(hp.setup().has_value());
    ASSERT_EQ(0, hp.op_rescan());
    EXPECT_EQ(-ENODEV, hp.op_remove("0000:aa:00.2"));
    EXPECT_EQ(-ENODEV, hp.op_hotplug("0000:aa:00.1"));
    // TOGGLE_SBR on an unknown-but-valid bdf resolves the target then fans out on
    // its bus; an unknown board still resolves to a target, so it RESCANs the bus
    // (no accelerators on bus 'aa') and returns 0 — assert it does not error/crash.
    EXPECT_EQ(0, hp.op_toggle_sbr("0000:aa:00"));
}

// ── Probe: many REMOVE→RESCAN cycles on a live subsystem leave no fd/socket/zombie
//    leak and the accelerator ends Active.  Exercises repeated model+PF teardown
//    and re-instantiation through the lifecycle lock. ────────────────────────────
TEST_F(HotplugTest, RemoveRescanCyclesNoLeak) {
    int before = open_fd_count();
    {
        HotplugSubsystem hp(make_config({"0000:61:00"}), fast_opts());
        ASSERT_TRUE(hp.setup().has_value());
        ASSERT_EQ(0, hp.op_rescan());
        for (int i = 0; i < 20; ++i) {
            ASSERT_EQ(0, hp.op_remove("0000:61:00")) << "cycle " << i; // board-level → Inactive
            ASSERT_EQ(AccelState::Inactive, hp.state_of("0000:61:00").value()) << "cycle " << i;
            ASSERT_EQ(0, hp.op_rescan()) << "cycle " << i;             // → Active
            ASSERT_EQ(AccelState::Active, hp.state_of("0000:61:00").value()) << "cycle " << i;
        }
        hp.remove();
    }
    int after = open_fd_count();
    // A small slack for gtest/loader fds; a per-cycle leak (20 cycles) would blow
    // well past this.
    EXPECT_LE(after, before + 4) << "before=" << before << " after=" << after;
}

// ── Probe: destroy the whole subsystem while a lifecycle op (TOGGLE_SBR sleeping
//    under the lock) is in flight AND clients are connected — no hang, no leak. ──
TEST_F(HotplugTest, DestructorDuringInflightSbrWithClients) {
    int before = open_fd_count();
    {
        HotplugSubsystem::Options o;
        o.sbr_delay = 3000ms;
        o.model_timeouts.request = 1000ms;
        o.model_timeouts.exit_wait = 500ms;
        o.model_timeouts.term_wait = 500ms;
        auto hp = std::make_unique<HotplugSubsystem>(make_config({"0000:61:00"}), o);
        ASSERT_TRUE(hp->setup().has_value());
        ASSERT_EQ(0, hp->op_rescan());
        UniqueFd user = connect_client(hp->socket_path());
        ASSERT_TRUE(static_cast<bool>(user));

        std::thread sbr([&] { (void)hp->op_toggle_sbr("0000:61:00"); });
        std::this_thread::sleep_for(80ms); // SBR now sleeping under the lock
        auto t0 = std::chrono::steady_clock::now();
        hp.reset(); // destructor: remove() must wake the SBR sleep + join everything
        auto elapsed = std::chrono::steady_clock::now() - t0;
        sbr.join();
        EXPECT_LT(elapsed, 2500ms) << "destructor did not interrupt the SBR sleep";
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 4) << "before=" << before << " after=" << after;
}

} // namespace
} // namespace slash_sysemu
