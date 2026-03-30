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

/// @file inspect.hpp
/// @brief Declaration of the Inspect (and Query) command.
///
/// The Inspect command displays the contents of a vbin file: its target
/// platform, clock frequency, and the kernels it contains along with their
/// arguments.  When used as Query (the type alias below), it retrieves the
/// same information from whatever was last loaded on a device, identified
/// by its BDF (Bus:Device.Function) address.

#ifndef SMI_INSPECT_HPP
#define SMI_INSPECT_HPP

#include <string>

/// @brief Static entry-point for the inspect / query command.
///
/// This class is not instantiable; it simply groups the command's option
/// struct and its `run()` entry-point.
class Inspect {
    Inspect() = delete;
public:
    /// @brief Options parsed from the CLI for inspect / query.
    struct Options {
        std::string vbinPath;      ///< Path to the vbin file to inspect.
        std::string bdf;           ///< BDF address of the device to query.
        bool isBdfQuery{};         ///< True when querying a device rather than a file.
        bool jsonOutput{};         ///< Emit compact JSON instead of human-readable text.
        bool prettyJsonOutput{};   ///< Emit indented JSON instead of human-readable text.
    };

    /// @brief Executes the inspect/query command.
    /// @param options Populated options struct.
    /// @return Exit code (0 on success).
    static int run(const Options& options);
};

/// Query is simply Inspect with Options::isBdfQuery set to true.
using Query = Inspect;

#endif // SMI_INSPECT_HPP
