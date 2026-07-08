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
 * @file hotplug_sysemu_test.cpp
 *
 * GTest suite for the hotplug socket-transport path (SLASH_TRANSPORT_SOCKET).
 * Uses SysemuTestServer as an in-process daemon substitute.
 *
 * Covered:
 *   - Transport detection: socket path sets SLASH_TRANSPORT_SOCKET; ioctl path
 *     gives SLASH_TRANSPORT_IOCTL (existing /dev tests remain)
 *   - RESCAN no-arg round-trip: succeeds with return 0
 *   - REMOVE / TOGGLE_SBR / HOTPLUG: BDF echoed by server, recorded in last_hotplug_bdf
 *   - NULL bdf and empty bdf: both accepted (sent to daemon with empty bdf field)
 *   - Oversized bdf: EINVAL returned by libslash; no request sent to daemon
 *   - Daemon error return_value: mapped to -1 + correct errno
 *   - ENODEV on PeerClose, TruncatedReply, WrongSeq, WrongOp
 *   - RESCAN no-device: server returns -ENODEV, client maps to errno=ENODEV
 *   - Thread-clean: server Stop() joins all threads without crash
 */

#define _GNU_SOURCE

#include <gtest/gtest.h>

#include "sysemu_test_server.h"

#include <cerrno>
#include <cstring>
#include <string>

extern "C" {
#include <slash/ctldev.h>
#include <slash/hotplug.h>
}

/* ── Test fixture ─────────────────────────────────────────────────────────── */

class HotplugSysemuTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(srv_.Start()) << "SysemuTestServer::Start() failed";
        hp_ = slash_hotplug_open(srv_.Path().c_str());
        ASSERT_NE(hp_, nullptr) << "slash_hotplug_open failed: " << strerror(errno);
    }

    void TearDown() override
    {
        if (hp_) {
            slash_hotplug_close(hp_);
            hp_ = nullptr;
        }
        srv_.Stop();
    }

    SysemuTestServer     srv_;
    struct slash_hotplug *hp_ = nullptr;
};

/* ── Transport detection ──────────────────────────────────────────────────── */

TEST_F(HotplugSysemuTest, OpenSocketPathSetsSocketTransport)
{
    EXPECT_EQ(hp_->transport, SLASH_TRANSPORT_SOCKET);
    EXPECT_GE(hp_->fd, 0);
}

/* ── RESCAN ───────────────────────────────────────────────────────────────── */

TEST_F(HotplugSysemuTest, RescanSucceeds)
{
    EXPECT_EQ(slash_hotplug_rescan(hp_), 0);
}

TEST_F(HotplugSysemuTest, RescanNoDeviceReturnsEnodev)
{
    srv_.hotplug_no_device = true;
    int ret = slash_hotplug_rescan(hp_);
    srv_.hotplug_no_device = false;
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, ENODEV);
}

/* ── REMOVE ───────────────────────────────────────────────────────────────── */

TEST_F(HotplugSysemuTest, RemoveRecordsBdf)
{
    const char kBdf[] = "0000:03:00.2";
    ASSERT_EQ(slash_hotplug_remove(hp_, kBdf), 0);
    EXPECT_EQ(srv_.last_hotplug_bdf, kBdf);
}

TEST_F(HotplugSysemuTest, RemoveNullBdfSendsToDaemon)
{
    /* NULL bdf is allowed — empty bdf field sent; server records empty string. */
    ASSERT_EQ(slash_hotplug_remove(hp_, nullptr), 0);
    EXPECT_EQ(srv_.last_hotplug_bdf, "");
}

TEST_F(HotplugSysemuTest, RemoveEmptyBdfSendsToDaemon)
{
    ASSERT_EQ(slash_hotplug_remove(hp_, ""), 0);
    EXPECT_EQ(srv_.last_hotplug_bdf, "");
}

TEST_F(HotplugSysemuTest, RemoveOversizedBdfReturnsEinval)
{
    /* Build a bdf string that is >= SLASH_HOTPLUG_BDF_LEN bytes. */
    std::string too_long(SLASH_HOTPLUG_BDF_LEN, 'x');
    errno = 0;
    int ret = slash_hotplug_remove(hp_, too_long.c_str());
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, EINVAL);
    /* No request should have been dispatched. */
    EXPECT_FALSE(srv_.LastRequest().has_value());
}

TEST_F(HotplugSysemuTest, RemoveDaemonErrorMapsErrno)
{
    srv_.hotplug_error_code = -EPERM;
    int ret = slash_hotplug_remove(hp_, "0000:03:00.2");
    srv_.hotplug_error_code = 0;
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, EPERM);
}

/* ── TOGGLE_SBR ───────────────────────────────────────────────────────────── */

TEST_F(HotplugSysemuTest, ToggleSbrRecordsBdf)
{
    const char kBdf[] = "0000:03:00.1";
    ASSERT_EQ(slash_hotplug_toggle_sbr(hp_, kBdf), 0);
    EXPECT_EQ(srv_.last_hotplug_bdf, kBdf);
}

TEST_F(HotplugSysemuTest, ToggleSbrNullBdfSendsToDaemon)
{
    ASSERT_EQ(slash_hotplug_toggle_sbr(hp_, nullptr), 0);
    EXPECT_EQ(srv_.last_hotplug_bdf, "");
}

TEST_F(HotplugSysemuTest, ToggleSbrOversizedBdfReturnsEinval)
{
    std::string too_long(SLASH_HOTPLUG_BDF_LEN, 'y');
    errno = 0;
    int ret = slash_hotplug_toggle_sbr(hp_, too_long.c_str());
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(HotplugSysemuTest, ToggleSbrDaemonErrorMapsErrno)
{
    srv_.hotplug_error_code = -EBUSY;
    int ret = slash_hotplug_toggle_sbr(hp_, "0000:03:00.1");
    srv_.hotplug_error_code = 0;
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, EBUSY);
}

/* ── HOTPLUG ──────────────────────────────────────────────────────────────── */

TEST_F(HotplugSysemuTest, HotplugRecordsBdf)
{
    const char kBdf[] = "0000:05:00.2";
    ASSERT_EQ(slash_hotplug_hotplug(hp_, kBdf), 0);
    EXPECT_EQ(srv_.last_hotplug_bdf, kBdf);
}

TEST_F(HotplugSysemuTest, HotplugNullBdfSendsToDaemon)
{
    ASSERT_EQ(slash_hotplug_hotplug(hp_, nullptr), 0);
    EXPECT_EQ(srv_.last_hotplug_bdf, "");
}

TEST_F(HotplugSysemuTest, HotplugOversizedBdfReturnsEinval)
{
    std::string too_long(SLASH_HOTPLUG_BDF_LEN, 'z');
    errno = 0;
    int ret = slash_hotplug_hotplug(hp_, too_long.c_str());
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(HotplugSysemuTest, HotplugDaemonErrorMapsErrno)
{
    srv_.hotplug_error_code = -EIO;
    int ret = slash_hotplug_hotplug(hp_, "0000:05:00.2");
    srv_.hotplug_error_code = 0;
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, EIO);
}

/* ── ENODEV fault paths (PeerClose / TruncatedReply / WrongSeq / WrongOp) ── */

TEST_F(HotplugSysemuTest, RescanPeerCloseReturnsEnodev)
{
    srv_.InjectFault(SysemuFault::PeerClose);
    int ret = slash_hotplug_rescan(hp_);
    srv_.ClearFault();
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(HotplugSysemuTest, RescanTruncatedReplyReturnsEnodev)
{
    srv_.InjectFault(SysemuFault::TruncatedReply);
    int ret = slash_hotplug_rescan(hp_);
    srv_.ClearFault();
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(HotplugSysemuTest, RescanWrongSeqReturnsEnodev)
{
    srv_.InjectFault(SysemuFault::WrongSeq);
    int ret = slash_hotplug_rescan(hp_);
    srv_.ClearFault();
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(HotplugSysemuTest, RescanWrongOpReturnsEnodev)
{
    srv_.InjectFault(SysemuFault::WrongOp);
    int ret = slash_hotplug_rescan(hp_);
    srv_.ClearFault();
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(HotplugSysemuTest, RemovePeerCloseReturnsEnodev)
{
    srv_.InjectFault(SysemuFault::PeerClose);
    int ret = slash_hotplug_remove(hp_, "0000:03:00.2");
    srv_.ClearFault();
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(HotplugSysemuTest, RemoveWrongSeqReturnsEnodev)
{
    srv_.InjectFault(SysemuFault::WrongSeq);
    int ret = slash_hotplug_remove(hp_, "0000:03:00.2");
    srv_.ClearFault();
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(HotplugSysemuTest, RemoveWrongOpReturnsEnodev)
{
    srv_.InjectFault(SysemuFault::WrongOp);
    int ret = slash_hotplug_remove(hp_, "0000:03:00.2");
    srv_.ClearFault();
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, ENODEV);
}

/* ── Thread-clean and lifecycle ───────────────────────────────────────────── */

TEST_F(HotplugSysemuTest, CloseAndReopenSucceeds)
{
    /* First handle: issue a rescan, then close. */
    ASSERT_EQ(slash_hotplug_rescan(hp_), 0);
    ASSERT_EQ(slash_hotplug_close(hp_), 0);
    hp_ = nullptr;

    /* Re-open to verify server is still healthy. */
    hp_ = slash_hotplug_open(srv_.Path().c_str());
    ASSERT_NE(hp_, nullptr);
    EXPECT_EQ(slash_hotplug_rescan(hp_), 0);
}

TEST_F(HotplugSysemuTest, MultipleOpsSequential)
{
    const char kBdf[] = "0000:07:00.2";
    ASSERT_EQ(slash_hotplug_remove(hp_,      kBdf), 0);
    ASSERT_EQ(slash_hotplug_toggle_sbr(hp_,  kBdf), 0);
    ASSERT_EQ(slash_hotplug_rescan(hp_),           0);
    ASSERT_EQ(slash_hotplug_hotplug(hp_,     kBdf), 0);
    EXPECT_EQ(srv_.last_hotplug_bdf, kBdf);
}
