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

#ifndef VRTD_BAR_HPP
#define VRTD_BAR_HPP

#include <string>
#include <string_view>
#include <functional>
#include <stdint.h>

#include <vrtd/bar_file.hpp>

namespace vrtd {

/**
 * @brief Value-type metadata handle for a device BAR (Base Address Register).
 *
 * Provides discovery information and a convenience method to open/map the BAR.
 *
 * @par Semantics
 * - @c isUsable(): this BAR is currently accessible/mappable to the caller.
 * - @c isInUse(): this BAR is leased by another tenant (currently always false).
 * - @c getStartAddress(), @c getLength(): physical address and size in **bytes**.
 *
 * @par Lifetime
 * A @c Bar becomes invalid if its originating @c Session is closed or moved.
 * Any subsequent member call will throw.
 *
 * @par Thread safety
 * Methods are thread-safe and may be called concurrently; they synchronize
 * on the originating @c Session.
 */
class Bar {
public:
    ~Bar() = default;

    Bar(const Bar&)                = default;
    Bar& operator=(const Bar&)     = default;
    Bar(Bar&&) noexcept            = default;
    Bar& operator=(Bar&&) noexcept = default;

    /**
     * @brief Zero-based device index that owns this BAR.
     */
    uint32_t getDeviceNum() const noexcept;
    
    /**
     * @brief Zero-based BAR index on the device.
     */
    uint8_t getNum() const noexcept;
    
    /**
     * @brief Whether this BAR is currently usable (mappable) by the caller.
     */
    bool isUsable() const noexcept;

    /**
     * @brief Whether this BAR is currently in use by another tenant.
     *
     * @note In the current implementation this always returns false.
     */
    bool isInUse() const noexcept;

    /**
     * @brief Physical start address of the BAR.
     */
    uint64_t getStartAddress() const noexcept;

    /**
     * @brief Length/size of the BAR (bytes).
     */
    uint64_t getLength() const noexcept;

    /**
     * @brief Open and @c mmap() this BAR, returning an owning @c BarFile.
     *
     * @return @c BarFile that RAII-owns the FD and mapping; its destructor
     *         unmaps and closes automatically.
     * @throws vrtd::Error on failure.
     */
    BarFile openBarFile() const;
private:
    // Only allow the Session class to generate this class
    friend class Session;
    Bar(uint32_t deviceNum, uint8_t num, bool usable, bool inUse, uint64_t startAddress, uint64_t length, std::function<BarFile(const Bar&)> fOpenBarFile) noexcept;

    uint32_t deviceNum;
    uint8_t num;
    bool usable;
    bool inUse;
    uint64_t startAddress;
    uint64_t length;

    std::function<BarFile(const Bar&)> fOpenBarFile;
};

} // namespace vrtd

#endif // VRTD_BAR_HPP