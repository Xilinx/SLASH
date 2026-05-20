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
 * @c slash_bar_file_end_* calls. Direct raw access is available via
 * @c getRawPtr(), but requires manual bracketing.
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
     * @warning If a memory operation is still in progress (i.e., a live
     *          @c BarFilePtr returned by @c getPtr() has not been destroyed),
     *          the destructor may throw (e.g., to signal improper usage).
     *          Users must ensure all @c BarFilePtr instances are destroyed
     *          before destroying or closing the @c BarFile.
     */
    ~BarFile();

    BarFile(const BarFile&)            = delete;
    BarFile& operator=(const BarFile&) = delete;

    /**
     * @brief Move constructor; transfers ownership and closes the source.
     */
    BarFile(BarFile&&) noexcept;

    /**
     * @brief Move assignment; closes current, then takes ownership.
     */
    BarFile& operator=(BarFile&&) noexcept;

    /**
     * @brief Size of the mapped BAR in bytes.
     */
    size_t getLen() const noexcept;

    /**
     * @brief Get a raw volatile pointer into the mapping.
     *
     * @param address Byte offset from the start of the mapping (default 0).
     * @return @c volatile void* pointing at @p address inside the mapping.
     *
     * @warning Using the raw pointer requires the caller to manually bracket
     *          accesses with @c slash_bar_file_start_read() / @c _end_read() or
     *          @c slash_bar_file_start_write() / @c _end_write() as appropriate.
     *          Prefer @c getPtr<T>() for RAII-safe access.
     *
     * @throws std::runtime_error if the file is closed or @p address is out of range.
     */
    volatile void *getRawPtr(size_t address = 0) const noexcept;

    /**
     * @brief Close the mapping and underlying FD.
     *
     * After a successful close, @c isClosed() returns true and further
     * operations will throw.
     *
     * @warning Not idempotent/noexcept by design: if a memory operation is
     *          still in progress (i.e., a @c BarFilePtr is alive), this
     *          function may throw to signal misuse.
     */
    void close();

    /**
     * @brief Whether the BAR has been closed.
     */
    bool isClosed() const noexcept;

private:
    friend class Session;
    explicit BarFile(slash_bar_file barFile) noexcept;

    slash_bar_file barFile;

    // Internal single-operation guards (non-thread-safe).
    bool reading{};
    bool writing{};
    bool closed{};

public:
    /**
     * @brief Direction of an access session.
     */
    enum class Direction {
        Read,
        Write,
    };

    /**
     * @brief Acquire a typed RAII pointer into the BAR mapping.
     *
     * Starts a read or write session (depending on @p direction) and returns
     * a move-only @c BarFilePtr<T> that will automatically end the session on
     * destruction. Only one operation (read or write) may be active at a time.
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
     * @warning The caller is responsible for alignment correctness.
     */
    template<class T>
    BarFilePtr<T> getPtr(Direction direction, size_t address = 0) {
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
