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

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <thread>
#include <unistd.h>

#include "sysemu_test_server.h"

extern "C" {
#include <mock_sock.h>
#include <sock_transport.h>

#include <slash/qdma.h>
#include <slash/uapi/slash_interface.h>
#include <slash/uapi/slash_sysemu.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
}

static constexpr const char *DRIVER_QDMA_PATH = "/dev/slash_qdma_ctl0";
static constexpr const char *SYSEMU_QDMA_PATH =
    "/run/slash_sysemu/slash_qdma_ctl0";
static constexpr uint64_t DDR_BASE_ADDRESS = 0x60000000000ULL;

/* --- mock sock tests --- */

TEST(QdmaMockSock, CreateDestroy) {
    int mock_sock = slash_mock_sock_create(SLASH_MOCK_SOCK_ENDPOINT_QDMA);
    ASSERT_GE(mock_sock, 0);

    struct slash_qdma_info info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);

    uint32_t seq = 0;

    int rv = slash_sock_request(mock_sock, SLASH_QDMA_IOCTL_INFO, &info,
                                sizeof(info), NULL, 0, NULL, 0, 0, &seq);

    EXPECT_EQ(rv, 0);
    EXPECT_EQ(info.size, sizeof(info));
    EXPECT_EQ(info.qsets_max, 0);
    EXPECT_EQ(info.vf_max, 0);
    EXPECT_EQ(info.caps, 0);
    EXPECT_STREQ(info.bdf, "0000:61:00.1");

    close(mock_sock);
}

/* ── Helper: count open file descriptors in /proc/self/fd ────────────────────
 */

static int count_open_fds() {
    DIR *d = ::opendir("/proc/self/fd");
    if (!d)
        return -1;
    int n = 0;
    struct dirent *ent;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.')
            continue; /* skip . and .. */
        n++;
    }
    ::closedir(d);
    /* The dirfd itself is included in the count; subtract it. */
    return n - 1;
}

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
    /* Construct a minimal fake handle — we only need errno set by the NULL info
     * check. */
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
    EXPECT_EQ(slash_qdma_qpair_transfer(-1, 4, 0, 0, 4096, SLASH_QDMA_XFER_H2C),
              -1);
    EXPECT_EQ(errno, EINVAL);
}

// ─── Real device tests (requires /dev/slash_qdma_ctl0) ───────────────────────

class ParametrizedQdmaTest : public ::testing::TestWithParam<LibSlashBackend> {
  protected:
    void SetUp() override {
        backend = GetParam();
        if (backend == LibSlashBackend::DRIVER) {
            qdma_ = slash_qdma_open(DRIVER_QDMA_PATH);
            if (!qdma_) {
                GTEST_SKIP() << DRIVER_QDMA_PATH << " not available ("
                             << strerror(errno) << ")";
            }
        } else if (backend == LibSlashBackend::SYSEMU) {
            qdma_ = slash_qdma_open(SYSEMU_QDMA_PATH);
            if (!qdma_) {
                GTEST_SKIP() << SYSEMU_QDMA_PATH << " not available ("
                             << strerror(errno) << ")";
            }
        } else if (backend == LibSlashBackend::MOCK) {
            qdma_ = slash_qdma_open("@mock");
            if (!qdma_) {
                GTEST_FAIL()
                    << "Mock support not available (" << strerror(errno) << ")";
            }
        }
        EXPECT_GE(qdma_->fd, 0);
    }

    void TearDown() override {
        if (qdma_) {
            slash_qdma_close(qdma_);
            qdma_ = nullptr;
        }
    }

    /* Allocate a qpair and return its assigned qid. */
    uint32_t AddQpair() {
        struct slash_qdma_qpair_add add_req{};
        add_req.size = sizeof(add_req);
        add_req.mode = 0;       /* MM */
        add_req.dir_mask = 0x3; /* H2C + C2H */
        EXPECT_EQ(slash_qdma_qpair_add(qdma_, &add_req), 0);
        return add_req.qid;
    }

    /* Create a buffer on the CTL socket; destroys it in TearDown if kept. */
    slash_qdma_buffer MakeBuffer(uint64_t length = 4096) {
        slash_qdma_buffer buf{};
        EXPECT_EQ(slash_qdma_buffer_create(qdma_, length, &buf), 0);
        return buf;
    }

    LibSlashBackend backend;
    struct slash_qdma *qdma_ = nullptr;
};

TEST_P(ParametrizedQdmaTest, TransportMatchesParameter) {
    struct stat st{};
    switch (backend) {
    case LibSlashBackend::DRIVER:
        ASSERT_EQ(qdma_->transport, SLASH_TRANSPORT_IOCTL);
        ASSERT_EQ(::fstat(qdma_->fd, &st), 0);
        ASSERT_TRUE(S_ISCHR(st.st_mode));
        break;
    case LibSlashBackend::SYSEMU:
    case LibSlashBackend::MOCK:
        ASSERT_EQ(qdma_->transport, SLASH_TRANSPORT_SOCKET);
        ASSERT_EQ(::fstat(qdma_->fd, &st), 0);
        ASSERT_TRUE(S_ISSOCK(st.st_mode));
        break;
    default:
        GTEST_FAIL() << "Unknown backend tested!";
    }
}

TEST_P(ParametrizedQdmaTest, InfoRead) {
    struct slash_qdma_info info{};
    EXPECT_EQ(slash_qdma_info_read(qdma_, &info), 0) << strerror(errno);
    if (backend == LibSlashBackend::MOCK) {
        EXPECT_EQ(info.size, sizeof(info));
        EXPECT_EQ(info.qsets_max, 0);
        EXPECT_EQ(info.msix_qvecs, 0);
        EXPECT_EQ(info.vf_max, 0);
        EXPECT_EQ(info.caps, 0);
        EXPECT_STREQ(info.bdf, "0000:61:00.1");
    }
}

/* ── QPAIR lifecycle ────────────────────────────────────────────────────────
 */

TEST_P(ParametrizedQdmaTest, QpairAddReturnsQid) {
    struct slash_qdma_qpair_add add_req{};
    add_req.size = sizeof(add_req);
    add_req.mode = 0;
    add_req.dir_mask = 0x3;
    ASSERT_EQ(slash_qdma_qpair_add(qdma_, &add_req), 0) << strerror(errno);
}

TEST_P(ParametrizedQdmaTest, QpairStartSucceeds) {
    uint32_t qid = AddQpair();
    EXPECT_EQ(slash_qdma_qpair_start(qdma_, qid), 0) << strerror(errno);
}

TEST_P(ParametrizedQdmaTest, QpairStopSucceeds) {
    uint32_t qid = AddQpair();
    EXPECT_EQ(slash_qdma_qpair_start(qdma_, qid), 0) << strerror(errno);
    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0) << strerror(errno);
}

TEST_P(ParametrizedQdmaTest, QpairDelSucceeds) {
    uint32_t qid = AddQpair();
    EXPECT_EQ(slash_qdma_qpair_start(qdma_, qid), 0) << strerror(errno);
    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0) << strerror(errno);
    EXPECT_EQ(slash_qdma_qpair_del(qdma_, qid), 0) << strerror(errno);
}

TEST_P(ParametrizedQdmaTest, QpairAddAcceptsKeyholeAperture) {
    struct slash_qdma_qpair_add req{};
    req.mode = 0;       /* QDMA_Q_MODE_MM */
    req.dir_mask = 0x1; /* H2C */
    req.aperture_size = 4096;

    ASSERT_EQ(slash_qdma_qpair_add(qdma_, &req), 0) << strerror(errno);
    uint32_t qid = req.qid;

    EXPECT_EQ(slash_qdma_qpair_start(qdma_, qid), 0) << strerror(errno);
    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0) << strerror(errno);
    EXPECT_EQ(slash_qdma_qpair_del(qdma_, qid), 0) << strerror(errno);
}

/* --- Buffers
 * -------------------------------------------------------------------- */

TEST_P(ParametrizedQdmaTest, BufCreateViaCtlFD) {
    slash_qdma_buffer buf{};
    ASSERT_EQ(slash_qdma_buffer_create(qdma_, 4096, &buf), 0)
        << strerror(errno);

    EXPECT_GE(buf.fd, 0);
    EXPECT_NE(buf.addr, nullptr);
    EXPECT_EQ(buf.length, 4096u);
    EXPECT_EQ(buf.granule, 4096u);

    // Write to the buffer and read from it, to check that it behaves like a
    // buffer.
    for (size_t i = 0; i < 4096 / sizeof(size_t); i++) {
        static_cast<size_t *>(buf.addr)[i] = i;
    }
    for (size_t i = 0; i < 4096 / sizeof(size_t); i++) {
        EXPECT_EQ(static_cast<size_t *>(buf.addr)[i], i);
    }

    EXPECT_EQ(slash_qdma_buffer_destroy(&buf), 0);
}

TEST_P(ParametrizedQdmaTest, BufCreateViaQpairFD) {
    uint32_t qid = AddQpair();
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);

    int xfer_fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    ASSERT_GE(xfer_fd, 0);

    slash_qdma_buffer buf{};
    ASSERT_EQ(slash_qdma_qpair_buffer_create(xfer_fd, 4096, &buf), 0);

    EXPECT_GE(buf.fd, 0);
    EXPECT_NE(buf.addr, nullptr);
    EXPECT_EQ(buf.length, 4096u);
    EXPECT_EQ(buf.granule, 4096u);

    // Write to the buffer and read from it, to check that it behaves like a
    // buffer.
    for (size_t i = 0; i < 4096 / sizeof(size_t); i++) {
        static_cast<size_t *>(buf.addr)[i] = i;
    }
    for (size_t i = 0; i < 4096 / sizeof(size_t); i++) {
        EXPECT_EQ(static_cast<size_t *>(buf.addr)[i], i);
    }

    EXPECT_EQ(slash_qdma_buffer_destroy(&buf), 0);
    ::close(xfer_fd);
}

/* ── Adversary: qdma_raw_fd_seq __thread isolation across threads ───────────
 */

TEST_P(ParametrizedQdmaTest, RawFdSeqIsPerThreadNotShared) {
    /*
     * Verify that concurrent slash_qdma_qpair_buffer_create calls on
     * DIFFERENT qpair sockets from different threads do not corrupt each
     * other's sequence counters.  Each thread uses its own __thread
     * qdma_raw_fd_seq which should start at 0 per thread, and both threads
     * should independently succeed.
     *
     * The test creates two separate qpair fds (each backed by its own
     * SEQPACKET socketpair in the server) and issues concurrent BUF_CREATE
     * requests from two threads.  Both must succeed and return valid memfds.
     */
    uint32_t qid0 = AddQpair();
    uint32_t qid1 = AddQpair();
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid0), 0);
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid1), 0);

    int xfer_fd0 = slash_qdma_qpair_get_fd(qdma_, qid0, 0);
    int xfer_fd1 = slash_qdma_qpair_get_fd(qdma_, qid1, 0);
    ASSERT_GE(xfer_fd0, 0);
    ASSERT_GE(xfer_fd1, 0);

    slash_qdma_buffer buf0{}, buf1{};
    bool ok0 = false, ok1 = false;

    std::thread t0([&] {
        ok0 = (slash_qdma_qpair_buffer_create(xfer_fd0, 4096, &buf0) == 0);
    });
    std::thread t1([&] {
        ok1 = (slash_qdma_qpair_buffer_create(xfer_fd1, 4096, &buf1) == 0);
    });
    t0.join();
    t1.join();

    EXPECT_TRUE(ok0) << "thread 0 BUF_CREATE failed";
    EXPECT_TRUE(ok1) << "thread 1 BUF_CREATE failed";

    if (ok0) {
        EXPECT_GE(buf0.fd, 0);
        slash_qdma_buffer_destroy(&buf0);
    }
    if (ok1) {
        EXPECT_GE(buf1.fd, 0);
        slash_qdma_buffer_destroy(&buf1);
    }

    ::close(xfer_fd0);
    ::close(xfer_fd1);
}

/* ── End-to-end transfers
 * ──────────────────────────────────────────────────────── */

TEST_P(ParametrizedQdmaTest, QueueDmaTransfer) {
    static constexpr size_t XFER_SIZE = 4096;

    // Add a Memory-Mapped queue pair with both H2C and C2H enabled.
    struct slash_qdma_qpair_add req{};
    req.mode = 0;       /* QDMA_Q_MODE_MM */
    req.dir_mask = 0x3; /* H2C | C2H */
    req.h2c_ring_sz = 0;
    req.c2h_ring_sz = 0;
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

    if (backend == LibSlashBackend::DRIVER) {
        EXPECT_EQ(src_buf.transfer_hint, SLASH_QDMA_TRANSFER_HINT_V80);
        EXPECT_EQ(dst_buf.transfer_hint, SLASH_QDMA_TRANSFER_HINT_V80);
    } else {
        EXPECT_EQ(src_buf.transfer_hint, SLASH_QDMA_TRANSFER_HINT_SINGLE_QPAIR);
        EXPECT_EQ(dst_buf.transfer_hint, SLASH_QDMA_TRANSFER_HINT_SINGLE_QPAIR);
    }
    EXPECT_EQ(src_buf.granule, 4096u);
    EXPECT_EQ(dst_buf.granule, 4096u);

    auto *src = static_cast<uint8_t *>(src_buf.addr);
    auto *dst = static_cast<uint8_t *>(dst_buf.addr);
    for (size_t i = 0; i < XFER_SIZE; ++i) {
        src[i] = static_cast<uint8_t>(i & 0xFF);
    }
    std::memset(dst, 0, XFER_SIZE);

    ssize_t written =
        slash_qdma_qpair_transfer(queue_fd, src_buf.fd, 0, DDR_BASE_ADDRESS,
                                  XFER_SIZE, SLASH_QDMA_XFER_H2C);
    EXPECT_EQ(written, static_cast<ssize_t>(XFER_SIZE));

    // Read back from DDR (C2H) and verify.
    ssize_t read_bytes =
        slash_qdma_qpair_transfer(queue_fd, dst_buf.fd, 0, DDR_BASE_ADDRESS,
                                  XFER_SIZE, SLASH_QDMA_XFER_C2H);
    EXPECT_EQ(read_bytes, static_cast<ssize_t>(XFER_SIZE));
    EXPECT_EQ(std::memcmp(src, dst, XFER_SIZE), 0);

    EXPECT_EQ(slash_qdma_buffer_destroy(&src_buf), 0);
    EXPECT_EQ(slash_qdma_buffer_destroy(&dst_buf), 0);

    EXPECT_EQ(close(queue_fd), 0);

    EXPECT_EQ(slash_qdma_qpair_stop(qdma_, qid), 0);
    EXPECT_EQ(slash_qdma_qpair_del(qdma_, qid), 0);
}

TEST_P(ParametrizedQdmaTest, BufferCreateZeroLength) {
    slash_qdma_buffer buf{};
    EXPECT_EQ(slash_qdma_buffer_create(qdma_, 0, &buf), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST_P(ParametrizedQdmaTest, PartialLengthTransfer) {
    static constexpr size_t BUFFER_SIZE = 4096;
    static constexpr size_t XFER_SIZE = 4096 - 17;

    struct slash_qdma_qpair_add req{};
    req.mode = 0;       /* QDMA_Q_MODE_MM */
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

    ssize_t written =
        slash_qdma_qpair_transfer(queue_fd, src_buf.fd, 0, DDR_BASE_ADDRESS,
                                  XFER_SIZE, SLASH_QDMA_XFER_H2C);
    EXPECT_EQ(written, static_cast<ssize_t>(XFER_SIZE));

    ssize_t read_bytes =
        slash_qdma_qpair_transfer(queue_fd, dst_buf.fd, 0, DDR_BASE_ADDRESS,
                                  XFER_SIZE, SLASH_QDMA_XFER_C2H);
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
        req.mode = 0;       /* QDMA_Q_MODE_MM */
        req.dir_mask = 0x3; /* H2C | C2H */
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
    h2c[1] = {1,    SLASH_QDMA_XFER_H2C,     src_buf.fd, 0,
              HALF, DDR_BASE_ADDRESS + HALF, HALF};
    EXPECT_EQ(slash_qdma_qpair_transfer_batch(fd, h2c, 2),
              static_cast<ssize_t>(XFER_SIZE));

    // C2H: read both halves back across both channels in one ioctl.
    struct slash_qdma_subxfer c2h[2]{};
    c2h[0] = {0, SLASH_QDMA_XFER_C2H, dst_buf.fd, 0, 0, DDR_BASE_ADDRESS, HALF};
    c2h[1] = {1,    SLASH_QDMA_XFER_C2H,     dst_buf.fd, 0,
              HALF, DDR_BASE_ADDRESS + HALF, HALF};
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
    if (backend != LibSlashBackend::DRIVER) {
        GTEST_SKIP()
            << "qpair fds returned by sysemu/mock are unix domain sockets "
               "and thus support read/write for socket messages";
    }

    struct slash_qdma_qpair_add req{};
    req.mode = 0;
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

INSTANTIATE_TEST_SUITE_P(QdmaTest, ParametrizedQdmaTest,
                         testing::Values(LibSlashBackend::DRIVER,
                                         LibSlashBackend::SYSEMU,
                                         LibSlashBackend::MOCK),
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

/* ── QDMA sysemu transport tests
 * ────────────────────────────────────────────────── */

class QdmaSocketTransportTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(srv_.Start())
            << "server failed to start: " << std::strerror(errno);
        qdma_ = slash_qdma_open(srv_.Path().c_str());
        ASSERT_NE(qdma_, nullptr)
            << "slash_qdma_open failed: " << std::strerror(errno);
        ASSERT_EQ(qdma_->transport, SLASH_TRANSPORT_SOCKET);
        ASSERT_EQ(qdma_->fd >= 0, true);
    }

    void TearDown() override {
        if (qdma_) {
            slash_qdma_close(qdma_);
            qdma_ = nullptr;
        }
        srv_.Stop();
    }

    /* Allocate a qpair and return its assigned qid. */
    uint32_t AddQpair() {
        struct slash_qdma_qpair_add add_req{};
        add_req.size = sizeof(add_req);
        add_req.mode = 0;       /* MM */
        add_req.dir_mask = 0x3; /* H2C + C2H */
        EXPECT_EQ(slash_qdma_qpair_add(qdma_, &add_req), 0);
        return add_req.qid;
    }

    /* Create a buffer on the CTL socket; destroys it in TearDown if kept. */
    slash_qdma_buffer MakeBuffer(uint64_t length = 4096) {
        slash_qdma_buffer buf{};
        EXPECT_EQ(slash_qdma_buffer_create(qdma_, length, &buf), 0);
        return buf;
    }

    SysemuTestServer srv_;
    struct slash_qdma *qdma_{nullptr};
};

/* ── INFO ───────────────────────────────────────────────────────────────────
 */

TEST_F(QdmaSocketTransportTest, InfoReturnsCapabilities) {
    slash_qdma_info info{};
    info.size = sizeof(info);
    ASSERT_EQ(slash_qdma_info_read(qdma_, &info), 0);
    EXPECT_EQ(info.qsets_max, srv_.qdma_qsets_max);
    EXPECT_EQ(info.msix_qvecs, srv_.qdma_msix_qvecs);
    EXPECT_EQ(info.vf_max, srv_.qdma_vf_max);
}

TEST_F(QdmaSocketTransportTest, InfoBdfForwarded) {
    srv_.qdma_bdf = "0000:61:00.1";
    slash_qdma_info info{};
    info.size = sizeof(info);
    ASSERT_EQ(slash_qdma_info_read(qdma_, &info), 0);
    EXPECT_STREQ(info.bdf, "0000:61:00.1");
}

/* ── ENODEV mapping on transport failures ───────────────────────────────────
 */

TEST_F(QdmaSocketTransportTest, EnodevOnPeerClose) {
    srv_.InjectFault(SysemuFault::PeerClose);
    slash_qdma_info info{};
    info.size = sizeof(info);
    EXPECT_EQ(slash_qdma_info_read(qdma_, &info), -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(QdmaSocketTransportTest, EnodevOnTruncatedReply) {
    srv_.InjectFault(SysemuFault::TruncatedReply);
    slash_qdma_info info{};
    info.size = sizeof(info);
    EXPECT_EQ(slash_qdma_info_read(qdma_, &info), -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(QdmaSocketTransportTest, EnodevOnWrongSeq) {
    srv_.InjectFault(SysemuFault::WrongSeq);
    slash_qdma_info info{};
    info.size = sizeof(info);
    EXPECT_EQ(slash_qdma_info_read(qdma_, &info), -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(QdmaSocketTransportTest, EnodevOnWrongOp) {
    srv_.InjectFault(SysemuFault::WrongOp);
    slash_qdma_info info{};
    info.size = sizeof(info);
    EXPECT_EQ(slash_qdma_info_read(qdma_, &info), -1);
    EXPECT_EQ(errno, ENODEV);
}

/* ── QPAIR_GET_FD ───────────────────────────────────────────────────────────
 */

TEST_F(QdmaSocketTransportTest, QpairGetFdReturnsSocketFd) {
    uint32_t qid = AddQpair();
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);

    int xfer_fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    ASSERT_GE(xfer_fd, 0) << "get_fd failed: " << std::strerror(errno);

    /* The returned fd must be a socket (XFER endpoint). */
    struct stat st{};
    ASSERT_EQ(::fstat(xfer_fd, &st), 0);
    EXPECT_TRUE(S_ISSOCK(st.st_mode));

    ::close(xfer_fd);
}

TEST_F(QdmaSocketTransportTest, QpairGetFdMultiReturnsTwoQpairs) {
    uint32_t qids[2];
    qids[0] = AddQpair();
    qids[1] = AddQpair();
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qids[0]), 0);
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qids[1]), 0);

    int xfer_fd = slash_qdma_qpair_get_fd_multi(qdma_, qids, 2, 0);
    ASSERT_GE(xfer_fd, 0) << "get_fd_multi failed: " << std::strerror(errno);

    struct stat st{};
    ASSERT_EQ(::fstat(xfer_fd, &st), 0);
    EXPECT_TRUE(S_ISSOCK(st.st_mode));

    ::close(xfer_fd);
}

/* ── BUF_CREATE (CTL socket) ────────────────────────────────────────────────
 */

TEST_F(QdmaSocketTransportTest, BufCreateDaemonErrorMapsErrno) {
    srv_.InjectFault(SysemuFault::DaemonError, ENOMEM);
    slash_qdma_buffer buf{};
    int ret = slash_qdma_buffer_create(qdma_, 4096, &buf);
    srv_.ClearFault();
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, ENOMEM);
}

/* ── fd-leak audits ─────────────────────────────────────────────────────────
 */

TEST_F(QdmaSocketTransportTest, FdLeakAuditBufCreateOnDaemonError) {
    /*
     * When BUF_CREATE returns a DaemonError (-ENOMEM), the call must fail
     * cleanly with errno=ENOMEM.  No fd should be returned or left open.
     * We verify by checking buf.fd is not set and errno is correct.
     * (A process-global fd count would race with other parallel tests.)
     */
    for (int i = 0; i < 8; ++i) {
        srv_.InjectFault(SysemuFault::DaemonError, ENOMEM);
        slash_qdma_buffer buf{};
        buf.fd = -999; /* sentinel */
        int ret = slash_qdma_buffer_create(qdma_, 4096, &buf);
        srv_.ClearFault();
        EXPECT_EQ(ret, -1);
        EXPECT_EQ(errno, ENOMEM);
        /* buf.fd must not have been populated with a valid fd */
        EXPECT_EQ(buf.fd, -999);
    }
}

TEST_F(QdmaSocketTransportTest, FdLeakAuditBufCreateDaemonErrorRepeated) {
    /*
     * Repeated BUF_CREATE DaemonError calls: each must fail without leaking
     * the CTL socket or leaving the buf in an inconsistent state.  After all
     * iterations the CTL socket is still usable (verify with INFO).
     */
    for (int i = 0; i < 16; ++i) {
        srv_.InjectFault(SysemuFault::DaemonError, ENOMEM);
        slash_qdma_buffer buf{};
        buf.fd = -1;
        EXPECT_EQ(slash_qdma_buffer_create(qdma_, 4096, &buf), -1);
        EXPECT_EQ(errno, ENOMEM);
        srv_.ClearFault();
        EXPECT_EQ(buf.fd, -1);
    }

    /* CTL socket must still be alive after all the error-path calls. */
    slash_qdma_info info{};
    info.size = sizeof(info);
    EXPECT_EQ(slash_qdma_info_read(qdma_, &info), 0);
}

/* ── Null / bad-arg guard tests ─────────────────────────────────────────────
 */

TEST_F(QdmaSocketTransportTest, InfoReadNullInfoPtr) {
    EXPECT_EQ(slash_qdma_info_read(qdma_, nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(QdmaSocketTransportTest, QpairAddNullReq) {
    EXPECT_EQ(slash_qdma_qpair_add(qdma_, nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(QdmaSocketTransportTest, BufCreateZeroLength) {
    slash_qdma_buffer buf{};
    EXPECT_EQ(slash_qdma_buffer_create(qdma_, 0, &buf), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(QdmaSocketTransportTest, BufCreateNullBufOut) {
    EXPECT_EQ(slash_qdma_buffer_create(qdma_, 4096, nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST_F(QdmaSocketTransportTest, TransferBatchNullXfers) {
    uint32_t qid = AddQpair();
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);
    int xfer_fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    ASSERT_GE(xfer_fd, 0);

    EXPECT_EQ(slash_qdma_qpair_transfer_batch(xfer_fd, nullptr, 1), -1);
    EXPECT_EQ(errno, EINVAL);

    ::close(xfer_fd);
}

TEST_F(QdmaSocketTransportTest, TransferBatchZeroCount) {
    uint32_t qid = AddQpair();
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);
    int xfer_fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    ASSERT_GE(xfer_fd, 0);

    struct slash_qdma_subxfer dummy{};
    dummy.direction = SLASH_QDMA_XFER_H2C;
    EXPECT_EQ(slash_qdma_qpair_transfer_batch(xfer_fd, &dummy, 0), -1);
    EXPECT_EQ(errno, EINVAL);

    ::close(xfer_fd);
}

/* ── Adversary: QPAIR_GET_FD ENODEV on all transport fault paths ────────────
 */

TEST_F(QdmaSocketTransportTest, QpairGetFdEnodevOnPeerClose) {
    uint32_t qid = AddQpair();
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);

    srv_.InjectFault(SysemuFault::PeerClose);
    errno = 0;
    int fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    srv_.ClearFault();

    EXPECT_EQ(fd, -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(QdmaSocketTransportTest, QpairGetFdEnodevOnWrongSeq) {
    uint32_t qid = AddQpair();
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);

    srv_.InjectFault(SysemuFault::WrongSeq);
    errno = 0;
    int fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    srv_.ClearFault();

    EXPECT_EQ(fd, -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(QdmaSocketTransportTest, QpairGetFdEnodevOnWrongOp) {
    uint32_t qid = AddQpair();
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qid), 0);

    srv_.InjectFault(SysemuFault::WrongOp);
    errno = 0;
    int fd = slash_qdma_qpair_get_fd(qdma_, qid, 0);
    srv_.ClearFault();

    EXPECT_EQ(fd, -1);
    EXPECT_EQ(errno, ENODEV);
}

TEST_F(QdmaSocketTransportTest, QpairGetFdMultiEnodevOnPeerClose) {
    uint32_t qids[2];
    qids[0] = AddQpair();
    qids[1] = AddQpair();
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qids[0]), 0);
    ASSERT_EQ(slash_qdma_qpair_start(qdma_, qids[1]), 0);

    srv_.InjectFault(SysemuFault::PeerClose);
    errno = 0;
    int fd = slash_qdma_qpair_get_fd_multi(qdma_, qids, 2, 0);
    srv_.ClearFault();

    EXPECT_EQ(fd, -1);
    EXPECT_EQ(errno, ENODEV);
}

/* ── Adversary: QPAIR_GET_FD fd-leak when daemon returns error + fd ────────
 *
 * This test exercises the confirmed bug in slash_qdma_qpair_get_fd (and
 * _multi): when the daemon returns a negative return_value (daemon error) but
 * simultaneously sends an fd via SCM_RIGHTS, the client must close that fd.
 * Without the fix, it is leaked.
 *
 * We bypass SysemuTestServer and speak the wire protocol directly, because
 * the test server's DaemonError fault does not attach an fd to the error reply.
 * ── */

namespace {

/*
 * Raw server that:
 *   1. accepts one SEQPACKET connection
 *   2. receives the client's QPAIR_GET_FD request
 *   3. replies with return_value = -EINVAL (an error) but also attaches a
 *      memfd via SCM_RIGHTS — a misbehaving daemon scenario.
 *
 * The memfd sent to the client must not be left open in the client process
 * after slash_qdma_qpair_get_fd returns -1.
 */
static void raw_qpair_fd_error_server(int listen_fd, int dummy_fd_to_send) {
    /* Accept the connection. */
    int conn = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (conn < 0)
        return;

    /* Drain the client request. */
    char drain_buf[4096];
    struct iovec drain_iov{};
    drain_iov.iov_base = drain_buf;
    drain_iov.iov_len = sizeof(drain_buf);
    struct msghdr drain_msg{};
    drain_msg.msg_iov = &drain_iov;
    drain_msg.msg_iovlen = 1;
    slash_sysemu_socket_header req_hdr{};
    ssize_t n = ::recvmsg(conn, &drain_msg, 0);
    if (n >= static_cast<ssize_t>(sizeof(req_hdr))) {
        std::memcpy(&req_hdr, drain_buf, sizeof(req_hdr));
    }

    /* Build an error response that also carries an fd. */
    slash_sysemu_socket_header resp{};
    resp.ioctl_op = req_hdr.ioctl_op;
    resp.sequence_id = req_hdr.sequence_id;
    resp.return_value = static_cast<uint32_t>(-EINVAL);
    resp.pad = 0;

    struct iovec iov{};
    iov.iov_base = &resp;
    iov.iov_len = sizeof(resp);

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    std::memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = static_cast<socklen_t>(sizeof(cmsg_buf));

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = static_cast<socklen_t>(CMSG_LEN(sizeof(int)));
    std::memcpy(CMSG_DATA(cmsg), &dummy_fd_to_send, sizeof(dummy_fd_to_send));

    ::sendmsg(conn, &msg, MSG_NOSIGNAL);
    ::close(conn);
}

} /* namespace */

TEST_F(QdmaSocketTransportTest, QpairGetFdNoFdLeakOnDaemonErrorWithFd) {
    /*
     * Adversary probe for the confirmed fd-leak bug:
     *   slash_qdma_qpair_get_fd() returns -1 when rv < 0 but does NOT close
     *   the recv_fd if the daemon sent one alongside the error response.
     *
     * Steps:
     *   1. Bind a raw listening socket (not SysemuTestServer).
     *   2. slash_qdma_open() connects to it, establishing the CTL socket.
     *   3. On the server side, respond to the QPAIR_GET_FD request with
     *      return_value = -EINVAL AND an attached memfd (misbehaving daemon).
     *   4. slash_qdma_qpair_get_fd() must return -1/EINVAL.
     *   5. No fd leak: /proc/self/fd count must not grow.
     *
     * This test FAILS before the fix and PASSES after.
     */

    /* Create a temp socket path. */
    char tmpl[] = "/tmp/slash_adv_qpairfd_XXXXXX";
    int tmp = ::mkstemp(tmpl);
    ASSERT_GE(tmp, 0);
    ::close(tmp);
    ::unlink(tmpl);

    int server_listen = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(server_listen, 0);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, tmpl, sizeof(addr.sun_path) - 1);
    ASSERT_EQ(::bind(server_listen, reinterpret_cast<struct sockaddr *>(&addr),
                     sizeof(addr)),
              0);
    ASSERT_EQ(::listen(server_listen, 2), 0);

    /* Open a QDMA handle connecting to our raw socket. */
    struct slash_qdma *qdma = slash_qdma_open(tmpl);
    ASSERT_NE(qdma, nullptr) << "slash_qdma_open: " << std::strerror(errno);
    ASSERT_EQ(qdma->transport, SLASH_TRANSPORT_SOCKET);

    /*
     * The server must first handle the slash_qdma_open() connection
     * then handle QPAIR_ADD (if called) then QPAIR_GET_FD.
     * For simplicity we skip QPAIR_ADD: the server just needs to handle
     * whatever QPAIR_GET_FD we send.
     *
     * Actually: slash_qdma_open just connects; it sends no messages until
     * an operation is called.  So we accept once (for the open), then handle
     * the QPAIR_GET_FD.
     *
     * Use a memfd as the "attached fd" the misbehaving daemon sends.
     */
    int leaked_memfd = ::memfd_create("adv_test_leak", MFD_CLOEXEC);
    ASSERT_GE(leaked_memfd, 0);

    /* Baseline fd count (after all allocations above). */
    int fd_baseline = count_open_fds();

    /*
     * Server thread: accept the slash_qdma_open() connection, drain the
     * QPAIR_GET_FD request, and reply with return_value=-EINVAL plus an fd
     * attached via SCM_RIGHTS.  The client should close that fd on rv<0;
     * if it does not, fd_after > fd_baseline.
     *
     * slash_qdma_open() connects immediately; the connection sits in the
     * listen backlog.  The server accepts it, then waits for the first
     * request (sent when slash_qdma_qpair_get_fd() is called).
     */
    std::thread server([&] {
        int conn = ::accept4(server_listen, nullptr, nullptr, SOCK_CLOEXEC);
        if (conn < 0)
            return;

        /* Drain the QPAIR_GET_FD request. */
        char dbuf[4096];
        struct iovec diov{};
        diov.iov_base = dbuf;
        diov.iov_len = sizeof(dbuf);
        struct msghdr dmsg{};
        dmsg.msg_iov = &diov;
        dmsg.msg_iovlen = 1;
        slash_sysemu_socket_header rh{};
        ssize_t rn = ::recvmsg(conn, &dmsg, 0);
        if (rn >= static_cast<ssize_t>(sizeof(rh))) {
            std::memcpy(&rh, dbuf, sizeof(rh));
        }

        /* Reply with -EINVAL + the leaked_memfd attached. */
        slash_sysemu_socket_header resp{};
        resp.ioctl_op = rh.ioctl_op;
        resp.sequence_id = rh.sequence_id;
        resp.return_value = static_cast<uint32_t>(-EINVAL);
        resp.pad = 0;

        struct iovec iov{};
        iov.iov_base = &resp;
        iov.iov_len = sizeof(resp);

        char cbuf[CMSG_SPACE(sizeof(int))];
        std::memset(cbuf, 0, sizeof(cbuf));

        struct msghdr smsg{};
        smsg.msg_iov = &iov;
        smsg.msg_iovlen = 1;
        smsg.msg_control = cbuf;
        smsg.msg_controllen = static_cast<socklen_t>(sizeof(cbuf));

        struct cmsghdr *sc = CMSG_FIRSTHDR(&smsg);
        sc->cmsg_level = SOL_SOCKET;
        sc->cmsg_type = SCM_RIGHTS;
        sc->cmsg_len = static_cast<socklen_t>(CMSG_LEN(sizeof(int)));
        std::memcpy(CMSG_DATA(sc), &leaked_memfd, sizeof(leaked_memfd));

        ::sendmsg(conn, &smsg, MSG_NOSIGNAL);
        ::close(conn);
    });

    /* Client: call QPAIR_GET_FD — the raw server will reply with -EINVAL+fd. */
    errno = 0;
    int xfd = slash_qdma_qpair_get_fd(qdma, 0, 0);

    server.join();

    /* Must fail with EINVAL. */
    EXPECT_EQ(xfd, -1);
    EXPECT_EQ(errno, EINVAL);

    /*
     * fd count must NOT have grown: the transferred copy of leaked_memfd
     * must have been closed by slash_qdma_qpair_get_fd.
     *
     * If the bug is present (fd not closed on rv < 0), fd_after > fd_baseline.
     */
    int fd_after = count_open_fds();
    EXPECT_EQ(fd_after, fd_baseline)
        << "fd leak detected: slash_qdma_qpair_get_fd did not close the fd "
           "received from a misbehaving daemon error response";

    slash_qdma_close(qdma);
    ::close(leaked_memfd);
    ::close(server_listen);
    ::unlink(tmpl);
}

/* ── Adversary: QPAIR_GET_FD_MULTI fd-leak on daemon error with fd ──────────
 */

TEST_F(QdmaSocketTransportTest, QpairGetFdMultiNoFdLeakOnDaemonErrorWithFd) {
    /*
     * Same probe as QpairGetFdNoFdLeakOnDaemonErrorWithFd but exercising
     * slash_qdma_qpair_get_fd_multi, which has the identical bug at line
     * 634-636 of qdma.c.
     */
    char tmpl[] = "/tmp/slash_adv_qpairfd_multi_XXXXXX";
    int tmp = ::mkstemp(tmpl);
    ASSERT_GE(tmp, 0);
    ::close(tmp);
    ::unlink(tmpl);

    int server_listen = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(server_listen, 0);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, tmpl, sizeof(addr.sun_path) - 1);
    ASSERT_EQ(::bind(server_listen, reinterpret_cast<struct sockaddr *>(&addr),
                     sizeof(addr)),
              0);
    ASSERT_EQ(::listen(server_listen, 2), 0);

    struct slash_qdma *qdma = slash_qdma_open(tmpl);
    ASSERT_NE(qdma, nullptr);
    ASSERT_EQ(qdma->transport, SLASH_TRANSPORT_SOCKET);

    int leaked_memfd = ::memfd_create("adv_multi_leak", MFD_CLOEXEC);
    ASSERT_GE(leaked_memfd, 0);

    int fd_baseline = count_open_fds();

    std::thread server_thread([&] {
        int conn = ::accept4(server_listen, nullptr, nullptr, SOCK_CLOEXEC);
        if (conn < 0)
            return;

        char dbuf[4096];
        struct iovec diov{};
        diov.iov_base = dbuf;
        diov.iov_len = sizeof(dbuf);
        struct msghdr dmsg{};
        dmsg.msg_iov = &diov;
        dmsg.msg_iovlen = 1;
        slash_sysemu_socket_header rh{};
        ssize_t rn = ::recvmsg(conn, &dmsg, 0);
        if (rn >= static_cast<ssize_t>(sizeof(rh))) {
            std::memcpy(&rh, dbuf, sizeof(rh));
        }

        slash_sysemu_socket_header resp{};
        resp.ioctl_op = rh.ioctl_op;
        resp.sequence_id = rh.sequence_id;
        resp.return_value = static_cast<uint32_t>(-ENODEV);
        resp.pad = 0;

        struct iovec iov2{};
        iov2.iov_base = &resp;
        iov2.iov_len = sizeof(resp);

        char cbuf[CMSG_SPACE(sizeof(int))];
        std::memset(cbuf, 0, sizeof(cbuf));

        struct msghdr smsg{};
        smsg.msg_iov = &iov2;
        smsg.msg_iovlen = 1;
        smsg.msg_control = cbuf;
        smsg.msg_controllen = static_cast<socklen_t>(sizeof(cbuf));

        struct cmsghdr *sc = CMSG_FIRSTHDR(&smsg);
        sc->cmsg_level = SOL_SOCKET;
        sc->cmsg_type = SCM_RIGHTS;
        sc->cmsg_len = static_cast<socklen_t>(CMSG_LEN(sizeof(int)));
        std::memcpy(CMSG_DATA(sc), &leaked_memfd, sizeof(leaked_memfd));

        ::sendmsg(conn, &smsg, MSG_NOSIGNAL);
        ::close(conn);
    });

    uint32_t qids[1] = {0};
    errno = 0;
    int xfd = slash_qdma_qpair_get_fd_multi(qdma, qids, 1, 0);

    server_thread.join();

    EXPECT_EQ(xfd, -1);
    /* errno may be ENODEV or EINVAL depending on the daemon's response */
    EXPECT_NE(errno, 0);

    int fd_after = count_open_fds();
    EXPECT_EQ(fd_after, fd_baseline)
        << "fd leak: slash_qdma_qpair_get_fd_multi did not close the fd "
           "received from a misbehaving daemon";

    slash_qdma_close(qdma);
    ::close(leaked_memfd);
    ::close(server_listen);
    ::unlink(tmpl);
}