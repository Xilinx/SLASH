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
 * @file bar.cpp
 *
 * Implementation of the vrtd::Bar C++ wrapper.
 *
 * Bar is a lightweight value-type that holds PCI BAR metadata (number,
 * physical start address, length, usable/in-use flags) plus a callback
 * for opening a memory-mapped BarFile.  It is constructed by Session
 * when the user queries device BARs and does not own any kernel
 * resources itself.
 */

#include <vrtd/bar.hpp>

namespace vrtd {

Bar::Bar(uint32_t deviceNum, uint8_t num, bool usable, bool inUse, uint64_t startAddress, uint64_t length, std::function<BarFile(const Bar&)> fOpenBarFile) noexcept {
    this->deviceNum = deviceNum;
    this->num = num;
    this->usable = usable;
    this->inUse = inUse;
    this->startAddress = startAddress;
    this->length = length;
    this->fOpenBarFile = fOpenBarFile;
}

uint32_t Bar::getDeviceNum() const noexcept {
    return deviceNum;
}

uint8_t Bar::getNum() const noexcept {
    return num;
}

bool Bar::isUsable() const noexcept {
    return usable;
}

bool Bar::isInUse() const noexcept {
    return inUse;
}

uint64_t Bar::getStartAddress() const noexcept {
    return startAddress;
}

uint64_t Bar::getLength() const noexcept {
    return length;
}

BarFile Bar::openBarFile() const {
    return fOpenBarFile(*this);
}

}
