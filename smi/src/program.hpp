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

/// @file program.hpp
/// @brief Declaration of the Program command.
///
/// The Program command loads a vbin file onto a V80 device identified
/// by its BDF address.

#ifndef SMI_PROGRAM_HPP
#define SMI_PROGRAM_HPP

#include <string>

/// @brief Static entry-point for the program command.
///
/// This class is not instantiable; it groups the command's option
/// struct and its run() entry-point.
class Program {
    Program() = delete;
public:
    /// @brief Options parsed from the CLI for the program command.
    struct Options {
        std::string vbinPath;   ///< Path to the vbin file to load onto the device.
        std::string bdf;        ///< BDF (Bus:Device.Function) address of the target device.
    };

    /// @brief Executes the program command.
    /// @param options Populated options struct.
    /// @return Exit code (0 on success).
    static int run(const Options& options);
};

#endif // SMI_PROGRAM_HPP
