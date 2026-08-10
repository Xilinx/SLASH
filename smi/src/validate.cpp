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

/// @file validate.cpp
/// @brief Implementation of the Validate command.
///
/// Resets a V80 board, then exercises HBM and DDR memory over PCIe:
///   1. Data integrity checks (write pattern, read back, verify).
///   2. Parallel bandwidth measurements (N threads, one per buffer).
///
/// We use libvrtdpp (vrtd::Session / vrtd::Device / vrtd::Buffer) directly
/// rather than the higher-level vrt::Device because vrt::Device requires a
/// vrtbin path for system-map parsing, which is unnecessary for raw memory
/// validation.
///
/// TODO: Decide whether vrt::Device should gain a vrtbin-less constructor so
///       that commands like validate can go through the standard vrt:: layer.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "validate.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <exception>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <glob.h>
#include <sys/mman.h>
#include <unistd.h>

#include <vrtd/session.hpp>

#include "bdf.hpp"
#include "raw_transfer.hpp"

#ifdef SMI_ENABLE_QDMA_DRIVER_BACKEND
#include "qdma_driver_backend.hpp"
#endif

extern "C" {
#include <slash/qdma.h>
}

namespace {

using smi::raw::throwSystemError;

/// Region constants mirror vrt/vrtd/src/allocator.h, which is private.
static constexpr uint64_t HBM_BASE = 0x4000000000ULL;
static constexpr uint64_t DDR_BASE = 0x60000000000ULL;
static constexpr uint64_t MEM_REGION_SIZE = 512ULL * 1024 * 1024;
static constexpr uint64_t MEMORY_SPACE_SIZE = 64ULL * MEM_REGION_SIZE;
static constexpr uint64_t MAX_BUFFER_SIZE = MEM_REGION_SIZE;
static constexpr uint64_t TRANSFER_ALIGNMENT = 4096ULL;

static constexpr uint32_t QDMA_Q_MODE_MM = 0;
static constexpr uint32_t QDMA_DIR_H2C = 0x1;
static constexpr uint32_t QDMA_DIR_C2H = 0x2;
static constexpr uint32_t QDMA_RING_SZ_IDX = 0;

/// Required alignment for placement sizes/offsets: the QDMA transfer alignment
/// (4 KiB base pages).
static uint64_t requiredAlignment(const Validate::Options& options) {
    (void)options;
    return TRANSFER_ALIGNMENT;
}

/// Per-buffer AXI-MM channel selection.  A single-element list applies to every
/// buffer; otherwise the list has exactly one entry per logical position
/// (validated in validatePlacement) and is indexed directly.
static Validate::Options::MmChannel mmChannelForPosition(const Validate::Options& options,
                                                         uint64_t position) {
    const auto& list = options.mmChannels;
    return list.size() == 1 ? list.front() : list[position];
}

/// Map the per-buffer channel selection to the vrtd channel enum.
static vrtd::MmChannel vrtdMmChannel(const Validate::Options& options, uint64_t position) {
    switch (mmChannelForPosition(options, position)) {
    case Validate::Options::MmChannel::Ch0: return vrtd::MmChannel::Ch0;
    case Validate::Options::MmChannel::Ch1: return vrtd::MmChannel::Ch1;
    case Validate::Options::MmChannel::Auto:
    default: return vrtd::MmChannel::Auto;
    }
}

/// Map the per-buffer channel selection to the SLASH UAPI channel enum.
static slash_qdma_mm_channel slashMmChannel(const Validate::Options& options, uint64_t position) {
    switch (mmChannelForPosition(options, position)) {
    case Validate::Options::MmChannel::Ch0: return SLASH_QDMA_MM_CHANNEL_0;
    case Validate::Options::MmChannel::Ch1: return SLASH_QDMA_MM_CHANNEL_1;
    case Validate::Options::MmChannel::Auto:
    default: return SLASH_QDMA_MM_CHANNEL_AUTO;
    }
}

/// Map the per-buffer channel selection to a concrete channel for the
/// off-the-shelf QDMA driver; -1 means auto (queue spreads by qid % channels).
static int qdmaDriverMmChannel(const Validate::Options& options, uint64_t position) {
    switch (mmChannelForPosition(options, position)) {
    case Validate::Options::MmChannel::Ch0: return 0;
    case Validate::Options::MmChannel::Ch1: return 1;
    case Validate::Options::MmChannel::Auto:
    default: return -1;
    }
}

static std::string trim(std::string_view text) {
    size_t first = 0;
    while (first < text.size() &&
           std::isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }

    size_t last = text.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }

    return std::string{text.substr(first, last - first)};
}

static uint64_t parseByteSizeText(std::string_view text) {
    std::string value = trim(text);
    if (value.empty()) {
        throw std::invalid_argument("value must not be empty");
    }

    uint64_t multiplier = 1;
    if (!value.empty() && (value.back() == 'b' || value.back() == 'B')) {
        value.pop_back();
    }
    if (!value.empty()) {
        const char suffix = value.back();
        if (suffix == 'k' || suffix == 'K') {
            multiplier = 1024ULL;
            value.pop_back();
        } else if (suffix == 'm' || suffix == 'M') {
            multiplier = 1024ULL * 1024ULL;
            value.pop_back();
        } else if (suffix == 'g' || suffix == 'G') {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
            value.pop_back();
        }
    }

    value = trim(value);
    if (value.empty() || value.front() == '-' || value.front() == '+') {
        throw std::invalid_argument("value must be an unsigned byte count");
    }

    size_t parsed = 0;
    uint64_t bytes = 0;
    try {
        bytes = std::stoull(value, &parsed, 0);
    } catch (const std::exception&) {
        throw std::invalid_argument("value must be an unsigned byte count");
    }

    if (parsed != value.size()) {
        throw std::invalid_argument("unrecognized byte-size suffix");
    }
    if (bytes > std::numeric_limits<uint64_t>::max() / multiplier) {
        throw std::invalid_argument("byte-size value is too large");
    }

    return bytes * multiplier;
}

static bool isAligned(uint64_t value, uint64_t alignment) {
    return (value % alignment) == 0;
}

static bool checkAligned(const char* name, uint64_t value, uint64_t alignment) {
    if (!isAligned(value, alignment)) {
        std::cerr << "validate: " << name << " must be " << alignment
                  << "-byte aligned" << std::endl;
        return false;
    }
    return true;
}

static bool checkMemoryPlacementRange(const char* memoryName,
                                      const Validate::Options& options,
                                      uint64_t positions) {
    if (positions == 0) {
        return true;
    }

    const uint64_t lastPosition = positions - 1;
    if (lastPosition != 0 &&
        options.offset > (std::numeric_limits<uint64_t>::max() - options.startingOffset) /
                             lastPosition) {
        std::cerr << "validate: " << memoryName
                  << " placement overflows 64-bit address arithmetic" << std::endl;
        return false;
    }

    const uint64_t lastStart = options.startingOffset + lastPosition * options.offset;
    if (lastStart > MEMORY_SPACE_SIZE || options.bufferSize > MEMORY_SPACE_SIZE - lastStart) {
        std::cerr << "validate: " << memoryName << " placement exceeds available "
                  << (MEMORY_SPACE_SIZE / (1024ULL * 1024ULL)) << " MiB address space"
                  << std::endl;
        return false;
    }

    return true;
}

/// Paired-mode per-channel region stride (NSU / pseudo-channel spacing),
/// resolving 0 to half the per-memory address space.
static uint64_t pairedRegionStride(const Validate::Options& options) {
    return options.channelRegionStride != 0 ? options.channelRegionStride
                                            : (MEMORY_SPACE_SIZE / 2);
}

/// Placement check for Paired channel allocation: even/odd positions occupy two
/// regions `pairedRegionStride()` bytes apart, each packed by in-region index.
/// Verifies neither region overflows into the next nor past the memory space.
static bool checkMemoryPlacementRangePaired(const char* memoryName,
                                            const Validate::Options& options,
                                            uint64_t positions) {
    if (positions == 0) {
        return true;
    }

    const uint64_t stride = pairedRegionStride(options);
    const uint64_t alignment = requiredAlignment(options);
    if (stride == 0 || (stride % alignment) != 0) {
        std::cerr << "validate: --channel-region-stride must be a non-zero multiple of "
                  << alignment << " bytes" << std::endl;
        return false;
    }
    if (stride > MEMORY_SPACE_SIZE) {
        std::cerr << "validate: --channel-region-stride exceeds the "
                  << (MEMORY_SPACE_SIZE / (1024ULL * 1024ULL)) << " MiB per-memory address space"
                  << std::endl;
        return false;
    }

    // Highest in-region index used across both regions (positions 0..positions-1,
    // split even/odd, each using index = position >> 1).
    const uint64_t maxIndex = (positions - 1) >> 1;
    if (maxIndex != 0 &&
        options.offset > (std::numeric_limits<uint64_t>::max() - options.startingOffset) / maxIndex) {
        std::cerr << "validate: " << memoryName
                  << " paired placement overflows 64-bit address arithmetic" << std::endl;
        return false;
    }
    const uint64_t lastStart = options.startingOffset + maxIndex * options.offset;

    // Each region must hold its last buffer without spilling into the next region.
    if (lastStart > stride || options.bufferSize > stride - lastStart) {
        std::cerr << "validate: " << memoryName
                  << " paired placement overflows the per-channel region (stride " << stride
                  << " bytes); reduce --threads/--buffer-size/--offset or raise"
                     " --channel-region-stride" << std::endl;
        return false;
    }
    // Region 1 sits one stride higher and must still fit the memory space.
    if (lastStart + options.bufferSize > MEMORY_SPACE_SIZE - stride) {
        std::cerr << "validate: " << memoryName
                  << " paired placement exceeds available "
                  << (MEMORY_SPACE_SIZE / (1024ULL * 1024ULL)) << " MiB address space"
                  << std::endl;
        return false;
    }
    return true;
}

static bool validatePlacement(const Validate::Options& options) {
    const uint64_t positions = 2ULL * options.threads;
    if (options.mmChannels.size() != 1 && options.mmChannels.size() != positions) {
        std::cerr << "validate: --mm-channel list must have exactly 1 or " << positions
                  << " entries (one per buffer position = 2 x --threads); got "
                  << options.mmChannels.size() << std::endl;
        return false;
    }

    if (options.bufferSize == 0 || options.bufferSize > MAX_BUFFER_SIZE) {
        std::cerr << "validate: --buffer-size must be in the range 1..512M" << std::endl;
        return false;
    }
    if (options.offset == 0) {
        std::cerr << "validate: --offset must be greater than zero" << std::endl;
        return false;
    }
    const uint64_t alignment = requiredAlignment(options);
    if (!checkAligned("--buffer-size", options.bufferSize, alignment) ||
        !checkAligned("--offset", options.offset, alignment) ||
        !checkAligned("--starting-offset", options.startingOffset, alignment)) {
        return false;
    }
    if (options.offset < options.bufferSize) {
        std::cerr << "validate: --offset must be at least --buffer-size so buffers do not overlap"
                  << std::endl;
        return false;
    }

    const bool paired =
        options.channelAllocation == Validate::Options::ChannelAllocation::Paired;
    if (paired && !options.rawTransferTest && !options.useQdmaDriver) {
        std::cerr << "validate: --channel-allocation paired only applies to the raw transfer"
                     " tests (--raw-transfer-test or --use-qdma-driver)" << std::endl;
        return false;
    }
    if ((options.bandwidthIterations > 1 || options.bandwidthDuration > 0.0) &&
        !options.rawTransferTest && !options.useQdmaDriver) {
        std::cerr << "validate: --bandwidth-iterations/--bandwidth-duration only apply to the raw transfer"
                     " tests (--raw-transfer-test or --use-qdma-driver)" << std::endl;
        return false;
    }
    if (options.ringSizeIndex.has_value() &&
        !options.rawTransferTest && !options.useQdmaDriver) {
        std::cerr << "validate: --ring-size-index only applies to the raw transfer"
                     " tests (--raw-transfer-test or --use-qdma-driver)" << std::endl;
        return false;
    }
    if (options.bandwidthDuration < 0.0) {
        std::cerr << "validate: --bandwidth-duration must be non-negative" << std::endl;
        return false;
    }
    if (options.ringSizeIndex.has_value() && *options.ringSizeIndex > 15) {
        std::cerr << "validate: --ring-size-index must be in the range 0..15" << std::endl;
        return false;
    }

    const auto checkRange = paired ? checkMemoryPlacementRangePaired : checkMemoryPlacementRange;
    if (!options.ddrOnly && !checkRange("HBM", options, positions)) {
        return false;
    }
    if (!options.hbmOnly && !checkRange("DDR", options, positions)) {
        return false;
    }

    return true;
}

static uint64_t addressFor(uint64_t memoryBase,
                           const Validate::Options& options,
                           uint64_t position) {
    return memoryBase + options.startingOffset + position * options.offset;
}

/// Device address for a raw-transfer buffer, honouring the channel-allocation
/// strategy.  In Paired mode the mm-channel (position&1 -- which SLASH maps to
/// the SW-context host_id and hence the CPM5 NoC NMU) is coupled to a distinct
/// memory region (NSU): even positions land in region 0, odd positions in
/// region 1, pairedRegionStride() bytes higher, each packed by its in-region
/// index.  This mirrors dma-perf's offset_ch0/offset_ch1 so the two NMUs drive
/// independent memory endpoints instead of converging on one.
static uint64_t rawAddressFor(uint64_t memoryBase,
                              const Validate::Options& options,
                              uint64_t position) {
    if (options.channelAllocation == Validate::Options::ChannelAllocation::Paired) {
        const uint64_t channel = position & 1ULL;
        const uint64_t inRegionIndex = position >> 1;
        return memoryBase + channel * pairedRegionStride(options) +
               options.startingOffset + inRegionIndex * options.offset;
    }
    return addressFor(memoryBase, options, position);
}

/// Print which raw-transfer channel-allocation strategy is in effect.
static void printChannelAllocation(const Validate::Options& options) {
    if (options.channelAllocation == Validate::Options::ChannelAllocation::Paired) {
        std::cout << "Channel allocation: paired (even positions -> mm-channel 0 / region 0, "
                     "odd -> mm-channel 1 / region 1; region stride 0x"
                  << std::hex << pairedRegionStride(options) << std::dec << " bytes)" << std::endl;
    } else {
        std::cout << "Channel allocation: auto (mm-channel = qid&1, linear addressing)" << std::endl;
    }
}

/// Print the raw-transfer queue ring-size override, when one was requested.
static void printRingSizeIndex(const Validate::Options& options) {
    if (options.ringSizeIndex.has_value()) {
        std::cout << "QDMA ring size index: " << *options.ringSizeIndex << std::endl;
    }
}

/// Print the per-buffer AXI-MM channel selection in effect.
static void printMmChannel(const Validate::Options& options) {
    std::cout << "MM channel: ";
    for (size_t i = 0; i < options.mmChannels.size(); ++i) {
        if (i != 0) {
            std::cout << ",";
        }
        switch (options.mmChannels[i]) {
        case Validate::Options::MmChannel::Ch0: std::cout << "0"; break;
        case Validate::Options::MmChannel::Ch1: std::cout << "1"; break;
        case Validate::Options::MmChannel::Auto:
        default: std::cout << "auto"; break;
        }
    }
    std::cout << (options.mmChannels.size() == 1 ? " (all buffers)" : " (per buffer position)")
              << std::endl;
}

static bool checkHostMemoryBudget(const Validate::Options& options) {
    const uint64_t maxConcurrentBuffers = (!options.ddrOnly && !options.hbmOnly)
        ? 4ULL * options.threads
        : 2ULL * options.threads;
    const uint64_t requiredBytes = maxConcurrentBuffers * options.bufferSize;

    const long pageSize = sysconf(_SC_PAGESIZE);
    const long availablePages = sysconf(_SC_AVPHYS_PAGES);

    if (pageSize <= 0 || availablePages <= 0) {
        std::cerr << "Warning: unable to estimate available host memory for validate; "
                  << "peak mapped buffer footprint is "
                  << (requiredBytes / (1024ULL * 1024ULL)) << " MiB." << std::endl;
        return true;
    }

    const auto availableBytes = static_cast<uint64_t>(pageSize) *
        static_cast<uint64_t>(availablePages);
    if (requiredBytes > availableBytes) {
        std::cerr << "validate: requested test can map up to "
                  << (requiredBytes / (1024ULL * 1024ULL)) << " MiB of host buffers, "
                  << "but only about " << (availableBytes / (1024ULL * 1024ULL))
                  << " MiB is currently available. Reduce --threads or use --ddr-only/--hbm-only."
                  << std::endl;
        return false;
    }

    return true;
}

static void warnIfNotRoot(const char* mode) {
    if (geteuid() != 0) {
        std::cerr << "Warning: " << mode
                  << " usually needs root or udev-granted access to QDMA device nodes/sysfs."
                  << std::endl;
    }
}

class RawQdmaDevice {
public:
    explicit RawQdmaDevice(const std::string& path) : qdma_{slash_qdma_open(path.c_str())} {
        if (qdma_ == nullptr) {
            throwSystemError("Failed to open QDMA device " + path);
        }
    }

    RawQdmaDevice(const RawQdmaDevice&) = delete;
    RawQdmaDevice& operator=(const RawQdmaDevice&) = delete;

    RawQdmaDevice(RawQdmaDevice&& other) noexcept : qdma_{other.qdma_} {
        other.qdma_ = nullptr;
    }

    RawQdmaDevice& operator=(RawQdmaDevice&& other) noexcept {
        if (this != &other) {
            cleanup();
            qdma_ = other.qdma_;
            other.qdma_ = nullptr;
        }
        return *this;
    }

    ~RawQdmaDevice() {
        cleanup();
    }

    slash_qdma* get() const {
        return qdma_;
    }

private:
    void cleanup() {
        if (qdma_ != nullptr) {
            (void)slash_qdma_close(qdma_);
            qdma_ = nullptr;
        }
    }

    slash_qdma* qdma_ = nullptr;
};

std::string resolveQdmaDevicePath(const std::string& boardBdf) {
    struct GlobResult {
        glob_t paths{};
        ~GlobResult() { globfree(&paths); }
    } devices;

    const int ret = glob("/dev/slash_qdma_ctl*", GLOB_ERR, nullptr, &devices.paths);
    if (ret == GLOB_NOMATCH) {
        throw std::runtime_error("No QDMA devices found matching /dev/slash_qdma_ctl*");
    }
    if (ret != 0) {
        throw std::runtime_error("Failed to enumerate /dev/slash_qdma_ctl*");
    }

    const std::string expectedBdf = boardBdf + ".1";
    for (size_t i = 0; i < devices.paths.gl_pathc; ++i) {
        const std::string path = devices.paths.gl_pathv[i];
        RawQdmaDevice qdma(path);

        struct slash_qdma_info info{};
        if (slash_qdma_info_read(qdma.get(), &info) != 0) {
            throwSystemError("Failed to read QDMA info from " + path);
        }
        if (std::strncmp(info.bdf, expectedBdf.c_str(), sizeof(info.bdf)) == 0) {
            return path;
        }
    }

    throw std::runtime_error("No QDMA device found for PF1 " + expectedBdf);
}

class RawTransferBuffer {
public:
    RawTransferBuffer(slash_qdma* qdma, uint64_t physAddr, uint64_t size,
                      slash_qdma_mm_channel mmChannel,
                      uint32_t ringSizeIndex)
        : qdma_{qdma}, physAddr_{physAddr}, size_{size},
          mmChannel_{mmChannel}, ringSizeIndex_{ringSizeIndex} {
        try {
            createBuffer();
            createQpair();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    RawTransferBuffer(const RawTransferBuffer&) = delete;
    RawTransferBuffer& operator=(const RawTransferBuffer&) = delete;

    RawTransferBuffer(RawTransferBuffer&& other) noexcept {
        moveFrom(other);
    }

    RawTransferBuffer& operator=(RawTransferBuffer&& other) noexcept {
        if (this != &other) {
            cleanup();
            moveFrom(other);
        }
        return *this;
    }

    ~RawTransferBuffer() {
        cleanup();
    }

    void* data() {
        return data_;
    }

    uint64_t getSize() const {
        return size_;
    }

    void syncToDevice(uint64_t offset, uint64_t size) {
        validateSyncRange(offset, size);
        transfer(offset, size, /*toDevice=*/true);
    }

    void syncFromDevice(uint64_t offset, uint64_t size) {
        validateSyncRange(offset, size);
        transfer(offset, size, /*toDevice=*/false);
    }

private:
    void moveFrom(RawTransferBuffer& other) noexcept {
        qdma_ = other.qdma_;
        fd_ = other.fd_;
        qid_ = other.qid_;
        qpairCreated_ = other.qpairCreated_;
        qpairStarted_ = other.qpairStarted_;
        buf_ = other.buf_;
        data_ = other.data_;
        physAddr_ = other.physAddr_;
        size_ = other.size_;
        transferStepSize_ = other.transferStepSize_;
        mmChannel_ = other.mmChannel_;
        ringSizeIndex_ = other.ringSizeIndex_;

        other.qdma_ = nullptr;
        other.fd_ = -1;
        other.qid_ = 0;
        other.qpairCreated_ = false;
        other.qpairStarted_ = false;
        other.buf_ = slash_qdma_buffer{};
        other.data_ = nullptr;
        other.physAddr_ = 0;
        other.size_ = 0;
        other.transferStepSize_ = 0;
        other.ringSizeIndex_ = QDMA_RING_SZ_IDX;
    }

    void createBuffer() {
        // The kernel owns the DMA buffer (pages + SGL + DMA map built once at
        // create time); we mmap it for CPU access via buf_.addr.
        if (slash_qdma_buffer_create(qdma_, size_, &buf_) != 0) {
            throwSystemError("Failed to create raw transfer DMA buffer");
        }
        data_ = buf_.addr;
        transferStepSize_ = smi::raw::BASE_TRANSFER_STEP_SIZE;

        // Pre-fault the mapping so the page-fault cost stays out of the timed
        // transfer loop.
        auto* touch = static_cast<volatile uint8_t*>(data_);
        for (uint64_t off = 0; off < size_; off += transferStepSize_) {
            touch[off] = 0;
        }
    }

    void createQpair() {
        if (qdma_ == nullptr || size_ == 0) {
            throw std::invalid_argument("Invalid raw transfer buffer arguments");
        }

        struct slash_qdma_qpair_add req{};
        req.size = sizeof(req);
        req.mode = QDMA_Q_MODE_MM;
        req.dir_mask = QDMA_DIR_H2C | QDMA_DIR_C2H;
        req.mm_channel = mmChannel_;
        req.h2c_ring_sz = ringSizeIndex_;
        req.c2h_ring_sz = ringSizeIndex_;
        req.cmpt_ring_sz = ringSizeIndex_;

        if (slash_qdma_qpair_add(qdma_, &req) != 0) {
            throwSystemError("Failed to add raw transfer QDMA queue pair");
        }
        qid_ = req.qid;
        qpairCreated_ = true;

        if (slash_qdma_qpair_start(qdma_, qid_) != 0) {
            throwSystemError("Failed to start raw transfer QDMA queue pair");
        }
        qpairStarted_ = true;

        fd_ = slash_qdma_qpair_get_fd(qdma_, qid_, O_CLOEXEC);
        if (fd_ < 0) {
            throwSystemError("Failed to get raw transfer QDMA queue fd");
        }
    }

    void validateSyncRange(uint64_t offset, uint64_t size) const {
        smi::raw::validateSyncRange(offset, size, size_, physAddr_, transferStepSize_);
    }

    void transfer(uint64_t offset, uint64_t size, bool toDevice) {
        // Issue via the array transfer ioctl with a single sub-transfer on this
        // buffer's queue pair (qpair_index 0).  Channel parallelism for the
        // bandwidth test comes from running many buffers concurrently, each
        // pinned to a channel by mm_channel (see the channel-allocation knobs).
        struct slash_qdma_subxfer xfer{};
        xfer.qpair_index = 0;
        xfer.direction = toDevice ? SLASH_QDMA_XFER_H2C : SLASH_QDMA_XFER_C2H;
        xfer.buf_fd = buf_.fd;
        xfer.buf_offset = offset;
        xfer.dev_addr = physAddr_ + offset;
        xfer.length = size;

        ssize_t n = slash_qdma_qpair_transfer_batch(fd_, &xfer, 1);
        if (n < 0) {
            throwSystemError(toDevice ? "Raw QDMA write failed"
                                      : "Raw QDMA read failed");
        }
        if (static_cast<uint64_t>(n) != size) {
            throw std::runtime_error("Raw QDMA transfer moved fewer bytes than requested");
        }
    }

    void cleanup() {
        if (fd_ >= 0) {
            (void)close(fd_);
            fd_ = -1;
        }
        if (qpairStarted_) {
            (void)slash_qdma_qpair_stop(qdma_, qid_);
            qpairStarted_ = false;
        }
        if (qpairCreated_) {
            (void)slash_qdma_qpair_del(qdma_, qid_);
            qpairCreated_ = false;
        }
        if (buf_.addr != nullptr) {
            (void)slash_qdma_buffer_destroy(&buf_);
            buf_ = slash_qdma_buffer{};
        }
        data_ = nullptr;
    }

    slash_qdma* qdma_ = nullptr;
    int fd_ = -1;
    uint32_t qid_ = 0;
    bool qpairCreated_ = false;
    bool qpairStarted_ = false;
    slash_qdma_buffer buf_{};
    void* data_ = nullptr;
    uint64_t physAddr_ = 0;
    uint64_t size_ = 0;
    uint64_t transferStepSize_ = 0;
    slash_qdma_mm_channel mmChannel_ = SLASH_QDMA_MM_CHANNEL_AUTO;
    uint32_t ringSizeIndex_ = QDMA_RING_SZ_IDX;
};

/// Fill @p buf with a deterministic pattern seeded by @p seed.
static void fillPattern(void* buf, uint64_t size, uint32_t seed) {
    auto* p = static_cast<uint32_t*>(buf);
    uint64_t count = size / sizeof(uint32_t);
    for (uint64_t i = 0; i < count; ++i) {
        p[i] = static_cast<uint32_t>(i) ^ seed;
    }
}

/// Verify @p buf matches the pattern produced by fillPattern().
/// Returns true on match, false on first mismatch.
static bool verifyPattern(const void* buf, uint64_t size, uint32_t seed) {
    auto* p = static_cast<const uint32_t*>(buf);
    uint64_t count = size / sizeof(uint32_t);
    for (uint64_t i = 0; i < count; ++i) {
        if (p[i] != (static_cast<uint32_t>(i) ^ seed)) {
            return false;
        }
    }
    return true;
}

/// Run data integrity on every buffer: write pattern → sync to device →
/// clear host → sync from device → verify.
///
/// Output policy: per-buffer FAIL lines are printed as failures occur; OK
/// buffers are silent.  A single summary line ("N/N OK" or "M/N OK, K
/// FAIL") is printed at the end.
///
/// @return true if all buffers pass.
template<typename Buffer>
static bool testDataIntegrity(std::vector<Buffer>& buffers,
                              const std::string& label) {
    const size_t total = buffers.size();
    size_t passed = 0;

    for (size_t i = 0; i < total; ++i) {
        auto& buf = buffers[i];
        uint32_t seed = static_cast<uint32_t>(i);
        uint64_t size = buf.getSize();

        fillPattern(buf.data(), size, seed);
        buf.syncToDevice(0, size);

        std::memset(buf.data(), 0, size);
        buf.syncFromDevice(0, size);

        if (verifyPattern(buf.data(), size, seed)) {
            ++passed;
        } else {
            std::cout << "    " << label << i << ": FAIL" << std::endl;
        }
    }

    if (passed == total) {
        std::cout << "    " << total << "/" << total << " OK" << std::endl;
    } else {
        std::cout << "    " << passed << "/" << total << " OK, "
                  << (total - passed) << " FAIL" << std::endl;
    }

    return passed == total;
}

static double mbPerSecond(uint64_t bytes, std::chrono::duration<double> elapsed) {
    const double totalMB = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return totalMB / elapsed.count();
}

static void printBandwidthMetric(const char* label, double mbps) {
    std::cout << "    " << label << ": " << std::fixed << std::setprecision(2)
              << mbps << " MB/s" << std::endl;
}

struct BandwidthRepeatOptions {
    uint64_t iterations = 1;
    std::chrono::duration<double> duration{0.0};

    bool durationMode() const {
        return duration.count() > 0.0;
    }

    bool isRepeated() const {
        return durationMode() || iterations > 1;
    }
};

static BandwidthRepeatOptions repeatOptionsFromValidate(const Validate::Options& options) {
    BandwidthRepeatOptions repeat;
    repeat.iterations = std::max<uint64_t>(1, options.bandwidthIterations);
    repeat.duration = std::chrono::duration<double>(options.bandwidthDuration);
    return repeat;
}

static void printBandwidthRepeatMode(const BandwidthRepeatOptions& repeat) {
    if (repeat.durationMode()) {
        std::cout << "Bandwidth mode: duration " << std::fixed << std::setprecision(3)
                  << repeat.duration.count() << " s" << std::endl;
    } else if (repeat.iterations > 1) {
        std::cout << "Bandwidth mode: " << repeat.iterations << " iterations" << std::endl;
    }
}

template<typename Buffer>
static uint64_t fillBuffers(std::vector<Buffer>& buffers, int value) {
    uint64_t totalBytes = 0;
    for (auto& buf : buffers) {
        std::memset(buf.data(), value, buf.getSize());
        totalBytes += buf.getSize();
    }
    return totalBytes;
}

template<typename Buffer>
static void launchTransferThreads(std::vector<Buffer>& buffers,
                                  bool toDevice,
                                  std::vector<std::thread>& threads,
                                  std::vector<std::exception_ptr>& errors,
                                  size_t errorOffset) {
    for (size_t i = 0; i < buffers.size(); ++i) {
        threads.emplace_back([&buffers, &errors, i, errorOffset, toDevice] {
            try {
                if (toDevice) {
                    buffers[i].syncToDevice(0, buffers[i].getSize());
                } else {
                    buffers[i].syncFromDevice(0, buffers[i].getSize());
                }
            } catch (...) {
                errors[errorOffset + i] = std::current_exception();
            }
        });
    }
}

template<typename Buffer>
static void runTransfers(std::vector<Buffer>& buffers, bool toDevice) {
    std::vector<std::thread> threads;
    std::vector<std::exception_ptr> errors(buffers.size());
    threads.reserve(buffers.size());

    launchTransferThreads(buffers, toDevice, threads, errors, 0);

    for (auto& t : threads) {
        t.join();
    }
    for (auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
}

static uint64_t joinRepeatedTransferThreads(std::vector<std::thread>& threads,
                                            std::vector<std::exception_ptr>& errors,
                                            const std::vector<uint64_t>& bytes) {
    for (auto& t : threads) {
        t.join();
    }
    for (auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }

    uint64_t totalBytes = 0;
    for (uint64_t value : bytes) {
        totalBytes += value;
    }
    return totalBytes;
}

template<typename Buffer>
static std::pair<uint64_t, std::chrono::duration<double>>
runRepeatedTransfers(std::vector<Buffer>& buffers,
                     bool toDevice,
                     const BandwidthRepeatOptions& repeat) {
    std::vector<std::thread> threads;
    std::vector<std::exception_ptr> errors(buffers.size());
    std::vector<uint64_t> bytes(buffers.size(), 0);
    threads.reserve(buffers.size());

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + repeat.duration;

    for (size_t i = 0; i < buffers.size(); ++i) {
        threads.emplace_back([&buffers, &errors, &bytes, i, toDevice, repeat, deadline] {
            try {
                const uint64_t size = buffers[i].getSize();
                uint64_t completed = 0;

                if (repeat.durationMode()) {
                    while (std::chrono::steady_clock::now() < deadline) {
                        if (toDevice) {
                            buffers[i].syncToDevice(0, size);
                        } else {
                            buffers[i].syncFromDevice(0, size);
                        }
                        ++completed;
                    }
                } else {
                    for (uint64_t iter = 0; iter < repeat.iterations; ++iter) {
                        if (toDevice) {
                            buffers[i].syncToDevice(0, size);
                        } else {
                            buffers[i].syncFromDevice(0, size);
                        }
                        ++completed;
                    }
                }

                bytes[i] = completed * size;
            } catch (...) {
                errors[i] = std::current_exception();
            }
        });
    }

    const uint64_t totalBytes = joinRepeatedTransferThreads(threads, errors, bytes);
    const auto end = std::chrono::steady_clock::now();
    return {totalBytes, end - start};
}

template<typename Buffer>
static double testSingleDirectionBandwidth(std::vector<Buffer>& buffers,
                                           bool toDevice,
                                           const BandwidthRepeatOptions& repeat = {}) {
    (void)fillBuffers(buffers, toDevice ? 0xAB : 0xCD);

    if (!toDevice) {
        runTransfers(buffers, /*toDevice=*/true);
        for (auto& buf : buffers) {
            std::memset(buf.data(), 0, buf.getSize());
        }
    }

    const auto [totalBytes, elapsed] = runRepeatedTransfers(buffers, toDevice, repeat);

    return mbPerSecond(totalBytes, elapsed);
}

template<typename Buffer>
static void testBidirectionalBandwidth(std::vector<Buffer>& writeBuffers,
                                       std::vector<Buffer>& readBuffers,
                                       const BandwidthRepeatOptions& repeat = {}) {
    (void)fillBuffers(writeBuffers, 0xAB);
    (void)fillBuffers(readBuffers, 0xCD);

    // Prime device memory before timing so the C2H side reads initialized data.
    runTransfers(readBuffers, /*toDevice=*/true);
    for (auto& buf : readBuffers) {
        std::memset(buf.data(), 0, buf.getSize());
    }

    std::vector<std::thread> threads;
    std::vector<std::exception_ptr> errors(writeBuffers.size() + readBuffers.size());
    std::vector<uint64_t> writeThreadBytes(writeBuffers.size(), 0);
    std::vector<uint64_t> readThreadBytes(readBuffers.size(), 0);
    threads.reserve(errors.size());

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + repeat.duration;

    for (size_t i = 0; i < writeBuffers.size(); ++i) {
        threads.emplace_back([&writeBuffers, &errors, &writeThreadBytes, i, repeat, deadline] {
            try {
                const uint64_t size = writeBuffers[i].getSize();
                uint64_t completed = 0;

                if (repeat.durationMode()) {
                    while (std::chrono::steady_clock::now() < deadline) {
                        writeBuffers[i].syncToDevice(0, size);
                        ++completed;
                    }
                } else {
                    for (uint64_t iter = 0; iter < repeat.iterations; ++iter) {
                        writeBuffers[i].syncToDevice(0, size);
                        ++completed;
                    }
                }

                writeThreadBytes[i] = completed * size;
            } catch (...) {
                errors[i] = std::current_exception();
            }
        });
    }
    for (size_t i = 0; i < readBuffers.size(); ++i) {
        threads.emplace_back([&readBuffers, &errors, &readThreadBytes, i,
                              repeat, deadline, errorOffset = writeBuffers.size()] {
            try {
                const uint64_t size = readBuffers[i].getSize();
                uint64_t completed = 0;

                if (repeat.durationMode()) {
                    while (std::chrono::steady_clock::now() < deadline) {
                        readBuffers[i].syncFromDevice(0, size);
                        ++completed;
                    }
                } else {
                    for (uint64_t iter = 0; iter < repeat.iterations; ++iter) {
                        readBuffers[i].syncFromDevice(0, size);
                        ++completed;
                    }
                }

                readThreadBytes[i] = completed * size;
            } catch (...) {
                errors[errorOffset + i] = std::current_exception();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    const auto end = std::chrono::steady_clock::now();

    for (auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }

    const auto elapsed = end - start;
    uint64_t writeBytes = 0;
    uint64_t readBytes = 0;
    for (uint64_t value : writeThreadBytes) {
        writeBytes += value;
    }
    for (uint64_t value : readThreadBytes) {
        readBytes += value;
    }
    const double writeMBps = mbPerSecond(writeBytes, elapsed);
    const double readMBps = mbPerSecond(readBytes, elapsed);

    printBandwidthMetric("Read", readMBps);
    printBandwidthMetric("Write", writeMBps);
    printBandwidthMetric("Total", readMBps + writeMBps);
}

template<typename Buffer>
static void testBandwidthSuite(std::vector<Buffer>& singleDirectionBuffers,
                               const std::string& label,
                               const std::string& backendSuffix,
                               const BandwidthRepeatOptions& repeat = {}) {
    std::cout << "Testing " << label << " read bandwidth ("
              << singleDirectionBuffers.size() << " threads" << backendSuffix << ")..." << std::endl;
    printBandwidthMetric("Read", testSingleDirectionBandwidth(singleDirectionBuffers, /*toDevice=*/false, repeat));

    std::cout << "Testing " << label << " write bandwidth ("
              << singleDirectionBuffers.size() << " threads" << backendSuffix << ")..." << std::endl;
    printBandwidthMetric("Write", testSingleDirectionBandwidth(singleDirectionBuffers, /*toDevice=*/true, repeat));
}

template<typename Buffer>
static void testBidirectionalBandwidthSuite(std::vector<Buffer>& bidirectionalWriteBuffers,
                                            std::vector<Buffer>& bidirectionalReadBuffers,
                                            const std::string& label,
                                            const std::string& backendSuffix,
                                            const BandwidthRepeatOptions& repeat = {}) {
    std::cout << "Testing " << label << " bidirectional bandwidth ("
              << (bidirectionalWriteBuffers.size() + bidirectionalReadBuffers.size())
              << " threads" << backendSuffix << ")..." << std::endl;
    testBidirectionalBandwidth(bidirectionalWriteBuffers, bidirectionalReadBuffers, repeat);
}

static vrtd::Buffer openValidateHbmBuffer(const vrtd::Device& device,
                                          const Validate::Options& options,
                                          uint64_t position) {
    if (options.placementExplicit) {
        return device.openRawBuffer(addressFor(HBM_BASE, options, position),
                                    options.bufferSize, vrtd::BufferAllocDir::Bidirectional,
                                    vrtdMmChannel(options, position));
    }

    return device.openHbmBuffer(static_cast<uint32_t>(position), options.bufferSize,
                                vrtd::BufferAllocDir::Bidirectional,
                                vrtdMmChannel(options, position));
}

static vrtd::Buffer openValidateDdrBuffer(const vrtd::Device& device,
                                          const Validate::Options& options,
                                          uint64_t position) {
    if (options.placementExplicit) {
        return device.openRawBuffer(addressFor(DDR_BASE, options, position),
                                    options.bufferSize, vrtd::BufferAllocDir::Bidirectional,
                                    vrtdMmChannel(options, position));
    }

    return device.openDdrBuffer(options.bufferSize, vrtd::BufferAllocDir::Bidirectional,
                                vrtdMmChannel(options, position));
}

static int runRawTransferTest(const std::string& bdf, const Validate::Options& options) {
    const unsigned N = options.threads;
    const BandwidthRepeatOptions repeat = repeatOptionsFromValidate(options);

    if (!options.noReset) {
        std::cout << "Raw transfer mode skips reset; continuing without VRTD reset." << std::endl;
    }
    warnIfNotRoot("SLASH raw transfer mode");

    const std::string qdmaPath = resolveQdmaDevicePath(bdf);
    std::cout << "Using raw QDMA device " << qdmaPath << "..." << std::endl;
    printChannelAllocation(options);
    printMmChannel(options);
    printRingSizeIndex(options);
    printBandwidthRepeatMode(repeat);

    RawQdmaDevice qdma(qdmaPath);
    const uint32_t ringSizeIndex = options.ringSizeIndex.value_or(QDMA_RING_SZ_IDX);

    if (!options.ddrOnly) {
        std::cout << "Testing HBM data integrity (" << N << " regions, raw QDMA)..." << std::endl;
        {
            std::vector<RawTransferBuffer> hbmBuffers;
            hbmBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                hbmBuffers.emplace_back(qdma.get(), rawAddressFor(HBM_BASE, options, i),
                                        options.bufferSize,
                                        slashMmChannel(options, i), ringSizeIndex);
            }

            if (!testDataIntegrity(hbmBuffers, "HBM")) {
                std::cerr << "HBM data integrity check failed" << std::endl;
                return 1;
            }

            testBandwidthSuite(hbmBuffers, "HBM", ", raw QDMA", repeat);
        }
        {
            // Bidirectional HBM: positions interleave R/W across regions
            // 0..2N-1.  Reads land on even regions, writes on odd regions.
            std::vector<RawTransferBuffer> hbmWriteBuffers;
            std::vector<RawTransferBuffer> hbmReadBuffers;
            hbmWriteBuffers.reserve(N);
            hbmReadBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                hbmReadBuffers.emplace_back(qdma.get(),
                                            rawAddressFor(HBM_BASE, options, 2 * i),
                                            options.bufferSize,
                                            slashMmChannel(options, 2 * i), ringSizeIndex);
                hbmWriteBuffers.emplace_back(qdma.get(),
                                             rawAddressFor(HBM_BASE, options, 2 * i + 1),
                                             options.bufferSize,
                                             slashMmChannel(options, 2 * i + 1), ringSizeIndex);
            }

            testBidirectionalBandwidthSuite(hbmWriteBuffers, hbmReadBuffers, "HBM", ", raw QDMA", repeat);
        }
    }

    if (!options.hbmOnly) {
        std::cout << "Testing DDR data integrity (" << N << " buffers, raw QDMA)..." << std::endl;
        {
            std::vector<RawTransferBuffer> ddrBuffers;
            ddrBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                ddrBuffers.emplace_back(qdma.get(), rawAddressFor(DDR_BASE, options, i),
                                        options.bufferSize,
                                        slashMmChannel(options, i), ringSizeIndex);
            }

            if (!testDataIntegrity(ddrBuffers, "DDR")) {
                std::cerr << "DDR data integrity check failed" << std::endl;
                return 1;
            }

            testBandwidthSuite(ddrBuffers, "DDR", ", raw QDMA", repeat);
        }
        {
            // Bidirectional DDR: positions interleave R/W across slot indices
            // 0..2N-1 of the DDR address space.
            std::vector<RawTransferBuffer> ddrWriteBuffers;
            std::vector<RawTransferBuffer> ddrReadBuffers;
            ddrWriteBuffers.reserve(N);
            ddrReadBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                ddrReadBuffers.emplace_back(qdma.get(),
                                            rawAddressFor(DDR_BASE, options, 2 * i),
                                            options.bufferSize,
                                            slashMmChannel(options, 2 * i), ringSizeIndex);
                ddrWriteBuffers.emplace_back(qdma.get(),
                                             rawAddressFor(DDR_BASE, options, 2 * i + 1),
                                             options.bufferSize,
                                             slashMmChannel(options, 2 * i + 1), ringSizeIndex);
            }

            testBidirectionalBandwidthSuite(ddrWriteBuffers, ddrReadBuffers, "DDR", ", raw QDMA", repeat);
        }
    }

    if (!options.ddrOnly && !options.hbmOnly) {
        {
            std::vector<RawTransferBuffer> parBuffers;
            parBuffers.reserve(2 * N);
            for (unsigned i = 0; i < N; ++i) {
                parBuffers.emplace_back(qdma.get(), rawAddressFor(HBM_BASE, options, i),
                                        options.bufferSize,
                                        slashMmChannel(options, i), ringSizeIndex);
            }
            for (unsigned i = 0; i < N; ++i) {
                parBuffers.emplace_back(qdma.get(), rawAddressFor(DDR_BASE, options, i),
                                        options.bufferSize,
                                        slashMmChannel(options, i), ringSizeIndex);
            }

            testBandwidthSuite(parBuffers, "HBM+DDR", ", raw QDMA", repeat);
        }
        {
            // Bidirectional HBM+DDR: 4N positions total.  Positions 0..2N-1
            // are HBM (interleaved R/W across regions 0..2N-1); positions
            // 2N..4N-1 are DDR (interleaved R/W across DDR slots 0..2N-1).
            // Channel = (p / 2) & 1 throughout.
            std::vector<RawTransferBuffer> parWriteBuffers;
            std::vector<RawTransferBuffer> parReadBuffers;
            parWriteBuffers.reserve(2 * N);
            parReadBuffers.reserve(2 * N);
            for (unsigned i = 0; i < N; ++i) {
                parReadBuffers.emplace_back(qdma.get(),
                                            rawAddressFor(HBM_BASE, options, 2 * i),
                                            options.bufferSize,
                                            slashMmChannel(options, 2 * i), ringSizeIndex);
                parWriteBuffers.emplace_back(qdma.get(),
                                             rawAddressFor(HBM_BASE, options, 2 * i + 1),
                                             options.bufferSize,
                                             slashMmChannel(options, 2 * i + 1), ringSizeIndex);
            }
            for (unsigned i = 0; i < N; ++i) {
                parReadBuffers.emplace_back(qdma.get(),
                                            rawAddressFor(DDR_BASE, options, 2 * i),
                                            options.bufferSize,
                                            slashMmChannel(options, 2 * i), ringSizeIndex);
                parWriteBuffers.emplace_back(qdma.get(),
                                             rawAddressFor(DDR_BASE, options, 2 * i + 1),
                                             options.bufferSize,
                                             slashMmChannel(options, 2 * i + 1), ringSizeIndex);
            }

            testBidirectionalBandwidthSuite(parWriteBuffers, parReadBuffers, "HBM+DDR", ", raw QDMA", repeat);
        }
    }

    return 0;
}

/// Raw integrity + bandwidth test driven over the off-the-shelf Xilinx QDMA
/// driver instead of SLASH.  smi provisions queues itself (qmax + netlink
/// add/start) and transfers over the per-queue char devices.
static int runQdmaDriverTest(const std::string& bdf, const Validate::Options& options) {
#ifndef SMI_ENABLE_QDMA_DRIVER_BACKEND
    (void)bdf;
    (void)options;
    std::cerr << "validate: this v80-smi build was configured without "
              << "--use-qdma-driver support. Rebuild with "
              << "-DSMI_ENABLE_QDMA_DRIVER_BACKEND=ON." << std::endl;
    return 1;
#else
    const unsigned N = options.threads;
    const BandwidthRepeatOptions repeat = repeatOptionsFromValidate(options);

    if (!options.noReset) {
        std::cout << "QDMA-driver raw mode skips reset; continuing without VRTD reset." << std::endl;
    }
    warnIfNotRoot("QDMA-driver raw mode");

    const bool runParallel = !options.ddrOnly && !options.hbmOnly;

    std::cout << "Using off-the-shelf Xilinx QDMA driver for board " << bdf << "..." << std::endl;
    printChannelAllocation(options);
    printMmChannel(options);
    printRingSizeIndex(options);
    printBandwidthRepeatMode(repeat);
    smi::qdma_driver::QdmaDriverDevice qdma(bdf, options.ringSizeIndex);
    std::cout << "Resolved QDMA function " << qdma.functionBdf() << std::endl;
    qdma.ensureQmax(runParallel ? 4 * N : 2 * N);

    const unsigned mmChannels = qdma.mmChannelMax();
    if (mmChannels > 1) {
        std::cout << "Distributing queues across " << mmChannels
                  << " MM channels (channel = qid % " << mmChannels << ")." << std::endl;
    } else {
        std::cout << "Device exposes a single MM channel; all queues on channel 0." << std::endl;
    }

    if (!options.ddrOnly) {
        std::cout << "Testing HBM data integrity (" << N << " regions, QDMA driver)..." << std::endl;
        {
            std::vector<smi::qdma_driver::QdmaDriverBuffer> hbmBuffers;
            hbmBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                hbmBuffers.emplace_back(qdma, i, rawAddressFor(HBM_BASE, options, i),
                                        options.bufferSize,
                                        qdmaDriverMmChannel(options, i));
            }

            if (!testDataIntegrity(hbmBuffers, "HBM")) {
                std::cerr << "HBM data integrity check failed" << std::endl;
                return 1;
            }

            testBandwidthSuite(hbmBuffers, "HBM", ", QDMA driver", repeat);
        }
        {
            std::vector<smi::qdma_driver::QdmaDriverBuffer> hbmWriteBuffers;
            std::vector<smi::qdma_driver::QdmaDriverBuffer> hbmReadBuffers;
            hbmWriteBuffers.reserve(N);
            hbmReadBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                hbmReadBuffers.emplace_back(qdma, i,
                                            rawAddressFor(HBM_BASE, options, 2 * i),
                                            options.bufferSize,
                                            qdmaDriverMmChannel(options, 2 * i));
                hbmWriteBuffers.emplace_back(qdma, N + i,
                                             rawAddressFor(HBM_BASE, options, 2 * i + 1),
                                             options.bufferSize,
                                             qdmaDriverMmChannel(options, 2 * i + 1));
            }

            testBidirectionalBandwidthSuite(hbmWriteBuffers, hbmReadBuffers, "HBM", ", QDMA driver", repeat);
        }
    }

    if (!options.hbmOnly) {
        std::cout << "Testing DDR data integrity (" << N << " buffers, QDMA driver)..." << std::endl;
        {
            std::vector<smi::qdma_driver::QdmaDriverBuffer> ddrBuffers;
            ddrBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                ddrBuffers.emplace_back(qdma, i, rawAddressFor(DDR_BASE, options, i),
                                        options.bufferSize,
                                        qdmaDriverMmChannel(options, i));
            }

            if (!testDataIntegrity(ddrBuffers, "DDR")) {
                std::cerr << "DDR data integrity check failed" << std::endl;
                return 1;
            }

            testBandwidthSuite(ddrBuffers, "DDR", ", QDMA driver", repeat);
        }
        {
            std::vector<smi::qdma_driver::QdmaDriverBuffer> ddrWriteBuffers;
            std::vector<smi::qdma_driver::QdmaDriverBuffer> ddrReadBuffers;
            ddrWriteBuffers.reserve(N);
            ddrReadBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                ddrReadBuffers.emplace_back(qdma, i,
                                            rawAddressFor(DDR_BASE, options, 2 * i),
                                            options.bufferSize,
                                            qdmaDriverMmChannel(options, 2 * i));
                ddrWriteBuffers.emplace_back(qdma, N + i,
                                             rawAddressFor(DDR_BASE, options, 2 * i + 1),
                                             options.bufferSize,
                                             qdmaDriverMmChannel(options, 2 * i + 1));
            }

            testBidirectionalBandwidthSuite(ddrWriteBuffers, ddrReadBuffers, "DDR", ", QDMA driver", repeat);
        }
    }

    if (runParallel) {
        {
            std::vector<smi::qdma_driver::QdmaDriverBuffer> parBuffers;
            parBuffers.reserve(2 * N);
            for (unsigned i = 0; i < N; ++i) {
                parBuffers.emplace_back(qdma, i, rawAddressFor(HBM_BASE, options, i),
                                        options.bufferSize,
                                        qdmaDriverMmChannel(options, i));
            }
            for (unsigned i = 0; i < N; ++i) {
                parBuffers.emplace_back(qdma, N + i, rawAddressFor(DDR_BASE, options, i),
                                        options.bufferSize,
                                        qdmaDriverMmChannel(options, i));
            }

            testBandwidthSuite(parBuffers, "HBM+DDR", ", QDMA driver", repeat);
        }
        {
            std::vector<smi::qdma_driver::QdmaDriverBuffer> parWriteBuffers;
            std::vector<smi::qdma_driver::QdmaDriverBuffer> parReadBuffers;
            parWriteBuffers.reserve(2 * N);
            parReadBuffers.reserve(2 * N);
            for (unsigned i = 0; i < N; ++i) {
                parReadBuffers.emplace_back(qdma, i,
                                            rawAddressFor(HBM_BASE, options, 2 * i),
                                            options.bufferSize,
                                            qdmaDriverMmChannel(options, 2 * i));
                parWriteBuffers.emplace_back(qdma, 2 * N + i,
                                             rawAddressFor(HBM_BASE, options, 2 * i + 1),
                                             options.bufferSize,
                                             qdmaDriverMmChannel(options, 2 * i + 1));
            }
            for (unsigned i = 0; i < N; ++i) {
                parReadBuffers.emplace_back(qdma, N + i,
                                            rawAddressFor(DDR_BASE, options, 2 * i),
                                            options.bufferSize,
                                            qdmaDriverMmChannel(options, 2 * i));
                parWriteBuffers.emplace_back(qdma, 3 * N + i,
                                             rawAddressFor(DDR_BASE, options, 2 * i + 1),
                                             options.bufferSize,
                                             qdmaDriverMmChannel(options, 2 * i + 1));
            }

            testBidirectionalBandwidthSuite(parWriteBuffers, parReadBuffers, "HBM+DDR", ", QDMA driver", repeat);
        }
    }

    return 0;
#endif
}

} // namespace

uint64_t Validate::parseByteSizeOption(const std::string& text) {
    return parseByteSizeText(text);
}

std::vector<Validate::Options::MmChannel> Validate::parseMmChannelSpec(const std::string& text) {
    std::vector<Options::MmChannel> result;
    size_t start = 0;
    while (true) {
        const size_t comma = text.find(',', start);
        std::string token = trim(comma == std::string::npos ? text.substr(start)
                                                            : text.substr(start, comma - start));
        std::transform(token.begin(), token.end(), token.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (token == "auto") {
            result.push_back(Options::MmChannel::Auto);
        } else if (token == "0") {
            result.push_back(Options::MmChannel::Ch0);
        } else if (token == "1") {
            result.push_back(Options::MmChannel::Ch1);
        } else {
            throw std::invalid_argument("mm-channel entries must be auto, 0, or 1");
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (result.empty()) {
        throw std::invalid_argument("mm-channel spec must not be empty");
    }
    return result;
}

int Validate::run(const Options& options) {
    std::string bdf = resolveBoardBdf(options.bdf, "validate");
    unsigned N = options.threads;

    if (!validatePlacement(options)) {
        return 1;
    }

    if (!checkHostMemoryBudget(options)) {
        return 1;
    }

    if (options.rawTransferTest) {
        return runRawTransferTest(bdf, options);
    }

    if (options.useQdmaDriver) {
        return runQdmaDriverTest(bdf, options);
    }

    // -- Step 1: (Optional) Reset the device via vrtd --
    if (!options.noReset) {
        std::cout << "Resetting device " << bdf << "..." << std::endl;
        {
            vrtd::Session session;
            auto device = session.getDeviceByBdf(bdf);
            device.hotplugOp(vrtd::HotplugOp::ResetSequence);
        }
        // Session is torn down; the daemon has re-discovered the device.
    }

    vrtd::Session session;
    auto device = session.getDeviceByBdf(bdf);

    printMmChannel(options);

    // -- Step 2: HBM — integrity then bandwidth --
    if (!options.ddrOnly) {
        std::cout << "Testing HBM data integrity (" << N << " regions)..." << std::endl;
        {
            std::vector<vrtd::Buffer> hbmBuffers;
            hbmBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                hbmBuffers.push_back(openValidateHbmBuffer(device, options, i));
            }

            if (!testDataIntegrity(hbmBuffers, "HBM")) {
                std::cerr << "HBM data integrity check failed" << std::endl;
                return 1;
            }

            testBandwidthSuite(hbmBuffers, "HBM", "");
        }
        // HBM buffers released.
        {
            std::vector<vrtd::Buffer> hbmWriteBuffers;
            std::vector<vrtd::Buffer> hbmReadBuffers;
            hbmWriteBuffers.reserve(N);
            hbmReadBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                hbmReadBuffers.push_back(openValidateHbmBuffer(device, options, 2 * i));
                hbmWriteBuffers.push_back(openValidateHbmBuffer(device, options, 2 * i + 1));
            }

            testBidirectionalBandwidthSuite(hbmWriteBuffers, hbmReadBuffers, "HBM", "");
        }
        // Bidirectional HBM buffers released.
    }

    // -- Step 3: DDR — integrity then bandwidth --
    if (!options.hbmOnly) {
        std::cout << "Testing DDR data integrity (" << N << " buffers)..." << std::endl;
        {
            std::vector<vrtd::Buffer> ddrBuffers;
            ddrBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                ddrBuffers.push_back(openValidateDdrBuffer(device, options, i));
            }

            if (!testDataIntegrity(ddrBuffers, "DDR")) {
                std::cerr << "DDR data integrity check failed" << std::endl;
                return 1;
            }

            testBandwidthSuite(ddrBuffers, "DDR", "");
        }
        // DDR buffers released.
        {
            std::vector<vrtd::Buffer> ddrWriteBuffers;
            std::vector<vrtd::Buffer> ddrReadBuffers;
            ddrWriteBuffers.reserve(N);
            ddrReadBuffers.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                if (options.placementExplicit) {
                    ddrReadBuffers.push_back(openValidateDdrBuffer(device, options, 2 * i));
                    ddrWriteBuffers.push_back(openValidateDdrBuffer(device, options, 2 * i + 1));
                } else {
                    ddrWriteBuffers.push_back(openValidateDdrBuffer(device, options, i));
                    ddrReadBuffers.push_back(openValidateDdrBuffer(device, options, i));
                }
            }

            testBidirectionalBandwidthSuite(ddrWriteBuffers, ddrReadBuffers, "DDR", "");
        }
        // Bidirectional DDR buffers released.
    }

    // -- Step 4: HBM + DDR in parallel --
    if (!options.ddrOnly && !options.hbmOnly) {
        {
            std::vector<vrtd::Buffer> parBuffers;
            parBuffers.reserve(2 * N);
            for (unsigned i = 0; i < N; ++i) {
                parBuffers.push_back(openValidateHbmBuffer(device, options, i));
            }
            for (unsigned i = 0; i < N; ++i) {
                parBuffers.push_back(openValidateDdrBuffer(device, options, i));
            }

            testBandwidthSuite(parBuffers, "HBM+DDR", "");
        }
        // Parallel single-direction buffers released.
        {
            std::vector<vrtd::Buffer> parWriteBuffers;
            std::vector<vrtd::Buffer> parReadBuffers;
            parWriteBuffers.reserve(2 * N);
            parReadBuffers.reserve(2 * N);
            for (unsigned i = 0; i < N; ++i) {
                parReadBuffers.push_back(openValidateHbmBuffer(device, options, 2 * i));
                parWriteBuffers.push_back(openValidateHbmBuffer(device, options, 2 * i + 1));
            }
            for (unsigned i = 0; i < N; ++i) {
                if (options.placementExplicit) {
                    parReadBuffers.push_back(openValidateDdrBuffer(device, options, 2 * i));
                    parWriteBuffers.push_back(openValidateDdrBuffer(device, options, 2 * i + 1));
                } else {
                    parWriteBuffers.push_back(openValidateDdrBuffer(device, options, i));
                    parReadBuffers.push_back(openValidateDdrBuffer(device, options, i));
                }
            }

            testBidirectionalBandwidthSuite(parWriteBuffers, parReadBuffers, "HBM+DDR", "");
        }
        // Parallel bidirectional buffers released.
    }

    return 0;
}
