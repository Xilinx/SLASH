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

/// @file debug/mem_poke.cpp
/// @brief Implementation of the debug device-memory read/write command.

#include "mem_poke.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <vrtd/session.hpp>

#include "../bdf.hpp"

namespace {

// ---- Region constants (mirror vrt/vrtd/src/allocator.h, which is private) --

constexpr uint64_t HBM_BASE         = 0x4000000000ULL;
constexpr uint64_t DDR_BASE         = 0x60000000000ULL;
constexpr uint64_t MEM_REGION_SIZE  = 512ULL * 1024 * 1024;
constexpr uint32_t HBM_REGION_COUNT = 64;
constexpr uint32_t DDR_REGION_COUNT = 64;

// ---- Region helpers ---------------------------------------------------------

std::string toUpper(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

MemRegion parseRegion(std::string_view text) {
    const std::string upper = toUpper(text);

    if (upper == "RAW") {
        return MemRegion{MemRegionKind::Raw, 0, false};
    }
    if (upper == "DDR") {
        return MemRegion{MemRegionKind::Ddr, 0, false};
    }
    if (upper == "HBM") {
        return MemRegion{MemRegionKind::Hbm, 0, true};
    }
    if (upper.size() > 3 && upper.substr(0, 3) == "HBM") {
        const std::string_view indexStr = std::string_view(upper).substr(3);
        uint32_t index{};
        const auto* begin = indexStr.data();
        const auto* end   = begin + indexStr.size();
        const auto result = std::from_chars(begin, end, index);
        if (result.ec == std::errc() && result.ptr == end && index < HBM_REGION_COUNT) {
            return MemRegion{MemRegionKind::Hbm, index, false};
        }
    }

    throw std::invalid_argument(
        std::string("Invalid region '") + std::string(text) +
        "': must be DDR, HBM, HBM0..HBM63, or RAW");
}

uint64_t regionBase(const MemRegion& region) {
    switch (region.kind) {
    case MemRegionKind::Raw: return 0;
    case MemRegionKind::Ddr: return DDR_BASE;
    case MemRegionKind::Hbm: return HBM_BASE + region.hbmIndex * MEM_REGION_SIZE;
    }
    return 0; // unreachable
}

uint64_t regionSize(const MemRegion& region) {
    switch (region.kind) {
    case MemRegionKind::Raw: return std::numeric_limits<uint64_t>::max();
    case MemRegionKind::Ddr: return DDR_REGION_COUNT * MEM_REGION_SIZE;
    case MemRegionKind::Hbm:
        return region.hbmWholeSpace ? HBM_REGION_COUNT * MEM_REGION_SIZE : MEM_REGION_SIZE;
    }
    return 0; // unreachable
}

// ---- General helpers --------------------------------------------------------

bool hasHexPrefix(const std::string_view text) {
    return text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
}

uint64_t parseUnsigned(const std::string_view text,
                       const char* fieldName) {
    if (text.empty()) {
        throw std::invalid_argument(std::string(fieldName) + " is required");
    }

    std::string_view digits = text;
    int base = 10;

    if (hasHexPrefix(text)) {
        digits = text.substr(2);
        base = 16;

        if (digits.empty()) {
            throw std::invalid_argument(std::string(fieldName) + " has no digits after 0x prefix");
        }
    }

    uint64_t value{};
    const auto* begin = digits.data();
    const auto* end = begin + digits.size();
    std::from_chars_result result = std::from_chars(begin, end, value, base);
    if (result.ec != std::errc() || result.ptr != end) {
        throw std::invalid_argument(std::string("Invalid ") + fieldName + ": '" + std::string(text) + "'");
    }

    return value;
}

void validateOptions(const MemPoke::Options& options) {
    // --print-base-address / --print-size are mutually exclusive with I/O flags
    if (options.printBaseAddress || options.printSize) {
        if (options.readMode || options.writeMode) {
            throw std::invalid_argument(
                "--print-base-address/--print-size cannot be combined with --read or --write");
        }
        if (options.relativeAddress) {
            throw std::invalid_argument(
                "--print-base-address/--print-size cannot be combined with --relative");
        }
        if (!options.addressText.empty()) {
            throw std::invalid_argument(
                "--print-base-address/--print-size cannot be combined with an address argument");
        }
        if (options.valueText.has_value()) {
            throw std::invalid_argument(
                "--print-base-address/--print-size cannot be combined with a value argument");
        }
        if (options.filePath.has_value()) {
            throw std::invalid_argument(
                "--print-base-address/--print-size cannot be combined with --file");
        }
        return;
    }

    if (options.readMode == options.writeMode) {
        throw std::invalid_argument("Exactly one of --read or --write must be specified");
    }

    if (options.wordSize != 1 && options.wordSize != 2 &&
        options.wordSize != 4 && options.wordSize != 8) {
        throw std::invalid_argument("word-size must be one of: 1, 2, 4, 8");
    }

    if (options.count == 0) {
        throw std::invalid_argument("count must be greater than zero");
    }

    if (options.filePath.has_value()) {
        if (options.valueText.has_value()) {
            throw std::invalid_argument("value argument is not allowed with --file");
        }
    } else {
        // Scalar (non-file) mode rules
        if (options.writeMode && options.count != 1) {
            throw std::invalid_argument("--count must be 1 for --write (use --file for multi-word writes)");
        }

        if (options.writeMode && !options.valueText.has_value()) {
            throw std::invalid_argument("value is required for --write (or use --file)");
        }

        if (options.readMode && options.valueText.has_value()) {
            throw std::invalid_argument("value is not allowed for --read");
        }
    }
}

void validateRangeAndAlignment(uint64_t address, uint64_t count, unsigned wordSize) {
    if (address % wordSize != 0) {
        throw std::invalid_argument("address must be aligned to word-size");
    }

    if (count > (std::numeric_limits<uint64_t>::max() / wordSize)) {
        throw std::invalid_argument("requested count is too large");
    }
}

// ---- Scalar word-oriented mode ------------------------------------------

template<typename T>
void printValue(T value, bool hexMode) {
    if (hexMode) {
        const std::ios_base::fmtflags flags = std::cout.flags();
        const char fill = std::cout.fill();
        std::cout << "0x"
                  << std::hex << std::nouppercase
                  << std::setw(static_cast<int>(sizeof(T) * 2))
                  << std::setfill('0')
                  << static_cast<uint64_t>(value)
                  << '\n';
        std::cout.flags(flags);
        std::cout.fill(fill);
    } else {
        std::cout << static_cast<uint64_t>(value) << '\n';
    }
}

template<typename T>
void runRead(vrtd::Buffer& buf, uint64_t count, bool hexMode) {
    buf.syncFromDevice(0, count * sizeof(T));
    const T* ptr = static_cast<const T*>(buf.data());
    for (uint64_t i = 0; i < count; ++i) {
        printValue(ptr[i], hexMode);
    }
}

template<typename T>
void runWrite(vrtd::Buffer& buf, uint64_t value) {
    T typed = static_cast<T>(value);
    std::memcpy(buf.data(), &typed, sizeof(T));
    buf.syncToDevice(0, sizeof(T));
}

void executeByWordSize(const MemPoke::Options& options,
                       vrtd::Buffer& buf,
                       uint64_t count,
                       uint64_t value) {
    switch (options.wordSize) {
    case 1:
        if (value > std::numeric_limits<uint8_t>::max()) {
            throw std::invalid_argument("value does not fit in 1-byte word");
        }
        if (options.readMode) {
            runRead<uint8_t>(buf, count, options.hexMode);
        } else {
            runWrite<uint8_t>(buf, value);
        }
        break;
    case 2:
        if (value > std::numeric_limits<uint16_t>::max()) {
            throw std::invalid_argument("value does not fit in 2-byte word");
        }
        if (options.readMode) {
            runRead<uint16_t>(buf, count, options.hexMode);
        } else {
            runWrite<uint16_t>(buf, value);
        }
        break;
    case 4:
        if (value > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument("value does not fit in 4-byte word");
        }
        if (options.readMode) {
            runRead<uint32_t>(buf, count, options.hexMode);
        } else {
            runWrite<uint32_t>(buf, value);
        }
        break;
    case 8:
        if (options.readMode) {
            runRead<uint64_t>(buf, count, options.hexMode);
        } else {
            runWrite<uint64_t>(buf, value);
        }
        break;
    default:
        throw std::runtime_error("Internal error: unsupported word-size");
    }
}

// ---- File mode -----------------------------------------------------------

/// Write a hexdump of @p data to @p out.
///
/// Format: "XXXXXXXX: HH HH HH HH HH HH HH HH  HH HH HH HH HH HH HH HH\n"
/// 16 bytes per line, address prefix, no 0x prefix on bytes, groups of 8.
void writeHexdump(std::ostream& out, const uint8_t* data, uint64_t size, uint64_t baseAddr) {
    const std::ios_base::fmtflags flags = out.flags();
    const char fill = out.fill();

    out << std::hex << std::nouppercase << std::setfill('0');

    for (uint64_t offset = 0; offset < size; offset += 16) {
        out << std::setw(8) << (baseAddr + offset) << ':';

        const uint64_t lineBytes = std::min<uint64_t>(16, size - offset);
        for (uint64_t i = 0; i < lineBytes; ++i) {
            if (i == 8) {
                out << ' '; // extra space between the two groups of 8
            }
            out << ' ' << std::setw(2) << static_cast<unsigned>(data[offset + i]);
        }
        out << '\n';
    }

    out.flags(flags);
    out.fill(fill);
}

/// Parse a hexdump or plain hex stream into bytes.
///
/// Accepts any mix of whitespace, colons, and newlines between hex digit pairs.
/// Stops at EOF.  Throws if an odd digit is left over or a non-hex character
/// is encountered (other than whitespace/colons).
std::vector<uint8_t> parseHexStream(std::istream& in) {
    std::vector<uint8_t> result;

    int hi = -1; // pending high nibble (-1 = none)
    int ch;
    while ((ch = in.get()) != std::char_traits<char>::eof()) {
        if (std::isxdigit(ch)) {
            int nibble = (ch >= '0' && ch <= '9') ? ch - '0'
                       : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                                                   : ch - 'A' + 10;
            if (hi < 0) {
                hi = nibble;
            } else {
                result.push_back(static_cast<uint8_t>((hi << 4) | nibble));
                hi = -1;
            }
        } else if (std::isspace(ch) || ch == ':') {
            // Separators are fine; a lone nibble before a separator is an error.
            if (hi >= 0) {
                throw std::invalid_argument("Odd hex digit in input (unpaired nibble)");
            }
        } else {
            throw std::invalid_argument(
                std::string("Unexpected character in hex input: '") + static_cast<char>(ch) + "'");
        }
    }

    if (hi >= 0) {
        throw std::invalid_argument("Odd hex digit at end of input (unpaired nibble)");
    }

    return result;
}

void runFileRead(vrtd::Buffer& buf, uint64_t totalBytes, uint64_t address,
                 const std::string& filePath, bool hexMode) {
    buf.syncFromDevice(0, totalBytes);
    const uint8_t* data = static_cast<const uint8_t*>(buf.data());

    std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Cannot open output file: " + filePath);
    }

    if (hexMode) {
        writeHexdump(out, data, totalBytes, address);
    } else {
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(totalBytes));
        if (!out) {
            throw std::runtime_error("Failed to write binary data to file: " + filePath);
        }
    }
}

void runFileWrite(vrtd::Buffer& buf, uint64_t totalBytes,
                  const std::string& filePath, bool hexMode) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open input file: " + filePath);
    }

    std::vector<uint8_t> fileData;

    if (hexMode) {
        fileData = parseHexStream(in);
    } else {
        fileData.assign(std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>());
    }

    if (fileData.size() != totalBytes) {
        throw std::invalid_argument(
            "File size (" + std::to_string(fileData.size()) +
            " bytes) does not match requested transfer size (" +
            std::to_string(totalBytes) + " bytes)");
    }

    std::memcpy(buf.data(), fileData.data(), totalBytes);
    buf.syncToDevice(0, totalBytes);
}

} // namespace

int MemPoke::run(const Options& options) {
    validateOptions(options);

    const MemRegion region = parseRegion(options.regionText);

    // --print-base-address / --print-size: no device access needed.
    if (options.printBaseAddress || options.printSize) {
        const std::ios_base::fmtflags flags = std::cout.flags();
        std::cout << std::hex << std::nouppercase;
        if (options.printBaseAddress) {
            std::cout << "0x" << regionBase(region) << '\n';
        }
        if (options.printSize) {
            std::cout << "0x" << regionSize(region) << '\n';
        }
        std::cout.flags(flags);
        return 0;
    }

    const uint64_t rawAddress = parseUnsigned(options.addressText, "address");

    // Resolve relative addresses before bounds checking.
    uint64_t address = rawAddress;
    if (options.relativeAddress) {
        if (region.kind == MemRegionKind::Raw) {
            throw std::invalid_argument("--relative has no effect with --region RAW");
        }
        address = regionBase(region) + rawAddress;
    }

    if (options.count > std::numeric_limits<uint64_t>::max() / options.wordSize) {
        throw std::invalid_argument("requested count is too large");
    }
    const uint64_t totalBytes = options.count * static_cast<uint64_t>(options.wordSize);

    if (!options.filePath.has_value()) {
        validateRangeAndAlignment(address, options.count, options.wordSize);
    }

    // Region bounds check (skipped for RAW).
    if (region.kind != MemRegionKind::Raw) {
        const uint64_t base = regionBase(region);
        const uint64_t size = regionSize(region);
        // Check: address must be within [base, base+size) and address+totalBytes <= base+size.
        // Written to avoid unsigned underflow: totalBytes <= size && address - base <= size - totalBytes.
        if (address < base || totalBytes > size || (address - base) > size - totalBytes) {
            throw std::invalid_argument(
                "address+size is out of bounds for region " + options.regionText);
        }
    }

    const std::string bdf = resolveBoardBdf(options.bdf, "debug mem-poke");

    vrtd::Session session;
    auto device = session.getDeviceByBdf(bdf);

    auto buf = device.openRawBuffer(
        address,
        totalBytes,
        options.readMode ? vrtd::BufferAllocDir::DeviceToHost
                         : vrtd::BufferAllocDir::HostToDevice
    );

    if (options.filePath.has_value()) {
        if (options.readMode) {
            runFileRead(buf, totalBytes, address, *options.filePath, options.hexMode);
        } else {
            runFileWrite(buf, totalBytes, *options.filePath, options.hexMode);
        }
    } else {
        const uint64_t value = options.valueText.has_value()
            ? parseUnsigned(*options.valueText, "value")
            : 0;
        executeByWordSize(options, buf, options.count, value);
    }

    return 0;
}
