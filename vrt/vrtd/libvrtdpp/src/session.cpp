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

/**
 * @file session.cpp
 *
 * Implementation of the vrtd::Session C++ wrapper.
 *
 * Session manages a single AF_UNIX connection to the vrtd daemon,
 * providing thread-safe request dispatch (via an internal mutex) and
 * RAII resource management.
 *
 * The key design pattern is **callback injection**: when creating QDMA
 * queue pairs or other resources, Session passes lambdas that capture
 * the session fd.  This allows resource objects (QdmaQpair, etc.) to
 * issue their own cleanup requests to the daemon when destroyed,
 * without holding a direct reference to the Session.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include "vrtd/wire.h"
#endif

#include <vrtd/session.hpp>
#include <vrtd/error.hpp>

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <utility>
#include <string>

namespace vrtd {

Session::Session(const char *socketPath)
: m(std::make_unique<std::mutex>()) {
    fd = vrtd_connect(socketPath);

    if (fd == -1) {
        throw std::runtime_error(std::string("Failed to open socket ") + strerrordesc_np(errno));
    }
}

Session::~Session() noexcept
{
    close();
}

Session::Session(Session&& other) noexcept
{
    if (!other.isClosed()) {
        std::lock_guard<std::mutex> lk(*other.m);

        fd = std::exchange(other.fd, -1);
        m = std::exchange(other.m, nullptr);
    } else {
        fd = -1;
        m = nullptr;
    }
}

Session& Session::operator=(Session&& other) noexcept
{
    close();

    if (!other.isClosed()) {
        std::lock_guard<std::mutex> lk(*other.m);

        fd = std::exchange(other.fd, -1);
        m = std::exchange(other.m, nullptr);
    }

    return *this;
}

uint32_t Session::getNumDevices() const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    uint32_t numDevices;

    auto ret = vrtd_get_num_devices(fd, &numDevices);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    return numDevices;
}

Device Session::getDevice(size_t i) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    vrtd_device_info info = {};

    auto ret = vrtd_get_device_info(fd, i, &info);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    return Device(
        i,
        {info.name, strnlen(info.name, sizeof(info.name))},
        {info.pci.bdf, strnlen(info.pci.bdf, sizeof(info.pci.bdf))},
        info.pci.vendor_id,
        info.pci.device_id,
        info.pci.subsystem_vendor_id,
        info.pci.subsystem_device_id,
        [&](const Device& device, uint8_t num) { return getBar(device, num); },
        [&](const Device& device, const slash_qdma_qpair_add& cfg) { return createQdmaQpair(device, cfg); },
        [&](const Device& device, BufferAllocType type, uint64_t size, uint64_t arg, BufferAllocDir dir) {
            return openBuffer(device, type, size, arg, dir);
        },
        [&](const Device& device, uint64_t phys_addr, uint64_t size, BufferAllocDir dir) {
            return openBufferRaw(device, phys_addr, size, dir);
        },
        [&](const Device& device, HotplugOp op, uint8_t function) { return hotplugOp(device, op, function); },
        [&](const Device& device, int input_fd) { return designWrite(device, input_fd); },
        [&](const Device& device, std::string_view path) { return designWriteFile(device, path); },
        [&](const Device& device, ClockRegion region) { return getClockRate(device, region); },
        [&](const Device& device, ClockRegion region, uint32_t rate_hz) { return setClockRate(device, region, rate_hz); },
        [&](const Device& device) { return getSensorInfo(device); }
    );
}

Device Session::getDeviceByBdf(std::string_view bdf) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    // Normalize to board-level BDF (DDDD:BB:DD) to match how the daemon
    // stores devices.  Strip function digit if present, and prepend domain
    // 0000: if only one colon (short BDF like "03:00").
    std::string bdf_str(bdf);

    // Strip function digit (.F)
    auto dot = bdf_str.rfind('.');
    if (dot != std::string::npos) {
        std::cerr << "Warning: BDF '" << bdf
                  << "' contains a PF function number; "
                  << "stripping " << bdf_str.substr(dot)
                  << " — use board address (e.g. "
                  << bdf_str.substr(0, dot) << ") instead"
                  << std::endl;
        bdf_str = bdf_str.substr(0, dot);
    }

    // Prepend default domain if missing
    if (bdf_str.find(':') == bdf_str.rfind(':')) {
        bdf_str = "0000:" + bdf_str;
    }

    uint32_t dev_num = 0;
    auto ret = vrtd_get_device_by_bdf(fd, bdf_str.c_str(), &dev_num);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    vrtd_device_info info = {};
    ret = vrtd_get_device_info(fd, dev_num, &info);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    return Device(
        dev_num,
        {info.name, strnlen(info.name, sizeof(info.name))},
        {info.pci.bdf, strnlen(info.pci.bdf, sizeof(info.pci.bdf))},
        info.pci.vendor_id,
        info.pci.device_id,
        info.pci.subsystem_vendor_id,
        info.pci.subsystem_device_id,
        [&](const Device& device, uint8_t num) { return getBar(device, num); },
        [&](const Device& device, const slash_qdma_qpair_add& cfg) { return createQdmaQpair(device, cfg); },
        [&](const Device& device, BufferAllocType type, uint64_t size, uint64_t arg, BufferAllocDir dir) {
            return openBuffer(device, type, size, arg, dir);
        },
        [&](const Device& device, uint64_t phys_addr, uint64_t size, BufferAllocDir dir) {
            return openBufferRaw(device, phys_addr, size, dir);
        },
        [&](const Device& device, HotplugOp op, uint8_t function) { return hotplugOp(device, op, function); },
        [&](const Device& device, int input_fd) { return designWrite(device, input_fd); },
        [&](const Device& device, std::string_view path) { return designWriteFile(device, path); },
        [&](const Device& device, ClockRegion region) { return getClockRate(device, region); },
        [&](const Device& device, ClockRegion region, uint32_t rate_hz) { return setClockRate(device, region, rate_hz); },
        [&](const Device& device) { return getSensorInfo(device); }
    );
}

Bar Session::getBar(const Device& device, uint8_t num) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    slash_ioctl_bar_info barInfo;

    auto ret = vrtd_get_bar_info(fd, device.getNum(), num, &barInfo);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    return Bar(device.getNum(), num, barInfo.usable, barInfo.in_use, barInfo.start_address, barInfo.length, [&](const Bar&bar) { return openBarFile(bar); } );
}

BarFile Session::openBarFile(const Bar& bar) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    slash_bar_file barFile;

    auto ret = vrtd_open_bar_file(fd, bar.getDeviceNum(), bar.getNum(), &barFile);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    return BarFile(barFile);
}

slash_qdma_info Session::getQdmaInfo(const Device& device) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    slash_qdma_info info;
    auto ret = vrtd_qdma_get_info(fd, device.getNum(), &info);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    return info;
}

QdmaQpair Session::createQdmaQpair(
    const Device& device,
    const slash_qdma_qpair_add& cfg
) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    slash_qdma_qpair_add tmp = cfg;
    auto ret = vrtd_qdma_qpair_add(fd, device.getNum(), &tmp);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    return QdmaQpair(
        device.getNum(),
        tmp.qid,
        [this, device](const QdmaQpair& qp) { startQdmaQpair(device, qp.getQid()); },
        [this, device](const QdmaQpair& qp) { stopQdmaQpair(device, qp.getQid()); },
        [this, device](const QdmaQpair& qp) { deleteQdmaQpair(device, qp.getQid()); },
        [this, device](const QdmaQpair& qp, uint32_t flags) { return openQdmaQpairFd(device, qp.getQid(), flags); }
    );
}

Buffer Session::openBuffer(
    const Device& device,
    BufferAllocType allocType,
    uint64_t size,
    uint64_t allocArg,
    BufferAllocDir allocDir
) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    struct vrtd_buffer *raw = nullptr;
    auto ret = vrtd_buffer_open(
        fd,
        device.getNum(),
        static_cast<uint32_t>(allocType),
        static_cast<uint32_t>(allocDir),
        allocArg,
        size,
        &raw
    );
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    if (raw == nullptr) {
        throw Error(VRTD_RET_INTERNAL_ERROR);
    }

    return Buffer(raw);
}

Buffer Session::openBufferRaw(
    const Device& device,
    uint64_t phys_addr,
    uint64_t size,
    BufferAllocDir allocDir
) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    struct vrtd_buffer *raw = nullptr;
    auto ret = vrtd_buffer_open_raw(
        fd,
        device.getNum(),
        phys_addr,
        size,
        static_cast<uint32_t>(allocDir),
        &raw
    );
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    if (raw == nullptr) {
        throw Error(VRTD_RET_INTERNAL_ERROR);
    }

    return Buffer(raw);
}

void Session::hotplugOp(const Device& device, HotplugOp op,
                        uint8_t function) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    auto ret = vrtd_device_hotplug_op(fd, device.getNum(),
                                      static_cast<uint8_t>(op), function);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }
}

void Session::designWrite(const Device& device, int input_fd) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    auto ret = vrtd_design_write(fd, device.getNum(), input_fd);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }
}

void Session::designWriteFile(const Device& device, std::string_view path) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    std::string path_str(path);
    auto ret = vrtd_design_write_file(fd, device.getNum(), path_str.c_str());
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }
}

uint32_t Session::getClockRate(const Device& device, ClockRegion region) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    uint32_t rate = 0;
    auto ret = vrtd_clock_get_rate(fd, device.getNum(), static_cast<uint32_t>(region), &rate);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    return rate;
}

uint32_t Session::setClockRate(const Device& device, ClockRegion region, uint32_t rate_hz) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    uint32_t achieved = 0;
    auto ret = vrtd_clock_set_rate(fd, device.getNum(), static_cast<uint32_t>(region), rate_hz, &achieved);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    return achieved;
}

void Session::startQdmaQpair(const Device& device, uint32_t qid) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    auto ret = vrtd_qdma_qpair_start(fd, device.getNum(), qid);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }
}

void Session::stopQdmaQpair(const Device& device, uint32_t qid) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    auto ret = vrtd_qdma_qpair_stop(fd, device.getNum(), qid);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }
}

void Session::deleteQdmaQpair(const Device& device, uint32_t qid) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    auto ret = vrtd_qdma_qpair_del(fd, device.getNum(), qid);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }
}

int Session::openQdmaQpairFd(const Device& device, uint32_t qid, uint32_t flags) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    int qfd = -1;
    auto ret = vrtd_qdma_qpair_get_fd(fd, device.getNum(), qid, flags, &qfd);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    return qfd;
}

std::vector<SensorEntry> Session::getSensorInfo(const Device& device) const {
    if (isClosed()) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }
    std::lock_guard<std::mutex> lk(*m);

    struct vrtd_sensor_entry entries[VRTD_SENSOR_MAX_ENTRIES];
    uint32_t count = 0;

    auto ret = vrtd_get_sensor_info(fd, device.getNum(), entries,
                                    VRTD_SENSOR_MAX_ENTRIES, &count);
    if (ret != VRTD_RET_OK) {
        throw Error(ret);
    }

    std::vector<SensorEntry> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        result.push_back(SensorEntry{
            std::string(entries[i].name, strnlen(entries[i].name, sizeof(entries[i].name))),
            entries[i].type,
            entries[i].status,
            entries[i].unit_mod,
            entries[i].value
        });
    }

    return result;
}

void Session::close() noexcept {
    if (isClosed()) {
        return;
    }

    ::close(fd);
    fd = -1;
    m = nullptr;
}

bool Session::isClosed() const noexcept {
    if (fd == -1 || !m) {
        return true;
    } else {
        return false;
    }
}

}
