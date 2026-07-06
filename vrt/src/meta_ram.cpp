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

/// @file meta_ram.cpp
/// @brief Encode/decode helpers for the static-shell per-RM metadata RAMs.

#include <vrt/meta_ram.hpp>

#include <cstring>
#include <stdexcept>

#include <zlib.h>

namespace vrt {

namespace {

/// gzip-compress a byte range using zlib (gzip wrapper, windowBits +16).
std::vector<uint8_t> gzipCompress(const uint8_t* data, size_t len) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("meta_ram: deflateInit2 failed");
    }

    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(len);

    std::vector<uint8_t> out;
    std::vector<uint8_t> chunk(1 << 15);
    int rc = Z_OK;
    do {
        zs.next_out = chunk.data();
        zs.avail_out = static_cast<uInt>(chunk.size());
        rc = deflate(&zs, Z_FINISH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            deflateEnd(&zs);
            throw std::runtime_error("meta_ram: deflate failed");
        }
        out.insert(out.end(), chunk.data(), chunk.data() + (chunk.size() - zs.avail_out));
    } while (rc != Z_STREAM_END);

    deflateEnd(&zs);
    return out;
}

/// gzip-decompress a byte range using zlib.
std::string gzipDecompress(const uint8_t* data, size_t len) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) {
        throw std::runtime_error("meta_ram: inflateInit2 failed");
    }

    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(len);

    std::string out;
    std::vector<char> chunk(1 << 15);
    int rc = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(chunk.data());
        zs.avail_out = static_cast<uInt>(chunk.size());
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&zs);
            throw std::runtime_error("meta_ram: inflate failed");
        }
        out.append(chunk.data(), chunk.size() - zs.avail_out);
    } while (rc != Z_STREAM_END);

    inflateEnd(&zs);
    return out;
}

uint32_t readWord(const std::vector<uint8_t>& b, size_t off) {
    uint32_t w;
    std::memcpy(&w, b.data() + off, sizeof(w));
    return w;
}

void writeWord(std::vector<uint8_t>& b, size_t off, uint32_t w) {
    std::memcpy(b.data() + off, &w, sizeof(w));
}

}  // namespace

std::vector<uint8_t> encodeMetaRam(const std::string& xml) {
    std::vector<uint8_t> payload =
        gzipCompress(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());

    if (payload.size() > META_RAM_MAX_PAYLOAD) {
        throw std::runtime_error(
            "meta_ram: compressed metadata (" + std::to_string(payload.size()) +
            " bytes) exceeds RAM payload capacity (" +
            std::to_string(META_RAM_MAX_PAYLOAD) + " bytes)");
    }

    const uint32_t crc = static_cast<uint32_t>(
        crc32(crc32(0L, Z_NULL, 0), payload.data(), static_cast<uInt>(payload.size())));

    // Header + payload, zero-padded up to a 32-bit boundary.
    size_t total = META_RAM_HEADER_BYTES + payload.size();
    total = (total + 3) & ~static_cast<size_t>(3);
    std::vector<uint8_t> image(total, 0);

    writeWord(image, 0,
              (META_RAM_MAGIC << 16) | META_RAM_FLAG_GZIP | META_RAM_VERSION);
    writeWord(image, 4, static_cast<uint32_t>(payload.size()));
    writeWord(image, 8, crc);
    // word[3] reserved (zero).
    std::memcpy(image.data() + META_RAM_HEADER_BYTES, payload.data(), payload.size());

    return image;
}

std::string decodeMetaRam(const std::vector<uint8_t>& image) {
    if (image.size() < META_RAM_HEADER_BYTES) {
        throw std::runtime_error("meta_ram: image too small for header");
    }

    const uint32_t w0 = readWord(image, 0);
    if ((w0 >> 16) != META_RAM_MAGIC) {
        throw std::runtime_error("meta_ram: bad magic (RAM empty or uninitialized)");
    }

    const uint32_t len = readWord(image, 4);
    const uint32_t crc = readWord(image, 8);
    if (len > META_RAM_MAX_PAYLOAD ||
        META_RAM_HEADER_BYTES + len > image.size()) {
        throw std::runtime_error("meta_ram: payload length out of range");
    }

    const uint8_t* payload = image.data() + META_RAM_HEADER_BYTES;
    const uint32_t actualCrc = static_cast<uint32_t>(
        crc32(crc32(0L, Z_NULL, 0), payload, static_cast<uInt>(len)));
    if (actualCrc != crc) {
        throw std::runtime_error("meta_ram: CRC mismatch");
    }

    return gzipDecompress(payload, len);
}

}  // namespace vrt
