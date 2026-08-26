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
#include <memory>

#include <vrtd/wire.h>

struct vrtd_buffer;

namespace vrtd {

namespace detail {
class Connection;
}

/**
 * @brief Device memory class requested from the vrtd allocator.
 *
 * Values mirror @c vrtd_alloc_type. The interpretation of Buffer::getAllocArg()
 * depends on the selected class.
 */
enum class BufferAllocType : uint32_t {
    Ddr     = VRTD_ALLOC_TYPE_DDR, ///< Allocate from DDR memory.
    Hbm     = VRTD_ALLOC_TYPE_HBM, ///< Allocate from one requested HBM region.
    HbmVnoc = VRTD_ALLOC_TYPE_HBM_VNOC, ///< Let vrtd place the HBM allocation.
};

/**
 * @brief QDMA transfer directions enabled for a buffer.
 *
 * Values mirror @c vrtd_alloc_dir and constrain which sync method may be used.
 */
enum class BufferAllocDir : uint32_t {
    Bidirectional  = VRTD_ALLOC_DIR_BIDIRECTIONAL, ///< Permit both sync directions.
    HostToDevice   = VRTD_ALLOC_DIR_HOST_TO_DEVICE, ///< Reject syncFromDevice().
    DeviceToHost   = VRTD_ALLOC_DIR_DEVICE_TO_HOST, ///< Reject syncToDevice().
};

/**
 * @brief AXI-MM / NoC channel selection for a buffer's QDMA queue pair.
 *
 * Mirrors @c vrtd_mm_channel (values must stay in sync). @c Auto stripes across
 * channels by (qid & 1); @c Ch0 / @c Ch1 pin to a single channel.
 */
enum class MmChannel : uint32_t {
    Auto = 0, ///< Stripe across channels by (qid & 1).
    Ch0  = 1, ///< Pin to AXI-MM/NoC channel 0.
    Ch1  = 2, ///< Pin to AXI-MM/NoC channel 1.
};

/**
 * @brief RAII wrapper for a vrtd buffer allocation.
 *
 * A @c Buffer owns the underlying @c vrtd_buffer, including its qpair FD and
 * host-side staging buffer. Destruction closes the FD and releases the mapping.
 * The buffer retains the connection needed for daemon-side cleanup.
 *
 * @note Move-only; copying is disabled. The moved-from object is closed.
 * @warning A single Buffer is not safe for concurrent close, move, or sync
 * operations. Operations on independent buffers may run concurrently.
 */
class Buffer {
public:
    /**
     * @brief Release daemon and local buffer resources.
     *
     * Cleanup is best-effort and does not throw. All pointers and borrowed
     * descriptors obtained from this object become invalid.
     */
    ~Buffer();

    /** Native allocation and descriptor ownership cannot be copied. */
    Buffer(const Buffer&)            = delete;
    Buffer& operator=(const Buffer&) = delete;

    /** Transfer ownership and leave @p other closed. */
    Buffer(Buffer&& other) noexcept;

    /**
     * @brief Close the current allocation, then adopt @p other.
     *
     * The moved-from object becomes closed.
     */
    Buffer& operator=(Buffer&& other) noexcept;

    /**
     * @brief Return the owning device index, or zero when closed.
     *
     * Zero is also a valid device index; use isClosed() to disambiguate.
     */
    uint32_t getDeviceNum() const noexcept;

    /**
     * @brief Return the requested allocation type.
     *
     * A closed object reports BufferAllocType::Ddr as a sentinel.
     */
    BufferAllocType getAllocType() const noexcept;

    /**
     * @brief Return the enabled QDMA transfer direction.
     *
     * A closed object reports BufferAllocDir::Bidirectional as a sentinel.
     */
    BufferAllocDir getAllocDir() const noexcept;

    /**
     * @brief Return the type-specific allocation argument, or zero if closed.
     *
     * For BufferAllocType::Hbm this is the requested HBM region.
     */
    uint64_t getAllocArg() const noexcept;

    /**
     * @brief Return the allocated byte count after allocator rounding.
     *
     * A closed object returns zero.
     */
    uint64_t getSize() const noexcept;

    /**
     * @brief Return the physical device address, or zero if closed.
     */
    uint64_t getPhysAddr() const noexcept;

    /**
     * @brief Return the mutable host mapping, or null if closed.
     *
     * The pointer remains mutable for compatibility even on a const Buffer and
     * becomes invalid on close, move, or destruction.
     */
    void *data() const noexcept;

    /**
     * @brief Return the writable host mapping, or null if closed.
     *
     * The pointer becomes invalid on close, move, or destruction.
     */
    void *data() noexcept;

    /**
     * @brief Borrow the owned file descriptor without transferring ownership.
     *
     * @return QDMA descriptor, or -1 after close or releaseFd().
     * @warning Do not close the returned FD directly unless you have called
     *          @c releaseFd(). Prefer @c close().
     */
    int getFd() const noexcept;

    /**
     * @brief Release qpair FD ownership to the caller.
     *
     * The buffer remains valid, but later cleanup will not close that
     * descriptor.
     *
     * @return Released descriptor, or -1 if no native buffer is owned.
     * @warning The caller must close a successfully released descriptor.
     */
    int releaseFd() noexcept;

    /**
     * @brief Close and destroy the buffer via vrtd (idempotent).
     *
     * While connected, cleanup asks vrtd to release the server allocation and
     * then destroys local mappings and descriptors. After connection closure,
     * only local state is destroyed because vrtd already reclaims all resources
     * associated with the disconnected client. Errors are ignored.
     */
    void close() noexcept;

    /**
     * @brief Return whether this object owns no native buffer.
     *
     * Moved-from objects are closed.
     */
    bool isClosed() const noexcept;

    /**
     * @brief Sync host buffer contents to the device.
     *
     * @param offset First byte in the host mapping.
     * @param size Number of bytes to transfer.
     * @throws vrtd::Error if closed, if the range is invalid, if the buffer is
     *         device-to-host-only, or if DMA fails.
     */
    void syncToDevice(uint64_t offset, uint64_t size);

    /**
     * @brief Sync device contents into the host buffer.
     *
     * @param offset First byte in the host mapping.
     * @param size Number of bytes to transfer.
     * @throws vrtd::Error if closed, if the range is invalid, if the buffer is
     *         host-to-device-only, or if DMA fails.
     */
    void syncFromDevice(uint64_t offset, uint64_t size);

    /**
     * @brief Reject stream access to the ioctl-only buffer descriptor.
     *
     * QDMA buffer descriptors support registered-buffer ioctls rather than
     * read/write stream operations, so this compatibility entry point never
     * returns.
     *
     * @param mode Ignored compatibility argument.
     * @throws std::runtime_error for both open and closed buffers.
     */
    std::fstream fstream(
        std::ios_base::openmode mode =
            std::ios_base::in | std::ios_base::out | std::ios_base::binary
    ) const;

private:
    friend class Device;

    /**
     * @brief Adopt a native buffer and retain its cleanup connection.
     *
     * @param connection Open shared connection that created @p buffer.
     * @param buffer Exclusively owned native handle.
     */
    Buffer(std::shared_ptr<detail::Connection> connection,
           struct vrtd_buffer *buffer) noexcept;

    /** Shared connection used to coordinate sync and daemon-side cleanup. */
    std::shared_ptr<detail::Connection> connection;

    /** Exclusively owned native handle, or null when closed or moved-from. */
    struct vrtd_buffer *buffer{nullptr};
};

} // namespace vrtd

#endif // VRTD_BUFFER_HPP
