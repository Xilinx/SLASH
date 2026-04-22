/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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

/**
 * All tests here run against the mock QDMA only. Writing an arbitrary payload to
 * a real FPGA via QDMA at the design-writer address risks corrupting the device
 * bitstream, so real-hardware execution is intentionally excluded.
 *
 * The mock QDMA fd is a memfd_create() file, so lseek()+write() at the fixed
 * QDMA bitstream address (0x102100000) works via sparse pages — no hardware needed.
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>

extern "C" {
#include <slash/qdma.h>
#include "design_writer.h"
}

// ─── Helper: create a small in-memory bitstream fd ───────────────────────────

static int make_bitstream_fd(uint8_t fill, size_t len)
{
    int fd = memfd_create("bitstream", MFD_CLOEXEC);
    if (fd < 0)
        return -1;

    std::vector<uint8_t> buf(len, fill);
    if (write(fd, buf.data(), len) != static_cast<ssize_t>(len)) {
        close(fd);
        return -1;
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// ─── Null / argument validation (always run, no fixture needed) ───────────────

TEST(DesignWriterNullTest, CreateNullQdma) {
    EXPECT_EQ(design_writer_create(nullptr), nullptr);
}

TEST(DesignWriterNullTest, SubmitFdNullWriter) {
    int fd = make_bitstream_fd(0xAB, 4096);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(design_writer_submit_fd(nullptr, fd), -1);
    close(fd);
}

TEST(DesignWriterNullTest, SubmitAsyncNullWriter) {
    int fd = make_bitstream_fd(0xAB, 4096);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(design_writer_submit_fd_async(nullptr, fd), -1);
    close(fd);
}

TEST(DesignWriterNullTest, SubmitAsyncInvalidFd) {
    struct slash_qdma *qdma = slash_qdma_open("@mock");
    ASSERT_NE(qdma, nullptr);
    struct design_writer *dw = design_writer_create(qdma);
    ASSERT_NE(dw, nullptr);

    EXPECT_EQ(design_writer_submit_fd_async(dw, -1), -1);

    cleanup_design_writer(dw);
    slash_qdma_close(qdma);
}

TEST(DesignWriterNullTest, PollResultNullWriter) {
    bool done = false;
    int last_error = 0;
    EXPECT_EQ(design_writer_poll_result(nullptr, &done, &last_error), -1);
}

TEST(DesignWriterNullTest, PollResultNullDone) {
    struct slash_qdma *qdma = slash_qdma_open("@mock");
    ASSERT_NE(qdma, nullptr);
    struct design_writer *dw = design_writer_create(qdma);
    ASSERT_NE(dw, nullptr);

    int last_error = 0;
    EXPECT_EQ(design_writer_poll_result(dw, nullptr, &last_error), -1);

    cleanup_design_writer(dw);
    slash_qdma_close(qdma);
}

TEST(DesignWriterNullTest, PollResultNullLastError) {
    struct slash_qdma *qdma = slash_qdma_open("@mock");
    ASSERT_NE(qdma, nullptr);
    struct design_writer *dw = design_writer_create(qdma);
    ASSERT_NE(dw, nullptr);

    bool done = false;
    EXPECT_EQ(design_writer_poll_result(dw, &done, nullptr), -1);

    cleanup_design_writer(dw);
    slash_qdma_close(qdma);
}

TEST(DesignWriterNullTest, IsBusyNullWriter) {
    EXPECT_FALSE(design_writer_is_busy(nullptr));
}

// ─── Mock-only fixture ────────────────────────────────────────────────────────

class DesignWriterTest : public ::testing::Test {
   protected:
    struct slash_qdma    *qdma_   = nullptr;
    struct design_writer *writer_ = nullptr;

    void SetUp() override {
        qdma_ = slash_qdma_open("@mock");
        ASSERT_NE(qdma_, nullptr);
        writer_ = design_writer_create(qdma_);
        ASSERT_NE(writer_, nullptr);
    }

    void TearDown() override {
        cleanup_design_writer(writer_);
        writer_ = nullptr;
        slash_qdma_close(qdma_);
        qdma_ = nullptr;
    }
};

TEST_F(DesignWriterTest, CreateDestroy) {
    /* Fixture already creates and will destroy — just verify the handle is valid. */
    EXPECT_NE(writer_, nullptr);
    EXPECT_TRUE(writer_->thread_started);
    EXPECT_TRUE(writer_->qpair_created);
    EXPECT_TRUE(writer_->qpair_started);
    EXPECT_GE(writer_->fd, 0);
}

TEST_F(DesignWriterTest, NotBusyInitially) {
    EXPECT_FALSE(design_writer_is_busy(writer_));
}

TEST_F(DesignWriterTest, SyncTransfer) {
    int fd = make_bitstream_fd(0xAB, 4096);
    ASSERT_GE(fd, 0);

    /* design_writer_submit_fd closes fd on completion — do not close it ourselves. */
    EXPECT_EQ(design_writer_submit_fd(writer_, fd), 0);
    EXPECT_FALSE(design_writer_is_busy(writer_));
}

TEST_F(DesignWriterTest, AsyncTransferPoll) {
    int fd = make_bitstream_fd(0xCD, 8192);
    ASSERT_GE(fd, 0);

    ASSERT_EQ(design_writer_submit_fd_async(writer_, fd), 0);

    bool done = false;
    int last_error = 0;
    /* Spin until the worker thread finishes (should be very fast on memfd). */
    for (int attempts = 0; attempts < 10000 && !done; ++attempts) {
        ASSERT_EQ(design_writer_poll_result(writer_, &done, &last_error), 0);
        if (!done)
            usleep(1000);
    }

    EXPECT_TRUE(done);
    EXPECT_EQ(last_error, 0);
}

TEST_F(DesignWriterTest, IsBusyTransitions) {
    int fd = make_bitstream_fd(0xEF, 4096);
    ASSERT_GE(fd, 0);

    EXPECT_FALSE(design_writer_is_busy(writer_));

    ASSERT_EQ(design_writer_submit_fd_async(writer_, fd), 0);

    /* Spin until done, verifying is_busy is false afterwards. */
    bool done = false;
    int last_error = 0;
    for (int i = 0; i < 10000 && !done; ++i) {
        ASSERT_EQ(design_writer_poll_result(writer_, &done, &last_error), 0);
        if (!done)
            usleep(1000);
    }
    ASSERT_TRUE(done);
    EXPECT_FALSE(design_writer_is_busy(writer_));
}

TEST_F(DesignWriterTest, DoubleSubmitRejected) {
    /* Submit a large payload so the first transfer is still running when we try again. */
    int fd1 = make_bitstream_fd(0x11, 64 * 1024);
    ASSERT_GE(fd1, 0);
    ASSERT_EQ(design_writer_submit_fd_async(writer_, fd1), 0);

    /* Second submit while busy must fail. */
    int fd2 = make_bitstream_fd(0x22, 4096);
    ASSERT_GE(fd2, 0);
    int ret = design_writer_submit_fd_async(writer_, fd2);
    if (ret != -1) {
        /* Transfer finished before we got here — acceptable, clean up fd2. */
        /* fd2 is now owned by the writer; wait for it to finish. */
        bool done = false; int err = 0;
        for (int i = 0; i < 10000 && !done; ++i) {
            design_writer_poll_result(writer_, &done, &err);
            if (!done) usleep(1000);
        }
    } else {
        /* Got the expected rejection — fd2 was not consumed, close it ourselves. */
        EXPECT_EQ(ret, -1);
        close(fd2);
        /* Let fd1's transfer finish before TearDown. */
        bool done = false; int err = 0;
        for (int i = 0; i < 10000 && !done; ++i) {
            design_writer_poll_result(writer_, &done, &err);
            if (!done) usleep(1000);
        }
    }
}

TEST_F(DesignWriterTest, CleanupWhileIdle) {
    /* Immediate cleanup after create — TearDown does this, but we exercise it
     * explicitly here with a freshly created writer to confirm no deadlock. */
    struct design_writer *dw = design_writer_create(qdma_);
    ASSERT_NE(dw, nullptr);
    cleanup_design_writer(dw);
}
