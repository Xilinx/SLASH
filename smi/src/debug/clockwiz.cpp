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

/// @file debug/clockwiz.cpp
/// @brief Implementation of the debug clock read/set command.

#include "clockwiz.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
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

std::string toLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

vrtd::ClockRegion parseClockRegion(const std::string_view text) {
    const std::string normalized = toLower(text);
    if (normalized == "user") {
        return vrtd::ClockRegion::User;
    }
    if (normalized == "service") {
        return vrtd::ClockRegion::Service;
    }

    throw std::invalid_argument("region must be one of: user, service");
}

uint32_t parseSetRate(const Clockwiz::Options& options) {
    if (!options.setRateText.has_value()) {
        throw std::invalid_argument("--set requires a rate in Hz");
    }

    const uint64_t rate = parseUnsigned(*options.setRateText, "set rate");
    if (rate == 0) {
        throw std::invalid_argument("set rate must be greater than zero");
    }
    if (rate > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("set rate does not fit in 32-bit Hz value");
    }

    return static_cast<uint32_t>(rate);
}

void validateOptions(const Clockwiz::Options& options) {
    const bool hasSet = options.setRateText.has_value();
    if (options.getMode == hasSet) {
        throw std::invalid_argument("Exactly one of --get or --set must be specified");
    }

    if (options.hexMode && hasSet) {
        throw std::invalid_argument("--hex is only valid with --get");
    }

    (void)parseClockRegion(options.regionText);
    if (hasSet) {
        (void)parseSetRate(options);
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

} // namespace

int Clockwiz::run(const Options& options) {
    validateOptions(options);

    const std::string bdf = resolveBoardBdf(options.bdf, "debug clockwiz");
    const vrtd::ClockRegion region = parseClockRegion(options.regionText);

    vrtd::Session session;
    auto device = session.getDeviceByBdf(bdf);

    if (options.getMode) {
        const uint32_t currentRate = device.getClockRate(region);
        printValue(currentRate, options.hexMode);
        return 0;
    }

    const uint32_t requestedRate = parseSetRate(options);
    const uint32_t achievedRate = device.setClockRate(region, requestedRate);

    std::cout << "requested_hz=" << requestedRate << '\n';
    std::cout << "achieved_hz=" << achievedRate << '\n';

    return 0;
}
