/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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

#include "common.hpp"

#include <cerrno>
#include <thread>

extern "C" {
#include <slash/ctldev.h>
#include <slash/hotplug.h>
#include <slash/qdma.h>
}

// ─── Null / invalid argument tests (no hardware needed) ──────────────────────

TEST(HotplugOpenTest, NonexistentPathFails) {
    errno = 0;
    struct slash_hotplug *hp = slash_hotplug_open("/nonexistent/slash_hotplug");
    EXPECT_EQ(hp, nullptr);
    EXPECT_EQ(errno, ENOENT);
}

TEST(HotplugCloseTest, NullHandle) {
    errno = 0;
    EXPECT_EQ(slash_hotplug_close(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(HotplugRescanTest, NullHandle) {
    errno = 0;
    EXPECT_EQ(slash_hotplug_rescan(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(HotplugRemoveTest, NullHandle) {
    errno = 0;
    EXPECT_EQ(slash_hotplug_remove(nullptr, "0000:00:00.0"), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(HotplugToggleSbrTest, NullHandle) {
    errno = 0;
    EXPECT_EQ(slash_hotplug_toggle_sbr(nullptr, "0000:00:00.0"), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(HotplugHotplugTest, NullHandle) {
    errno = 0;
    EXPECT_EQ(slash_hotplug_hotplug(nullptr, "0000:00:00.0"), -1);
    EXPECT_EQ(errno, EINVAL);
}

// ─── Real device tests (requires /dev/slash_hotplug) ─────────────────────────

class HotplugTest : public ::testing::TestWithParam<LibSlashBackend> {
  protected:
    LibSlashBackend backend;
    std::string ctldev_path;
    std::string qdma_path;
    struct slash_hotplug *hp_ = nullptr;

    void SetUp() override {
        backend = GetParam();
        if (backend == LibSlashBackend::DRIVER) {
            hp_ = slash_hotplug_open(SLASH_DRIVER_HOTPLUG_PATH);
            if (!hp_) {
                GTEST_SKIP() << SLASH_DRIVER_HOTPLUG_PATH
                             << " not available (errno=" << errno << ")";
            }
            ctldev_path = SLASH_DRIVER_CTLDEV_PATH;
            qdma_path = SLASH_DRIVER_QDMA_PATH;
        } else if (backend == LibSlashBackend::SYSEMU) {
            hp_ = slash_hotplug_open(SLASH_SYSEMU_HOTPLUG_PATH);
            if (!hp_) {
                GTEST_SKIP() << SLASH_SYSEMU_HOTPLUG_PATH
                             << " not available (errno=" << errno << ")";
            }
            ctldev_path = SLASH_SYSEMU_CTLDEV_PATH;
            qdma_path = SLASH_SYSEMU_QDMA_PATH;
        }
        EXPECT_GE(hp_->fd, 0);
    }

    void TearDown() override {
        if (hp_) {
            EXPECT_EQ(slash_hotplug_close(hp_), 0);
            hp_ = nullptr;
        }
    }

    /*
     * Strip the ".<function>" suffix from a full BDF ("DDDD:BB:DD.F") to get
     * the board BDF ("DDDD:BB:DD").
     */
    static std::string board_of(const char *bdf, size_t cap) {
        std::string s(bdf, ::strnlen(bdf, cap));
        std::string::size_type dot = s.rfind('.');
        if (dot != std::string::npos)
            s.erase(dot);
        return s;
    }

    /*
     * Read the board BDF ("DDDD:BB:DD", function suffix stripped) from the
     * ctldev device_info.  Returns "" if the ctl socket is unavailable.  Used
     * to target the destructive hotplug ops at the actual accelerator without
     * hard-coding it.
     */
    std::string board_bdf_from_ctl() {
        struct slash_ctldev *dev = slash_ctldev_open(ctldev_path.c_str());
        if (!dev)
            return "";
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
     * board BDF (device_info bdf with the function suffix stripped) to equal it
     * — so "up" means "the expected accelerator is up", not merely "some device
     * is up".  This catches a restore that brings back the wrong / a
     * mislabelled device.
     */
    bool ctl_alive(const std::string &expect_board = "") {
        struct slash_ctldev *dev = slash_ctldev_open(ctldev_path.c_str());
        if (!dev)
            return false;
        struct slash_ioctl_device_info *info = slash_device_info_read(dev);
        bool ok = (info != nullptr);
        if (info && !expect_board.empty()) {
            ok = (board_of(info->bdf, sizeof(info->bdf)) == expect_board);
        }
        if (info)
            slash_device_info_free(info);
        slash_ctldev_close(dev);
        return ok;
    }

    /*
     * Poll until the control device reaches the desired state or the ~1s budget
     * expires.  Hotplug ops are synchronous in the daemon (REMOVE unlinks and
     * RESCAN rebinds before responding), so this only guards against brief
     * socket (re)creation races.  Returns true iff the desired state was
     * observed.
     *
     * For an EXISTENCE check (want=true) with a non-empty @expect_board, the
     * device must be up AND report that board BDF.  For an ABSENCE check
     * (want=false) the board is irrelevant — we wait for the device to be truly
     * gone (raw aliveness false), so an up-but-wrong-BDF device is never
     * mistaken for "removed".
     */
    bool wait_for_ctl(bool want, const std::string &expect_board = "") {
        for (int i = 0; i < 40; ++i) {
            bool alive = want ? ctl_alive(expect_board) : ctl_alive();
            if (alive == want)
                return true;
            ::usleep(25 * 1000); /* 25 ms */
        }
        bool alive = want ? ctl_alive(expect_board) : ctl_alive();
        return alive == want;
    }

    /*
     * True iff the QDMA subsystem (PF1) is currently reachable AND answers
     * info_read — i.e. PF1's socket exists and is bound to a live model.  After
     * a REMOVE of PF1 the socket is unlinked, so the open fails.  Unlike the
     * control device, PF1 is torn down/rebuilt independently of PF0/PF2, so
     * this is the probe that observes a PF1 remove/restore (the control socket
     * stays up throughout).
     *
     * If @expect_board is non-empty, additionally require the reported board
     * BDF (info.bdf with the function suffix stripped) to equal it — so "up"
     * means "the expected accelerator's QDMA is up", not merely "some QDMA is
     * up".
     */
    bool qdma_alive(const std::string &expect_board = "") {
        struct slash_qdma *q = slash_qdma_open(qdma_path.c_str());
        if (!q)
            return false;
        struct slash_qdma_info info{};
        bool ok = (slash_qdma_info_read(q, &info) == 0);
        if (ok && !expect_board.empty()) {
            ok = (board_of(info.bdf, sizeof(info.bdf)) == expect_board);
        }
        slash_qdma_close(q);
        return ok;
    }

    /*
     * Poll until PF1's QDMA reaches the desired state or the ~1s budget
     * expires. Mirrors wait_for_ctl: for an EXISTENCE check (want=true) with a
     * non-empty
     * @expect_board the QDMA must be up AND report that board; for an ABSENCE
     * check (want=false) the board is irrelevant.
     */
    bool wait_for_qdma(bool want, const std::string &expect_board = "") {
        for (int i = 0; i < 40; ++i) {
            bool alive = want ? qdma_alive(expect_board) : qdma_alive();
            if (alive == want)
                return true;
            ::usleep(25 * 1000); /* 25 ms */
        }
        bool alive = want ? qdma_alive(expect_board) : qdma_alive();
        return alive == want;
    }

    /*
     * Perform a single H2C or C2H transfer against a device address over a
     * FRESH QDMA session (open → add → start → get_fd → buf_create → transfer →
     * teardown). For H2C, @host is copied into the DMA buffer before the
     * transfer; for C2H, the buffer is zeroed, transferred, then copied back
     * into @host.  @len must be a whole number of pages and @dev_addr
     * page-aligned.  Returns true on success; on failure @err carries a short
     * reason.  Each call uses a brand-new connection so a post-reconstruction
     * call naturally exercises the rebuilt QDMA subsystem.
     */
    bool qdma_xfer(const char *qdma_path, uint32_t dir, uint64_t dev_addr,
                   void *host, size_t len, std::string &err) {
        struct slash_qdma *q = slash_qdma_open(qdma_path);
        if (!q) {
            err = std::string("qdma_open: ") + strerror(errno);
            return false;
        }

        struct slash_qdma_qpair_add add{};
        ::memset(&add, 0, sizeof(add));
        add.size = sizeof(add);
        add.mode = 0;       /* MM */
        add.dir_mask = 0x3; /* H2C | C2H */
        add.h2c_ring_sz = 0;
        add.c2h_ring_sz = 0;
        add.cmpt_ring_sz = 0;

        bool ok = false;
        bool have_qpair = false;
        int xfer = -1;
        struct slash_qdma_buffer buf{};

        if (slash_qdma_qpair_add(q, &add) == 0 &&
            slash_qdma_qpair_start(q, add.qid) == 0) {
            have_qpair = true;
            xfer = slash_qdma_qpair_get_fd(q, add.qid, 0);
            if (xfer >= 0 &&
                slash_qdma_qpair_buffer_create(xfer, len, &buf) == 0) {
                if (dir == SLASH_QDMA_XFER_H2C) {
                    ::memcpy(buf.addr, host, len);
                } else {
                    ::memset(buf.addr, 0, len);
                }
                ssize_t n = slash_qdma_qpair_transfer(xfer, buf.fd, 0, dev_addr,
                                                      len, dir);
                ok = (n == static_cast<ssize_t>(len));
                if (!ok) {
                    err = "transfer returned " + std::to_string(n) + " (" +
                          strerror(errno) + ")";
                } else if (dir == SLASH_QDMA_XFER_C2H) {
                    ::memcpy(host, buf.addr, len);
                }
            } else {
                err = std::string("get_fd/buf_create: ") + strerror(errno);
            }
        } else {
            err = std::string("qpair add/start: ") + strerror(errno);
        }

        if (buf.addr)
            slash_qdma_buffer_destroy(&buf);
        if (xfer >= 0)
            ::close(xfer);
        if (have_qpair) {
            slash_qdma_qpair_stop(q, add.qid);
            slash_qdma_qpair_del(q, add.qid);
        }
        slash_qdma_close(q);
        return ok;
    }
};

/* Just run a rescan without removing any prior devices. */
TEST_P(HotplugTest, RescanSucceeds) {
    EXPECT_EQ(slash_hotplug_rescan(hp_), 0)
        << "slash_hotplug_rescan failed: " << strerror(errno);
}

TEST_P(HotplugTest, RemovePf2ThenRescanRestores) {
    std::string board = board_bdf_from_ctl();
    ASSERT_FALSE(board.empty())
        << "could not read board BDF from ctl device_info";
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
        << "control device should be restored after RESCAN and report board "
        << board;
}

TEST_P(HotplugTest, HotplugBoardRoundTrip) {
    /*
     * HOTPLUG = REMOVE + RESCAN as one operation; self-restoring.  Targeting
     * the bare board BDF removes all PFs then brings them back.
     */
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

TEST_P(HotplugTest, ToggleSbrBoardRoundTrip) {
    GTEST_SKIP() << "Secondary bus resets are known to cause issues on some "
                    "systems. We therefore skip this automated test for now.";

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

TEST_P(HotplugTest, DeviceMemoryPersistsAcrossPf1RemoveRescan) {
    /*
     * Write a unique pattern to card memory via QDMA, REMOVE PF1 (the QDMA
     * function itself), RESCAN, then read it back via a FRESH QDMA session.
     *
     * On the sysemu daemon, REMOVE PF1 tears down ONLY the QDMA subsystem
     * socket; PF0 and PF2 remain present, so the model process is NOT torn down
     * (the last-PF rule keeps it alive) and the control socket stays up
     * throughout. RESCAN then restores PF1 via restore_pf(Pf1), which does NOT
     * reconfigure — it simply rebuilds a fresh QDMA subsystem bound to the
     * CURRENT live model client.  Therefore: (A) the reconstructed QDMA must
     * report the expected board BDF, and (B) the previously written bytes must
     * still be present. If the rebuilt QDMA had instead bound to a NEW model
     * process, its HBM/DDR would be freshly zeroed and the read-back would not
     * match — so this is the regression guard for "the new QDMA subsystem
     * reconnects to the OLD model process, not a new one".
     *
     * With the physical card, REMOVE PF1 only tears down the host's knowledge
     * of the QDMA core, the physical memory is of course unaffected. Therefore,
     * this behavior must also be correct here.
     *
     * Detection keys on the QDMA socket (qdma_alive / wait_for_qdma), not the
     * control socket, because PF1 remove/restore leaves PF2's control socket
     * up.
     */
    std::string board = board_bdf_from_ctl();
    ASSERT_FALSE(board.empty())
        << "could not read board BDF from ctl device_info";
    ASSERT_TRUE(qdma_alive(board))
        << "precondition: QDMA (PF1) must be up and report board " << board;

    /* A page of a distinctive, position-dependent pattern at a non-zero addr.
     */
    const uint64_t kDevAddr =
        HBM_BASE_ADDRESS; /* page-aligned HBM offset, != other tests */
    const size_t kLen = 4096;
    std::vector<uint8_t> pattern(kLen);
    for (size_t i = 0; i < kLen; ++i) {
        pattern[i] = static_cast<uint8_t>((i * 31u + 7u) ^ 0xA5u);
    }

    std::string err;
    ASSERT_TRUE(qdma_xfer(qdma_path.c_str(), SLASH_QDMA_XFER_H2C, kDevAddr,
                          pattern.data(), kLen, err))
        << "initial H2C write failed: " << err;

    /* Sleeping for a second to await the completion of the transfer. */
    std::this_thread::sleep_for(std::chrono::seconds(1));

    /* REMOVE PF1 → QDMA socket unlinked; model + PF2 keep running. */
    std::string pf1 = board + ".1";
    ASSERT_EQ(slash_hotplug_remove(hp_, pf1.c_str()), 0)
        << "REMOVE(" << pf1 << ") failed: " << strerror(errno);
    EXPECT_TRUE(wait_for_qdma(false))
        << "QDMA socket should be torn down after REMOVE of PF1";
    /* The control device (PF2) — and thus the model — must stay up the whole
     * time. */
    EXPECT_TRUE(ctl_alive(board))
        << "control device (PF2) must stay up while PF1 is removed";
    ASSERT_EQ(slash_hotplug_rescan(hp_), 0)
        << "RESCAN failed: " << strerror(errno);
    EXPECT_TRUE(wait_for_qdma(true, board))
        << "QDMA (PF1) should be restored and report board " << board;

    /* (A) The reconstructed QDMA subsystem still reports the expected board
     * BDF. */
    struct slash_qdma *q = slash_qdma_open(qdma_path.c_str());
    ASSERT_NE(q, nullptr) << "reopen QDMA after rescan failed: "
                          << strerror(errno);
    struct slash_qdma_info info{};
    ASSERT_EQ(slash_qdma_info_read(q, &info), 0);
    EXPECT_EQ(board_of(info.bdf, sizeof(info.bdf)), board)
        << "reconstructed QDMA reports BDF '" << info.bdf
        << "', expected board " << board;
    slash_qdma_close(q);

    /* (B) The data must still be there — proving the reconstructed QDMA
     * reconnected to the SAME model process (a new one would have zeroed
     * HBM/DDR). */
    std::vector<uint8_t> readback(kLen, 0);
    ASSERT_TRUE(qdma_xfer(qdma_path.c_str(), SLASH_QDMA_XFER_C2H, kDevAddr,
                          readback.data(), kLen, err))
        << "post-rescan C2H read failed: " << err;
    EXPECT_EQ(readback, pattern)
        << "device memory did not persist across PF1 remove/rescan — the "
           "reconstructed QDMA subsystem appears to have reconnected to a NEW "
           "model process instead of the original one";
}

INSTANTIATE_TEST_SUITE_P(HotplugTest, HotplugTest,
                         testing::Values(LibSlashBackend::DRIVER,
                                         LibSlashBackend::SYSEMU),
                         [](auto info) {
                             switch (info.param) {
                             case LibSlashBackend::DRIVER:
                                 return "driver";
                             case LibSlashBackend::SYSEMU:
                                 return "sysemu";
                             case LibSlashBackend::MOCK:
                                 return "mock";
                             default:
                                 return "unknown";
                             }
                         });
