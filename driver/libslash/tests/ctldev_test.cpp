/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

extern "C" {
#include <slash/ctldev.h>
}

static constexpr uint64_t MOCK_BAR_SIZE = 64ULL * 1024ULL * 1024ULL;
static constexpr const char *REAL_CTLDEV_PATH = "/dev/slash_ctl0";

// ─── Null / invalid argument tests (no hardware needed) ──────────────────────

TEST(CtldevOpenTest, NullPath) {
    errno = 0;
    struct slash_ctldev *dev = slash_ctldev_open(nullptr);
    EXPECT_EQ(dev, nullptr);
    EXPECT_EQ(errno, EFAULT);
}

TEST(CtldevCloseTest, NullHandle) {
    errno = 0;
    EXPECT_EQ(slash_ctldev_close(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(CtldevBarFileCloseTest, NullHandle) {
    errno = 0;
    EXPECT_EQ(slash_bar_file_close(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

// ─── Mock-mode tests (no hardware needed) ────────────────────────────────────

class MockCtldevTest : public ::testing::Test {
   protected:
    void SetUp() override {
        dev_ = slash_ctldev_open("@mock");
        ASSERT_NE(dev_, nullptr);
        ASSERT_TRUE(dev_->mock);
    }

    void TearDown() override {
        if (dev_) {
            slash_ctldev_close(dev_);
            dev_ = nullptr;
        }
    }

    struct slash_ctldev *dev_ = nullptr;
};

TEST_F(MockCtldevTest, OpenReturnsMockHandle) {
    EXPECT_TRUE(dev_->mock);
    EXPECT_EQ(dev_->fd, -1);
}

TEST_F(MockCtldevTest, CloseSucceeds) {
    EXPECT_EQ(slash_ctldev_close(dev_), 0);
    dev_ = nullptr;
}

TEST_F(MockCtldevTest, DeviceInfoRead) {
    struct slash_ioctl_device_info *info = slash_device_info_read(dev_);
    ASSERT_NE(info, nullptr);
    EXPECT_STREQ(info->bdf, "0000:00:00.0");
    slash_device_info_free(info);
}

TEST_F(MockCtldevTest, DeviceInfoReadNullHandle) {
    errno = 0;
    EXPECT_EQ(slash_device_info_read(nullptr), nullptr);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(MockCtldevTest, BarInfoReadBar0Usable) {
    struct slash_ioctl_bar_info *info = slash_bar_info_read(dev_, 0);
    ASSERT_NE(info, nullptr);
    EXPECT_NE(info->usable, 0);
    EXPECT_EQ(info->length, MOCK_BAR_SIZE);
    EXPECT_EQ(info->bar_number, 0);
    slash_bar_info_free(info);
}

TEST_F(MockCtldevTest, BarInfoReadNonZeroBarsNotUsable) {
    for (int bar = 1; bar <= 5; ++bar) {
        struct slash_ioctl_bar_info *info = slash_bar_info_read(dev_, bar);
        ASSERT_NE(info, nullptr) << "bar=" << bar;
        EXPECT_EQ(info->usable, 0) << "bar=" << bar;
        slash_bar_info_free(info);
    }
}

TEST_F(MockCtldevTest, BarInfoReadNullHandle) {
    errno = 0;
    EXPECT_EQ(slash_bar_info_read(nullptr, 0), nullptr);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(MockCtldevTest, BarFileOpenBar0) {
    struct slash_bar_file *bar = slash_bar_file_open(dev_, 0, 0);
    ASSERT_NE(bar, nullptr);
    EXPECT_NE(bar->map, nullptr);
    EXPECT_EQ(bar->len, MOCK_BAR_SIZE);
    EXPECT_TRUE(bar->mock);
    EXPECT_EQ(slash_bar_file_close(bar), 0);
}

TEST_F(MockCtldevTest, BarFileOpenNonZeroBarFails) {
    errno = 0;
    struct slash_bar_file *bar = slash_bar_file_open(dev_, 1, 0);
    EXPECT_EQ(bar, nullptr);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(MockCtldevTest, BarFileOpenNullHandle) {
    errno = 0;
    EXPECT_EQ(slash_bar_file_open(nullptr, 0, 0), nullptr);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(MockCtldevTest, BarFileSyncIsNoopInMockMode) {
    struct slash_bar_file *bar = slash_bar_file_open(dev_, 0, 0);
    ASSERT_NE(bar, nullptr);

    EXPECT_EQ(slash_bar_file_start_write(bar), 0);
    EXPECT_EQ(slash_bar_file_end_write(bar), 0);
    EXPECT_EQ(slash_bar_file_start_read(bar), 0);
    EXPECT_EQ(slash_bar_file_end_read(bar), 0);

    EXPECT_EQ(slash_bar_file_close(bar), 0);
}

TEST_F(MockCtldevTest, BarFileMapIsReadWrite) {
    struct slash_bar_file *bar = slash_bar_file_open(dev_, 0, 0);
    ASSERT_NE(bar, nullptr);

    auto *p = static_cast<uint32_t *>(bar->map);
    p[0] = 0xDEADBEEFu;
    EXPECT_EQ(p[0], 0xDEADBEEFu);

    EXPECT_EQ(slash_bar_file_close(bar), 0);
}

// ─── Real device tests (requires /dev/slash_ctl0) ────────────────────────────

class RealCtldevTest : public ::testing::Test {
   protected:
    void SetUp() override {
        dev_ = slash_ctldev_open(REAL_CTLDEV_PATH);
        if (!dev_) {
            GTEST_SKIP() << REAL_CTLDEV_PATH << " not available (errno=" << errno << ")";
        }
    }

    void TearDown() override {
        if (dev_) {
            slash_ctldev_close(dev_);
            dev_ = nullptr;
        }
    }

    struct slash_ctldev *dev_ = nullptr;
};

TEST_F(RealCtldevTest, OpenSucceeds) {
    EXPECT_FALSE(dev_->mock);
    EXPECT_GE(dev_->fd, 0);
}

TEST_F(RealCtldevTest, DeviceInfoBdfNonEmpty) {
    struct slash_ioctl_device_info *info = slash_device_info_read(dev_);
    ASSERT_NE(info, nullptr);
    EXPECT_GT(strlen(info->bdf), 0u);
    slash_device_info_free(info);
}

TEST_F(RealCtldevTest, Bar0InfoUsable) {
    struct slash_ioctl_bar_info *info = slash_bar_info_read(dev_, 0);
    ASSERT_NE(info, nullptr);
    EXPECT_NE(info->usable, 0);
    EXPECT_GT(info->length, 0u);
    slash_bar_info_free(info);
}

TEST_F(RealCtldevTest, Bar0FileOpenAndSync) {
    struct slash_bar_file *bar = slash_bar_file_open(dev_, 0, 0);
    ASSERT_NE(bar, nullptr);
    EXPECT_NE(bar->map, nullptr);
    EXPECT_GT(bar->len, 0u);
    EXPECT_FALSE(bar->mock);

    EXPECT_EQ(slash_bar_file_start_write(bar), 0);
    EXPECT_EQ(slash_bar_file_end_write(bar), 0);

    EXPECT_EQ(slash_bar_file_close(bar), 0);
}
