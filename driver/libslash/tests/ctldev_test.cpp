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
#include <cstdint>
#include <cstring>

extern "C" {
#include <slash/ctldev.h>
}

static constexpr uint64_t MOCK_BAR_SIZE = 64ULL * 1024ULL * 1024ULL;

// ─── Null / invalid argument tests (no hardware needed) ──────────────────────

TEST(CtldevNullTest, Open) {
    errno = 0;
    struct slash_ctldev *dev = slash_ctldev_open(nullptr);
    EXPECT_EQ(dev, nullptr);
    EXPECT_EQ(errno, EINVAL);
}

TEST(CtldevNullTest, Close) {
    errno = 0;
    EXPECT_EQ(slash_ctldev_close(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(CtldevNullTest, DeviceInfoRead) {
    errno = 0;
    struct slash_ioctl_device_info *dev = slash_device_info_read(nullptr);
    EXPECT_EQ(dev, nullptr);
    EXPECT_EQ(errno, EINVAL);
}

TEST(CtldevNullTest, DeviceInfoFree) {
    // No way for us to check anything.
    // The address sanitizer catches errors in nullptr handling.
    slash_device_info_free(nullptr);
}

TEST(CtldevNullTest, BarInfoRead) {
    errno = 0;
    struct slash_ioctl_bar_info *dev = slash_bar_info_read(nullptr, 0);
    EXPECT_EQ(dev, nullptr);
    EXPECT_EQ(errno, EINVAL);
}

TEST(CtldevNullTest, BarInfoFree) {
    // No way for us to check anything.
    // The address sanitizer catches errors in nullptr handling.
    slash_bar_info_free(nullptr);
}

TEST(CtldevNullTest, BarFileOpen) {
    errno = 0;
    struct slash_bar_file *bar_file = slash_bar_file_open(nullptr, 0, 0);
    EXPECT_EQ(bar_file, nullptr);
    EXPECT_EQ(errno, EINVAL);
}

TEST(CtldevNullTest, BarFileClose) {
    errno = 0;
    EXPECT_EQ(slash_bar_file_close(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(CtldevNullTest, BarFileSync) {
    errno = 0;
    EXPECT_EQ(slash_bar_file_sync(nullptr, 0), -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(slash_bar_file_start_write(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(slash_bar_file_end_write(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(slash_bar_file_start_read(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(slash_bar_file_end_read(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

// ─── Real device tests (requires /dev/slash_ctl0) ────────────────────────────

class CtldevTest : public ::testing::TestWithParam<LibSlashBackend> {
  protected:
    void SetUp() override {
        backend = GetParam();
        switch (backend) {
        case LibSlashBackend::DRIVER:
            dev_ = slash_ctldev_open(SLASH_DRIVER_CTLDEV_PATH);
            if (!dev_) {
                GTEST_SKIP() << SLASH_DRIVER_CTLDEV_PATH << " not available ("
                             << strerror(errno) << ")";
            }
            break;
        case LibSlashBackend::SYSEMU:
            dev_ = slash_ctldev_open(SLASH_SYSEMU_CTLDEV_PATH);
            if (!dev_) {
                GTEST_SKIP() << SLASH_SYSEMU_CTLDEV_PATH << " not available ("
                             << strerror(errno) << ")";
            }
            break;
        case LibSlashBackend::MOCK:
            dev_ = slash_ctldev_open("@mock");
            if (!dev_) {
                GTEST_FAIL()
                    << "Mock support not available (" << strerror(errno) << ")";
            }
        default:
            GTEST_FAIL() << "Unknown backend!";
        }

        EXPECT_GE(dev_->fd, 0);
    }

    void TearDown() override {
        if (dev_) {
            EXPECT_EQ(slash_ctldev_close(dev_), 0);
            dev_ = nullptr;
        }
    }

    LibSlashBackend backend;
    struct slash_ctldev *dev_ = nullptr;
};

TEST_P(CtldevTest, DeviceInfoBdfNonEmpty) {
    struct slash_ioctl_device_info *info = slash_device_info_read(dev_);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->vendor_id,           0x10EEu);
    EXPECT_EQ(info->device_id,           0x50B6u);
    EXPECT_EQ(info->subsystem_vendor_id, 0x10EEu);
    EXPECT_EQ(info->subsystem_device_id, 0x000eu);
    EXPECT_GT(strlen(info->bdf), 0u);
    if (backend == LibSlashBackend::MOCK) {
        EXPECT_STREQ(info->bdf, "0000:00:00.1"); // TODO: Insert correct BDF
    }
    slash_device_info_free(info);
}

TEST_P(CtldevTest, EvenBarsUsable) {
    for (int bar = 0; bar < 6; bar++) {
        struct slash_ioctl_bar_info *info = slash_bar_info_read(dev_, bar);
        ASSERT_NE(info, nullptr);
        if (bar % 2 == 0) {
            EXPECT_GT(info->usable, 0) << "Bar " << bar << " unusable";
            if (backend == LibSlashBackend::MOCK && bar == 0) {
                EXPECT_EQ(info->length, MOCK_BAR_SIZE);
            } else {
                EXPECT_GT(info->length, 0u);
            }
        } else {
            EXPECT_EQ(info->usable, 0);
        }
        EXPECT_EQ(info->bar_number, bar);
        slash_bar_info_free(info);
    }
}

TEST_P(CtldevTest, Bar0FileOpenAndSync) {
    struct slash_bar_file *bar = slash_bar_file_open(dev_, 0, 0);
    ASSERT_NE(bar, nullptr);
    EXPECT_NE(bar->map, nullptr);
    EXPECT_GT(bar->len, 0u);

    // This test assumes a normal Vitis HLS kernel at offset 0.
    // Such a kernel has their first parameter register at offset 0x10 relative
    // to their base, which is the fifth integer.
    EXPECT_EQ(slash_bar_file_start_write(bar), 0);
    static_cast<uint32_t *>(bar->map)[4] = 42;
    EXPECT_EQ(slash_bar_file_end_write(bar), 0);

    EXPECT_EQ(slash_bar_file_start_read(bar), 0);
    EXPECT_EQ(static_cast<uint32_t *>(bar->map)[4], 42);
    EXPECT_EQ(slash_bar_file_end_read(bar), 0);

    EXPECT_EQ(slash_bar_file_close(bar), 0);
}

TEST_P(CtldevTest, BarFileSyncFlocks) {
    if (backend == LibSlashBackend::DRIVER) {
        GTEST_SKIP() << "The driver exposes dmabufs instead of memfds";
    }
    struct slash_bar_file *bar = slash_bar_file_open(dev_, 0, 0);
    EXPECT_NE(bar, nullptr);
    EXPECT_NE(bar->map, nullptr);
    EXPECT_GT(bar->len, 0);

    int separate_fd =
        ::open(("/proc/self/fd/" + std::to_string(bar->fd)).c_str(),
               O_RDWR | O_CLOEXEC);
    EXPECT_GT(separate_fd, 0) << strerror(errno);

    auto check_flock_fails = [separate_fd](int op) {
        int rv = flock(separate_fd, op | LOCK_NB);
        EXPECT_NE(rv, 0);
        EXPECT_EQ(errno, EWOULDBLOCK) << strerror(errno);
        if (rv == 0) {
            EXPECT_EQ(flock(separate_fd, LOCK_UN), 0);
        }
    };
    auto check_flock_succeeds = [separate_fd](int op) {
        int rv = flock(separate_fd, op | LOCK_NB);
        EXPECT_EQ(rv, 0) << strerror(errno);
        if (rv == 0) {
            EXPECT_EQ(flock(separate_fd, LOCK_UN), 0);
        }
    };

    // Establishing the current state: Locking is possible and the lock is
    // released.
    check_flock_succeeds(LOCK_EX);
    check_flock_succeeds(LOCK_SH);

    // Testing write locking
    EXPECT_EQ(slash_bar_file_start_write(bar), 0);
    check_flock_fails(LOCK_EX);
    check_flock_fails(LOCK_SH);

    EXPECT_EQ(slash_bar_file_end_write(bar), 0);
    check_flock_succeeds(LOCK_EX);
    check_flock_succeeds(LOCK_SH);

    EXPECT_EQ(slash_bar_file_start_read(bar), 0);
    check_flock_fails(LOCK_EX);
    check_flock_succeeds(LOCK_SH);

    EXPECT_EQ(slash_bar_file_end_read(bar), 0);
    check_flock_succeeds(LOCK_EX);
    check_flock_succeeds(LOCK_SH);

    close(separate_fd);
    EXPECT_EQ(slash_bar_file_close(bar), 0);
}

INSTANTIATE_TEST_SUITE_P(
    CtldevTest, CtldevTest,
    testing::Values(LibSlashBackend::DRIVER,
                    LibSlashBackend::SYSEMU), //, LibSlashBackend::MOCK),
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
