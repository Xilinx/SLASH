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

#include "bar_memfd.h"
#include "ctl_ioctls.h"
#include "ctl_subsystem.h"
#include "transport.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace slash_sysemu {
namespace {

using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Test helpers
// ─────────────────────────────────────────────────────────────────────────────

// A short deadline so a hung test fails fast rather than blocking ctest.
constexpr auto kIoTimeout = 5s;

// Set a receive timeout on a socket so a client recv() never blocks forever.
void set_rcv_timeout(int fd, std::chrono::milliseconds ms) {
    struct timeval tv{};
    tv.tv_sec  = static_cast<time_t>(ms.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((ms.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Connect a fresh SEQPACKET client socket to the named path, retrying briefly
// while the listener comes up.  Returns an owning UniqueFd (invalid on failure).
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

// Count open fds (for leak checks).
int open_fd_count() {
    int count = 0;
    try {
        for (auto& e : std::filesystem::directory_iterator("/proc/self/fd")) {
            (void)e;
            ++count;
        }
    } catch (...) {}
    return count;
}

// A unique base dir under /tmp for each test's socket path.
class CtlSubsystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto bars = make_standard_bars();
        ASSERT_TRUE(bars.has_value()) << bars.error().message;
        bars_ = std::make_unique<BarSet>(std::move(bars.value()));

        base_ = std::filesystem::temp_directory_path() /
                ("slash_ctl_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter_++));
        std::filesystem::create_directories(base_);
        sock_path_ = (base_ / "slash_ctl0").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }

    std::unique_ptr<CtlSubsystem> make_subsystem() {
        return std::make_unique<CtlSubsystem>(sock_path_, "0000:61:00", *bars_);
    }

    std::unique_ptr<BarSet> bars_;
    std::filesystem::path   base_;
    std::string             sock_path_;
    static inline std::atomic<int> counter_{0};
};

// ── Request builders ─────────────────────────────────────────────────────────

Result<ReceivedMessage> do_bar_info(int fd, uint8_t bar_number, uint32_t seq) {
    slash_ioctl_bar_info info{};
    info.size       = sizeof(info);
    info.bar_number = bar_number;
    slash_sysemu_socket_header h{kSlashCtldevIoctlGetBarInfo, seq, 0, 0};
    std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&info), sizeof(info));
    return send_request(fd, h, payload, {});
}

Result<ReceivedMessage> do_bar_fd(int fd, uint8_t bar_number, uint32_t seq) {
    slash_ioctl_bar_fd_request req{};
    req.size       = sizeof(req);
    req.bar_number = bar_number;
    slash_sysemu_socket_header h{kSlashCtldevIoctlGetBarFd, seq, 0, 0};
    std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    return send_request(fd, h, payload, {});
}

Result<ReceivedMessage> do_device_info(int fd, uint32_t seq) {
    slash_ioctl_device_info dev{};
    dev.size = sizeof(dev);
    slash_sysemu_socket_header h{kSlashCtldevIoctlGetDeviceInfo, seq, 0, 0};
    std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&dev), sizeof(dev));
    return send_request(fd, h, payload, {});
}

int32_t ret_of(const ReceivedMessage& m) {
    return static_cast<int32_t>(m.header.return_value);
}

// ─────────────────────────────────────────────────────────────────────────────
// setup / socket lifecycle
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(CtlSubsystemTest, SetupCreatesSocket) {
    // Ownership + mode are systemd's (User=/Group= + UMask=), so the subsystem
    // no longer chowns/chmods; it just creates the SEQPACKET socket at the path.
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    EXPECT_TRUE(sub->is_active());

    struct stat st{};
    ASSERT_EQ(0, ::stat(sock_path_.c_str(), &st));
    EXPECT_TRUE(S_ISSOCK(st.st_mode));
}

TEST_F(CtlSubsystemTest, SetupIsIdempotent) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    EXPECT_TRUE(sub->setup().has_value()); // second setup is a no-op success
    EXPECT_TRUE(sub->is_active());
}

TEST_F(CtlSubsystemTest, SetupUnlinksStaleSocketFile) {
    // Pre-create a stale plain file at the socket path.
    { int fd = ::open(sock_path_.c_str(), O_CREAT | O_WRONLY, 0600); ASSERT_GE(fd, 0); ::close(fd); }
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    struct stat st{};
    ASSERT_EQ(0, ::stat(sock_path_.c_str(), &st));
    EXPECT_TRUE(S_ISSOCK(st.st_mode));
}

TEST_F(CtlSubsystemTest, SetupRejectsOverlongPath) {
    std::string long_path = (base_ / std::string(200, 'x')).string();
    CtlSubsystem sub(long_path, "0000:61:00", *bars_);
    auto r = sub.setup();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(ErrorKind::Transport, r.error().kind);
    EXPECT_FALSE(sub.is_active());
}

// ─────────────────────────────────────────────────────────────────────────────
// GET_BAR_INFO
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(CtlSubsystemTest, BarInfoPresentBars) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    struct Case { uint8_t bar; uint64_t len; };
    const Case cases[] = {
        {0, kUserRegionSize},
        {2, kServiceLayerSize},
        {4, kClockWizardSize},
    };
    uint32_t seq = 1;
    for (const auto& tc : cases) {
        auto resp = do_bar_info(c.get(), tc.bar, seq++);
        ASSERT_TRUE(resp.has_value()) << "bar " << int(tc.bar);
        const auto& m = resp.value();
        EXPECT_EQ(0, ret_of(m));
        ASSERT_EQ(sizeof(slash_ioctl_bar_info), m.payload.size());
        slash_ioctl_bar_info info{};
        std::memcpy(&info, m.payload.data(), sizeof(info));
        EXPECT_EQ(tc.bar, info.bar_number);
        EXPECT_EQ(1u, info.usable);
        EXPECT_EQ(0u, info.in_use);
        EXPECT_EQ(0u, info.start_address);
        EXPECT_EQ(tc.len, info.length);
    }
}

TEST_F(CtlSubsystemTest, BarInfoAbsentBars) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    uint32_t seq = 1;
    for (uint8_t bar : {uint8_t{1}, uint8_t{3}, uint8_t{5}, uint8_t{6}, uint8_t{200}}) {
        auto resp = do_bar_info(c.get(), bar, seq++);
        ASSERT_TRUE(resp.has_value()) << "bar " << int(bar);
        slash_ioctl_bar_info info{};
        ASSERT_EQ(sizeof(info), resp.value().payload.size());
        std::memcpy(&info, resp.value().payload.data(), sizeof(info));
        EXPECT_EQ(0u, info.usable) << "bar " << int(bar);
        EXPECT_EQ(0u, info.length);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GET_BAR_FD
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(CtlSubsystemTest, BarFdPresentReturnsMappableFd) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    auto resp = do_bar_fd(c.get(), 0, 1);
    ASSERT_TRUE(resp.has_value());
    auto& m = resp.value();
    EXPECT_EQ(0, ret_of(m));
    ASSERT_EQ(1u, m.fds.size());
    ASSERT_TRUE(static_cast<bool>(m.fds[0]));

    slash_ioctl_bar_fd_request req{};
    ASSERT_EQ(sizeof(req), m.payload.size());
    std::memcpy(&req, m.payload.data(), sizeof(req));
    EXPECT_EQ(kUserRegionSize, req.length);

    // The fd must map to the BAR's size.
    int fd = m.fds[0].get();
    struct stat st{};
    ASSERT_EQ(0, ::fstat(fd, &st));
    EXPECT_EQ(static_cast<off_t>(kUserRegionSize), st.st_size);

    void* p = ::mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, 0);
    ASSERT_NE(MAP_FAILED, p);
    ::munmap(p, 4096);
}

TEST_F(CtlSubsystemTest, BarFdRoundTripReflectsDaemonWrite) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    // Daemon writes a sentinel into the user-region BAR.
    const std::size_t off = 0x40;
    const uint32_t    val = 0xCAFEBABEu;
    ASSERT_TRUE(bars_->user_region.write_u32(off, val).has_value());

    auto resp = do_bar_fd(c.get(), 0, 1);
    ASSERT_TRUE(resp.has_value());
    ASSERT_EQ(1u, resp.value().fds.size());
    int fd = resp.value().fds[0].get();

    void* p = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(MAP_FAILED, p);
    uint32_t seen;
    std::memcpy(&seen, static_cast<uint8_t*>(p) + off, sizeof(seen));
    EXPECT_EQ(val, seen); // client sees the daemon's write (same inode)
    ::munmap(p, 4096);
}

TEST_F(CtlSubsystemTest, BarFdIsDistinctDescriptionFlockConflicts) {
    // The critical proof that reopen() gave a DISTINCT open file description:
    // an exclusive flock held by the daemon on the BarMemfd and an exclusive
    // flock on the received fd must genuinely CONFLICT.
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    auto resp = do_bar_fd(c.get(), 0, 1);
    ASSERT_TRUE(resp.has_value());
    ASSERT_EQ(1u, resp.value().fds.size());
    int user_fd = resp.value().fds[0].get();

    // Take an exclusive lock on the daemon-side BarMemfd fd.
    int daemon_fd = bars_->user_region.fd();
    ASSERT_EQ(0, ::flock(daemon_fd, LOCK_EX));

    // A non-blocking exclusive lock on the user's distinct description must fail
    // with EWOULDBLOCK — proving the two descriptions collide.
    int rc = ::flock(user_fd, LOCK_EX | LOCK_NB);
    EXPECT_EQ(-1, rc);
    EXPECT_TRUE(errno == EWOULDBLOCK || errno == EAGAIN);

    ::flock(daemon_fd, LOCK_UN);
    // After the daemon releases, the user can acquire.
    EXPECT_EQ(0, ::flock(user_fd, LOCK_EX | LOCK_NB));
    ::flock(user_fd, LOCK_UN);
}

TEST_F(CtlSubsystemTest, BarFdAbsentReturnsError) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    for (uint8_t bar : {uint8_t{1}, uint8_t{3}, uint8_t{5}, uint8_t{99}}) {
        auto resp = do_bar_fd(c.get(), bar, 1);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(-EINVAL, ret_of(resp.value())) << "bar " << int(bar);
        EXPECT_TRUE(resp.value().fds.empty());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GET_DEVICE_INFO
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(CtlSubsystemTest, DeviceInfoFields) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    auto resp = do_device_info(c.get(), 7);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(0, ret_of(resp.value()));
    EXPECT_EQ(7u, resp.value().header.sequence_id);
    slash_ioctl_device_info dev{};
    ASSERT_EQ(sizeof(dev), resp.value().payload.size());
    std::memcpy(&dev, resp.value().payload.data(), sizeof(dev));
    EXPECT_STREQ("0000:61:00.2", dev.bdf);
    EXPECT_EQ(0x10EEu, dev.vendor_id);
    EXPECT_EQ(0x50B6u, dev.device_id);
    EXPECT_EQ(0x10EEu, dev.subsystem_vendor_id);
    EXPECT_EQ(0x000eu, dev.subsystem_device_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// Unknown ioctl
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(CtlSubsystemTest, UnknownIoctlReturnsErrorWorkerSurvives) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    slash_sysemu_socket_header h{0xDEADBEEFu, 1, 0, 0};
    auto resp = send_request(c.get(), h, {}, {});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(-ENOSYS, ret_of(resp.value()));
    EXPECT_EQ(0xDEADBEEFu, resp.value().header.ioctl_op);

    // The same connection still serves the next request.
    auto resp2 = do_device_info(c.get(), 2);
    ASSERT_TRUE(resp2.has_value());
    EXPECT_EQ(0, ret_of(resp2.value()));
}

// ─────────────────────────────────────────────────────────────────────────────
// REMOVE / RESTORE
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(CtlSubsystemTest, RemoveUnlinksSocketAndForcesDisconnect) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    // Sanity: works before remove.
    ASSERT_TRUE(do_device_info(c.get(), 1).has_value());

    sub->remove();
    EXPECT_FALSE(sub->is_active());
    // Socket file is gone.
    struct stat st{};
    EXPECT_NE(0, ::stat(sock_path_.c_str(), &st));

    // The client's next request fails (forced disconnect).
    auto resp = do_device_info(c.get(), 2);
    EXPECT_FALSE(resp.has_value());

    // The borrowed BAR memfds still work.
    EXPECT_TRUE(bars_->user_region.write_u32(0, 0x1234u).has_value());
    auto rv = bars_->user_region.read_u32(0);
    ASSERT_TRUE(rv.has_value());
    EXPECT_EQ(0x1234u, rv.value());
}

TEST_F(CtlSubsystemTest, RestoreAfterRemove) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    { UniqueFd c = connect_client(sock_path_); ASSERT_TRUE(static_cast<bool>(c)); }
    sub->remove();

    // Re-setup: a fresh client connects and GET_BAR_INFO works again.
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c2 = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c2));
    auto resp = do_bar_info(c2.get(), 0, 1);
    ASSERT_TRUE(resp.has_value());
    slash_ioctl_bar_info info{};
    std::memcpy(&info, resp.value().payload.data(), sizeof(info));
    EXPECT_EQ(1u, info.usable);
    EXPECT_EQ(kUserRegionSize, info.length);
}

TEST_F(CtlSubsystemTest, RemoveIdempotent) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    sub->remove();
    sub->remove(); // no-op
    EXPECT_FALSE(sub->is_active());
}

TEST_F(CtlSubsystemTest, RemoveWithoutSetup) {
    auto sub = make_subsystem();
    sub->remove(); // no-op on never-setup
    EXPECT_FALSE(sub->is_active());
}

TEST_F(CtlSubsystemTest, MultipleRemoveRestoreCycles) {
    auto sub = make_subsystem();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(sub->setup().has_value()) << "cycle " << i;
        UniqueFd c = connect_client(sock_path_);
        ASSERT_TRUE(static_cast<bool>(c)) << "cycle " << i;
        auto resp = do_device_info(c.get(), 1);
        ASSERT_TRUE(resp.has_value()) << "cycle " << i;
        EXPECT_EQ(0, ret_of(resp.value()));
        sub->remove();
    }
    // No leftover socket file.
    struct stat st{};
    EXPECT_NE(0, ::stat(sock_path_.c_str(), &st));
}

// ─────────────────────────────────────────────────────────────────────────────
// Teardown safety
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(CtlSubsystemTest, DestructorWhileClientConnected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    ASSERT_TRUE(do_device_info(c.get(), 1).has_value());
    // Destroy the subsystem while the client is still connected — must not hang
    // or leak.
    sub.reset();
    // The client's next op now fails.
    EXPECT_FALSE(do_device_info(c.get(), 2).has_value());
}

TEST_F(CtlSubsystemTest, NoFdLeakAcrossSetupRemove) {
    int before = open_fd_count();
    {
        auto sub = make_subsystem();
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(sub->setup().has_value());
            UniqueFd c = connect_client(sock_path_);
            ASSERT_TRUE(static_cast<bool>(c));
            ASSERT_TRUE(do_bar_fd(c.get(), 2, 1).has_value());
            sub->remove();
        }
    }
    int after = open_fd_count();
    // Allow small slack for lazily-created runtime fds; the point is no growth
    // proportional to the loop count.
    EXPECT_LE(after, before + 2) << "before=" << before << " after=" << after;
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrency
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(CtlSubsystemTest, ManyConcurrentClients) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());

    constexpr int kClients      = 16;
    constexpr int kReqPerClient = 40;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kClients; ++t) {
        threads.emplace_back([&, t] {
            UniqueFd c = connect_client(sock_path_);
            if (!c) { failures.fetch_add(1); return; }
            for (int i = 0; i < kReqPerClient; ++i) {
                uint32_t seq = static_cast<uint32_t>(t * 1000 + i);
                // Rotate across all three ioctls.
                Result<ReceivedMessage> r =
                    (i % 3 == 0) ? do_bar_info(c.get(), 0, seq)
                    : (i % 3 == 1) ? do_bar_fd(c.get(), 4, seq)
                                   : do_device_info(c.get(), seq);
                if (!r) { failures.fetch_add(1); return; }
                if (r.value().header.sequence_id != seq) { failures.fetch_add(1); return; }
                if (r.value().header.ioctl_op !=
                    ((i % 3 == 0) ? kSlashCtldevIoctlGetBarInfo
                     : (i % 3 == 1) ? kSlashCtldevIoctlGetBarFd
                                    : kSlashCtldevIoctlGetDeviceInfo)) {
                    failures.fetch_add(1);
                    return;
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(0, failures.load());
}

TEST_F(CtlSubsystemTest, RemoveWhileClientsBusy) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());

    std::atomic<bool> go{true};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            UniqueFd c = connect_client(sock_path_);
            if (!c) return;
            uint32_t seq = 1;
            while (go.load()) {
                auto r = do_device_info(c.get(), seq++);
                if (!r) break; // forced disconnect on remove()
            }
        });
    }
    std::this_thread::sleep_for(50ms);
    sub->remove(); // must not deadlock with busy workers
    go.store(false);
    for (auto& th : threads) th.join();
    EXPECT_FALSE(sub->is_active());
}

TEST_F(CtlSubsystemTest, ConnectionCountTracksLiveConnections) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    EXPECT_EQ(0u, sub->connection_count());

    UniqueFd c1 = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c1));
    // Issue a request so the listener has definitely accepted + registered it.
    ASSERT_TRUE(do_device_info(c1.get(), 1).has_value());

    // Wait briefly for the count to reflect the registered connection.
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (sub->connection_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    EXPECT_GE(sub->connection_count(), 1u);

    // Close the client; after the peer closes, the worker exits and (on the next
    // accept-driven reap, or at remove()) the count drops.  remove() must bring
    // it to zero regardless.
    c1.reset();
    sub->remove();
    EXPECT_EQ(0u, sub->connection_count());
}

// ─────────────────────────────────────────────────────────────────────────────
// ABI wire-layout sanity (matches the kernel UAPI natural layout)
// ─────────────────────────────────────────────────────────────────────────────

// ═════════════════════════════════════════════════════════════════════════════
// ADVERSARY PROBES (Step 9)
//
// Every probe below was devised by the adversary agent to hunt for fd/thread
// leaks, lifecycle races, protocol-abuse crashes, and spec-conformance gaps in
// the PF2 ctl subsystem.  Probes that pass are retained as hardening/regression
// tests; any probe that revealed a bug carries a comment naming the finding.
// ═════════════════════════════════════════════════════════════════════════════

// ── Probe: GET_BAR_FD must not leak the daemon's reopened fd across many calls ──
// The handler reopen()s the memfd, sends it via SCM_RIGHTS, and relies on the
// UniqueFd dtor to close the daemon's copy.  Loop it hundreds of times on ONE
// connection and assert the daemon's fd count does not grow proportionally.
TEST_F(CtlSubsystemTest, BarFdNoDaemonFdLeakUnderManyCalls) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    // Warm up so lazily-created runtime fds exist before the baseline.
    for (int i = 0; i < 5; ++i) {
        auto r = do_bar_fd(c.get(), 0, i + 1);
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(1u, r.value().fds.size());
        // Client-side fd is closed by ReceivedMessage dtor at loop end.
    }
    int before = open_fd_count();

    constexpr int kIters = 400;
    for (int i = 0; i < kIters; ++i) {
        auto r = do_bar_fd(c.get(), (i % 3 == 0) ? 0 : (i % 3 == 1) ? 2 : 4,
                           static_cast<uint32_t>(100 + i));
        ASSERT_TRUE(r.has_value()) << "iter " << i;
        ASSERT_EQ(1u, r.value().fds.size()) << "iter " << i;
        // The client's received fd is dropped (closed) each iteration.
    }
    int after = open_fd_count();
    // Daemon must not accumulate fds; allow tiny slack for allocator/runtime.
    EXPECT_LE(after, before + 3) << "before=" << before << " after=" << after
                                << " (daemon leaking reopened BAR fds?)";
}

// ── Probe: the flock-conflict distinct-description property holds for ALL BARs ──
// The existing test only proves it for BAR 0.  A description that is NOT distinct
// (e.g. dup instead of reopen) would silently break exclusion for a BAR.
TEST_F(CtlSubsystemTest, BarFdDistinctDescriptionForAllBars) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    struct Case { uint8_t bar; BarKind kind; };
    const Case cases[] = {
        {0, BarKind::UserRegion},
        {2, BarKind::ServiceLayer},
        {4, BarKind::ClockWizard},
    };
    uint32_t seq = 1;
    for (const auto& tc : cases) {
        auto resp = do_bar_fd(c.get(), tc.bar, seq++);
        ASSERT_TRUE(resp.has_value()) << "bar " << int(tc.bar);
        ASSERT_EQ(1u, resp.value().fds.size()) << "bar " << int(tc.bar);
        int user_fd   = resp.value().fds[0].get();
        int daemon_fd = bars_->by_kind(tc.kind).fd();

        ASSERT_EQ(0, ::flock(daemon_fd, LOCK_EX)) << "bar " << int(tc.bar);
        int rc = ::flock(user_fd, LOCK_EX | LOCK_NB);
        EXPECT_EQ(-1, rc) << "bar " << int(tc.bar) << " (description not distinct!)";
        EXPECT_TRUE(errno == EWOULDBLOCK || errno == EAGAIN) << "bar " << int(tc.bar);
        ::flock(daemon_fd, LOCK_UN);
        EXPECT_EQ(0, ::flock(user_fd, LOCK_EX | LOCK_NB)) << "bar " << int(tc.bar);
        ::flock(user_fd, LOCK_UN);
    }
}

// ── Probe: a second GET_BAR_FD on the same BAR yields a distinct, working fd ────
// Two consecutive fds for the same BAR must be independent descriptions (their
// flocks conflict) and both map the same underlying inode.
TEST_F(CtlSubsystemTest, BarFdSecondCallDistinctWorkingDescription) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    auto r1 = do_bar_fd(c.get(), 0, 1);
    auto r2 = do_bar_fd(c.get(), 0, 2);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_EQ(1u, r1.value().fds.size());
    ASSERT_EQ(1u, r2.value().fds.size());
    int fd1 = r1.value().fds[0].get();
    int fd2 = r2.value().fds[0].get();
    EXPECT_NE(fd1, fd2);

    // Distinct descriptions: an exclusive lock on fd1 conflicts with fd2.
    ASSERT_EQ(0, ::flock(fd1, LOCK_EX));
    int rc = ::flock(fd2, LOCK_EX | LOCK_NB);
    EXPECT_EQ(-1, rc);
    EXPECT_TRUE(errno == EWOULDBLOCK || errno == EAGAIN);
    ::flock(fd1, LOCK_UN);

    // Both map the same inode: a write via fd1 is visible via fd2.
    void* p1 = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
    void* p2 = ::mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd2, 0);
    ASSERT_NE(MAP_FAILED, p1);
    ASSERT_NE(MAP_FAILED, p2);
    const uint32_t sentinel = 0xA5A5F00Du;
    std::memcpy(static_cast<uint8_t*>(p1) + 0x80, &sentinel, sizeof(sentinel));
    uint32_t seen = 0;
    std::memcpy(&seen, static_cast<uint8_t*>(p2) + 0x80, sizeof(seen));
    EXPECT_EQ(sentinel, seen);
    ::munmap(p1, 4096);
    ::munmap(p2, 4096);
}

// ── Probe: the fd handed to the user is CLOEXEC (transport uses MSG_CMSG_CLOEXEC) ─
// Architecture: GET_BAR_FD flags field documents "Only O_CLOEXEC is honoured".
// The received fd on the client side must carry FD_CLOEXEC.
TEST_F(CtlSubsystemTest, BarFdReceivedFdIsCloexec) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    auto r = do_bar_fd(c.get(), 0, 1);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(1u, r.value().fds.size());
    int fd    = r.value().fds[0].get();
    int flags = ::fcntl(fd, F_GETFD);
    ASSERT_NE(-1, flags);
    EXPECT_TRUE(flags & FD_CLOEXEC) << "received BAR fd is not CLOEXEC";
}

// ── Probe: GET_BAR_INFO over the full __u8 bar_number range never crashes and ──
//    reports usable==1 ONLY for 0/2/4, in_use always 0, start_address always 0.
TEST_F(CtlSubsystemTest, BarInfoFullByteRange) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    for (int b = 0; b <= 255; ++b) {
        auto resp = do_bar_info(c.get(), static_cast<uint8_t>(b),
                                static_cast<uint32_t>(b + 1));
        ASSERT_TRUE(resp.has_value()) << "bar " << b;
        slash_ioctl_bar_info info{};
        ASSERT_EQ(sizeof(info), resp.value().payload.size()) << "bar " << b;
        std::memcpy(&info, resp.value().payload.data(), sizeof(info));
        bool present = (b == 0 || b == 2 || b == 4);
        EXPECT_EQ(present ? 1u : 0u, info.usable) << "bar " << b;
        EXPECT_EQ(0u, info.in_use) << "bar " << b;
        EXPECT_EQ(0u, info.start_address) << "bar " << b;
        if (!present) { EXPECT_EQ(0u, info.length) << "bar " << b; }
    }
}

// ── Probe: a truncated request (header only, no arg struct) must not OOB-read ──
//    and must be REJECTED with -EINVAL (the daemon does not fabricate a default
//    bar_number=0 response for a malformed request).  ASan would flag any OOB
//    read.  The worker must survive and serve the next request.
TEST_F(CtlSubsystemTest, TruncatedRequestHeaderOnly) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    // Send only the header for each ioctl (empty payload) → -EINVAL, no payload.
    for (uint32_t op : {kSlashCtldevIoctlGetBarInfo, kSlashCtldevIoctlGetBarFd,
                        kSlashCtldevIoctlGetDeviceInfo}) {
        slash_sysemu_socket_header h{op, 1, 0, 0};
        auto resp = send_request(c.get(), h, {}, {});
        ASSERT_TRUE(resp.has_value()) << "op " << op;
        EXPECT_EQ(op, resp.value().header.ioctl_op) << "op " << op;
        EXPECT_EQ(-EINVAL, ret_of(resp.value())) << "op " << op;
        EXPECT_TRUE(resp.value().fds.empty()) << "op " << op;
    }
    // Connection still serves a normal request afterwards.
    auto ok = do_device_info(c.get(), 99);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(0, ret_of(ok.value()));
}

// ── Probe: a payload SMALLER than the struct but non-empty (short read) ────────
//    The handler rejects it with -EINVAL rather than defaulting the missing
//    fields — a malformed request must not be silently accepted, and must not
//    read OOB (ASan catches any overrun).
TEST_F(CtlSubsystemTest, ShortPayloadSmallerThanStruct) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    // 3 bytes of payload for GET_BAR_INFO (struct is 24 bytes) → -EINVAL.
    std::array<uint8_t, 3> stub{0x00, 0x02, 0x04};
    slash_sysemu_socket_header h{kSlashCtldevIoctlGetBarInfo, 1, 0, 0};
    auto resp = send_request(c.get(), h, std::span<const uint8_t>(stub), {});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(-EINVAL, ret_of(resp.value()));

    // Same for GET_BAR_FD → -EINVAL, no fd.
    slash_sysemu_socket_header h2{kSlashCtldevIoctlGetBarFd, 2, 0, 0};
    auto resp2 = send_request(c.get(), h2, std::span<const uint8_t>(stub), {});
    ASSERT_TRUE(resp2.has_value());
    EXPECT_EQ(-EINVAL, ret_of(resp2.value()));
    EXPECT_TRUE(resp2.value().fds.empty());

    // Worker survives → a well-formed request still works.
    auto ok = do_bar_info(c.get(), 0, 3);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(0, ret_of(ok.value()));
}

// ── Probe: an oversized payload (> kMaxPayloadBytes) is rejected cleanly ───────
//    recv on the daemon side flags MSG_TRUNC → Protocol error → worker closes the
//    connection.  Must not crash; must not wedge the listener (a fresh client
//    still works).
TEST_F(CtlSubsystemTest, OversizedPayloadClosesConnCleanly) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    // Send a datagram larger than kMaxPayloadBytes so the daemon's recv sees
    // MSG_TRUNC.  Use a raw sendmsg since send_request caps payload sizes.
    std::vector<uint8_t> big(kMaxPayloadBytes + sizeof(slash_sysemu_socket_header) + 4096, 0xEE);
    slash_sysemu_socket_header h{kSlashCtldevIoctlGetDeviceInfo, 1, 0, 0};
    std::memcpy(big.data(), &h, sizeof(h));
    iovec iov{big.data(), big.size()};
    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;
    ssize_t sent = ::sendmsg(c.get(), &msg, MSG_NOSIGNAL);
    // The send itself may succeed (kernel buffers it) or fail with EMSGSIZE.
    (void)sent;

    // The daemon worker drops this connection.  A fresh client must still work,
    // proving the listener is unharmed.
    UniqueFd c2 = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c2));
    auto ok = do_device_info(c2.get(), 1);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(0, ret_of(ok.value()));
}

// ── Probe: sending SCM_RIGHTS fds on an ioctl that expects none must not leak ──
//    The received fds land in msg.fds (UniqueFd) and are dropped/closed.  Loop it
//    and assert the daemon's fd count is stable.
TEST_F(CtlSubsystemTest, UnexpectedScmRightsNoLeak) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    // Warm up.
    ASSERT_TRUE(do_device_info(c.get(), 1).has_value());
    int before = open_fd_count();

    for (int i = 0; i < 200; ++i) {
        // Attach a throwaway fd (dup of stdin) to a GET_DEVICE_INFO request.
        int extra = ::dup(0);
        ASSERT_GE(extra, 0);
        std::array<int, 1> fds{extra};
        slash_ioctl_device_info dev{};
        dev.size = sizeof(dev);
        slash_sysemu_socket_header h{kSlashCtldevIoctlGetDeviceInfo,
                                  static_cast<uint32_t>(2 + i), 0, 0};
        std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&dev), sizeof(dev));
        auto resp = send_request(c.get(), h, payload, std::span<const int>(fds));
        ::close(extra); // close our copy; the daemon must close ITS copy too
        ASSERT_TRUE(resp.has_value()) << "iter " << i;
        EXPECT_EQ(0, ret_of(resp.value()));
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 3) << "before=" << before << " after=" << after
                                << " (daemon leaking unexpected SCM_RIGHTS fds?)";
}

// ── Probe: client sends then immediately closes; the worker must exit cleanly ──
//    Churn many short connections; assert no thread/fd accumulation.  Also
//    exercises the listener's opportunistic reap path.
TEST_F(CtlSubsystemTest, ShortConnectionChurnNoAccumulation) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());

    int before = open_fd_count();
    constexpr int kConns = 300;
    for (int i = 0; i < kConns; ++i) {
        UniqueFd c = connect_client(sock_path_);
        ASSERT_TRUE(static_cast<bool>(c)) << "conn " << i;
        // One request, then close immediately.
        auto r = do_device_info(c.get(), 1);
        ASSERT_TRUE(r.has_value()) << "conn " << i;
        // c closes here.
    }

    // Give the listener a moment to reap done connections via a final accept-less
    // window; then a fresh connection triggers reap_finished_connections().
    for (int i = 0; i < 3; ++i) {
        UniqueFd c = connect_client(sock_path_);
        ASSERT_TRUE(static_cast<bool>(c));
        ASSERT_TRUE(do_device_info(c.get(), 1).has_value());
        std::this_thread::sleep_for(20ms);
    }

    // connection_count must not have grown to O(kConns): stale done entries are
    // reaped.  A handful of not-yet-reaped entries is acceptable.
    EXPECT_LT(sub->connection_count(), 20u)
        << "connection map accumulating stale entries: " << sub->connection_count();

    int after = open_fd_count();
    // The daemon must not hold O(kConns) connection fds.
    EXPECT_LE(after, before + 20) << "before=" << before << " after=" << after
                                  << " (daemon leaking connection fds/threads?)";
    sub->remove();
    EXPECT_EQ(0u, sub->connection_count());
}

// ── Probe: forced disconnect while a client is BLOCKED in recv for a response ──
//    The client sends a request whose response the daemon has produced, then the
//    PF is REMOVEd.  A client blocked in recv must get a transport failure, never
//    a hang or a wrong/partial response.
TEST_F(CtlSubsystemTest, ForcedDisconnectWhileClientBlockedInRecv) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    // Make sure the connection is established+registered.
    ASSERT_TRUE(do_device_info(c.get(), 1).has_value());

    // Client thread blocks in recv_message with no pending request.
    std::atomic<bool> got_result{false};
    std::atomic<bool> was_error{false};
    std::thread reader([&] {
        auto r = recv_message(c.get());
        was_error.store(!r.has_value());
        got_result.store(true);
    });

    std::this_thread::sleep_for(50ms);
    sub->remove(); // forced disconnect

    // The blocked recv must return (with an error) promptly, not hang.
    auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
    while (!got_result.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(got_result.load()) << "blocked recv() hung after remove()!";
    EXPECT_TRUE(was_error.load()) << "recv() returned success after forced disconnect";
    reader.join();
}

// ── Probe: a connected-but-silent client (never sends) is cleaned up by remove ─
//    Its worker is blocked in recv; remove() must shutdown() the conn fd, unblock
//    it, and join — no hang, connection_count back to 0.
TEST_F(CtlSubsystemTest, SilentClientWorkerCleanedUpByRemove) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    // Wait until the worker is registered (connection accepted).  We cannot send
    // a request (that would let the worker loop); instead poll connection_count.
    auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
    while (sub->connection_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_GE(sub->connection_count(), 1u) << "silent connection never registered";

    // remove() must unblock the silent worker's recv and join it.
    sub->remove();
    EXPECT_EQ(0u, sub->connection_count());
    EXPECT_FALSE(sub->is_active());
}

// ── Probe: response mirrors sequence_id AND ioctl_op exactly, return_value sign ─
//    Spec: sequence_id mirrored; ioctl_op mirrored; return_value is -errno on
//    error (interpreted as signed) and >=0 on success.
TEST_F(CtlSubsystemTest, ResponseMirrorsHeaderAndErrnoSign) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    // Success case: return_value >= 0.
    auto ok = do_bar_info(c.get(), 0, 0x12345678u);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(0x12345678u, ok.value().header.sequence_id);
    EXPECT_EQ(kSlashCtldevIoctlGetBarInfo, ok.value().header.ioctl_op);
    EXPECT_GE(ret_of(ok.value()), 0);

    // Error case: absent BAR fd → return_value == -EINVAL (negative when signed).
    auto err = do_bar_fd(c.get(), 1, 0x0BADF00Du);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(0x0BADF00Du, err.value().header.sequence_id);
    EXPECT_EQ(kSlashCtldevIoctlGetBarFd, err.value().header.ioctl_op);
    EXPECT_EQ(-EINVAL, ret_of(err.value()));
    EXPECT_LT(ret_of(err.value()), 0);
}

// ── Probe: N clients each doing GET_BAR_FD simultaneously get distinct, valid ──
//    descriptions with no crossed responses and no fd double-close.
TEST_F(CtlSubsystemTest, ConcurrentGetBarFdDistinctDescriptions) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());

    constexpr int kClients = 12;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kClients; ++t) {
        threads.emplace_back([&, t] {
            UniqueFd c = connect_client(sock_path_);
            if (!c) { failures.fetch_add(1); return; }
            for (int i = 0; i < 20; ++i) {
                uint32_t seq = static_cast<uint32_t>(t * 1000 + i);
                auto r = do_bar_fd(c.get(), 0, seq);
                if (!r) { failures.fetch_add(1); return; }
                if (r.value().header.sequence_id != seq) { failures.fetch_add(1); return; }
                if (r.value().header.ioctl_op != kSlashCtldevIoctlGetBarFd) {
                    failures.fetch_add(1); return;
                }
                if (r.value().fds.size() != 1) { failures.fetch_add(1); return; }
                // The fd must be a valid, mappable memfd of the user-region size.
                struct stat st{};
                if (::fstat(r.value().fds[0].get(), &st) != 0 ||
                    st.st_size != static_cast<off_t>(kUserRegionSize)) {
                    failures.fetch_add(1); return;
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(0, failures.load());
}

// ── Probe: setup() over a pre-existing DIRECTORY at the socket path fails clean ─
//    unlink() cannot remove a non-empty directory; bind() then fails.  setup()
//    must return an error and leave the subsystem inactive (no leaked thread/fd).
TEST_F(CtlSubsystemTest, SetupOverExistingDirectoryFailsClean) {
    // Create a directory at the socket path.
    std::filesystem::create_directories(sock_path_);
    ASSERT_TRUE(std::filesystem::is_directory(sock_path_));

    int before = open_fd_count();
    auto sub = make_subsystem();
    auto r = sub->setup();
    EXPECT_FALSE(r.has_value()) << "setup succeeded over a directory?!";
    EXPECT_FALSE(sub->is_active());
    int after = open_fd_count();
    EXPECT_LE(after, before + 1) << "setup leaked an fd on the failure path";

    // Clean up for TearDown.
    std::filesystem::remove(sock_path_);
}

// ── Probe: destructor racing an in-flight accept + rapid setup/remove churn ────
//    Stress the listener/remove lifecycle: many setup→(client)→remove cycles
//    with the client connecting concurrently, under ASan for thread/fd safety.
TEST_F(CtlSubsystemTest, SetupRemoveChurnWithConcurrentConnect) {
    auto sub = make_subsystem();
    int before = open_fd_count();
    // A single non-retrying connect attempt (unlike connect_client, which retries
    // for kIoTimeout).  Retrying here would stall the churn 5s/cycle against the
    // just-removed socket — a harness artifact, not a daemon property.
    auto try_connect_once = [&]() -> UniqueFd {
        UniqueFd fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
        if (!fd) return {};
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, sock_path_.c_str(), sock_path_.size());
        if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            set_rcv_timeout(fd.get(), 500ms);
            return fd;
        }
        return {};
    };
    for (int cycle = 0; cycle < 30; ++cycle) {
        ASSERT_TRUE(sub->setup().has_value()) << "cycle " << cycle;
        std::atomic<bool> stop{false};
        std::thread connector([&] {
            while (!stop.load()) {
                UniqueFd c = try_connect_once();
                if (c) { (void)do_device_info(c.get(), 1); }
                else { std::this_thread::sleep_for(1ms); }
            }
        });
        std::this_thread::sleep_for(3ms);
        sub->remove();
        stop.store(true);
        connector.join();
        EXPECT_EQ(0u, sub->connection_count()) << "cycle " << cycle;
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 3) << "before=" << before << " after=" << after;
}

// ── Probe: connect-storm racing repeated remove() — no missed/leaked worker ────
//    The listener registers a worker under conns_mtx_ after accept().  If a
//    connection were accepted but never registered while remove() was moving the
//    map out, its thread would be neither shutdown()n nor joined — a leaked,
//    joinable std::thread whose ~thread() calls std::terminate().  Hammer the
//    accept/register vs remove/move-out window many times; every remove() must
//    return connection_count()==0 and the process must not abort.
TEST_F(CtlSubsystemTest, ConnectStormRacingRemoveNoLeakedWorker) {
    auto sub = make_subsystem();
    auto try_connect_once = [&]() -> UniqueFd {
        UniqueFd fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
        if (!fd) return {};
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, sock_path_.c_str(), sock_path_.size());
        if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        return {};
    };
    for (int cycle = 0; cycle < 40; ++cycle) {
        ASSERT_TRUE(sub->setup().has_value()) << "cycle " << cycle;
        std::atomic<bool> stop{false};
        std::vector<std::thread> storm;
        for (int t = 0; t < 4; ++t) {
            storm.emplace_back([&] {
                while (!stop.load()) {
                    UniqueFd c = try_connect_once();
                    (void)c; // do not send: maximise the accept-before-register
                             // window on the daemon side, then drop immediately.
                }
            });
        }
        // remove() as soon as connections are landing, to hit the register window.
        std::this_thread::sleep_for(2ms);
        sub->remove();
        stop.store(true);
        for (auto& th : storm) th.join();
        EXPECT_EQ(0u, sub->connection_count()) << "cycle " << cycle;
        EXPECT_FALSE(sub->is_active()) << "cycle " << cycle;
    }
}

// ── Probe: fd-number reuse across sequential short connections (emplace-key) ────
//    conns_ is keyed by the raw connection fd.  A worker closes its fd on exit;
//    the listener may then accept() the SAME fd number for a new connection.  If
//    the reaper had not removed the old (done) entry first, emplace() would fail
//    silently and the new worker would be untracked (never shutdown/joined).
//    Serve a GET_BAR_FD on each short connection (forces the daemon to reopen and
//    then close a memfd fd, churning fd numbers) and verify every connection is
//    served and the map/fd count stay bounded.
TEST_F(CtlSubsystemTest, FdNumberReuseAcrossShortConnections) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    int before = open_fd_count();
    for (int i = 0; i < 250; ++i) {
        UniqueFd c = connect_client(sock_path_);
        ASSERT_TRUE(static_cast<bool>(c)) << "conn " << i;
        auto r = do_bar_fd(c.get(), 0, 1);
        ASSERT_TRUE(r.has_value()) << "conn " << i;
        ASSERT_EQ(1u, r.value().fds.size()) << "conn " << i;
        // c and the received fd close here.
    }
    // Trigger a final reap and confirm the map does not hold ~250 stale entries.
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    ASSERT_TRUE(do_device_info(c.get(), 1).has_value());
    EXPECT_LT(sub->connection_count(), 20u)
        << "conns_ accumulating (emplace-key collision leaking entries?)";
    int after = open_fd_count();
    EXPECT_LE(after, before + 20) << "before=" << before << " after=" << after;
    sub->remove();
    EXPECT_EQ(0u, sub->connection_count());
}

// ── Regression: remove() must NOT shutdown() done entries (fd-reuse hazard) ─────
//
// Root cause: remove() iterated conns_ and called ::shutdown() on every map key
// without checking the done flag.  A worker sets done=true under conns_mtx_ then
// closes its fd (UniqueFd dtor).  In the original code the done-check and
// shutdown() ran OUTSIDE the lock, so a worker could set done + close its fd in
// the window between remove() reading done==false and calling shutdown() — the fd
// number gets reused by a new live connection, and shutdown() tears it down.
//
// The fix: done-check + shutdown() are performed UNDER conns_mtx_.  Invariant:
// while the lock is held a !done worker cannot have closed its fd (it must acquire
// the same lock to set done before the UniqueFd dtor runs), so every !done entry's
// fd is guaranteed open.  done==true entries are skipped — their fd may be reused.
//
// How this test constructs the scenario:
//   1. Subsystem A: many short-lived connections leave stale done entries; their
//      fd numbers are freed when the workers close, available for OS reuse.
//   2. Subsystem B: a fresh subsystem whose accept() may reuse those fd numbers.
//   3. A.remove(): OLD code → shutdown() on all conns_ keys including reused ones,
//      tearing down B's live connection.  NEW code → skips done entries; B unaffected.
//   4. Assert B's live client can still exchange a request/response.
//
// 50 cycles; probabilistic under OLD code, always safe under NEW code.
TEST_F(CtlSubsystemTest, RemoveDoesNotShutdownStaleOrReusedFdNumbers) {
    auto bars_b = make_standard_bars();
    ASSERT_TRUE(bars_b.has_value());
    BarSet bars_b_set = std::move(bars_b.value());

    // Subsystem B runs on a distinct path alongside subsystem A.
    std::string sock_b = (base_ / "slash_ctl_b").string();

    constexpr int kCycles    = 50;
    constexpr int kStormConns = 12; // short connections to A per cycle

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        // ── Phase 1: start A and hammer it with short connections ──────────────
        auto sub_a = make_subsystem();
        ASSERT_TRUE(sub_a->setup().has_value()) << "A setup cycle " << cycle;

        // Open many short connections on A so that done entries accumulate;
        // these release their fd numbers when the workers exit (OLD code) making
        // those numbers available to be reused.
        for (int i = 0; i < kStormConns; ++i) {
            UniqueFd c = connect_client(sock_path_);
            if (c) { (void)do_device_info(c.get(), 1); }
            // c closes here → server worker sees EOF and sets done=true.
        }

        // Give the workers time to exit so done entries accumulate in conns_
        // (under OLD code their fds are now freed; under NEW code they are kept
        // open by Connection::fd).  We do NOT call reap_finished_connections()
        // (it's private); just wait a moment.
        std::this_thread::sleep_for(15ms);

        // ── Phase 2: start B and make one live connection ──────────────────────
        CtlSubsystem sub_b(sock_b, "0000:62:00", bars_b_set);
        ASSERT_TRUE(sub_b.setup().has_value()) << "B setup cycle " << cycle;

        // B's accept() is now competing for fd numbers.  Under OLD code, the
        // freed fd numbers from A's workers are the most likely candidates.
        UniqueFd live = connect_client(sock_b);
        ASSERT_TRUE(static_cast<bool>(live)) << "B connect cycle " << cycle;

        // ── Phase 3: call A.remove() ───────────────────────────────────────────
        // OLD code: iterates all conns_ keys (including stale done entries whose
        //   fd numbers may now be B's live connections) and calls shutdown() on
        //   each → B's live connection is torn down.
        // NEW code: skips done==true entries; only shuts down live entries (which
        //   hold their own fd in Connection::fd, guaranteed open and valid).
        sub_a->remove();
        ASSERT_FALSE(sub_a->is_active()) << "cycle " << cycle;

        // ── Phase 4: assert B's live connection survived ───────────────────────
        // Exchange one GET_DEVICE_INFO request/response.  If shutdown() was called
        // on B's live fd, recv_message will return an error (EOF/EINVAL).
        auto resp = do_device_info(live.get(), 42);
        EXPECT_TRUE(resp.has_value())
            << "cycle " << cycle
            << ": B's live connection was disrupted by A.remove() — "
               "stale done entry shutdown() hazard detected";
        if (resp.has_value()) {
            EXPECT_EQ(0, static_cast<int32_t>(resp.value().header.return_value))
                << "cycle " << cycle << ": unexpected return_value";
        }

        sub_b.remove();
    }
}

// ── Probe: worker finishes concurrently with remove() — no wrong-fd shutdown ─────
//
// Exercises the narrow window where a worker's peer closes at exactly the same
// time remove() is running.  With the under-lock done-check the race has exactly
// two safe outcomes:
//   a) remove() sees done==false  → shutdown() fires; worker wakes, tries to set
//      done (blocks on the lock until remove() releases it), finds the map cleared,
//      returns — no-op.  Fd is closed by the worker's UniqueFd on return.
//   b) remove() sees done==true   → shutdown() skipped; fd already closed/closing.
// Both are correct; neither can hit a recycled fd.
//
// The test hammers many setup→connect→remove cycles where the single connection
// peer closes simultaneously.  ASan/TSan must be clean; connection_count must
// reach 0 after each remove().
TEST_F(CtlSubsystemTest, ConcurrentWorkerFinishAndRemoveIsRaceFree) {
    constexpr int kCycles = 200;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        auto sub = make_subsystem();
        ASSERT_TRUE(sub->setup().has_value()) << "cycle " << cycle;

        // Connect one client and immediately let it go out of scope on a
        // background thread, so the peer-close races with sub->remove() below.
        UniqueFd c = connect_client(sock_path_);
        ASSERT_TRUE(static_cast<bool>(c)) << "cycle " << cycle;

        // Confirm the connection is registered (worker spawned).
        ASSERT_TRUE(do_device_info(c.get(), 1).has_value()) << "cycle " << cycle;

        // Release the client on a thread that races with remove().
        std::thread closer([cc = std::move(c)] {
            // cc closes here; the worker on the daemon side sees EOF and races
            // to set done + close its fd at the same time remove() is running.
        });

        // remove() runs concurrently with the closer thread — the under-lock
        // done-check must handle both orderings (done seen before/after close).
        sub->remove();
        closer.join();

        EXPECT_FALSE(sub->is_active()) << "cycle " << cycle;
        EXPECT_EQ(0u, sub->connection_count()) << "cycle " << cycle;
    }
}

TEST(CtlIoctlAbiTest, WireSizesMatchKernelNaturalLayout) {
    // These structs come from the official libslash UAPI header; their natural
    // (kernel) sizes are what libslash puts on the wire.
    EXPECT_EQ(24u, sizeof(slash_ioctl_bar_info));
    EXPECT_EQ(24u, sizeof(slash_ioctl_bar_fd_request));
    EXPECT_EQ(44u, sizeof(slash_ioctl_device_info));
    // Command numbers encode _IOWR('v', nr, sizeof(T)): low byte = nr, next = 'v'.
    EXPECT_EQ(0x30u, kSlashCtldevIoctlGetBarInfo & 0xFFu);
    EXPECT_EQ('v', (kSlashCtldevIoctlGetBarInfo >> 8) & 0xFFu);
    EXPECT_EQ(0x31u, kSlashCtldevIoctlGetBarFd & 0xFFu);
    EXPECT_EQ(0x32u, kSlashCtldevIoctlGetDeviceInfo & 0xFFu);
}

} // namespace
} // namespace slash_sysemu
