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
 * Move semantics are fully supported; copying is disabled.
 */

#include <vrtd/bar_file.hpp>

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <utility>

namespace vrtd {

BarFile::BarFile(slash_bar_file barFile) noexcept {
    this->barFile = barFile;
}

BarFile::~BarFile() {
    close();
}

BarFile::BarFile(BarFile&& other) noexcept {
    barFile = std::exchange(other.barFile, {});
    reading = std::exchange(other.reading, false);
    writing = std::exchange(other.writing, false);
    closed  = std::exchange(other.closed, true);
}

BarFile& BarFile::operator=(BarFile&& other) noexcept {
    close();

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
        throw std::runtime_error("Bar file closed while in memory operation");
    }

    munmap(barFile.map, barFile.len);
    ::close(barFile.fd);
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

    if (address >= barFile.len) {
        return nullptr;
    }

    volatile uint8_t *p = static_cast<volatile uint8_t *>(barFile.map);

    return &p[address];
}

}
