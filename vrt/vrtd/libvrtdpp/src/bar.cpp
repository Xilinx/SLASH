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

#include <vrtd/bar.hpp>

#include "connection.hpp"

#include <vrtd/error.hpp>

#include <utility>

namespace vrtd {

/*
 * Snapshot the daemon's BAR metadata while retaining the shared connection.
 * The handle can therefore open a mapping after its parent Device or Session
 * facade has been destroyed.
 */
Bar::Bar(std::shared_ptr<detail::Connection> connection,
         uint32_t deviceNum,
         uint8_t num,
         bool usable,
         bool inUse,
         uint64_t startAddress,
         uint64_t length) noexcept
    : connection(std::move(connection))
    , deviceNum(deviceNum)
    , num(num)
    , usable(usable)
    , inUse(inUse)
    , startAddress(startAddress)
    , length(length)
{
}

uint32_t Bar::getDeviceNum() const noexcept
{
    return deviceNum;
}

uint8_t Bar::getNum() const noexcept
{
    return num;
}

bool Bar::isUsable() const noexcept
{
    return usable;
}

bool Bar::isInUse() const noexcept
{
    return inUse;
}

uint64_t Bar::getStartAddress() const noexcept
{
    return startAddress;
}

uint64_t Bar::getLength() const noexcept
{
    return length;
}

BarFile Bar::openBarFile() const
{
    if (!connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    return BarFile(connection->openBarFile(deviceNum, num));
}

} // namespace vrtd
