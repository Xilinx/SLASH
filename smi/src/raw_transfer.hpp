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

#ifndef SMI_RAW_TRANSFER_HPP
#define SMI_RAW_TRANSFER_HPP

/// @file raw_transfer.hpp
/// @brief Backend-agnostic helpers for the raw QDMA memory-mapped transfer
///        tests used by `smi validate`.
///
/// The SLASH backend (libslash queue-pair fds) and the off-the-shelf Xilinx
/// QDMA-driver backend (/dev/qdma<idx>-MM-<qid> char devices) share the exact
/// same host-side buffer setup and pread/pwrite transfer loop -- only the way
/// the file descriptor and device address get provisioned differs.  Those
/// shared pieces live here so both backends behave (and time) identically.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

/// Per-transfer timing instrumentation.
///
/// When SLASH_QDMA_TIMING is non-zero (compile-time flag, e.g. built with
/// -DSLASH_QDMA_TIMING=1), the raw-transfer path logs the wall-clock cost of
/// each pwrite/pread syscall plus the aggregate per-transfer time and
/// effective bandwidth.  This is the userspace counterpart to the kernel's
/// SLASH_QDMA_TIMING breakdown.
#ifndef SLASH_QDMA_TIMING
#define SLASH_QDMA_TIMING 0
#endif

namespace smi::raw {

/// Host transfer sizes mirror libvrtd's QDMA staging policy.
static constexpr uint64_t BASE_TRANSFER_STEP_SIZE = 4ULL * 1024ULL;

[[noreturn]] inline void throwSystemError(const std::string& message) {
    throw std::runtime_error(message + ": " + std::strerror(errno));
}

/// A host staging buffer plus the DMA granule it is backed by.
///
/// `step` is always BASE_TRANSFER_STEP_SIZE (4 KiB base pages).  It is used
/// only for range/alignment validation: the whole range is transferred in a
/// single syscall and the kernel builds one DMA descriptor per page.
struct HostMapping {
    void* data = nullptr;
    uint64_t size = 0;
    uint64_t step = 0;
};

/// Create a host staging buffer of 4 KiB base pages for raw transfers.  @p
/// physAddr is the device address this buffer backs and is only used to make
/// error messages actionable.
inline HostMapping createHostMapping(uint64_t size, uint64_t physAddr) {
    HostMapping mapping;
    mapping.size = size;

    // Map regular base pages.  MAP_POPULATE is deliberately omitted: it would
    // pre-fault the whole buffer during mmap(), i.e. before the MADV_NOHUGEPAGE
    // below can take effect. On hosts with transparent hugepages set to
    // "always", those early faults hand back 2 MiB THP compound pages, and
    // MADV_NOHUGEPAGE does not split pages that are already faulted in. The
    // driver's strict 4 KiB base-page path (slash_qdma_map_user_base_pages_to_sgl)
    // then rejects every transfer with -EINVAL ("4 KiB transfer is not backed by
    // a base page").
    (void)physAddr;
    mapping.data = mmap(nullptr,
                        size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS,
                        -1,
                        0);
    if (mapping.data == MAP_FAILED) {
        throwSystemError("Failed to mmap raw transfer host buffer");
    }

    // Disable THP for this region *before* any page is faulted in, so that
    // every fault below allocates a genuine 4 KiB base page.
    if (madvise(mapping.data, size, MADV_NOHUGEPAGE) != 0) {
        const int savedErrno = errno;
        (void)munmap(mapping.data, size);
        mapping.data = nullptr;
        errno = savedErrno;
        throwSystemError("Failed to disable transparent hugepages for raw transfer host buffer");
    }

    // Pre-fault the buffer as base pages, replacing the MAP_POPULATE dropped
    // above. Touching one byte per page now that VM_NOHUGEPAGE is set forces
    // the kernel to back each page with a 4 KiB base page up front (and keeps
    // the page-fault cost out of the timed transfer loop).
    {
        volatile uint8_t* touch = static_cast<volatile uint8_t*>(mapping.data);
        for (uint64_t off = 0; off < size; off += BASE_TRANSFER_STEP_SIZE) {
            touch[off] = 0;
        }
    }

    mapping.step = BASE_TRANSFER_STEP_SIZE;
    return mapping;
}

/// Release a host mapping created by createHostMapping().
inline void destroyHostMapping(HostMapping& mapping) noexcept {
    if (mapping.data != nullptr && mapping.data != MAP_FAILED) {
        (void)munmap(mapping.data, mapping.size);
        mapping.data = nullptr;
    }
}

/// Validate that a [offset, offset+size) request is aligned and in range for a
/// buffer of @p bufSize bytes backing device address @p physAddr, given the
/// mapping's @p step.
inline void validateSyncRange(uint64_t offset, uint64_t size, uint64_t bufSize,
                              uint64_t physAddr, uint64_t step) {
    if (step == 0 || size == 0) {
        throw std::invalid_argument("Invalid raw transfer size");
    }
    if ((offset % step) != 0 || (size % step) != 0 ||
        (bufSize % step) != 0 || (physAddr % step) != 0) {
        throw std::invalid_argument("Raw transfer range is not aligned to the host mapping step");
    }
    if (offset > bufSize || size > bufSize - offset) {
        throw std::out_of_range("Raw transfer range exceeds buffer size");
    }
    // Both granules transfer the whole range in a single pread/pwrite, so the
    // size must fit in ssize_t regardless of step.
    if (size > static_cast<uint64_t>(std::numeric_limits<ssize_t>::max())) {
        throw std::invalid_argument("Raw transfer size exceeds syscall limit");
    }
}

/// Perform a raw memory-mapped QDMA transfer over @p fd using pread/pwrite,
/// with the device (endpoint) address encoded as the file offset.
///
/// @param fd        Per-queue char device / queue-pair fd.
/// @param data      Host staging buffer base.
/// @param physAddr  Device-side base address for this buffer.
/// @param offset    Byte offset within the buffer (and added to physAddr).
/// @param size      Number of bytes to transfer.
/// @param step      Mapping step size (see HostMapping::step).
/// @param toDevice  true for H2C (pwrite), false for C2H (pread).
inline void rawTransfer(int fd, void* data, uint64_t physAddr, uint64_t offset,
                        uint64_t size, [[maybe_unused]] uint64_t step,
                        bool toDevice) {
    // Issue the whole range in a single syscall regardless of page granule.
    // The kernel pins every page in the range and builds one descriptor per
    // page, submitting a single multi-descriptor libqdma request (libqdma
    // refills the descriptor ring as needed). This keeps syscall/submit
    // overhead independent of the page size -- the 4 KiB path no longer costs
    // one syscall (and one single-descriptor DMA) per page.
    const uint64_t syscallSize = size;
    const uint64_t endOffset = offset + size;
#if SLASH_QDMA_TIMING
    const auto xferStart = std::chrono::steady_clock::now();
#endif

    for (uint64_t currOffset = offset; currOffset < endOffset; currOffset += syscallSize) {
        uint64_t transferred = 0;
        while (transferred < syscallSize) {
            const auto* src = static_cast<const uint8_t*>(data) + currOffset + transferred;
            auto* dst = static_cast<uint8_t*>(data) + currOffset + transferred;
            const size_t remaining = static_cast<size_t>(syscallSize - transferred);
            const off_t devOffset = static_cast<off_t>(physAddr + currOffset + transferred);

#if SLASH_QDMA_TIMING
            const auto callStart = std::chrono::steady_clock::now();
#endif
            ssize_t ret = toDevice
                ? pwrite(fd, src, remaining, devOffset)
                : pread(fd, dst, remaining, devOffset);

            if (ret < 0 && errno == EINTR) {
                continue;
            }
            if (ret <= 0) {
                throwSystemError(toDevice ? "Raw QDMA write failed" : "Raw QDMA read failed");
            }
#if SLASH_QDMA_TIMING
            const auto callNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - callStart)
                                    .count();
            std::fprintf(stderr,
                         "validate: timing %s dev=0x%llx bytes=%zu ret=%zd syscall=%lld ns\n",
                         toDevice ? "H2C" : "C2H",
                         static_cast<unsigned long long>(devOffset), remaining, ret,
                         static_cast<long long>(callNs));
#endif
            transferred += static_cast<uint64_t>(ret);
        }
    }

#if SLASH_QDMA_TIMING
    const auto totalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - xferStart)
                             .count();
    const double mb = static_cast<double>(size) / (1024.0 * 1024.0);
    const double sec = static_cast<double>(totalNs) / 1e9;
    std::fprintf(stderr,
                 "validate: timing %s xfer dev=0x%llx size=%llu step=%llu total=%lld ns (%.1f MB/s)\n",
                 toDevice ? "H2C" : "C2H",
                 static_cast<unsigned long long>(physAddr + offset),
                 static_cast<unsigned long long>(size),
                 static_cast<unsigned long long>(step), static_cast<long long>(totalNs),
                 sec > 0.0 ? mb / sec : 0.0);
#endif
}

} // namespace smi::raw

#endif // SMI_RAW_TRANSFER_HPP
