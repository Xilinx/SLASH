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

#include <vrtd/device.hpp>

namespace vrtd {

Device::Device(uint32_t num,
               std::string_view name,
               std::string_view bdf,
               uint16_t vendorId,
               uint16_t deviceId,
               uint16_t subsystemVendorId,
               uint16_t subsystemDeviceId,
               std::function<Bar(const Device&, uint8_t)> fGetBar,
               std::function<QdmaQpair(const Device&, const struct slash_qdma_qpair_add&)> fCreateQdmaQpair,
               std::function<Buffer(const Device&, BufferAllocType, uint64_t, uint64_t, BufferAllocDir)> fOpenBuffer,
               std::function<Buffer(const Device&, uint64_t, uint64_t, BufferAllocDir)> fOpenBufferRaw,
               std::function<void(const Device&, HotplugOp, uint8_t)> fHotplugOp,
               std::function<void(const Device&, int)> fDesignWrite,
               std::function<void(const Device&, std::string_view)> fDesignWriteFile,
               std::function<uint32_t(const Device&, ClockRegion)> fGetClockRate,
               std::function<uint32_t(const Device&, ClockRegion, uint32_t)> fSetClockRate,
               std::function<std::vector<SensorEntry>(const Device&)> fGetSensorInfo) {
    this->num = num;
    this->name = name;
    this->bdf = bdf;
    this->vendorId = vendorId;
    this->deviceId = deviceId;
    this->subsystemVendorId = subsystemVendorId;
    this->subsystemDeviceId = subsystemDeviceId;
    this->fGetBar = fGetBar;
    this->fCreateQdmaQpair = fCreateQdmaQpair;
    this->fOpenBuffer = fOpenBuffer;
    this->fOpenBufferRaw = fOpenBufferRaw;
    this->fHotplugOp = fHotplugOp;
    this->fDesignWrite = fDesignWrite;
    this->fDesignWriteFile = fDesignWriteFile;
    this->fGetClockRate = fGetClockRate;
    this->fSetClockRate = fSetClockRate;
    this->fGetSensorInfo = fGetSensorInfo;
}

uint32_t Device::getNum() const noexcept {
    return num;
}

const std::string& Device::getName() const noexcept {
    return name;
}

const std::string& Device::getBdf() const noexcept {
    return bdf;
}

uint16_t Device::getVendorId() const noexcept {
    return vendorId;
}

uint16_t Device::getDeviceId() const noexcept {
    return deviceId;
}

uint16_t Device::getSubsystemVendorId() const noexcept {
    return subsystemVendorId;
}

uint16_t Device::getSubsystemDeviceId() const noexcept {
    return subsystemDeviceId;
}

Bar Device::getBar(uint8_t num) const {
    return fGetBar(*this, num);
}

QdmaQpair Device::createQdmaQpair(const struct slash_qdma_qpair_add& cfg) const {
    return fCreateQdmaQpair(*this, cfg);
}

Buffer Device::openBuffer(BufferAllocType allocType,
                          uint64_t size,
                          uint64_t allocArg,
                          BufferAllocDir allocDir) const {
    return fOpenBuffer(*this, allocType, size, allocArg, allocDir);
}

Buffer Device::openRawBuffer(uint64_t phys_addr,
                             uint64_t size,
                             BufferAllocDir allocDir) const {
    return fOpenBufferRaw(*this, phys_addr, size, allocDir);
}

void Device::hotplugOp(HotplugOp op, uint8_t function) const {
    fHotplugOp(*this, op, function);
}

void Device::designWrite(int input_fd) const {
    fDesignWrite(*this, input_fd);
}

void Device::designWriteFile(std::string_view path) const {
    fDesignWriteFile(*this, path);
}

uint32_t Device::getClockRate(ClockRegion region) const {
    return fGetClockRate(*this, region);
}

uint32_t Device::setClockRate(ClockRegion region, uint32_t rate_hz) const {
    return fSetClockRate(*this, region, rate_hz);
}

std::vector<SensorEntry> Device::getSensorInfo() const {
    return fGetSensorInfo(*this);
}

}
