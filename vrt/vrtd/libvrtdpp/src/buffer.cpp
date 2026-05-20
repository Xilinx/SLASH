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

/**
 * @file buffer.cpp
 *
 * Implementation of the vrtd::Buffer C++ wrapper.
 *
 * Buffer provides RAII ownership of a @c vrtd_buffer obtained from the
 * daemon via libvrtd.  It wraps the host-side mmap, the QDMA queue pair
 * fd, and the device-side physical address into a single movable object.
 *
 * Key features:
 *  - Move-only semantics (no copies); destruction calls vrtd_buffer_close().
 *  - @c syncToDevice() / @c syncFromDevice() for DMA transfers.
 *  - @c fstream() opens a std::fstream on the qpair fd via
 *    @c /proc/self/fd/<n>, allowing stream-style I/O on the DMA channel.
 *  - @c releaseFd() transfers qpair fd ownership to the caller.
 */

#include <vrtd/buffer.hpp>
#include <vrtd/error.hpp>
#include <vrtd/vrtd.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace vrtd {

Buffer::Buffer(struct vrtd_buffer *buffer) noexcept
    : buffer(buffer)
{
}

Buffer::~Buffer()
{
    close();
}

Buffer::Buffer(Buffer&& other) noexcept
    : buffer(other.buffer)
{
    other.buffer = nullptr;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    close();

    buffer = other.buffer;
    other.buffer = nullptr;

    return *this;
}

uint32_t Buffer::getDeviceNum() const noexcept
{
    return buffer ? buffer->dev : 0u;
}

BufferAllocType Buffer::getAllocType() const noexcept
{
    if (buffer == nullptr) {
        return BufferAllocType::Ddr;
    }
    return static_cast<BufferAllocType>(buffer->alloc_type);
}

BufferAllocDir Buffer::getAllocDir() const noexcept
{
    if (buffer == nullptr) {
        return BufferAllocDir::Bidirectional;
    }
    return static_cast<BufferAllocDir>(buffer->alloc_dir);
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
    int ret = buffer->qpair_fd;
    buffer->qpair_fd = -1;
    return ret;
}

void Buffer::close() noexcept
{
    if (buffer != nullptr) {
        (void) vrtd_buffer_close(buffer);
        buffer = nullptr;
    }
}

bool Buffer::isClosed() const noexcept
{
    return buffer == nullptr;
}

std::fstream Buffer::fstream(std::ios_base::openmode mode) const
{
    if (isClosed()) {
        throw std::runtime_error("Buffer is closed");
    }

    int fd = getFd();
    if (fd < 0) {
        throw std::runtime_error("Buffer FD is invalid");
    }

    std::string path = "/proc/self/fd/" + std::to_string(fd);

    std::fstream stream;
    stream.open(path, mode);
    if (!stream.is_open()) {
        throw std::runtime_error("Failed to open fstream for buffer");
    }

    return stream;
}

void Buffer::syncToDevice(uint64_t offset, uint64_t size)
{
    if (buffer == nullptr) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    enum vrtd_ret ret = vrtd_buffer_sync_to_device(buffer, offset, size);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }
}

void Buffer::syncFromDevice(uint64_t offset, uint64_t size)
{
    if (buffer == nullptr) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    enum vrtd_ret ret = vrtd_buffer_sync_from_device(buffer, offset, size);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }
}

} // namespace vrtd
