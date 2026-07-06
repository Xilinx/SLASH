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

/// @file shell_meta.cpp
/// @brief Implementation of the per-RM metadata RAM readback.

#include "shell_meta.hpp"

#include <cstring>
#include <stdexcept>
#include <vector>

#include <vrt/meta_ram.hpp>
#include <vrtd/session.hpp>

std::string readShellMeta(const std::string& bdf, MetaRegion region) {
    const uint64_t ramOffset = (region == MetaRegion::Service)
                                   ? vrt::SERVICE_META_RAM_OFFSET
                                   : vrt::USER_META_RAM_OFFSET;

    vrtd::Session session;
    auto device = session.getDeviceByBdf(bdf);
    auto bar = device.getBar(vrt::META_RAM_BAR);

    if (!bar.isUsable()) {
        throw std::runtime_error("BAR4 is not usable; cannot read metadata RAM");
    }

    vrtd::BarFile barFile = bar.openBarFile();
    if (barFile.getLen() < ramOffset + vrt::META_RAM_SIZE) {
        throw std::runtime_error("BAR4 too small for metadata RAM");
    }

    // Read the header first to learn the payload length, then read only what we
    // need (header + payload, rounded up to a word).
    std::vector<uint8_t> header(vrt::META_RAM_HEADER_BYTES);
    {
        auto ptr = barFile.getPtr<uint32_t>(vrtd::BarFile::Direction::Read,
                                            static_cast<size_t>(ramOffset));
        for (uint32_t i = 0; i < vrt::META_RAM_HEADER_BYTES / sizeof(uint32_t); ++i) {
            const uint32_t w = ptr[i];
            std::memcpy(header.data() + i * sizeof(uint32_t), &w, sizeof(w));
        }
    }

    uint32_t len;
    std::memcpy(&len, header.data() + 4, sizeof(len));
    if (len > vrt::META_RAM_MAX_PAYLOAD) {
        throw std::runtime_error("metadata RAM payload length out of range");
    }

    size_t total = vrt::META_RAM_HEADER_BYTES + len;
    total = (total + 3) & ~static_cast<size_t>(3);

    std::vector<uint8_t> image(total);
    std::memcpy(image.data(), header.data(), vrt::META_RAM_HEADER_BYTES);
    {
        auto ptr = barFile.getPtr<uint32_t>(vrtd::BarFile::Direction::Read,
                                            static_cast<size_t>(ramOffset));
        for (uint32_t i = vrt::META_RAM_HEADER_BYTES / sizeof(uint32_t);
             i < total / sizeof(uint32_t); ++i) {
            const uint32_t w = ptr[i];
            std::memcpy(image.data() + i * sizeof(uint32_t), &w, sizeof(w));
        }
    }

    return vrt::decodeMetaRam(image);
}
