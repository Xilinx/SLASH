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
#include <cstdlib>
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

TEST(QdmaNullTest, BufferCreate) {
    struct slash_qdma_buffer buf{};
    errno = 0;
    EXPECT_EQ(slash_qdma_buffer_create(nullptr, 4096, &buf), -1);
    EXPECT_EQ(errno, EINVAL);

    struct slash_qdma fake{};
    fake.fd = -1;
    errno = 0;
    EXPECT_EQ(slash_qdma_buffer_create(&fake, 4096, nullptr), -1);
    EXPECT_EQ(errno, EINVAL);

    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_buffer_create(-1, 4096, &buf), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, BufferDestroy) {
    errno = 0;
    EXPECT_EQ(slash_qdma_buffer_destroy(nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, Transfer) {
    errno = 0;
    /* Invalid qpair fd is rejected. */
    EXPECT_EQ(slash_qdma_qpair_transfer(-1, 4, 0, 0, 4096, SLASH_QDMA_XFER_H2C), -1);
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

    // Kernel-owned buffers created through the queue-pair fd.
    struct slash_qdma_buffer src_buf{};
    struct slash_qdma_buffer dst_buf{};
    ASSERT_EQ(slash_qdma_qpair_buffer_create(queue_fd, XFER_SIZE, &src_buf), 0);
    ASSERT_EQ(slash_qdma_qpair_buffer_create(queue_fd, XFER_SIZE, &dst_buf), 0);
    auto *src = static_cast<uint8_t *>(src_buf.addr);
    auto *dst = static_cast<uint8_t *>(dst_buf.addr);
    for (size_t i = 0; i < XFER_SIZE; ++i) {
        src[i] = static_cast<uint8_t>(i & 0xFF);
    }
    std::memset(dst, 0, XFER_SIZE);

    ssize_t written = slash_qdma_qpair_transfer(
        queue_fd, src_buf.fd, 0, DDR_BASE_ADDRESS, XFER_SIZE, SLASH_QDMA_XFER_H2C);
    EXPECT_EQ(written, static_cast<ssize_t>(XFER_SIZE));

    // Read back from DDR (C2H) and verify.
    ssize_t read_bytes = slash_qdma_qpair_transfer(
        queue_fd, dst_buf.fd, 0, DDR_BASE_ADDRESS, XFER_SIZE, SLASH_QDMA_XFER_C2H);
    EXPECT_EQ(read_bytes, static_cast<ssize_t>(XFER_SIZE));
    EXPECT_EQ(std::memcmp(src, dst, XFER_SIZE), 0);

    EXPECT_EQ(slash_qdma_buffer_destroy(&src_buf), 0);
    EXPECT_EQ(slash_qdma_buffer_destroy(&dst_buf), 0);

    EXPECT_EQ(close(queue_fd), 0);

    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0);
    EXPECT_EQ(slash_qdma_qpair_del(qdma_, qid), 0);
}

TEST_P(ParametrizedQdmaTest, BufferCreateTransfer) {
    static constexpr size_t XFER_SIZE = 4096;

    struct slash_qdma_qpair_add req{};
    req.mode     = 0;   /* QDMA_Q_MODE_MM */
    req.dir_mask = 0x3; /* H2C | C2H */

    ASSERT_EQ(slash_qdma_qpair_add(qdma_, &req), 0);
    uint32_t qid = req.qid;
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);

    int queue_fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    ASSERT_GE(queue_fd, 0);

    // Kernel-owned buffers created through the control handle.
    struct slash_qdma_buffer src_buf{};
    struct slash_qdma_buffer dst_buf{};
    ASSERT_EQ(slash_qdma_buffer_create(qdma_, XFER_SIZE, &src_buf), 0);
    ASSERT_EQ(slash_qdma_buffer_create(qdma_, XFER_SIZE, &dst_buf), 0);
    EXPECT_EQ(src_buf.transfer_hint, SLASH_QDMA_TRANSFER_HINT_V80);
    EXPECT_EQ(dst_buf.transfer_hint, SLASH_QDMA_TRANSFER_HINT_V80);
    auto *src = static_cast<uint8_t *>(src_buf.addr);
    auto *dst = static_cast<uint8_t *>(dst_buf.addr);
    for (size_t i = 0; i < XFER_SIZE; ++i) {
        src[i] = static_cast<uint8_t>(i & 0xFF);
    }
    std::memset(dst, 0, XFER_SIZE);

    // H2C: push the source buffer to the device.
    ssize_t written = slash_qdma_qpair_transfer(queue_fd, src_buf.fd, 0,
                                                DDR_BASE_ADDRESS, XFER_SIZE,
                                                SLASH_QDMA_XFER_H2C);
    EXPECT_EQ(written, static_cast<ssize_t>(XFER_SIZE));

    // C2H: pull it back into the destination buffer and verify.
    ssize_t read_bytes = slash_qdma_qpair_transfer(queue_fd, dst_buf.fd, 0,
                                                   DDR_BASE_ADDRESS, XFER_SIZE,
                                                   SLASH_QDMA_XFER_C2H);
    EXPECT_EQ(read_bytes, static_cast<ssize_t>(XFER_SIZE));
    EXPECT_EQ(std::memcmp(src, dst, XFER_SIZE), 0);

    EXPECT_EQ(slash_qdma_buffer_destroy(&src_buf), 0);
    EXPECT_EQ(slash_qdma_buffer_destroy(&dst_buf), 0);

    EXPECT_EQ(close(queue_fd), 0);
    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0);
    EXPECT_EQ(slash_qdma_qpair_del(qdma_, qid), 0);
}

TEST_P(ParametrizedQdmaTest, QpairAddAcceptsKeyholeAperture) {
    struct slash_qdma_qpair_add req{};
    req.mode          = 0;   /* QDMA_Q_MODE_MM */
    req.dir_mask      = 0x1; /* H2C */
    req.aperture_size = 4096;

    ASSERT_EQ(slash_qdma_qpair_add(qdma_, &req), 0);
    uint32_t qid = req.qid;

    EXPECT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);
    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0);
    EXPECT_EQ(slash_qdma_qpair_del(qdma_, qid), 0);
}

TEST_P(ParametrizedQdmaTest, PartialLengthTransfer) {
    static constexpr size_t BUFFER_SIZE = 4096;
    static constexpr size_t XFER_SIZE = 4096 - 17;

    struct slash_qdma_qpair_add req{};
    req.mode     = 0;   /* QDMA_Q_MODE_MM */
    req.dir_mask = 0x3; /* H2C | C2H */

    ASSERT_EQ(slash_qdma_qpair_add(qdma_, &req), 0);
    uint32_t qid = req.qid;
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);

    int queue_fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    ASSERT_GE(queue_fd, 0);

    struct slash_qdma_buffer src_buf{};
    struct slash_qdma_buffer dst_buf{};
    ASSERT_EQ(slash_qdma_buffer_create(qdma_, BUFFER_SIZE, &src_buf), 0);
    ASSERT_EQ(slash_qdma_buffer_create(qdma_, BUFFER_SIZE, &dst_buf), 0);
    auto *src = static_cast<uint8_t *>(src_buf.addr);
    auto *dst = static_cast<uint8_t *>(dst_buf.addr);
    for (size_t i = 0; i < XFER_SIZE; ++i) {
        src[i] = static_cast<uint8_t>((i * 3 + 5) & 0xFF);
    }
    std::memset(dst, 0, BUFFER_SIZE);

    ssize_t written = slash_qdma_qpair_transfer(queue_fd, src_buf.fd, 0,
                                                DDR_BASE_ADDRESS, XFER_SIZE,
                                                SLASH_QDMA_XFER_H2C);
    EXPECT_EQ(written, static_cast<ssize_t>(XFER_SIZE));

    ssize_t read_bytes = slash_qdma_qpair_transfer(queue_fd, dst_buf.fd, 0,
                                                   DDR_BASE_ADDRESS, XFER_SIZE,
                                                   SLASH_QDMA_XFER_C2H);
    EXPECT_EQ(read_bytes, static_cast<ssize_t>(XFER_SIZE));
    EXPECT_EQ(std::memcmp(src, dst, XFER_SIZE), 0);

    EXPECT_EQ(slash_qdma_buffer_destroy(&src_buf), 0);
    EXPECT_EQ(slash_qdma_buffer_destroy(&dst_buf), 0);

    EXPECT_EQ(close(queue_fd), 0);
    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0);
    EXPECT_EQ(slash_qdma_qpair_del(qdma_, qid), 0);
}

TEST_P(ParametrizedQdmaTest, MultiQpairBatchTransfer) {
    // Two 4 KiB halves transferred concurrently across two queue pairs bound to
    // a single fd, exercising the get-fd-multi + batch transfer API.
    static constexpr size_t HALF = 4096;
    static constexpr size_t XFER_SIZE = 2 * HALF;

    uint32_t qids[2] = {0, 0};
    for (int ch = 0; ch < 2; ++ch) {
        struct slash_qdma_qpair_add req{};
        req.mode       = 0;   /* QDMA_Q_MODE_MM */
        req.dir_mask   = 0x3; /* H2C | C2H */
        req.mm_channel = static_cast<uint32_t>(
            ch == 0 ? SLASH_QDMA_MM_CHANNEL_0 : SLASH_QDMA_MM_CHANNEL_1);
        ASSERT_EQ(slash_qdma_qpair_add(qdma_, &req), 0);
        qids[ch] = req.qid;
        ASSERT_EQ(slash_qdma_qpair_start(qdma_, qids[ch]), 0);
    }

    int fd = slash_qdma_qpair_get_fd_multi(qdma_, qids, 2, 0);
    ASSERT_GE(fd, 0);

    struct slash_qdma_buffer src_buf{};
    struct slash_qdma_buffer dst_buf{};
    ASSERT_EQ(slash_qdma_qpair_buffer_create(fd, XFER_SIZE, &src_buf), 0);
    ASSERT_EQ(slash_qdma_qpair_buffer_create(fd, XFER_SIZE, &dst_buf), 0);
    auto *src = static_cast<uint8_t *>(src_buf.addr);
    auto *dst = static_cast<uint8_t *>(dst_buf.addr);
    for (size_t i = 0; i < XFER_SIZE; ++i) {
        src[i] = static_cast<uint8_t>((i * 7 + 1) & 0xFF);
    }
    std::memset(dst, 0, XFER_SIZE);

    // H2C: lower half on qpair 0, upper half on qpair 1, in one ioctl.
    struct slash_qdma_subxfer h2c[2]{};
    h2c[0] = {0, SLASH_QDMA_XFER_H2C, src_buf.fd, 0, 0, DDR_BASE_ADDRESS, HALF};
    h2c[1] = {1, SLASH_QDMA_XFER_H2C, src_buf.fd, 0, HALF, DDR_BASE_ADDRESS + HALF, HALF};
    EXPECT_EQ(slash_qdma_qpair_transfer_batch(fd, h2c, 2),
              static_cast<ssize_t>(XFER_SIZE));

    // C2H: read both halves back across both channels in one ioctl.
    struct slash_qdma_subxfer c2h[2]{};
    c2h[0] = {0, SLASH_QDMA_XFER_C2H, dst_buf.fd, 0, 0, DDR_BASE_ADDRESS, HALF};
    c2h[1] = {1, SLASH_QDMA_XFER_C2H, dst_buf.fd, 0, HALF, DDR_BASE_ADDRESS + HALF, HALF};
    EXPECT_EQ(slash_qdma_qpair_transfer_batch(fd, c2h, 2),
              static_cast<ssize_t>(XFER_SIZE));

    EXPECT_EQ(std::memcmp(src, dst, XFER_SIZE), 0);

    EXPECT_EQ(slash_qdma_buffer_destroy(&src_buf), 0);
    EXPECT_EQ(slash_qdma_buffer_destroy(&dst_buf), 0);

    EXPECT_EQ(close(fd), 0);
    for (int ch = 0; ch < 2; ++ch) {
        EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qids[ch]), 0);
        EXPECT_EQ(slash_qdma_qpair_del(qdma_, qids[ch]), 0);
    }
}

TEST(QdmaNullTest, QpairGetFdMultiInvalid) {
    uint32_t qids[2] = {0, 1};
    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_get_fd_multi(nullptr, qids, 2, 0), -1);
    EXPECT_EQ(errno, EINVAL);

    struct slash_qdma fake{};
    fake.fd = -1;
    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_get_fd_multi(&fake, qids, 0, 0), -1);
    EXPECT_EQ(errno, EINVAL);

    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_get_fd_multi(&fake, qids, 3, 0), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(QdmaNullTest, TransferBatchInvalid) {
    struct slash_qdma_subxfer x{};
    x.direction = SLASH_QDMA_XFER_H2C;
    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_transfer_batch(-1, &x, 1), -1);
    EXPECT_EQ(errno, EINVAL);

    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_transfer_batch(3, nullptr, 1), -1);
    EXPECT_EQ(errno, EINVAL);

    errno = 0;
    EXPECT_EQ(slash_qdma_qpair_transfer_batch(3, &x, 0), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST_P(ParametrizedQdmaTest, QueueFdReadWriteRejectedOnHardware) {
    if (mock) {
        GTEST_SKIP() << "mock qpair fds are memfds and still support read/write";
    }

    struct slash_qdma_qpair_add req{};
    req.mode     = 0;
    req.dir_mask = 0x3;

    ASSERT_EQ(slash_qdma_qpair_add(qdma_, &req), 0);
    uint32_t qid = req.qid;
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);

    int queue_fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    ASSERT_GE(queue_fd, 0);

    uint8_t byte = 0;
    errno = 0;
    EXPECT_EQ(write(queue_fd, &byte, sizeof(byte)), -1);
    EXPECT_TRUE(errno == EINVAL || errno == EOPNOTSUPP || errno == EBADF);

    errno = 0;
    EXPECT_EQ(read(queue_fd, &byte, sizeof(byte)), -1);
    EXPECT_TRUE(errno == EINVAL || errno == EOPNOTSUPP || errno == EBADF);

    EXPECT_EQ(close(queue_fd), 0);
    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0);
    EXPECT_EQ(slash_qdma_qpair_del(qdma_, qid), 0);
}

TEST_P(ParametrizedQdmaTest, CloseSucceeds) {
    EXPECT_EQ(slash_qdma_close(qdma_), 0);
    qdma_ = nullptr;
}

INSTANTIATE_TEST_SUITE_P(QdmaTest, ParametrizedQdmaTest, testing::Values(true, false));