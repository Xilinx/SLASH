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

#ifndef SMI_RESET_HPP
#define SMI_RESET_HPP

/// @file program.hpp
/// @brief Declaration of the Reset command.
///
/// The Reset hardware resets a V80 board. The board must be programmed
/// with the static SLASH design.

#include <string>

/// @brief Static entry-point for the reset command.
///
/// This class is not instantiable; it groups the command's option
/// struct and its run() entry-point.
class Reset {
    Reset() = delete;
public:
    /// @brief Options parsed from the CLI for the reset command.
    struct Options {
        std::string bdf;        ///< BDF (Bus:Device.Function) address of the target device.
    };

    /// @brief Executes the reset command.
    /// @param options Populated options struct.
    /// @return Exit code (0 on success).
    static int run(const Options& options);
};

#endif // SMI_RESET_HPP
