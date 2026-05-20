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

extern "C" {
#include <slash/hotplug.h>
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

class RealHotplugTest : public ::testing::Test {
   protected:
    void SetUp() override {
        hp_ = slash_hotplug_open(SLASH_HOTPLUG_DEFAULT_PATH);
        if (!hp_) {
            GTEST_SKIP() << SLASH_HOTPLUG_DEFAULT_PATH
                         << " not available (errno=" << errno << ")";
        }
    }

    void TearDown() override {
        if (hp_) {
            slash_hotplug_close(hp_);
            hp_ = nullptr;
        }
    }

    struct slash_hotplug *hp_ = nullptr;
};

TEST_F(RealHotplugTest, OpenDefaultPathSucceeds) {
    EXPECT_GE(hp_->fd, 0);
}

TEST_F(RealHotplugTest, OpenExplicitPathSucceeds) {
    struct slash_hotplug *hp2 = slash_hotplug_open("/dev/slash_hotplug");
    ASSERT_NE(hp2, nullptr);
    EXPECT_GE(hp2->fd, 0);
    EXPECT_EQ(slash_hotplug_close(hp2), 0);
}

TEST_F(RealHotplugTest, CloseSucceeds) {
    EXPECT_EQ(slash_hotplug_close(hp_), 0);
    hp_ = nullptr;
}
