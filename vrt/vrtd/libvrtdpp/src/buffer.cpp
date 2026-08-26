/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <vrtd/buffer.hpp>

#include "connection.hpp"

#include <vrtd/error.hpp>
#include <vrtd/vrtd.h>

#include <stdexcept>
#include <utility>

namespace vrtd {

/*
 * The native handle stores the connection's socket FD for daemon cleanup.
 * Retain the Connection beside it so that descriptor cannot outlive its owner.
 */
Buffer::Buffer(std::shared_ptr<detail::Connection> connection,
               struct vrtd_buffer *buffer) noexcept
    : connection(std::move(connection))
    , buffer(buffer)
{
}

Buffer::~Buffer()
{
    close();
}

Buffer::Buffer(Buffer&& other) noexcept
    : connection(std::move(other.connection))
    , buffer(std::exchange(other.buffer, nullptr))
{
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    /*
     * Release through the destination's original connection before replacing
     * it. Moving the connection first would route cleanup to the wrong daemon.
     */
    close();
    connection = std::move(other.connection);
    buffer = std::exchange(other.buffer, nullptr);

    return *this;
}

uint32_t Buffer::getDeviceNum() const noexcept
{
    return buffer ? buffer->dev : 0u;
}

BufferAllocType Buffer::getAllocType() const noexcept
{
    return buffer
        ? static_cast<BufferAllocType>(buffer->alloc_type)
        : BufferAllocType::Ddr;
}

BufferAllocDir Buffer::getAllocDir() const noexcept
{
    return buffer
        ? static_cast<BufferAllocDir>(buffer->alloc_dir)
        : BufferAllocDir::Bidirectional;
}

uint64_t Buffer::getAllocArg() const noexcept
{
    return buffer ? buffer->alloc_arg : 0u;
}

uint64_t Buffer::getSize() const noexcept
{
    return buffer ? buffer->size : 0u;
}

uint64_t Buffer::getPhysAddr() const noexcept
{
    return buffer ? buffer->phys_addr : 0u;
}

void *Buffer::data() const noexcept
{
    return buffer ? buffer->buf : nullptr;
}

void *Buffer::data() noexcept
{
    return buffer ? buffer->buf : nullptr;
}

int Buffer::getFd() const noexcept
{
    return buffer ? buffer->qpair_fd : -1;
}

int Buffer::releaseFd() noexcept
{
    if (buffer == nullptr) {
        return -1;
    }

    /* The -1 sentinel prevents later RAII cleanup from closing caller-owned FD. */
    return std::exchange(buffer->qpair_fd, -1);
}

void Buffer::close() noexcept
{
    if (buffer == nullptr) {
        return;
    }

    /*
     * Detach ownership before cleanup so repeated or re-entrant close calls
     * cannot destroy the same native handle twice.
     */
    auto raw = std::exchange(buffer, nullptr);

    if (connection) {
        connection->closeBuffer(raw);
    } else {
        /* A malformed/moved owner can still release its local native state. */
        (void) vrtd_buffer_destroy(raw);
    }
}

bool Buffer::isClosed() const noexcept
{
    return buffer == nullptr;
}

std::fstream Buffer::fstream(std::ios_base::openmode mode) const
{
    /*
     * Buffer QDMA descriptors are ioctl endpoints, not byte streams. Keep the
     * compatibility API explicit rather than manufacturing a misleading stream.
     */
    (void) mode;

    if (isClosed()) {
        throw std::runtime_error("Buffer is closed");
    }

    throw std::runtime_error(
        "Buffer qpair fds are ioctl-only; use syncToDevice/syncFromDevice"
    );
}

void Buffer::syncToDevice(uint64_t offset, uint64_t size)
{
    if (buffer == nullptr || !connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    connection->syncBufferToDevice(buffer, offset, size);
}

void Buffer::syncFromDevice(uint64_t offset, uint64_t size)
{
    if (buffer == nullptr || !connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    connection->syncBufferFromDevice(buffer, offset, size);
}

} // namespace vrtd
