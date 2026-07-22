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

/// @file write_static_shell.hpp
/// @brief Declaration of the WriteStaticShell command.

#ifndef SMI_WRITE_STATIC_SHELL_HPP
#define SMI_WRITE_STATIC_SHELL_HPP

#include <string>
#include <vector>

/// @brief Static entry-point for the write-static-shell command.
///
/// This class is not instantiable; it groups the command's option
/// struct and its run() entry-point.
class WriteStaticShell {
    WriteStaticShell() = delete;
public:
    /// @brief Options parsed from the CLI for the write-static-shell command.
    struct Options {
        bool flash = false;                 ///< Use VRTD cfgmem flash programming.
        bool jtag = false;                  ///< Use JTAG/xsdb instead of cfgmem flash.
        std::string bdf;                    ///< Target board address, unless noRemoveDevice is set.
        std::string pdiPath;                ///< Override PDI path for active development.
        bool noRemoveDevice = false;        ///< Skip pre-JTAG PCIe remove; JTAG mode only.
        std::vector<std::string> bashSources; ///< Environment setup scripts for JTAG mode.
        std::string xsdbTargetId;           ///< Optional Versal xcv80 XSDB target_id.
    };

    /// @brief Executes the write-static-shell command.
    /// @param options Populated options struct.
    /// @return Exit code (0 on success).
    static int run(const Options& options);
};

#endif // SMI_WRITE_STATIC_SHELL_HPP
