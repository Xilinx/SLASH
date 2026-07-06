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

#include "mock_model_server.h"
#include "model_client.h"
#include "qdma_ioctls.h"
#include "qdma_subsystem.h"
#include "transport.h"
#include "vbin_store.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace slash_emu {
namespace {

using namespace std::chrono_literals;
using slash_emu::test::MockModelServer;

// A short deadline so a hung test fails fast rather than blocking ctest.
constexpr auto kIoTimeout = 5s;

// HBM/DDR-ish device address used for non-reconfig transfers (NOT the aperture).
constexpr uint64_t kDevAddr = 0x40000000ull;

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
        for (auto& e : std::filesystem::directory_iterator("/proc/self/fd")) {
            (void)e;
            ++count;
        }
    } catch (...) {}
    return count;
}

std::string unique_endpoint() {
    static std::atomic<int> counter{0};
    return "ipc:///tmp/slash_emu_qdma_model_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter++);
}

int32_t ret_of(const ReceivedMessage& m) {
    return static_cast<int32_t>(m.header.return_value);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture: a QdmaSubsystem backed by a MockModelServer + real VbinStore
// ─────────────────────────────────────────────────────────────────────────────

class QdmaSubsystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = std::filesystem::temp_directory_path() /
                ("slash_qdma_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter_++));
        std::filesystem::create_directories(base_);
        sock_path_ = (base_ / "slash_qdma_ctl0").string();

        endpoint_ = unique_endpoint();
        server_   = std::make_unique<MockModelServer>(endpoint_);
        // A moderate model timeout.  Real (mock) requests complete in
        // microseconds even under sanitizer load, so 5s is ample headroom for a
        // correctly-serialised transfer; it also bounds how long the
        // dead-model probe waits for its -ENODEV.  (The concurrency risk is the
        // CLIENT recv timeout, which those tests set generously, not this one.)
        auto mc   = ModelClient::connect(endpoint_, 5000ms);
        if (!mc) { FAIL() << "model connect: " << mc.error().message; }
        model_ = std::make_unique<ModelClient>(std::move(mc.value()));

        vbin_ = std::make_unique<VbinStore>(base_ / "vbin", "0000:61:00");
        // No bootstrap needed: append_staging creates the staging file on demand,
        // but bootstrap ensures the directory exists.  Create the dir directly.
        std::filesystem::create_directories(vbin_->dir());
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }

    std::unique_ptr<QdmaSubsystem> make_subsystem() {
        return std::make_unique<QdmaSubsystem>(sock_path_, ::getuid(), ::getgid(),
                                               mode_t{0600}, "0000:61:00",
                                               *model_, *vbin_);
    }

    std::filesystem::path            base_;
    std::string                      sock_path_;
    std::string                      endpoint_;
    std::unique_ptr<MockModelServer> server_;
    std::unique_ptr<ModelClient>     model_;
    std::unique_ptr<VbinStore>       vbin_;
    static inline std::atomic<int>   counter_{0};
};

// ── Request builders (CTL endpoint) ──────────────────────────────────────────

Result<ReceivedMessage> do_info(int fd, uint32_t seq) {
    slash_qdma_info info{};
    info.size = sizeof(info);
    slash_emu_socket_header h{kSlashQdmaIoctlInfo, seq, 0, 0};
    std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&info), sizeof(info));
    return send_request(fd, h, p, {});
}

Result<ReceivedMessage> do_qpair_add(int fd, uint32_t mode, uint32_t dir_mask, uint32_t seq) {
    slash_qdma_qpair_add add{};
    add.size     = sizeof(add);
    add.mode     = mode;
    add.dir_mask = dir_mask;
    slash_emu_socket_header h{kSlashQdmaIoctlQpairAdd, seq, 0, 0};
    std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&add), sizeof(add));
    return send_request(fd, h, p, {});
}

Result<ReceivedMessage> do_q_op(int fd, uint32_t qid, uint32_t op, uint32_t seq) {
    slash_qdma_qpair_op qop{};
    qop.size = sizeof(qop);
    qop.qid  = qid;
    qop.op   = op;
    slash_emu_socket_header h{kSlashQdmaIoctlQOp, seq, 0, 0};
    std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&qop), sizeof(qop));
    return send_request(fd, h, p, {});
}

// QPAIR_GET_FD with a single legacy qid (qpair_count == 0).
Result<ReceivedMessage> do_get_fd(int fd, uint32_t qid, uint32_t seq) {
    slash_qdma_qpair_fd_request req{};
    req.size        = sizeof(req);
    req.qid         = qid;
    req.qpair_count = 0;
    slash_emu_socket_header h{kSlashQdmaIoctlQpairGetFd, seq, 0, 0};
    std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    return send_request(fd, h, p, {});
}

// QPAIR_GET_FD binding an explicit list of qids.
Result<ReceivedMessage> do_get_fd_list(int fd, const std::vector<uint32_t>& qids, uint32_t seq) {
    slash_qdma_qpair_fd_request req{};
    req.size        = sizeof(req);
    req.qpair_count = static_cast<uint32_t>(qids.size());
    for (std::size_t i = 0; i < qids.size() && i < SLASH_QDMA_FD_MAX_QPAIRS; ++i) {
        req.qpair_ids[i] = qids[i];
    }
    slash_emu_socket_header h{kSlashQdmaIoctlQpairGetFd, seq, 0, 0};
    std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    return send_request(fd, h, p, {});
}

Result<ReceivedMessage> do_buf_create(int fd, uint64_t length, uint32_t seq) {
    slash_qdma_buf_create req{};
    req.size   = sizeof(req);
    req.length = length;
    slash_emu_socket_header h{kSlashQdmaIoctlBufCreate, seq, 0, 0};
    std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    return send_request(fd, h, p, {});
}

// Issue a TRANSFER on an XFER fd.  @p subxfers describes the sub-transfers; the
// caller supplies the fds to send as ancillary data (buf_fd already holds the
// INDEX into that list).
Result<ReceivedMessage> do_transfer(int fd, const std::vector<slash_qdma_subxfer>& subxfers,
                                    const std::vector<int>& fds, uint32_t seq) {
    slash_qdma_transfer xfer{};
    xfer.size  = sizeof(xfer);
    xfer.count = static_cast<uint32_t>(subxfers.size());
    for (std::size_t i = 0; i < subxfers.size() && i < SLASH_QDMA_FD_MAX_QPAIRS; ++i) {
        xfer.xfers[i] = subxfers[i];
    }
    slash_emu_socket_header h{kSlashQdmaQpairIoctlTransfer, seq, 0, 0};
    std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&xfer), sizeof(xfer));
    return send_request(fd, h, p, std::span<const int>(fds));
}

// Convenience: create + START a MM qpair with both directions and return its qid.
uint32_t add_started_qpair(int fd, uint32_t& seq) {
    auto add = do_qpair_add(fd, kQdmaQModeMm, 0x3, seq++);
    EXPECT_TRUE(add.has_value());
    slash_qdma_qpair_add a{};
    std::memcpy(&a, add.value().payload.data(), sizeof(a));
    uint32_t qid = a.qid;
    auto st = do_q_op(fd, qid, SLASH_QDMA_QUEUE_OP_START, seq++);
    EXPECT_TRUE(st.has_value());
    EXPECT_EQ(0, ret_of(st.value()));
    return qid;
}

// Obtain a working XFER fd for a single started qpair; returns the fd.
UniqueFd open_xfer(int ctl_fd, uint32_t qid, uint32_t& seq) {
    auto r = do_get_fd(ctl_fd, qid, seq++);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(0, ret_of(r.value()));
    EXPECT_EQ(1u, r.value().fds.size());
    UniqueFd xfer(std::move(r.value().fds[0]));
    set_rcv_timeout(xfer.get(), 3000ms);
    return xfer;
}

// ═════════════════════════════════════════════════════════════════════════════
// INFO
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(QdmaSubsystemTest, InfoReturnsPf1BdfAndZeroedCaps) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    auto resp = do_info(c.get(), 7);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(0, ret_of(resp.value()));
    EXPECT_EQ(7u, resp.value().header.sequence_id);
    ASSERT_EQ(sizeof(slash_qdma_info), resp.value().payload.size());
    slash_qdma_info info{};
    std::memcpy(&info, resp.value().payload.data(), sizeof(info));
    EXPECT_EQ(0u, info.qsets_max);
    EXPECT_EQ(0u, info.msix_qvecs);
    EXPECT_EQ(0u, info.vf_max);
    EXPECT_EQ(0u, info.caps);
    EXPECT_STREQ("0000:61:00.1", info.bdf);
}

TEST_F(QdmaSubsystemTest, InfoTruncatedPayloadRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    slash_emu_socket_header h{kSlashQdmaIoctlInfo, 1, 0, 0};
    auto resp = send_request(c.get(), h, {}, {});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(-EINVAL, ret_of(resp.value()));
}

// ═════════════════════════════════════════════════════════════════════════════
// QPAIR_ADD + state machine
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(QdmaSubsystemTest, QpairAddAssignsIncrementingQids) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    uint32_t seq = 1;
    for (uint32_t expected = 0; expected < 5; ++expected) {
        auto r = do_qpair_add(c.get(), kQdmaQModeMm, 0x3, seq++);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(0, ret_of(r.value()));
        slash_qdma_qpair_add a{};
        std::memcpy(&a, r.value().payload.data(), sizeof(a));
        EXPECT_EQ(expected, a.qid);
    }
    EXPECT_EQ(5u, sub->qpair_count());
}

TEST_F(QdmaSubsystemTest, QpairAddStreamingModeUnsupported) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    auto r = do_qpair_add(c.get(), kQdmaQModeSt, 0x3, 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EOPNOTSUPP, ret_of(r.value()));
    EXPECT_EQ(0u, sub->qpair_count());
}

TEST_F(QdmaSubsystemTest, QpairAddCmptDirUnsupported) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    auto r = do_qpair_add(c.get(), kQdmaQModeMm, 0x4 /*CMPT*/, 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EOPNOTSUPP, ret_of(r.value()));
}

TEST_F(QdmaSubsystemTest, QpairAddNoDirectionRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    auto r = do_qpair_add(c.get(), kQdmaQModeMm, 0x0, 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EINVAL, ret_of(r.value()));
}

TEST_F(QdmaSubsystemTest, FullLifecycleAddStartGetFdCloseStopDel) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;

    // ADD -> Stopped
    auto add = do_qpair_add(c.get(), kQdmaQModeMm, 0x3, seq++);
    ASSERT_TRUE(add.has_value());
    slash_qdma_qpair_add a{};
    std::memcpy(&a, add.value().payload.data(), sizeof(a));
    uint32_t qid = a.qid;

    // GET_FD from Stopped must fail (needs Started).
    {
        auto r = do_get_fd(c.get(), qid, seq++);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(-EINVAL, ret_of(r.value()));
    }

    // START -> Started
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_START, seq++).value()));
    // START again is idempotent (Started -> Started).
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_START, seq++).value()));

    // GET_FD -> Used
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    ASSERT_TRUE(static_cast<bool>(xfer));

    // Close the transfer fd -> qpair returns to Started.
    xfer.reset();
    // Wait for the session worker to observe EOF and transition back.
    auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
    while (sub->session_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    EXPECT_EQ(0u, sub->session_count());

    // Now GET_FD should succeed again (proves Started, not stuck in Used).
    UniqueFd xfer2 = open_xfer(c.get(), qid, seq);
    ASSERT_TRUE(static_cast<bool>(xfer2));
    xfer2.reset();

    // STOP -> Stopped, then DEL -> removed.
    while (sub->session_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_STOP, seq++).value()));
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_DEL, seq++).value()));
    EXPECT_EQ(0u, sub->qpair_count());

    // Q_OP on a deleted qpair -> -EINVAL.
    EXPECT_EQ(-EINVAL, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_START, seq++).value()));
}

TEST_F(QdmaSubsystemTest, QOpUnknownQidAndUnknownOp) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    EXPECT_EQ(-EINVAL, ret_of(do_q_op(c.get(), 999, SLASH_QDMA_QUEUE_OP_START, seq++).value()));
    uint32_t qid = add_started_qpair(c.get(), seq);
    EXPECT_EQ(-EINVAL, ret_of(do_q_op(c.get(), qid, 0xDEAD, seq++).value()));
}

// ═════════════════════════════════════════════════════════════════════════════
// BUF_CREATE
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(QdmaSubsystemTest, BufCreateOnCtlReturnsMappableMemfd) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    const uint64_t    len  = page * 3;
    auto r = do_buf_create(c.get(), len, 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(0, ret_of(r.value()));
    ASSERT_EQ(1u, r.value().fds.size());
    slash_qdma_buf_create out{};
    std::memcpy(&out, r.value().payload.data(), sizeof(out));
    EXPECT_EQ(static_cast<uint32_t>(page), out.granule);
    EXPECT_EQ(static_cast<uint32_t>(SLASH_QDMA_TRANSFER_HINT_SINGLE_QPAIR), out.transfer_hint);

    int fd = r.value().fds[0].get();
    struct stat st{};
    ASSERT_EQ(0, ::fstat(fd, &st));
    EXPECT_EQ(static_cast<off_t>(len), st.st_size);

    void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(MAP_FAILED, p);
    // Round-trip a byte through the mapping.
    static_cast<uint8_t*>(p)[0]       = 0xAB;
    static_cast<uint8_t*>(p)[len - 1] = 0xCD;
    EXPECT_EQ(0xAB, static_cast<uint8_t*>(p)[0]);
    EXPECT_EQ(0xCD, static_cast<uint8_t*>(p)[len - 1]);
    ::munmap(p, len);
}

TEST_F(QdmaSubsystemTest, BufCreateOnXferEndpoint) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    auto r = do_buf_create(xfer.get(), page, 100);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(0, ret_of(r.value()));
    ASSERT_EQ(1u, r.value().fds.size());
    struct stat st{};
    ASSERT_EQ(0, ::fstat(r.value().fds[0].get(), &st));
    EXPECT_EQ(static_cast<off_t>(page), st.st_size);
}

TEST_F(QdmaSubsystemTest, BufCreateRejectsZeroAndNonPageMultiple) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    EXPECT_EQ(-EINVAL, ret_of(do_buf_create(c.get(), 0, 1).value()));
    EXPECT_EQ(-EINVAL, ret_of(do_buf_create(c.get(), 1, 2).value()));
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    EXPECT_EQ(-EINVAL, ret_of(do_buf_create(c.get(), page + 1, 3).value()));
}

TEST_F(QdmaSubsystemTest, BufCreateNoDaemonFdLeak) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    const std::size_t page = static_cast<std::size_t>(::getpagesize());

    // Warm up.
    for (int i = 0; i < 5; ++i) {
        auto r = do_buf_create(c.get(), page, static_cast<uint32_t>(i + 1));
        ASSERT_TRUE(r.has_value());
        ASSERT_EQ(1u, r.value().fds.size());
    }
    int before = open_fd_count();
    for (int i = 0; i < 400; ++i) {
        auto r = do_buf_create(c.get(), page, static_cast<uint32_t>(100 + i));
        ASSERT_TRUE(r.has_value()) << "iter " << i;
        ASSERT_EQ(1u, r.value().fds.size()) << "iter " << i;
        // The client's received fd is closed by ReceivedMessage dtor each iter.
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 3) << "before=" << before << " after=" << after
                                << " (daemon leaking memfds?)";
}

// ═════════════════════════════════════════════════════════════════════════════
// QPAIR_GET_FD + endpoint enforcement
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(QdmaSubsystemTest, GetFdRejectedOnXferEndpoint) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    // CTL-only ops on the XFER endpoint are rejected with -EINVAL.
    EXPECT_EQ(-EINVAL, ret_of(do_info(xfer.get(), 100).value()));
    EXPECT_EQ(-EINVAL, ret_of(do_qpair_add(xfer.get(), kQdmaQModeMm, 0x3, 101).value()));
    EXPECT_EQ(-EINVAL, ret_of(do_q_op(xfer.get(), qid, SLASH_QDMA_QUEUE_OP_STOP, 102).value()));
    EXPECT_EQ(-EINVAL, ret_of(do_get_fd(xfer.get(), qid, 103).value()));
}

TEST_F(QdmaSubsystemTest, TransferRejectedOnCtlEndpoint) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    slash_qdma_subxfer sx{};
    sx.qpair_index = 0;
    sx.direction   = SLASH_QDMA_XFER_H2C;
    sx.buf_fd      = 0;
    sx.length      = 0;
    int extra = ::dup(0);
    ASSERT_GE(extra, 0);
    auto r = do_transfer(c.get(), {sx}, {extra}, 1);
    ::close(extra);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EINVAL, ret_of(r.value()));
}

TEST_F(QdmaSubsystemTest, GetFdRequiresStartedState) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    // Add but do NOT start.
    auto add = do_qpair_add(c.get(), kQdmaQModeMm, 0x3, seq++);
    slash_qdma_qpair_add a{};
    std::memcpy(&a, add.value().payload.data(), sizeof(a));
    auto r = do_get_fd(c.get(), a.qid, seq++);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EINVAL, ret_of(r.value()));
    EXPECT_TRUE(r.value().fds.empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// TRANSFER: H2C to HBM/DDR
// ═════════════════════════════════════════════════════════════════════════════

// Fill a fresh memfd of @p bytes with a pattern and return an owning fd.
UniqueFd make_filled_buf(const std::vector<uint8_t>& bytes) {
    UniqueFd fd(::memfd_create("t", MFD_CLOEXEC));
    EXPECT_TRUE(static_cast<bool>(fd));
    EXPECT_EQ(0, ::ftruncate(fd.get(), static_cast<off_t>(bytes.size())));
    EXPECT_EQ(static_cast<ssize_t>(bytes.size()),
              ::pwrite(fd.get(), bytes.data(), bytes.size(), 0));
    return fd;
}

TEST_F(QdmaSubsystemTest, TransferH2cToHbm) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    std::vector<uint8_t> src(page);
    std::iota(src.begin(), src.end(), 1);
    UniqueFd buf = make_filled_buf(src);

    slash_qdma_subxfer sx{};
    sx.qpair_index = 0;
    sx.direction   = SLASH_QDMA_XFER_H2C;
    sx.buf_fd      = 0; // index into the ancillary fd list
    sx.buf_offset  = 0;
    sx.dev_addr    = kDevAddr;
    sx.length      = page;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 200);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(static_cast<int32_t>(page), ret_of(r.value()));

    // The model received the bytes via populate at kDevAddr.
    for (std::size_t i = 0; i < page; ++i) {
        EXPECT_EQ(src[i], server_->peek(kDevAddr + i)) << "byte " << i;
    }
}

TEST_F(QdmaSubsystemTest, TransferH2cLargeMultiChunk) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    // 200 KiB > 64 KiB chunk → exercises the multi-chunk path.
    const std::size_t len = 200 * 1024;
    std::vector<uint8_t> src(len);
    for (std::size_t i = 0; i < len; ++i) src[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    UniqueFd buf = make_filled_buf(src);

    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_H2C;
    sx.buf_fd    = 0;
    sx.dev_addr  = kDevAddr;
    sx.length    = len;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 300);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(static_cast<int32_t>(len), ret_of(r.value()));
    for (std::size_t i = 0; i < len; i += 997) {
        EXPECT_EQ(src[i], server_->peek(kDevAddr + i)) << "byte " << i;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// TRANSFER: C2H from HBM/DDR
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(QdmaSubsystemTest, TransferC2hFromHbm) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    std::vector<uint8_t> seed(page);
    for (std::size_t i = 0; i < page; ++i) seed[i] = static_cast<uint8_t>(0xF0 ^ (i & 0xFF));
    server_->poke_buffer(kDevAddr, seed);

    // Empty destination buffer.
    UniqueFd buf(::memfd_create("dst", MFD_CLOEXEC));
    ASSERT_TRUE(static_cast<bool>(buf));
    ASSERT_EQ(0, ::ftruncate(buf.get(), static_cast<off_t>(page)));

    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_C2H;
    sx.buf_fd    = 0;
    sx.dev_addr  = kDevAddr;
    sx.length    = page;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 400);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(static_cast<int32_t>(page), ret_of(r.value()));

    std::vector<uint8_t> got(page);
    ASSERT_EQ(static_cast<ssize_t>(page), ::pread(buf.get(), got.data(), page, 0));
    EXPECT_EQ(seed, got);
}

// ═════════════════════════════════════════════════════════════════════════════
// TRANSFER: full host→device→host roundtrip (A → C → B, A == B)
// ═════════════════════════════════════════════════════════════════════════════
//
// End-to-end memory read/write through the real datapath: two HOST buffers A and
// B allocated via BUF_CREATE (the actual DMA-buffer allocation ioctl, returning
// mappable memfds) and one DEVICE buffer C living at a device address in the
// model.  Data is written into A, DMA'd A→C (H2C: pread(A) → ModelClient::
// populate), then DMA'd C→B (C2H: ModelClient::fetch_buffer → pwrite(B)).  After
// both transfers B must byte-for-byte equal A.  Sized at 128 KiB so the transfer
// also crosses the 64 KiB per-chunk boundary in both directions.
TEST_F(QdmaSubsystemTest, RoundtripHostToDeviceToHostAEqualsB) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;

    // A started MM qpair with both directions, and its transfer session.
    uint32_t qid  = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    const std::size_t len  = 128 * 1024;         // > 64 KiB → multi-chunk
    ASSERT_EQ(0u, len % page) << "len must be a page multiple for BUF_CREATE";
    const uint64_t   dev_c = kDevAddr;           // device buffer C's address

    // ── Allocate host buffers A and B via BUF_CREATE ─────────────────────────
    auto make_host_buf = [&](uint32_t s) -> UniqueFd {
        auto r = do_buf_create(c.get(), len, s);
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(0, ret_of(r.value()));
        EXPECT_EQ(1u, r.value().fds.size());
        return r.has_value() && r.value().fds.size() == 1
                   ? UniqueFd(std::move(r.value().fds[0]))
                   : UniqueFd{};
    };
    UniqueFd bufA = make_host_buf(seq++);
    UniqueFd bufB = make_host_buf(seq++);
    ASSERT_TRUE(static_cast<bool>(bufA));
    ASSERT_TRUE(static_cast<bool>(bufB));

    // ── Fill host buffer A through its mapping (as a user would) ─────────────
    std::vector<uint8_t> pattern(len);
    for (std::size_t i = 0; i < len; ++i) {
        pattern[i] = static_cast<uint8_t>((i * 131u + 17u) & 0xFF);
    }
    {
        void* pa = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, bufA.get(), 0);
        ASSERT_NE(MAP_FAILED, pa);
        std::memcpy(pa, pattern.data(), len);
        ASSERT_EQ(0, ::munmap(pa, len));
    }

    // ── Transfer 1: A → C (H2C) ──────────────────────────────────────────────
    {
        slash_qdma_subxfer sx{};
        sx.qpair_index = 0;
        sx.direction   = SLASH_QDMA_XFER_H2C;
        sx.buf_fd      = 0;      // index into the ancillary fd list ({bufA})
        sx.buf_offset  = 0;
        sx.dev_addr    = dev_c;
        sx.length      = len;
        auto r = do_transfer(xfer.get(), {sx}, {bufA.get()}, seq++);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(static_cast<int32_t>(len), ret_of(r.value()));
    }

    // Device buffer C now holds A's bytes (checked directly in the model memory).
    for (std::size_t i = 0; i < len; i += 1024) {
        ASSERT_EQ(pattern[i], server_->peek(dev_c + i)) << "device byte " << i;
    }

    // ── Transfer 2: C → B (C2H) ──────────────────────────────────────────────
    {
        slash_qdma_subxfer sx{};
        sx.qpair_index = 0;
        sx.direction   = SLASH_QDMA_XFER_C2H;
        sx.buf_fd      = 0;      // index into the ancillary fd list ({bufB})
        sx.buf_offset  = 0;
        sx.dev_addr    = dev_c;
        sx.length      = len;
        auto r = do_transfer(xfer.get(), {sx}, {bufB.get()}, seq++);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(static_cast<int32_t>(len), ret_of(r.value()));
    }

    // ── A must byte-for-byte equal B ─────────────────────────────────────────
    std::vector<uint8_t> a_bytes(len);
    std::vector<uint8_t> b_bytes(len);
    ASSERT_EQ(static_cast<ssize_t>(len), ::pread(bufA.get(), a_bytes.data(), len, 0));
    ASSERT_EQ(static_cast<ssize_t>(len), ::pread(bufB.get(), b_bytes.data(), len, 0));
    EXPECT_EQ(a_bytes, b_bytes) << "roundtrip A→C→B did not preserve the data";
    EXPECT_EQ(pattern, b_bytes) << "B does not match the original pattern written to A";
}

// ═════════════════════════════════════════════════════════════════════════════
// TRANSFER: H2C to the reconfiguration aperture
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(QdmaSubsystemTest, TransferH2cToReconfigApertureAppendsStaging) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    std::vector<uint8_t> chunk1(page, 0xAA);
    std::vector<uint8_t> chunk2(page, 0xBB);
    UniqueFd buf1 = make_filled_buf(chunk1);
    UniqueFd buf2 = make_filled_buf(chunk2);

    auto send_ap = [&](UniqueFd& buf, uint32_t s) {
        slash_qdma_subxfer sx{};
        sx.direction = SLASH_QDMA_XFER_H2C;
        sx.buf_fd    = 0;
        sx.dev_addr  = kReconfigApertureAddr;
        sx.length    = page;
        auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, s);
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(static_cast<int32_t>(page), ret_of(r.value()));
    };
    send_ap(buf1, 500);
    send_ap(buf2, 501);

    // The staging VBIN contains chunk1 followed by chunk2, and the model saw NO
    // populate for the aperture address.
    auto staged = vbin_->read_staging();
    ASSERT_TRUE(staged.has_value());
    ASSERT_EQ(2 * page, staged.value().size());
    for (std::size_t i = 0; i < page; ++i) EXPECT_EQ(0xAA, staged.value()[i]);
    for (std::size_t i = 0; i < page; ++i) EXPECT_EQ(0xBB, staged.value()[page + i]);
    EXPECT_EQ(0, server_->peek(kReconfigApertureAddr));
    // No fetch/populate requests should have touched the model at all here.
    EXPECT_EQ(0u, server_->request_count());
}

// ═════════════════════════════════════════════════════════════════════════════
// TRANSFER: stopped / removed qpair -> -ENODEV
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(QdmaSubsystemTest, TransferWithStoppedQpairEnodev) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    // Stop the qpair underneath the live session.
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_STOP, seq++).value()));

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    UniqueFd buf = make_filled_buf(std::vector<uint8_t>(page, 0x11));
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_H2C;
    sx.buf_fd    = 0;
    sx.dev_addr  = kDevAddr;
    sx.length    = page;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 600);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-ENODEV, ret_of(r.value()));
}

TEST_F(QdmaSubsystemTest, TransferWithDeletedQpairEnodev) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    // Delete the qpair underneath the live session.
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_DEL, seq++).value()));

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    UniqueFd buf = make_filled_buf(std::vector<uint8_t>(page, 0x22));
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_H2C;
    sx.buf_fd    = 0;
    sx.dev_addr  = kDevAddr;
    sx.length    = page;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 700);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-ENODEV, ret_of(r.value()));
}

// ═════════════════════════════════════════════════════════════════════════════
// FD-index resolution
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(QdmaSubsystemTest, TransferMultiSubxferFdIndices) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    // Two qpairs bound to one XFER fd.
    uint32_t qid0 = add_started_qpair(c.get(), seq);
    uint32_t qid1 = add_started_qpair(c.get(), seq);
    auto gf = do_get_fd_list(c.get(), {qid0, qid1}, seq++);
    ASSERT_TRUE(gf.has_value());
    ASSERT_EQ(0, ret_of(gf.value()));
    ASSERT_EQ(1u, gf.value().fds.size());
    UniqueFd xfer(std::move(gf.value().fds[0]));
    set_rcv_timeout(xfer.get(), 3000ms);

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    std::vector<uint8_t> a(page, 0x10), b(page, 0x20);
    UniqueFd buf_a = make_filled_buf(a);
    UniqueFd buf_b = make_filled_buf(b);

    // Two sub-transfers: sub 0 uses qpair_index 0 + fd index 0; sub 1 uses
    // qpair_index 1 + fd index 1.  Distinct device addrs.
    slash_qdma_subxfer sx0{};
    sx0.qpair_index = 0; sx0.direction = SLASH_QDMA_XFER_H2C; sx0.buf_fd = 0;
    sx0.dev_addr = kDevAddr;            sx0.length = page;
    slash_qdma_subxfer sx1{};
    sx1.qpair_index = 1; sx1.direction = SLASH_QDMA_XFER_H2C; sx1.buf_fd = 1;
    sx1.dev_addr = kDevAddr + 0x100000; sx1.length = page;

    auto r = do_transfer(xfer.get(), {sx0, sx1}, {buf_a.get(), buf_b.get()}, 800);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(static_cast<int32_t>(2 * page), ret_of(r.value()));
    EXPECT_EQ(0x10, server_->peek(kDevAddr));
    EXPECT_EQ(0x20, server_->peek(kDevAddr + 0x100000));
}

TEST_F(QdmaSubsystemTest, TransferOutOfRangeFdIndexRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    UniqueFd buf = make_filled_buf(std::vector<uint8_t>(page, 0x33));
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_H2C;
    sx.buf_fd    = 5; // index 5 but only one fd sent → out of range
    sx.dev_addr  = kDevAddr;
    sx.length    = page;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 900);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EINVAL, ret_of(r.value()));
}

// ═════════════════════════════════════════════════════════════════════════════
// Model serialisation
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(QdmaSubsystemTest, ConcurrentTransfersSerializeModelRequests) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;

    // Two independent sessions, each with its own qpair + XFER fd.
    constexpr int kSessions = 4;
    std::vector<UniqueFd> xfers;
    for (int i = 0; i < kSessions; ++i) {
        uint32_t qid = add_started_qpair(c.get(), seq);
        xfers.push_back(open_xfer(c.get(), qid, seq));
        // All sessions serialise through one ModelClient, so a client's response
        // can wait behind every other session's model requests.  Under -j16 +
        // sanitizer load that queue is long; use a generous client recv timeout
        // so a merely-slow (correctly-serialised) transfer is not mis-read as a
        // failure.
        set_rcv_timeout(xfers.back().get(), 30000ms);
    }

    const std::size_t len = 128 * 1024; // multi-chunk to widen the race window
    std::vector<uint8_t> src(len, 0x5A);

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kSessions; ++i) {
        threads.emplace_back([&, i] {
            UniqueFd buf = make_filled_buf(src);
            slash_qdma_subxfer sx{};
            sx.direction = SLASH_QDMA_XFER_H2C;
            sx.buf_fd    = 0;
            sx.dev_addr  = kDevAddr + static_cast<uint64_t>(i) * 0x400000;
            sx.length    = len;
            for (int rep = 0; rep < 3; ++rep) {
                auto r = do_transfer(xfers[i].get(), {sx}, {buf.get()},
                                     static_cast<uint32_t>(1000 + i * 10 + rep));
                if (!r || ret_of(r.value()) != static_cast<int32_t>(len)) {
                    failures.fetch_add(1);
                    return;
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(0, failures.load());
    // The mock model asserts one-in-flight; correct serialisation keeps it at 1.
    EXPECT_EQ(1, server_->max_in_flight());
}

// ═════════════════════════════════════════════════════════════════════════════
// REMOVE / RESCAN
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(QdmaSubsystemTest, RemoveForgetsQpairsDisconnectsAndPreservesModel) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    ASSERT_EQ(1u, sub->qpair_count());
    ASSERT_EQ(1u, sub->session_count());

    sub->remove();
    EXPECT_FALSE(sub->is_active());
    // Socket file gone.
    struct stat st{};
    EXPECT_NE(0, ::stat(sock_path_.c_str(), &st));
    // Qpairs forgotten.
    EXPECT_EQ(0u, sub->qpair_count());
    EXPECT_EQ(0u, sub->session_count());

    // CTL + XFER clients are force-disconnected.
    EXPECT_FALSE(do_info(c.get(), seq++).has_value());
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    UniqueFd buf = make_filled_buf(std::vector<uint8_t>(page, 0x44));
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0; sx.dev_addr = kDevAddr; sx.length = page;
    EXPECT_FALSE(do_transfer(xfer.get(), {sx}, {buf.get()}, seq++).has_value());

    // The borrowed model still works directly.
    std::vector<uint8_t> data{1, 2, 3, 4};
    EXPECT_TRUE(model_->populate(0x1000, std::span<const uint8_t>(data)).has_value());
}

TEST_F(QdmaSubsystemTest, RescanAfterRemoveWorks) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    {
        UniqueFd c = connect_client(sock_path_);
        ASSERT_TRUE(static_cast<bool>(c));
        uint32_t seq = 1;
        (void)add_started_qpair(c.get(), seq);
    }
    sub->remove();

    // RESCAN.
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c2 = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c2));
    uint32_t seq = 1;
    // qid allocation restarts at 0 after re-init.
    auto add = do_qpair_add(c2.get(), kQdmaQModeMm, 0x3, seq++);
    ASSERT_TRUE(add.has_value());
    slash_qdma_qpair_add a{};
    std::memcpy(&a, add.value().payload.data(), sizeof(a));
    EXPECT_EQ(0u, a.qid);
    // A transfer round-trips again.
    uint32_t qid = a.qid;
    ASSERT_EQ(0, ret_of(do_q_op(c2.get(), qid, SLASH_QDMA_QUEUE_OP_START, seq++).value()));
    UniqueFd xfer = open_xfer(c2.get(), qid, seq);
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    std::vector<uint8_t> src(page, 0x77);
    UniqueFd buf = make_filled_buf(src);
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0; sx.dev_addr = kDevAddr; sx.length = page;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 999);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(static_cast<int32_t>(page), ret_of(r.value()));
}

TEST_F(QdmaSubsystemTest, MultipleRemoveRescanCycles) {
    auto sub = make_subsystem();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(sub->setup().has_value()) << "cycle " << i;
        UniqueFd c = connect_client(sock_path_);
        ASSERT_TRUE(static_cast<bool>(c)) << "cycle " << i;
        ASSERT_TRUE(do_info(c.get(), 1).has_value()) << "cycle " << i;
        sub->remove();
    }
    struct stat st{};
    EXPECT_NE(0, ::stat(sock_path_.c_str(), &st));
}

TEST_F(QdmaSubsystemTest, DestructorWhileClientsAndSessionsConnected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    // Destroy while both CTL + XFER are live — must not hang or leak.
    sub.reset();
    EXPECT_FALSE(do_info(c.get(), seq++).has_value());
}

TEST_F(QdmaSubsystemTest, NoFdLeakAcrossSetupRemove) {
    int before = open_fd_count();
    {
        auto sub = make_subsystem();
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(sub->setup().has_value());
            UniqueFd c = connect_client(sock_path_);
            ASSERT_TRUE(static_cast<bool>(c));
            uint32_t seq = 1;
            uint32_t qid = add_started_qpair(c.get(), seq);
            UniqueFd xfer = open_xfer(c.get(), qid, seq);
            (void)do_buf_create(c.get(), static_cast<uint64_t>(::getpagesize()), 50);
            sub->remove();
        }
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 2) << "before=" << before << " after=" << after;
}

// ═════════════════════════════════════════════════════════════════════════════
// Unknown ioctl + malformed
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(QdmaSubsystemTest, UnknownIoctlEnosysWorkerSurvives) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    slash_emu_socket_header h{0xDEADBEEFu, 1, 0, 0};
    auto resp = send_request(c.get(), h, {}, {});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(-ENOSYS, ret_of(resp.value()));
    // Worker survives.
    EXPECT_TRUE(do_info(c.get(), 2).has_value());
}

// ═════════════════════════════════════════════════════════════════════════════
// ABI wire-layout sanity
// ═════════════════════════════════════════════════════════════════════════════

TEST(QdmaIoctlAbiTest, InfoStructCarriesBdfAndCmdEncodesSize) {
    // The bdf field was appended (Step 10 ABI change); sizeof grows to 52.
    EXPECT_EQ(52u, sizeof(slash_qdma_info));
    // _IOWR('v', 0x50, struct slash_qdma_info): low byte = nr, next byte = 'v'.
    EXPECT_EQ(0x50u, kSlashQdmaIoctlInfo & 0xFFu);
    EXPECT_EQ('v', (kSlashQdmaIoctlInfo >> 8) & 0xFFu);
    EXPECT_EQ(0x51u, kSlashQdmaIoctlQpairAdd & 0xFFu);
    EXPECT_EQ(0x52u, kSlashQdmaIoctlQOp & 0xFFu);
    EXPECT_EQ(0x53u, kSlashQdmaIoctlQpairGetFd & 0xFFu);
    EXPECT_EQ(0x54u, kSlashQdmaIoctlBufCreate & 0xFFu);
    EXPECT_EQ(0x56u, kSlashQdmaQpairIoctlTransfer & 0xFFu);
}

// ═════════════════════════════════════════════════════════════════════════════
// ADVERSARY PROBES (Step 10)
//
// Probes devised to hunt for fd/thread/memfd leaks, lifecycle/teardown races,
// protocol-abuse crashes, qpair state-machine holes, transfer-correctness gaps,
// and model-serialisation violations in the PF1 QDMA subsystem.  Probes that pass
// are retained as hardening/regression tests; any probe that revealed a bug
// carries a comment naming the finding.
// ═════════════════════════════════════════════════════════════════════════════

// ── Probe: every ioctl with a short (< struct) payload → -EINVAL, no OOB read ──
// ASan would flag any overrun.  The worker must survive and serve the next req.
TEST_F(QdmaSubsystemTest, ShortPayloadEveryCtlOpRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    std::array<uint8_t, 3> stub{1, 2, 3};
    for (uint32_t op : {kSlashQdmaIoctlInfo, kSlashQdmaIoctlQpairAdd, kSlashQdmaIoctlQOp,
                        kSlashQdmaIoctlQpairGetFd, kSlashQdmaIoctlBufCreate}) {
        slash_emu_socket_header h{op, 1, 0, 0};
        auto r = send_request(c.get(), h, std::span<const uint8_t>(stub), {});
        ASSERT_TRUE(r.has_value()) << "op " << op;
        EXPECT_EQ(-EINVAL, ret_of(r.value())) << "op " << op;
        EXPECT_TRUE(r.value().fds.empty()) << "op " << op;
    }
    EXPECT_EQ(0, ret_of(do_info(c.get(), 99).value()));
}

// ── Probe: an oversized datagram (> kMaxPayloadBytes) closes the conn cleanly ──
TEST_F(QdmaSubsystemTest, OversizedPayloadClosesConnCleanly) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));

    std::vector<uint8_t> big(kMaxPayloadBytes + sizeof(slash_emu_socket_header) + 4096, 0xEE);
    slash_emu_socket_header h{kSlashQdmaIoctlInfo, 1, 0, 0};
    std::memcpy(big.data(), &h, sizeof(h));
    iovec iov{big.data(), big.size()};
    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;
    (void)::sendmsg(c.get(), &msg, MSG_NOSIGNAL);

    UniqueFd c2 = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c2));
    EXPECT_EQ(0, ret_of(do_info(c2.get(), 1).value()));
}

// ── Probe: unexpected SCM_RIGHTS on a CTL op that expects none must not leak ────
TEST_F(QdmaSubsystemTest, UnexpectedScmRightsNoLeak) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    ASSERT_TRUE(do_info(c.get(), 1).has_value());
    int before = open_fd_count();
    for (int i = 0; i < 200; ++i) {
        int extra = ::dup(0);
        ASSERT_GE(extra, 0);
        std::array<int, 1> fds{extra};
        slash_qdma_info info{};
        info.size = sizeof(info);
        slash_emu_socket_header h{kSlashQdmaIoctlInfo, static_cast<uint32_t>(2 + i), 0, 0};
        std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&info), sizeof(info));
        auto r = send_request(c.get(), h, p, std::span<const int>(fds));
        ::close(extra);
        ASSERT_TRUE(r.has_value()) << "iter " << i;
        EXPECT_EQ(0, ret_of(r.value()));
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 3) << "before=" << before << " after=" << after;
}

// ── Probe: GET_FD with qpair_count > SLASH_QDMA_FD_MAX_QPAIRS → -EINVAL ─────────
TEST_F(QdmaSubsystemTest, GetFdTooManyQpairsRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    slash_qdma_qpair_fd_request req{};
    req.size         = sizeof(req);
    req.qpair_count  = SLASH_QDMA_FD_MAX_QPAIRS + 1;
    req.qpair_ids[0] = qid;
    slash_emu_socket_header h{kSlashQdmaIoctlQpairGetFd, seq, 0, 0};
    std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    auto r = send_request(c.get(), h, p, {});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EINVAL, ret_of(r.value()));
    EXPECT_TRUE(r.value().fds.empty());
}

// ── Probe: GET_FD referencing a mix of started + unknown qpair strands NEITHER ──
TEST_F(QdmaSubsystemTest, GetFdPartialInvalidLeavesNoQpairStranded) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t good = add_started_qpair(c.get(), seq);
    auto r = do_get_fd_list(c.get(), {good, 9999}, seq++);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EINVAL, ret_of(r.value()));
    EXPECT_TRUE(r.value().fds.empty());
    UniqueFd xfer = open_xfer(c.get(), good, seq);
    EXPECT_TRUE(static_cast<bool>(xfer));
}

// ── Probe: a second GET_FD on a Used qpair is rejected, no second session ──────
TEST_F(QdmaSubsystemTest, SecondGetFdOnUsedQpairRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    auto r = do_get_fd(c.get(), qid, seq++);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EINVAL, ret_of(r.value()));
    EXPECT_TRUE(r.value().fds.empty());
    EXPECT_EQ(1u, sub->session_count());
}

// ── Probe: START on a Used qpair is invalid → -EINVAL ──────────────────────────
TEST_F(QdmaSubsystemTest, StartOnUsedQpairRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    EXPECT_EQ(-EINVAL, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_START, seq++).value()));
}

// ── Probe: STOP underneath a live session → session transfer -ENODEV, and the
//    last-close must NOT resurrect the stopped qpair to Started. ────────────────
TEST_F(QdmaSubsystemTest, StopUnderneathSessionThenCloseKeepsStopped) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_STOP, seq++).value()));

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    UniqueFd buf = make_filled_buf(std::vector<uint8_t>(page, 0x5));
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0; sx.dev_addr = kDevAddr; sx.length = page;
    EXPECT_EQ(-ENODEV, ret_of(do_transfer(xfer.get(), {sx}, {buf.get()}, seq++).value()));

    xfer.reset();
    auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
    while (sub->session_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    EXPECT_EQ(-EINVAL, ret_of(do_get_fd(c.get(), qid, seq++).value())); // still Stopped
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_START, seq++).value()));
    UniqueFd xfer2 = open_xfer(c.get(), qid, seq);
    EXPECT_TRUE(static_cast<bool>(xfer2));
}

// ── Probe: TRANSFER count==0 and count>MAX → -EINVAL ───────────────────────────
TEST_F(QdmaSubsystemTest, TransferBadCountRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    int extra = ::dup(0);
    ASSERT_GE(extra, 0);
    for (uint32_t cnt : {uint32_t{0}, uint32_t{SLASH_QDMA_FD_MAX_QPAIRS + 5}}) {
        slash_qdma_transfer x{};
        x.size = sizeof(x); x.count = cnt;
        slash_emu_socket_header h{kSlashQdmaQpairIoctlTransfer, 10 + cnt, 0, 0};
        std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&x), sizeof(x));
        std::array<int, 1> fds{extra};
        auto r = send_request(xfer.get(), h, p, std::span<const int>(fds));
        ASSERT_TRUE(r.has_value()) << "count " << cnt;
        EXPECT_EQ(-EINVAL, ret_of(r.value())) << "count " << cnt;
    }
    ::close(extra);
}

// ── Probe: TRANSFER with out-of-range qpair_index → -EINVAL ────────────────────
TEST_F(QdmaSubsystemTest, TransferBadQpairIndexRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    UniqueFd buf = make_filled_buf(std::vector<uint8_t>(page, 0x6));
    slash_qdma_subxfer sx{};
    sx.qpair_index = 3;
    sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0; sx.dev_addr = kDevAddr; sx.length = page;
    EXPECT_EQ(-EINVAL, ret_of(do_transfer(xfer.get(), {sx}, {buf.get()}, 20).value()));
}

// ── Probe: TRANSFER with a direction not enabled on the qpair → -EINVAL ─────────
TEST_F(QdmaSubsystemTest, TransferDisabledDirectionRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    auto add = do_qpair_add(c.get(), kQdmaQModeMm, 0x1 /*H2C only*/, seq++);
    slash_qdma_qpair_add a{};
    std::memcpy(&a, add.value().payload.data(), sizeof(a));
    uint32_t qid = a.qid;
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_START, seq++).value()));
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    UniqueFd buf(::memfd_create("d", MFD_CLOEXEC));
    ASSERT_TRUE(static_cast<bool>(buf));
    ASSERT_EQ(0, ::ftruncate(buf.get(), static_cast<off_t>(page)));
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_C2H; sx.buf_fd = 0; sx.dev_addr = kDevAddr; sx.length = page;
    EXPECT_EQ(-EINVAL, ret_of(do_transfer(xfer.get(), {sx}, {buf.get()}, 30).value()));
}

// ── Probe: TRANSFER with an invalid direction value → -EINVAL ──────────────────
TEST_F(QdmaSubsystemTest, TransferBadDirectionValueRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    UniqueFd buf = make_filled_buf(std::vector<uint8_t>(page, 0x7));
    slash_qdma_subxfer sx{};
    sx.direction = 0xABCD;
    sx.buf_fd = 0; sx.dev_addr = kDevAddr; sx.length = page;
    EXPECT_EQ(-EINVAL, ret_of(do_transfer(xfer.get(), {sx}, {buf.get()}, 40).value()));
}

// ── Probe: TRANSFER honours buf_offset (transfers the right slice) ─────────────
TEST_F(QdmaSubsystemTest, TransferHonoursBufOffset) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    std::vector<uint8_t> src(page * 2);
    for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i & 0xFF);
    UniqueFd buf = make_filled_buf(src);
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0;
    sx.buf_offset = page; sx.dev_addr = kDevAddr; sx.length = page;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 50);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(static_cast<int32_t>(page), ret_of(r.value()));
    for (std::size_t i = 0; i < page; i += 251) {
        EXPECT_EQ(src[page + i], server_->peek(kDevAddr + i)) << "byte " << i;
    }
}

// ── Probe: XFER endpoint survives a rejected/unknown op and serves a later one ──
TEST_F(QdmaSubsystemTest, XferWorkerSurvivesRejectedOp) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    EXPECT_EQ(-EINVAL, ret_of(do_info(xfer.get(), 60).value()));
    {
        slash_emu_socket_header h{0xFEEDu, 61, 0, 0};
        auto r = send_request(xfer.get(), h, {}, {});
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(-ENOSYS, ret_of(r.value()));
    }
    auto r = do_buf_create(xfer.get(), static_cast<uint64_t>(::getpagesize()), 62);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(0, ret_of(r.value()));
    ASSERT_EQ(1u, r.value().fds.size());
}

// ── Probe: closing a MULTI-qpair session's fd returns ALL its qpairs to Started ─
TEST_F(QdmaSubsystemTest, MultiQpairSessionCloseReturnsAllToStarted) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t q0 = add_started_qpair(c.get(), seq);
    uint32_t q1 = add_started_qpair(c.get(), seq);
    auto gf = do_get_fd_list(c.get(), {q0, q1}, seq++);
    ASSERT_TRUE(gf.has_value());
    ASSERT_EQ(0, ret_of(gf.value()));
    ASSERT_EQ(1u, gf.value().fds.size());
    UniqueFd xfer(std::move(gf.value().fds[0]));
    EXPECT_EQ(-EINVAL, ret_of(do_get_fd(c.get(), q0, seq++).value())); // both Used
    xfer.reset();
    auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
    while (sub->session_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    UniqueFd x0 = open_xfer(c.get(), q0, seq);
    UniqueFd x1 = open_xfer(c.get(), q1, seq);
    EXPECT_TRUE(static_cast<bool>(x0));
    EXPECT_TRUE(static_cast<bool>(x1));
}

// ── Probe: returned XFER + BUF_CREATE fds are CLOEXEC ──────────────────────────
TEST_F(QdmaSubsystemTest, ReturnedFdsAreCloexec) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    auto gf = do_get_fd(c.get(), qid, seq++);
    ASSERT_TRUE(gf.has_value());
    ASSERT_EQ(1u, gf.value().fds.size());
    int flags = ::fcntl(gf.value().fds[0].get(), F_GETFD);
    ASSERT_NE(-1, flags);
    EXPECT_TRUE(flags & FD_CLOEXEC) << "XFER fd not CLOEXEC";
    auto bc = do_buf_create(c.get(), static_cast<uint64_t>(::getpagesize()), seq++);
    ASSERT_TRUE(bc.has_value());
    ASSERT_EQ(1u, bc.value().fds.size());
    flags = ::fcntl(bc.value().fds[0].get(), F_GETFD);
    ASSERT_NE(-1, flags);
    EXPECT_TRUE(flags & FD_CLOEXEC) << "BUF_CREATE fd not CLOEXEC";
}

// ── Probe: many GET_FD open/close cycles leak no daemon fds/threads ────────────
TEST_F(QdmaSubsystemTest, GetFdOpenCloseChurnNoLeak) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    auto wait_no_session = [&] {
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (sub->session_count() != 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
    };
    for (int i = 0; i < 5; ++i) { UniqueFd x = open_xfer(c.get(), qid, seq); x.reset(); wait_no_session(); }
    int before = open_fd_count();
    for (int i = 0; i < 100; ++i) {
        UniqueFd x = open_xfer(c.get(), qid, seq);
        ASSERT_TRUE(static_cast<bool>(x)) << "iter " << i;
        x.reset();
        wait_no_session();
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 10) << "before=" << before << " after=" << after;
    sub->remove();
}

// ── Probe: remove() while transfer sessions are mid-transfer must not hang ─────
TEST_F(QdmaSubsystemTest, RemoveWhileSessionsBusyNoHang) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    constexpr int kSessions = 4;
    std::vector<UniqueFd> xfers;
    for (int i = 0; i < kSessions; ++i) {
        uint32_t qid = add_started_qpair(c.get(), seq);
        xfers.push_back(open_xfer(c.get(), qid, seq));
        set_rcv_timeout(xfers.back().get(), 30000ms);
    }
    const std::size_t len = 128 * 1024;
    std::vector<uint8_t> src(len, 0x3C);
    std::atomic<bool> go{true};
    std::vector<std::thread> threads;
    for (int i = 0; i < kSessions; ++i) {
        threads.emplace_back([&, i] {
            UniqueFd buf = make_filled_buf(src);
            slash_qdma_subxfer sx{};
            sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0;
            sx.dev_addr = kDevAddr + static_cast<uint64_t>(i) * 0x400000; sx.length = len;
            while (go.load()) {
                auto r = do_transfer(xfers[i].get(), {sx}, {buf.get()},
                                     static_cast<uint32_t>(2000 + i));
                if (!r) break;
            }
        });
    }
    std::this_thread::sleep_for(50ms);
    sub->remove();
    go.store(false);
    for (auto& t : threads) t.join();
    EXPECT_FALSE(sub->is_active());
    EXPECT_EQ(0u, sub->session_count());
    EXPECT_EQ(0u, sub->qpair_count());
}

// ── Probe: model-dead (Transport error) during H2C transfer → -ENODEV ──────────
TEST_F(QdmaSubsystemTest, TransferModelDeadEnodev) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    set_rcv_timeout(xfer.get(), 30000ms);
    server_->set_fault(slash_emu::test::FaultMode::Close, 0);
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    UniqueFd buf = make_filled_buf(std::vector<uint8_t>(page, 0x9));
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0; sx.dev_addr = kDevAddr; sx.length = page;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 70);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-ENODEV, ret_of(r.value()));
}

// ── Probe: setup() over an existing DIRECTORY at the socket path fails clean ────
TEST_F(QdmaSubsystemTest, SetupOverExistingDirectoryFailsClean) {
    std::filesystem::create_directories(sock_path_);
    ASSERT_TRUE(std::filesystem::is_directory(sock_path_));
    int before = open_fd_count();
    auto sub = make_subsystem();
    auto r = sub->setup();
    EXPECT_FALSE(r.has_value());
    EXPECT_FALSE(sub->is_active());
    int after = open_fd_count();
    EXPECT_LE(after, before + 1) << "setup leaked an fd on the failure path";
    std::filesystem::remove(sock_path_);
}

// ── Probe: response mirrors sequence_id + ioctl_op and errno sign convention ───
TEST_F(QdmaSubsystemTest, ResponseMirrorsHeaderAndErrnoSign) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    auto ok = do_info(c.get(), 0x12345678u);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(0x12345678u, ok.value().header.sequence_id);
    EXPECT_EQ(kSlashQdmaIoctlInfo, ok.value().header.ioctl_op);
    EXPECT_GE(ret_of(ok.value()), 0);
    auto err = do_qpair_add(c.get(), kQdmaQModeSt, 0x3, 0x0BADF00Du);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(0x0BADF00Du, err.value().header.sequence_id);
    EXPECT_EQ(kSlashQdmaIoctlQpairAdd, err.value().header.ioctl_op);
    EXPECT_LT(ret_of(err.value()), 0);
}

// ── Probe: concurrent CTL clients ADD/DEL their own qpairs — table stays sane ──
TEST_F(QdmaSubsystemTest, ConcurrentQpairAddDelConsistent) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    constexpr int kClients = 8;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kClients; ++t) {
        threads.emplace_back([&, t] {
            UniqueFd cc = connect_client(sock_path_);
            if (!cc) { failures.fetch_add(1); return; }
            uint32_t seq = static_cast<uint32_t>(t * 1000 + 1);
            for (int i = 0; i < 20; ++i) {
                auto add = do_qpair_add(cc.get(), kQdmaQModeMm, 0x3, seq++);
                if (!add || ret_of(add.value()) != 0) { failures.fetch_add(1); return; }
                slash_qdma_qpair_add a{};
                std::memcpy(&a, add.value().payload.data(), sizeof(a));
                auto del = do_q_op(cc.get(), a.qid, SLASH_QDMA_QUEUE_OP_DEL, seq++);
                if (!del || ret_of(del.value()) != 0) { failures.fetch_add(1); return; }
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(0, failures.load());
    EXPECT_EQ(0u, sub->qpair_count());
}

// ═════════════════════════════════════════════════════════════════════════════
// ADVERSARY PROBES — round 2 (Step 10 review)
//
// Targeted at the lead's four suspicions plus a broad sweep for fd/thread leaks,
// state-machine holes, transfer-boundary quirks, endpoint routing, and teardown
// races.  Probes that pass are retained as regression tests.
// ═════════════════════════════════════════════════════════════════════════════

// ── Suspicion 1: return_value is int32 but total is uint64 ─────────────────────
// A single subxfer of length 0 returns 0 (no model traffic); a small transfer
// returns the exact byte count.  We cannot allocate 2 GiB to force the overflow,
// so this documents the CONTRACT boundary: the return is the low 32 bits of the
// accumulated total interpreted as a signed int32.  A transfer whose total is in
// [0, INT32_MAX] round-trips faithfully; anything at/above 2 GiB would alias an
// errno.  With SLASH_QDMA_FD_MAX_QPAIRS==2 the only way to reach 2 GiB is a
// >=1 GiB-per-subxfer request, which is not exercisable here — we pin the
// small-transfer contract and note the theoretical wrap.
TEST_F(QdmaSubsystemTest, TransferReturnValueIsExactByteCountSmall) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    // Two subxfers on the SAME qpair (index 0), buf indices 0 and 1: total 2*page.
    // Exercises the accumulator across subxfers and confirms it stays exact and
    // well below INT32_MAX (no wrap).
    std::vector<uint8_t> a(page, 0x01), b(page, 0x02);
    UniqueFd buf_a = make_filled_buf(a);
    UniqueFd buf_b = make_filled_buf(b);
    slash_qdma_subxfer sx0{};
    sx0.qpair_index = 0; sx0.direction = SLASH_QDMA_XFER_H2C; sx0.buf_fd = 0;
    sx0.dev_addr = kDevAddr;              sx0.length = page;
    slash_qdma_subxfer sx1{};
    sx1.qpair_index = 0; sx1.direction = SLASH_QDMA_XFER_H2C; sx1.buf_fd = 1;
    sx1.dev_addr = kDevAddr + 0x200000;  sx1.length = page;
    auto r = do_transfer(xfer.get(), {sx0, sx1}, {buf_a.get(), buf_b.get()}, 10);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(static_cast<int32_t>(2 * page), ret_of(r.value()));
    EXPECT_GT(ret_of(r.value()), 0); // no sign flip for a small total
}

// ── Probe: a zero-length subxfer transfers nothing, returns 0, no model call ────
// count>=1 with length==0 is accepted by the precondition; the transfer loop must
// simply skip it (no pread/populate, no fetch), return 0.
TEST_F(QdmaSubsystemTest, TransferZeroLengthSubxferReturnsZeroNoModel) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    int extra = ::dup(0);
    ASSERT_GE(extra, 0);
    slash_qdma_subxfer sx{};
    sx.qpair_index = 0; sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0;
    sx.dev_addr = kDevAddr; sx.length = 0;
    auto r = do_transfer(xfer.get(), {sx}, {extra}, 11);
    ::close(extra);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(0, ret_of(r.value()));
    EXPECT_EQ(0u, server_->request_count());
}

// ── Suspicion 2: precondition-vs-execution race — STOP between check & populate ─
// The architecture says the driver does NOT invalidate a live session: once the
// precondition passes (qpair Started/Used), a concurrent STOP must NOT abort a
// transfer already past its check.  We can't hit the exact microsecond window
// deterministically, but we can hammer it: repeatedly transfer on a session while
// a second CTL client STOPs+STARTs the same qpair.  Every transfer must return
// either the full byte count (started when checked) or -ENODEV (stopped when
// checked) — never a torn/EIO/short result, and the daemon must not crash.
TEST_F(QdmaSubsystemTest, TransferVsConcurrentStopStartNoTornResult) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    set_rcv_timeout(xfer.get(), 30000ms);

    const std::size_t len = 128 * 1024;
    std::vector<uint8_t> src(len, 0x9C);
    UniqueFd buf = make_filled_buf(src);

    std::atomic<bool> go{true};
    std::atomic<int>  bad{0};
    // Flipper: a separate CTL connection toggling STOP/START underneath.
    std::thread flipper([&] {
        UniqueFd cc = connect_client(sock_path_);
        if (!cc) { bad.fetch_add(1); return; }
        uint32_t s = 50000;
        while (go.load()) {
            (void)do_q_op(cc.get(), qid, SLASH_QDMA_QUEUE_OP_STOP, s++);
            (void)do_q_op(cc.get(), qid, SLASH_QDMA_QUEUE_OP_START, s++);
        }
    });
    for (int i = 0; i < 200; ++i) {
        slash_qdma_subxfer sx{};
        sx.qpair_index = 0; sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0;
        sx.dev_addr = kDevAddr; sx.length = len;
        auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, static_cast<uint32_t>(60000 + i));
        if (!r) { bad.fetch_add(1); break; }
        int32_t rv = ret_of(r.value());
        // Only two acceptable outcomes: full success or clean -ENODEV.
        if (rv != static_cast<int32_t>(len) && rv != -ENODEV) {
            bad.fetch_add(1);
        }
    }
    go.store(false);
    flipper.join();
    EXPECT_EQ(0, bad.load());
}

// ── Suspicion 3: fd-number matching in connection_loop done-flagging ────────────
// connection_loop flags done by matching w->fd == fd.get() while the fd is STILL
// OPEN (UniqueFd not yet destroyed), so no other worker can hold the same fd
// number.  Churn many short-lived CTL connections concurrently; every one must be
// reaped (connection_count → 0) with no double-flag/UAF (ASan/TSan clean) and no
// fd leak.
TEST_F(QdmaSubsystemTest, ConnectionChurnAllReapedNoMisflag) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    int before = open_fd_count();
    constexpr int kRounds = 6;
    for (int r = 0; r < kRounds; ++r) {
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                UniqueFd cc = connect_client(sock_path_);
                if (cc) { (void)do_info(cc.get(), 1); }
                // cc closes immediately → connection worker sees EOF and flags done.
            });
        }
        for (auto& th : threads) th.join();
    }
    // All connection workers must drain to zero.
    auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
    while (sub->connection_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    EXPECT_EQ(0u, sub->connection_count());
    int after = open_fd_count();
    EXPECT_LE(after, before + 3) << "before=" << before << " after=" << after;
}

// ── Suspicion 4: connection_count() counts sessions too (single worker map) ─────
// Document the actual contract: connection_count() is the count of ALL live
// workers (CTL connections + XFER sessions), while session_count() is the XFER
// subset.  One CTL client + one XFER session ⇒ connection_count()==2,
// session_count()==1.
TEST_F(QdmaSubsystemTest, ConnectionCountIncludesSessions) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    // Wait for the CTL connection worker to register.
    auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
    while (sub->connection_count() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(do_info(c.get(), 1).has_value());
    EXPECT_EQ(1u, sub->connection_count());
    EXPECT_EQ(0u, sub->session_count());

    uint32_t seq = 2;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    // Now: 1 CTL + 1 session live.
    EXPECT_EQ(2u, sub->connection_count());
    EXPECT_EQ(1u, sub->session_count());
}

// ── Probe: C2H from the reconfiguration aperture is NOT special-cased ───────────
// to_reconfig is only set for H2C; a C2H targeting kReconfigApertureAddr must go
// to the model fetch path (dev_addr treated as an ordinary device address), not
// the staging VBIN.  Documents that the aperture is write-only-to-staging.
TEST_F(QdmaSubsystemTest, C2hFromReconfigApertureUsesModelNotStaging) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    std::vector<uint8_t> seed(page, 0x7E);
    server_->poke_buffer(kReconfigApertureAddr, seed);
    UniqueFd buf(::memfd_create("d", MFD_CLOEXEC));
    ASSERT_TRUE(static_cast<bool>(buf));
    ASSERT_EQ(0, ::ftruncate(buf.get(), static_cast<off_t>(page)));
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_C2H; sx.buf_fd = 0;
    sx.dev_addr = kReconfigApertureAddr; sx.length = page;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 12);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(static_cast<int32_t>(page), ret_of(r.value()));
    std::vector<uint8_t> got(page);
    ASSERT_EQ(static_cast<ssize_t>(page), ::pread(buf.get(), got.data(), page, 0));
    EXPECT_EQ(seed, got); // came from the model, not staging
    // Staging must be untouched by a C2H.
    auto staged = vbin_->read_staging();
    if (staged.has_value()) { EXPECT_EQ(0u, staged.value().size()); }
}

// ── Probe: reconfig-aperture H2C multi-chunk keeps dev_addr pinned (append order)
// A >64 KiB H2C to the aperture must append every chunk to staging in order and
// must NOT advance dev_addr per chunk (it is not a linear memory region).  We
// verify the staged bytes equal the source exactly.
TEST_F(QdmaSubsystemTest, ReconfigApertureMultiChunkAppendsInOrder) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);

    const std::size_t len = 200 * 1024; // > 3 chunks of 64 KiB
    std::vector<uint8_t> src(len);
    for (std::size_t i = 0; i < len; ++i) src[i] = static_cast<uint8_t>((i * 13 + 5) & 0xFF);
    UniqueFd buf = make_filled_buf(src);
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0;
    sx.dev_addr = kReconfigApertureAddr; sx.length = len;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 13);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(static_cast<int32_t>(len), ret_of(r.value()));
    auto staged = vbin_->read_staging();
    ASSERT_TRUE(staged.has_value());
    ASSERT_EQ(len, staged.value().size());
    EXPECT_EQ(src, staged.value());
    EXPECT_EQ(0u, server_->request_count()); // model never touched
}

// ── Probe: BUF_CREATE with an unexpected ancillary fd must not leak it ──────────
// BUF_CREATE takes no input fd; if the user attaches one, the daemon must drop it
// (ReceivedMessage dtor) and not leak.  Also confirms BUF_CREATE still succeeds.
TEST_F(QdmaSubsystemTest, BufCreateWithUnexpectedFdNoLeak) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    // Warm up.
    for (int i = 0; i < 5; ++i) (void)do_buf_create(c.get(), page, static_cast<uint32_t>(i + 1));
    int before = open_fd_count();
    for (int i = 0; i < 200; ++i) {
        int extra = ::dup(0);
        ASSERT_GE(extra, 0);
        std::array<int, 1> fds{extra};
        slash_qdma_buf_create req{};
        req.size = sizeof(req); req.length = page;
        slash_emu_socket_header h{kSlashQdmaIoctlBufCreate, static_cast<uint32_t>(100 + i), 0, 0};
        std::span<const uint8_t> p(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
        auto r = send_request(c.get(), h, p, std::span<const int>(fds));
        ::close(extra);
        ASSERT_TRUE(r.has_value()) << "iter " << i;
        EXPECT_EQ(0, ret_of(r.value())) << "iter " << i;
        ASSERT_EQ(1u, r.value().fds.size()) << "iter " << i; // still returns memfd
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 3) << "before=" << before << " after=" << after;
}

// ── Probe: TRANSFER referencing a qpair_index whose qid was DELETED → -ENODEV,
//    but the transferred fds must not leak on that error path. ──────────────────
TEST_F(QdmaSubsystemTest, TransferDeletedQpairNoFdLeakOnErrorPath) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_DEL, seq++).value()));

    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    int before = open_fd_count();
    for (int i = 0; i < 200; ++i) {
        UniqueFd buf = make_filled_buf(std::vector<uint8_t>(page, 0x55));
        slash_qdma_subxfer sx{};
        sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0; sx.dev_addr = kDevAddr; sx.length = page;
        auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, static_cast<uint32_t>(200 + i));
        ASSERT_TRUE(r.has_value()) << "iter " << i;
        EXPECT_EQ(-ENODEV, ret_of(r.value())) << "iter " << i;
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 3) << "before=" << before << " after=" << after
                                << " (fd leaked on -ENODEV path?)";
}

// ── Probe: many BUF_CREATE fds attached to ONE transfer, only some referenced ───
// resolve_fd_index moves out only the referenced index; unreferenced fds must be
// closed by the ReceivedMessage dtor.  Repeat to catch a leak.
TEST_F(QdmaSubsystemTest, TransferExtraUnreferencedFdsClosed) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    set_rcv_timeout(xfer.get(), 30000ms);
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    int before = open_fd_count();
    for (int i = 0; i < 100; ++i) {
        UniqueFd buf = make_filled_buf(std::vector<uint8_t>(page, 0x66));
        int e1 = ::dup(0), e2 = ::dup(0);
        ASSERT_GE(e1, 0); ASSERT_GE(e2, 0);
        // Send 3 fds; reference only index 1 (the real buffer).
        slash_qdma_subxfer sx{};
        sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 1; sx.dev_addr = kDevAddr; sx.length = page;
        auto r = do_transfer(xfer.get(), {sx}, {e1, buf.get(), e2},
                             static_cast<uint32_t>(400 + i));
        ::close(e1); ::close(e2);
        ASSERT_TRUE(r.has_value()) << "iter " << i;
        EXPECT_EQ(static_cast<int32_t>(page), ret_of(r.value())) << "iter " << i;
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 3) << "before=" << before << " after=" << after;
}

// ── Probe: TRANSFER on a session whose CTL parent connection has closed ─────────
// The XFER session is independent of the CTL connection that spawned it.  Closing
// the CTL client must NOT tear down the session; the transfer still works.
TEST_F(QdmaSubsystemTest, XferSessionSurvivesCtlClose) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd xfer;
    uint32_t qid = 0;
    {
        UniqueFd c = connect_client(sock_path_);
        ASSERT_TRUE(static_cast<bool>(c));
        uint32_t seq = 1;
        qid = add_started_qpair(c.get(), seq);
        xfer = open_xfer(c.get(), qid, seq);
        // c closes here (CTL connection gone), session must remain.
    }
    set_rcv_timeout(xfer.get(), 30000ms);
    ASSERT_EQ(1u, sub->session_count());
    const std::size_t page = static_cast<std::size_t>(::getpagesize());
    std::vector<uint8_t> src(page, 0x71);
    UniqueFd buf = make_filled_buf(src);
    slash_qdma_subxfer sx{};
    sx.direction = SLASH_QDMA_XFER_H2C; sx.buf_fd = 0; sx.dev_addr = kDevAddr; sx.length = page;
    auto r = do_transfer(xfer.get(), {sx}, {buf.get()}, 500);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(static_cast<int32_t>(page), ret_of(r.value()));
}

// ── Probe: DEL a Used qpair, then close the session — must not resurrect it ─────
// Used -[DEL underneath]-> gone.  On last close, session_loop only restores
// qpairs that are still present AND Used; a DELeted qid must stay gone.
TEST_F(QdmaSubsystemTest, DelUsedQpairThenCloseStaysGone) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    ASSERT_EQ(1u, sub->qpair_count());
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_DEL, seq++).value()));
    EXPECT_EQ(0u, sub->qpair_count());
    xfer.reset();
    auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
    while (sub->session_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    EXPECT_EQ(0u, sub->session_count());
    EXPECT_EQ(0u, sub->qpair_count()); // not resurrected
    // The qid is free to be reallocated by a fresh ADD (next_qid_ keeps counting).
    auto add = do_qpair_add(c.get(), kQdmaQModeMm, 0x3, seq++);
    ASSERT_TRUE(add.has_value());
    EXPECT_EQ(0, ret_of(add.value()));
}

// ── Probe: STOP a Used qpair, then close — session restores only if still Used ──
// STOP moves Used→Stopped (driver marks it stopped underneath).  On last close,
// the qpair is Stopped (not Used) so session_loop must leave it Stopped, NOT
// force it to Started.  (Already covered indirectly; this pins the Used→STOP edge
// explicitly by observing the qpair is Stopped afterwards via a failed GET_FD.)
TEST_F(QdmaSubsystemTest, StopUsedQpairThenCloseLeavesStopped) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq); // Used
    ASSERT_EQ(0, ret_of(do_q_op(c.get(), qid, SLASH_QDMA_QUEUE_OP_STOP, seq++).value()));
    xfer.reset();
    auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
    while (sub->session_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    EXPECT_EQ(1u, sub->qpair_count());
    // Stopped ⇒ GET_FD fails.
    EXPECT_EQ(-EINVAL, ret_of(do_get_fd(c.get(), qid, seq++).value()));
}

// ── Probe: interleaved H2C+C2H subxfers in one TRANSFER on one session ──────────
// Two subxfers, one H2C then one C2H on the same (bidirectional) qpair; both must
// apply and the return is their combined byte count.
TEST_F(QdmaSubsystemTest, TransferMixedH2cThenC2hOneCall) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    const std::size_t page = static_cast<std::size_t>(::getpagesize());

    // Seed model memory at addr2 for the C2H half.
    std::vector<uint8_t> seed(page);
    for (std::size_t i = 0; i < page; ++i) seed[i] = static_cast<uint8_t>(0x33 + (i & 7));
    const uint64_t addr2 = kDevAddr + 0x800000;
    server_->poke_buffer(addr2, seed);

    std::vector<uint8_t> h2c_src(page, 0x44);
    UniqueFd src_buf = make_filled_buf(h2c_src);
    UniqueFd dst_buf(::memfd_create("dst", MFD_CLOEXEC));
    ASSERT_TRUE(static_cast<bool>(dst_buf));
    ASSERT_EQ(0, ::ftruncate(dst_buf.get(), static_cast<off_t>(page)));

    slash_qdma_subxfer sx0{}; // H2C to kDevAddr from fd0
    sx0.qpair_index = 0; sx0.direction = SLASH_QDMA_XFER_H2C; sx0.buf_fd = 0;
    sx0.dev_addr = kDevAddr; sx0.length = page;
    slash_qdma_subxfer sx1{}; // C2H from addr2 into fd1
    sx1.qpair_index = 0; sx1.direction = SLASH_QDMA_XFER_C2H; sx1.buf_fd = 1;
    sx1.dev_addr = addr2; sx1.length = page;

    auto r = do_transfer(xfer.get(), {sx0, sx1}, {src_buf.get(), dst_buf.get()}, 14);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(static_cast<int32_t>(2 * page), ret_of(r.value()));
    // H2C landed in the model.
    EXPECT_EQ(0x44, server_->peek(kDevAddr));
    // C2H landed in the dst buffer.
    std::vector<uint8_t> got(page);
    ASSERT_EQ(static_cast<ssize_t>(page), ::pread(dst_buf.get(), got.data(), page, 0));
    EXPECT_EQ(seed, got);
}

// ── Probe: rapid setup→remove→setup with no ops must not leak fds/threads ───────
TEST_F(QdmaSubsystemTest, RapidSetupRemoveCyclesNoLeak) {
    int before = open_fd_count();
    {
        auto sub = make_subsystem();
        for (int i = 0; i < 20; ++i) {
            ASSERT_TRUE(sub->setup().has_value()) << "cycle " << i;
            sub->remove();
        }
    }
    int after = open_fd_count();
    EXPECT_LE(after, before + 2) << "before=" << before << " after=" << after;
}

// ── Probe: GET_FD list with a DUPLICATE qid is REJECTED (-EINVAL) ──────────────
// Contract (lead decision, mirrors the driver): binding the same qpair twice into
// one session has no legitimate caller.  handle_qpair_get_fd rejects {qid,qid}
// with -EINVAL BEFORE the Started→Used transition, so the qpair stays Started, no
// session is created, and no fd is handed back or leaked.
TEST_F(QdmaSubsystemTest, GetFdDuplicateQidRejected) {
    auto sub = make_subsystem();
    ASSERT_TRUE(sub->setup().has_value());
    UniqueFd c = connect_client(sock_path_);
    ASSERT_TRUE(static_cast<bool>(c));
    uint32_t seq = 1;
    uint32_t qid = add_started_qpair(c.get(), seq);
    int before = open_fd_count();
    auto r = do_get_fd_list(c.get(), {qid, qid}, seq++);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(-EINVAL, ret_of(r.value()));
    EXPECT_TRUE(r.value().fds.empty());
    // No session was created and no client fd was handed back / leaked.
    EXPECT_EQ(0u, sub->session_count());
    int after = open_fd_count();
    EXPECT_LE(after, before + 1) << "before=" << before << " after=" << after;
    // The qpair is untouched (still Started): a normal single-qid GET_FD works.
    UniqueFd xfer = open_xfer(c.get(), qid, seq);
    EXPECT_TRUE(static_cast<bool>(xfer));
}

} // namespace
} // namespace slash_emu
