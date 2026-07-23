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

/// @file debug/hotplug.hpp
/// @brief Declaration of the Hotplug debug command.

#ifndef SMI_DEBUG_HOTPLUG_HPP
#define SMI_DEBUG_HOTPLUG_HPP

#include <cstdint>
#include <optional>
#include <string>

/// @brief Static entry-point for the debug hotplug-op command.
///
/// This class is not instantiable; it groups the command options and
/// its run() entry-point.
class Hotplug {
    Hotplug() = delete;
public:
    /// @brief Options parsed from the CLI for the hotplug-op command.
    struct Options {
        std::string bdf;                    ///< Target board address for device-level ops.
        std::string opText;                 ///< Hotplug operation selector.
        std::optional<uint8_t> function;    ///< Optional PCI function number.
    };

    /// @brief Executes the hotplug-op command.
    /// @param options Populated options struct.
    /// @return Exit code (0 on success).
    static int run(const Options& options);
};

#endif // SMI_DEBUG_HOTPLUG_HPP
