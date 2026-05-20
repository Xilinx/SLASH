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
#include <cstring>
#include <unistd.h>

extern "C" {
#include <slash/qdma.h>
}

static constexpr const char *REAL_QDMA_PATH = "/dev/slash_qdma_ctl0";
static constexpr uint64_t DDR_BASE_ADDRESS = 0x60000000000ULL;

// ─── Null / invalid argument tests (no hardware needed) ──────────────────────

TEST(QdmaNullTest, Open) {
    errno = 0;
    EXPECT_EQ(slash_qdma_open(nullptr), nullptr);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, Close) {
    errno = 0;
    EXPECT_EQ(slash_qdma_close(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, NullInfoRead) {
    struct slash_qdma_info info{};
    errno = 0;
    EXPECT_EQ(slash_qdma_info_read(nullptr, &info), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, FakeInfoRead) {
    /* Construct a minimal fake handle — we only need errno set by the NULL info check. */
    struct slash_qdma fake{};
    fake.fd = -1;
    errno = 0;
    EXPECT_EQ(slash_qdma_info_read(&fake, nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, NullQpairAdd) {
    struct slash_qdma_qpair_add req{};
    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_add(nullptr, &req), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, FakeQpairAdd) {
    struct slash_qdma fake{};
    fake.fd = -1;
    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_add(&fake, nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, QpairStart) {
    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_start(nullptr, 0), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, QpairStop) {
    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_stop(nullptr, 0), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, QpairDel) {
    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_del(nullptr, 0), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, QpaiGetFd) {
    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_get_fd(nullptr, 0, 0), -1);
    EXPECT_EQ(errno, EINVAL);
}

// ─── Real device tests (requires /dev/slash_qdma_ctl0) ───────────────────────

class ParametrizedQdmaTest : public ::testing::TestWithParam<bool> {
   protected:
    bool mock;

    void SetUp() override {
        mock = GetParam();
        if (mock) {
            qdma_ = slash_qdma_open("@mock");
            EXPECT_NE(qdma_, nullptr);
        } else {
            qdma_ = slash_qdma_open(REAL_QDMA_PATH);
            if (!qdma_) {
                GTEST_SKIP() << REAL_QDMA_PATH << " not available (errno=" << errno << ")";
            }
        }
    }

    void TearDown() override {
        if (qdma_) {
            slash_qdma_close(qdma_);
            qdma_ = nullptr;
        }
    }

    struct slash_qdma *qdma_ = nullptr;
};

TEST_P(ParametrizedQdmaTest, OpenSucceeds) {
    EXPECT_GE(qdma_->fd, mock ? -1 : 0);
    EXPECT_EQ(qdma_->priv != nullptr, mock);
}

TEST_P(ParametrizedQdmaTest, InfoRead) {
    struct slash_qdma_info info{};
    EXPECT_EQ(slash_qdma_info_read(qdma_, &info), 0);
}

TEST_P(ParametrizedQdmaTest, QueueDmaTransfer) {
    static constexpr size_t XFER_SIZE = 4096;

    // Add a Memory-Mapped queue pair with both H2C and C2H enabled.
    struct slash_qdma_qpair_add req{};
    req.mode         = 0; /* QDMA_Q_MODE_MM */
    req.dir_mask     = 0x3; /* H2C | C2H */
    req.h2c_ring_sz  = 0;
    req.c2h_ring_sz  = 0;
    req.cmpt_ring_sz = 0;

    ASSERT_EQ(slash_qdma_qpair_add(qdma_, &req), 0);
    uint32_t qid = req.qid;

    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);

    int queue_fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    ASSERT_GE(queue_fd, 0);

    // Write a known pattern to DDR (H2C).
    uint8_t src[XFER_SIZE];
    for (size_t i = 0; i < XFER_SIZE; ++i) {
        src[i] = static_cast<uint8_t>(i & 0xFF);
    }
    ssize_t written = pwrite(queue_fd, src, XFER_SIZE, static_cast<off_t>(DDR_BASE_ADDRESS));
    EXPECT_EQ(written, static_cast<ssize_t>(XFER_SIZE));

    // Read back from DDR (C2H) and verify.
    uint8_t dst[XFER_SIZE]{};
    ssize_t read_bytes = pread(queue_fd, dst, XFER_SIZE, static_cast<off_t>(DDR_BASE_ADDRESS));
    EXPECT_EQ(read_bytes, static_cast<ssize_t>(XFER_SIZE));
    EXPECT_EQ(std::memcmp(src, dst, XFER_SIZE), 0);

    EXPECT_EQ(close(queue_fd), 0);

    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0);
    EXPECT_EQ(slash_qdma_qpair_del(qdma_, qid), 0);
}

TEST_P(ParametrizedQdmaTest, CloseSucceeds) {
    EXPECT_EQ(slash_qdma_close(qdma_), 0);
    qdma_ = nullptr;
}

INSTANTIATE_TEST_SUITE_P(QdmaTest, ParametrizedQdmaTest, testing::Values(true, false));