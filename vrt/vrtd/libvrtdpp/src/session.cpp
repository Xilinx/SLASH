/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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

#include <vrtd/session.hpp>

#include "connection.hpp"

#include <vrtd/error.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#include <string.h>
#include <unistd.h>

namespace vrtd {

namespace {

/**
 * Convert one fixed-width wire snapshot into the owning public C++ value.
 *
 * No pointers into the wire structure escape this helper.
 *
 * @param raw Status structure returned by libvrtd.
 * @return Equivalent public status value with copied scalar fields.
 */
CfgmemProgramStatus convertCfgmemStatus(
    const struct vrtd_cfgmem_program_status& raw
)
{
    CfgmemProgramStatus status;
    status.jobId = raw.job_id;
    status.state = static_cast<CfgmemProgramState>(raw.state);
    status.phase = static_cast<CfgmemProgramPhase>(raw.phase);
    status.bytesWritten = raw.bytes_written;
    status.bytesTotal = raw.bytes_total;
    status.elapsedMsec = raw.elapsed_msec;
    status.result = static_cast<enum vrtd_ret>(raw.result);

    return status;
}

} // namespace

Session::Session(const char *socketPath)
{
    /*
     * An explicit path wins. Otherwise honor a non-empty environment override
     * and treat an empty value as unset before falling back to the system path.
     */
    if (socketPath == nullptr) {
        const char *envPath = std::getenv("VRTD_SOCKET");
        socketPath = (envPath != nullptr && envPath[0] != '\0')
            ? envPath
            : VRTD_STANDARD_PATH;
    }

    connection = std::make_shared<detail::Connection>(socketPath);
}

Session::~Session() noexcept = default;

Session::Session(Session&& other) noexcept = default;

Session& Session::operator=(Session&& other) noexcept = default;

uint32_t Session::getNumDevices() const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    return connection->getNumDevices();
}

Device Session::getDevice(size_t i) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    /* Device strings are fixed-width wire arrays and need bounded copies. */
    auto info = connection->getDeviceInfo(i);

    return Device(
        connection,
        i,
        {info.name, strnlen(info.name, sizeof(info.name))},
        {info.pci.bdf, strnlen(info.pci.bdf, sizeof(info.pci.bdf))},
        info.pci.vendor_id,
        info.pci.device_id,
        info.pci.subsystem_vendor_id,
        info.pci.subsystem_device_id
    );
}

Device Session::getDeviceByBdf(std::string_view bdf) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    /*
     * vrtd identifies a board rather than an individual physical function.
     * Accept function-form input for compatibility, but warn before discarding
     * the suffix because that changes the requested identifier.
     */
    std::string normalized(bdf);
    auto dot = normalized.rfind('.');
    if (dot != std::string::npos) {
        std::cerr << "Warning: BDF '" << bdf
                  << "' contains a PF function number; stripping "
                  << normalized.substr(dot)
                  << " — use board address (e.g. "
                  << normalized.substr(0, dot) << ") instead"
                  << std::endl;
        normalized.erase(dot);
    }

    /* Domainless BB:DD input is resolved in the default PCI domain. */
    if (normalized.find(':') == normalized.rfind(':')) {
        normalized.insert(0, "0000:");
    }

    /*
     * Resolve the normalized board first, then snapshot its metadata into the
     * returned Device. Bounded copies tolerate non-NUL-terminated wire arrays.
     */
    uint32_t dev = connection->getDeviceByBdf(normalized);
    auto info = connection->getDeviceInfo(dev);

    return Device(
        connection,
        dev,
        {info.name, strnlen(info.name, sizeof(info.name))},
        {info.pci.bdf, strnlen(info.pci.bdf, sizeof(info.pci.bdf))},
        info.pci.vendor_id,
        info.pci.device_id,
        info.pci.subsystem_vendor_id,
        info.pci.subsystem_device_id
    );
}

void Session::hotplugRescan() const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    connection->hotplugRescan();
}

uint64_t Session::cfgmemProgramStart(
    const Device& device,
    int inputFd,
    uint8_t bootDevice,
    uint32_t partition
) const
{
    /*
     * A numeric device index is meaningful only on the connection that issued
     * it. Pointer identity accepts children retained across a Session move but
     * rejects handles obtained from a different daemon connection.
     */
    if (!connection || device.connection.get() != connection.get()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    return connection->cfgmemProgramStart(
        device.getNum(),
        inputFd,
        bootDevice,
        partition
    );
}

CfgmemProgramStatus Session::cfgmemProgramStatus(uint64_t jobId) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    return convertCfgmemStatus(connection->cfgmemProgramStatus(jobId));
}

void Session::cfgmemProgramFileProgress(
    const Device& device,
    std::string_view path,
    uint8_t bootDevice,
    uint32_t partition,
    CfgmemProgressCallback progressCallback,
    uint64_t pollIntervalMsec
) const
{
    /*
     * Validate ownership before starting the irreversible daemon job. A
     * callback exception later stops polling but cannot cancel this job.
     */
    if (!connection || device.connection.get() != connection.get()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    /* Start asynchronously, then normalize zero to the documented default. */
    uint64_t jobId = connection->cfgmemProgramFileStart(
        device.getNum(),
        path,
        bootDevice,
        partition
    );
    uint64_t sleepMsec = pollIntervalMsec == 0
        ? 10000ULL
        : pollIntervalMsec;

    for (;;) {
        /*
         * Invoke the optional callback outside Connection's request guard. This
         * permits re-entrant queries and, when supplied, lets it observe the
         * terminal snapshot before this function handles the final result.
         */
        auto status = convertCfgmemStatus(
            connection->cfgmemProgramStatus(jobId)
        );

        if (progressCallback) {
            progressCallback(status);
        }

        if (status.state == CfgmemProgramState::Done ||
            status.state == CfgmemProgramState::Failed) {
            if (status.result != VRTD_RET_OK) {
                throw Error(status.result);
            }
            return;
        }

        /* Delay only between nonterminal snapshots. */
        usleep(static_cast<useconds_t>(sleepMsec * 1000ULL));
    }
}

slash_qdma_info Session::getQdmaInfo(const Device& device) const
{
    /* Reject foreign device indices before they reach the daemon. */
    if (!connection || device.connection.get() != connection.get()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    return connection->getQdmaInfo(device.getNum());
}

void Session::close() noexcept
{
    if (connection) {
        connection->close();
    }
}

bool Session::isClosed() const noexcept
{
    return !connection || connection->isClosed();
}

Session::operator bool() const noexcept
{
    return !isClosed();
}

} // namespace vrtd
