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

#include "connection.hpp"

#include <vrtd/error.hpp>

#include <string>
#include <utility>

#include <unistd.h>

namespace vrtd::detail {

namespace {

/**
 * Translate the C API's return convention into the C++ exception contract.
 *
 * @param ret Result returned by a libvrtd operation.
 * @throws vrtd::Error when @p ret is not VRTD_RET_OK.
 */
void throwIfError(enum vrtd_ret ret)
{
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }
}

} // namespace

Connection::Connection(const char *socketPath)
    : socketFd(vrtd_connect(socketPath))
{
    if (socketFd == -1) {
        throw Error(VRTD_RET_BAD_CONN);
    }
}

Connection::~Connection() noexcept
{
    close();
}

Connection::RequestGuard::RequestGuard(const Connection& connection)
    : lifecycleLock()
    , requestLock()
{
    /*
     * Reject known-closed connections before touching either mutex. close()
     * publishes this state before waiting for active lifecycle readers.
     */
    if (connection.closed.load(std::memory_order_acquire)) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    /*
     * Pin the socket before joining the serialized request stream. Recheck
     * after each potentially blocking acquisition so a racing close cannot
     * admit new work while it waits for older operations to drain.
     */
    lifecycleLock =
        std::shared_lock<std::shared_mutex>(connection.lifecycleMutex);
    if (connection.closed.load(std::memory_order_acquire)) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    requestLock = std::unique_lock<std::mutex>(connection.requestMutex);
    if (connection.closed.load(std::memory_order_acquire)) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
}

Connection::TransferGuard::TransferGuard(const Connection& connection)
    : lifecycleLock()
{
    /*
     * Avoid joining the lifecycle gate after close starts, then recheck under
     * the shared lock to cover a close racing the first observation.
     */
    if (connection.closed.load(std::memory_order_acquire)) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    lifecycleLock =
        std::shared_lock<std::shared_mutex>(connection.lifecycleMutex);
    if (connection.closed.load(std::memory_order_acquire)) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
}

Connection::RequestGuard Connection::request() const
{
    return RequestGuard(*this);
}

Connection::TransferGuard Connection::transfer() const
{
    return TransferGuard(*this);
}

uint32_t Connection::getNumDevices() const
{
    auto guard = request();
    uint32_t count = 0;

    throwIfError(vrtd_get_num_devices(this->socketFd, &count));

    return count;
}

vrtd_device_info Connection::getDeviceInfo(uint32_t dev) const
{
    auto guard = request();
    struct vrtd_device_info info = {};

    throwIfError(vrtd_get_device_info(this->socketFd, dev, &info));

    return info;
}

uint32_t Connection::getDeviceByBdf(std::string_view bdf) const
{
    auto guard = request();

    /* The C API requires owned NUL-terminated storage, unlike string_view. */
    std::string value(bdf);
    uint32_t dev = 0;

    throwIfError(vrtd_get_device_by_bdf(this->socketFd, value.c_str(), &dev));

    return dev;
}

slash_ioctl_bar_info Connection::getBarInfo(uint32_t dev, uint8_t bar) const
{
    auto guard = request();
    struct slash_ioctl_bar_info info = {};

    throwIfError(vrtd_get_bar_info(this->socketFd, dev, bar, &info));

    return info;
}

slash_bar_file Connection::openBarFile(uint32_t dev, uint8_t bar) const
{
    auto guard = request();
    struct slash_bar_file file = {};

    throwIfError(vrtd_open_bar_file(this->socketFd, dev, bar, &file));

    return file;
}

slash_qdma_info Connection::getQdmaInfo(uint32_t dev) const
{
    auto guard = request();
    struct slash_qdma_info info = {};

    throwIfError(vrtd_qdma_get_info(this->socketFd, dev, &info));

    return info;
}

uint32_t Connection::createQdmaQpair(
    uint32_t dev,
    const struct slash_qdma_qpair_add& cfg
) const
{
    auto guard = request();

    /*
     * libvrtd writes the allocated QID into its input structure. Preserve the
     * const C++ contract by giving the C API a private copy.
     */
    struct slash_qdma_qpair_add value = cfg;

    throwIfError(vrtd_qdma_qpair_add(this->socketFd, dev, &value));

    return value.qid;
}

void Connection::startQdmaQpair(uint32_t dev, uint32_t qid) const
{
    auto guard = request();

    throwIfError(vrtd_qdma_qpair_start(this->socketFd, dev, qid));
}

void Connection::stopQdmaQpair(uint32_t dev, uint32_t qid) const
{
    auto guard = request();

    throwIfError(vrtd_qdma_qpair_stop(this->socketFd, dev, qid));
}

void Connection::deleteQdmaQpair(uint32_t dev, uint32_t qid) const
{
    auto guard = request();

    throwIfError(vrtd_qdma_qpair_del(this->socketFd, dev, qid));
}

int Connection::openQdmaQpairFd(
    uint32_t dev,
    uint32_t qid,
    uint32_t flags
) const
{
    auto guard = request();
    int qpairFd = -1;

    throwIfError(
        vrtd_qdma_qpair_get_fd(this->socketFd, dev, qid, flags, &qpairFd)
    );

    return qpairFd;
}

vrtd_buffer *Connection::openBuffer(
    uint32_t dev,
    uint32_t allocType,
    uint64_t size,
    uint64_t allocArg,
    uint32_t allocDir,
    enum vrtd_mm_channel mmChannel
) const
{
    auto guard = request();
    struct vrtd_buffer *buffer = nullptr;

    throwIfError(
        vrtd_buffer_open(
            this->socketFd,
            dev,
            allocType,
            allocDir,
            allocArg,
            size,
            mmChannel,
            &buffer
        )
    );

    /*
     * A successful C call must return an adoptable handle. Do not construct a
     * C++ owner that could neither operate on nor release a null handle.
     */
    if (buffer == nullptr) {
        throw Error(VRTD_RET_INTERNAL_ERROR);
    }

    return buffer;
}

vrtd_buffer *Connection::openRawBuffer(
    uint32_t dev,
    uint64_t physAddr,
    uint64_t size,
    uint32_t allocDir,
    enum vrtd_mm_channel mmChannel
) const
{
    auto guard = request();
    struct vrtd_buffer *buffer = nullptr;

    throwIfError(
        vrtd_buffer_open_raw(
            this->socketFd,
            dev,
            physAddr,
            size,
            allocDir,
            mmChannel,
            &buffer
        )
    );

    /* Treat success without a native handle as a C client invariant failure. */
    if (buffer == nullptr) {
        throw Error(VRTD_RET_INTERNAL_ERROR);
    }

    return buffer;
}

void Connection::closeBuffer(struct vrtd_buffer *buffer) const noexcept
{
    if (buffer == nullptr) {
        return;
    }

    /*
     * Buffer cleanup is a noexcept RAII path. While connected, serialize the
     * close RPC with other control traffic. After disconnect, destroy only
     * local state; vrtd already reclaims the client's server-side resources.
     */
    try {
        std::shared_lock<std::shared_mutex> lifecycleLock(lifecycleMutex);
        if (closed.load(std::memory_order_acquire)) {
            (void) vrtd_buffer_destroy(buffer);
            return;
        }

        std::lock_guard<std::mutex> requestLock(requestMutex);
        (void) vrtd_buffer_close(buffer);
    } catch (...) {
        /* Locking or transport failure must not leak local mappings or FDs. */
        (void) vrtd_buffer_destroy(buffer);
    }
}

void Connection::syncBufferToDevice(
    struct vrtd_buffer *buffer,
    uint64_t offset,
    uint64_t size
) const
{
    auto guard = transfer();

    throwIfError(vrtd_buffer_sync_to_device(buffer, offset, size));
}

void Connection::syncBufferFromDevice(
    struct vrtd_buffer *buffer,
    uint64_t offset,
    uint64_t size
) const
{
    auto guard = transfer();

    throwIfError(vrtd_buffer_sync_from_device(buffer, offset, size));
}

void Connection::hotplugOp(
    uint32_t dev,
    uint8_t op,
    uint8_t function
) const
{
    auto guard = request();

    throwIfError(vrtd_device_hotplug_op(this->socketFd, dev, op, function));
}

void Connection::hotplugRescan() const
{
    auto guard = request();

    throwIfError(vrtd_device_hotplug_rescan(this->socketFd));
}

void Connection::designWrite(uint32_t dev, int inputFd) const
{
    auto guard = request();

    throwIfError(vrtd_design_write(this->socketFd, dev, inputFd));
}

void Connection::designWriteFile(uint32_t dev, std::string_view path) const
{
    auto guard = request();

    /* Materialize NUL-terminated storage for the path-based C API. */
    std::string value(path);

    throwIfError(vrtd_design_write_file(this->socketFd, dev, value.c_str()));
}

void Connection::cfgmemProgram(
    uint32_t dev,
    int inputFd,
    uint8_t bootDevice,
    uint32_t partition
) const
{
    auto guard = request();

    throwIfError(
        vrtd_cfgmem_program(
            this->socketFd,
            dev,
            inputFd,
            bootDevice,
            partition
        )
    );
}

void Connection::cfgmemProgramFile(
    uint32_t dev,
    std::string_view path,
    uint8_t bootDevice,
    uint32_t partition
) const
{
    auto guard = request();

    /* Materialize NUL-terminated storage for the path-based C API. */
    std::string value(path);

    throwIfError(
        vrtd_cfgmem_program_file(
            this->socketFd,
            dev,
            value.c_str(),
            bootDevice,
            partition
        )
    );
}

uint64_t Connection::cfgmemProgramStart(
    uint32_t dev,
    int inputFd,
    uint8_t bootDevice,
    uint32_t partition
) const
{
    auto guard = request();
    uint64_t jobId = 0;

    throwIfError(
        vrtd_cfgmem_program_start(
            this->socketFd,
            dev,
            inputFd,
            bootDevice,
            partition,
            &jobId
        )
    );

    return jobId;
}

uint64_t Connection::cfgmemProgramFileStart(
    uint32_t dev,
    std::string_view path,
    uint8_t bootDevice,
    uint32_t partition
) const
{
    auto guard = request();
    /* Materialize NUL-terminated storage for the path-based C API. */
    std::string value(path);
    uint64_t jobId = 0;

    throwIfError(
        vrtd_cfgmem_program_file_start(
            this->socketFd,
            dev,
            value.c_str(),
            bootDevice,
            partition,
            &jobId
        )
    );

    return jobId;
}

struct vrtd_cfgmem_program_status Connection::cfgmemProgramStatus(
    uint64_t jobId
) const
{
    auto guard = request();
    struct vrtd_cfgmem_program_status status = {};

    throwIfError(vrtd_cfgmem_program_status(this->socketFd, jobId, &status));

    return status;
}

uint32_t Connection::getClockRate(uint32_t dev, uint32_t region) const
{
    auto guard = request();
    uint32_t rate = 0;

    throwIfError(vrtd_clock_get_rate(this->socketFd, dev, region, &rate));

    return rate;
}

uint32_t Connection::setClockRate(
    uint32_t dev,
    uint32_t region,
    uint32_t rateHz
) const
{
    auto guard = request();
    uint32_t achieved = 0;

    throwIfError(
        vrtd_clock_set_rate(this->socketFd, dev, region, rateHz, &achieved)
    );

    return achieved;
}

std::vector<vrtd_sensor_entry> Connection::getSensorInfo(uint32_t dev) const
{
    auto guard = request();
    struct vrtd_sensor_entry entries[VRTD_SENSOR_MAX_ENTRIES] = {};
    uint32_t count = 0;

    throwIfError(
        vrtd_get_sensor_info(
            this->socketFd,
            dev,
            entries,
            VRTD_SENSOR_MAX_ENTRIES,
            &count
        )
    );

    /* The C API bounds count by the supplied fixed-capacity output array. */
    return {entries, entries + count};
}

void Connection::close() noexcept
{
    /*
     * Publish closure before waiting for lifecycle readers. This prevents
     * queued callers from extending the drain indefinitely. The exclusive lock
     * then guarantees no active operation still uses the descriptor.
     */
    if (closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    std::unique_lock<std::shared_mutex> lifecycleLock(lifecycleMutex);
    int fd = std::exchange(socketFd, -1);

    if (fd >= 0) {
        (void) ::close(fd);
    }
}

bool Connection::isClosed() const noexcept
{
    return closed.load(std::memory_order_acquire);
}

} // namespace vrtd::detail
