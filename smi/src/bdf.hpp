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

/// @file bdf.hpp
/// @brief BDF (Bus:Device.Function) parsing and normalization for SMI user input.
///
/// Accepts four formats: DDDD:BB:DD.F, BB:DD.F, DDDD:BB:DD, BB:DD.
/// Normalizes to board-level DDDD:BB:DD for internal use.  If a function
/// digit is supplied, prints a warning and strips it.

#ifndef SMI_BDF_HPP
#define SMI_BDF_HPP

#include <iostream>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>

/// Result of parsing a BDF string from user input.
struct ParsedBdf {
    std::string domain;    ///< 4-hex-digit domain, e.g. "0000".
    std::string bus;       ///< 2-hex-digit bus, e.g. "03".
    std::string device;    ///< 2-hex-digit device/slot, e.g. "00".
    std::optional<unsigned int> function; ///< Function digit 0-7, or nullopt.

    /// Returns the board-level base: "DDDD:BB:DD" (no function).
    std::string base() const {
        return domain + ":" + bus + ":" + device;
    }
};

/// Parse a user-supplied BDF string.
///
/// Accepts: "DDDD:BB:DD.F", "BB:DD.F", "DDDD:BB:DD", "BB:DD".
/// Prepends domain "0000" if not present.
///
/// @param input  Raw BDF string from user.
/// @return ParsedBdf on success.
/// @throws std::invalid_argument if the format is unrecognized.
inline ParsedBdf parseBdf(const std::string& input) {
    static const std::regex bdfRegex(
        R"(^(?:([0-9a-fA-F]{4}):)?([0-9a-fA-F]{2}):([0-9a-fA-F]{2})(?:\.([0-7]))?$)"
    );

    std::smatch match;
    if (!std::regex_match(input, match, bdfRegex)) {
        throw std::invalid_argument(
            "Invalid BDF format: '" + input + "'. "
            "Expected DDDD:BB:DD, BB:DD, DDDD:BB:DD.F, or BB:DD.F "
            "(e.g. 0000:03:00 or 03:00)");
    }

    ParsedBdf result;
    result.domain = match[1].matched ? match[1].str() : "0000";
    result.bus = match[2].str();
    result.device = match[3].str();
    if (match[4].matched) {
        result.function = static_cast<unsigned int>(match[4].str()[0] - '0');
    }

    return result;
}

/// Resolve a user-supplied BDF to a board-level "DDDD:BB:DD" string.
///
/// If a function digit (.F) is present, a warning is printed to stderr
/// and the function is stripped.  The result is always "DDDD:BB:DD".
///
/// @param input     Raw BDF string from user.
/// @param cmdName   Command name for the warning message (e.g. "reset").
/// @return Board-level BDF in "DDDD:BB:DD" format.
/// @throws std::invalid_argument if the BDF format is invalid.
inline std::string resolveBoardBdf(const std::string& input,
                                   const std::string& cmdName) {
    auto parsed = parseBdf(input);

    if (parsed.function.has_value()) {
        std::cerr << "Warning: " << cmdName
                  << " operates on a board, not a specific PF. "
                  << "Ignoring function ." << *parsed.function
                  << " from '" << input << "'; using board address "
                  << parsed.base() << std::endl;
    }

    return parsed.base();
}

#endif // SMI_BDF_HPP
