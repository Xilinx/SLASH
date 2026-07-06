// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//  and associated documentation files (the "Software"), to deal in the Software without
//  restriction, including without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ################################################################################################

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // memfd_create
#endif

#include "bar_memfd.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>   // flock
#include <sys/mman.h>   // memfd_create, mmap
#include <unistd.h>     // ftruncate, pread/pwrite, close

namespace slash_emu {

namespace {

TransportError os_error(const std::string& what) {
    return TransportError{ErrorKind::Transport, what + ": " + std::strerror(errno)};
}

TransportError range_error(std::size_t offset, std::size_t len, std::size_t size) {
    return TransportError{ErrorKind::Protocol,
                          "BAR access out of range: offset " + std::to_string(offset) + " + len " +
                              std::to_string(len) + " > size " + std::to_string(size)};
}

// RAII flock bracket: acquire on construction, LOCK_UN on destruction.  operation
// is LOCK_SH or LOCK_EX.  ok() reports whether the lock was acquired.
class FlockGuard {
public:
    FlockGuard(int fd, int operation) : fd_(fd) {
        // Retry on EINTR: a blocking flock interrupted by a signal (the daemon
        // installs lifecycle signal handlers) returns -1/EINTR and must be
        // retried, not reported as a (spurious) transport failure.
        int rc;
        do {
            rc = ::flock(fd_, operation);
        } while (rc != 0 && errno == EINTR);
        locked_ = (rc == 0);
    }
    ~FlockGuard() {
        if (locked_) {
            // LOCK_UN can also be interrupted; retry so the lock is always dropped.
            int rc;
            do {
                rc = ::flock(fd_, LOCK_UN);
            } while (rc != 0 && errno == EINTR);
        }
    }
    FlockGuard(const FlockGuard&)            = delete;
    FlockGuard& operator=(const FlockGuard&) = delete;

    [[nodiscard]] bool ok() const noexcept { return locked_; }

private:
    int  fd_;
    bool locked_;
};

// Check that [offset, offset+len) fits within [0, size) without overflow.
bool in_range(std::size_t offset, std::size_t len, std::size_t size) {
    if (offset > size) {
        return false;
    }
    return len <= size - offset; // no overflow: offset <= size
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

Result<BarMemfd> BarMemfd::create(std::size_t size, const std::string& name) {
    int raw = ::memfd_create(name.c_str(), MFD_CLOEXEC);
    if (raw < 0) {
        return Result<BarMemfd>::err(os_error("memfd_create"));
    }
    UniqueFd fd(raw);

    if (::ftruncate(fd.get(), static_cast<off_t>(size)) != 0) {
        return Result<BarMemfd>::err(os_error("ftruncate"));
    }

    void* map = nullptr;
    if (size > 0) {
        map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
        if (map == MAP_FAILED) {
            return Result<BarMemfd>::err(os_error("mmap"));
        }
    }

    BarMemfd bar;
    bar.fd_   = std::move(fd);
    bar.map_  = map;
    bar.size_ = size;
    return Result<BarMemfd>::ok(std::move(bar));
}

BarMemfd::BarMemfd(BarMemfd&& o) noexcept
    : fd_(std::move(o.fd_)), map_(std::exchange(o.map_, nullptr)), size_(std::exchange(o.size_, 0)) {}

BarMemfd& BarMemfd::operator=(BarMemfd&& o) noexcept {
    if (this != &o) {
        close_mapping();
        fd_   = std::move(o.fd_);
        map_  = std::exchange(o.map_, nullptr);
        size_ = std::exchange(o.size_, 0);
    }
    return *this;
}

BarMemfd::~BarMemfd() { close_mapping(); }

void BarMemfd::close_mapping() noexcept {
    if (map_ != nullptr) {
        ::munmap(map_, size_);
        map_ = nullptr;
    }
    // fd_ closes itself (UniqueFd).
}

// ─────────────────────────────────────────────────────────────────────────────
// Access
// ─────────────────────────────────────────────────────────────────────────────

Result<uint32_t> BarMemfd::read_u32(std::size_t offset) const {
    if (!in_range(offset, sizeof(uint32_t), size_)) {
        return Result<uint32_t>::err(range_error(offset, sizeof(uint32_t), size_));
    }
    // Serialise same-object daemon-side access for the whole flock bracket.
    std::lock_guard<std::mutex> mlock(*mutex_);
    FlockGuard guard(fd_.get(), LOCK_SH);
    if (!guard.ok()) {
        return Result<uint32_t>::err(os_error("flock(LOCK_SH)"));
    }
    // Little-endian assemble from the mapped bytes (portable; no aliasing UB).
    const auto* base = static_cast<const unsigned char*>(map_) + offset;
    uint32_t    v    = static_cast<uint32_t>(base[0]) | (static_cast<uint32_t>(base[1]) << 8) |
                 (static_cast<uint32_t>(base[2]) << 16) | (static_cast<uint32_t>(base[3]) << 24);
    return Result<uint32_t>::ok(v);
}

Result<void> BarMemfd::write_u32(std::size_t offset, uint32_t value) {
    if (!in_range(offset, sizeof(uint32_t), size_)) {
        return Result<void>::err(range_error(offset, sizeof(uint32_t), size_));
    }
    std::lock_guard<std::mutex> mlock(*mutex_);
    FlockGuard guard(fd_.get(), LOCK_EX);
    if (!guard.ok()) {
        return Result<void>::err(os_error("flock(LOCK_EX)"));
    }
    auto* base = static_cast<unsigned char*>(map_) + offset;
    base[0]    = static_cast<unsigned char>(value & 0xff);
    base[1]    = static_cast<unsigned char>((value >> 8) & 0xff);
    base[2]    = static_cast<unsigned char>((value >> 16) & 0xff);
    base[3]    = static_cast<unsigned char>((value >> 24) & 0xff);
    return Result<void>::ok();
}

Result<std::vector<uint8_t>> BarMemfd::read(std::size_t offset, std::size_t len) const {
    if (!in_range(offset, len, size_)) {
        return Result<std::vector<uint8_t>>::err(range_error(offset, len, size_));
    }
    std::lock_guard<std::mutex> mlock(*mutex_);
    FlockGuard guard(fd_.get(), LOCK_SH);
    if (!guard.ok()) {
        return Result<std::vector<uint8_t>>::err(os_error("flock(LOCK_SH)"));
    }
    const auto* base = static_cast<const unsigned char*>(map_) + offset;
    return Result<std::vector<uint8_t>>::ok(std::vector<uint8_t>(base, base + len));
}

Result<void> BarMemfd::write(std::size_t offset, std::span<const uint8_t> bytes) {
    if (!in_range(offset, bytes.size(), size_)) {
        return Result<void>::err(range_error(offset, bytes.size(), size_));
    }
    std::lock_guard<std::mutex> mlock(*mutex_);
    FlockGuard guard(fd_.get(), LOCK_EX);
    if (!guard.ok()) {
        return Result<void>::err(os_error("flock(LOCK_EX)"));
    }
    if (!bytes.empty()) {
        std::memcpy(static_cast<unsigned char*>(map_) + offset, bytes.data(), bytes.size());
    }
    return Result<void>::ok();
}

Result<uint32_t> BarMemfd::update_u32(std::size_t offset,
                                      const std::function<uint32_t(uint32_t)>& fn) {
    if (!in_range(offset, sizeof(uint32_t), size_)) {
        return Result<uint32_t>::err(range_error(offset, sizeof(uint32_t), size_));
    }
    // One mutex + one EXCLUSIVE flock bracket spanning the whole read-modify-write
    // so the RMW is atomic against other daemon threads AND the user.
    std::lock_guard<std::mutex> mlock(*mutex_);
    FlockGuard                  guard(fd_.get(), LOCK_EX);
    if (!guard.ok()) {
        return Result<uint32_t>::err(os_error("flock(LOCK_EX)"));
    }
    auto*    base = static_cast<unsigned char*>(map_) + offset;
    uint32_t cur  = static_cast<uint32_t>(base[0]) | (static_cast<uint32_t>(base[1]) << 8) |
                   (static_cast<uint32_t>(base[2]) << 16) | (static_cast<uint32_t>(base[3]) << 24);
    uint32_t next = fn(cur);
    base[0]       = static_cast<unsigned char>(next & 0xff);
    base[1]       = static_cast<unsigned char>((next >> 8) & 0xff);
    base[2]       = static_cast<unsigned char>((next >> 16) & 0xff);
    base[3]       = static_cast<unsigned char>((next >> 24) & 0xff);
    return Result<uint32_t>::ok(next);
}

Result<UniqueFd> BarMemfd::reopen() const {
    // Open a fresh file description on the same underlying memfd via the magic
    // /proc symlink.  This is NOT dup(): dup() shares the open file description,
    // whose flock would never conflict with the daemon's; a distinct description
    // is what makes the daemon-vs-user flock model work.
    std::string proc_path = "/proc/self/fd/" + std::to_string(fd_.get());
    int         raw       = ::open(proc_path.c_str(), O_RDWR | O_CLOEXEC);
    if (raw < 0) {
        return Result<UniqueFd>::err(os_error("open(" + proc_path + ")"));
    }
    return Result<UniqueFd>::ok(UniqueFd(raw));
}

// ─────────────────────────────────────────────────────────────────────────────
// Standard BAR set
// ─────────────────────────────────────────────────────────────────────────────

std::size_t bar_kind_size(BarKind kind) noexcept {
    switch (kind) {
        case BarKind::UserRegion:   return kUserRegionSize;
        case BarKind::ServiceLayer: return kServiceLayerSize;
        case BarKind::ClockWizard:  return kClockWizardSize;
    }
    return 0;
}

int bar_kind_index(BarKind kind) noexcept {
    switch (kind) {
        case BarKind::UserRegion:   return 0;
        case BarKind::ServiceLayer: return 2;
        case BarKind::ClockWizard:  return 4;
    }
    return -1;
}

BarMemfd& BarSet::by_kind(BarKind kind) noexcept {
    switch (kind) {
        case BarKind::UserRegion:   return user_region;
        case BarKind::ServiceLayer: return service_layer;
        case BarKind::ClockWizard:  return clock_wizard;
    }
    return user_region; // unreachable; keeps the compiler happy
}

const BarMemfd& BarSet::by_kind(BarKind kind) const noexcept {
    switch (kind) {
        case BarKind::UserRegion:   return user_region;
        case BarKind::ServiceLayer: return service_layer;
        case BarKind::ClockWizard:  return clock_wizard;
    }
    return user_region;
}

BarMemfd* BarSet::by_index(int bar_index) noexcept {
    switch (bar_index) {
        case 0: return &user_region;
        case 2: return &service_layer;
        case 4: return &clock_wizard;
        default: return nullptr;
    }
}

const BarMemfd* BarSet::by_index(int bar_index) const noexcept {
    switch (bar_index) {
        case 0: return &user_region;
        case 2: return &service_layer;
        case 4: return &clock_wizard;
        default: return nullptr;
    }
}

Result<BarSet> make_standard_bars() {
    Result<BarMemfd> user = BarMemfd::create(kUserRegionSize, "slash_bar_user");
    if (!user) {
        return Result<BarSet>::err(user.error());
    }
    Result<BarMemfd> service = BarMemfd::create(kServiceLayerSize, "slash_bar_service");
    if (!service) {
        return Result<BarSet>::err(service.error());
    }
    Result<BarMemfd> clock = BarMemfd::create(kClockWizardSize, "slash_bar_clock");
    if (!clock) {
        return Result<BarSet>::err(clock.error());
    }

    BarSet set{std::move(user.value()), std::move(service.value()), std::move(clock.value())};
    return Result<BarSet>::ok(std::move(set));
}

} // namespace slash_emu
