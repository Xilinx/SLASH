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

#ifndef VRTD_SESSION_HPP
#define VRTD_SESSION_HPP

#include <stdint.h>
#include <vrtd/vrtd.h>
#include <vrtd/device.hpp>
#include <vrtd/bar.hpp>
#include <vrtd/bar_file.hpp>
#include <vrtd/buffer.hpp>
#include <vrtd/qdma_qpair.hpp>

#include <mutex>
#include <memory>
#include <string_view>

namespace vrtd {


/**
 * @brief Owning session/connection to the V Runtime Daemon (vrtd).
 *
 * A @c Session wraps a connected libvrtd socket and provides typed, exception-based
 * access to devices and BARs. All public member functions are thread-safe; calls
 * synchronize on an internal @c std::mutex.
 *
 * @par Exceptions
 * Most member functions throw #vrtd::Error on failure. The destructor never throws.
 *
 * @par Lifetime and moves
 * - The session is non-copyable and movable.
 * - Moving a session leaves the moved-from object in the closed state
 *   (i.e., @c isClosed()==true and @c operator bool() == false).
 * - **Important:** Any @c Device or @c Bar previously obtained from a session becomes
 *   invalid once that session is closed or moved; subsequent operations on those
 *   objects will throw.
 */
class Session {
public:
    /**
     * @brief Construct and connect to the vrtd socket.
     *
     * @param socket_path Filesystem path to the vrtd UNIX socket.
     *                    Defaults to the standard path.
     * @throws vrtd::Error if the connection cannot be established.
     */
    explicit Session(const char *socket_path = VRTD_STANDARD_PATH);

    /**
     * @brief Destructor; closes the session if still open.
     */
    ~Session() noexcept;

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    /**
     * @brief Move-construct a session.
     *
     * The moved-from session becomes closed.
     *
     * @param other The session to move from.
     */
    Session(Session&& other) noexcept;

    /**
     * @brief Move-assign a session.
     *
     * Closes any existing connection, then takes ownership from @p other.
     * The moved-from session becomes closed.
    *
     * @param other The session to move from.
     */
    Session& operator=(Session&& other) noexcept;

    /**
     * @brief Number of devices visible via vrtd.
     * @return Device count.
     * @throws vrtd::Error on error.
     *
     * @par Thread safety
     * Safe for concurrent calls across threads.
     */
    uint32_t getNumDevices() const;

    /**
     * @brief Retrieve a device handle by index.
     *
     * @param i Zero-based device index; must be less than @c getNumDevices().
     * @return A lightweight @c Device value referring back to this session.
     * @throws vrtd::Error if @p i is out of range or if the session is not usable.
     *
     * @par Notes
     * The returned @c Device becomes invalid if this session is later closed or moved.
     */
    Device getDevice(size_t i) const;

    /**
     * @brief Retrieve a device handle by PCI BDF string.
     *
     * @param bdf PCI BDF string (e.g., "0000:65:00.0").
     * @return A lightweight @c Device value referring back to this session.
     * @throws vrtd::Error if the device cannot be found or if the session is not usable.
     */
    Device getDeviceByBdf(std::string_view bdf) const;

    /**
     * @brief Query QDMA capabilities for a device.
     *
     * @param device Device for which to query QDMA info.
     * @return A copy of the QDMA capability struct as reported by the daemon.
     * @throws vrtd::Error on error.
     */
    struct slash_qdma_info getQdmaInfo(const Device& device) const;

    /**
     * @brief Explicitly close the session.
     *
     * Idempotent. After closing, @c isClosed()==true and further operations
     * on this session or on previously obtained @c Device/@c Bar objects will throw.
     */
    void close() noexcept;

    /**
     * @brief Whether the session is closed.
     */
    bool isClosed() const noexcept;

    /**
     * @brief Truthiness conversion.
     *
     * @return @c true if the session is open (not closed).
     */
    explicit operator bool() const noexcept;
private:
    int fd;
    mutable std::unique_ptr<std::mutex> m;

    /**
     * @internal Obtains a BAR for @p device. Called via @c Device::getBar().
     */
    Bar getBar(const Device& device, uint8_t bar_number) const;

    /**
     * @internal Opens and mmaps a BAR file. Called via @c Bar::openBarFile().
     */
    BarFile openBarFile(const Bar &bar) const;

    /**
     * @internal Create a QDMA qpair on a device.
     *
     * Returns an owning @c QdmaQpair that will automatically delete
     * the qpair on destruction.
     *
     * @param device Device on which to create the qpair.
     * @param cfg    Qpair configuration parameters. The returned qpair
     *               exposes @c getQid().
     * @return An owning @c QdmaQpair.
     * @throws vrtd::Error on error.
     */
    QdmaQpair createQdmaQpair(
        const Device& device,
        const struct slash_qdma_qpair_add& cfg
    ) const;

    /**
     * @internal Open a buffer (allocation + QDMA qpair).
     *
     * @param device    Device on which to allocate.
     * @param allocType Allocation type.
     * @param size      Requested size in bytes.
     * @param allocArg  Allocation argument (HBM region index for HBM).
     * @param allocDir  QDMA transfer direction.
     * @return An owning @c Buffer.
     * @throws vrtd::Error on error.
     */
    Buffer openBuffer(
        const Device& device,
        BufferAllocType allocType,
        uint64_t size,
        uint64_t allocArg,
        BufferAllocDir allocDir
    ) const;

    /**
     * @internal Open a raw buffer (QDMA qpair at caller-specified device address).
     *
     * @param device    Device on which to create the qpair.
     * @param phys_addr Caller-specified device physical address (bypasses allocator).
     * @param size      Size in bytes.
     * @param allocDir  QDMA transfer direction.
     * @return An owning @c Buffer.
     * @throws vrtd::Error on error.
     */
    Buffer openBufferRaw(
        const Device& device,
        uint64_t phys_addr,
        uint64_t size,
        BufferAllocDir allocDir
    ) const;

    /**
     * @internal Perform a PCIe hotplug operation.
     *
     * For board-level operations (Rescan, ResetSequence), @p function is ignored.
     * For PF-level operations (Remove, ToggleSbr, Hotplug), @p function selects
     * the PCI physical function (0-7).
     *
     * @param device   Device target.
     * @param op       One of vrtd::HotplugOp.
     * @param function PCI function number (0-7) for PF-level ops.
     * @throws vrtd::Error on error.
     */
    void hotplugOp(const Device& device, HotplugOp op,
                   uint8_t function = 0) const;

    /**
     * @internal Perform a design writer transfer using an input FD.
     *
     * @param device   Device owning the design writer.
     * @param input_fd Input file descriptor to transfer from.
     * @throws vrtd::Error on error.
     */
    void designWrite(const Device& device, int input_fd) const;

    /**
     * @internal Perform a design writer transfer using a file path.
     *
     * @param device Device owning the design writer.
     * @param path   Input file path to transfer from.
     * @throws vrtd::Error on error.
     */
    void designWriteFile(const Device& device, std::string_view path) const;

    /**
     * @internal Get clock rate for a region.
     *
     * @param device Device owning the clock.
     * @param region One of vrtd_clock_region.
     * @return Current rate in Hz.
     * @throws vrtd::Error on error.
     */
    uint32_t getClockRate(const Device& device, ClockRegion region) const;

    /**
     * @internal Set clock rate for a region.
     *
     * @param device Device owning the clock.
     * @param region One of vrtd_clock_region.
     * @param rate_hz Requested rate in Hz.
     * @return Achieved rate in Hz.
     * @throws vrtd::Error on error.
     */
    uint32_t setClockRate(const Device& device, ClockRegion region, uint32_t rate_hz) const;

    /**
     * @internal Start, stop or delete an existing QDMA qpair.
     *
     * Convenience wrappers around the vrtd QDMA queue-op requests, used by
     * @c QdmaQpair via the callbacks injected by @c createQdmaQpair().
     *
     * @throws vrtd::Error on error.
     */
    void startQdmaQpair(const Device& device, uint32_t qid) const;
    void stopQdmaQpair (const Device& device, uint32_t qid) const;
    void deleteQdmaQpair(const Device& device, uint32_t qid) const;

    /**
     * @internal Obtain a read/write file descriptor for a QDMA qpair.
     *
     * @param device Device owning the qpair.
     * @param qid    Qpair identifier as returned by @c createQdmaQpair().
     * @param flags  OR of O_CLOEXEC and 0 (other flags may be rejected).
     * @return A new file descriptor referring to the qpair, owned by the caller.
     * @throws vrtd::Error on error.
     */
    int openQdmaQpairFd(const Device& device, uint32_t qid, uint32_t flags = 0) const;

    /**
     * @internal Query sensor information for a device.
     *
     * @param device Device to query sensors for.
     * @return Vector of sensor entries.
     * @throws vrtd::Error on error.
     */
    std::vector<SensorEntry> getSensorInfo(const Device& device) const;
};

}

#endif // VRTD_SESSION_HPP
