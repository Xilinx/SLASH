/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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

/// @file shell_build_id.cpp
/// @brief Implementation of the shell build-ID hardware read.

#include "shell_build_id.hpp"

#include <cinttypes>
#include <cstdio>
#include <stdexcept>

#include <vrtd/session.hpp>

std::string BuildId::commitHex() const {
    // 60-bit commit prefix: 28 high hash bits followed by the 32 low bits.
    char buf[32];
    const uint64_t commit = (static_cast<uint64_t>(hiHash) << 32) | lo;
    std::snprintf(buf, sizeof(buf), "0x%015" PRIx64, commit);
    return std::string(buf);
}

const char* BuildId::shellName() const {
    return shell == ShellVariant::Compute ? "compute" : "service";
}

BuildId readBuildId(const std::string& bdf) {
    vrtd::Session session;
    auto device = session.getDeviceByBdf(bdf);
    auto bar = device.getBar(BUILD_ID_BAR);

    if (!bar.isUsable()) {
        throw std::runtime_error("BAR4 is not usable; cannot read shell build ID");
    }

    vrtd::BarFile barFile = bar.openBarFile();
    if (barFile.getLen() < BUILD_ID_REG_HI + sizeof(uint32_t)) {
        throw std::runtime_error("BAR4 too small for build-ID register");
    }

    const uint32_t lo =
        *barFile.getPtr<uint32_t>(vrtd::BarFile::Direction::Read,
                                  static_cast<size_t>(BUILD_ID_REG_LO));
    const uint32_t hi =
        *barFile.getPtr<uint32_t>(vrtd::BarFile::Direction::Read,
                                  static_cast<size_t>(BUILD_ID_REG_HI));

    BuildId id;
    id.lo = lo;
    id.hiHash = hi & BUILD_ID_HI_HASH_MASK;
    id.dirty = (hi & BUILD_ID_HI_DIRTY_MASK) != 0;
    id.shell = (hi & BUILD_ID_HI_SHELL_MASK) != 0 ? ShellVariant::Compute
                                                  : ShellVariant::Service;
    return id;
}
