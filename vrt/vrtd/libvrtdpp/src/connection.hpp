/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef VRTDPP_CONNECTION_HPP
#define VRTDPP_CONNECTION_HPP

#include <vrtd/vrtd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <vector>

namespace vrtd::detail {

/**
 * @brief Shared owner and serializer for one vrtd client connection.
 *
 * Public libvrtdpp handles retain this object so that device resources may
 * outlive the Session facade that discovered them. Control-plane operations
 * are serialized because libvrtd performs one synchronous request/response
 * exchange on the shared socket. Buffer DMA operations use only the lifecycle
 * gate and may therefore run concurrently on independent buffers.
 *
 * Calling close() publishes the permanent closed state before waiting for
 * active operations to drain. New operations then fail locally while existing
 * operations retain a valid socket until their lifecycle guards are released.
 */
class Connection {
public:
    /**
     * @brief Connect to a vrtd UNIX socket.
     *
     * @param socketPath Non-null path passed directly to vrtd_connect().
     * @throws vrtd::Error with VRTD_RET_BAD_CONN if the connection fails.
     */
    explicit Connection(const char *socketPath);

    /**
     * @brief Close the socket when the final shared owner is destroyed.
     *
     * Destruction is idempotent and never throws.
     */
    ~Connection() noexcept;

    /** Connection state and synchronization primitives cannot be copied. */
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /** A live connection has a stable address and cannot be moved. */
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    /** Return the visible device count; throw vrtd::Error on request failure. */
    uint32_t getNumDevices() const;

    /** Return copied name and PCI metadata for zero-based device @p dev. */
    struct vrtd_device_info getDeviceInfo(uint32_t dev) const;

    /**
     * @brief Resolve a canonical PCI BDF to a daemon device index.
     *
     * @param bdf Board-level PCI BDF accepted by vrtd.
     * @return Zero-based daemon device index.
     * @throws vrtd::Error if no matching device exists or the request fails.
     */
    uint32_t getDeviceByBdf(std::string_view bdf) const;

    /** Return copied metadata for BAR @p bar on zero-based device @p dev. */
    struct slash_ioctl_bar_info getBarInfo(uint32_t dev, uint8_t bar) const;

    /**
     * @brief Open and map a device BAR.
     *
     * @param dev Zero-based daemon device index.
     * @param bar Zero-based PCI BAR index.
     * @return Owning mapping value that must be adopted by BarFile or released
     *         with vrtd_close_bar_file().
     * @throws vrtd::Error if the BAR cannot be opened or mapped.
     */
    struct slash_bar_file openBarFile(uint32_t dev, uint8_t bar) const;

    /** Return copied QDMA capabilities for zero-based device @p dev. */
    struct slash_qdma_info getQdmaInfo(uint32_t dev) const;

    /**
     * @brief Create a QDMA queue pair without modifying caller-owned input.
     *
     * @param dev Zero-based daemon device index.
     * @param cfg Queue configuration copied into the protocol request.
     * @return Kernel-assigned queue-pair identifier.
     * @throws vrtd::Error if creation fails.
     */
    uint32_t createQdmaQpair(
        uint32_t dev,
        const struct slash_qdma_qpair_add& cfg
    ) const;

    /** Start queue pair @p qid on device @p dev. */
    void startQdmaQpair(uint32_t dev, uint32_t qid) const;

    /** Stop queue pair @p qid on device @p dev. */
    void stopQdmaQpair(uint32_t dev, uint32_t qid) const;

    /**
     * @brief Delete queue pair @p qid on device @p dev.
     *
     * The kernel implicitly stops an active queue before releasing it.
     * @throws vrtd::Error if deletion fails.
     */
    void deleteQdmaQpair(uint32_t dev, uint32_t qid) const;

    /**
     * @brief Obtain a new ioctl-only descriptor for a queue pair.
     *
     * @param dev Zero-based daemon device index.
     * @param qid Existing queue-pair identifier.
     * @param flags Zero or O_CLOEXEC; other flags may be rejected.
     * @return New descriptor owned by the caller.
     * @throws vrtd::Error if the descriptor cannot be obtained.
     */
    int openQdmaQpairFd(uint32_t dev, uint32_t qid, uint32_t flags) const;

    /**
     * @brief Allocate a daemon-managed device buffer and DMA resources.
     *
     * @param dev Zero-based daemon device index.
     * @param allocType One of the vrtd allocation-type wire values.
     * @param size Requested byte count; the returned handle records any
     *             allocator rounding.
     * @param allocArg Allocation-type-specific argument, such as an HBM region.
     * @param allocDir Permitted DMA transfer direction.
     * @param mmChannel AXI-MM/NoC channel selection.
     * @return Exclusively owned native handle that must be passed to
     *         closeBuffer().
     * @throws vrtd::Error if allocation or local handle creation fails.
     */
    struct vrtd_buffer *openBuffer(
        uint32_t dev,
        uint32_t allocType,
        uint64_t size,
        uint64_t allocArg,
        uint32_t allocDir,
        enum vrtd_mm_channel mmChannel
    ) const;

    /**
     * @brief Open DMA resources for a caller-selected device address.
     *
     * @param dev Zero-based daemon device index.
     * @param physAddr Device physical address; the caller must ensure the range
     *                 is valid and does not conflict with another allocation.
     * @param size Byte count covered by the raw handle.
     * @param allocDir Permitted DMA transfer direction.
     * @param mmChannel AXI-MM/NoC channel selection.
     * @return Exclusively owned native handle that must be passed to
     *         closeBuffer().
     * @throws vrtd::Error on permission, validation, or resource failure.
     */
    struct vrtd_buffer *openRawBuffer(
        uint32_t dev,
        uint64_t physAddr,
        uint64_t size,
        uint32_t allocDir,
        enum vrtd_mm_channel mmChannel
    ) const;

    /**
     * @brief Best-effort release of a native buffer handle.
     *
     * Sends daemon-side cleanup while the connection is open. Once the
     * connection is closed, or if cleanup machinery fails, it releases only
     * local mappings and descriptors because vrtd reclaims client resources on
     * disconnect. A null handle is a no-op and no errors escape.
     *
     * @param buffer Exclusively owned native handle to consume.
     */
    void closeBuffer(struct vrtd_buffer *buffer) const noexcept;

    /**
     * @brief Transfer a host-buffer range to the device.
     *
     * @param buffer Open native buffer permitting host-to-device transfer.
     * @param offset First byte in the host mapping.
     * @param size Number of bytes to transfer.
     * @throws vrtd::Error for a closed connection, invalid range or direction,
     *         or DMA failure.
     */
    void syncBufferToDevice(
        struct vrtd_buffer *buffer,
        uint64_t offset,
        uint64_t size
    ) const;

    /**
     * @brief Transfer a device-buffer range into its host mapping.
     *
     * @param buffer Open native buffer permitting device-to-host transfer.
     * @param offset First byte in the host mapping.
     * @param size Number of bytes to transfer.
     * @throws vrtd::Error for a closed connection, invalid range or direction,
     *         or DMA failure.
     */
    void syncBufferFromDevice(
        struct vrtd_buffer *buffer,
        uint64_t offset,
        uint64_t size
    ) const;

    /**
     * @brief Apply a PCIe hotplug operation to a device or physical function.
     *
     * Rescan is global and ignores @p dev and @p function. Reset ignores
     * @p function. Remove and hotplug accept PF 0-7 or the all-functions
     * sentinel; SBR requires one PF.
     *
     * @param dev Zero-based daemon device index.
     * @param op One of the vrtd hotplug-operation wire values.
     * @param function Physical function selector or all-functions sentinel.
     * @throws vrtd::Error if the operation or selector is invalid or fails.
     */
    void hotplugOp(uint32_t dev, uint8_t op, uint8_t function) const;

    /**
     * @brief Trigger a global PCI bus rescan.
     *
     * Device availability and daemon indices may change after this operation.
     * @throws vrtd::Error if the rescan request fails.
     */
    void hotplugRescan() const;

    /**
     * @brief Synchronously program the device design from an open descriptor.
     *
     * SCM_RIGHTS duplicates @p inputFd for the daemon; ownership of the
     * caller's descriptor is unchanged.
     *
     * @param dev Zero-based daemon device index.
     * @param inputFd Readable design image descriptor.
     * @throws vrtd::Error if the transfer fails or another transfer is busy.
     */
    void designWrite(uint32_t dev, int inputFd) const;

    /**
     * @brief Synchronously program the device design from a file path.
     *
     * libvrtd opens and closes the file internally.
     *
     * @param dev Zero-based daemon device index.
     * @param path Design image path.
     * @throws vrtd::Error if the file or design transfer fails.
     */
    void designWriteFile(uint32_t dev, std::string_view path) const;
    /**
     * @brief Synchronously program cfgmem and perform the managed reset.
     *
     * @param dev Zero-based daemon device index.
     * @param inputFd Readable PDI descriptor. SCM_RIGHTS preserves caller
     *                ownership of the original descriptor.
     * @param bootDevice AMI boot-device selector.
     * @param partition Flash partition to program and select.
     * @throws vrtd::Error if programming or reset fails.
     */
    void cfgmemProgram(
        uint32_t dev,
        int inputFd,
        uint8_t bootDevice,
        uint32_t partition
    ) const;

    /**
     * @brief Synchronously program cfgmem from a path and reset the device.
     *
     * @param dev Zero-based daemon device index.
     * @param path PDI path opened and closed internally by libvrtd.
     * @param bootDevice AMI boot-device selector.
     * @param partition Flash partition to program and select.
     * @throws vrtd::Error if the file, programming, or reset operation fails.
     */
    void cfgmemProgramFile(
        uint32_t dev,
        std::string_view path,
        uint8_t bootDevice,
        uint32_t partition
    ) const;

    /**
     * @brief Submit an asynchronous cfgmem programming job.
     *
     * @param dev Zero-based daemon device index.
     * @param inputFd Readable PDI descriptor. SCM_RIGHTS preserves caller
     *                ownership of the original descriptor.
     * @param bootDevice AMI boot-device selector.
     * @param partition Flash partition to program and select.
     * @return Daemon-assigned job identifier for cfgmemProgramStatus().
     * @throws vrtd::Error if the job cannot be submitted.
     */
    uint64_t cfgmemProgramStart(
        uint32_t dev,
        int inputFd,
        uint8_t bootDevice,
        uint32_t partition
    ) const;

    /**
     * @brief Submit an asynchronous cfgmem job from a file path.
     *
     * @param dev Zero-based daemon device index.
     * @param path PDI path opened and closed internally by libvrtd.
     * @param bootDevice AMI boot-device selector.
     * @param partition Flash partition to program and select.
     * @return Daemon-assigned job identifier for cfgmemProgramStatus().
     * @throws vrtd::Error if the file cannot be opened or submission fails.
     */
    uint64_t cfgmemProgramFileStart(
        uint32_t dev,
        std::string_view path,
        uint8_t bootDevice,
        uint32_t partition
    ) const;

    /**
     * @brief Return the latest status snapshot for a cfgmem job.
     *
     * @param jobId Identifier returned by a cfgmem start operation.
     * @return Copied progress, phase, timing, and final-result fields.
     * @throws vrtd::Error if the job is unknown, belongs to another client, or
     *         the request fails.
     */
    struct vrtd_cfgmem_program_status cfgmemProgramStatus(
        uint64_t jobId
    ) const;

    /** Return clock @p region for zero-based device @p dev in hertz. */
    uint32_t getClockRate(uint32_t dev, uint32_t region) const;

    /**
     * @brief Request a clock rate and return the achieved value.
     *
     * Hardware quantization may make the achieved rate differ from @p rateHz.
     *
     * @param dev Zero-based daemon device index.
     * @param region One of the vrtd clock-region wire values.
     * @param rateHz Requested rate in hertz.
     * @return Achieved rate in hertz.
     * @throws vrtd::Error if the region, rate, or operation is invalid.
     */
    uint32_t setClockRate(
        uint32_t dev,
        uint32_t region,
        uint32_t rateHz
    ) const;

    /**
     * @brief Return a copied snapshot of all sensors reported for a device.
     *
     * Each entry carries a fixed-width name plus raw type, status, unit
     * exponent, and value fields.
     *
     * @param dev Zero-based daemon device index.
     * @return Variable-length copied sensor snapshot.
     * @throws vrtd::Error if the sensor query fails.
     */
    std::vector<struct vrtd_sensor_entry> getSensorInfo(uint32_t dev) const;

    /**
     * @brief Permanently close the shared connection.
     *
     * The operation first rejects new work, then waits for active operations
     * before closing the socket. Repeated calls are no-ops and no errors escape.
     */
    void close() noexcept;

    /** Return whether close has begun or completed. */
    bool isClosed() const noexcept;

private:
    /**
     * @brief Pins the socket lifetime and serializes one protocol exchange.
     *
     * The lifecycle lock is acquired before the request lock. The connection
     * socket remains open until this guard is destroyed, and no second caller
     * can interleave a request/response pair on it.
     */
    class RequestGuard {
    public:
        /**
         * @brief Acquire a guard or reject a concurrent close.
         *
         * Closed-state checks around both lock acquisitions prevent work from
         * joining after close has published the terminal state.
         *
         * @throws vrtd::Error with VRTD_RET_BAD_LIB_CALL when closing or closed.
         */
        explicit RequestGuard(const Connection& connection);

    private:
        /** Shared lifecycle ownership that prevents socket teardown. */
        std::shared_lock<std::shared_mutex> lifecycleLock;

        /** Exclusive ownership of the synchronous request/response stream. */
        std::unique_lock<std::mutex> requestLock;
    };

    /**
     * @brief Pins the connection lifetime for one buffer DMA transfer.
     *
     * Unlike RequestGuard, this guard does not serialize operations. Independent
     * buffers may therefore transfer concurrently while close() waits for all
     * active transfers to release their shared lifecycle locks.
     */
    class TransferGuard {
    public:
        /**
         * @brief Acquire a transfer guard or reject a concurrent close.
         *
         * @throws vrtd::Error with VRTD_RET_BAD_LIB_CALL when closing or closed.
         */
        explicit TransferGuard(const Connection& connection);

    private:
        /** Shared lifecycle ownership that prevents connection teardown. */
        std::shared_lock<std::shared_mutex> lifecycleLock;
    };

    /**
     * @brief Acquire a serialized request guard.
     *
     * @throws vrtd::Error with VRTD_RET_BAD_LIB_CALL when closing or closed.
     */
    RequestGuard request() const;

    /**
     * @brief Acquire a concurrent buffer-transfer guard.
     *
     * @throws vrtd::Error with VRTD_RET_BAD_LIB_CALL when closing or closed.
     */
    TransferGuard transfer() const;

    /** Shared by active operations and held exclusively by close(). */
    mutable std::shared_mutex lifecycleMutex;

    /** Prevents interleaved request/response pairs on the shared socket. */
    mutable std::mutex requestMutex;

    /** One-way state flag set before close waits for active operations. */
    std::atomic<bool> closed{false};

    /** Owned daemon socket, or -1 after exclusive teardown. */
    int socketFd{-1};
};

} // namespace vrtd::detail

#endif // VRTDPP_CONNECTION_HPP
