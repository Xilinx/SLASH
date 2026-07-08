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
 * @file libslash_e2e_test.cpp
 *
 * Opt-in end-to-end tests that drive libslash against a REAL, already-running
 * slash_sysemu daemon.  The suite is HERMETIC-SKIP by default: every test
 * calls GTEST_SKIP() when the required environment variables are unset, so
 * "ctest" stays green with no daemon present.
 *
 * THIS SUITE NEVER SPAWNS THE DAEMON ITSELF.  Before running with paths set,
 * the user must build, install, and launch slash_sysemu independently.
 *
 * Environment variables (all optional; absence skips related tests):
 *
 *   SLASH_E2E_CTL     Path to the daemon's slash_ctl<n> socket.
 *                     Typical: /run/slash_sysemu/slash_ctl0
 *
 *   SLASH_E2E_QDMA    Path to the daemon's slash_qdma_ctl<n> socket.
 *                     Typical: /run/slash_sysemu/slash_qdma_ctl0
 *
 *   SLASH_E2E_HOTPLUG Path to the daemon's slash_hotplug socket.
 *                     Typical: /run/slash_sysemu/slash_hotplug
 *
 * Coverage (when paths are set):
 *   - ctldev: device_info (BDF non-empty), bar_info (at least one usable BAR),
 *     GET_BAR_FD mmap + flock-bracketed write/read round-trip
 *   - qdma: INFO (bdf non-empty, qsets_max > 0), qpair add/start/get_fd,
 *     BUF_CREATE memfd, H2C→C2H round-trip at a real HBM address (BDF-derived,
 *     uses address 0 which the daemon models as general HBM/DDR space)
 *   - hotplug: RESCAN succeeds
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include <slash/ctldev.h>
#include <slash/hotplug.h>
#include <slash/qdma.h>
#include <sys/mman.h>
#include <unistd.h>
}

/* ── Skip helpers ─────────────────────────────────────────────────────────── */

static const char *e2e_ctl_path()
{
    return std::getenv("SLASH_E2E_CTL");
}

static const char *e2e_qdma_path()
{
    return std::getenv("SLASH_E2E_QDMA");
}

static const char *e2e_hotplug_path()
{
    return std::getenv("SLASH_E2E_HOTPLUG");
}

#define SKIP_IF_NO_CTL()                                                         \
    do {                                                                          \
        if (!e2e_ctl_path()) {                                                    \
            GTEST_SKIP() << "SLASH_E2E_CTL not set; skipping E2E ctldev tests";  \
        }                                                                         \
    } while (0)

#define SKIP_IF_NO_QDMA()                                                         \
    do {                                                                           \
        if (!e2e_qdma_path()) {                                                    \
            GTEST_SKIP() << "SLASH_E2E_QDMA not set; skipping E2E qdma tests";    \
        }                                                                          \
    } while (0)

#define SKIP_IF_NO_HOTPLUG()                                                         \
    do {                                                                              \
        if (!e2e_hotplug_path()) {                                                    \
            GTEST_SKIP() << "SLASH_E2E_HOTPLUG not set; skipping E2E hotplug tests"; \
        }                                                                             \
    } while (0)

/* ── ctldev E2E tests ─────────────────────────────────────────────────────── */

class E2ECtldevTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        SKIP_IF_NO_CTL();
        dev_ = slash_ctldev_open(e2e_ctl_path());
        ASSERT_NE(dev_, nullptr)
            << "slash_ctldev_open(" << e2e_ctl_path()
            << ") failed: " << strerror(errno);
    }

    void TearDown() override
    {
        if (dev_) {
            slash_ctldev_close(dev_);
            dev_ = nullptr;
        }
    }

    struct slash_ctldev *dev_ = nullptr;
};

TEST_F(E2ECtldevTest, DeviceInfoBdfNonEmpty)
{
    struct slash_ioctl_device_info *info = slash_device_info_read(dev_);
    ASSERT_NE(info, nullptr);
    EXPECT_GT(strnlen(info->bdf, sizeof(info->bdf)), 0u)
        << "daemon returned an empty BDF string";
    slash_device_info_free(info);
}

TEST_F(E2ECtldevTest, BarInfoAtLeastOneUsable)
{
    bool found_usable = false;
    for (int bar = 0; bar < 6; ++bar) {
        struct slash_ioctl_bar_info *bi = slash_bar_info_read(dev_, bar);
        if (!bi) continue;
        if (bi->usable) {
            found_usable = true;
        }
        slash_bar_info_free(bi);
        if (found_usable) break;
    }
    EXPECT_TRUE(found_usable) << "daemon reported no usable BARs";
}

TEST_F(E2ECtldevTest, GetBarFdMmapAndFlockRoundTrip)
{
    /*
     * Find the first usable BAR, open it, mmap it, then perform a
     * flock-bracketed write/read round-trip to verify the memfd is live.
     */
    int usable_bar = -1;
    size_t usable_len = 0;
    for (int bar = 0; bar < 6; ++bar) {
        struct slash_ioctl_bar_info *bi = slash_bar_info_read(dev_, bar);
        if (!bi) continue;
        if (bi->usable && bi->size >= 4) {
            usable_bar = bar;
            usable_len = static_cast<size_t>(bi->size);
        }
        slash_bar_info_free(bi);
        if (usable_bar >= 0) break;
    }
    ASSERT_GE(usable_bar, 0) << "no usable BAR — daemon not ready?";

    struct slash_bar_file *bf = slash_bar_file_open(dev_, usable_bar, 0);
    ASSERT_NE(bf, nullptr) << "slash_bar_file_open(bar=" << usable_bar << ") failed";
    ASSERT_NE(bf->map, nullptr);
    ASSERT_NE(bf->map, MAP_FAILED);
    ASSERT_GE(bf->len, static_cast<size_t>(4));

    /* Write a sentinel under exclusive lock, read it back under shared lock. */
    const uint32_t kSentinel = 0xA55A5AA5u;

    ASSERT_EQ(slash_bar_file_start_write(bf), 0);
    std::memcpy(bf->map, &kSentinel, sizeof(kSentinel));
    ASSERT_EQ(slash_bar_file_end_write(bf), 0);

    uint32_t readback = 0;
    ASSERT_EQ(slash_bar_file_start_read(bf), 0);
    std::memcpy(&readback, bf->map, sizeof(readback));
    ASSERT_EQ(slash_bar_file_end_read(bf), 0);

    EXPECT_EQ(readback, kSentinel);

    slash_bar_file_close(bf);
    (void)usable_len;
}

/* ── qdma E2E tests ───────────────────────────────────────────────────────── */

class E2EQdmaTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        SKIP_IF_NO_QDMA();
        qdma_ = slash_qdma_open(e2e_qdma_path());
        ASSERT_NE(qdma_, nullptr)
            << "slash_qdma_open(" << e2e_qdma_path()
            << ") failed: " << strerror(errno);
    }

    void TearDown() override
    {
        if (qdma_) {
            slash_qdma_close(qdma_);
            qdma_ = nullptr;
        }
    }

    struct slash_qdma *qdma_ = nullptr;
};

TEST_F(E2EQdmaTest, InfoBdfNonEmptyAndQsetsPositive)
{
    struct slash_qdma_info info{};
    ASSERT_EQ(slash_qdma_info_read(qdma_, &info), 0);
    EXPECT_GT(strnlen(info.bdf, sizeof(info.bdf)), 0u)
        << "daemon returned an empty QDMA BDF string";
    EXPECT_GT(info.qsets_max, 0u);
}

TEST_F(E2EQdmaTest, QpairAddStartGetFd)
{
    struct slash_qdma_qpair_add req{};
    req.size         = sizeof(req);
    req.mode         = 0;    /* MM mode */
    req.dir_mask     = 0x3;  /* H2C | C2H */
    req.h2c_ring_sz  = 4;
    req.c2h_ring_sz  = 4;
    req.cmpt_ring_sz = 4;

    ASSERT_EQ(slash_qdma_qpair_add(qdma_, &req), 0);
    uint32_t qid = req.qid;

    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);

    int xfer_fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    ASSERT_GE(xfer_fd, 0) << "QPAIR_GET_FD failed: " << strerror(errno);

    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0);
    EXPECT_EQ(slash_qdma_qpair_del(qdma_, qid), 0);
    ::close(xfer_fd);
}

TEST_F(E2EQdmaTest, BufCreateAndH2cC2hRoundTrip)
{
    /*
     * Full consumer flow:
     *   add → start → get_fd → buf_create → H2C → C2H → compare
     *
     * The dev_addr 0x0 maps to the beginning of the daemon's emulated HBM/DDR
     * address space.  We read INFO to confirm the daemon is live, but do NOT
     * hard-code a device BDF into the transfer — the address space is always
     * anchored at 0 in the daemon model.
     */
    struct slash_qdma_info info{};
    ASSERT_EQ(slash_qdma_info_read(qdma_, &info), 0);

    struct slash_qdma_qpair_add req{};
    req.size         = sizeof(req);
    req.mode         = 0;
    req.dir_mask     = 0x3;
    req.h2c_ring_sz  = 4;
    req.c2h_ring_sz  = 4;
    req.cmpt_ring_sz = 4;
    ASSERT_EQ(slash_qdma_qpair_add(qdma_, &req), 0);
    uint32_t qid = req.qid;
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);

    int xfer_fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    ASSERT_GE(xfer_fd, 0);

    /* Create a 4 KiB DMA buffer. */
    const size_t kBufLen = 4096;
    struct slash_qdma_buffer buf{};
    ASSERT_EQ(slash_qdma_qpair_buffer_create(xfer_fd, kBufLen, &buf), 0);
    ASSERT_NE(buf.addr, nullptr);
    ASSERT_NE(buf.addr, MAP_FAILED);

    /* Fill the buffer with a test pattern. */
    const uint64_t kPattern = 0xFEEDFACEDEADC0DEULL;
    std::memcpy(buf.addr, &kPattern, sizeof(kPattern));

    /* H2C: write to dev_addr 0 (start of emulated HBM). */
    const uint64_t kDevAddr = 0;
    ssize_t sent = slash_qdma_qpair_transfer(xfer_fd, buf.fd,
                                              0 /* buf_offset */,
                                              kDevAddr,
                                              sizeof(kPattern),
                                              SLASH_QDMA_XFER_H2C);
    ASSERT_EQ(sent, static_cast<ssize_t>(sizeof(kPattern)))
        << "H2C transfer failed: " << strerror(errno);

    /* Clear the host buffer so we can verify C2H fills it. */
    std::memset(buf.addr, 0, sizeof(kPattern));

    /* C2H: read back from the same address. */
    ssize_t recvd = slash_qdma_qpair_transfer(xfer_fd, buf.fd,
                                               0,
                                               kDevAddr,
                                               sizeof(kPattern),
                                               SLASH_QDMA_XFER_C2H);
    ASSERT_EQ(recvd, static_cast<ssize_t>(sizeof(kPattern)))
        << "C2H transfer failed: " << strerror(errno);

    uint64_t got = 0;
    std::memcpy(&got, buf.addr, sizeof(got));
    EXPECT_EQ(got, kPattern) << "H2C→C2H round-trip: data mismatch";

    EXPECT_EQ(slash_qdma_buffer_destroy(&buf), 0);
    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0);
    EXPECT_EQ(slash_qdma_qpair_del(qdma_, qid), 0);
    ::close(xfer_fd);
}

/* ── hotplug E2E tests ────────────────────────────────────────────────────── */

class E2EHotplugTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        SKIP_IF_NO_HOTPLUG();
        hp_ = slash_hotplug_open(e2e_hotplug_path());
        ASSERT_NE(hp_, nullptr)
            << "slash_hotplug_open(" << e2e_hotplug_path()
            << ") failed: " << strerror(errno);
    }

    void TearDown() override
    {
        if (hp_) {
            slash_hotplug_close(hp_);
            hp_ = nullptr;
        }
    }

    struct slash_hotplug *hp_ = nullptr;
};

TEST_F(E2EHotplugTest, RescanSucceeds)
{
    /*
     * RESCAN is safe to issue at any time — the daemon re-discovers already-
     * tracked devices idempotently and does not reset hardware.
     */
    EXPECT_EQ(slash_hotplug_rescan(hp_), 0)
        << "slash_hotplug_rescan failed: " << strerror(errno);
}
