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

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vrtd {

namespace detail {
class Connection;
}

/**
 * @brief Lifecycle state of an asynchronous cfgmem programming job.
 *
 * Values mirror the libvrtd wire protocol and may be compared directly with
 * the corresponding @c VRTD_CFGMEM_PROGRAM_STATE_* constants.
 */
enum class CfgmemProgramState : uint32_t {
    Queued = VRTD_CFGMEM_PROGRAM_STATE_QUEUED,   ///< Waiting for a worker.
    Running = VRTD_CFGMEM_PROGRAM_STATE_RUNNING, ///< Programming is active.
    Done = VRTD_CFGMEM_PROGRAM_STATE_DONE,       ///< Completed successfully.
    Failed = VRTD_CFGMEM_PROGRAM_STATE_FAILED,   ///< Completed with an error.
};

/**
 * @brief Current phase of an asynchronous cfgmem programming job.
 *
 * The phase identifies the operation presently executing; use
 * CfgmemProgramState to determine whether the job is terminal.
 */
enum class CfgmemProgramPhase : uint32_t {
    Queued = VRTD_CFGMEM_PROGRAM_PHASE_QUEUED, ///< Waiting for execution.
    OpeningAmi = VRTD_CFGMEM_PROGRAM_PHASE_OPENING_AMI, ///< Opening AMI.
    DownloadingPdi = VRTD_CFGMEM_PROGRAM_PHASE_DOWNLOADING_PDI, ///< Writing the PDI.
    SelectingPartition = VRTD_CFGMEM_PROGRAM_PHASE_SELECTING_PARTITION, ///< Selecting boot.
    ResetPreparing = VRTD_CFGMEM_PROGRAM_PHASE_RESET_PREPARING, ///< Preparing reset.
    RemovingPcie = VRTD_CFGMEM_PROGRAM_PHASE_REMOVING_PCIE, ///< Removing PCI functions.
    TogglingSbr = VRTD_CFGMEM_PROGRAM_PHASE_TOGGLING_SBR, ///< Toggling secondary-bus reset.
    RescanningPcie = VRTD_CFGMEM_PROGRAM_PHASE_RESCANNING_PCIE, ///< Rescanning PCI.
    RediscoveringDevice = VRTD_CFGMEM_PROGRAM_PHASE_REDISCOVERING_DEVICE, ///< Reopening the board.
    Done = VRTD_CFGMEM_PROGRAM_PHASE_DONE, ///< Successful terminal phase.
    Failed = VRTD_CFGMEM_PROGRAM_PHASE_FAILED, ///< Failed terminal phase.
};

/** @brief Copied progress snapshot for one cfgmem programming job. */
struct CfgmemProgramStatus {
    uint64_t jobId = 0; ///< Daemon-assigned job identifier.
    CfgmemProgramState state = CfgmemProgramState::Queued; ///< Job lifecycle state.
    CfgmemProgramPhase phase = CfgmemProgramPhase::Queued; ///< Current operation.
    uint64_t bytesWritten = 0; ///< PDI bytes written so far.
    uint64_t bytesTotal = 0; ///< Total PDI byte count.
    uint64_t elapsedMsec = 0; ///< Elapsed job time in milliseconds.
    enum vrtd_ret result = VRTD_RET_OK; ///< Result code; meaningful when terminal.
};

/**
 * @brief Callback invoked with each observed cfgmem progress snapshot.
 *
 * Any observed terminal snapshot is delivered before
 * cfgmemProgramFileProgress() handles its result. Throwing from the callback
 * stops local polling but does not cancel the daemon job.
 */
using CfgmemProgressCallback = std::function<void(const CfgmemProgramStatus&)>;

/**
 * @brief Owning session/connection to the VRT Daemon (vrtd).
 *
 * A @c Session provides typed, exception-based access to a shared connection
 * to vrtd. Objects created from the session retain that connection and may
 * outlive the Session facade.
 *
 * @par Exceptions
 * Most member functions throw #vrtd::Error on failure. The destructor never throws.
 *
 * @par Lifetime and moves
 * - The session is non-copyable and movable.
 * - Moving a session leaves the moved-from object in the closed state
 *   (i.e., @c isClosed()==true and @c operator bool() == false).
 * - Moving or destroying the Session facade does not invalidate objects
 *   previously obtained from it.
 * - Explicitly calling @c close() closes the shared connection and invalidates
 *   all objects backed by it.
 */
class Session {
public:
    /**
     * @brief Construct and connect to the vrtd socket.
     *
     * @param socket_path Filesystem path to the vrtd UNIX socket. When
     *                    @c nullptr (the default), the path is taken from the
     *                    @c VRTD_SOCKET environment variable if it is set and
     *                    non-empty, otherwise it falls back to
     *                    #VRTD_STANDARD_PATH. This mirrors how vrt::Device
     *                    resolves the daemon socket.
     * @throws vrtd::Error if the connection cannot be established.
     */
    explicit Session(const char *socket_path = nullptr);

    /**
     * @brief Destructor; releases this facade's connection reference.
     *
     * The connection closes when its final owning object is destroyed.
     */
    ~Session() noexcept;

    /** A Session facade has unique ownership semantics and cannot be copied. */
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
     * Releases this facade's existing connection, then takes ownership from
     * @p other. Objects using the old connection remain valid.
     * The moved-from session becomes closed.
     *
     * @param other The session to move from.
     */
    Session& operator=(Session&& other) noexcept;

    /**
     * @brief Number of devices visible via vrtd.
     * @return Device count.
     * @throws vrtd::Error on error.
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
     * The returned @c Device retains the shared connection. It becomes invalid
     * only when that connection is explicitly closed.
     */
    Device getDevice(size_t i) const;

    /**
     * @brief Retrieve a device handle by PCI BDF string.
     *
     * Accepts a board BDF such as @c 0000:65:00 or domainless @c 65:00.
     * A physical-function suffix is stripped because vrtd indexes whole boards;
     * stripping emits a warning to stderr.
     *
     * @param bdf PCI board BDF, optionally with a domain or function suffix.
     * @return A lightweight @c Device value referring back to this session.
     * @throws vrtd::Error if the device cannot be found or if the session is not usable.
     */
    Device getDeviceByBdf(std::string_view bdf) const;

    /**
     * @brief Trigger a PCI bus rescan.
     *
     * Rescan is a global hotplug operation. It does not target a specific
     * vrtd device and does not require a Device handle.
     *
     * @throws vrtd::Error on error.
     */
    void hotplugRescan() const;

    /**
     * @brief Submit an asynchronous cfgmem programming job from an open FD.
     *
     * @p device must originate from this shared connection. The descriptor is
     * duplicated for the daemon through SCM_RIGHTS and remains caller-owned.
     *
     * @param device Device whose configuration memory will be programmed.
     * @param input_fd Readable PDI descriptor.
     * @param bootDevice AMI boot-device selector.
     * @param partition Flash partition to program and select.
     * @return Daemon-assigned job identifier for cfgmemProgramStatus().
     * @throws vrtd::Error if ownership validation or submission fails.
     */
    uint64_t cfgmemProgramStart(
        const Device& device,
        int input_fd,
        uint8_t bootDevice,
        uint32_t partition
    ) const;

    /**
     * @brief Return the latest snapshot for a cfgmem programming job.
     *
     * @param jobId Identifier returned by cfgmemProgramStart().
     * @return Copied progress and terminal-result fields.
     * @throws vrtd::Error if the job is unknown, belongs to another client, or
     *         the request fails.
     */
    CfgmemProgramStatus cfgmemProgramStatus(uint64_t jobId) const;

    /**
     * @brief Program a PDI file while reporting asynchronous progress.
     *
     * Starts a daemon job, polls it until a terminal state, and invokes
     * @p progressCallback for every observed snapshot including the terminal
     * one. A zero polling interval selects the ten-second default. Callback
     * exceptions stop local polling without cancelling the daemon job.
     *
     * @param device Device from this shared connection.
     * @param path PDI file opened by libvrtd.
     * @param bootDevice AMI boot-device selector.
     * @param partition Flash partition to program and select.
     * @param progressCallback Optional observer invoked outside connection locks.
     * @param pollIntervalMsec Delay between nonterminal polls in milliseconds.
     * @throws vrtd::Error on submission, polling, or terminal job failure.
     */
    void cfgmemProgramFileProgress(
        const Device& device,
        std::string_view path,
        uint8_t bootDevice,
        uint32_t partition,
        CfgmemProgressCallback progressCallback,
        uint64_t pollIntervalMsec = 10000
    ) const;

    /**
     * @brief Query QDMA capabilities for a device.
     *
     * @param device Device from this shared connection.
     * @return A copy of the QDMA capability struct as reported by the daemon.
     * @throws vrtd::Error if the device belongs to another connection or the
     *         query fails.
     */
    struct slash_qdma_info getQdmaInfo(const Device& device) const;

    /**
     * @brief Explicitly close the session.
     *
     * Idempotent. After closing, @c isClosed()==true and further operations
     * on this session or on previously obtained connection-backed objects
     * throw. The call first rejects new work and then waits for active
     * operations to finish before closing the socket.
     */
    void close() noexcept;

    /**
     * @brief Return whether this facade has no connection or close has begun.
     */
    bool isClosed() const noexcept;

    /**
     * @brief Truthiness conversion.
     *
     * @return @c true if the session is open (not closed).
     */
    explicit operator bool() const noexcept;
private:
    /**
     * @brief Shared connection retained by this facade and all derived handles.
     *
     * A null pointer marks a moved-from Session. A non-null connection may
     * itself be permanently closed.
     */
    std::shared_ptr<detail::Connection> connection;

};

}

#endif // VRTD_SESSION_HPP
