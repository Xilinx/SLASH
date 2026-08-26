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

#ifndef VRTD_BAR_FILE_HPP
#define VRTD_BAR_FILE_HPP

#include <slash/ctldev.h>

#include <vrtd/bar_file_ptr.hpp>

#include <stdexcept>
#include <cstdint>

namespace vrtd {

/**
 * @brief Owning RAII handle for a mapped BAR region.
 *
 * Encapsulates a @c slash_bar_file containing the BAR mapping (@c map) and
 * length (@c len). Provides typed access via @c getPtr<T>() which brackets
 * memory access with the appropriate @c slash_bar_file_start_* /
 * @c slash_bar_file_end_* calls. Direct raw access through @c getRawPtr() is
 * intentionally unsynchronized and unsafe; this API exposes no manual
 * synchronization handle, so prefer typed access for normal MMIO.
 *
 * @warning Not thread-safe. At most one memory operation (read or write)
 *          may be active at a time per @c BarFile instance. Concurrent
 *          calls to @c getPtr() / @c getRawPtr() on the same object are
 *          not allowed.
 *
 * @note Move-only; copying is disabled. The moved-from object is closed.
 */
class BarFile {
public:
    /**
     * @brief Destructor.
     *
     * Releases the mapping and FD if still open.
     *
     * @pre Every @c BarFilePtr created by this object has been destroyed.
     *
     * The destructor cannot report a violated precondition. If an access
     * session is still active, it forcibly releases the native resources as a
     * last resort; any outstanding pointer is then invalid.
     */
    ~BarFile() noexcept;

    /** A mapping and its descriptor have exclusive ownership and cannot copy. */
    BarFile(const BarFile&)            = delete;
    BarFile& operator=(const BarFile&) = delete;

    /**
     * @brief Move constructor; transfers ownership and closes the source.
     *
     * @throws std::runtime_error if @p other has a live @c BarFilePtr. The
     *         source remains unchanged in that case.
     */
    BarFile(BarFile&&);

    /**
     * @brief Move assignment; closes current, then takes ownership.
     *
     * Self-assignment has no effect.
     *
     * @throws std::runtime_error if either object has a live @c BarFilePtr.
     *         Both objects remain unchanged in that case.
     */
    BarFile& operator=(BarFile&&);

    /**
     * @brief Size of the mapped BAR in bytes.
     *
     * @return Mapping length, or zero after close or move.
     */
    size_t getLen() const noexcept;

    /**
     * @brief Get a raw volatile pointer into the mapping.
     *
     * @param address Byte offset from the start of the mapping (default 0).
     * @return Pointer to the selected byte, or @c nullptr if closed or if the
     *         starting offset is outside the mapping.
     *
     * Only the starting byte is bounds-checked. The caller owns width and
     * alignment correctness and must stop using the pointer before close,
     * move, or destruction. This access path cannot establish a DMA-BUF
     * synchronization session; use getPtr() when synchronization is required.
     *
     * In other words: you probably don't want to use this. 
     */
    volatile void *getRawPtr(size_t address = 0) const noexcept;

    /**
     * @brief Close the mapping and underlying FD.
     *
     * After a successful close, @c isClosed() returns true, @c getLen()
     * returns zero, and @c getRawPtr() returns null. Typed access throws.
     *
     * The operation is idempotent. If a memory operation is still in progress
     * (i.e., a @c BarFilePtr is alive), it throws to signal misuse.
     */
    void close();

    /**
     * @brief Whether the BAR has been closed.
     */
    bool isClosed() const noexcept;

private:
    friend class Bar;

    /**
     * @brief Adopt an already mapped BAR returned by libvrtd.
     *
     * @param barFile Owning mapping and descriptor consumed by this object.
     */
    explicit BarFile(slash_bar_file barFile) noexcept;

    /** Exclusively owned native mapping released by vrtd_close_bar_file(). */
    slash_bar_file barFile;

    /** True while one BarFilePtr owns a read synchronization session. */
    bool reading{};

    /** True while one BarFilePtr owns a write synchronization session. */
    bool writing{};

    /** Permanent closed or moved-from state; mutually excludes typed access. */
    bool closed{};

public:
    /**
     * @brief DMA-BUF synchronization direction selected by getPtr().
     */
    enum class Direction {
        Read,  ///< Bracket access with the native read synchronization calls.
        Write, ///< Bracket access with the native write synchronization calls.
    };

    /**
     * @brief Acquire a typed RAII pointer into the BAR mapping.
     *
     * Starts a read or write session (depending on @p direction) and returns
     * a move-only @c BarFilePtr<T> that will automatically end the session on
     * destruction. Only one operation (read or write) may be active at a time,
     * and this BarFile must outlive the returned pointer.
     *
     * @tparam T Element type. Must be an object type; recommended to be
     *           trivially copyable/standard-layout. Accesses are through
     *           @c volatile pointers to model device memory semantics.
     * @param direction Whether this is a read or write operation.
     * @param address   Byte offset into the mapping where @c T is addressed.
     *
     * @return @c BarFilePtr<T> owning the access session.
     *
     * @throws std::runtime_error if:
     *         - the file is closed,
     *         - @p address is out of range,
     *         - another read/write operation is already in progress,
     *         - @p direction is invalid.
     *
     * @warning Only @p address is bounds-checked. The caller must ensure
     *          @c sizeof(T) fits in the remaining mapping and that alignment is
     *          valid. Native synchronization failures are currently ignored.
     */
    template<class T>
    BarFilePtr<T> getPtr(Direction direction, size_t address = 0) {
        /*
         * Validate the local state before starting a synchronization session.
         * A returned pointer owns the only active session for this mapping.
         */
        if (closed) {
            throw std::runtime_error("Memory operation on closed bar file");
        }

        if (address >= barFile.len) {
            throw std::runtime_error("Bad address");
        }

        if (reading || writing) {
            throw std::runtime_error("Memory operation already in progress");
        }

        volatile uint8_t *p = static_cast<volatile uint8_t *>(barFile.map);
        volatile T *paddr = reinterpret_cast<volatile T *>(&p[address]);

        std::function<void()> callback{};

        /*
         * Pair the native START operation with a callback-owned END operation.
         * The callback also clears the local exclusion flag so another access
         * can begin after the returned BarFilePtr is destroyed.
         */
        if (direction == Direction::Read) {
            slash_bar_file_start_read(&barFile);
            reading = true;
            callback = [&]{
                slash_bar_file_end_read(&barFile);
                reading = false;
            };
        } else if (direction == Direction::Write) {
            slash_bar_file_start_write(&barFile);
            writing = true;
            callback = [&]{
                slash_bar_file_end_write(&barFile);
                writing = false;
            };
        } else {
            throw std::runtime_error("Bad direction");
        }

        return BarFilePtr(paddr, callback);
    }
};

} // namespace vrtd

#endif // VRTD_BAR_FILE_HPP
