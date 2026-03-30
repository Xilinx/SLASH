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

/// @file utils.hpp
/// @brief Common utilities for formatting, sentinel values, and output in smi.

#ifndef SMI_UTILS_HPP
#define SMI_UTILS_HPP

#include <charconv>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <json/json.h>

/// Indentation constant for hierarchical text output (one level).
constexpr char INDENT1[] = "    ";

/// Indentation constant for hierarchical text output (two levels).
constexpr char INDENT2[] = "        ";

/// Indentation constant for hierarchical text output (three levels).
constexpr char INDENT3[] = "            ";

/// Converts an integer value to a "0x"-prefixed hexadecimal string.
///
/// @tparam Int  Integral type of the value.
/// @param value The integer to convert.
/// @return      Hex string (e.g., "0x1a3f").
/// @throws std::runtime_error if the internal conversion fails (should not
///         happen under normal circumstances).
template<class Int>
std::string toHexString(Int value) {
    constexpr size_t buf_size{32};
    char buf[buf_size]{'0', 'x'};
    std::to_chars_result result = std::to_chars(buf + 2, buf + buf_size, value, 16);

    if (result.ec != std::errc()) {
        throw std::runtime_error("Internal error in toHexString. This is a bug in smi. Please report.");
    }

    return {buf};
}

/// Returns a generic sentinel value for the given integer type.
///
/// @tparam Int  Integral type.
/// @return      -1 for signed types; max representable value for unsigned types.
template<class Int>
consteval Int sentinel() {
    if constexpr (std::numeric_limits<Int>::is_signed) {
        return static_cast<Int>(-1);
    } else {
        return std::numeric_limits<Int>::max();
    }
}


/// Tests whether @p i holds the sentinel value for its type.
///
/// @tparam Int  Integral type.
/// @param i     Value to test.
/// @return      true if @p i equals sentinel<Int>().
template<class Int>
constexpr bool isSentinel(Int i) {
    return i == sentinel<Int>();
}

/// Prints an object to stdout, either as human-readable text or as JSON.
///
/// In text mode the object is streamed via its `operator<<` overload.
/// In JSON mode the object is first converted via an ADL-found `toJson()`
/// overload and then serialized through jsoncpp.
///
/// In order to use this, commands must define `operator<<` and `toJson()`
/// functions for the objects they want to print.
///
/// @tparam T          Type that supports `operator<<` and a free `toJson()`.
/// @param object      The object to print.
/// @param json        If true, output compact JSON.
/// @param prettyJson  If true, output indented (pretty-printed) JSON.
template<class T>
void print(const T& object, bool json = false, bool prettyJson = false) {
    if (json || prettyJson) {
        const auto data = toJson(object);

        Json::StreamWriterBuilder builder;
        builder["indentation"] = (prettyJson ? INDENT1 : "");
        std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
        writer->write(data, &std::cout);
        std::cout << std::endl;  // writer doesn't add trailing newline
    } else {
        std::cout << object << std::flush;
    }
} 

#endif // SMI_PROGRAM_HPP
