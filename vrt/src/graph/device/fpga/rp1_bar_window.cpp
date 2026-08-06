/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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

#include <vrt/graph/device/fpga/rp1_bar_window.hpp>

#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace vrt::graph::fpga {

namespace {

/// Backend abstraction. Two implementations: BarFile (production) and
/// raw-buffer (tests). Both share the same window-offset arithmetic.
struct Backend {
    virtual ~Backend() = default;
    virtual std::size_t mappedLength() const noexcept                                  = 0;
    virtual void        readBytes (std::uint32_t abs_off, void*  dst, std::size_t n)   = 0;
    virtual void        writeBytes(std::uint32_t abs_off, const void* src, std::size_t n) = 0;
    virtual std::uint32_t readU32 (std::uint32_t abs_off)                              = 0;
    virtual void          writeU32(std::uint32_t abs_off, std::uint32_t v)             = 0;
};

/*
 * A BarFile pointer is also a dma-buf synchronization bracket, so it must not
 * escape one accessor. The lock prevents two callers from nesting incompatible
 * read/write brackets on the same mapping.
 */
class BarFileBackend final : public Backend {
   public:
    explicit BarFileBackend(vrtd::BarFile bar_file) : barFile_(std::move(bar_file)) {}

    std::size_t mappedLength() const noexcept override { return barFile_.getLen(); }

    void readBytes(std::uint32_t abs_off, void* dst, std::size_t n) override {
        if (n == 0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        auto p = barFile_.getPtr<std::uint8_t>(vrtd::BarFile::Direction::Read, abs_off);
        std::memcpy(dst, const_cast<const std::uint8_t*>(p.get()), n);
    }

    void writeBytes(std::uint32_t abs_off, const void* src, std::size_t n) override {
        if (n == 0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        auto p = barFile_.getPtr<std::uint8_t>(vrtd::BarFile::Direction::Write, abs_off);
        std::memcpy(const_cast<std::uint8_t*>(p.get()), src, n);
    }

    std::uint32_t readU32(std::uint32_t abs_off) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto p = barFile_.getPtr<std::uint32_t>(vrtd::BarFile::Direction::Read, abs_off);
        return *p;
    }

    void writeU32(std::uint32_t abs_off, std::uint32_t v) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto p = barFile_.getPtr<std::uint32_t>(vrtd::BarFile::Direction::Write, abs_off);
        *p = v;
    }

   private:
    vrtd::BarFile barFile_;
    std::mutex    mtx_;
};

class RawBufferBackend final : public Backend {
   public:
    RawBufferBackend(void* base, std::size_t length)
        : base_(static_cast<volatile std::uint8_t*>(base)), length_(length) {}

    std::size_t mappedLength() const noexcept override { return length_; }

    void readBytes(std::uint32_t abs_off, void* dst, std::size_t n) override {
        if (n == 0) return;
        boundsCheck(abs_off, n);
        std::memcpy(dst,
                    const_cast<const std::uint8_t*>(base_ + abs_off),
                    n);
    }

    void writeBytes(std::uint32_t abs_off, const void* src, std::size_t n) override {
        if (n == 0) return;
        boundsCheck(abs_off, n);
        std::memcpy(const_cast<std::uint8_t*>(base_ + abs_off),
                    src, n);
    }

    std::uint32_t readU32(std::uint32_t abs_off) override {
        boundsCheck(abs_off, sizeof(std::uint32_t));
        std::uint32_t v;
        std::memcpy(&v,
                    const_cast<const std::uint8_t*>(base_ + abs_off),
                    sizeof(v));
        return v;
    }

    void writeU32(std::uint32_t abs_off, std::uint32_t v) override {
        boundsCheck(abs_off, sizeof(std::uint32_t));
        std::memcpy(const_cast<std::uint8_t*>(base_ + abs_off),
                    &v, sizeof(v));
    }

   private:
    void boundsCheck(std::uint32_t off, std::size_t n) const {
        if (static_cast<std::uint64_t>(off) + n > length_) {
            throw std::out_of_range("Rp1BarWindow raw buffer: access out of range");
        }
    }

    volatile std::uint8_t* base_;
    std::size_t            length_;
};

}  // namespace

struct Rp1BarWindow::Impl {
    std::unique_ptr<Backend> backend;
    std::uint64_t            windowOffset;

    /*
     * Check the complete range in 64 bits before narrowing to the backend's
     * 32-bit BAR offset; this rejects both end overflow and an oversized origin.
     */
    std::uint32_t absoluteOffset(std::uint32_t window_off, std::size_t n) const {
        const std::uint64_t abs = windowOffset + window_off;
        const std::uint64_t end = abs + n;
        if (end > backend->mappedLength() || end < abs /* overflow */) {
            throw std::out_of_range(
                "Rp1BarWindow: window-relative offset " + std::to_string(window_off) +
                " + " + std::to_string(n) + " exceeds mapping " +
                std::to_string(backend->mappedLength()));
        }
        return static_cast<std::uint32_t>(abs);
    }
};

// ---- Construction --------------------------------------------------------

Rp1BarWindow::Rp1BarWindow(vrtd::BarFile bar_file, std::uint64_t window_offset)
    : impl_(std::make_unique<Impl>()) {
    impl_->backend      = std::make_unique<BarFileBackend>(std::move(bar_file));
    impl_->windowOffset = window_offset;

    if (impl_->backend->mappedLength() < window_offset + kWindowSize) {
        throw std::invalid_argument(
            "Rp1BarWindow: BAR mapping " +
            std::to_string(impl_->backend->mappedLength()) +
            " is smaller than window_offset + kWindowSize (" +
            std::to_string(window_offset + kWindowSize) + ")");
    }
}

Rp1BarWindow::Rp1BarWindow(void* base, std::size_t length, std::uint64_t window_offset)
    : impl_(std::make_unique<Impl>()) {
    if (base == nullptr) {
        throw std::invalid_argument("Rp1BarWindow: raw buffer base is null");
    }
    if (length < window_offset) {
        throw std::invalid_argument(
            "Rp1BarWindow: raw buffer length " + std::to_string(length) +
            " < window_offset " + std::to_string(window_offset));
    }
    impl_->backend      = std::make_unique<RawBufferBackend>(base, length);
    impl_->windowOffset = window_offset;
}

Rp1BarWindow::~Rp1BarWindow()                              = default;
Rp1BarWindow::Rp1BarWindow(Rp1BarWindow&&) noexcept            = default;
Rp1BarWindow& Rp1BarWindow::operator=(Rp1BarWindow&&) noexcept = default;

// ---- Bulk access ---------------------------------------------------------

void Rp1BarWindow::readAt(std::uint32_t offset, void* dst, std::size_t n) {
    if (n == 0) return;
    const auto abs = impl_->absoluteOffset(offset, n);
    impl_->backend->readBytes(abs, dst, n);
}

void Rp1BarWindow::writeAt(std::uint32_t offset, const void* src, std::size_t n) {
    if (n == 0) return;
    /*
     * Bound each dma-buf synchronization window: large graph images otherwise
     * pin one BAR write bracket for the full copy and delay peer access.
     */
    static constexpr std::size_t kChunk = 1u << 20;  // Keep BAR sync windows bounded.
    const auto* bytes = static_cast<const std::uint8_t*>(src);
    while (n > 0) {
        const std::size_t step = (n < kChunk) ? n : kChunk;
        const auto abs = impl_->absoluteOffset(offset, step);
        impl_->backend->writeBytes(abs, bytes, step);
        offset += static_cast<std::uint32_t>(step);
        bytes  += step;
        n      -= step;
    }
}

void Rp1BarWindow::zeroAt(std::uint32_t offset, std::size_t n) {
    if (n == 0) return;
    static constexpr std::size_t kChunk = 256;
    const std::uint8_t zeros[kChunk] = {};
    while (n > 0) {
        const std::size_t step = (n < kChunk) ? n : kChunk;
        writeAt(offset, zeros, step);
        offset += static_cast<std::uint32_t>(step);
        n      -= step;
    }
}

std::uint32_t Rp1BarWindow::readU32(std::uint32_t offset) {
    const auto abs = impl_->absoluteOffset(offset, sizeof(std::uint32_t));
    return impl_->backend->readU32(abs);
}

void Rp1BarWindow::writeU32(std::uint32_t offset, std::uint32_t value) {
    const auto abs = impl_->absoluteOffset(offset, sizeof(std::uint32_t));
    impl_->backend->writeU32(abs, value);
}

// ---- Typed convenience ---------------------------------------------------

void Rp1BarWindow::clearSignal(std::uint32_t slot, std::uint32_t sig_array_offset) {
    rp1_signal_slot_t empty{};
    writeAt(sig_array_offset + slot * sizeof(rp1_signal_slot_t),
            &empty, sizeof(empty));
}

void Rp1BarWindow::readSignal(std::uint32_t slot, rp1_signal_slot_t& out,
                              std::uint32_t sig_array_offset) {
    readAt(sig_array_offset + slot * sizeof(rp1_signal_slot_t),
           &out, sizeof(out));
}

void Rp1BarWindow::readCq(std::uint32_t idx, rp1_cq_entry_t& out,
                          std::uint32_t cq_offset) {
    readAt(cq_offset + idx * sizeof(rp1_cq_entry_t),
           &out, sizeof(out));
}

void Rp1BarWindow::readTrace(std::uint32_t idx, rp1_trace_entry_t& out,
                             std::uint32_t trace_offset) {
    readAt(trace_offset + idx * sizeof(rp1_trace_entry_t),
           &out, sizeof(out));
}

// ---- Diagnostics ---------------------------------------------------------

std::size_t Rp1BarWindow::mappedLength() const noexcept {
    return impl_->backend->mappedLength();
}

std::uint64_t Rp1BarWindow::windowOffset() const noexcept {
    return impl_->windowOffset;
}

}  // namespace vrt::graph::fpga
