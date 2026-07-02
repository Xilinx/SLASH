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

#include <vrt/qdma/qdma_intf.hpp>

#include <cstring>

#include <slash/qdma.h>
#include <vrtd/device.hpp>

namespace {
constexpr uint32_t kQdmaModeSt = 1u;
constexpr uint32_t kQdmaDirH2C = 1u << 0;
constexpr uint32_t kQdmaDirC2H = 1u << 1;
constexpr uint32_t kQdmaRingSzIdx = 0u;
}

namespace vrt {

QdmaIntf::QdmaIntf(const vrtd::Device& device, const uint32_t queueIdx, StreamDirection direction)
    : queueIdx(queueIdx) {
    struct slash_qdma_qpair_add qpair_cfg = {0};
    qpair_cfg.size = sizeof(qpair_cfg);
    qpair_cfg.mode = kQdmaModeSt;
    qpair_cfg.h2c_ring_sz = kQdmaRingSzIdx;
    qpair_cfg.c2h_ring_sz = kQdmaRingSzIdx;
    qpair_cfg.cmpt_ring_sz = kQdmaRingSzIdx;
    qpair_cfg.dir_mask = (direction == StreamDirection::HOST_TO_DEVICE)
        ? kQdmaDirH2C
        : kQdmaDirC2H;

    qpair = device.createQdmaQpair(qpair_cfg);
    qpair->start();
    qpairFd = qpair->fd(O_CLOEXEC);
}

QdmaIntf::~QdmaIntf() {
    if (qpairFd >= 0) {
        close(qpairFd);
        qpairFd = -1;
    }
}

namespace {
constexpr uint64_t kQdmaPage = 4096ULL;
inline uint64_t roundUpToPage(uint64_t v) { return (v + kQdmaPage - 1) & ~(kQdmaPage - 1); }
}  // namespace

ssize_t QdmaIntf::write_from_buffer(const char* fname, char* buffer, uint64_t size, uint64_t base) {
    if (qpairFd < 0) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                           "QDMA streaming not initialized");
        return -EIO;
    }
    if (size == 0) {
        return 0;
    }

    // The kernel buffer owns its DMA-mapped pages; stage the caller's data into
    // the mapping, then transfer whole pages.
    const uint64_t aligned = roundUpToPage(size);
    struct slash_qdma_buffer buf{};
    if (slash_qdma_qpair_buffer_create(qpairFd, aligned, &buf) != 0) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                           "Could not create QDMA write buffer for {}", fname);
        return -EIO;
    }
    std::memcpy(buf.addr, buffer, size);

    ssize_t rc = slash_qdma_qpair_transfer(qpairFd, buf.fd, 0, base, aligned,
                                           SLASH_QDMA_XFER_H2C);
    (void)slash_qdma_buffer_destroy(&buf);
    if (rc != (ssize_t)aligned) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__, "Could not write to {}",
                           fname);
        return -EIO;
    }
    return (ssize_t)size;
}

ssize_t QdmaIntf::read_to_buffer(const char* fname, char* buffer, uint64_t size, uint64_t base) {
    if (qpairFd < 0) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                           "QDMA streaming not initialized");
        return -EIO;
    }
    if (size == 0) {
        return 0;
    }

    const uint64_t aligned = roundUpToPage(size);
    struct slash_qdma_buffer buf{};
    if (slash_qdma_qpair_buffer_create(qpairFd, aligned, &buf) != 0) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                           "Could not create QDMA read buffer for {}", fname);
        return -EIO;
    }

    ssize_t rc = slash_qdma_qpair_transfer(qpairFd, buf.fd, 0, base, aligned,
                                           SLASH_QDMA_XFER_C2H);
    if (rc == (ssize_t)aligned) {
        std::memcpy(buffer, buf.addr, size);
    }
    (void)slash_qdma_buffer_destroy(&buf);
    if (rc != (ssize_t)aligned) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__, "Could not read from {}",
                           fname);
        return -EIO;
    }
    return (ssize_t)size;
}

void QdmaIntf::write_buff(char* buffer, uint64_t start_addr, uint64_t size) {
    utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__,
                       "Writing buffer with size: {x} at address {x}", size, start_addr);
    write_from_buffer("qdma-qpair", buffer, size, start_addr);
}

void QdmaIntf::read_buff(char* buffer, uint64_t start_addr, uint64_t size) {
    utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__,
                       "Reading buffer with size: {x} at address {x}", size, start_addr);
    read_to_buffer("qdma-qpair", buffer, size, start_addr);
}

uint32_t QdmaIntf::getQueueIdx() { return queueIdx; }

}  // namespace vrt
