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
 * @file bar_file.cpp
 *
 * Implementation of the vrtd::BarFile C++ wrapper.
 *
 * BarFile provides RAII management of a memory-mapped PCI BAR region.
 * It wraps a slash_bar_file (fd + mmap pointer + length) obtained from
 * the daemon, and unmaps/closes on destruction.
 *
 * Move semantics transfer an idle mapping; copying is disabled.
 */

#include <vrtd/bar_file.hpp>
#include <vrtd/vrtd.h>

#include <utility>

namespace vrtd {

BarFile::BarFile(slash_bar_file barFile) noexcept {
    /* Adopt the mapping and FD exactly as returned by libvrtd. */
    this->barFile = barFile;
}

BarFile::~BarFile() noexcept {
    try {
        close();
    } catch (...) {
        /*
         * A live access violates the destructor precondition. Destruction cannot
         * preserve the object or report close()'s error, so release the native
         * resources as a final backstop despite invalidating the live pointer.
         */
        vrtd_close_bar_file(&barFile);
        closed = true;
    }
}

BarFile::BarFile(BarFile&& other) {
    /*
     * A live pointer's callback remains bound to the source object. Refuse to
     * separate that callback from the mapping it must synchronize.
     */
    if (other.reading || other.writing) {
        throw std::runtime_error("Bar file moved while in memory operation");
    }

    /* Transfer the idle mapping, then permanently close the source. */
    barFile = std::exchange(other.barFile, {});
    reading = std::exchange(other.reading, false);
    writing = std::exchange(other.writing, false);
    closed  = std::exchange(other.closed, true);
}

BarFile& BarFile::operator=(BarFile&& other) {
    if (this == &other) {
        return *this;
    }

    /*
     * Validate the source before closing the destination so either failure
     * leaves both objects unchanged.
     */
    if (other.reading || other.writing) {
        throw std::runtime_error("Bar file moved while in memory operation");
    }

    /* close() rejects a live destination before releasing its resources. */
    close();

    /* Transfer the idle mapping, then permanently close the source. */
    barFile = std::exchange(other.barFile, {});
    reading = std::exchange(other.reading, false);
    writing = std::exchange(other.writing, false);
    closed  = std::exchange(other.closed, true);

    return *this;
}

void BarFile::close() {
    if (closed) {
        return;
    }

    if (reading || writing) {
        /*
         * Unmapping here would invalidate a live volatile pointer and prevent
         * its cleanup callback from ending the native synchronization session.
         */
        throw std::runtime_error("Bar file closed while in memory operation");
    }

    vrtd_close_bar_file(&barFile);
    closed = true;
}

bool BarFile::isClosed() const noexcept {
    return closed;
}

size_t BarFile::getLen() const noexcept {
    if (closed) {
        return 0;
    }

    return barFile.len;
}

volatile void *BarFile::getRawPtr(size_t address) const noexcept {
    if (closed) {
        return nullptr;
    }

    /*
     * Raw access can validate only the starting byte. The caller owns access
     * width and alignment because this API has no element type.
     */
    if (address >= barFile.len) {
        return nullptr;
    }

    volatile uint8_t *p = static_cast<volatile uint8_t *>(barFile.map);

    return &p[address];
}

}
