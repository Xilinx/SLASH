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

#ifndef VRTD_BUFFER_HPP
#define VRTD_BUFFER_HPP

#include <cstdint>
#include <fstream>

#include <vrtd/wire.h>

struct vrtd_buffer;

namespace vrtd {

/**
 * @brief Allocation type for buffers.
 */
enum class BufferAllocType : uint32_t {
    Ddr     = VRTD_ALLOC_TYPE_DDR,
    Hbm     = VRTD_ALLOC_TYPE_HBM,
    HbmVnoc = VRTD_ALLOC_TYPE_HBM_VNOC,
};

/**
 * @brief Direction for QDMA transfers for a buffer.
 */
enum class BufferAllocDir : uint32_t {
    Bidirectional  = VRTD_ALLOC_DIR_BIDIRECTIONAL,
    HostToDevice   = VRTD_ALLOC_DIR_HOST_TO_DEVICE,
    DeviceToHost   = VRTD_ALLOC_DIR_DEVICE_TO_HOST,
};

/**
 * @brief RAII wrapper for a vrtd buffer allocation.
 *
 * A @c Buffer owns the underlying @c vrtd_buffer, including its qpair FD and
 * host-side staging buffer. Destruction closes the FD and releases the mapping.
 *
 * @note Move-only; copying is disabled. The moved-from object is closed.
 */
class Buffer {
public:
    ~Buffer();

    Buffer(const Buffer&)            = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    /**
     * @brief Device index owning this buffer.
     */
    uint32_t getDeviceNum() const noexcept;

    /**
     * @brief Allocation type requested for this buffer.
     */
    BufferAllocType getAllocType() const noexcept;

    /**
     * @brief QDMA transfer direction for this buffer.
     */
    BufferAllocDir getAllocDir() const noexcept;

    /**
     * @brief Allocation argument (HBM region index for HBM allocations).
     */
    uint64_t getAllocArg() const noexcept;

    /**
     * @brief Allocated size in bytes (rounded to subregions).
     */
    uint64_t getSize() const noexcept;

    /**
     * @brief Physical device address for this allocation.
     */
    uint64_t getPhysAddr() const noexcept;

    /**
     * @brief Pointer to the host staging buffer.
     */
    void *data() const noexcept;


    /**
     * @brief Pointer to the host staging buffer.
     */
    void *data() noexcept;

    /**
     * @brief Borrow the owned file descriptor without transferring ownership.
     *
     * @warning Do not close the returned FD directly unless you have called
     *          @c releaseFd(). Prefer @c close().
     */
    int getFd() const noexcept;

    /**
     * @brief Release qpair FD ownership to the caller.
     *
     * The buffer remains valid, but will no longer close the FD on destruction.
     */
    int releaseFd() noexcept;

    /**
     * @brief Close and destroy the buffer via vrtd (idempotent).
     *
     * Releases the server-side allocation and local staging buffer. Errors
     * are ignored to preserve noexcept semantics.
     */
    void close() noexcept;

    /**
     * @brief Whether the buffer has been closed or destroyed.
     */
    bool isClosed() const noexcept;

    /**
     * @brief Sync host buffer contents to the device.
     *
     * @throws vrtd::Error on error.
     */
    void syncToDevice(uint64_t offset, uint64_t size);

    /**
     * @brief Sync device contents into the host buffer.
     *
     * @throws vrtd::Error on error.
     */
    void syncFromDevice(uint64_t offset, uint64_t size);

    /**
     * @brief Obtain a std::fstream bound to this buffer FD.
     *
     * @param mode Standard iostream open mode (defaults to in|out|binary).
     * @return A @c std::fstream owning its own FD.
     *
     * @throws std::runtime_error if the buffer is closed or the stream cannot be opened.
     *
     * @note Implementation is Linux-specific and relies on @c /proc/self/fd.
     */
    std::fstream fstream(
        std::ios_base::openmode mode =
            std::ios_base::in | std::ios_base::out | std::ios_base::binary
    ) const;

private:
    friend class Session;

    explicit Buffer(struct vrtd_buffer *buffer) noexcept;

    struct vrtd_buffer *buffer{nullptr};
};

} // namespace vrtd

#endif // VRTD_BUFFER_HPP
