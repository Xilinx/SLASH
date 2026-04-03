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

/// @file debug/bar_poke.cpp
/// @brief Implementation of the debug BAR read/write command.

#include "bar_poke.hpp"

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

#include <vrtd/session.hpp>

#include "../bdf.hpp"

namespace {

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

void validateOptions(const BarPoke::Options& options) {
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

    if (options.writeMode && options.count != 1) {
        throw std::invalid_argument("--count must be 1 for --write");
    }

    if (options.writeMode && !options.valueText.has_value()) {
        throw std::invalid_argument("value is required for --write");
    }

    if (options.readMode && options.valueText.has_value()) {
        throw std::invalid_argument("value is not allowed for --read");
    }
}

void validateRangeAndAlignment(uint64_t address,
                               uint64_t count,
                               unsigned wordSize,
                               uint64_t barLength) {
    if (address % wordSize != 0) {
        throw std::invalid_argument("address must be aligned to word-size");
    }

    if (address > barLength) {
        throw std::invalid_argument("address is outside BAR range");
    }

    if (count > (std::numeric_limits<uint64_t>::max() / wordSize)) {
        throw std::invalid_argument("requested count is too large");
    }

    const uint64_t totalBytes = count * wordSize;
    if (totalBytes > barLength - address) {
        throw std::invalid_argument("BAR access range is out of bounds");
    }
}

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
void runRead(vrtd::BarFile& barFile,
             uint64_t address,
             uint64_t count,
             bool hexMode) {
    auto ptr = barFile.getPtr<T>(vrtd::BarFile::Direction::Read,
                                 static_cast<size_t>(address));
    for (uint64_t i = 0; i < count; ++i) {
        printValue(ptr[i], hexMode);
    }
}

template<typename T>
void runWrite(vrtd::BarFile& barFile,
              uint64_t address,
              uint64_t value) {
    auto ptr = barFile.getPtr<T>(vrtd::BarFile::Direction::Write,
                                 static_cast<size_t>(address));
    *ptr = static_cast<T>(value);
}

void executeByWordSize(const BarPoke::Options& options,
                       vrtd::BarFile& barFile,
                       uint64_t address,
                       uint64_t count,
                       uint64_t value) {
    switch (options.wordSize) {
    case 1:
        if (value > std::numeric_limits<uint8_t>::max()) {
            throw std::invalid_argument("value does not fit in 1-byte word");
        }
        if (options.readMode) {
            runRead<uint8_t>(barFile, address, count, options.hexMode);
        } else {
            runWrite<uint8_t>(barFile, address, value);
        }
        break;
    case 2:
        if (value > std::numeric_limits<uint16_t>::max()) {
            throw std::invalid_argument("value does not fit in 2-byte word");
        }
        if (options.readMode) {
            runRead<uint16_t>(barFile, address, count, options.hexMode);
        } else {
            runWrite<uint16_t>(barFile, address, value);
        }
        break;
    case 4:
        if (value > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument("value does not fit in 4-byte word");
        }
        if (options.readMode) {
            runRead<uint32_t>(barFile, address, count, options.hexMode);
        } else {
            runWrite<uint32_t>(barFile, address, value);
        }
        break;
    case 8:
        if (options.readMode) {
            runRead<uint64_t>(barFile, address, count, options.hexMode);
        } else {
            runWrite<uint64_t>(barFile, address, value);
        }
        break;
    default:
        throw std::runtime_error("Internal error: unsupported word-size");
    }
}

} // namespace

int BarPoke::run(const Options& options) {
    validateOptions(options);

    const uint64_t address = parseUnsigned(options.addressText, "address");
    const uint64_t value = options.valueText.has_value()
        ? parseUnsigned(*options.valueText, "value")
        : 0;

    const std::string bdf = resolveBoardBdf(options.bdf, "debug bar-poke");

    vrtd::Session session;
    auto device = session.getDeviceByBdf(bdf);
    auto bar = device.getBar(static_cast<uint8_t>(options.bar));

    if (!bar.isUsable()) {
        throw std::runtime_error("Requested BAR is not usable");
    }

    vrtd::BarFile barFile = bar.openBarFile();
    const uint64_t barLength = static_cast<uint64_t>(barFile.getLen());

    validateRangeAndAlignment(address, options.count, options.wordSize, barLength);
    executeByWordSize(options, barFile, address, options.count, value);

    return 0;
}
