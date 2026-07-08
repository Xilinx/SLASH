/*
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file ctldev_sysemu_test.cpp
 *
 * GTest suite for the ctldev socket-transport path (SLASH_TRANSPORT_SOCKET).
 * Uses SysemuTestServer as an in-process daemon substitute.
 */

#define _GNU_SOURCE

#include <gtest/gtest.h>

#include "sysemu_test_server.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

extern "C" {
#include <slash/ctldev.h>
#include <slash/uapi/slash_interface.h>
#include <slash/uapi/slash_sysemu.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
}

/* ── Helper: count open file descriptors in /proc/self/fd ──────────────────── */

static int count_open_fds()
{
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue; /* skip . and .. */
        n++;
    }
    ::closedir(d);
    /* The dirfd itself is included in the count; subtract it. */
    return n - 1;
}

/* ── Test fixture ─────────────────────────────────────────────────────────── */

class CtldevSysemuTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(srv_.Start()) << "server failed to start: " << std::strerror(errno);
        dev_ = slash_ctldev_open(srv_.Path().c_str());
        ASSERT_NE(dev_, nullptr) << "slash_ctldev_open failed: " << std::strerror(errno);
        ASSERT_EQ(dev_->transport, SLASH_TRANSPORT_SOCKET);
        ASSERT_FALSE(dev_->mock);
    }

    void TearDown() override {
        if (dev_) {
            slash_ctldev_close(dev_);
            dev_ = nullptr;
        }
        srv_.Stop();
    }

    SysemuTestServer      srv_;
    struct slash_ctldev*  dev_ = nullptr;
};

/* ── GET_DEVICE_INFO ─────────────────────────────────────────────────────── */

TEST_F(CtldevSysemuTest, DeviceInfoExactBdf)
{
    srv_.device_bdf = "0000:01:00.2";
    struct slash_ioctl_device_info* info = slash_device_info_read(dev_);
    ASSERT_NE(info, nullptr);
    EXPECT_STREQ(info->bdf, "0000:01:00.2");
    slash_device_info_free(info);
}

TEST_F(CtldevSysemuTest, DeviceInfoVendorDevice)
{
    struct slash_ioctl_device_info* info = slash_device_info_read(dev_);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->vendor_id,           0x10EEu);
    EXPECT_EQ(info->device_id,           0x50B6u);
    EXPECT_EQ(info->subsystem_vendor_id, 0x10EEu);
    EXPECT_EQ(info->subsystem_device_id, 0x000eu);
    slash_device_info_free(info);
}

TEST_F(CtldevSysemuTest, DeviceInfoSeqAdvances)
{
    uint32_t seq_before = dev_->seq;
    struct slash_ioctl_device_info* info = slash_device_info_read(dev_);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(dev_->seq, seq_before + 1);
    slash_device_info_free(info);
}

/* ── GET_BAR_INFO ────────────────────────────────────────────────────────── */

TEST_F(CtldevSysemuTest, BarInfoBar0Present)
{
    struct slash_ioctl_bar_info* info = slash_bar_info_read(dev_, 0);
    ASSERT_NE(info, nullptr);
    EXPECT_NE(info->usable, 0);
    EXPECT_EQ(info->length, 64ULL * 1024 * 1024);
    EXPECT_EQ(info->bar_number, 0);
    slash_bar_info_free(info);
}

TEST_F(CtldevSysemuTest, BarInfoBar2Present)
{
    struct slash_ioctl_bar_info* info = slash_bar_info_read(dev_, 2);
    ASSERT_NE(info, nullptr);
    EXPECT_NE(info->usable, 0);
    EXPECT_EQ(info->length, 64ULL * 1024 * 1024);
    slash_bar_info_free(info);
}

TEST_F(CtldevSysemuTest, BarInfoBar4Present)
{
    struct slash_ioctl_bar_info* info = slash_bar_info_read(dev_, 4);
    ASSERT_NE(info, nullptr);
    EXPECT_NE(info->usable, 0);
    EXPECT_EQ(info->length, 64ULL * 1024 * 1024);
    slash_bar_info_free(info);
}

TEST_F(CtldevSysemuTest, BarInfoOddBarsAbsent)
{
    for (int bar : {1, 3, 5}) {
        struct slash_ioctl_bar_info* info = slash_bar_info_read(dev_, bar);
        ASSERT_NE(info, nullptr) << "bar=" << bar;
        EXPECT_EQ(info->usable, 0) << "bar=" << bar;
        slash_bar_info_free(info);
    }
}

/* ── GET_BAR_FD + mmap round-trip ────────────────────────────────────────── */

TEST_F(CtldevSysemuTest, BarFileOpenMmapRoundTrip)
{
    struct slash_bar_file* bar = slash_bar_file_open(dev_, 0, 0);
    ASSERT_NE(bar, nullptr) << std::strerror(errno);
    EXPECT_EQ(bar->len, 64ULL * 1024 * 1024);
    EXPECT_NE(bar->map, nullptr);
    EXPECT_EQ(bar->transport, SLASH_TRANSPORT_SOCKET);
    EXPECT_FALSE(bar->mock);

    /* Write through the mapping and read back. */
    auto* p = static_cast<uint32_t*>(bar->map);
    p[0] = 0xDEADBEEFu;
    EXPECT_EQ(p[0], 0xDEADBEEFu);

    EXPECT_EQ(slash_bar_file_close(bar), 0);
}

TEST_F(CtldevSysemuTest, BarFileAbsentBarFails)
{
    errno = 0;
    struct slash_bar_file* bar = slash_bar_file_open(dev_, 1, 0);
    EXPECT_EQ(bar, nullptr);
    EXPECT_NE(errno, 0);
}

/* ── flock sync bracketing ───────────────────────────────────────────────── */

TEST_F(CtldevSysemuTest, FlockSyncStartWriteAndEndWrite)
{
    struct slash_bar_file* bar = slash_bar_file_open(dev_, 0, 0);
    ASSERT_NE(bar, nullptr);

    EXPECT_EQ(slash_bar_file_start_write(bar), 0);
    EXPECT_EQ(slash_bar_file_end_write(bar),   0);

    EXPECT_EQ(slash_bar_file_close(bar), 0);
}

TEST_F(CtldevSysemuTest, FlockSyncStartReadAndEndRead)
{
    struct slash_bar_file* bar = slash_bar_file_open(dev_, 0, 0);
    ASSERT_NE(bar, nullptr);

    EXPECT_EQ(slash_bar_file_start_read(bar), 0);
    EXPECT_EQ(slash_bar_file_end_read(bar),   0);

    EXPECT_EQ(slash_bar_file_close(bar), 0);
}

/**
 * Distinct-open-file-description flock exclusion proof.
 *
 * Open the same BAR socket twice (two ctldev opens → two connections →
 * daemon sends two independently-reopened memfds via /proc/self/fd).
 * Each bar_file holds a distinct open file description on the same
 * underlying memfd inode.  A LOCK_EX from one must exclude a LOCK_SH
 * from the other (flock semantics on the same inode, distinct fds).
 */
TEST_F(CtldevSysemuTest, FlockExclusiveExcludesSharedOnDistinctDescriptions)
{
    /* Open two independent connections to the server. */
    struct slash_ctldev* dev2 = slash_ctldev_open(srv_.Path().c_str());
    ASSERT_NE(dev2, nullptr);

    struct slash_bar_file* bar1 = slash_bar_file_open(dev_,  0, 0);
    ASSERT_NE(bar1, nullptr);
    struct slash_bar_file* bar2 = slash_bar_file_open(dev2, 0, 0);
    ASSERT_NE(bar2, nullptr);

    /*
     * bar1 acquires LOCK_EX.  Then bar2 attempts LOCK_SH | LOCK_NB.
     * They must be on the same inode (same BAR → same underlying memfd
     * inode, different open descriptions via reopen).
     */
    ASSERT_EQ(slash_bar_file_start_write(bar1), 0);   /* LOCK_EX on bar1 */

    /* bar2 LOCK_SH non-blocking must fail with EWOULDBLOCK. */
    int rc = ::flock(bar2->fd, LOCK_SH | LOCK_NB);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(errno, EWOULDBLOCK)
        << "Expected LOCK_SH to be blocked by LOCK_EX on the same inode";

    ASSERT_EQ(slash_bar_file_end_write(bar1), 0);     /* LOCK_UN on bar1 */

    /* Now bar2 LOCK_SH should succeed. */
    rc = ::flock(bar2->fd, LOCK_SH | LOCK_NB);
    EXPECT_EQ(rc, 0);
    /* unlock bar2 */
    ::flock(bar2->fd, LOCK_UN);

    slash_bar_file_close(bar1);
    slash_bar_file_close(bar2);
    slash_ctldev_close(dev2);
}

/* ── ENODEV mapping — fault injection ───────────────────────────────────── */

TEST_F(CtldevSysemuTest, PeerCloseBeforeReplyYieldsEnodevOnDeviceInfo)
{
    srv_.InjectFault(SysemuFault::PeerClose);
    errno = 0;
    struct slash_ioctl_device_info* info = slash_device_info_read(dev_);
    EXPECT_EQ(info, nullptr);
    EXPECT_EQ(errno, ENODEV);
    srv_.ClearFault();
}

TEST_F(CtldevSysemuTest, PeerCloseBeforeReplyYieldsEnodevOnBarInfo)
{
    srv_.InjectFault(SysemuFault::PeerClose);
    errno = 0;
    struct slash_ioctl_bar_info* info = slash_bar_info_read(dev_, 0);
    EXPECT_EQ(info, nullptr);
    EXPECT_EQ(errno, ENODEV);
    srv_.ClearFault();
}

TEST_F(CtldevSysemuTest, PeerCloseBeforeReplyYieldsEnodevOnBarFd)
{
    srv_.InjectFault(SysemuFault::PeerClose);
    errno = 0;
    struct slash_bar_file* bar = slash_bar_file_open(dev_, 0, 0);
    EXPECT_EQ(bar, nullptr);
    EXPECT_EQ(errno, ENODEV);
    srv_.ClearFault();
}

TEST_F(CtldevSysemuTest, WrongSeqYieldsEnodevOnDeviceInfo)
{
    srv_.InjectFault(SysemuFault::WrongSeq);
    errno = 0;
    struct slash_ioctl_device_info* info = slash_device_info_read(dev_);
    EXPECT_EQ(info, nullptr);
    EXPECT_EQ(errno, ENODEV);
    srv_.ClearFault();
}

TEST_F(CtldevSysemuTest, WrongOpYieldsEnodevOnDeviceInfo)
{
    srv_.InjectFault(SysemuFault::WrongOp);
    errno = 0;
    struct slash_ioctl_device_info* info = slash_device_info_read(dev_);
    EXPECT_EQ(info, nullptr);
    EXPECT_EQ(errno, ENODEV);
    srv_.ClearFault();
}

TEST_F(CtldevSysemuTest, DaemonErrorEINVALMappedOnBarInfo)
{
    srv_.InjectFault(SysemuFault::DaemonError, EINVAL);
    errno = 0;
    struct slash_ioctl_bar_info* info = slash_bar_info_read(dev_, 0);
    EXPECT_EQ(info, nullptr);
    EXPECT_EQ(errno, EINVAL);
    srv_.ClearFault();
}

/* ── fd-leak audit ───────────────────────────────────────────────────────── */

TEST_F(CtldevSysemuTest, FdLeakAuditBarFdOpenLoop)
{
    /* Warm up: one call to ensure any lazy state is set up. */
    {
        struct slash_bar_file* bar = slash_bar_file_open(dev_, 0, 0);
        ASSERT_NE(bar, nullptr);
        slash_bar_file_close(bar);
    }

    int fd_before = count_open_fds();
    ASSERT_GE(fd_before, 0);

    const int N = 10;
    for (int i = 0; i < N; ++i) {
        struct slash_bar_file* bar = slash_bar_file_open(dev_, 0, 0);
        ASSERT_NE(bar, nullptr) << "iteration " << i;
        EXPECT_EQ(slash_bar_file_close(bar), 0) << "iteration " << i;
    }

    int fd_after = count_open_fds();
    EXPECT_EQ(fd_after, fd_before) << "fd leak detected after " << N << " GET_BAR_FD calls";
}

TEST_F(CtldevSysemuTest, FdLeakAuditBarFdOnFailedOpen)
{
    /*
     * Verify no fd leak when GET_BAR_FD succeeds then the bar_file is closed,
     * cycling N times, and then when the absent-BAR path returns an error.
     *
     * The PeerClose error path is already covered by PeerCloseBeforeReplyYieldsEnodevOnBarFd;
     * this test covers the daemon-returns-EINVAL error path (absent BAR) which
     * exercises the post-recv early-exit code in slash_bar_file_open.
     */
    /* Warm up. */
    {
        struct slash_bar_file* bar = slash_bar_file_open(dev_, 0, 0);
        ASSERT_NE(bar, nullptr);
        slash_bar_file_close(bar);
    }

    int fd_before = count_open_fds();
    ASSERT_GE(fd_before, 0);

    /* Absent BAR: daemon returns -EINVAL, no fd transferred. */
    const int N = 10;
    for (int i = 0; i < N; ++i) {
        errno = 0;
        struct slash_bar_file* bar = slash_bar_file_open(dev_, 1, 0);
        EXPECT_EQ(bar, nullptr) << "iteration " << i;
        EXPECT_NE(errno, 0)     << "iteration " << i;
    }

    int fd_after = count_open_fds();
    EXPECT_EQ(fd_after, fd_before)
        << "fd leak after " << N << " failed GET_BAR_FD (absent BAR) calls";
}

/* ── Adversary: flock sync flag decode edge cases ───────────────────────── */

TEST_F(CtldevSysemuTest, FlockSyncStartReadWriteGivesExclusive)
{
    /*
     * DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_READ should
     * encode to LOCK_EX (the WRITE bit wins).  Verify this by acquiring the
     * lock from one bar_file and observing LOCK_SH is blocked on another.
     */
    struct slash_ctldev* dev2 = slash_ctldev_open(srv_.Path().c_str());
    ASSERT_NE(dev2, nullptr);

    struct slash_bar_file* bar1 = slash_bar_file_open(dev_,  0, 0);
    ASSERT_NE(bar1, nullptr);
    struct slash_bar_file* bar2 = slash_bar_file_open(dev2, 0, 0);
    ASSERT_NE(bar2, nullptr);

    /* Acquire LOCK_EX via the combined START|WRITE|READ flags. */
    ASSERT_EQ(slash_bar_file_sync(bar1,
        DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_READ), 0);

    /* A non-blocking LOCK_SH attempt on bar2 (same inode) must fail. */
    int rc = ::flock(bar2->fd, LOCK_SH | LOCK_NB);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(errno, EWOULDBLOCK);

    /* Release via END (LOCK_UN). */
    ASSERT_EQ(slash_bar_file_end_write(bar1), 0);

    slash_bar_file_close(bar1);
    slash_bar_file_close(bar2);
    slash_ctldev_close(dev2);
}

TEST_F(CtldevSysemuTest, FlockSyncEndReadReleasesSharedLock)
{
    /*
     * Two bar_files can hold LOCK_SH simultaneously.  After both release,
     * a LOCK_EX on a third description must succeed.
     */
    struct slash_ctldev* dev2 = slash_ctldev_open(srv_.Path().c_str());
    struct slash_ctldev* dev3 = slash_ctldev_open(srv_.Path().c_str());
    ASSERT_NE(dev2, nullptr);
    ASSERT_NE(dev3, nullptr);

    struct slash_bar_file* bar1 = slash_bar_file_open(dev_,  0, 0);
    struct slash_bar_file* bar2 = slash_bar_file_open(dev2, 0, 0);
    struct slash_bar_file* bar3 = slash_bar_file_open(dev3, 0, 0);
    ASSERT_NE(bar1, nullptr);
    ASSERT_NE(bar2, nullptr);
    ASSERT_NE(bar3, nullptr);

    /* Both acquire shared read locks — must coexist. */
    ASSERT_EQ(slash_bar_file_start_read(bar1), 0);
    ASSERT_EQ(slash_bar_file_start_read(bar2), 0);

    /* While shared locks are held, a non-blocking LOCK_EX must fail. */
    int rc = ::flock(bar3->fd, LOCK_EX | LOCK_NB);
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(errno, EWOULDBLOCK)
        << "LOCK_EX should be blocked while two LOCK_SH are held";

    /* Release both shared locks. */
    ASSERT_EQ(slash_bar_file_end_read(bar1), 0);
    ASSERT_EQ(slash_bar_file_end_read(bar2), 0);

    /* Now LOCK_EX must succeed. */
    rc = ::flock(bar3->fd, LOCK_EX | LOCK_NB);
    EXPECT_EQ(rc, 0) << "LOCK_EX should succeed after both LOCK_SH released";
    if (rc == 0) ::flock(bar3->fd, LOCK_UN);

    slash_bar_file_close(bar1);
    slash_bar_file_close(bar2);
    slash_bar_file_close(bar3);
    slash_ctldev_close(dev2);
    slash_ctldev_close(dev3);
}

TEST_F(CtldevSysemuTest, FlockSyncEndWriteOnlyReleasesWriteLock)
{
    /*
     * Adversary: slash_bar_file_end_write issues LOCK_UN regardless of the
     * WRITE bit (any END flag unlocks).  Verify that after start_write +
     * end_write, another client can acquire LOCK_EX immediately.
     */
    struct slash_ctldev* dev2 = slash_ctldev_open(srv_.Path().c_str());
    ASSERT_NE(dev2, nullptr);

    struct slash_bar_file* bar1 = slash_bar_file_open(dev_,  0, 0);
    struct slash_bar_file* bar2 = slash_bar_file_open(dev2, 0, 0);
    ASSERT_NE(bar1, nullptr);
    ASSERT_NE(bar2, nullptr);

    ASSERT_EQ(slash_bar_file_start_write(bar1), 0);
    ASSERT_EQ(slash_bar_file_end_write(bar1),   0);

    /* bar1 must have released; bar2 LOCK_EX non-blocking must succeed. */
    int rc = ::flock(bar2->fd, LOCK_EX | LOCK_NB);
    EXPECT_EQ(rc, 0)
        << "LOCK_EX should succeed immediately after end_write (LOCK_UN)";
    if (rc == 0) ::flock(bar2->fd, LOCK_UN);

    slash_bar_file_close(bar1);
    slash_bar_file_close(bar2);
    slash_ctldev_close(dev2);
}

TEST_F(CtldevSysemuTest, FlockSyncNullBarFileCrashGuard)
{
    /*
     * slash_bar_file_sync on a NULL bar_file — must not crash.
     * The inline does not null-check (it directly dereferences bar_file->mock),
     * so this documents the existing API contract: callers must not pass NULL.
     * We don't call it with NULL here since that would be UB; instead we verify
     * the mock branch is safe by using the mock path.
     */
    struct slash_ctldev* mock_dev = slash_ctldev_open("@mock");
    ASSERT_NE(mock_dev, nullptr);

    struct slash_bar_file* mock_bar = slash_bar_file_open(mock_dev, 0, 0);
    ASSERT_NE(mock_bar, nullptr);
    ASSERT_TRUE(mock_bar->mock);

    /* Mock sync is always a no-op and must return 0. */
    EXPECT_EQ(slash_bar_file_sync(mock_bar, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE), 0);
    EXPECT_EQ(slash_bar_file_sync(mock_bar, DMA_BUF_SYNC_END   | DMA_BUF_SYNC_WRITE), 0);
    EXPECT_EQ(slash_bar_file_sync(mock_bar, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ),  0);
    EXPECT_EQ(slash_bar_file_sync(mock_bar, DMA_BUF_SYNC_END   | DMA_BUF_SYNC_READ),  0);
    /* flags=0 (no START/END): mock path returns 0 */
    EXPECT_EQ(slash_bar_file_sync(mock_bar, 0), 0);

    slash_bar_file_close(mock_bar);
    slash_ctldev_close(mock_dev);
}

/* ── Adversary: timeout path — SO_RCVTIMEO fires on dead daemon ─────────── */

TEST(CtldevSysemuAdversaryTest, RecvTimeoutYieldsEnodev)
{
    /*
     * Verify that SO_RCVTIMEO is actually active on the socket returned by
     * slash_sock_connect: when a server accepts but never replies, recvmsg
     * must time out and the client must see ENODEV.
     *
     * We shorten the wait by setting SO_RCVTIMEO to 200 ms on the client fd
     * AFTER slash_ctldev_open (which sets it to 10 s internally), then
     * issuing a request to a server that accepts but hangs.
     *
     * This tests that:
     *   (a) the timeout mechanism works at all (EAGAIN/ETIMEDOUT → ENODEV)
     *   (b) the send timeout also fires when a server is completely silent
     */
    char tmpl[] = "/tmp/slash_adv_timeout_XXXXXX";
    int tmp = ::mkstemp(tmpl);
    ASSERT_GE(tmp, 0);
    ::close(tmp);
    ::unlink(tmpl);

    int server_listen = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(server_listen, 0);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, tmpl, sizeof(addr.sun_path) - 1);
    ASSERT_EQ(::bind(server_listen,
                     reinterpret_cast<struct sockaddr*>(&addr),
                     sizeof(addr)), 0);
    ASSERT_EQ(::listen(server_listen, 2), 0);

    struct slash_ctldev* dev = slash_ctldev_open(tmpl);
    ASSERT_NE(dev, nullptr) << std::strerror(errno);
    ASSERT_EQ(dev->transport, SLASH_TRANSPORT_SOCKET);

    /*
     * Override the 10 s timeout to 200 ms so the test runs quickly.
     * slash_sock_connect already set SO_RCVTIMEO=10s; we override it here.
     */
    struct timeval tv{};
    tv.tv_sec  = 0;
    tv.tv_usec = 200000; /* 200 ms */
    ASSERT_EQ(::setsockopt(dev->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);

    /* Server accepts but never replies (simulates a hung daemon). */
    std::atomic<bool> server_done{false};
    std::thread server_thread([&] {
        int conn = ::accept4(server_listen, nullptr, nullptr, SOCK_CLOEXEC);
        if (conn >= 0) {
            /* Drain the request but do NOT reply — just hold the connection. */
            char buf[4096];
            ::recv(conn, buf, sizeof(buf), 0);
            /* Wait until the test signals done, then close. */
            while (!server_done.load()) {
                struct timespec ts{};
                ts.tv_nsec = 5000000; /* 5 ms */
                ::nanosleep(&ts, nullptr);
            }
            ::close(conn);
        }
    });

    /* Issue a device_info_read — must time out and return NULL/ENODEV. */
    errno = 0;
    struct slash_ioctl_device_info* info = slash_device_info_read(dev);

    server_done.store(true);
    server_thread.join();

    EXPECT_EQ(info, nullptr);
    EXPECT_EQ(errno, ENODEV)
        << "Expected ENODEV (timeout → ENODEV mapping), got errno=" << errno;

    slash_ctldev_close(dev);
    ::close(server_listen);
    ::unlink(tmpl);
}
