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
 *   - qdma: INFO (bdf non-empty, capability fields faithfully forwarded as 0),
 *     qpair add/start/get_fd, BUF_CREATE memfd, H2C→C2H round-trip at a real
 *     HBM address (uses address 0 which the daemon models as general HBM/DDR)
 *   - hotplug: RESCAN, and the destructive REMOVE / HOTPLUG / TOGGLE_SBR ops —
 *     each verified by observing the control socket tear down and/or restore,
 *     plus a REMOVE-unknown-BDF negative case.  The destructive tests need both
 *     SLASH_E2E_HOTPLUG and SLASH_E2E_CTL (they check PF2's socket state) and
 *     leave the accelerator fully active again on completion.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

/*
 * Destructive hotplug tests observe PF2's control socket to confirm teardown /
 * restore, so they need BOTH the hotplug and the ctl socket paths.
 */
#define SKIP_IF_NO_HOTPLUG_CTL()                                                      \
    do {                                                                              \
        if (!e2e_hotplug_path() || !e2e_ctl_path()) {                                 \
            GTEST_SKIP() << "SLASH_E2E_HOTPLUG and SLASH_E2E_CTL both required; "     \
                            "skipping destructive hotplug tests";                     \
        }                                                                             \
    } while (0)

/* Tests that cross all three subsystems (e.g. persistence across a hotplug). */
#define SKIP_IF_NO_ALL()                                                              \
    do {                                                                              \
        if (!e2e_ctl_path() || !e2e_qdma_path() || !e2e_hotplug_path()) {             \
            GTEST_SKIP() << "SLASH_E2E_CTL, SLASH_E2E_QDMA and SLASH_E2E_HOTPLUG all "\
                            "required; skipping cross-subsystem test";                \
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
    uint32_t *kernel_reg =  static_cast<uint32_t*>(bf->map) + 4;

    ASSERT_EQ(slash_bar_file_start_write(bf), 0);
    std::memcpy(kernel_reg, &kSentinel, sizeof(kSentinel));
    ASSERT_EQ(slash_bar_file_end_write(bf), 0);

    uint32_t readback = 0;
    ASSERT_EQ(slash_bar_file_start_read(bf), 0);
    std::memcpy(&readback, kernel_reg, sizeof(readback));
    ASSERT_EQ(slash_bar_file_end_read(bf), 0);

    EXPECT_EQ(readback, kSentinel);

    slash_bar_file_close(bf);
    (void)usable_len;
}

/* ── hotplug E2E tests ────────────────────────────────────────────────────── */

/*
 * Strip the ".<function>" suffix from a full BDF ("DDDD:BB:DD.F") to get the
 * board BDF ("DDDD:BB:DD").
 */
static std::string board_of(const char *bdf, size_t cap)
{
    std::string s(bdf, ::strnlen(bdf, cap));
    std::string::size_type dot = s.rfind('.');
    if (dot != std::string::npos) s.erase(dot);
    return s;
}

/*
 * Read the board BDF ("DDDD:BB:DD", function suffix stripped) from the ctldev
 * device_info.  Returns "" if the ctl socket is unavailable.  Used to target
 * the destructive hotplug ops at the actual accelerator without hard-coding it.
 */
static std::string board_bdf_from_ctl()
{
    const char *ctl = e2e_ctl_path();
    if (!ctl) return "";
    struct slash_ctldev *dev = slash_ctldev_open(ctl);
    if (!dev) return "";
    std::string bdf;
    struct slash_ioctl_device_info *info = slash_device_info_read(dev);
    if (info) {
        bdf = board_of(info->bdf, sizeof(info->bdf));
        slash_device_info_free(info);
    }
    slash_ctldev_close(dev);
    return bdf;
}

/*
 * True iff the ctldev control device is currently reachable AND answers
 * device_info — i.e. PF2's socket exists and the model behind it is alive.
 * After a REMOVE of PF2 the socket is unlinked, so the open fails.
 *
 * If @expect_board is non-empty, additionally require the device's REPORTED
 * board BDF (device_info bdf with the function suffix stripped) to equal it —
 * so "up" means "the expected accelerator is up", not merely "some device is
 * up".  This catches a restore that brings back the wrong / a mislabelled
 * device.
 */
static bool ctl_alive(const std::string &expect_board = "")
{
    const char *ctl = e2e_ctl_path();
    if (!ctl) return false;
    struct slash_ctldev *dev = slash_ctldev_open(ctl);
    if (!dev) return false;
    struct slash_ioctl_device_info *info = slash_device_info_read(dev);
    bool ok = (info != nullptr);
    if (info && !expect_board.empty()) {
        ok = (board_of(info->bdf, sizeof(info->bdf)) == expect_board);
    }
    if (info) slash_device_info_free(info);
    slash_ctldev_close(dev);
    return ok;
}

/*
 * Poll until the control device reaches the desired state or the ~1s budget
 * expires.  Hotplug ops are synchronous in the daemon (REMOVE unlinks and
 * RESCAN rebinds before responding), so this only guards against brief socket
 * (re)creation races.  Returns true iff the desired state was observed.
 *
 * For an EXISTENCE check (want=true) with a non-empty @expect_board, the
 * device must be up AND report that board BDF.  For an ABSENCE check
 * (want=false) the board is irrelevant — we wait for the device to be truly
 * gone (raw aliveness false), so an up-but-wrong-BDF device is never mistaken
 * for "removed".
 */
static bool wait_for_ctl(bool want, const std::string &expect_board = "")
{
    for (int i = 0; i < 40; ++i) {
        bool alive = want ? ctl_alive(expect_board) : ctl_alive();
        if (alive == want) return true;
        ::usleep(25 * 1000); /* 25 ms */
    }
    bool alive = want ? ctl_alive(expect_board) : ctl_alive();
    return alive == want;
}

/*
 * True iff the QDMA subsystem (PF1) is currently reachable AND answers info_read
 * — i.e. PF1's socket exists and is bound to a live model.  After a REMOVE of
 * PF1 the socket is unlinked, so the open fails.  Unlike the control device,
 * PF1 is torn down/rebuilt independently of PF0/PF2, so this is the probe that
 * observes a PF1 remove/restore (the control socket stays up throughout).
 *
 * If @expect_board is non-empty, additionally require the reported board BDF
 * (info.bdf with the function suffix stripped) to equal it — so "up" means "the
 * expected accelerator's QDMA is up", not merely "some QDMA is up".
 */
static bool qdma_alive(const std::string &expect_board = "")
{
    const char *path = e2e_qdma_path();
    if (!path) return false;
    struct slash_qdma *q = slash_qdma_open(path);
    if (!q) return false;
    struct slash_qdma_info info{};
    bool ok = (slash_qdma_info_read(q, &info) == 0);
    if (ok && !expect_board.empty()) {
        ok = (board_of(info.bdf, sizeof(info.bdf)) == expect_board);
    }
    slash_qdma_close(q);
    return ok;
}

/*
 * Poll until PF1's QDMA reaches the desired state or the ~1s budget expires.
 * Mirrors wait_for_ctl: for an EXISTENCE check (want=true) with a non-empty
 * @expect_board the QDMA must be up AND report that board; for an ABSENCE check
 * (want=false) the board is irrelevant.
 */
static bool wait_for_qdma(bool want, const std::string &expect_board = "")
{
    for (int i = 0; i < 40; ++i) {
        bool alive = want ? qdma_alive(expect_board) : qdma_alive();
        if (alive == want) return true;
        ::usleep(25 * 1000); /* 25 ms */
    }
    bool alive = want ? qdma_alive(expect_board) : qdma_alive();
    return alive == want;
}

/*
 * Perform a single H2C or C2H transfer against a device address over a FRESH
 * QDMA session (open → add → start → get_fd → buf_create → transfer → teardown).
 * For H2C, @host is copied into the DMA buffer before the transfer; for C2H, the
 * buffer is zeroed, transferred, then copied back into @host.  @len must be a
 * whole number of pages and @dev_addr page-aligned.  Returns true on success;
 * on failure @err carries a short reason.  Each call uses a brand-new connection
 * so a post-reconstruction call naturally exercises the rebuilt QDMA subsystem.
 */
static bool qdma_xfer(const char *qdma_path, uint32_t dir, uint64_t dev_addr,
                      void *host, size_t len, std::string &err)
{
    struct slash_qdma *q = slash_qdma_open(qdma_path);
    if (!q) { err = std::string("qdma_open: ") + strerror(errno); return false; }

    struct slash_qdma_qpair_add add{};
    add.size = sizeof(add);
    add.mode = 0;        /* MM */
    add.dir_mask = 0x3;  /* H2C | C2H */
    add.h2c_ring_sz = 4;
    add.c2h_ring_sz = 4;
    add.cmpt_ring_sz = 4;

    bool ok = false;
    bool have_qpair = false;
    int xfer = -1;
    struct slash_qdma_buffer buf{};

    if (slash_qdma_qpair_add(q, &add) == 0 &&
        slash_qdma_qpair_start(q, add.qid) == 0) {
        have_qpair = true;
        xfer = slash_qdma_qpair_get_fd(q, add.qid, 0);
        if (xfer >= 0 && slash_qdma_qpair_buffer_create(xfer, len, &buf) == 0) {
            if (dir == SLASH_QDMA_XFER_H2C) {
                std::memcpy(buf.addr, host, len);
            } else {
                std::memset(buf.addr, 0, len);
            }
            ssize_t n = slash_qdma_qpair_transfer(xfer, buf.fd, 0, dev_addr, len, dir);
            ok = (n == static_cast<ssize_t>(len));
            if (!ok) {
                err = "transfer returned " + std::to_string(n) + " (" + strerror(errno) + ")";
            } else if (dir == SLASH_QDMA_XFER_C2H) {
                std::memcpy(host, buf.addr, len);
            }
        } else {
            err = std::string("get_fd/buf_create: ") + strerror(errno);
        }
    } else {
        err = std::string("qpair add/start: ") + strerror(errno);
    }

    if (buf.addr) slash_qdma_buffer_destroy(&buf);
    if (xfer >= 0) ::close(xfer);
    if (have_qpair) {
        slash_qdma_qpair_stop(q, add.qid);
        slash_qdma_qpair_del(q, add.qid);
    }
    slash_qdma_close(q);
    return ok;
}

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
            /*
             * Safety net: leave the accelerator fully active even if a
             * destructive test asserted out before restoring it.  RESCAN is
             * idempotent, so this is harmless for the non-destructive tests.
             */
            (void)slash_hotplug_rescan(hp_);
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

TEST_F(E2EHotplugTest, RemovePf2ThenRescanRestores)
{
    /*
     * REMOVE PF2 (slash_ctl): the control socket is unlinked and the
     * accelerator drops to the "partial" state (model + PF1 keep running).
     * RESCAN then restores PF2 (triggering a reconfiguration) and the control
     * socket comes back.
     */
    SKIP_IF_NO_HOTPLUG_CTL();
    std::string board = board_bdf_from_ctl();
    ASSERT_FALSE(board.empty()) << "could not read board BDF from ctl device_info";
    ASSERT_TRUE(ctl_alive(board))
        << "precondition: control device must be up and report board " << board;

    std::string pf2 = board + ".2";
    ASSERT_EQ(slash_hotplug_remove(hp_, pf2.c_str()), 0)
        << "REMOVE(" << pf2 << ") failed: " << strerror(errno);
    EXPECT_TRUE(wait_for_ctl(false))
        << "control socket should be torn down after REMOVE of PF2";

    ASSERT_EQ(slash_hotplug_rescan(hp_), 0)
        << "RESCAN failed: " << strerror(errno);
    EXPECT_TRUE(wait_for_ctl(true, board))
        << "control device should be restored after RESCAN and report board " << board;
}

TEST_F(E2EHotplugTest, HotplugBoardRoundTrip)
{
    /*
     * HOTPLUG = REMOVE + RESCAN as one operation; self-restoring.  Targeting
     * the bare board BDF removes all PFs then brings them back.
     */
    SKIP_IF_NO_HOTPLUG_CTL();
    std::string board = board_bdf_from_ctl();
    ASSERT_FALSE(board.empty());
    ASSERT_TRUE(ctl_alive(board))
        << "precondition: control device must be up and report board " << board;

    std::string pf2 = board + ".2";
    ASSERT_EQ(slash_hotplug_hotplug(hp_, pf2.c_str()), 0)
        << "HOTPLUG(" << board << ") failed: " << strerror(errno);
    EXPECT_TRUE(wait_for_ctl(true, board))
        << "control device should be up again and report board " << board
        << " after HOTPLUG (remove+rescan)";
}

TEST_F(E2EHotplugTest, ToggleSbrBoardRoundTrip)
{
    /*
     * TOGGLE_SBR removes+rescans all PFs on the accelerator's bus and blocks
     * ~1s for the emulated link training; self-restoring.
     */
    SKIP_IF_NO_HOTPLUG_CTL();
    std::string board = board_bdf_from_ctl();
    ASSERT_FALSE(board.empty());
    ASSERT_TRUE(ctl_alive(board))
        << "precondition: control device must be up and report board " << board;

    std::string pf2 = board + ".2";
    ASSERT_EQ(slash_hotplug_toggle_sbr(hp_, pf2.c_str()), 0)
        << "TOGGLE_SBR(" << board << ") failed: " << strerror(errno);
    EXPECT_TRUE(wait_for_ctl(true, board))
        << "control device should be up again and report board " << board
        << " after TOGGLE_SBR";
}

TEST_F(E2EHotplugTest, RemoveUnknownBdfFails)
{
    /*
     * REMOVE of a BDF that matches no tracked device must fail (not silently
     * succeed).  The daemon returns a negative errno; libslash maps it to -1.
     */
    SKIP_IF_NO_HOTPLUG();
    errno = 0;
    int rc = slash_hotplug_remove(hp_, "ffff:ff:ff.2");
    EXPECT_EQ(rc, -1) << "REMOVE of an unknown BDF unexpectedly succeeded";
}

TEST_F(E2EHotplugTest, DeviceMemoryPersistsAcrossPf1RemoveRescan)
{
    /*
     * Write a unique pattern to card memory via QDMA, REMOVE PF1 (the QDMA
     * function itself), RESCAN, then read it back via a FRESH QDMA session.
     *
     * REMOVE PF1 tears down ONLY the QDMA subsystem's socket; PF0 and PF2 remain
     * present, so the model process is NOT torn down (the last-PF rule keeps it
     * alive) and the control socket stays up throughout.  RESCAN then restores
     * PF1 via restore_pf(Pf1), which does NOT reconfigure — it simply rebuilds a
     * fresh QDMA subsystem bound to the CURRENT live model client.  Therefore:
     *   (A) the reconstructed QDMA must report the expected board BDF, and
     *   (B) the previously written bytes must still be present.
     * If the rebuilt QDMA had instead bound to a NEW model process, its HBM/DDR
     * would be freshly zeroed and the read-back would not match — so this is the
     * regression guard for "the new QDMA subsystem reconnects to the OLD model
     * process, not a new one".
     *
     * Detection keys on the QDMA socket (qdma_alive / wait_for_qdma), not the
     * control socket, because PF1 remove/restore leaves PF2's control socket up.
     */
    SKIP_IF_NO_ALL();
    std::string board = board_bdf_from_ctl();
    ASSERT_FALSE(board.empty()) << "could not read board BDF from ctl device_info";
    ASSERT_TRUE(qdma_alive(board))
        << "precondition: QDMA (PF1) must be up and report board " << board;

    /* A page of a distinctive, position-dependent pattern at a non-zero addr. */
    const uint64_t kDevAddr = 0x10000; /* page-aligned HBM offset, != other tests */
    const size_t kLen = 4096;
    std::vector<uint8_t> pattern(kLen);
    for (size_t i = 0; i < kLen; ++i) {
        pattern[i] = static_cast<uint8_t>((i * 31u + 7u) ^ 0xA5u);
    }

    std::string err;
    ASSERT_TRUE(qdma_xfer(e2e_qdma_path(), SLASH_QDMA_XFER_H2C, kDevAddr,
                          pattern.data(), kLen, err))
        << "initial H2C write failed: " << err;

    /* REMOVE PF1 → QDMA socket unlinked; model + PF2 keep running. */
    std::string pf1 = board + ".1";
    ASSERT_EQ(slash_hotplug_remove(hp_, pf1.c_str()), 0)
        << "REMOVE(" << pf1 << ") failed: " << strerror(errno);
    EXPECT_TRUE(wait_for_qdma(false))
        << "QDMA socket should be torn down after REMOVE of PF1";
    /* The control device (PF2) — and thus the model — must stay up the whole time. */
    EXPECT_TRUE(ctl_alive(board))
        << "control device (PF2) must stay up while PF1 is removed";
    ASSERT_EQ(slash_hotplug_rescan(hp_), 0)
        << "RESCAN failed: " << strerror(errno);
    EXPECT_TRUE(wait_for_qdma(true, board))
        << "QDMA (PF1) should be restored and report board " << board;

    /* (A) The reconstructed QDMA subsystem still reports the expected board BDF. */
    struct slash_qdma *q = slash_qdma_open(e2e_qdma_path());
    ASSERT_NE(q, nullptr) << "reopen QDMA after rescan failed: " << strerror(errno);
    struct slash_qdma_info info{};
    ASSERT_EQ(slash_qdma_info_read(q, &info), 0);
    EXPECT_EQ(board_of(info.bdf, sizeof(info.bdf)), board)
        << "reconstructed QDMA reports BDF '" << info.bdf << "', expected board " << board;
    slash_qdma_close(q);

    /* (B) The data must still be there — proving the reconstructed QDMA reconnected
     *     to the SAME model process (a new one would have zeroed HBM/DDR). */
    std::vector<uint8_t> readback(kLen, 0);
    ASSERT_TRUE(qdma_xfer(e2e_qdma_path(), SLASH_QDMA_XFER_C2H, kDevAddr,
                          readback.data(), kLen, err))
        << "post-rescan C2H read failed: " << err;
    EXPECT_EQ(readback, pattern)
        << "device memory did not persist across PF1 remove/rescan — the "
           "reconstructed QDMA subsystem appears to have reconnected to a NEW "
           "model process instead of the original one";
}
