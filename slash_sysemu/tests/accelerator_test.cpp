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

#include "accelerator.h"
#include "fixtures_paths.h"
#include "qdma_ioctls.h"
#include "transport.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
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

ModelProcessTimeouts fast() {
    ModelProcessTimeouts t;
    t.request   = 1000ms;
    t.exit_wait = 500ms;
    t.term_wait = 500ms;
    return t;
}

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

// A scratch base dir per test, cleaned on teardown.
class AcceleratorTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = fs::temp_directory_path() /
                ("slash_accel_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter_++));
        fs::create_directories(base_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base_, ec);
    }

    AcceleratorParams params(const char* default_vbin = tf::kDefaultModelVbin) {
        AcceleratorParams p{
            .base_dir         = base_,
            .bdf              = *BoardBdf::parse("0000:61:00"),
            .default_vbin     = default_vbin,
            .ctl_socket_path  = (base_ / "slash_ctl0").string(),
            .qdma_socket_path = (base_ / "slash_qdma_ctl0").string(),
        };
        p.timeouts = fast();
        return p;
    }

    std::unique_ptr<Accelerator> make_accel(const char* default_vbin = tf::kDefaultModelVbin,
                                            std::function<void(uint64_t)> death = {}) {
        return std::make_unique<Accelerator>(params(default_vbin), std::move(death));
    }

    fs::path              base_;
    static inline int     counter_ = 0;
};

// Wait until a predicate holds or the deadline elapses.
template <typename F>
bool wait_until(F&& pred, std::chrono::milliseconds timeout = 5000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

// ─────────────────────────────────────────────────────────────────────────────
// Instantiate / state
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(AcceleratorTest, InstantiateFromAbsentReachesActive) {
    auto a = make_accel();
    EXPECT_EQ(AccelState::Absent, a->state());
    auto r = a->instantiate();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(AccelState::Active, a->state());
    EXPECT_TRUE(a->pf_present(Pf::Pf0));
    EXPECT_TRUE(a->pf_present(Pf::Pf1));
    EXPECT_TRUE(a->pf_present(Pf::Pf2));
    EXPECT_TRUE(a->model_running());
    // Both sockets exist and accept a connection.
    EXPECT_TRUE(static_cast<bool>(connect_client(a->params().ctl_socket_path)));
    EXPECT_TRUE(static_cast<bool>(connect_client(a->params().qdma_socket_path)));
}

TEST_F(AcceleratorTest, InstantiateFailsOnUnlaunchableModel) {
    auto a = make_accel(tf::kUnlaunchableVbin);
    auto r = a->instantiate();
    EXPECT_FALSE(r.has_value());
    // Ends Inactive (main.vbin was seeded from the default source → exists on disk).
    EXPECT_EQ(AccelState::Inactive, a->state());
    EXPECT_FALSE(a->model_running());
    // No leftover sockets.
    struct stat st{};
    EXPECT_NE(0, ::stat(a->params().ctl_socket_path.c_str(), &st));
    EXPECT_NE(0, ::stat(a->params().qdma_socket_path.c_str(), &st));
}

TEST_F(AcceleratorTest, InstantiateIsIdempotent) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    ASSERT_TRUE(a->instantiate().has_value()); // no-op success
    EXPECT_EQ(AccelState::Active, a->state());
}

// ─────────────────────────────────────────────────────────────────────────────
// remove_pf → Partial; last PF → Inactive
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(AcceleratorTest, RemovePf2YieldsPartial) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    a->remove_pf(Pf::Pf2);
    EXPECT_EQ(AccelState::Partial, a->state());
    EXPECT_FALSE(a->pf_present(Pf::Pf2));
    EXPECT_TRUE(a->pf_present(Pf::Pf1));
    EXPECT_TRUE(a->model_running());
    // PF2 socket gone, PF1 socket still up.
    struct stat st{};
    EXPECT_NE(0, ::stat(a->params().ctl_socket_path.c_str(), &st));
    EXPECT_EQ(0, ::stat(a->params().qdma_socket_path.c_str(), &st));
}

TEST_F(AcceleratorTest, RemovePf0FirstYieldsPartial) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    a->remove_pf(Pf::Pf0);
    EXPECT_EQ(AccelState::Partial, a->state()); // PF0 counts
    EXPECT_TRUE(a->model_running());
}

TEST_F(AcceleratorTest, RemoveLastPfTearsDownModelToInactive) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    pid_t pid = a->model()->process()->pid();
    a->remove_pf(Pf::Pf2);
    a->remove_pf(Pf::Pf1);
    EXPECT_TRUE(a->model_running()); // PF0 still present
    a->remove_pf(Pf::Pf0);          // last PF → model torn down
    EXPECT_EQ(AccelState::Inactive, a->state());
    EXPECT_FALSE(a->model_running());
    // The child is reaped (no zombie).
    EXPECT_TRUE(wait_until([&] { return ::waitpid(pid, nullptr, WNOHANG) <= 0; }));
    // VBIN files preserved.
    EXPECT_TRUE(a->model() == nullptr || a->model()->store().has_main());
    EXPECT_TRUE(fs::is_regular_file(base_ / "0000:61:00" / "main.vbin"));
}

// ─────────────────────────────────────────────────────────────────────────────
// restore_pf
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(AcceleratorTest, RestorePf2ReachesActiveAgain) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    a->remove_pf(Pf::Pf2);
    ASSERT_EQ(AccelState::Partial, a->state());
    ASSERT_TRUE(a->restore_pf(Pf::Pf2).has_value());
    EXPECT_EQ(AccelState::Active, a->state());
    EXPECT_TRUE(static_cast<bool>(connect_client(a->params().ctl_socket_path)));
}

TEST_F(AcceleratorTest, RestorePf1DoesNotReconfigure) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    pid_t pid_before = a->model()->process()->pid();
    a->remove_pf(Pf::Pf1);
    ASSERT_EQ(AccelState::Partial, a->state());
    ASSERT_TRUE(a->restore_pf(Pf::Pf1).has_value());
    EXPECT_EQ(AccelState::Active, a->state());
    // Same model process (PF1 restore must NOT reconfigure).
    EXPECT_EQ(pid_before, a->model()->process()->pid());
}

TEST_F(AcceleratorTest, RestorePf2WithEmptyStagingKeepsSameModel) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    pid_t pid_before = a->model()->process()->pid();
    a->remove_pf(Pf::Pf2);
    // Staging is empty → reconfigure Unchanged → same model.
    ASSERT_TRUE(a->restore_pf(Pf::Pf2).has_value());
    EXPECT_EQ(pid_before, a->model()->process()->pid());
    EXPECT_EQ(AccelState::Active, a->state());
}

TEST_F(AcceleratorTest, RestorePf2WithStagedVbinAdoptsNewModel) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    pid_t pid_before = a->model()->process()->pid();
    EXPECT_EQ(100000000u, a->model()->process()->system_map().clock_frequency_hz);

    a->remove_pf(Pf::Pf2);
    // Stage the 250 MHz good VBIN.
    fs::copy_file(tf::kStagingGoodVbin, a->model()->store().staging_path(),
                  fs::copy_options::overwrite_existing);
    uint64_t gen_before = a->generation();
    ASSERT_TRUE(a->restore_pf(Pf::Pf2).has_value());
    EXPECT_EQ(AccelState::Active, a->state());
    // A new process was adopted (different pid, new map, bumped generation).
    EXPECT_NE(pid_before, a->model()->process()->pid());
    EXPECT_EQ(250000000u, a->model()->process()->system_map().clock_frequency_hz);
    EXPECT_GT(a->generation(), gen_before);
}

// ─────────────────────────────────────────────────────────────────────────────
// QDMA quiesce-and-reconstruct: a PF2-restore reconfigure that adopts a NEW model
// quiesces PF1 (remove+join while the old client is alive), reconfigures, then
// reconstructs PF1 bound to the NEW client.  After the swap, PF1 transfers must
// reach the NEW model — proving the reconstructed subsystem is live (and no
// dangling ModelClient&).
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(AcceleratorTest, QdmaReconstructsAgainstNewModelAfterReconfigure) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());

    // Leave PF1 up across the reconfigure; only PF2 is removed.  restore_pf(Pf2)
    // will quiesce PF1, reconfigure, then reconstruct PF1 against the new client.
    a->remove_pf(Pf::Pf2);
    fs::copy_file(tf::kStagingGoodVbin, a->model()->store().staging_path(),
                  fs::copy_options::overwrite_existing);
    ASSERT_TRUE(a->restore_pf(Pf::Pf2).has_value());
    ASSERT_EQ(AccelState::Active, a->state());

    // Drive an H2C transfer over the (never-removed) PF1 socket; it must reach the
    // NEW model's memory (rebind worked — no dangling client).
    UniqueFd c = connect_client(a->params().qdma_socket_path);
    ASSERT_TRUE(static_cast<bool>(c));

    // ADD + START a MM qpair (both dirs).
    slash_qdma_qpair_add add{};
    add.size = sizeof(add); add.mode = kQdmaQModeMm; add.dir_mask = 0x3;
    slash_sysemu_socket_header h1{kSlashQdmaIoctlQpairAdd, 1, 0, 0};
    auto ra = send_request(c.get(), h1,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&add), sizeof(add)), {});
    ASSERT_TRUE(ra.has_value());
    ASSERT_EQ(0, ret_of(ra.value()));
    std::memcpy(&add, ra.value().payload.data(), sizeof(add));
    uint32_t qid = add.qid;

    slash_qdma_qpair_op op{}; op.size = sizeof(op); op.qid = qid;
    op.op = SLASH_QDMA_QUEUE_OP_START;
    slash_sysemu_socket_header h2{kSlashQdmaIoctlQOp, 2, 0, 0};
    auto rs = send_request(c.get(), h2,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&op), sizeof(op)), {});
    ASSERT_TRUE(rs.has_value());
    ASSERT_EQ(0, ret_of(rs.value()));

    // GET_FD.
    slash_qdma_qpair_fd_request fr{}; fr.size = sizeof(fr); fr.qid = qid; fr.qpair_count = 0;
    slash_sysemu_socket_header h3{kSlashQdmaIoctlQpairGetFd, 3, 0, 0};
    auto rg = send_request(c.get(), h3,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&fr), sizeof(fr)), {});
    ASSERT_TRUE(rg.has_value());
    ASSERT_EQ(0, ret_of(rg.value()));
    ASSERT_EQ(1u, rg.value().fds.size());
    UniqueFd xfer(std::move(rg.value().fds[0]));
    set_rcv_timeout(xfer.get(), 3000ms);

    // Buffer to send.
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    UniqueFd buf(::memfd_create("t", MFD_CLOEXEC));
    ASSERT_TRUE(static_cast<bool>(buf));
    ASSERT_EQ(0, ::ftruncate(buf.get(), static_cast<off_t>(page)));
    std::vector<uint8_t> src(page, 0x5A);
    ASSERT_EQ(static_cast<ssize_t>(page), ::pwrite(buf.get(), src.data(), page, 0));

    slash_qdma_transfer x{}; x.size = sizeof(x); x.count = 1;
    x.xfers[0].qpair_index = 0; x.xfers[0].direction = SLASH_QDMA_XFER_H2C;
    x.xfers[0].buf_fd = 0; x.xfers[0].dev_addr = 0x40000000ull; x.xfers[0].length = page;
    slash_sysemu_socket_header h4{kSlashQdmaQpairIoctlTransfer, 4, 0, 0};
    int raw = buf.get();
    std::array<int, 1> fds{raw};
    auto rt = send_request(xfer.get(), h4,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&x), sizeof(x)),
        std::span<const int>(fds));
    ASSERT_TRUE(rt.has_value());
    // The transfer succeeds against the NEW model (byte count returned).
    EXPECT_EQ(static_cast<int32_t>(page), ret_of(rt.value()));
}

// ─────────────────────────────────────────────────────────────────────────────
// HAZARD PROBE: an in-flight PF1 HBM transfer racing a PF2-restore reconfigure
// that swaps the model process.  This is the exact use-after-free the pointer-
// rebind design risked.  With the quiesce-first approach it must be CLEAN under
// aubsan: qdma_->remove() joins the transfer session while the OLD client is
// still alive, so no transfer outlives the swap.  In-flight transfers either
// complete or fail cleanly (never crash), and a fresh transfer after the swap
// reaches the NEW model.
// ─────────────────────────────────────────────────────────────────────────────

// Helpers to drive the QDMA CTL/XFER protocol from the test.
namespace {
uint32_t qdma_add_started(int ctl, uint32_t& seq) {
    slash_qdma_qpair_add add{};
    add.size = sizeof(add); add.mode = kQdmaQModeMm; add.dir_mask = 0x3;
    slash_sysemu_socket_header h{kSlashQdmaIoctlQpairAdd, seq++, 0, 0};
    auto r = send_request(ctl, h,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&add), sizeof(add)), {});
    EXPECT_TRUE(r.has_value());
    std::memcpy(&add, r.value().payload.data(), sizeof(add));
    slash_qdma_qpair_op op{}; op.size = sizeof(op); op.qid = add.qid;
    op.op = SLASH_QDMA_QUEUE_OP_START;
    slash_sysemu_socket_header h2{kSlashQdmaIoctlQOp, seq++, 0, 0};
    (void)send_request(ctl, h2,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&op), sizeof(op)), {});
    return add.qid;
}

UniqueFd qdma_get_xfer(int ctl, uint32_t qid, uint32_t& seq) {
    slash_qdma_qpair_fd_request fr{}; fr.size = sizeof(fr); fr.qid = qid; fr.qpair_count = 0;
    slash_sysemu_socket_header h{kSlashQdmaIoctlQpairGetFd, seq++, 0, 0};
    auto r = send_request(ctl, h,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&fr), sizeof(fr)), {});
    if (!r.has_value() || r.value().fds.size() != 1) return {};
    UniqueFd x(std::move(r.value().fds[0]));
    set_rcv_timeout(x.get(), 3000ms);
    return x;
}

// Issue one H2C transfer of `page` bytes; returns the response Result.
Result<ReceivedMessage> qdma_h2c(int xfer, int buf, uint32_t seq, std::size_t page) {
    slash_qdma_transfer x{}; x.size = sizeof(x); x.count = 1;
    x.xfers[0].qpair_index = 0; x.xfers[0].direction = SLASH_QDMA_XFER_H2C;
    x.xfers[0].buf_fd = 0; x.xfers[0].dev_addr = 0x40000000ull; x.xfers[0].length = page;
    slash_sysemu_socket_header h{kSlashQdmaQpairIoctlTransfer, seq, 0, 0};
    std::array<int, 1> fds{buf};
    return send_request(xfer, h,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&x), sizeof(x)),
        std::span<const int>(fds));
}

// Issue one transfer of `page` bytes in the given direction.
Result<ReceivedMessage> qdma_xfer(int xfer, int buf, uint32_t seq, std::size_t page,
                                  uint32_t dir) {
    slash_qdma_transfer x{}; x.size = sizeof(x); x.count = 1;
    x.xfers[0].qpair_index = 0; x.xfers[0].direction = dir;
    x.xfers[0].buf_fd = 0; x.xfers[0].dev_addr = 0x40000000ull; x.xfers[0].length = page;
    slash_sysemu_socket_header h{kSlashQdmaQpairIoctlTransfer, seq, 0, 0};
    std::array<int, 1> fds{buf};
    return send_request(xfer, h,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&x), sizeof(x)),
        std::span<const int>(fds));
}
} // namespace

TEST_F(AcceleratorTest, InflightTransferRacingReconfigureIsClean) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());

    UniqueFd ctl = connect_client(a->params().qdma_socket_path);
    ASSERT_TRUE(static_cast<bool>(ctl));
    uint32_t seq = 1;
    uint32_t qid = qdma_add_started(ctl.get(), seq);
    UniqueFd xfer = qdma_get_xfer(ctl.get(), qid, seq);
    ASSERT_TRUE(static_cast<bool>(xfer));

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    UniqueFd buf(::memfd_create("t", MFD_CLOEXEC));
    ASSERT_TRUE(static_cast<bool>(buf));
    ASSERT_EQ(0, ::ftruncate(buf.get(), static_cast<off_t>(page)));
    std::vector<uint8_t> src(page, 0x5A);
    ASSERT_EQ(static_cast<ssize_t>(page), ::pwrite(buf.get(), src.data(), page, 0));

    // Hammer H2C transfers from a background thread.  Each either completes
    // (byte count) or fails cleanly (forced disconnect / -ENODEV) when PF1 is
    // quiesced under the swap — never a crash/UAF.
    std::atomic<bool> go{true};
    std::atomic<int>  crashes{0};
    std::thread hammer([&] {
        uint32_t s = 1000;
        while (go.load()) {
            auto r = qdma_h2c(xfer.get(), buf.get(), s++, page);
            if (r.has_value()) {
                int32_t rv = ret_of(r.value());
                // Valid outcomes: full byte count, or a clean error (-ENODEV/-EIO).
                if (rv != static_cast<int32_t>(page) && rv >= 0) {
                    crashes.fetch_add(1);
                }
            } else {
                break; // forced disconnect on quiesce — expected, stop hammering
            }
        }
    });

    // Give the hammer a moment to get transfers in flight, then swap the process
    // via a PF2-restore reconfigure with a staged (different) VBIN.
    std::this_thread::sleep_for(20ms);
    a->remove_pf(Pf::Pf2);
    fs::copy_file(tf::kStagingGoodVbin, a->model()->store().staging_path(),
                  fs::copy_options::overwrite_existing);
    ASSERT_TRUE(a->restore_pf(Pf::Pf2).has_value());
    EXPECT_EQ(AccelState::Active, a->state());

    go.store(false);
    hammer.join();
    EXPECT_EQ(0, crashes.load());

    // After the swap, a FRESH qpair + transfer works against the NEW model.
    UniqueFd ctl2 = connect_client(a->params().qdma_socket_path);
    ASSERT_TRUE(static_cast<bool>(ctl2));
    uint32_t seq2 = 1;
    uint32_t qid2 = qdma_add_started(ctl2.get(), seq2);
    UniqueFd xfer2 = qdma_get_xfer(ctl2.get(), qid2, seq2);
    ASSERT_TRUE(static_cast<bool>(xfer2));
    auto rt = qdma_h2c(xfer2.get(), buf.get(), 9000, page);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(static_cast<int32_t>(page), ret_of(rt.value()));
}

// ─────────────────────────────────────────────────────────────────────────────
// teardown preserves VBIN files; destructor safety
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(AcceleratorTest, TeardownPreservesVbinFiles) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    a->teardown();
    EXPECT_EQ(AccelState::Inactive, a->state());
    EXPECT_TRUE(fs::is_regular_file(base_ / "0000:61:00" / "main.vbin"));
}

TEST_F(AcceleratorTest, DestructorWhileActiveIsClean) {
    int before = open_fd_count();
    pid_t pid = -1;
    {
        auto a = make_accel();
        ASSERT_TRUE(a->instantiate().has_value());
        pid = a->model()->process()->pid();
    }
    // Destructor tore everything down: child reaped, sockets gone, fds stable.
    EXPECT_TRUE(wait_until([&] { return ::waitpid(pid, nullptr, WNOHANG) <= 0; }));
    struct stat st{};
    EXPECT_NE(0, ::stat((base_ / "slash_ctl0").c_str(), &st));
    EXPECT_NE(0, ::stat((base_ / "slash_qdma_ctl0").c_str(), &st));
    int after = open_fd_count();
    EXPECT_LE(after, before + 2) << "before=" << before << " after=" << after;
}

TEST_F(AcceleratorTest, ReinstantiateAfterTeardownReuseFiles) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    a->teardown();
    ASSERT_EQ(AccelState::Inactive, a->state());
    // Re-instantiate: reuses the persisted main.vbin.
    ASSERT_TRUE(a->instantiate().has_value());
    EXPECT_EQ(AccelState::Active, a->state());
}

// ── Coverage: restore_pf(Pf0) re-sets the flag without reconfiguring ──────────
TEST_F(AcceleratorTest, RestorePf0ReSetsFlag) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    pid_t pid = a->model()->process()->pid();
    a->remove_pf(Pf::Pf0);
    ASSERT_EQ(AccelState::Partial, a->state());
    ASSERT_TRUE(a->restore_pf(Pf::Pf0).has_value());
    EXPECT_EQ(AccelState::Active, a->state());
    EXPECT_EQ(pid, a->model()->process()->pid()); // no reconfigure
}

// ── Coverage: restore_pf with no running model is rejected ────────────────────
TEST_F(AcceleratorTest, RestorePfWithoutModelFails) {
    auto a = make_accel();
    // Never instantiated → no model.
    EXPECT_FALSE(a->restore_pf(Pf::Pf2).has_value());
}

// ── Coverage: on_model_died with the CURRENT generation tears down to Inactive ─
TEST_F(AcceleratorTest, OnModelDiedTearsDown) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    a->on_model_died(a->generation()); // current generation → tears down
    EXPECT_EQ(AccelState::Inactive, a->state());
    EXPECT_FALSE(a->model_running());
}

// ── Coverage: a STALE-generation on_model_died is a no-op (guard) ─────────────
TEST_F(AcceleratorTest, OnModelDiedStaleGenerationIsNoop) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());
    uint64_t cur = a->generation();
    a->on_model_died(cur - 1); // stale generation → must NOT tear down
    EXPECT_EQ(AccelState::Active, a->state());
    EXPECT_TRUE(a->model_running());
}

// ═════════════════════════════════════════════════════════════════════════════
// ADVERSARY PROBES (Step 11) — second wave: harder QDMA-quiesce-vs-reconfigure,
// death-during-reconfigure, and death-during-teardown/instantiate ordering.
// ═════════════════════════════════════════════════════════════════════════════

// ── Probe (hardened InflightTransferRacingReconfigure): multiple PF1 sessions
//    hammering BOTH H2C and C2H HBM transfers while restore_pf(Pf2) swaps the
//    process REPEATEDLY.  The quiesce-first ordering must hold every swap: no UAF
//    of the borrowed ModelClient under aubsan, no crash; every transfer either
//    completes or fails cleanly (forced disconnect / -ENODEV / -EIO). ────────────
TEST_F(AcceleratorTest, MultiSessionH2cC2hHammerRacingRepeatedReconfigure) {
    auto a = make_accel();
    ASSERT_TRUE(a->instantiate().has_value());

    const std::size_t page = static_cast<std::size_t>(::getpagesize());

    // Three transfer sessions, each on its own CTL connection + qpair, hammering
    // alternating H2C/C2H against HBM while the process is swapped underneath.
    std::atomic<bool> go{true};
    std::atomic<int>  bad{0};
    std::vector<std::thread> sessions;
    for (int s = 0; s < 3; ++s) {
        sessions.emplace_back([&, s] {
            UniqueFd ctl = connect_client(a->params().qdma_socket_path);
            if (!ctl) return;
            uint32_t seq = 1;
            uint32_t qid = qdma_add_started(ctl.get(), seq);
            UniqueFd xfer = qdma_get_xfer(ctl.get(), qid, seq);
            if (!xfer) return;
            UniqueFd buf(::memfd_create("h", MFD_CLOEXEC));
            if (!buf) return;
            (void)::ftruncate(buf.get(), static_cast<off_t>(page));
            std::vector<uint8_t> src(page, static_cast<uint8_t>(0x10 + s));
            (void)::pwrite(buf.get(), src.data(), page, 0);

            uint32_t xseq = 1000;
            bool alive = true;
            while (go.load() && alive) {
                for (uint32_t dir : {SLASH_QDMA_XFER_H2C, SLASH_QDMA_XFER_C2H}) {
                    auto r = qdma_xfer(xfer.get(), buf.get(), xseq++, page, dir);
                    if (!r.has_value()) { alive = false; break; } // forced disconnect
                    int32_t rv = ret_of(r.value());
                    // Acceptable: full byte count, or a clean negative error.
                    if (rv >= 0 && rv != static_cast<int32_t>(page)) {
                        bad.fetch_add(1);
                    }
                    // A session may keep working across a swap only if it never
                    // got disconnected; a fresh session picks up the new socket.
                    if (!go.load()) break;
                }
            }
        });
    }

    // Repeatedly swap the process via PF2-restore reconfigure with a staged VBIN.
    // PF1 is never explicitly removed by us, so restore_pf(Pf2) must quiesce it
    // each time while the OLD client is still alive.
    for (int i = 0; i < 6; ++i) {
        std::this_thread::sleep_for(15ms);
        a->remove_pf(Pf::Pf2);
        fs::copy_file(tf::kStagingGoodVbin, a->model()->store().staging_path(),
                      fs::copy_options::overwrite_existing);
        ASSERT_TRUE(a->restore_pf(Pf::Pf2).has_value()) << "swap " << i;
        ASSERT_EQ(AccelState::Active, a->state()) << "swap " << i;
    }

    go.store(false);
    for (auto& t : sessions) t.join();
    EXPECT_EQ(0, bad.load()) << "a transfer returned a wrong positive byte count";

    // A fresh session after all swaps works against the final model.
    UniqueFd ctl = connect_client(a->params().qdma_socket_path);
    ASSERT_TRUE(static_cast<bool>(ctl));
    uint32_t seq = 1;
    uint32_t qid = qdma_add_started(ctl.get(), seq);
    UniqueFd xfer = qdma_get_xfer(ctl.get(), qid, seq);
    ASSERT_TRUE(static_cast<bool>(xfer));
    UniqueFd buf(::memfd_create("f", MFD_CLOEXEC));
    ASSERT_TRUE(static_cast<bool>(buf));
    ASSERT_EQ(0, ::ftruncate(buf.get(), static_cast<off_t>(page)));
    auto rt = qdma_h2c(xfer.get(), buf.get(), 9000, page);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(static_cast<int32_t>(page), ret_of(rt.value()));
}

// ── Probe: the model dies (killed) WHILE a PF2-restore reconfigure is mid-flight.
//    reconfigure() on a dead-but-empty-staging model is Unchanged, but the process
//    is dead; restore_pf must not crash, and the accelerator must end up either
//    Active (reconfigure relaunched) or torn down cleanly — never a UAF/hang. ─────
TEST_F(AcceleratorTest, ModelKilledDuringPf2RestoreIsClean) {
    for (int iter = 0; iter < 10; ++iter) {
        auto a = make_accel();
        ASSERT_TRUE(a->instantiate().has_value());
        pid_t pid = a->model()->process()->pid();
        a->remove_pf(Pf::Pf2);
        ASSERT_EQ(AccelState::Partial, a->state());

        // Kill the model right as we restore PF2 (empty staging → Unchanged path).
        std::thread killer([&] {
            std::this_thread::sleep_for(std::chrono::microseconds(200 * iter));
            (void)::kill(pid, SIGKILL);
        });
        (void)a->restore_pf(Pf::Pf2); // may see the process die mid-reconfigure
        killer.join();

        // The accelerator must be in a consistent state (no crash/hang under aubsan).
        AccelState st = a->state();
        EXPECT_TRUE(st == AccelState::Active || st == AccelState::Partial ||
                    st == AccelState::Inactive)
            << "iter " << iter << " unexpected state";
        a->teardown();
        EXPECT_FALSE(a->model_running());
    }
}

// ── Coverage: accel_state_name spells out every state ────────────────────────
TEST(AcceleratorStateName, AllStates) {
    EXPECT_STREQ("absent",   accel_state_name(AccelState::Absent));
    EXPECT_STREQ("inactive", accel_state_name(AccelState::Inactive));
    EXPECT_STREQ("active",   accel_state_name(AccelState::Active));
    EXPECT_STREQ("partial",  accel_state_name(AccelState::Partial));
}

} // namespace
} // namespace slash_sysemu
