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

#ifndef SMI_QDMA_DRIVER_BACKEND_HPP
#define SMI_QDMA_DRIVER_BACKEND_HPP

/// @file qdma_driver_backend.hpp
/// @brief Raw-transfer backend for the off-the-shelf Xilinx QDMA driver.
///
/// This backend mirrors the surface of validate.cpp's SLASH RawTransferBuffer
/// (data()/getSize()/syncToDevice()/syncFromDevice()) so the templated
/// integrity and bandwidth tests work unchanged, but it drives the upstream
/// QDMA driver (submodules/qdma_drv) instead of SLASH/libslash:
///
///   - Queue lifecycle (add/start/stop/del) is performed over generic netlink
///     (family "xnl_pf"), exactly as the `dma-ctl` utility does.
///   - The function's `qmax` is provisioned via sysfs if it is too small.
///   - Data movement uses the per-queue char device /dev/qdma<idx>-MM-<qid>
///     with the device address carried as the file offset.
///
/// Unlike SLASH there is no control device or custom ioctl ABI; the stock
/// driver must be bound to the function for any of this to work.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "raw_transfer.hpp"

namespace smi::qdma_driver {

/// Opaque generic-netlink client used to talk to the QDMA driver.
class XnlClient;

/// Represents a single PCIe function managed by the upstream QDMA driver.
///
/// Resolves the driver's device index from the board BDF, ensures enough
/// queues are provisioned (qmax), and provides queue lifecycle operations.
class QdmaDriverDevice {
public:
    /// @param boardBdf Board-level BDF "DDDD:BB:DD" (function is resolved by
    ///                 enumerating the driver's device list).
    explicit QdmaDriverDevice(const std::string& boardBdf,
                              std::optional<uint32_t> ringSizeIndex = std::nullopt);
    ~QdmaDriverDevice();

    QdmaDriverDevice(const QdmaDriverDevice&) = delete;
    QdmaDriverDevice& operator=(const QdmaDriverDevice&) = delete;

    /// Ensure the function has at least @p needed queues provisioned, writing
    /// the sysfs `qmax` entry (which re-initializes the queue set) if required.
    void ensureQmax(unsigned needed);

    /// Add + start a bidirectional AXI-MM queue pair at relative index @p qid.
    ///
    /// queueStart pins the pair to MM engine channel `qid % mmChannelMax()`,
    /// spreading queues across the device's MM channels (the channel only
    /// takes effect on `q start`; the driver ignores it on `q add`).
    void queueAdd(uint32_t qid);
    /// Start queue @p qid pinned to MM engine @p channel (0-based, clamped to
    /// the device's channel count).
    void queueStart(uint32_t qid, uint32_t channel);

    /// Stop + delete a queue pair.  Best-effort; never throws (safe in dtors).
    void queueStop(uint32_t qid) noexcept;
    void queueDel(uint32_t qid) noexcept;

    /// Char-device path for queue @p qid, e.g. "/dev/qdma61001-MM-0".
    std::string charDevPath(uint32_t qid) const;

    /// Resolved 0000:BB:DD.F PCI address of the QDMA function.
    const std::string& functionBdf() const { return functionBdf_; }

    /// Number of MM (memory-mapped) DMA engine channels the function exposes.
    /// CPM5 (V80) reports 2; older/soft IPs report 1.  Always >= 1.
    unsigned mmChannelMax() const { return mmChannelMax_; }

private:
    void refreshQmax();

    std::unique_ptr<XnlClient> nl_;
    unsigned index_ = 0;          ///< Driver device index (qdma<index>).
    std::string functionBdf_;     ///< Full BDF including function.
    unsigned qmax_ = 0;           ///< Currently provisioned queue count.
    unsigned mmChannelMax_ = 1;   ///< Number of MM engine channels (>= 1).
    uint32_t ringSizeIndex_ = 0;  ///< QRNGSZ_IDX used when starting queues.
};

/// One host buffer bound to a freshly-created upstream QDMA queue pair.
///
/// Satisfies the buffer concept used by validate.cpp's testDataIntegrity() /
/// testBandwidth() templates.
class QdmaDriverBuffer {
public:
    /// @param mmChannel Concrete MM channel to pin to, or -1 to spread the
    ///                  queue across channels by qid % channel-count.
    QdmaDriverBuffer(QdmaDriverDevice& device, uint32_t qid, uint64_t physAddr, uint64_t size,
                     int mmChannel);

    QdmaDriverBuffer(const QdmaDriverBuffer&) = delete;
    QdmaDriverBuffer& operator=(const QdmaDriverBuffer&) = delete;

    QdmaDriverBuffer(QdmaDriverBuffer&& other) noexcept { moveFrom(other); }
    QdmaDriverBuffer& operator=(QdmaDriverBuffer&& other) noexcept {
        if (this != &other) {
            cleanup();
            moveFrom(other);
        }
        return *this;
    }

    ~QdmaDriverBuffer();

    void* data() { return mapping_.data; }
    uint64_t getSize() const { return mapping_.size; }

    void syncToDevice(uint64_t offset, uint64_t size) {
        raw::validateSyncRange(offset, size, mapping_.size, physAddr_, mapping_.step);
        raw::rawTransfer(fd_, mapping_.data, physAddr_, offset, size, mapping_.step, /*toDevice=*/true);
    }

    void syncFromDevice(uint64_t offset, uint64_t size) {
        raw::validateSyncRange(offset, size, mapping_.size, physAddr_, mapping_.step);
        raw::rawTransfer(fd_, mapping_.data, physAddr_, offset, size, mapping_.step, /*toDevice=*/false);
    }

private:
    void moveFrom(QdmaDriverBuffer& other) noexcept;
    void cleanup() noexcept;

    QdmaDriverDevice* device_ = nullptr;
    uint32_t qid_ = 0;
    bool queueAdded_ = false;
    bool queueStarted_ = false;
    int fd_ = -1;
    uint64_t physAddr_ = 0;
    raw::HostMapping mapping_{};
};

} // namespace smi::qdma_driver

#endif // SMI_QDMA_DRIVER_BACKEND_HPP
