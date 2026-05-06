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

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <vector>

extern "C" {
#include <slash/qdma.h>
#include "allocator.h"
#include "buffer.h"
}

static constexpr const char    *REAL_QDMA_PATH   = "/dev/slash_qdma_ctl0";
static constexpr uint64_t       XFER_SIZE        = 4096;
static constexpr uint64_t       CLIENT_ID        = 42;

// ─── Null / argument validation (no hardware needed, always run) ──────────────

TEST(BufferNullTest, NullQdma) {
    struct device_memory_map *map = device_memory_map_create();
    ASSERT_NE(map, nullptr);
    struct buffer *buf = buffer_create(nullptr, map, ALLOCATION_TYPE_DDR,
                                       VRTD_ALLOC_DIR_HOST_TO_DEVICE,
                                       XFER_SIZE, 0, CLIENT_ID, nullptr);
    EXPECT_EQ(buf, nullptr);
    device_memory_map_cleanup(map);
}

TEST(BufferNullTest, NullMap) {
    struct slash_qdma *qdma = slash_qdma_open("@mock");
    ASSERT_NE(qdma, nullptr);
    struct buffer *buf = buffer_create(qdma, nullptr, ALLOCATION_TYPE_DDR,
                                       VRTD_ALLOC_DIR_HOST_TO_DEVICE,
                                       XFER_SIZE, 0, CLIENT_ID, nullptr);
    EXPECT_EQ(buf, nullptr);
    slash_qdma_close(qdma);
}

TEST(BufferNullTest, ZeroSize) {
    struct slash_qdma *qdma = slash_qdma_open("@mock");
    ASSERT_NE(qdma, nullptr);
    struct device_memory_map *map = device_memory_map_create();
    ASSERT_NE(map, nullptr);
    struct buffer *buf = buffer_create(qdma, map, ALLOCATION_TYPE_DDR,
                                       VRTD_ALLOC_DIR_HOST_TO_DEVICE,
                                       0, 0, CLIENT_ID, nullptr);
    EXPECT_EQ(buf, nullptr);
    device_memory_map_cleanup(map);
    slash_qdma_close(qdma);
}

TEST(BufferNullTest, ZeroClientId) {
    struct slash_qdma *qdma = slash_qdma_open("@mock");
    ASSERT_NE(qdma, nullptr);
    struct device_memory_map *map = device_memory_map_create();
    ASSERT_NE(map, nullptr);
    struct buffer *buf = buffer_create(qdma, map, ALLOCATION_TYPE_DDR,
                                       VRTD_ALLOC_DIR_HOST_TO_DEVICE,
                                       XFER_SIZE, 0, 0, nullptr);
    EXPECT_EQ(buf, nullptr);
    device_memory_map_cleanup(map);
    slash_qdma_close(qdma);
}

TEST(BufferNullTest, InvalidDirection) {
    struct slash_qdma *qdma = slash_qdma_open("@mock");
    ASSERT_NE(qdma, nullptr);
    struct device_memory_map *map = device_memory_map_create();
    ASSERT_NE(map, nullptr);
    struct buffer *buf = buffer_create(qdma, map, ALLOCATION_TYPE_DDR,
                                       static_cast<vrtd_alloc_dir>(99),
                                       XFER_SIZE, 0, CLIENT_ID, nullptr);
    EXPECT_EQ(buf, nullptr);
    device_memory_map_cleanup(map);
    slash_qdma_close(qdma);
}

TEST(BufferNullTest, CleanupNull) {
    cleanup_buffer(nullptr);
}

TEST(BufferNullTest, RawNullQdma) {
    struct buffer *buf = buffer_create_raw(nullptr, DDR_START_ADDRESS, XFER_SIZE,
                                           VRTD_ALLOC_DIR_HOST_TO_DEVICE);
    EXPECT_EQ(buf, nullptr);
    EXPECT_EQ(errno, EINVAL);
}

TEST(BufferNullTest, RawZeroSize) {
    struct slash_qdma *qdma = slash_qdma_open("@mock");
    ASSERT_NE(qdma, nullptr);
    struct buffer *buf = buffer_create_raw(qdma, DDR_START_ADDRESS, 0,
                                           VRTD_ALLOC_DIR_HOST_TO_DEVICE);
    EXPECT_EQ(buf, nullptr);
    EXPECT_EQ(errno, EINVAL);
    slash_qdma_close(qdma);
}

// ─── Parameterized fixture (mock + real hardware) ────────────────────────────

class BufferTest : public ::testing::TestWithParam<bool> {
   protected:
    bool mock;
    struct slash_qdma         *qdma_ = nullptr;
    struct device_memory_map  *map_  = nullptr;

    void SetUp() override {
        mock = GetParam();
        if (mock) {
            qdma_ = slash_qdma_open("@mock");
            ASSERT_NE(qdma_, nullptr);
        } else {
            qdma_ = slash_qdma_open(REAL_QDMA_PATH);
            if (qdma_ == nullptr) {
                GTEST_SKIP() << REAL_QDMA_PATH << " not available (errno=" << errno << ")";
            }
        }
        map_ = device_memory_map_create();
        ASSERT_NE(map_, nullptr);
    }

    void TearDown() override {
        device_memory_map_cleanup(map_);
        map_ = nullptr;
        if (qdma_) {
            slash_qdma_close(qdma_);
            qdma_ = nullptr;
        }
    }
};

TEST_P(BufferTest, LifecycleBidirectional) {
    struct buffer *buf = buffer_create(qdma_, map_, ALLOCATION_TYPE_DDR,
                                       VRTD_ALLOC_DIR_BIDIRECTIONAL,
                                       XFER_SIZE, 0, CLIENT_ID, nullptr);
    ASSERT_NE(buf, nullptr);
    EXPECT_GE(buf->fd, 0);

    uint8_t src[XFER_SIZE];
    for (size_t i = 0; i < XFER_SIZE; ++i)
        src[i] = static_cast<uint8_t>(i & 0xFF);

    ssize_t written = pwrite(buf->fd, src, XFER_SIZE, static_cast<off_t>(buf->addr));
    EXPECT_EQ(written, static_cast<ssize_t>(XFER_SIZE));

    uint8_t dst[XFER_SIZE]{};
    ssize_t read_bytes = pread(buf->fd, dst, XFER_SIZE, static_cast<off_t>(buf->addr));
    EXPECT_EQ(read_bytes, static_cast<ssize_t>(XFER_SIZE));
    EXPECT_EQ(std::memcmp(src, dst, XFER_SIZE), 0);

    cleanup_buffer(buf);
}

TEST_P(BufferTest, RawCreateAndIO) {
    struct buffer *buf = buffer_create_raw(qdma_, DDR_START_ADDRESS, XFER_SIZE,
                                           VRTD_ALLOC_DIR_BIDIRECTIONAL);
    ASSERT_NE(buf, nullptr);
    EXPECT_GE(buf->fd, 0);
    EXPECT_EQ(buf->addr, DDR_START_ADDRESS);
    EXPECT_FALSE(buf->allocation_valid);

    uint8_t src[XFER_SIZE];
    std::memset(src, 0xCD, sizeof(src));
    ssize_t written = pwrite(buf->fd, src, XFER_SIZE, static_cast<off_t>(DDR_START_ADDRESS));
    EXPECT_EQ(written, static_cast<ssize_t>(XFER_SIZE));

    uint8_t dst[XFER_SIZE]{};
    ssize_t n = pread(buf->fd, dst, XFER_SIZE, static_cast<off_t>(DDR_START_ADDRESS));
    EXPECT_EQ(n, static_cast<ssize_t>(XFER_SIZE));
    EXPECT_EQ(std::memcmp(src, dst, XFER_SIZE), 0);

    cleanup_buffer(buf);
}

TEST_P(BufferTest, QueueExhaustion) {
    /* The mock QDMA supports 64 queues (QDMA_MOCK_MAX_QUEUES).
     * Real hardware queue limits vary and exhaustion may not be reachable,
     * so this test is restricted to mock mode. */
    if (!mock) {
        GTEST_SKIP() << "Queue exhaustion test is mock-only";
    }

    static constexpr int MAX_QUEUES = 64;
    std::vector<struct buffer *> bufs;
    bufs.reserve(MAX_QUEUES);

    for (int i = 0; i < MAX_QUEUES; ++i) {
        struct buffer *buf = buffer_create_raw(qdma_, DDR_START_ADDRESS + i * XFER_SIZE,
                                               XFER_SIZE, VRTD_ALLOC_DIR_HOST_TO_DEVICE);
        ASSERT_NE(buf, nullptr) << "Expected success for queue " << i;
        bufs.push_back(buf);
    }

    /* 65th allocation must fail */
    struct buffer *overflow = buffer_create_raw(qdma_, DDR_START_ADDRESS,
                                                XFER_SIZE, VRTD_ALLOC_DIR_HOST_TO_DEVICE);
    EXPECT_EQ(overflow, nullptr);
    EXPECT_EQ(errno, ENOSPC);

    for (struct buffer *b : bufs)
        cleanup_buffer(b);
}

INSTANTIATE_TEST_SUITE_P(BufferTest, BufferTest, testing::Values(true, false),
    [](const testing::TestParamInfo<bool> &info) {
        return info.param ? "Mock" : "RealHardware";
    });
