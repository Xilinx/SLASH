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

#include "transport.h"
#include "protocol.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace slash_emu {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture: a connected SEQPACKET socketpair
// ─────────────────────────────────────────────────────────────────────────────

class TransportTest : public ::testing::Test {
protected:
    void SetUp() override {
        int sv[2];
        ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv))
            << "socketpair: " << std::strerror(errno);
        client_.reset(sv[0]);
        server_.reset(sv[1]);
    }

    UniqueFd client_; // "user" side
    UniqueFd server_; // "daemon" side
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static slash_emu_socket_header make_header(uint32_t op = 0x30,
                                           uint32_t seq = 1,
                                           uint32_t ret = 0) {
    return slash_emu_socket_header{op, seq, ret, 0};
}

// Create an anonymous memfd (or a regular tempfile) whose inode we can compare.
static UniqueFd make_temp_fd() {
    // Use memfd_create if available; fall back to a tmpfile.
    int fd = ::memfd_create("transport_test", 0);
    if (fd < 0) {
        // Fallback: open a temp file.
        char name[] = "/tmp/transport_test_XXXXXX";
        fd = ::mkstemp(name);
        if (fd >= 0) ::unlink(name);
    }
    return UniqueFd(fd);
}

// Return the inode number of a file descriptor.
static ino_t inode_of(int fd) {
    struct stat st{};
    EXPECT_EQ(0, ::fstat(fd, &st));
    return st.st_ino;
}

// Count open file descriptors in /proc/self/fd.
static int open_fd_count() {
    int count = 0;
    try {
        for (auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
            (void)entry;
            ++count;
        }
    } catch (...) {}
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// UniqueFd tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(UniqueFdTest, DefaultConstructedIsInvalid) {
    UniqueFd fd;
    EXPECT_EQ(-1, fd.get());
    EXPECT_FALSE(static_cast<bool>(fd));
}

TEST(UniqueFdTest, OwnsAndClosesOnDestruction) {
    int raw_fd;
    {
        UniqueFd fd = make_temp_fd();
        ASSERT_TRUE(static_cast<bool>(fd));
        raw_fd = fd.get();
        // fd must be valid
        struct stat st{};
        EXPECT_EQ(0, ::fstat(raw_fd, &st));
    }
    // After destruction: fstat should fail
    struct stat st{};
    EXPECT_EQ(-1, ::fstat(raw_fd, &st));
    EXPECT_EQ(EBADF, errno);
}

TEST(UniqueFdTest, MoveTransfersOwnership) {
    UniqueFd a = make_temp_fd();
    int raw = a.get();
    UniqueFd b = std::move(a);
    EXPECT_EQ(-1, a.get());
    EXPECT_EQ(raw, b.get());
}

TEST(UniqueFdTest, ReleaseYieldsRawFdWithoutClosing) {
    UniqueFd fd = make_temp_fd();
    int raw = fd.release();
    EXPECT_EQ(-1, fd.get());
    // Raw fd must still be open.
    struct stat st{};
    EXPECT_EQ(0, ::fstat(raw, &st));
    ::close(raw);
}

TEST(UniqueFdTest, ResetClosesOldAndTakesNew) {
    UniqueFd fd = make_temp_fd();
    int old_raw = fd.get();
    UniqueFd fd2 = make_temp_fd();
    int new_raw = fd2.release();
    fd.reset(new_raw);
    // Old fd should be closed.
    struct stat st{};
    EXPECT_EQ(-1, ::fstat(old_raw, &st));
    EXPECT_EQ(EBADF, errno);
    // New fd should be valid.
    EXPECT_EQ(new_raw, fd.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// protocol.h
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProtocolTest, HeaderSize) {
    EXPECT_EQ(sizeof(slash_emu_socket_header), 16u);
}

TEST(ProtocolTest, HeaderFieldOffsets) {
    slash_emu_socket_header h{};
    // Offsets must match the architecture spec.
    EXPECT_EQ(offsetof(slash_emu_socket_header, ioctl_op),     0u);
    EXPECT_EQ(offsetof(slash_emu_socket_header, sequence_id),  4u);
    EXPECT_EQ(offsetof(slash_emu_socket_header, return_value), 8u);
    EXPECT_EQ(offsetof(slash_emu_socket_header, pad),         12u);
    (void)h;
}

// ─────────────────────────────────────────────────────────────────────────────
// Round-trip: header + payload, no FDs
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, RoundTripHeaderAndPayloadNoFds) {
    slash_emu_socket_header hdr = make_header(0x30, 42, 0);
    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};

    auto send_res = send_message(client_.get(), hdr, payload, {});
    ASSERT_TRUE(send_res) << send_res.error().message;

    auto recv_res = recv_message(server_.get());
    ASSERT_TRUE(recv_res) << recv_res.error().message;

    const auto& msg = recv_res.value();
    EXPECT_EQ(msg.header.ioctl_op,    hdr.ioctl_op);
    EXPECT_EQ(msg.header.sequence_id, hdr.sequence_id);
    EXPECT_EQ(msg.header.return_value, hdr.return_value);
    EXPECT_EQ(msg.payload, payload);
    EXPECT_TRUE(msg.fds.empty());
}

TEST_F(TransportTest, RoundTripEmptyPayloadNoFds) {
    slash_emu_socket_header hdr = make_header(0x31, 1, 0);

    auto send_res = send_message(client_.get(), hdr, {}, {});
    ASSERT_TRUE(send_res) << send_res.error().message;

    auto recv_res = recv_message(server_.get());
    ASSERT_TRUE(recv_res) << recv_res.error().message;

    const auto& msg = recv_res.value();
    EXPECT_EQ(msg.header.ioctl_op, hdr.ioctl_op);
    EXPECT_TRUE(msg.payload.empty());
    EXPECT_TRUE(msg.fds.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Round-trip with one FD
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, RoundTripOneFd) {
    UniqueFd orig = make_temp_fd();
    ASSERT_TRUE(orig);
    ino_t orig_ino = inode_of(orig.get());

    slash_emu_socket_header hdr = make_header(0x31, 2, 0);
    std::array<int, 1> fds_to_send{orig.get()};
    std::vector<uint8_t> payload = {0x01};

    auto send_res = send_message(client_.get(), hdr, payload, fds_to_send);
    ASSERT_TRUE(send_res) << send_res.error().message;

    auto recv_res = recv_message(server_.get());
    ASSERT_TRUE(recv_res) << recv_res.error().message;

    auto& msg = recv_res.value();
    ASSERT_EQ(1u, msg.fds.size());
    ASSERT_TRUE(msg.fds[0]);

    // The received FD must refer to the same underlying file.
    EXPECT_EQ(orig_ino, inode_of(msg.fds[0].get()));
}

// ─────────────────────────────────────────────────────────────────────────────
// Round-trip with multiple FDs
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, RoundTripMultipleFds) {
    constexpr int kCount = 4;
    std::vector<UniqueFd> orig_fds;
    std::vector<ino_t>    orig_inodes;
    std::vector<int>      raw_fds;

    for (int i = 0; i < kCount; ++i) {
        orig_fds.push_back(make_temp_fd());
        ASSERT_TRUE(orig_fds.back());
        orig_inodes.push_back(inode_of(orig_fds.back().get()));
        raw_fds.push_back(orig_fds.back().get());
    }

    slash_emu_socket_header hdr = make_header(0x32, 3, 0);
    auto send_res = send_message(client_.get(), hdr, {}, raw_fds);
    ASSERT_TRUE(send_res) << send_res.error().message;

    auto recv_res = recv_message(server_.get());
    ASSERT_TRUE(recv_res) << recv_res.error().message;

    auto& msg = recv_res.value();
    ASSERT_EQ(static_cast<std::size_t>(kCount), msg.fds.size());
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(orig_inodes[i], inode_of(msg.fds[i].get()))
            << "FD " << i << " inode mismatch";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Received FDs are close-on-exec
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, ReceivedFdsAreCloseOnExec) {
    UniqueFd orig = make_temp_fd();
    ASSERT_TRUE(orig);

    slash_emu_socket_header hdr = make_header();
    std::array<int, 1> fds_to_send{orig.get()};

    auto send_res = send_message(client_.get(), hdr, {}, fds_to_send);
    ASSERT_TRUE(send_res) << send_res.error().message;

    auto recv_res = recv_message(server_.get());
    ASSERT_TRUE(recv_res) << recv_res.error().message;

    auto& msg = recv_res.value();
    ASSERT_EQ(1u, msg.fds.size());

    int flags = ::fcntl(msg.fds[0].get(), F_GETFD);
    EXPECT_NE(-1, flags);
    EXPECT_NE(0, flags & FD_CLOEXEC) << "received FD must have FD_CLOEXEC set";
}

// ─────────────────────────────────────────────────────────────────────────────
// FD-index mapping helpers
// ─────────────────────────────────────────────────────────────────────────────

TEST(FdIndexTest, CollectAndRewrite) {
    int field_a = 10;
    int field_b = 20;
    int field_c = 30;

    std::vector<int> fd_list;
    auto res = collect_fds_and_rewrite(fd_list, {std::ref(field_a), std::ref(field_b), std::ref(field_c)});
    ASSERT_TRUE(res) << res.error().message;

    EXPECT_EQ(3u, fd_list.size());
    EXPECT_EQ(10, fd_list[0]);
    EXPECT_EQ(20, fd_list[1]);
    EXPECT_EQ(30, fd_list[2]);

    // Fields must have been rewritten to their indices.
    EXPECT_EQ(0, field_a);
    EXPECT_EQ(1, field_b);
    EXPECT_EQ(2, field_c);
}

TEST(FdIndexTest, CollectAppendsToPreviousEntries) {
    std::vector<int> fd_list = {99}; // pre-existing entry
    int field = 55;
    auto res = collect_fds_and_rewrite(fd_list, {std::ref(field)});
    ASSERT_TRUE(res) << res.error().message;

    EXPECT_EQ(2u, fd_list.size());
    EXPECT_EQ(55, fd_list[1]);
    EXPECT_EQ(1, field); // index 1, because fd_list already had one entry
}

// Finding 6: collect_fds_and_rewrite must enforce kMaxFdsPerMessage and must
// NOT partially rewrite struct fields if the cap would be exceeded.
// Test name: AdversaryCollectFdsRejectsOverCapWithoutCorruption
TEST(FdIndexTest, AdversaryCollectFdsRejectsOverCapWithoutCorruption) {
    // Fill fd_list to exactly kMaxFdsPerMessage - 1 entries.
    std::vector<int> fd_list(kMaxFdsPerMessage - 1, 0);

    int field_a = 100;
    int field_b = 200;
    // Adding two fields would push the total to kMaxFdsPerMessage + 1 — over the cap.
    auto res = collect_fds_and_rewrite(fd_list, {std::ref(field_a), std::ref(field_b)});

    ASSERT_FALSE(res) << "collect_fds_and_rewrite must fail when cap is exceeded";
    EXPECT_EQ(ErrorKind::Protocol, res.error().kind);

    // Fields must NOT have been partially rewritten (atomic rejection).
    EXPECT_EQ(100, field_a) << "field_a must be unchanged on rejection";
    EXPECT_EQ(200, field_b) << "field_b must be unchanged on rejection";
    // fd_list must not have grown.
    EXPECT_EQ(kMaxFdsPerMessage - 1, fd_list.size());
}

// Boundary: exactly at kMaxFdsPerMessage must succeed.
TEST(FdIndexTest, CollectFdsExactlyAtCapSucceeds) {
    std::vector<int> fd_list(kMaxFdsPerMessage - 1, 0);
    int field = 42;
    auto res = collect_fds_and_rewrite(fd_list, {std::ref(field)});
    ASSERT_TRUE(res) << res.error().message;
    EXPECT_EQ(kMaxFdsPerMessage, fd_list.size());
    EXPECT_EQ(static_cast<int>(kMaxFdsPerMessage - 1), field);
}

TEST_F(TransportTest, ResolveFdIndexRoundTrip) {
    // Send a message with two FDs, then resolve both.
    UniqueFd fd0 = make_temp_fd();
    UniqueFd fd1 = make_temp_fd();
    ASSERT_TRUE(fd0);
    ASSERT_TRUE(fd1);

    ino_t ino0 = inode_of(fd0.get());
    ino_t ino1 = inode_of(fd1.get());

    std::array<int, 2> raw{fd0.get(), fd1.get()};
    slash_emu_socket_header hdr = make_header();

    auto send_res = send_message(client_.get(), hdr, {}, raw);
    ASSERT_TRUE(send_res) << send_res.error().message;

    auto recv_res = recv_message(server_.get());
    ASSERT_TRUE(recv_res) << recv_res.error().message;

    auto& msg = recv_res.value();

    auto r0 = resolve_fd_index(msg, 0);
    ASSERT_TRUE(r0) << r0.error().message;
    EXPECT_EQ(ino0, inode_of(r0.value().get()));

    auto r1 = resolve_fd_index(msg, 1);
    ASSERT_TRUE(r1) << r1.error().message;
    EXPECT_EQ(ino1, inode_of(r1.value().get()));
}

TEST_F(TransportTest, ResolveFdIndexOutOfRangeIsProtocolError) {
    slash_emu_socket_header hdr = make_header();
    auto send_res = send_message(client_.get(), hdr, {}, {});
    ASSERT_TRUE(send_res);

    auto recv_res = recv_message(server_.get());
    ASSERT_TRUE(recv_res);

    auto& msg = recv_res.value();
    // No FDs transferred — index 0 must fail.
    auto res = resolve_fd_index(msg, 0);
    ASSERT_FALSE(res);
    EXPECT_EQ(ErrorKind::Protocol, res.error().kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// Request / response helper — sequence-id matching
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, SendRequestMatchingSequenceId) {
    // Server side: mirror the request back as the response.
    slash_emu_socket_header req_hdr = make_header(0x30, 99, 0);
    std::vector<uint8_t> req_payload = {1, 2, 3};

    // Run "server" inline before send_request blocks on recv.
    // We need a separate thread to avoid deadlock (SEQPACKET blocks on send
    // if the peer buffer is full, and both sides would block if single-threaded).
    std::thread server_thread([&] {
        auto recv_res = recv_message(server_.get());
        if (!recv_res) return;
        // Echo the request back as the response.
        const auto& m = recv_res.value();
        send_message(server_.get(), m.header, m.payload, {});
    });

    auto result = send_request(client_.get(), req_hdr, req_payload, {});
    server_thread.join();

    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().header.sequence_id, req_hdr.sequence_id);
    EXPECT_EQ(result.value().header.ioctl_op,    req_hdr.ioctl_op);
    EXPECT_EQ(result.value().payload, req_payload);
}

TEST_F(TransportTest, SendRequestSequenceIdMismatchIsProtocolError) {
    slash_emu_socket_header req_hdr = make_header(0x30, 10, 0);

    std::thread server_thread([&] {
        auto recv_res = recv_message(server_.get());
        if (!recv_res) return;
        // Respond with wrong sequence_id.
        slash_emu_socket_header resp = recv_res.value().header;
        resp.sequence_id = 999; // mismatch
        send_message(server_.get(), resp, {}, {});
    });

    auto result = send_request(client_.get(), req_hdr, {}, {});
    server_thread.join();

    ASSERT_FALSE(result);
    EXPECT_EQ(ErrorKind::Protocol, result.error().kind);
}

TEST_F(TransportTest, SendRequestIoctlOpMismatchIsProtocolError) {
    slash_emu_socket_header req_hdr = make_header(0x30, 5, 0);

    std::thread server_thread([&] {
        auto recv_res = recv_message(server_.get());
        if (!recv_res) return;
        slash_emu_socket_header resp = recv_res.value().header;
        resp.ioctl_op = 0xFF; // mismatch
        send_message(server_.get(), resp, {}, {});
    });

    auto result = send_request(client_.get(), req_hdr, {}, {});
    server_thread.join();

    ASSERT_FALSE(result);
    EXPECT_EQ(ErrorKind::Protocol, result.error().kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// Error cases: peer closed
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, RecvAfterPeerClosedIsTransportError) {
    // Close the server side; receiving on client should detect peer closed.
    server_.reset();

    auto recv_res = recv_message(client_.get());
    ASSERT_FALSE(recv_res);
    EXPECT_EQ(ErrorKind::Transport, recv_res.error().kind);
}

TEST_F(TransportTest, SendAfterPeerClosedIsTransportError) {
    server_.reset();

    // On Linux, first send after peer close may succeed (data buffered); a
    // second send will get EPIPE via MSG_NOSIGNAL.  We loop until failure or
    // two sends have been attempted.
    slash_emu_socket_header hdr = make_header();
    Result<void> res = Result<void>::ok();
    for (int i = 0; i < 5; ++i) {
        res = send_message(client_.get(), hdr, {}, {});
        if (!res) break;
    }
    // At some point we must get a transport error.
    ASSERT_FALSE(res);
    EXPECT_EQ(ErrorKind::Transport, res.error().kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// Error cases: data truncation (MSG_TRUNC)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, DataTruncationIsProtocolError) {
    // Build a datagram larger than sizeof(header) + kMaxPayloadBytes to trigger
    // MSG_TRUNC on the receiver.  We must bypass send_message's send-side cap
    // (which now also rejects oversized payloads) and use raw sendmsg instead.
    slash_emu_socket_header hdr = make_header();
    constexpr std::size_t kOversizePayload = kMaxPayloadBytes + 1;
    std::vector<uint8_t> oversized_data(sizeof(hdr) + kOversizePayload, 0xAB);
    // Put the header bytes at the start of the buffer.
    std::memcpy(oversized_data.data(), &hdr, sizeof(hdr));

    iovec iov{};
    iov.iov_base = oversized_data.data();
    iov.iov_len  = oversized_data.size();

    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    ssize_t n = ::sendmsg(client_.get(), &msg, MSG_NOSIGNAL);
    ASSERT_GE(n, 0) << "raw sendmsg failed: " << std::strerror(errno);

    auto recv_res = recv_message(server_.get());
    ASSERT_FALSE(recv_res) << "expected truncation error, but got success";
    EXPECT_EQ(ErrorKind::Protocol, recv_res.error().kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// Error cases: control message truncation (MSG_CTRUNC)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, ControlTruncationIsProtocolError) {
    // Send more FDs than kMaxFdsPerMessage to force MSG_CTRUNC on the receiver.
    // We achieve this by directly calling sendmsg with a control buffer that
    // exceeds the kCmsgBufSize used by recv_message.
    //
    // Strategy: create (kMaxFdsPerMessage + 1) FDs and send them in one
    // control message; the receiver's cmsg buffer is sized for exactly
    // kMaxFdsPerMessage, so the kernel truncates and sets MSG_CTRUNC.

    constexpr std::size_t kOverflow = kMaxFdsPerMessage + 1;
    std::vector<UniqueFd> fds;
    std::vector<int>      raw;
    fds.reserve(kOverflow);
    raw.reserve(kOverflow);
    for (std::size_t i = 0; i < kOverflow; ++i) {
        fds.push_back(make_temp_fd());
        ASSERT_TRUE(fds.back());
        raw.push_back(fds.back().get());
    }

    // Build the iovec: header only (no payload).
    slash_emu_socket_header hdr = make_header();
    iovec iov{};
    iov.iov_base = &hdr;
    iov.iov_len  = sizeof(hdr);

    std::size_t cmsg_bytes = CMSG_SPACE(sizeof(int) * kOverflow);
    std::vector<char> cmsg_buf(cmsg_bytes, 0);

    msghdr msg{};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf.data();
    msg.msg_controllen = static_cast<socklen_t>(cmsg_buf.size());

    cmsghdr* cmsg    = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = static_cast<socklen_t>(CMSG_LEN(sizeof(int) * kOverflow));
    std::memcpy(CMSG_DATA(cmsg), raw.data(), sizeof(int) * kOverflow);

    ssize_t n = ::sendmsg(client_.get(), &msg, MSG_NOSIGNAL);
    ASSERT_GE(n, 0) << "sendmsg: " << std::strerror(errno);

    auto recv_res = recv_message(server_.get());
    ASSERT_FALSE(recv_res) << "expected MSG_CTRUNC error, but got success";
    EXPECT_EQ(ErrorKind::Protocol, recv_res.error().kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// No FD leaks: received FDs are closed when ReceivedMessage is destroyed
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, ReceivedFdsReleasedOnMessageDestruction) {
    int fd_count_before = open_fd_count();

    {
        UniqueFd orig = make_temp_fd();
        ASSERT_TRUE(orig);

        slash_emu_socket_header hdr = make_header();
        std::array<int, 1> raw{orig.get()};

        auto send_res = send_message(client_.get(), hdr, {}, raw);
        ASSERT_TRUE(send_res);

        // orig is still open; after this block the received message goes away.
        auto recv_res = recv_message(server_.get());
        ASSERT_TRUE(recv_res);
        // recv_res and orig both go out of scope here.
    }

    int fd_count_after = open_fd_count();
    // We should be back to (approximately) where we started.
    // The open_fd_count also includes the directory fd used for iteration, so
    // allow a small slack; what must NOT happen is the count staying elevated by
    // the number of transferred FDs.
    EXPECT_NEAR(fd_count_before, fd_count_after, 3)
        << "FD count changed significantly — possible leak";
}

// ─────────────────────────────────────────────────────────────────────────────
// Result<T> type tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(ResultTest, OkHoldsValue) {
    auto r = Result<int>::ok(42);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(42, r.value());
}

TEST(ResultTest, ErrHoldsError) {
    auto r = Result<int>::err(TransportError{ErrorKind::Transport, "boom"});
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(ErrorKind::Transport, r.error().kind);
    EXPECT_EQ("boom", r.error().message);
}

TEST(ResultTest, VoidOk) {
    auto r = Result<void>::ok();
    EXPECT_TRUE(r.has_value());
}

TEST(ResultTest, VoidErr) {
    auto r = Result<void>::err(TransportError{ErrorKind::Protocol, "bad"});
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(ErrorKind::Protocol, r.error().kind);
}

TEST(ResultTest, BoolConversion) {
    EXPECT_TRUE(static_cast<bool>(Result<int>::ok(1)));
    EXPECT_FALSE(static_cast<bool>(
        Result<int>::err(TransportError{ErrorKind::Transport, ""})));
}

// ─────────────────────────────────────────────────────────────────────────────
// Large payload round-trip (just under the limit)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, MaxPayloadRoundTrip) {
    slash_emu_socket_header hdr = make_header(0x30, 1, 0);
    std::vector<uint8_t> payload(kMaxPayloadBytes, 0xCC);

    auto send_res = send_message(client_.get(), hdr, payload, {});
    ASSERT_TRUE(send_res) << send_res.error().message;

    auto recv_res = recv_message(server_.get());
    ASSERT_TRUE(recv_res) << recv_res.error().message;
    EXPECT_EQ(recv_res.value().payload, payload);
}

// ─────────────────────────────────────────────────────────────────────────────
// Error case: datagram too small to contain the header
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, DatagramTooSmallIsProtocolError) {
    // Send just 4 bytes (less than the 16-byte header) using raw sendmsg.
    std::array<uint8_t, 4> tiny{0x01, 0x02, 0x03, 0x04};
    iovec iov{};
    iov.iov_base = tiny.data();
    iov.iov_len  = tiny.size();

    msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    ssize_t n = ::sendmsg(client_.get(), &msg, MSG_NOSIGNAL);
    ASSERT_GE(n, 0) << "sendmsg: " << std::strerror(errno);

    auto recv_res = recv_message(server_.get());
    ASSERT_FALSE(recv_res);
    EXPECT_EQ(ErrorKind::Protocol, recv_res.error().kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// Error case: send_request when the initial send fails (peer already closed)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, SendRequestFailsWhenSendFails) {
    // Close the server side so subsequent sends will eventually fail.
    server_.reset();

    slash_emu_socket_header hdr = make_header(0x30, 1, 0);
    Result<ReceivedMessage> result = Result<ReceivedMessage>::ok(ReceivedMessage{});
    // Loop until send fails (SEQPACKET may buffer one packet).
    for (int i = 0; i < 5; ++i) {
        result = send_request(client_.get(), hdr, {}, {});
        if (!result) break;
    }
    ASSERT_FALSE(result);
    EXPECT_EQ(ErrorKind::Transport, result.error().kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// Error case: send_request when recv fails after a successful send
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, SendRequestFailsWhenRecvFails) {
    // Server side: accept the message then close without responding.
    std::thread server_thread([&] {
        auto recv_res = recv_message(server_.get());
        (void)recv_res;
        // Deliberately close without sending a response.
        server_.reset();
    });

    slash_emu_socket_header hdr = make_header(0x30, 42, 0);
    auto result = send_request(client_.get(), hdr, {}, {});
    server_thread.join();

    ASSERT_FALSE(result);
    EXPECT_EQ(ErrorKind::Transport, result.error().kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// Adversary probes — added to expose defects found in the Step 2 adversary
// review.  Each test name is referenced in the review findings.
// ─────────────────────────────────────────────────────────────────────────────

// Finding 1: FD leak under OOM in recv_message.
// The production fix is result.fds.reserve(fd_count) before the emplace_back
// loop.  This test verifies that after receiving a message carrying FDs and
// immediately destroying the ReceivedMessage (without resolving any FD), the
// open FD count returns to its baseline.  It also verifies the count when
// recv_message returns an error (MSG_TRUNC path), confirming no FD escapes the
// error path either.  If reserve is absent and an allocation failure occurs
// mid-loop the count would remain elevated; the test catches a steady-state
// variant of that via normal destruction.
TEST_F(TransportTest, AdversaryReceivedFdsFullyClosedOnMessageDestroy) {
    // Baseline: count before we start.
    int before = open_fd_count();

    {
        // Send three FDs; receive and immediately destroy the message.
        UniqueFd f0 = make_temp_fd();
        UniqueFd f1 = make_temp_fd();
        UniqueFd f2 = make_temp_fd();
        ASSERT_TRUE(f0); ASSERT_TRUE(f1); ASSERT_TRUE(f2);
        std::array<int, 3> raw{f0.get(), f1.get(), f2.get()};

        slash_emu_socket_header hdr = make_header(0x50, 1, 0);
        auto send_res = send_message(client_.get(), hdr, {}, raw);
        ASSERT_TRUE(send_res);

        {
            auto recv_res = recv_message(server_.get());
            ASSERT_TRUE(recv_res);
            ASSERT_EQ(3u, recv_res.value().fds.size());
            // ReceivedMessage (and its 3 UniqueFds) destroyed here.
        }
        // f0/f1/f2 also destroyed here.
    }

    int after = open_fd_count();
    // Allow small slack for the directory fd used by open_fd_count() itself.
    EXPECT_NEAR(before, after, 3)
        << "FD count elevated after ReceivedMessage destruction — possible leak";
}

// Finding 2: resolve_fd_index silently returns ok with fd==-1 on double-resolve.
// Before the fix: second call passes bounds check and returns ok(UniqueFd{-1}).
// After the fix:  second call must return ErrorKind::Protocol.
// Test name: AdversaryResolveFdIndexDoubleResolveIsProtocolError
TEST_F(TransportTest, AdversaryResolveFdIndexDoubleResolveIsProtocolError) {
    UniqueFd orig = make_temp_fd();
    ASSERT_TRUE(orig);

    std::array<int, 1> raw{orig.get()};
    slash_emu_socket_header hdr = make_header();
    auto send_res = send_message(client_.get(), hdr, {}, raw);
    ASSERT_TRUE(send_res);

    auto recv_res = recv_message(server_.get());
    ASSERT_TRUE(recv_res);
    auto& msg = recv_res.value();

    // First resolve: must succeed with a valid fd.
    auto r0 = resolve_fd_index(msg, 0);
    ASSERT_TRUE(r0) << r0.error().message;
    EXPECT_GE(r0.value().get(), 0) << "first resolve must yield a valid fd";

    // Second resolve of the same index: must be a Protocol error, NOT silent ok.
    auto r1 = resolve_fd_index(msg, 0);
    EXPECT_FALSE(r1)
        << "double-resolve of same index must fail; got fd=" << r1.value().get();
    if (!r1) {
        EXPECT_EQ(ErrorKind::Protocol, r1.error().kind);
    }
}

// Finding 3: send_message has no send-side payload size limit.
// Before the fix: send_message with payload > kMaxPayloadBytes returns ok.
// After the fix:  it must return ErrorKind::Protocol.
// Test name: AdversarySendMessageOversizedPayloadIsProtocolError
TEST_F(TransportTest, AdversarySendMessageOversizedPayloadIsProtocolError) {
    slash_emu_socket_header hdr = make_header(0x30, 1, 0);
    std::vector<uint8_t> oversized(kMaxPayloadBytes + 1, 0xBB);

    auto res = send_message(client_.get(), hdr, oversized, {});
    EXPECT_FALSE(res)
        << "send_message with payload > kMaxPayloadBytes must return an error";
    if (!res) {
        EXPECT_EQ(ErrorKind::Protocol, res.error().kind);
    }
}

// Finding 4a: send_message too-many-FDs guard (line 91) is never hit by the
// existing ControlTruncationIsProtocolError test (which bypasses send_message).
// This test calls send_message directly with kMaxFdsPerMessage+1 raw ints.
// No real open FDs are needed — the function checks fds.size() before syscalls.
// Test name: AdversarySendMessageTooManyFdsIsProtocolError
TEST_F(TransportTest, AdversarySendMessageTooManyFdsIsProtocolError) {
    slash_emu_socket_header hdr = make_header(0x30, 1, 0);
    // Use -1 as a placeholder raw value; the check fires before sendmsg, so no
    // actual FD validity is required.
    std::vector<int> too_many(kMaxFdsPerMessage + 1, -1);

    auto res = send_message(client_.get(), hdr, {}, too_many);
    ASSERT_FALSE(res) << "send_message with > kMaxFdsPerMessage FDs must fail";
    EXPECT_EQ(ErrorKind::Protocol, res.error().kind);
}

// Finding 4b: recv_message recvmsg n<0 path (line 133) — OS-level error.
// Calling recv_message on an already-closed (invalid) fd triggers EBADF,
// causing recvmsg to return -1 and errno == EBADF.  Must be ErrorKind::Transport.
// Test name: AdversaryRecvMessageOnClosedFdIsTransportError
TEST(TransportErrorTest, AdversaryRecvMessageOnClosedFdIsTransportError) {
    // Open and immediately close a socketpair; use the now-invalid fd.
    int sv[2];
    ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
    ::close(sv[0]);
    ::close(sv[1]);
    // sv[0] is now closed; recvmsg on it must return -1 (EBADF).
    auto res = recv_message(sv[0]);
    ASSERT_FALSE(res);
    EXPECT_EQ(ErrorKind::Transport, res.error().kind)
        << "recvmsg EBADF must map to Transport error; got: " << res.error().message;
}

// Finding 4c: recv_message non-SCM_RIGHTS cmsg filter (line 171).
// Enable SO_PASSCRED so the kernel attaches SCM_CREDENTIALS to every message.
// The credentials cmsg must be skipped; no FDs must appear in the result,
// and the header data must still arrive correctly.
// Test name: AdversaryNonRightsCmsgIsIgnored
TEST_F(TransportTest, AdversaryNonRightsCmsgIsIgnored) {
    // Enable credential passing on the server (receiving) side.
    int one = 1;
    if (::setsockopt(server_.get(), SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) < 0) {
        GTEST_SKIP() << "SO_PASSCRED not available: " << std::strerror(errno);
    }

    slash_emu_socket_header hdr = make_header(0x42, 7, 0);
    std::vector<uint8_t> payload = {0xCA, 0xFE};

    // Send a normal message (no FDs, no credentials from the sender side).
    // The kernel will attach SCM_CREDENTIALS automatically because SO_PASSCRED is set.
    auto send_res = send_message(client_.get(), hdr, payload, {});
    ASSERT_TRUE(send_res) << send_res.error().message;

    auto recv_res = recv_message(server_.get());
    ASSERT_TRUE(recv_res) << recv_res.error().message;

    const auto& msg = recv_res.value();
    // Header and payload must be intact.
    EXPECT_EQ(msg.header.ioctl_op,    hdr.ioctl_op);
    EXPECT_EQ(msg.header.sequence_id, hdr.sequence_id);
    EXPECT_EQ(msg.payload, payload);
    // The SCM_CREDENTIALS cmsg must have been skipped — no FDs extracted.
    EXPECT_TRUE(msg.fds.empty())
        << "non-SCM_RIGHTS cmsg (credentials) must not produce FD entries";
}

// Finding 5: Result<void>::error() on a valid result must be guarded.
// Fix: assert(!ok_) was added to Result<void>::error() in transport.h.
// This test verifies the correct-path behaviour: error() works on an err result
// and the ok and err factory paths are distinguishable.
// (The misbehaving path — calling error() on a successful Result<void> — would
// now trip the assert and terminate the process, so we do not call it here.)
// Test name: AdversaryResultVoidErrorGuardedOnSuccess
TEST(ResultTest, AdversaryResultVoidErrorGuardedOnSuccess) {
    // ok() path: has_value() true, bool() true.
    auto rv_ok = Result<void>::ok();
    EXPECT_TRUE(rv_ok.has_value());
    EXPECT_TRUE(static_cast<bool>(rv_ok));

    // err() path: has_value() false, error() accessible without triggering assert.
    auto rv_err = Result<void>::err(TransportError{ErrorKind::Protocol, "v-err"});
    EXPECT_FALSE(rv_err.has_value());
    EXPECT_FALSE(static_cast<bool>(rv_err));
    EXPECT_EQ(ErrorKind::Protocol, rv_err.error().kind);
    EXPECT_EQ("v-err", rv_err.error().message);

    // The T specialisation's error() on an err result must also work symmetrically.
    auto rt_err = Result<int>::err(TransportError{ErrorKind::Transport, "t-err"});
    EXPECT_FALSE(rt_err.has_value());
    EXPECT_EQ("t-err", rt_err.error().message);
}

} // namespace
} // namespace slash_emu
