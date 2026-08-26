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

#include <vrtd/device.hpp>

#include "connection.hpp"

#include <vrtd/error.hpp>

#include <string.h>

#include <utility>

namespace vrtd {

/*
 * Keep metadata independent of daemon-owned wire storage while retaining the
 * transport needed to create child resource handles.
 */
Device::Device(std::shared_ptr<detail::Connection> connection,
               uint32_t num,
               std::string_view name,
               std::string_view bdf,
               uint16_t vendorId,
               uint16_t deviceId,
               uint16_t subsystemVendorId,
               uint16_t subsystemDeviceId)
    : connection(std::move(connection))
    , num(num)
    , name(name)
    , bdf(bdf)
    , vendorId(vendorId)
    , deviceId(deviceId)
    , subsystemVendorId(subsystemVendorId)
    , subsystemDeviceId(subsystemDeviceId)
{
}

uint32_t Device::getNum() const noexcept
{
    return num;
}

const std::string& Device::getName() const noexcept
{
    return name;
}

const std::string& Device::getBdf() const noexcept
{
    return bdf;
}

uint16_t Device::getVendorId() const noexcept
{
    return vendorId;
}

uint16_t Device::getDeviceId() const noexcept
{
    return deviceId;
}

uint16_t Device::getSubsystemVendorId() const noexcept
{
    return subsystemVendorId;
}

uint16_t Device::getSubsystemDeviceId() const noexcept
{
    return subsystemDeviceId;
}

Bar Device::getBar(uint8_t bar) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    auto info = connection->getBarInfo(num, bar);
    return Bar(
        connection,
        num,
        bar,
        info.usable,
        info.in_use,
        info.start_address,
        info.length
    );
}

QdmaQpair Device::createQdmaQpair(
    const struct slash_qdma_qpair_add& cfg
) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    return QdmaQpair(connection, num, connection->createQdmaQpair(num, cfg));
}

Buffer Device::openBuffer(
    BufferAllocType allocType,
    uint64_t size,
    uint64_t allocArg,
    BufferAllocDir allocDir,
    MmChannel mmChannel
) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    auto raw = connection->openBuffer(
        num,
        static_cast<uint32_t>(allocType),
        size,
        allocArg,
        static_cast<uint32_t>(allocDir),
        static_cast<vrtd_mm_channel>(static_cast<uint32_t>(mmChannel))
    );

    return Buffer(connection, raw);
}

Buffer Device::openRawBuffer(
    uint64_t physAddr,
    uint64_t size,
    BufferAllocDir allocDir,
    MmChannel mmChannel
) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    auto raw = connection->openRawBuffer(
        num,
        physAddr,
        size,
        static_cast<uint32_t>(allocDir),
        static_cast<vrtd_mm_channel>(static_cast<uint32_t>(mmChannel))
    );

    return Buffer(connection, raw);
}

void Device::hotplugOp(HotplugOp op, uint8_t function) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    connection->hotplugOp(num, static_cast<uint8_t>(op), function);
}

void Device::designWrite(int inputFd) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    connection->designWrite(num, inputFd);
}

void Device::designWriteFile(std::string_view path) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    connection->designWriteFile(num, path);
}

void Device::cfgmemProgram(
    int inputFd,
    uint8_t bootDevice,
    uint32_t partition
) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    connection->cfgmemProgram(num, inputFd, bootDevice, partition);
}

void Device::cfgmemProgramFile(
    std::string_view path,
    uint8_t bootDevice,
    uint32_t partition
) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    connection->cfgmemProgramFile(num, path, bootDevice, partition);
}

uint32_t Device::getClockRate(ClockRegion region) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    return connection->getClockRate(num, static_cast<uint32_t>(region));
}

uint32_t Device::setClockRate(ClockRegion region, uint32_t rateHz) const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    return connection->setClockRate(
        num,
        static_cast<uint32_t>(region),
        rateHz
    );
}

std::vector<SensorEntry> Device::getSensorInfo() const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    auto raw = connection->getSensorInfo(num);

    std::vector<SensorEntry> result;
    result.reserve(raw.size());

    /*
     * Convert fixed-width C ABI entries into owning C++ values. Sensor names
     * are bounded because daemon-provided arrays need not be NUL-terminated.
     */
    for (const auto& entry : raw) {
        result.push_back(SensorEntry{
            std::string(
                entry.name,
                strnlen(entry.name, sizeof(entry.name))
            ),
            entry.type,
            entry.status,
            entry.unit_mod,
            entry.value
        });
    }

    return result;
}

} // namespace vrtd
