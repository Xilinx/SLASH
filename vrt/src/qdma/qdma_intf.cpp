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

ssize_t QdmaIntf::write_from_buffer(const char* fname, char* buffer, uint64_t size, uint64_t base) {
    if (qpairFd < 0) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                           "QDMA streaming not initialized");
        return -EIO;
    }
    int fd = qpairFd;
    ssize_t rc;
    uint64_t count = 0;
    char* buf = buffer;
    off_t offset = base;

    do { /* Support zero byte transfer */
        uint64_t bytes = size - count;

        if (bytes > RW_MAX_SIZE) bytes = RW_MAX_SIZE;

        if (offset) {
            rc = lseek(fd, offset, SEEK_SET);
            if (rc < 0) {
                utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                                   "Could not write to {}", fname);
                return -EIO;
            }
            if (rc != offset) {
                utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                                   "Could not write to {}", fname);
                return -EIO;
            }
        }

        /* write data to file from memory buffer */
        rc = write(fd, buf, bytes);
        if (rc < 0) {
            utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__, "Could not write to {}",
                               fname);
            return -EIO;
        }
        if (rc != bytes) {
            utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__, "Could not write to {}",
                               fname);
            return -EIO;
        }

        count += bytes;
        buf += bytes;
        offset += bytes;
    } while (count < size);

    if (count != size) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__, "Could not write to {}",
                           fname);
        return -EIO;
    }
    return count;
}

ssize_t QdmaIntf::read_to_buffer(const char* fname, char* buffer, uint64_t size, uint64_t base) {
    if (qpairFd < 0) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                           "QDMA streaming not initialized");
        return -EIO;
    }
    int fd = qpairFd;
    ssize_t rc;
    uint64_t count = 0;
    char* buf = buffer;
    off_t offset = base;

    do { /* Support zero byte transfer */
        uint64_t bytes = size - count;

        if (bytes > RW_MAX_SIZE) bytes = RW_MAX_SIZE;

        if (offset) {
            rc = lseek(fd, offset, SEEK_SET);
            if (rc < 0) {
                utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                                   "Could not read from {}", fname);
                return -EIO;
            }
            if (rc != offset) {
                utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                                   "Could not read from {}", fname);
                return -EIO;
            }
        }

        /* read data from file into memory buffer */
        rc = read(fd, buf, bytes);
        if (rc < 0) {
            utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                               "Could not read from {}", fname);
            return -EIO;
        }
        if (rc != bytes) {
            utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                               "Could not read from {}", fname);
            return -EIO;
        }

        count += bytes;
        buf += bytes;
        offset += bytes;
    } while (count < size);

    if (count != size) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__, "Could not read from {}",
                           fname);
        return -EIO;
    }
    return count;
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
